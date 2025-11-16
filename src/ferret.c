#include <stdlib.h>
#include <string.h>
#include "ferret.h"
#include "progress.h"

void fp_free_result(fp_result *result) {
    if (!result) {
        return;
    }
    for (size_t i = 0; i < result->output_count; ++i) {
        free(result->outputs[i].data);
        result->outputs[i].data = NULL;
        result->outputs[i].size = 0;
    }
}

void fp_free_job(fp_job *job) {
    if (!job) {
        return;
    }
    free(job->data);
    job->data = NULL;
    job->size = 0;
    if (job->progress) {
        fp_progress_release(job->progress);
        job->progress = NULL;
    }
}
