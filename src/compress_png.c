#include <png.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "compress.h"

typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
} fp_png_buffer;

static void fp_png_buffer_init(fp_png_buffer *buffer) {
    buffer->data = NULL;
    buffer->size = 0;
    buffer->capacity = 0;
}

static int fp_png_buffer_append(fp_png_buffer *buffer, const png_bytep data, png_size_t length) {
    size_t required = buffer->size + length;
    if (required > buffer->capacity) {
        size_t new_capacity = buffer->capacity == 0 ? 4096 : buffer->capacity;
        while (new_capacity < required) {
            new_capacity *= 2;
        }
        uint8_t *new_data = realloc(buffer->data, new_capacity);
        if (!new_data) {
            return 0;
        }
        buffer->data = new_data;
        buffer->capacity = new_capacity;
    }
    memcpy(buffer->data + buffer->size, data, length);
    buffer->size += length;
    return 1;
}

static void fp_png_write_fn(png_structp png_ptr, png_bytep data, png_size_t length) {
    fp_png_buffer *buffer = (fp_png_buffer *)png_get_io_ptr(png_ptr);
    if (!fp_png_buffer_append(buffer, data, length)) {
        png_error(png_ptr, "memory allocation failure");
    }
}

static void fp_png_flush_fn(png_structp png_ptr) {
    (void)png_ptr;
}

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t offset;
} fp_png_source;

static void fp_png_read_mem(png_structp png_ptr, png_bytep out, png_size_t len) {
    fp_png_source *src = (fp_png_source *)png_get_io_ptr(png_ptr);
    if (!src || src->offset + len > src->size) {
        png_error(png_ptr, "read beyond buffer");
    }
    memcpy(out, src->data + src->offset, len);
    src->offset += len;
}

fp_compress_code fp_decode_png(const uint8_t *input, size_t size, fp_rgba_image *out_image) {
    if (!input || !out_image || size == 0) {
        return FP_COMPRESS_DECODE_ERROR;
    }

    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        return FP_COMPRESS_DECODE_ERROR;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_read_struct(&png_ptr, NULL, NULL);
        return FP_COMPRESS_DECODE_ERROR;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        return FP_COMPRESS_DECODE_ERROR;
    }

    fp_png_source source = {
        .data = input,
        .size = size,
        .offset = 0,
    };

    png_set_read_fn(png_ptr, &source, fp_png_read_mem);
    png_read_info(png_ptr, info_ptr);

    png_uint_32 width = 0;
    png_uint_32 height = 0;
    int bit_depth = 0;
    int color_type = 0;
    png_get_IHDR(png_ptr, info_ptr, &width, &height, &bit_depth, &color_type, NULL, NULL, NULL);

    if (bit_depth == 16) {
        png_set_strip_16(png_ptr);
    }
    if (color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png_ptr);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png_ptr);
    }
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS)) {
        png_set_tRNS_to_alpha(png_ptr);
    }
    if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_filler(png_ptr, 0xFF, PNG_FILLER_AFTER);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png_ptr);
    }

    png_read_update_info(png_ptr, info_ptr);

    png_size_t rowbytes = png_get_rowbytes(png_ptr, info_ptr);
    size_t stride = (size_t)rowbytes;
    size_t total = stride * (size_t)height;
    if (height != 0 && total / (size_t)height != stride) {
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        return FP_COMPRESS_DECODE_ERROR;
    }

    uint8_t *pixels = malloc(total);
    if (!pixels) {
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        return FP_COMPRESS_DECODE_ERROR;
    }

    png_bytep *rows = malloc(sizeof(png_bytep) * height);
    if (!rows) {
        free(pixels);
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        return FP_COMPRESS_DECODE_ERROR;
    }
    for (png_uint_32 y = 0; y < height; ++y) {
        rows[y] = pixels + (size_t)y * stride;
    }

    png_read_image(png_ptr, rows);
    png_read_end(png_ptr, NULL);

    free(rows);
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);

    out_image->pixels = pixels;
    out_image->width = (unsigned)width;
    out_image->height = (unsigned)height;
    return FP_COMPRESS_OK;
}

void fp_rgba_image_free(fp_rgba_image *image) {
    if (!image) {
        return;
    }
    free(image->pixels);
    image->pixels = NULL;
    image->width = 0;
    image->height = 0;
}

fp_compress_code fp_compress_png_level(const fp_rgba_image *image,
                                       int compression_level,
                                       const char *label_in,
                                       fp_encoded_image *output) {
    const char *volatile label = label_in;
    const char *label_text = (label && *label) ? (const char *)label : "variant";
    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        return FP_COMPRESS_ENCODE_ERROR;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_write_struct(&png_ptr, NULL);
        return FP_COMPRESS_ENCODE_ERROR;
    }

    fp_png_buffer buffer;
    fp_png_buffer_init(&buffer);

    png_bytep *rows = malloc(sizeof(png_bytep) * image->height);
    if (!rows) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        return FP_COMPRESS_ENCODE_ERROR;
    }

    for (unsigned y = 0; y < image->height; ++y) {
        rows[y] = (png_bytep)(image->pixels + (size_t)y * image->width * 4);
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        free(rows);
        free(buffer.data);
        png_destroy_write_struct(&png_ptr, &info_ptr);
        return FP_COMPRESS_ENCODE_ERROR;
    }

    png_set_write_fn(png_ptr, &buffer, fp_png_write_fn, fp_png_flush_fn);
    png_set_IHDR(png_ptr,
                 info_ptr,
                 image->width,
                 image->height,
                 8,
                 PNG_COLOR_TYPE_RGBA,
                 PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_BASE,
                 PNG_FILTER_TYPE_BASE);
    png_set_compression_level(png_ptr, compression_level);
    png_set_filter(png_ptr, PNG_FILTER_TYPE_BASE, PNG_ALL_FILTERS);

    png_set_rows(png_ptr, info_ptr, rows);
    png_write_info(png_ptr, info_ptr);
    png_write_image(png_ptr, rows);
    png_write_end(png_ptr, info_ptr);

    free(rows);
    png_destroy_write_struct(&png_ptr, &info_ptr);

    output->data = buffer.data;
    output->size = buffer.size;
    strncpy(output->format, "png", sizeof(output->format) - 1);
    strncpy(output->label, label_text, sizeof(output->label) - 1);
    strncpy(output->mime, "image/png", sizeof(output->mime) - 1);
    strncpy(output->extension, "png", sizeof(output->extension) - 1);
    output->format[sizeof(output->format) - 1] = '\0';
    output->label[sizeof(output->label) - 1] = '\0';
    output->mime[sizeof(output->mime) - 1] = '\0';
    output->extension[sizeof(output->extension) - 1] = '\0';
    output->tuning[0] = '\0';

    return FP_COMPRESS_OK;
}

#define FP_Q_BUCKET_BITS 4
#define FP_Q_BUCKET_COUNT (1 << (FP_Q_BUCKET_BITS * 4))

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
    uint32_t count;
} fp_quant_color;

typedef struct {
    int start;
    int end;
    uint8_t rmin, rmax;
    uint8_t gmin, gmax;
    uint8_t bmin, bmax;
    uint8_t amin, amax;
    uint64_t total;
} fp_color_box;

static inline void fp_quant_box_recalc(fp_color_box *box, const fp_quant_color *colors) {
    if (!box || box->start >= box->end) {
        return;
    }
    uint8_t rmin = 255, rmax = 0;
    uint8_t gmin = 255, gmax = 0;
    uint8_t bmin = 255, bmax = 0;
    uint8_t amin = 255, amax = 0;
    uint64_t total = 0;
    for (int i = box->start; i < box->end; ++i) {
        const fp_quant_color *c = &colors[i];
        if (c->r < rmin) rmin = c->r;
        if (c->r > rmax) rmax = c->r;
        if (c->g < gmin) gmin = c->g;
        if (c->g > gmax) gmax = c->g;
        if (c->b < bmin) bmin = c->b;
        if (c->b > bmax) bmax = c->b;
        if (c->a < amin) amin = c->a;
        if (c->a > amax) amax = c->a;
        total += c->count;
    }
    box->rmin = rmin;
    box->rmax = rmax;
    box->gmin = gmin;
    box->gmax = gmax;
    box->bmin = bmin;
    box->bmax = bmax;
    box->amin = amin;
    box->amax = amax;
    box->total = total;
}

static int fp_quant_sort_channel = 0;

static int fp_quant_compare(const void *lhs, const void *rhs) {
    const fp_quant_color *a = (const fp_quant_color *)lhs;
    const fp_quant_color *b = (const fp_quant_color *)rhs;
    int diff = 0;
    switch (fp_quant_sort_channel) {
        case 0: diff = (int)a->r - (int)b->r; break;
        case 1: diff = (int)a->g - (int)b->g; break;
        case 2: diff = (int)a->b - (int)b->b; break;
        case 3: diff = (int)a->a - (int)b->a; break;
        default: diff = 0; break;
    }
    if (diff == 0) {
        if (a->count == b->count) {
            return 0;
        }
        return (a->count < b->count) ? 1 : -1;
    }
    return diff;
}

static fp_compress_code fp_encode_png_palette(const uint8_t *indexed,
                                              unsigned width,
                                              unsigned height,
                                              const fp_quant_color *palette,
                                              int palette_count,
                                              const char *label,
                                              fp_encoded_image *output) {
    if (!indexed || !palette || palette_count <= 0 || palette_count > 256 || !output) {
        return FP_COMPRESS_ENCODE_ERROR;
    }

    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        return FP_COMPRESS_ENCODE_ERROR;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_write_struct(&png_ptr, NULL);
        return FP_COMPRESS_ENCODE_ERROR;
    }

    fp_png_buffer buffer;
    fp_png_buffer_init(&buffer);

    if (setjmp(png_jmpbuf(png_ptr))) {
        free(buffer.data);
        png_destroy_write_struct(&png_ptr, &info_ptr);
        return FP_COMPRESS_ENCODE_ERROR;
    }

    png_set_write_fn(png_ptr, &buffer, fp_png_write_fn, fp_png_flush_fn);
    png_set_IHDR(png_ptr,
                 info_ptr,
                 width,
                 height,
                 8,
                 PNG_COLOR_TYPE_PALETTE,
                 PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_BASE,
                 PNG_FILTER_TYPE_BASE);
    png_set_compression_level(png_ptr, 6);

    png_color png_palette[256];
    png_byte trans_alpha[256];
    int num_trans = 0;
    for (int i = 0; i < palette_count; ++i) {
        png_palette[i].red = palette[i].r;
        png_palette[i].green = palette[i].g;
        png_palette[i].blue = palette[i].b;
        if (palette[i].a < 255) {
            trans_alpha[i] = palette[i].a;
            num_trans = i + 1;
        } else if (num_trans > 0) {
            trans_alpha[i] = 255;
        }
    }
    png_set_PLTE(png_ptr, info_ptr, png_palette, palette_count);
    if (num_trans > 0) {
        png_set_tRNS(png_ptr, info_ptr, trans_alpha, num_trans, NULL);
    }

    png_bytep *rows = malloc(sizeof(png_bytep) * height);
    if (!rows) {
        free(buffer.data);
        png_destroy_write_struct(&png_ptr, &info_ptr);
        return FP_COMPRESS_ENCODE_ERROR;
    }
    for (unsigned y = 0; y < height; ++y) {
        rows[y] = (png_bytep)(indexed + (size_t)y * width);
    }

    png_set_rows(png_ptr, info_ptr, rows);
    png_write_png(png_ptr, info_ptr, PNG_TRANSFORM_IDENTITY, NULL);

    free(rows);
    png_destroy_write_struct(&png_ptr, &info_ptr);

    output->data = buffer.data;
    output->size = buffer.size;
    strncpy(output->format, "png", sizeof(output->format) - 1);
    strncpy(output->label, label ? label : "pngquant", sizeof(output->label) - 1);
    strncpy(output->mime, "image/png", sizeof(output->mime) - 1);
    strncpy(output->extension, "png", sizeof(output->extension) - 1);
    output->format[sizeof(output->format) - 1] = '\0';
    output->label[sizeof(output->label) - 1] = '\0';
    output->mime[sizeof(output->mime) - 1] = '\0';
    output->extension[sizeof(output->extension) - 1] = '\0';
    output->tuning[0] = '\0';

    return FP_COMPRESS_OK;
}

fp_compress_code fp_compress_png_quantized(const fp_rgba_image *image,
                                           int target_colors,
                                           const char *label,
                                           fp_encoded_image *output) {
    if (!image || !output) {
        return FP_COMPRESS_ENCODE_ERROR;
    }
    const size_t total_pixels = (size_t)image->width * image->height;
    if (total_pixels == 0) {
        return FP_COMPRESS_ENCODE_ERROR;
    }

    if (target_colors <= 0) {
        target_colors = 128;
    }
    if (target_colors > 256) {
        target_colors = 256;
    }

    uint32_t *counts = calloc(FP_Q_BUCKET_COUNT, sizeof(uint32_t));
    uint64_t *sum_r = calloc(FP_Q_BUCKET_COUNT, sizeof(uint64_t));
    uint64_t *sum_g = calloc(FP_Q_BUCKET_COUNT, sizeof(uint64_t));
    uint64_t *sum_b = calloc(FP_Q_BUCKET_COUNT, sizeof(uint64_t));
    uint64_t *sum_a = calloc(FP_Q_BUCKET_COUNT, sizeof(uint64_t));
    if (!counts || !sum_r || !sum_g || !sum_b || !sum_a) {
        free(counts);
        free(sum_r);
        free(sum_g);
        free(sum_b);
        free(sum_a);
        return FP_COMPRESS_ENCODE_ERROR;
    }

    for (size_t i = 0; i < total_pixels; ++i) {
        const uint8_t *px = image->pixels + i * 4;
        uint32_t bucket = ((uint32_t)(px[0] >> FP_Q_BUCKET_BITS) << 12) |
                          ((uint32_t)(px[1] >> FP_Q_BUCKET_BITS) << 8) |
                          ((uint32_t)(px[2] >> FP_Q_BUCKET_BITS) << 4) |
                          (uint32_t)(px[3] >> FP_Q_BUCKET_BITS);
        counts[bucket]++;
        sum_r[bucket] += px[0];
        sum_g[bucket] += px[1];
        sum_b[bucket] += px[2];
        sum_a[bucket] += px[3];
    }

    size_t color_capacity = 0;
    for (uint32_t i = 0; i < FP_Q_BUCKET_COUNT; ++i) {
        if (counts[i] > 0) {
            color_capacity++;
        }
    }
    if (color_capacity == 0) {
        free(counts);
        free(sum_r);
        free(sum_g);
        free(sum_b);
        free(sum_a);
        return FP_COMPRESS_ENCODE_ERROR;
    }

    fp_quant_color *colors = malloc(sizeof(fp_quant_color) * color_capacity);
    if (!colors) {
        free(counts);
        free(sum_r);
        free(sum_g);
        free(sum_b);
        free(sum_a);
        return FP_COMPRESS_ENCODE_ERROR;
    }
    size_t color_count = 0;
    for (uint32_t i = 0; i < FP_Q_BUCKET_COUNT; ++i) {
        if (counts[i] == 0) {
            continue;
        }
        fp_quant_color color = {
            .r = (uint8_t)(sum_r[i] / counts[i]),
            .g = (uint8_t)(sum_g[i] / counts[i]),
            .b = (uint8_t)(sum_b[i] / counts[i]),
            .a = (uint8_t)(sum_a[i] / counts[i]),
            .count = counts[i],
        };
        colors[color_count++] = color;
    }
    free(counts);
    free(sum_r);
    free(sum_g);
    free(sum_b);
    free(sum_a);

    if ((int)color_count < target_colors) {
        target_colors = (int)color_count;
    }
    if (target_colors <= 0) {
        target_colors = 1;
    }

    fp_color_box boxes[256];
    int box_count = 1;
    boxes[0].start = 0;
    boxes[0].end = (int)color_count;
    fp_quant_box_recalc(&boxes[0], colors);

    while (box_count < target_colors) {
        int index = -1;
        uint8_t widest = 0;
        for (int i = 0; i < box_count; ++i) {
            int span = boxes[i].end - boxes[i].start;
            if (span < 2) {
                continue;
            }
            uint8_t r_range = boxes[i].rmax - boxes[i].rmin;
            uint8_t g_range = boxes[i].gmax - boxes[i].gmin;
            uint8_t b_range = boxes[i].bmax - boxes[i].bmin;
            uint8_t a_range = boxes[i].amax - boxes[i].amin;
            uint8_t max_range = r_range;
            if (g_range > max_range) max_range = g_range;
            if (b_range > max_range) max_range = b_range;
            if (a_range > max_range) max_range = a_range;
            if (max_range >= widest) {
                widest = max_range;
                index = i;
            }
        }

        if (index == -1) {
            break;
        }

        fp_color_box box = boxes[index];
        uint8_t r_range = box.rmax - box.rmin;
        uint8_t g_range = box.gmax - box.gmin;
        uint8_t b_range = box.bmax - box.bmin;
        uint8_t a_range = box.amax - box.amin;
        if (r_range >= g_range && r_range >= b_range && r_range >= a_range) {
            fp_quant_sort_channel = 0;
        } else if (g_range >= r_range && g_range >= b_range && g_range >= a_range) {
            fp_quant_sort_channel = 1;
        } else if (b_range >= r_range && b_range >= g_range && b_range >= a_range) {
            fp_quant_sort_channel = 2;
        } else {
            fp_quant_sort_channel = 3;
        }
        qsort(colors + box.start, (size_t)(box.end - box.start), sizeof(fp_quant_color), fp_quant_compare);

        uint64_t half = box.total / 2;
        int mid = box.start;
        uint64_t accum = 0;
        while (mid < box.end && accum < half) {
            accum += colors[mid].count;
            mid++;
        }
        if (mid <= box.start) {
            mid = box.start + 1;
        } else if (mid >= box.end) {
            mid = box.end - 1;
        }

        fp_color_box new_box = {
            .start = mid,
            .end = box.end,
        };
        boxes[index].end = mid;
        fp_quant_box_recalc(&boxes[index], colors);
        fp_quant_box_recalc(&new_box, colors);
        boxes[box_count++] = new_box;
    }

    int palette_count = box_count;
    fp_quant_color palette[256];
    for (int i = 0; i < palette_count; ++i) {
        const fp_color_box *box = &boxes[i];
        uint64_t sum_r = 0, sum_g = 0, sum_b = 0, sum_a = 0, total = 0;
        for (int idx = box->start; idx < box->end; ++idx) {
            const fp_quant_color *c = &colors[idx];
            sum_r += (uint64_t)c->r * c->count;
            sum_g += (uint64_t)c->g * c->count;
            sum_b += (uint64_t)c->b * c->count;
            sum_a += (uint64_t)c->a * c->count;
            total += c->count;
        }
        if (total == 0) {
            total = 1;
        }
        palette[i].r = (uint8_t)(sum_r / total);
        palette[i].g = (uint8_t)(sum_g / total);
        palette[i].b = (uint8_t)(sum_b / total);
        palette[i].a = (uint8_t)(sum_a / total);
        palette[i].count = (uint32_t)total;
    }

    uint8_t *indexed = malloc(total_pixels);
    if (!indexed) {
        free(colors);
        return FP_COMPRESS_ENCODE_ERROR;
    }
    for (size_t i = 0; i < total_pixels; ++i) {
        const uint8_t *px = image->pixels + i * 4;
        int best = 0;
        uint32_t best_dist = UINT32_MAX;
        for (int p = 0; p < palette_count; ++p) {
            int dr = (int)px[0] - (int)palette[p].r;
            int dg = (int)px[1] - (int)palette[p].g;
            int db = (int)px[2] - (int)palette[p].b;
            int da = (int)px[3] - (int)palette[p].a;
            uint32_t dist = (uint32_t)(dr * dr + dg * dg + db * db + da * da);
            if (dist < best_dist) {
                best_dist = dist;
                best = p;
            }
        }
        indexed[i] = (uint8_t)best;
    }

    char label_text[32];
    if (label && *label) {
        strncpy(label_text, label, sizeof(label_text) - 1);
        label_text[sizeof(label_text) - 1] = '\0';
    } else {
        snprintf(label_text, sizeof(label_text), "pngquant q80");
    }
    fp_compress_code code = fp_encode_png_palette(indexed,
                                                  image->width,
                                                  image->height,
                                                  palette,
                                                  palette_count,
                                                  label_text,
                                                  output);
    free(indexed);
    free(colors);
    return code;
}
