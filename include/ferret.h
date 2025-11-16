#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define FP_MAX_OUTPUTS 6
#define FP_FILENAME_MAX 256

struct fp_progress_channel;

typedef struct {
    char format[8];
    char label[32];
    int quality;
    int compression_level;
    int lossless;
    int speed;
} fp_requested_output;

typedef struct {
    int enabled;
    float tolerance;
} fp_trim_options;

typedef struct {
    int enabled;
    int x;
    int y;
    int width;
    int height;
} fp_crop_options;

struct fp_encoded_image {
    char format[8];
    char label[32];
    char mime[32];
    char extension[8];
    char tuning[8];
    uint8_t *data;
    size_t size;
};

typedef struct fp_encoded_image fp_encoded_image;

typedef struct {
    uint64_t id;
    char filename[FP_FILENAME_MAX];
    uint8_t *data;
    size_t size;
    struct timespec enqueue_ts;
    struct fp_progress_channel *progress;
    char tune_format[8];
    char tune_label[32];
    int tune_direction;
    int is_expert;
    fp_requested_output requested_outputs[FP_MAX_OUTPUTS];
    size_t requested_output_count;
    fp_trim_options trim_options;
    fp_crop_options crop_options;
} fp_job;

typedef struct {
    uint64_t id;
    size_t input_size;
    fp_encoded_image outputs[FP_MAX_OUTPUTS];
    size_t output_count;
    int status;
    char message[128];
    struct timespec start_ts;
    struct timespec end_ts;
    unsigned input_width;
    unsigned input_height;
    unsigned output_width;
    unsigned output_height;
    int trim_applied;
    int crop_applied;
} fp_result;

void fp_free_result(fp_result *result);
void fp_free_job(fp_job *job);
