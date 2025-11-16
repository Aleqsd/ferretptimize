#pragma once

#include <stddef.h>
#include "queue.h"
#include "progress.h"

int fp_server_run(const char *host, int port, size_t worker_count,
                  fp_queue *job_queue, fp_queue *result_queue,
                  fp_progress_registry *progress_registry);
