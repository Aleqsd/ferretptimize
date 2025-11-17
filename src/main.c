#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "queue.h"
#include "server.h"
#include "worker.h"
#include "progress.h"
#include "auth.h"

static void fp_load_env_file(const char *path) {
    if (!path) {
        return;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *cursor = line;
        while (*cursor == ' ' || *cursor == '\t') {
            ++cursor;
        }
        if (*cursor == '#' || *cursor == '\n' || *cursor == '\0') {
            continue;
        }
        char *eq = strchr(cursor, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = cursor;
        char *value = eq + 1;
        // trim trailing whitespace from key
        for (char *end = key + strlen(key) - 1; end >= key && isspace((unsigned char)*end); --end) {
            *end = '\0';
        }
        // trim trailing newline or spaces from value
        char *val_end = value + strlen(value);
        while (val_end > value && (val_end[-1] == '\n' || val_end[-1] == '\r' || isspace((unsigned char)val_end[-1]))) {
            *--val_end = '\0';
        }
        if (*key == '\0') {
            continue;
        }
        setenv(key, value, 0); // do not overwrite existing env vars
    }
    fclose(f);
}

static size_t fp_read_size_env(const char *name, size_t fallback) {
    const char *value = getenv(name);
    if (!value || !*value) {
        return fallback;
    }
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (end == value || parsed == 0) {
        return fallback;
    }
    return (size_t)parsed;
}

static int fp_read_int_env(const char *name, int fallback) {
    const char *value = getenv(name);
    if (!value || !*value) {
        return fallback;
    }
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value) {
        return fallback;
    }
    return (int)parsed;
}

int main(void) {
    fp_load_env_file(".env");

    const char *host = getenv("FERRET_HOST");
    if (!host || !*host) {
        host = "0.0.0.0";
    }
    int port = fp_read_int_env("FERRET_PORT", 4317);
    size_t worker_count = fp_read_size_env("FERRET_WORKERS", 4);
    if (worker_count == 0) {
        worker_count = 1;
    }
    size_t queue_size = fp_read_size_env("FERRET_QUEUE_SIZE", 128);
    if (queue_size < worker_count * 2) {
        queue_size = worker_count * 2;
    }

    fp_auth_store auth_store;
    if (fp_auth_store_init(&auth_store) != 0) {
        fprintf(stderr, "Failed to initialize auth and persistence\n");
        return 1;
    }

    fp_queue *job_queue = fp_queue_create(queue_size);
    fp_queue *result_queue = fp_queue_create(queue_size);
    if (!job_queue || !result_queue) {
        fprintf(stderr, "Failed to allocate queues\n");
        fp_queue_destroy(job_queue);
        fp_queue_destroy(result_queue);
        fp_auth_store_close(&auth_store);
        return 1;
    }

    fp_progress_registry *progress_registry = fp_progress_registry_create(queue_size * 2);
    if (!progress_registry) {
        fprintf(stderr, "Failed to create progress registry\n");
        fp_queue_destroy(job_queue);
        fp_queue_destroy(result_queue);
        fp_auth_store_close(&auth_store);
        return 1;
    }

    fp_worker *workers = fp_workers_create(worker_count, job_queue, result_queue, progress_registry);
    if (!workers) {
        fprintf(stderr, "Failed to start worker threads\n");
        fp_queue_destroy(job_queue);
        fp_queue_destroy(result_queue);
        fp_progress_registry_destroy(progress_registry);
        fp_auth_store_close(&auth_store);
        return 1;
    }

    int rc = fp_server_run(host, port, worker_count, job_queue, result_queue, progress_registry, &auth_store);

    fp_workers_destroy(workers, worker_count);
    fp_queue_destroy(job_queue);
    fp_queue_destroy(result_queue);
    fp_progress_registry_destroy(progress_registry);
    fp_auth_store_close(&auth_store);
    return rc == 0 ? 0 : 1;
}
