#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "image_ops.h"

static void fp_image_ops_seed_report(fp_rgba_image *image, fp_image_ops_report *report) {
    if (!report || !image) {
        return;
    }
    report->original_width = image->width;
    report->original_height = image->height;
    report->final_width = image->width;
    report->final_height = image->height;
}

int fp_crop_image(fp_rgba_image *image, int x, int y, int width, int height, fp_image_ops_report *report) {
    if (!image || width <= 0 || height <= 0) {
        return -1;
    }
    fp_image_ops_seed_report(image, report);
    int iw = (int)image->width;
    int ih = (int)image->height;
    if (iw <= 0 || ih <= 0) {
        return -1;
    }

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= iw || y >= ih) {
        return -1;
    }

    if (x + width > iw) {
        width = iw - x;
    }
    if (y + height > ih) {
        height = ih - y;
    }
    if (width <= 0 || height <= 0) {
        return -1;
    }

    size_t stride = (size_t)image->width * 4;
    const uint8_t *src = image->pixels + (size_t)y * stride + (size_t)x * 4;
    size_t new_stride = (size_t)width * 4;
    uint8_t *cropped = malloc((size_t)width * (size_t)height * 4);
    if (!cropped) {
        return -1;
    }

    for (int row = 0; row < height; ++row) {
        memcpy(cropped + (size_t)row * new_stride, src + (size_t)row * stride, new_stride);
    }

    free(image->pixels);
    image->pixels = cropped;
    image->width = (unsigned)width;
    image->height = (unsigned)height;

    if (report) {
        report->crop_applied = 1;
        report->final_width = (unsigned)width;
        report->final_height = (unsigned)height;
    }

    return 0;
}

int fp_trim_image(fp_rgba_image *image, float tolerance, fp_image_ops_report *report) {
    if (!image || !image->pixels) {
        return -1;
    }
    fp_image_ops_seed_report(image, report);

    if (tolerance < 0.0f) {
        tolerance = 0.0f;
    }
    if (tolerance > 1.0f) {
        tolerance = 1.0f;
    }
    unsigned threshold = (unsigned)(tolerance * 255.0f + 0.5f);

    unsigned width = image->width;
    unsigned height = image->height;
    if (width == 0 || height == 0) {
        return -1;
    }

    int min_x = (int)width;
    int min_y = (int)height;
    int max_x = -1;
    int max_y = -1;
    size_t stride = (size_t)width * 4;

    for (unsigned y = 0; y < height; ++y) {
        const uint8_t *row = image->pixels + y * stride;
        for (unsigned x = 0; x < width; ++x) {
            const uint8_t *px = row + x * 4;
            unsigned alpha = px[3];
            if (alpha > threshold) {
                if ((int)x < min_x) min_x = (int)x;
                if ((int)y < min_y) min_y = (int)y;
                if ((int)x > max_x) max_x = (int)x;
                if ((int)y > max_y) max_y = (int)y;
            }
        }
    }

    if (max_x < min_x || max_y < min_y) {
        // Entirely transparent; keep smallest 1x1 to avoid zero-area.
        min_x = 0;
        min_y = 0;
        max_x = 0;
        max_y = 0;
    }

    int new_w = max_x - min_x + 1;
    int new_h = max_y - min_y + 1;
    if ((unsigned)new_w == image->width && (unsigned)new_h == image->height && min_x == 0 && min_y == 0) {
        return 0;
    }

    int rc = fp_crop_image(image, min_x, min_y, new_w, new_h, report);
    if (rc == 0 && report) {
        report->trim_applied = 1;
    }
    return rc;
}
