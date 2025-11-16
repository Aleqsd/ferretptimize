#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <strings.h>
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

static int fp_clamp_int(int value, int min_val, int max_val) {
    if (value < min_val) {
        return min_val;
    }
    if (value > max_val) {
        return max_val;
    }
    return value;
}

static int fp_job_tune_direction(const fp_job *job, const char *format, const char *label) {
    if (!job || job->tune_direction == 0 || !job->tune_format[0]) {
        return 0;
    }
    if (!format || strcasecmp(job->tune_format, format) != 0) {
        return 0;
    }
    if (job->tune_label[0] && label && strcasecmp(job->tune_label, label) != 0) {
        return 0;
    }
    return job->tune_direction;
}

static bool fp_should_run_task(const fp_job *job, const char *format, const char *label) {
    if (!job || job->tune_format[0] == '\0') {
        return true;
    }
    if (!format || strcasecmp(job->tune_format, format) != 0) {
        return false;
    }
    if (job->tune_label[0] && label && strcasecmp(job->tune_label, label) != 0) {
        return false;
    }
    return true;
}

static void worker_eta_make_key(char *dst, size_t dst_len, const char *base_key, double work_units) {
    if (!dst || dst_len == 0) {
        return;
    }
    if (!base_key || !*base_key) {
        base_key = "eta";
    }
    double units = work_units;
    if (units <= 0.0) {
        units = 0.25;
    }
    if (units > 32.0) {
        units = 32.0;
    }
    int bucket = (int)(units * 4.0 + 0.5); // quarter-megapixel buckets
    snprintf(dst, dst_len, "%s_%02d", base_key, bucket);
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
    int tune_direction;
    const char *format;
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

static fp_compress_code fp_worker_png_more(const fp_rgba_image *image, int unused, const char *label, fp_encoded_image *output);

static fp_compress_code fp_worker_avif_encode(const fp_rgba_image *image, int quality, const char *label, fp_encoded_image *output) {
    (void)label;
    return fp_compress_avif(image, quality, output);
}

static void fp_worker_free_encoded(fp_encoded_image *img) {
    if (!img || !img->data) {
        return;
    }
    free(img->data);
    img->data = NULL;
    img->size = 0;
}

static fp_compress_code fp_worker_png_more(const fp_rgba_image *image, int unused, const char *label, fp_encoded_image *output) {
    (void)unused;
    if (!output) {
        return FP_COMPRESS_ENCODE_ERROR;
    }
    fp_encoded_image candidates[3] = {0};
    fp_compress_code codes[3];
    codes[0] = fp_worker_png_encode(image, 9, label, &candidates[0]);
    codes[1] = fp_worker_png_encode(image, 7, label, &candidates[1]);
    codes[2] = fp_worker_png_encode(image, 6, label, &candidates[2]);

    size_t best_idx = 0;
    size_t best_size = (size_t)-1;
    bool found = false;
    for (size_t i = 0; i < 3; ++i) {
        if (codes[i] != FP_COMPRESS_OK || candidates[i].data == NULL) {
            fp_worker_free_encoded(&candidates[i]);
            continue;
        }
        if (!found || candidates[i].size < best_size) {
            best_size = candidates[i].size;
            best_idx = i;
            found = true;
        }
    }
    if (!found) {
        return FP_COMPRESS_ENCODE_ERROR;
    }

    *output = candidates[best_idx];
    candidates[best_idx].data = NULL;
    for (size_t i = 0; i < 3; ++i) {
        if (i == best_idx) {
            continue;
        }
        fp_worker_free_encoded(&candidates[i]);
    }
    return FP_COMPRESS_OK;
}

static void worker_eta_save_sample(const char *key, double elapsed_ms, double units) {
    if (!key || !*key || elapsed_ms <= 0 || units <= 0) {
        return;
    }
    FILE *f = fopen(g_eta_store_path, "a");
    if (!f) {
        return;
    }
    long pos = ftell(f);
    if (pos == 0) {
        fputs("# Format/label ETA samples (per-run)\n", f);
        fputs("# key elapsed_ms work_units\n", f);
    }
    fprintf(f, "%s %.6f %.6f\n", key, elapsed_ms, units);
    fclose(f);
}

static void worker_eta_load(void) {
    FILE *f = fopen(g_eta_store_path, "r");
    if (!f) {
        return;
    }
    while (true) {
        fp_eta_entry entry = {0};
        double a = 0.0, b = 0.0;
        unsigned s = 0;
        int read = fscanf(f, "%31s %lf %lf %u", entry.key, &a, &b, &s);
        if (read == EOF || read == 0) {
            break;
        }
        if (read == 3) {
            entry.total_ms = a;
            entry.total_weight = b;
            entry.samples = 1;
        } else if (read == 4) {
            entry.total_ms = a;
            entry.total_weight = b;
            entry.samples = s;
        } else {
            continue;
        }
        if (entry.samples <= 0 || entry.total_weight <= 0.0) {
            continue;
        }
        size_t count = sizeof(g_eta_table.entries) / sizeof(g_eta_table.entries[0]);
        fp_eta_entry *slot = NULL;
        for (size_t i = 0; i < count; ++i) {
            fp_eta_entry *existing = &g_eta_table.entries[i];
            if (existing->samples > 0 && strncmp(existing->key, entry.key, sizeof(existing->key)) == 0) {
                existing->total_ms += entry.total_ms;
                existing->total_weight += entry.total_weight;
                existing->samples += entry.samples;
                slot = existing;
                break;
            }
            if (!slot && existing->samples == 0) {
                slot = existing;
            }
        }
        if (slot && slot->samples == 0) {
            *slot = entry;
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
        if (task->tune_direction > 0) {
            strncpy(task->output->tuning, "more", sizeof(task->output->tuning) - 1);
            task->output->tuning[sizeof(task->output->tuning) - 1] = '\0';
        } else if (task->tune_direction < 0) {
            strncpy(task->output->tuning, "less", sizeof(task->output->tuning) - 1);
            task->output->tuning[sizeof(task->output->tuning) - 1] = '\0';
        } else {
            task->output->tuning[0] = '\0';
        }
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

    int png_tune = fp_job_tune_direction(job, "png", "lossless");
    int pngquant_tune = fp_job_tune_direction(job, "png", "pngquant q80");
    int webp_tune = fp_job_tune_direction(job, "webp", "high");
    int avif_tune = fp_job_tune_direction(job, "avif", "medium");

    int png_level = fp_clamp_int(png_tune > 0 ? 9 : (png_tune < 0 ? 1 : 5), 1, 9);
    int pngquant_colors = fp_clamp_int(pngquant_tune > 0 ? 96 : (pngquant_tune < 0 ? 192 : 128), 8, 256);
    int webp_quality = fp_clamp_int(webp_tune > 0 ? 60 : (webp_tune < 0 ? 96 : 90), 10, 100);
    int avif_quality = fp_clamp_int(avif_tune > 0 ? 36 : (avif_tune < 0 ? 20 : 28), 0, 63);

    const char *png_label = "lossless";
    const char *pngquant_label = "pngquant q80";
    const char *webp_label = "high";
    const char *avif_label = "medium";

    fp_encode_task tasks[4];
    char task_eta_keys[4][32];
    size_t task_count = 0;

    if (fp_should_run_task(job, "png", png_label)) {
        size_t idx = task_count;
        worker_eta_make_key(task_eta_keys[idx], sizeof(task_eta_keys[idx]), "png_lossless", work_units);
        int png_quality = png_level;
        fp_encode_fn png_encoder = fp_worker_png_encode;
        const char *png_log = png_tune != 0 ? "PNG lossless (tuned)" : "PNG lossless";
        if (png_tune > 0) {
            png_quality = 0; // unused
            png_encoder = fp_worker_png_more;
            png_log = "PNG lossless (tuned-more)";
        }
        tasks[idx] = (fp_encode_task){
            .image = &image,
            .quality = png_quality,
            .label = png_label,
            .log_name = png_log,
            .eta_key = task_eta_keys[idx],
            .output = &result->outputs[idx],
            .encode = png_encoder,
            .job = job,
            .failure_status = -2,
            .failure_message = "png_compress_error",
            .work_units = work_units,
            .tune_direction = png_tune,
            .format = "png",
        };
        task_count++;
    }

    if (fp_should_run_task(job, "png", pngquant_label)) {
        size_t idx = task_count;
        worker_eta_make_key(task_eta_keys[idx], sizeof(task_eta_keys[idx]), "png_quant", work_units);
        tasks[idx] = (fp_encode_task){
            .image = &image,
            .quality = pngquant_colors,
            .label = pngquant_label,
            .log_name = pngquant_tune != 0 ? "PNG pngquant (tuned)" : "PNG pngquant q80",
            .eta_key = task_eta_keys[idx],
            .output = &result->outputs[idx],
            .encode = fp_worker_png_quant,
            .job = job,
            .failure_status = -5,
            .failure_message = "pngquant_error",
            .work_units = work_units,
            .tune_direction = pngquant_tune,
            .format = "png",
        };
        task_count++;
    }

    if (fp_should_run_task(job, "webp", webp_label)) {
        size_t idx = task_count;
        worker_eta_make_key(task_eta_keys[idx], sizeof(task_eta_keys[idx]), "webp_high", work_units);
        tasks[idx] = (fp_encode_task){
            .image = &image,
            .quality = webp_quality,
            .label = webp_label,
            .log_name = webp_tune != 0 ? "WEBP high (tuned)" : "WEBP high",
            .eta_key = task_eta_keys[idx],
            .output = &result->outputs[idx],
            .encode = fp_worker_webp_encode,
            .job = job,
            .failure_status = -3,
            .failure_message = "webp_compress_error",
            .work_units = work_units,
            .tune_direction = webp_tune,
            .format = "webp",
        };
        task_count++;
    }

    if (fp_should_run_task(job, "avif", avif_label)) {
        size_t idx = task_count;
        worker_eta_make_key(task_eta_keys[idx], sizeof(task_eta_keys[idx]), "avif_medium", work_units);
        tasks[idx] = (fp_encode_task){
            .image = &image,
            .quality = avif_quality,
            .label = avif_label,
            .log_name = avif_tune != 0 ? "AVIF medium (tuned)" : "AVIF medium",
            .eta_key = task_eta_keys[idx],
            .output = &result->outputs[idx],
            .encode = fp_worker_avif_encode,
            .job = job,
            .failure_status = -4,
            .failure_message = "avif_compress_error",
            .work_units = work_units,
            .tune_direction = avif_tune,
            .format = "avif",
        };
        task_count++;
    }

    if (task_count == 0) {
        result->status = -6;
        strncpy(result->message, "unknown_tune_target", sizeof(result->message) - 1);
        fp_rgba_image_free(&image);
        fp_free_job(job);
        free(job);
        fp_result_finish(result);
        return result;
    }

    pthread_t threads[task_count];
    bool started[4] = {0};
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
    worker_eta_save_sample(key, elapsed_ms, units);
    pthread_mutex_unlock(&g_eta_mutex);
    return avg_for_job;
}
