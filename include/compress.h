#pragma once

#include "ferret.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *pixels; // RGBA
    unsigned width;
    unsigned height;
} fp_rgba_image;

typedef enum {
    FP_COMPRESS_OK = 0,
    FP_COMPRESS_DECODE_ERROR = 1,
    FP_COMPRESS_ENCODE_ERROR = 2,
    FP_COMPRESS_UNSUPPORTED = 3
} fp_compress_code;

fp_compress_code fp_decode_png(const uint8_t *input, size_t size, fp_rgba_image *out_image);
void fp_rgba_image_free(fp_rgba_image *image);

fp_compress_code fp_compress_png_level(const fp_rgba_image *image,
                                       int compression_level,
                                       const char *label,
                                       fp_encoded_image *output);

fp_compress_code fp_compress_png_quantized(const fp_rgba_image *image,
                                           int target_colors,
                                           const char *label,
                                           fp_encoded_image *output);

fp_compress_code fp_compress_webp(const fp_rgba_image *image,
                                  int quality,
                                  fp_encoded_image *output);

fp_compress_code fp_compress_avif(const fp_rgba_image *image,
                                  int quality,
                                  fp_encoded_image *output);

#ifdef __cplusplus
}
#endif
