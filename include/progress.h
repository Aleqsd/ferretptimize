#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ferret.h"

typedef enum {
    FP_PROGRESS_EVENT_OUTPUT = 0,
    FP_PROGRESS_EVENT_STATUS = 1,
} fp_progress_event_type;

typedef struct fp_progress_event {
    fp_progress_event_type type;
    char event_name[16];
    char *payload;
    struct fp_progress_event *next;
} fp_progress_event;

typedef struct fp_progress_channel fp_progress_channel;
typedef struct fp_progress_registry fp_progress_registry;

fp_progress_registry *fp_progress_registry_create(size_t capacity);
void fp_progress_registry_destroy(fp_progress_registry *registry);

fp_progress_channel *fp_progress_register(fp_progress_registry *registry, uint64_t job_id);
fp_progress_channel *fp_progress_acquire(fp_progress_registry *registry, uint64_t job_id);
void fp_progress_retain(fp_progress_channel *channel);
void fp_progress_release(fp_progress_channel *channel);

int fp_progress_emit_output(fp_progress_channel *channel,
                            const fp_encoded_image *output,
                            size_t input_size,
                            double duration_ms,
                            double avg_duration_ms);
int fp_progress_emit_status(fp_progress_channel *channel, const char *status, const char *message, double duration_ms, size_t input_size);
void fp_progress_close(fp_progress_channel *channel);

fp_progress_event *fp_progress_next_event(fp_progress_channel *channel, bool *is_open);
void fp_progress_event_free(fp_progress_event *event);
