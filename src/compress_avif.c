#include <avif/avif.h>
#include <stdlib.h>
#include <string.h>
#include "compress.h"

fp_compress_code fp_compress_avif(const fp_rgba_image *image,
                                  int quality,
                                  fp_encoded_image *output) {
    if (!image || !output || !image->pixels) {
        return FP_COMPRESS_ENCODE_ERROR;
    }

    avifImage *avif = avifImageCreate(image->width, image->height, 8, AVIF_PIXEL_FORMAT_YUV420);
    if (!avif) {
        return FP_COMPRESS_ENCODE_ERROR;
    }

    avifRGBImage rgb;
    avifRGBImageSetDefaults(&rgb, avif);
    rgb.format = AVIF_RGB_FORMAT_RGBA;
    rgb.depth = 8;
    rgb.rowBytes = image->width * 4;
    rgb.pixels = image->pixels;

    avifResult convert = avifImageRGBToYUV(avif, &rgb);
    if (convert != AVIF_RESULT_OK) {
        avifImageDestroy(avif);
        return FP_COMPRESS_ENCODE_ERROR;
    }

    avifEncoder *encoder = avifEncoderCreate();
    if (!encoder) {
        avifImageDestroy(avif);
        return FP_COMPRESS_ENCODE_ERROR;
    }

    encoder->speed = 6;
    encoder->maxThreads = 4;
    encoder->minQuantizer = quality;
    encoder->maxQuantizer = quality + 8;
    if (encoder->maxQuantizer > 63) {
        encoder->maxQuantizer = 63;
    }

    avifRWData output_data = AVIF_DATA_EMPTY;
    avifResult encode_res = avifEncoderWrite(encoder, avif, &output_data);
    if (encode_res != AVIF_RESULT_OK) {
        avifRWDataFree(&output_data);
        avifEncoderDestroy(encoder);
        avifImageDestroy(avif);
        return FP_COMPRESS_ENCODE_ERROR;
    }

    size_t encoded_size = output_data.size;
    uint8_t *buffer = malloc(encoded_size);
    if (!buffer) {
        avifRWDataFree(&output_data);
        avifEncoderDestroy(encoder);
        avifImageDestroy(avif);
        return FP_COMPRESS_ENCODE_ERROR;
    }

    memcpy(buffer, output_data.data, encoded_size);
    avifRWDataFree(&output_data);
    avifEncoderDestroy(encoder);
    avifImageDestroy(avif);

    output->data = buffer;
    output->size = encoded_size;
    strncpy(output->format, "avif", sizeof(output->format) - 1);
    strncpy(output->label, "medium", sizeof(output->label) - 1);
    strncpy(output->mime, "image/avif", sizeof(output->mime) - 1);
    strncpy(output->extension, "avif", sizeof(output->extension) - 1);

    output->format[sizeof(output->format) - 1] = '\0';
    output->label[sizeof(output->label) - 1] = '\0';
    output->mime[sizeof(output->mime) - 1] = '\0';
    output->extension[sizeof(output->extension) - 1] = '\0';

    return FP_COMPRESS_OK;
}
