#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include "worker.h"
#include "compress.h"
#include "log.h"
#include "progress.h"

static void fp_result_finish(fp_result *result) {
    if (result) {
        clock_gettime(CLOCK_MONOTONIC, &result->end_ts);
    }
}

static double fp_timespec_diff_ms(const struct timespec *start, const struct timespec *end) {
    if (!start || !end) {
        return 0.0;
    }
    double sec = (double)(end->tv_sec - start->tv_sec) * 1000.0;
    double nsec = (double)(end->tv_nsec - start->tv_nsec) / 1000000.0;
    return sec + nsec;
}

typedef fp_compress_code (*fp_encode_fn)(const fp_rgba_image *, int, const char *, fp_encoded_image *);

typedef struct {
    char key[32];
    double total_ms;
    double total_weight;
    uint32_t samples;
} fp_eta_entry;

typedef struct {
    fp_eta_entry entries[8];
} fp_eta_table;

static fp_eta_table g_eta_table;
static pthread_mutex_t g_eta_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_eta_load_once = PTHREAD_ONCE_INIT;
static unsigned g_eta_dirty_counter = 0;
static const char *g_eta_store_path = "ferret_eta.dat";

typedef struct {
    const fp_rgba_image *image;
    int quality;
    const char *label;
    const char *log_name;
    const char *eta_key;
    fp_encoded_image *output;
    fp_encode_fn encode;
    struct timespec start_ts;
    struct timespec end_ts;
    fp_job *job;
    int failure_status;
    const char *failure_message;
    double work_units;
    fp_compress_code code;
} fp_encode_task;

static fp_compress_code fp_worker_png_encode(const fp_rgba_image *image, int level, const char *label, fp_encoded_image *output) {
    return fp_compress_png_level(image, level, label ? label : "variant", output);
}

static fp_compress_code fp_worker_png_quant(const fp_rgba_image *image, int palette_size, const char *label, fp_encoded_image *output) {
    if (palette_size <= 0) {
        palette_size = 128;
    }
    return fp_compress_png_quantized(image, palette_size, label, output);
}

static fp_compress_code fp_worker_webp_encode(const fp_rgba_image *image, int quality, const char *label, fp_encoded_image *output) {
    (void)label;
    return fp_compress_webp(image, quality, output);
}

static fp_compress_code fp_worker_avif_encode(const fp_rgba_image *image, int quality, const char *label, fp_encoded_image *output) {
    (void)label;
    return fp_compress_avif(image, quality, output);
}

static void worker_eta_save(void) {
    FILE *f = fopen(g_eta_store_path, "w");
    if (!f) {
        return;
    }
    fputs("# Format/label ETA data (ms total, work units, samples)\n", f);
    fputs("# key total_ms total_work samples\n", f);
    size_t count = sizeof(g_eta_table.entries) / sizeof(g_eta_table.entries[0]);
    for (size_t i = 0; i < count; ++i) {
        fp_eta_entry *entry = &g_eta_table.entries[i];
        if (entry->samples == 0 || entry->total_weight <= 0.0) {
            continue;
        }
        fprintf(f, "%s %.6f %.6f %u\n",
                entry->key,
                entry->total_ms,
                entry->total_weight,
                entry->samples);
    }
    fclose(f);
}

static void worker_eta_load(void) {
    FILE *f = fopen(g_eta_store_path, "r");
    if (!f) {
        return;
    }
    while (true) {
        fp_eta_entry entry = {0};
        if (fscanf(f, "%31s %lf %lf %u", entry.key, &entry.total_ms, &entry.total_weight, &entry.samples) != 4) {
            break;
        }
        if (entry.samples > 0 && entry.total_weight > 0.0) {
            size_t count = sizeof(g_eta_table.entries) / sizeof(g_eta_table.entries[0]);
            for (size_t i = 0; i < count; ++i) {
                if (g_eta_table.entries[i].samples == 0) {
                    g_eta_table.entries[i] = entry;
                    break;
                }
            }
        }
    }
    fclose(f);
}

static double worker_eta_update(const char *key, double elapsed_ms, double units);

static void *fp_encode_task_run(void *arg) {
    fp_encode_task *task = (fp_encode_task *)arg;
    if (!task || !task->encode) {
        return NULL;
    }
    clock_gettime(CLOCK_MONOTONIC, &task->start_ts);
    task->code = task->encode(task->image, task->quality, task->label, task->output);
    clock_gettime(CLOCK_MONOTONIC, &task->end_ts);
    if (task->code == FP_COMPRESS_OK) {
        double elapsed = fp_timespec_diff_ms(&task->start_ts, &task->end_ts);
        double work_units = task->work_units > 0 ? task->work_units : 1.0;
        double avg = worker_eta_update(task->eta_key, elapsed, work_units);
        if (task->job && task->job->progress) {
            fp_progress_emit_output(task->job->progress, task->output, task->job->size, elapsed, avg);
        }
        if (task->job) {
            fp_log_info("⏱️  Job #%llu %s finished in %.2f ms (avg %.2f ms)",
                        (unsigned long long)task->job->id,
                        task->log_name,
                        elapsed,
                        avg);
        }
    }
    return NULL;
}

static fp_result *fp_worker_handle_job(fp_job *job) {
    if (!job) {
        return NULL;
    }

    fp_log_info("🛠️  Worker picked up job #%llu", (unsigned long long)job->id);

    fp_result *result = calloc(1, sizeof(fp_result));
    if (!result) {
        fp_free_job(job);
        free(job);
        return NULL;
    }

    clock_gettime(CLOCK_MONOTONIC, &result->start_ts);
    result->id = job->id;
    result->input_size = job->size;

    fp_rgba_image image = {0};
    fp_compress_code code = fp_decode_png(job->data, job->size, &image);
    if (code != FP_COMPRESS_OK) {
        result->status = -1;
        strncpy(result->message, "decode_error", sizeof(result->message) - 1);
        fp_log_warn("🧨 decode failed for job #%llu", (unsigned long long)job->id);
        fp_free_job(job);
        free(job);
        fp_result_finish(result);
        return result;
    }

    double work_units = ((double)image.width * image.height) / 1000000.0;
    if (work_units <= 0) {
        work_units = 0.1;
    }

    fp_encode_task tasks[4] = {
        {
            .image = &image,
            .quality = 9,
            .label = "lossless",
            .log_name = "PNG lossless",
            .eta_key = "png_lossless",
            .output = &result->outputs[0],
            .encode = fp_worker_png_encode,
            .job = job,
            .failure_status = -2,
            .failure_message = "png_compress_error",
            .work_units = work_units,
        },
        {
            .image = &image,
            .quality = 128,
            .label = "pngquant q80",
            .log_name = "PNG pngquant q80",
            .eta_key = "png_quant",
            .output = &result->outputs[1],
            .encode = fp_worker_png_quant,
            .job = job,
            .failure_status = -5,
            .failure_message = "pngquant_error",
            .work_units = work_units,
        },
        {
            .image = &image,
            .quality = 90,
            .label = "high",
            .log_name = "WEBP high",
            .eta_key = "webp_high",
            .output = &result->outputs[2],
            .encode = fp_worker_webp_encode,
            .job = job,
            .failure_status = -3,
            .failure_message = "webp_compress_error",
            .work_units = work_units,
        },
        {
            .image = &image,
            .quality = 28,
            .label = "medium",
            .log_name = "AVIF medium",
            .eta_key = "avif_medium",
            .output = &result->outputs[3],
            .encode = fp_worker_avif_encode,
            .job = job,
            .failure_status = -4,
            .failure_message = "avif_compress_error",
            .work_units = work_units,
        },
    };

    const size_t task_count = sizeof(tasks) / sizeof(tasks[0]);
    pthread_t threads[task_count];
    bool started[task_count];
    memset(started, 0, sizeof(started));
    for (size_t i = 0; i < task_count; ++i) {
        if (pthread_create(&threads[i], NULL, fp_encode_task_run, &tasks[i]) == 0) {
            started[i] = true;
        } else {
            fp_encode_task_run(&tasks[i]);
        }
    }

    for (size_t i = 0; i < task_count; ++i) {
        if (started[i]) {
            pthread_join(threads[i], NULL);
        }
    }

    int failure_status = 0;
    const char *failure_message = NULL;
    for (size_t i = 0; i < task_count; ++i) {
        if (tasks[i].code != FP_COMPRESS_OK) {
            failure_status = tasks[i].failure_status;
            failure_message = tasks[i].failure_message;
            break;
        }
    }

    if (failure_status != 0) {
        result->status = failure_status;
        if (failure_message) {
            strncpy(result->message, failure_message, sizeof(result->message) - 1);
        }
        fp_free_result(result);
        result->output_count = 0;
        fp_log_warn("🧨 %s failed for job #%llu",
                    failure_message ? failure_message : "compression",
                    (unsigned long long)job->id);
        fp_rgba_image_free(&image);
        fp_free_job(job);
        free(job);
        fp_result_finish(result);
        return result;
    }

    result->output_count = task_count;
    result->status = 0;
    strncpy(result->message, "ok", sizeof(result->message) - 1);
    fp_log_info("🎯 Job #%llu outputs ready (%zu bytes in, %zu bytes out)",
                (unsigned long long)job->id,
                job->size,
                result->outputs[0].size + result->outputs[1].size +
                    result->outputs[2].size + result->outputs[3].size);

    fp_rgba_image_free(&image);
    fp_free_job(job);
    free(job);

    fp_result_finish(result);
    return result;
}

static void *fp_worker_thread(void *arg) {
    fp_worker *worker = (fp_worker *)arg;
    while (atomic_load_explicit(&worker->running, memory_order_acquire)) {
        fp_job *job = (fp_job *)fp_queue_pop(worker->job_queue);
        if (!job) {
            struct timespec ts = {0, 2000000};
            nanosleep(&ts, NULL);
            continue;
        }

        fp_result *result = fp_worker_handle_job(job);
        if (!result) {
            continue;
        }

        while (fp_queue_push(worker->result_queue, result) != 0) {
            struct timespec ts = {0, 2000000};
            nanosleep(&ts, NULL);
        }
    }

    return NULL;
}

fp_worker *fp_workers_create(size_t count, fp_queue *job_queue, fp_queue *result_queue, fp_progress_registry *progress_registry) {
    if (!job_queue || !result_queue || !progress_registry || count == 0) {
        return NULL;
    }

    fp_worker *workers = calloc(count, sizeof(fp_worker));
    if (!workers) {
        return NULL;
    }

    for (size_t i = 0; i < count; ++i) {
        workers[i].job_queue = job_queue;
        workers[i].result_queue = result_queue;
        workers[i].progress_registry = progress_registry;
        atomic_store_explicit(&workers[i].running, true, memory_order_release);
        if (pthread_create(&workers[i].thread, NULL, fp_worker_thread, &workers[i]) != 0) {
            atomic_store_explicit(&workers[i].running, false, memory_order_release);
            for (size_t j = 0; j < i; ++j) {
                atomic_store_explicit(&workers[j].running, false, memory_order_release);
                pthread_join(workers[j].thread, NULL);
            }
            free(workers);
            return NULL;
        }
    }

    return workers;
}

void fp_workers_destroy(fp_worker *workers, size_t count) {
    if (!workers) {
        return;
    }

    for (size_t i = 0; i < count; ++i) {
        atomic_store_explicit(&workers[i].running, false, memory_order_release);
    }

    for (size_t i = 0; i < count; ++i) {
        if (workers[i].thread) {
            pthread_join(workers[i].thread, NULL);
        }
    }

    free(workers);
}
static double worker_eta_update(const char *key, double elapsed_ms, double units) {
    if (!key || !*key || elapsed_ms <= 0) {
        return elapsed_ms;
    }
    if (units <= 0) {
        units = 1.0;
    }
    pthread_once(&g_eta_load_once, worker_eta_load);
    pthread_mutex_lock(&g_eta_mutex);
    fp_eta_entry *slot = NULL;
    size_t count = sizeof(g_eta_table.entries) / sizeof(g_eta_table.entries[0]);
    for (size_t i = 0; i < count; ++i) {
        fp_eta_entry *entry = &g_eta_table.entries[i];
        if (entry->samples == 0 && !slot) {
            slot = entry;
        }
        if (entry->samples > 0 && strncmp(entry->key, key, sizeof(entry->key)) == 0) {
            slot = entry;
            break;
        }
    }
    if (!slot) {
        slot = &g_eta_table.entries[0];
    }
    if (slot->samples == 0) {
        memset(slot, 0, sizeof(*slot));
        strncpy(slot->key, key, sizeof(slot->key) - 1);
        slot->key[sizeof(slot->key) - 1] = '\0';
    }
    slot->total_ms += elapsed_ms;
    slot->total_weight += units;
    slot->samples += 1;
    double avg_per_unit = slot->total_ms / slot->total_weight;
    double avg_for_job = avg_per_unit * units;
    g_eta_dirty_counter++;
    worker_eta_save();
    pthread_mutex_unlock(&g_eta_mutex);
    return avg_for_job;
}
