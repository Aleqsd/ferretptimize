#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "image_ops.h"

#define TEST_ASSERT(cond)                                                                         \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "Assertion failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);         \
            exit(EXIT_FAILURE);                                                                    \
        }                                                                                          \
    } while (0)

static void set_pixel(fp_rgba_image *img, unsigned x, unsigned y, unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    size_t idx = ((size_t)y * img->width + x) * 4;
    img->pixels[idx + 0] = r;
    img->pixels[idx + 1] = g;
    img->pixels[idx + 2] = b;
    img->pixels[idx + 3] = a;
}

static void test_trim_transparent_border(void) {
    fp_rgba_image img = {0};
    img.width = 4;
    img.height = 4;
    img.pixels = calloc(img.width * img.height, 4);
    TEST_ASSERT(img.pixels != NULL);

    for (unsigned y = 1; y < 3; ++y) {
        for (unsigned x = 1; x < 3; ++x) {
            set_pixel(&img, x, y, 255, 0, 0, 255);
        }
    }

    fp_image_ops_report report = {0};
    TEST_ASSERT(fp_trim_image(&img, 0.0f, &report) == 0);
    TEST_ASSERT(report.trim_applied == 1);
    TEST_ASSERT(report.crop_applied == 1);
    TEST_ASSERT(img.width == 2 && img.height == 2);
    TEST_ASSERT(report.final_width == 2 && report.final_height == 2);
    free(img.pixels);
}

static void test_crop_preserves_region(void) {
    fp_rgba_image img = {0};
    img.width = 5;
    img.height = 4;
    img.pixels = calloc(img.width * img.height, 4);
    TEST_ASSERT(img.pixels != NULL);

    // Mark a pixel that should survive cropping
    set_pixel(&img, 2, 2, 7, 8, 9, 10);

    fp_image_ops_report report = {0};
    TEST_ASSERT(fp_crop_image(&img, 1, 1, 3, 2, &report) == 0);
    TEST_ASSERT(report.crop_applied == 1);
    TEST_ASSERT(img.width == 3 && img.height == 2);

    // Original (2,2) maps to (1,1) after cropping
    size_t idx = ((size_t)1 * img.width + 1) * 4;
    TEST_ASSERT(img.pixels[idx + 0] == 7);
    TEST_ASSERT(img.pixels[idx + 1] == 8);
    TEST_ASSERT(img.pixels[idx + 2] == 9);
    TEST_ASSERT(img.pixels[idx + 3] == 10);

    free(img.pixels);
}

void run_image_ops_tests(void) {
    printf("\n🧪 [image-ops] Trimming transparent border\n");
    test_trim_transparent_border();
    printf("✅ [image-ops] Trimmed to opaque bounds\n");

    printf("\n🧪 [image-ops] Cropping region\n");
    test_crop_preserves_region();
    printf("✅ [image-ops] Crop preserved pixel data\n");
}
