#pragma once

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include "ferret.h"
#include "queue.h"
#include "progress.h"

typedef struct {
    fp_queue *job_queue;
    fp_queue *result_queue;
    fp_progress_registry *progress_registry;
    atomic_bool running;
    pthread_t thread;
} fp_worker;

fp_worker *fp_workers_create(size_t count, fp_queue *job_queue, fp_queue *result_queue, fp_progress_registry *progress_registry);
void fp_workers_destroy(fp_worker *workers, size_t count);
