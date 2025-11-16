#include <webp/encode.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "compress.h"

fp_compress_code fp_compress_webp(const fp_rgba_image *image,
                                  int quality,
                                  fp_encoded_image *output) {
    if (!image || !output || !image->pixels) {
        return FP_COMPRESS_ENCODE_ERROR;
    }

    uint8_t *encoded = NULL;
    size_t webp_size = WebPEncodeRGBA(image->pixels,
                                      (int)image->width,
                                      (int)image->height,
                                      (int)image->width * 4,
                                      (float)quality,
                                      &encoded);
    if (!webp_size || !encoded) {
        return FP_COMPRESS_ENCODE_ERROR;
    }

    uint8_t *buffer = malloc(webp_size);
    if (!buffer) {
        WebPFree(encoded);
        return FP_COMPRESS_ENCODE_ERROR;
    }

    memcpy(buffer, encoded, webp_size);
    WebPFree(encoded);

    output->data = buffer;
    output->size = webp_size;
    strncpy(output->format, "webp", sizeof(output->format) - 1);
    strncpy(output->label, "high", sizeof(output->label) - 1);
    strncpy(output->mime, "image/webp", sizeof(output->mime) - 1);
    strncpy(output->extension, "webp", sizeof(output->extension) - 1);

    output->format[sizeof(output->format) - 1] = '\0';
    output->label[sizeof(output->label) - 1] = '\0';
    output->mime[sizeof(output->mime) - 1] = '\0';
    output->extension[sizeof(output->extension) - 1] = '\0';

    return FP_COMPRESS_OK;
}

