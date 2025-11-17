#pragma once

#include "compress.h"

typedef struct {
    unsigned original_width;
    unsigned original_height;
    unsigned final_width;
    unsigned final_height;
    int trim_applied;
    int crop_applied;
} fp_image_ops_report;

int fp_trim_image(fp_rgba_image *image, float tolerance, fp_image_ops_report *report);
int fp_crop_image(fp_rgba_image *image, int x, int y, int width, int height, fp_image_ops_report *report);
