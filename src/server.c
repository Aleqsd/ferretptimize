#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <netinet/in.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "server.h"
#include "ferret.h"
#include "log.h"
#include "progress.h"

#define FP_MAX_HEADER (64 * 1024)
#define FP_MAX_UPLOAD (100 * 1024 * 1024)
#define FP_MIN_BUFFER 4096
#define FP_SLEEP_NS 2000000
#define FP_EXPERT_MAX_FILES 10
#define FP_EXPERT_MAX_FILE (20 * 1024 * 1024)
#define FP_EXPERT_MAX_TOTAL (100 * 1024 * 1024)
#define FP_EXPERT_MAX_DAILY_JOBS 500
#define FP_EXPERT_MAX_DAILY_BYTES (2ULL * 1024ULL * 1024ULL * 1024ULL)
#define FP_PRICE_MONTHLY_DEFAULT "price_expert_monthly"
#define FP_PRICE_ANNUAL_DEFAULT "price_expert_annual"
#define FP_PERIOD_MONTH_SECONDS (30 * 24 * 60 * 60)
#define FP_PERIOD_ANNUAL_SECONDS (365 * 24 * 60 * 60)

static const char *FP_PUBLIC_ROOT = "public";
static _Atomic uint64_t g_job_counter = 1;
static _Atomic uint64_t g_expert_request_count = 0;
static _Atomic uint64_t g_expert_request_files = 0;
static _Atomic uint64_t g_expert_request_bytes = 0;

typedef struct {
    uint64_t user_id;
    int day;
    uint64_t jobs;
    uint64_t bytes;
} fp_expert_usage_entry;

static fp_expert_usage_entry g_expert_usage[64];
static pthread_mutex_t g_expert_usage_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool fp_is_known_target(const char *format, const char *label) {
    if (!format || !*format) {
        return false;
    }
    if (strcasecmp(format, "png") == 0) {
        return !label || strcasecmp(label, "lossless") == 0 || strcasecmp(label, "pngquant q80") == 0;
    }
    if (strcasecmp(format, "webp") == 0) {
        return !label || strcasecmp(label, "high") == 0;
    }
    if (strcasecmp(format, "avif") == 0) {
        return !label || strcasecmp(label, "medium") == 0;
    }
    return false;
}

typedef struct {
    char name[64];
    char filename[FP_FILENAME_MAX];
    char content_type[128];
    const uint8_t *data;
    size_t size;
} fp_form_part;

typedef struct {
    int png_level;
    int png_quant_colors;
    int webp_quality;
    int avif_quality;
    int trim_enabled;
    float trim_tolerance;
    fp_crop_options crop;
} fp_expert_options;

typedef struct {
    char method[8];
    char path[256];
    char content_type[128];
    char filename[FP_FILENAME_MAX];
    char authorization[256];
    char cookies[512];
    char tune_format[8];
    char tune_label[32];
    int tune_direction;
    size_t content_length;
    uint64_t client_job_id;
} fp_http_request;

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} fp_buffer;

#define FP_APPEND_LITERAL(buffer, literal) fp_buffer_append((buffer), (literal), sizeof(literal) - 1)

static void fp_buffer_free(fp_buffer *buffer) {
    if (!buffer) {
        return;
    }
    free(buffer->data);
    buffer->data = NULL;
    buffer->size = 0;
    buffer->capacity = 0;
}

static int fp_buffer_reserve(fp_buffer *buffer, size_t additional) {
    size_t required = buffer->size + additional + 1;
    if (required <= buffer->capacity) {
        return 0;
    }
    size_t capacity = buffer->capacity ? buffer->capacity : 1024;
    while (capacity < required) {
        capacity *= 2;
    }
    char *next = realloc(buffer->data, capacity);
    if (!next) {
        return -1;
    }
    buffer->data = next;
    buffer->capacity = capacity;
    return 0;
}

static int fp_buffer_append(fp_buffer *buffer, const char *data, size_t len) {
    if (fp_buffer_reserve(buffer, len) != 0) {
        return -1;
    }
    memcpy(buffer->data + buffer->size, data, len);
    buffer->size += len;
    buffer->data[buffer->size] = '\0';
    return 0;
}

static int fp_buffer_append_char(fp_buffer *buffer, char c) {
    return fp_buffer_append(buffer, &c, 1);
}

static int fp_buffer_appendf(fp_buffer *buffer, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list args_copy;
    va_copy(args_copy, args);
    int needed = vsnprintf(NULL, 0, fmt, args_copy);
    va_end(args_copy);
    if (needed < 0) {
        va_end(args);
        return -1;
    }
    if (fp_buffer_reserve(buffer, (size_t)needed) != 0) {
        va_end(args);
        return -1;
    }
    vsnprintf(buffer->data + buffer->size, (size_t)needed + 1, fmt, args);
    buffer->size += (size_t)needed;
    va_end(args);
    return 0;
}

static int fp_buffer_append_json_string(fp_buffer *buffer, const char *value) {
    if (!value) {
        value = "";
    }
    if (fp_buffer_append_char(buffer, '"') != 0) {
        return -1;
    }
    for (const unsigned char *ptr = (const unsigned char *)value; *ptr; ++ptr) {
        unsigned char c = *ptr;
        switch (c) {
            case '\\':
                if (fp_buffer_append(buffer, "\\\\", 2) != 0) {
                    return -1;
                }
                break;
            case '"':
                if (fp_buffer_append(buffer, "\\\"", 2) != 0) {
                    return -1;
                }
                break;
            case '\n':
                if (fp_buffer_append(buffer, "\\n", 2) != 0) {
                    return -1;
                }
                break;
            case '\r':
                if (fp_buffer_append(buffer, "\\r", 2) != 0) {
                    return -1;
                }
                break;
            case '\t':
                if (fp_buffer_append(buffer, "\\t", 2) != 0) {
                    return -1;
                }
                break;
            default: {
                if (c < 0x20) {
                    char tmp[7];
                    snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                    if (fp_buffer_append(buffer, tmp, strlen(tmp)) != 0) {
                        return -1;
                    }
                } else {
                    if (fp_buffer_append_char(buffer, (char)c) != 0) {
                        return -1;
                    }
                }
                break;
            }
        }
    }
    if (fp_buffer_append_char(buffer, '"') != 0) {
        return -1;
    }
    return 0;
}

static int fp_base64url_decode(const char *input, uint8_t **out, size_t *out_len) {
    if (!input || !out || !out_len) {
        return -1;
    }
    size_t len = strlen(input);
    if (len == 0) {
        *out = NULL;
        *out_len = 0;
        return 0;
    }
    size_t padded_len = len;
    size_t mod = len % 4;
    if (mod) {
        padded_len += 4 - mod;
    }
    char *buf = malloc(padded_len + 1);
    if (!buf) {
        return -1;
    }
    for (size_t i = 0; i < len; ++i) {
        char c = input[i];
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
        buf[i] = c;
    }
    for (size_t i = len; i < padded_len; ++i) {
        buf[i] = '=';
    }
    buf[padded_len] = '\0';
    static const uint8_t table[256] = {
        ['A']=0,['B']=1,['C']=2,['D']=3,['E']=4,['F']=5,['G']=6,['H']=7,['I']=8,['J']=9,['K']=10,['L']=11,['M']=12,['N']=13,['O']=14,['P']=15,
        ['Q']=16,['R']=17,['S']=18,['T']=19,['U']=20,['V']=21,['W']=22,['X']=23,['Y']=24,['Z']=25,
        ['a']=26,['b']=27,['c']=28,['d']=29,['e']=30,['f']=31,['g']=32,['h']=33,['i']=34,['j']=35,['k']=36,['l']=37,['m']=38,['n']=39,['o']=40,['p']=41,
        ['q']=42,['r']=43,['s']=44,['t']=45,['u']=46,['v']=47,['w']=48,['x']=49,['y']=50,['z']=51,
        ['0']=52,['1']=53,['2']=54,['3']=55,['4']=56,['5']=57,['6']=58,['7']=59,['8']=60,['9']=61,
        ['+']=62,['/']=63
    };
    size_t out_capacity = (padded_len / 4) * 3;
    uint8_t *decoded = malloc(out_capacity);
    if (!decoded) {
        free(buf);
        return -1;
    }
    size_t out_idx = 0;
    for (size_t i = 0; i < padded_len; i += 4) {
        uint8_t a = table[(unsigned char)buf[i]];
        uint8_t b = table[(unsigned char)buf[i+1]];
        uint8_t c = buf[i+2] == '=' ? 0 : table[(unsigned char)buf[i+2]];
        uint8_t d = buf[i+3] == '=' ? 0 : table[(unsigned char)buf[i+3]];
        decoded[out_idx++] = (uint8_t)((a << 2) | (b >> 4));
        if (buf[i+2] != '=') {
            decoded[out_idx++] = (uint8_t)(((b & 0x0F) << 4) | (c >> 2));
        }
        if (buf[i+3] != '=') {
            decoded[out_idx++] = (uint8_t)(((c & 0x03) << 6) | d);
        }
    }
    free(buf);
    *out = decoded;
    *out_len = out_idx;
    return 0;
}

static int fp_parse_boundary(const char *content_type, char *boundary, size_t boundary_len) {
    if (!content_type || !boundary || boundary_len == 0) {
        return -1;
    }
    const char *b = strstr(content_type, "boundary=");
    if (!b) {
        return -1;
    }
    b += 9;
    while (*b == ' ' || *b == '\t') {
        ++b;
    }
    size_t len = 0;
    if (*b == '"') {
        ++b;
        while (b[len] && b[len] != '"' && len + 1 < boundary_len) {
            boundary[len] = b[len];
            ++len;
        }
    } else {
        while (b[len] && b[len] != ';' && b[len] != ' ' && len + 1 < boundary_len) {
            boundary[len] = b[len];
            ++len;
        }
    }
    boundary[len] = '\0';
    return len > 0 ? 0 : -1;
}

static int fp_extract_json_string(const char *json, const char *key, char *out, size_t out_len) {
    if (!json || !key || !out || out_len == 0) {
        return -1;
    }
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *pos = strstr(json, needle);
    if (!pos) {
        return 0;
    }
    pos = strchr(pos, ':');
    if (!pos) {
        return -1;
    }
    ++pos;
    while (*pos == ' ' || *pos == '\t' || *pos == '\n') {
        ++pos;
    }
    if (*pos != '"') {
        return -1;
    }
    ++pos;
    size_t idx = 0;
    while (*pos && *pos != '"' && idx + 1 < out_len) {
        out[idx++] = *pos++;
    }
    out[idx] = '\0';
    return idx > 0 ? 1 : 0;
}

static char *fp_find_json_value(const char *json, const char *key) {
    if (!json || !key) {
        return NULL;
    }
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    return strstr(json, needle);
}

static int fp_json_parse_int(const char *json, const char *key, int *out) {
    if (!json || !key || !out) {
        return -1;
    }
    char *loc = fp_find_json_value(json, key);
    if (!loc) {
        return 0; // not found
    }
    loc = strchr(loc, ':');
    if (!loc) {
        return -1;
    }
    ++loc;
    while (isspace((unsigned char)*loc)) {
        ++loc;
    }
    char *endptr = NULL;
    long val = strtol(loc, &endptr, 10);
    if (endptr == loc) {
        return -1;
    }
    *out = (int)val;
    return 1;
}

static int fp_json_parse_float(const char *json, const char *key, float *out) {
    if (!json || !key || !out) {
        return -1;
    }
    char *loc = fp_find_json_value(json, key);
    if (!loc) {
        return 0;
    }
    loc = strchr(loc, ':');
    if (!loc) {
        return -1;
    }
    ++loc;
    while (isspace((unsigned char)*loc)) {
        ++loc;
    }
    char *endptr = NULL;
    double val = strtod(loc, &endptr);
    if (endptr == loc) {
        return -1;
    }
    *out = (float)val;
    return 1;
}

static int fp_json_parse_bool(const char *json, const char *key, int *out) {
    if (!json || !key || !out) {
        return -1;
    }
    char *loc = fp_find_json_value(json, key);
    if (!loc) {
        return 0;
    }
    loc = strchr(loc, ':');
    if (!loc) {
        return -1;
    }
    ++loc;
    while (isspace((unsigned char)*loc)) {
        ++loc;
    }
    if (strncasecmp(loc, "true", 4) == 0) {
        *out = 1;
        return 1;
    }
    if (strncasecmp(loc, "false", 5) == 0) {
        *out = 0;
        return 1;
    }
    return -1;
}

static void fp_load_price_ids(char *monthly, size_t monthly_len, char *annual, size_t annual_len) {
    const char *monthly_env = getenv("FP_STRIPE_PRICE_MONTHLY");
    const char *annual_env = getenv("FP_STRIPE_PRICE_ANNUAL");
    if (monthly && monthly_len > 0) {
        snprintf(monthly, monthly_len, "%s", (monthly_env && *monthly_env) ? monthly_env : FP_PRICE_MONTHLY_DEFAULT);
    }
    if (annual && annual_len > 0) {
        snprintf(annual, annual_len, "%s", (annual_env && *annual_env) ? annual_env : FP_PRICE_ANNUAL_DEFAULT);
    }
}

static const char *fp_pick_price_id(const char *requested, const char *monthly, const char *annual, char *interval, size_t interval_len) {
    const char *price_id = monthly;
    if (interval && interval_len > 0) {
        snprintf(interval, interval_len, "monthly");
    }
    if (requested && *requested) {
        if (strcasecmp(requested, "annual") == 0 || strcasecmp(requested, "yearly") == 0) {
            price_id = annual;
            if (interval && interval_len > 0) {
                snprintf(interval, interval_len, "annual");
            }
        } else if (strcasecmp(requested, "monthly") == 0) {
            price_id = monthly;
            if (interval && interval_len > 0) {
                snprintf(interval, interval_len, "monthly");
            }
        } else if (strcasecmp(requested, annual) == 0) {
            price_id = annual;
            if (interval && interval_len > 0) {
                snprintf(interval, interval_len, "annual");
            }
        } else if (strcasecmp(requested, monthly) == 0) {
            price_id = monthly;
            if (interval && interval_len > 0) {
                snprintf(interval, interval_len, "monthly");
            }
        } else {
            price_id = requested;
            if (interval && interval_len > 0) {
                snprintf(interval, interval_len, "custom");
            }
        }
    }
    return price_id;
}

static void fp_generate_stub_id(const char *prefix, char *out, size_t out_len) {
    if (!out || out_len == 0) {
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    unsigned int rnd = (unsigned int)(ts.tv_nsec ^ ts.tv_sec ^ (unsigned int)getpid());
    snprintf(out, out_len, "%s%08x%08x", prefix ? prefix : "", rnd, (unsigned int)(rand() & 0xffffffff));
}

static time_t fp_period_for_price(const char *price_id, const char *monthly, const char *annual) {
    (void)monthly;
    if (price_id && annual && strcasecmp(price_id, annual) == 0) {
        return FP_PERIOD_ANNUAL_SECONDS;
    }
    return FP_PERIOD_MONTH_SECONDS;
}

static void fp_set_default_expert_options(fp_expert_options *opts) {
    if (!opts) {
        return;
    }
    memset(opts, 0, sizeof(*opts));
    opts->png_level = 6;
    opts->png_quant_colors = 128;
    opts->webp_quality = 90;
    opts->avif_quality = 28;
    opts->trim_tolerance = 0.01f;
}

static void fp_copy_expert_options(fp_expert_options *dst, const fp_expert_options *src) {
    if (!dst || !src) {
        return;
    }
    memcpy(dst, src, sizeof(*dst));
}

static double fp_elapsed_ms(const struct timespec *start, const struct timespec *end) {
    if (!start || !end) {
        return 0.0;
    }
    double start_ms = (double)start->tv_sec * 1000.0 + (double)start->tv_nsec / 1e6;
    double end_ms = (double)end->tv_sec * 1000.0 + (double)end->tv_nsec / 1e6;
    return end_ms - start_ms;
}

static int fp_parse_expert_metadata(const uint8_t *data, size_t len, fp_expert_options *opts) {
    if (!opts) {
        return -1;
    }
    if (!data || len == 0) {
        return 0;
    }
    char *json = strndup((const char *)data, len);
    if (!json) {
        return -1;
    }
    int val_int = 0;
    float val_float = 0.0f;
    int found = fp_json_parse_int(json, "pngLevel", &val_int);
    if (found == 1) {
        opts->png_level = val_int;
    }
    if (fp_json_parse_int(json, "pngQuantColors", &val_int) == 1) {
        opts->png_quant_colors = val_int;
    }
    if (fp_json_parse_int(json, "webpQuality", &val_int) == 1) {
        opts->webp_quality = val_int;
    }
    if (fp_json_parse_int(json, "avifQuality", &val_int) == 1) {
        opts->avif_quality = val_int;
    }
    if (fp_json_parse_bool(json, "trimEnabled", &val_int) == 1) {
        opts->trim_enabled = val_int;
    }
    if (fp_json_parse_float(json, "trimTolerance", &val_float) == 1) {
        opts->trim_tolerance = val_float;
    }

    char *trim_block = fp_find_json_value(json, "trim");
    if (trim_block) {
        if (fp_json_parse_bool(trim_block, "enabled", &val_int) == 1) {
            opts->trim_enabled = val_int;
        }
        if (fp_json_parse_float(trim_block, "tolerance", &val_float) == 1) {
            opts->trim_tolerance = val_float;
        }
    }

    char *crop_block = fp_find_json_value(json, "crop");
    if (crop_block) {
        int crop_enabled = 0;
        if (fp_json_parse_bool(crop_block, "enabled", &crop_enabled) == 1) {
            opts->crop.enabled = crop_enabled;
        }
        if (fp_json_parse_int(crop_block, "x", &val_int) == 1) {
            opts->crop.x = val_int;
        }
        if (fp_json_parse_int(crop_block, "y", &val_int) == 1) {
            opts->crop.y = val_int;
        }
        if (fp_json_parse_int(crop_block, "width", &val_int) == 1) {
            opts->crop.width = val_int;
        }
        if (fp_json_parse_int(crop_block, "height", &val_int) == 1) {
            opts->crop.height = val_int;
        }
    } else {
        if (fp_json_parse_bool(json, "cropEnabled", &val_int) == 1) {
            opts->crop.enabled = val_int;
        }
        if (fp_json_parse_int(json, "cropX", &val_int) == 1) {
            opts->crop.x = val_int;
        }
        if (fp_json_parse_int(json, "cropY", &val_int) == 1) {
            opts->crop.y = val_int;
        }
        if (fp_json_parse_int(json, "cropWidth", &val_int) == 1) {
            opts->crop.width = val_int;
        }
        if (fp_json_parse_int(json, "cropHeight", &val_int) == 1) {
            opts->crop.height = val_int;
        }
    }

    free(json);
    return 0;
}

static int fp_metadata_index_from_part_name(const char *name) {
    if (!name) {
        return -1;
    }
    if (strncasecmp(name, "metadata", 8) != 0) {
        return -1;
    }
    const char *cursor = name + 8;
    while (*cursor && (*cursor == '[' || *cursor == ']' || *cursor == '_' || *cursor == '-')) {
        ++cursor;
    }
    if (!*cursor) {
        return -1;
    }
    char *endptr = NULL;
    long idx = strtol(cursor, &endptr, 10);
    if (endptr == cursor || idx < 0 || idx > FP_EXPERT_MAX_FILES) {
        return -1;
    }
    return (int)idx;
}

static int fp_current_day_key(void) {
    time_t now = time(NULL);
    return (int)(now / 86400);
}

static int fp_track_expert_usage(uint64_t user_id, size_t file_count, size_t total_bytes, char *err, size_t err_len) {
    if (user_id == 0) {
        return 0; // allow unaffiliated keys (dev env)
    }
    int today = fp_current_day_key();
    int rc = 0;
    pthread_mutex_lock(&g_expert_usage_mutex);
    size_t slot = 0;
    for (; slot < sizeof(g_expert_usage) / sizeof(g_expert_usage[0]); ++slot) {
        fp_expert_usage_entry *entry = &g_expert_usage[slot];
        if (entry->user_id == user_id && entry->day == today) {
            break;
        }
        if (entry->user_id == 0 || entry->day != today) {
            entry->user_id = user_id;
            entry->day = today;
            entry->jobs = 0;
            entry->bytes = 0;
            break;
        }
    }
    if (slot == sizeof(g_expert_usage) / sizeof(g_expert_usage[0])) {
        rc = -1;
        if (err && err_len) {
            snprintf(err, err_len, "Server busy");
        }
    } else {
        fp_expert_usage_entry *entry = &g_expert_usage[slot];
        if (entry->jobs + file_count > FP_EXPERT_MAX_DAILY_JOBS) {
            rc = -1;
            if (err && err_len) {
                snprintf(err, err_len, "Daily job limit reached");
            }
        } else if (entry->bytes + total_bytes > FP_EXPERT_MAX_DAILY_BYTES) {
            rc = -1;
            if (err && err_len) {
                snprintf(err, err_len, "Daily byte limit reached");
            }
        } else {
            entry->jobs += file_count;
            entry->bytes += total_bytes;
        }
    }
    pthread_mutex_unlock(&g_expert_usage_mutex);
    return rc;
}

static const uint8_t *fp_memsearch(const uint8_t *haystack, size_t haystack_len, const uint8_t *needle, size_t needle_len) {
    if (!haystack || !needle || needle_len == 0 || haystack_len < needle_len) {
        return NULL;
    }
    for (size_t i = 0; i + needle_len <= haystack_len; ++i) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            return haystack + i;
        }
    }
    return NULL;
}

static int fp_parse_multipart(const uint8_t *body, size_t body_len, const char *boundary, fp_form_part *parts, size_t max_parts, size_t *out_count) {
    if (!body || !boundary || !parts || max_parts == 0) {
        return -1;
    }
    char boundary_line[128];
    snprintf(boundary_line, sizeof(boundary_line), "--%s", boundary);
    size_t boundary_line_len = strlen(boundary_line);
    const uint8_t *cursor = body;
    const uint8_t *end = body + body_len;
    size_t count = 0;

    while (true) {
        const uint8_t *marker = fp_memsearch(cursor, (size_t)(end - cursor), (const uint8_t *)boundary_line, boundary_line_len);
        if (!marker) {
            break;
        }
        marker += boundary_line_len;
        if (marker + 2 <= end && marker[0] == '-' && marker[1] == '-') {
            break; // end marker
        }
        if (marker + 2 > end || marker[0] != '\r' || marker[1] != '\n') {
            return -1;
        }
        const uint8_t *header_start = marker + 2;
        const uint8_t *header_end = fp_memsearch(header_start, (size_t)(end - header_start), (const uint8_t *)"\r\n\r\n", 4);
        if (!header_end) {
            return -1;
        }
        size_t header_len = (size_t)(header_end - header_start);
        char *header_copy = strndup((const char *)header_start, header_len);
        if (!header_copy) {
            return -1;
        }

        fp_form_part part = {0};
        char *saveptr = NULL;
        char *line = strtok_r(header_copy, "\r\n", &saveptr);
        while (line) {
            if (strncasecmp(line, "content-disposition", 19) == 0) {
                char *name_pos = strstr(line, "name=\"");
                if (name_pos) {
                    name_pos += 6;
                    char *endq = strchr(name_pos, '"');
                    if (endq) {
                        size_t len = (size_t)(endq - name_pos);
                        if (len >= sizeof(part.name)) {
                            len = sizeof(part.name) - 1;
                        }
                        memcpy(part.name, name_pos, len);
                        part.name[len] = '\0';
                    }
                }
                char *file_pos = strstr(line, "filename=\"");
                if (file_pos) {
                    file_pos += 10;
                    char *endq = strchr(file_pos, '"');
                    if (endq) {
                        size_t len = (size_t)(endq - file_pos);
                        if (len >= sizeof(part.filename)) {
                            len = sizeof(part.filename) - 1;
                        }
                        memcpy(part.filename, file_pos, len);
                        part.filename[len] = '\0';
                    }
                }
            } else if (strncasecmp(line, "content-type", 12) == 0) {
                char *colon = strchr(line, ':');
                if (colon) {
                    ++colon;
                    while (*colon == ' ' || *colon == '\t') {
                        ++colon;
                    }
                    strncpy(part.content_type, colon, sizeof(part.content_type) - 1);
                }
            }
            line = strtok_r(NULL, "\r\n", &saveptr);
        }
        free(header_copy);

        const uint8_t *data_start = header_end + 4;
        const uint8_t *next_boundary = fp_memsearch(data_start, (size_t)(end - data_start), (const uint8_t *)boundary_line, boundary_line_len);
        if (!next_boundary) {
            return -1;
        }
        if (next_boundary < body + 2 || next_boundary[-2] != '\r' || next_boundary[-1] != '\n') {
            return -1;
        }
        part.data = data_start;
        part.size = (size_t)(next_boundary - data_start - 2); // strip trailing \r\n before boundary

        if (part.name[0] && count < max_parts) {
            parts[count++] = part;
        }

        cursor = next_boundary;
    }

    if (out_count) {
        *out_count = count;
    }
    return 0;
}

static ssize_t fp_find_header_boundary(const char *buffer, size_t len) {
    if (len < 4) {
        return -1;
    }
    for (size_t i = 3; i < len; ++i) {
        if (buffer[i - 3] == '\r' && buffer[i - 2] == '\n' && buffer[i - 1] == '\r' && buffer[i] == '\n') {
            return (ssize_t)(i + 1);
        }
    }
    return -1;
}

static int fp_read_header_block(int fd, char **buffer_out, size_t *header_len_out, size_t *total_len_out) {
    size_t capacity = FP_MIN_BUFFER;
    char *buffer = malloc(capacity);
    if (!buffer) {
        return -1;
    }
    size_t len = 0;
    while (len < FP_MAX_HEADER) {
        if (capacity - len < FP_MIN_BUFFER) {
            size_t next_capacity = capacity * 2;
            char *tmp = realloc(buffer, next_capacity);
            if (!tmp) {
                free(buffer);
                return -1;
            }
            capacity = next_capacity;
            buffer = tmp;
        }
        ssize_t received = recv(fd, buffer + len, capacity - len, 0);
        if (received <= 0) {
            free(buffer);
            return -1;
        }
        len += (size_t)received;
        ssize_t boundary = fp_find_header_boundary(buffer, len);
        if (boundary >= 0) {
            *buffer_out = buffer;
            *header_len_out = (size_t)boundary;
            *total_len_out = len;
            return 0;
        }
    }

    free(buffer);
    return -1;
}

static int fp_parse_request(char *header_block, fp_http_request *request) {
    memset(request, 0, sizeof(*request));
    char *saveptr = NULL;
    char *line = strtok_r(header_block, "\r\n", &saveptr);
    if (!line) {
        return -1;
    }
    if (sscanf(line, "%7s %255s", request->method, request->path) != 2) {
        return -1;
    }
    for (char *p = request->method; *p; ++p) {
        *p = (char)toupper((unsigned char)*p);
    }

    while ((line = strtok_r(NULL, "\r\n", &saveptr))) {
        if (*line == '\0') {
            break;
        }
        char *colon = strchr(line, ':');
        if (!colon) {
            continue;
        }
        *colon = '\0';
        char *name = line;
        char *value = colon + 1;
        while (*value == ' ' || *value == '\t') {
            ++value;
        }
        for (char *p = name; *p; ++p) {
            *p = (char)tolower((unsigned char)*p);
        }
        if (strcmp(name, "content-length") == 0) {
            request->content_length = (size_t)strtoull(value, NULL, 10);
        } else if (strcmp(name, "content-type") == 0) {
            strncpy(request->content_type, value, sizeof(request->content_type) - 1);
        } else if (strcmp(name, "x-filename") == 0) {
            strncpy(request->filename, value, sizeof(request->filename) - 1);
        } else if (strcmp(name, "authorization") == 0) {
            strncpy(request->authorization, value, sizeof(request->authorization) - 1);
        } else if (strcmp(name, "cookie") == 0) {
            strncpy(request->cookies, value, sizeof(request->cookies) - 1);
        } else if (strcmp(name, "x-job-id") == 0) {
            request->client_job_id = strtoull(value, NULL, 10);
        } else if (strcmp(name, "x-tune-format") == 0) {
            strncpy(request->tune_format, value, sizeof(request->tune_format) - 1);
        } else if (strcmp(name, "x-tune-label") == 0) {
            strncpy(request->tune_label, value, sizeof(request->tune_label) - 1);
        } else if (strcmp(name, "x-tune-intent") == 0) {
            if (strncasecmp(value, "more", 4) == 0) {
                request->tune_direction = 1;
            } else if (strncasecmp(value, "less", 4) == 0) {
                request->tune_direction = -1;
            }
        }
    }
    return 0;
}

static void fp_trim_token(char *value) {
    if (!value) {
        return;
    }
    char *start = value;
    while (*start && isspace((unsigned char)*start)) {
        ++start;
    }
    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    if (start != value) {
        memmove(value, start, (size_t)(end - start + 1));
    }
}

static void fp_get_cookie_value(const fp_http_request *request, const char *name, char *out, size_t out_len) {
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!request || !name || !*name) {
        return;
    }
    const char *cookie_header = request->cookies;
    if (!cookie_header || !*cookie_header) {
        return;
    }
    size_t name_len = strlen(name);
    const char *cursor = cookie_header;
    while (*cursor) {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == ';') {
            ++cursor;
        }
        if (strncmp(cursor, name, name_len) == 0 && cursor[name_len] == '=') {
            cursor += name_len + 1;
            size_t idx = 0;
            while (*cursor && *cursor != ';' && idx + 1 < out_len) {
                out[idx++] = *cursor++;
            }
            out[idx] = '\0';
            fp_trim_token(out);
            return;
        }
        while (*cursor && *cursor != ';') {
            ++cursor;
        }
        if (*cursor == ';') {
            ++cursor;
        }
    }
}

static void fp_extract_auth_token(const char *auth_header, char *out, size_t out_len) {
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!auth_header) {
        return;
    }
    while (*auth_header && isspace((unsigned char)*auth_header)) {
        ++auth_header;
    }
    if (strncasecmp(auth_header, "bearer ", 7) == 0) {
        auth_header += 7;
    } else if (strncasecmp(auth_header, "apikey ", 7) == 0) {
        auth_header += 7;
    }
    while (*auth_header && isspace((unsigned char)*auth_header)) {
        ++auth_header;
    }
    size_t copy_len = strnlen(auth_header, out_len - 1);
    memcpy(out, auth_header, copy_len);
    out[copy_len] = '\0';
    fp_trim_token(out);
}

static bool fp_match_api_key(const char *token, const char *csv) {
    if (!token || !*token || !csv || !*csv) {
        return false;
    }
    const char *cursor = csv;
    while (*cursor) {
        while (*cursor == ',' || isspace((unsigned char)*cursor)) {
            ++cursor;
        }
        if (!*cursor) {
            break;
        }
        const char *start = cursor;
        while (*cursor && *cursor != ',') {
            ++cursor;
        }
        size_t len = (size_t)(cursor - start);
        while (len > 0 && isspace((unsigned char)start[len - 1])) {
            --len;
        }
        if (len > 0 && len < 256) {
            char candidate[256];
            memcpy(candidate, start, len);
            candidate[len] = '\0';
            fp_trim_token(candidate);
            if (candidate[0] != '\0' && strcmp(candidate, token) == 0) {
                return true;
            }
        }
        if (*cursor == ',') {
            ++cursor;
        }
    }
    return false;
}

static int fp_authenticate_request(fp_auth_store *auth_store, const fp_http_request *request, fp_auth_user *out_user);

static bool fp_is_expert_authorized(const fp_http_request *request,
                                    fp_auth_store *auth_store,
                                    fp_auth_user *out_user,
                                    char *auth_source,
                                    size_t auth_source_len,
                                    char *deny_reason,
                                    size_t deny_reason_len) {
    const char *keys_csv = getenv("FP_EXPERT_API_KEYS");
    const char *single_key = getenv("FP_EXPERT_API_KEY");
    if (auth_source && auth_source_len) {
        auth_source[0] = '\0';
    }
    if (deny_reason && deny_reason_len) {
        deny_reason[0] = '\0';
    }
    if (out_user) {
        memset(out_user, 0, sizeof(*out_user));
    }
    char token[256];
    fp_extract_auth_token(request ? request->authorization : NULL, token, sizeof(token));
    bool env_guard = (keys_csv && *keys_csv) || (single_key && *single_key);
    if (!env_guard && token[0] == '\0') {
        if (auth_source && auth_source_len) {
            snprintf(auth_source, auth_source_len, "dev");
        }
        return true; // dev mode: allow requests without keys
    }
    if (token[0] != '\0') {
        if (fp_match_api_key(token, keys_csv) || fp_match_api_key(token, single_key)) {
            if (auth_source && auth_source_len) {
                snprintf(auth_source, auth_source_len, "env_api_key");
            }
            return true;
        }
        if (auth_store) {
            fp_auth_user key_user = {0};
            if (fp_auth_api_key_allowed(auth_store, token, "expert", &key_user)) {
                if (fp_auth_has_active_subscription(auth_store, key_user.id)) {
                    if (out_user) {
                        *out_user = key_user;
                    }
                    if (auth_source && auth_source_len) {
                        snprintf(auth_source, auth_source_len, "api_key");
                    }
                    return true;
                }
                fp_log_warn("🔒 API key rejected: subscription inactive for user %llu", (unsigned long long)key_user.id);
                if (deny_reason && deny_reason_len) {
                    snprintf(deny_reason, deny_reason_len, "Subscription inactive");
                }
                return false;
            }
        }
    }
    if (auth_store) {
        fp_auth_user bearer = {0};
        if (fp_authenticate_request(auth_store, request, &bearer) == 1) {
            if (fp_auth_has_active_subscription(auth_store, bearer.id)) {
                if (out_user) {
                    *out_user = bearer;
                }
                if (auth_source && auth_source_len) {
                    snprintf(auth_source, auth_source_len, "jwt");
                }
                return true;
            }
            if (deny_reason && deny_reason_len) {
                snprintf(deny_reason, deny_reason_len, "Subscription inactive");
            }
            return false;
        }
    }
    if (deny_reason && deny_reason_len) {
        snprintf(deny_reason, deny_reason_len, "Expert mode requires Authorization: ApiKey <token>");
    }
    return false;
}

static int fp_authenticate_request(fp_auth_store *auth_store, const fp_http_request *request, fp_auth_user *out_user) {
    if (!auth_store || !request) {
        return -1;
    }
    char token[512];
    fp_extract_auth_token(request->authorization, token, sizeof(token));
    if (token[0] == '\0') {
        fp_get_cookie_value(request, "fp_access", token, sizeof(token));
    }
    if (token[0] == '\0') {
        return 0;
    }
    if (fp_auth_validate_access(auth_store, token, out_user) != 0) {
        return -1;
    }
    return 1;
}

static bool fp_parse_stream_path(const char *path, uint64_t *job_id_out) {
    if (!path || strncmp(path, "/api/jobs/", 10) != 0) {
        return false;
    }
    const char *cursor = path + 10;
    char *endptr = NULL;
    unsigned long long candidate = strtoull(cursor, &endptr, 10);
    if (!endptr || candidate == 0) {
        return false;
    }
    if (strcmp(endptr, "/events") != 0) {
        return false;
    }
    if (job_id_out) {
        *job_id_out = (uint64_t)candidate;
    }
    return true;
}

static const char *fp_guess_mime(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) {
        return "application/octet-stream";
    }
    if (strcmp(ext, ".html") == 0) {
        return "text/html; charset=utf-8";
    }
    if (strcmp(ext, ".css") == 0) {
        return "text/css; charset=utf-8";
    }
    if (strcmp(ext, ".js") == 0) {
        return "application/javascript";
    }
    if (strcmp(ext, ".png") == 0) {
        return "image/png";
    }
    if (strcmp(ext, ".svg") == 0) {
        return "image/svg+xml";
    }
    return "application/octet-stream";
}

static int fp_send_buffer(int fd, const void *data, size_t len) {
    const uint8_t *bytes = (const uint8_t *)data;
    size_t sent = 0;
    while (sent < len) {
#ifdef _WIN32
        ssize_t wrote = send(fd, (const char *)(bytes + sent), (int)(len - sent), 0);
#else
        ssize_t wrote = send(fd, bytes + sent, len - sent, 0);
#endif
        if (wrote <= 0) {
            return -1;
        }
        sent += (size_t)wrote;
    }
    return 0;
}

static int fp_send_http_with_headers(int fd, int status, const char *status_text, const char *content_type,
                                     const char *extra_headers, const void *body, size_t body_len) {
    if (!status_text) {
        status_text = "OK";
    }
    if (!content_type) {
        content_type = "text/plain; charset=utf-8";
    }
    fp_buffer header = {0};
    if (fp_buffer_appendf(&header,
                          "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nAccess-Control-Allow-Origin: *\r\n",
                          status,
                          status_text,
                          content_type,
                          body_len) != 0) {
        fp_buffer_free(&header);
        return -1;
    }
    if (extra_headers && *extra_headers) {
        if (fp_buffer_append(&header, extra_headers, strlen(extra_headers)) != 0) {
            fp_buffer_free(&header);
            return -1;
        }
    }
    if (FP_APPEND_LITERAL(&header, "Connection: close\r\n\r\n") != 0) {
        fp_buffer_free(&header);
        return -1;
    }
    if (fp_send_buffer(fd, header.data, header.size) != 0) {
        fp_buffer_free(&header);
        return -1;
    }
    fp_buffer_free(&header);
    if (body && body_len > 0) {
        if (fp_send_buffer(fd, body, body_len) != 0) {
            return -1;
        }
    }
    return 0;
}

static int fp_send_http(int fd, int status, const char *status_text, const char *content_type, const void *body, size_t body_len) {
    return fp_send_http_with_headers(fd, status, status_text, content_type, NULL, body, body_len);
}

static int fp_send_text(int fd, int status, const char *status_text, const char *message) {
    const char *body = message ? message : "";
    return fp_send_http(fd, status, status_text, "text/plain; charset=utf-8", body, strlen(body));
}

static int fp_build_filesystem_path(const char *request_path, char *out_path, size_t out_len) {
    if (!request_path || !out_path) {
        return -1;
    }
    const char *path = request_path;
    if (strcmp(request_path, "/") == 0) {
        path = "/index.html";
    }
    if (strstr(path, "..")) {
        return -1;
    }
    while (*path == '/') {
        ++path;
    }
    if (*path == '\0') {
        path = "index.html";
    }
    int written = snprintf(out_path, out_len, "%s/%s", FP_PUBLIC_ROOT, path);
    if (written < 0 || (size_t)written >= out_len) {
        return -1;
    }
    return 0;
}

static int fp_send_static_file(int fd, const char *request_path) {
    char fs_path[1024];
    if (fp_build_filesystem_path(request_path, fs_path, sizeof(fs_path)) != 0) {
        return fp_send_text(fd, 403, "Forbidden", "Forbidden");
    }

    int file_fd = open(fs_path, O_RDONLY);
    if (file_fd < 0) {
        return fp_send_text(fd, 404, "Not Found", "Not Found");
    }

    struct stat st;
    if (fstat(file_fd, &st) != 0) {
        close(file_fd);
        return fp_send_text(fd, 500, "Error", "Failed to stat file");
    }

    size_t size = (size_t)st.st_size;
    char *buffer = malloc(size);
    if (!buffer) {
        close(file_fd);
        return fp_send_text(fd, 500, "Error", "Out of memory");
    }

    size_t read_total = 0;
    while (read_total < size) {
        ssize_t read_bytes = read(file_fd, buffer + read_total, size - read_total);
        if (read_bytes <= 0) {
            free(buffer);
            close(file_fd);
            return fp_send_text(fd, 500, "Error", "Failed to read file");
        }
        read_total += (size_t)read_bytes;
    }

    close(file_fd);
    int rc = fp_send_http(fd, 200, "OK", fp_guess_mime(fs_path), buffer, size);
    free(buffer);
    return rc;
}

static int fp_send_env_js(int fd) {
    const char *client_id = getenv("FP_GOOGLE_CLIENT_ID");
    if (!client_id) {
        client_id = "";
    }
    fp_buffer resp = {0};
    if (FP_APPEND_LITERAL(&resp, "window.FP_GOOGLE_CLIENT_ID=") != 0 ||
        fp_buffer_append_json_string(&resp, client_id) != 0 ||
        FP_APPEND_LITERAL(&resp, ";\n") != 0) {
        fp_buffer_free(&resp);
        return fp_send_text(fd, 500, "Error", "Failed to build env payload");
    }
    int rc = fp_send_http(fd, 200, "OK", "application/javascript; charset=utf-8", resp.data, resp.size);
    fp_buffer_free(&resp);
    return rc;
}

static void fp_sanitize_filename(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src || !*src) {
        return;
    }
    const char *base = strrchr(src, '/');
    if (base) {
        src = base + 1;
    }
    base = strrchr(src, '\\');
    if (base) {
        src = base + 1;
    }
    size_t idx = 0;
    for (const char *p = src; *p && idx + 1 < dst_size; ++p) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '_' || c == '-' || c == '.') {
            dst[idx++] = (char)c;
        }
    }
    dst[idx] = '\0';
}

static char *fp_base64_encode(const uint8_t *data, size_t len) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t encoded_len = len ? (4 * ((len + 2) / 3)) : 0;
    if (encoded_len == 0) {
        char *empty = malloc(1);
        if (empty) {
            empty[0] = '\0';
        }
        return empty;
    }
    char *out = malloc(encoded_len + 1);
    if (!out) {
        return NULL;
    }
    size_t i = 0;
    size_t j = 0;
    while (i + 2 < len) {
        uint32_t triple = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
        out[j++] = table[(triple >> 18) & 0x3F];
        out[j++] = table[(triple >> 12) & 0x3F];
        out[j++] = table[(triple >> 6) & 0x3F];
        out[j++] = table[triple & 0x3F];
        i += 3;
    }
    size_t remaining = len - i;
    if (remaining == 1) {
        uint32_t triple = (uint32_t)data[i] << 16;
        out[j++] = table[(triple >> 18) & 0x3F];
        out[j++] = table[(triple >> 12) & 0x3F];
        out[j++] = '=';
        out[j++] = '=';
    } else if (remaining == 2) {
        uint32_t triple = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
        out[j++] = table[(triple >> 18) & 0x3F];
        out[j++] = table[(triple >> 12) & 0x3F];
        out[j++] = table[(triple >> 6) & 0x3F];
        out[j++] = '=';
    }
    out[encoded_len] = '\0';
    return out;
}

static double fp_duration_ms(const fp_result *result) {
    if (!result) {
        return 0.0;
    }
    double start = (double)result->start_ts.tv_sec * 1000.0 + (double)result->start_ts.tv_nsec / 1e6;
    double end = (double)result->end_ts.tv_sec * 1000.0 + (double)result->end_ts.tv_nsec / 1e6;
    return end - start;
}

static int fp_send_json_error(int fd, int status, const char *message) {
    fp_buffer buffer = {0};
    if (FP_APPEND_LITERAL(&buffer, "{\"status\":\"error\",\"message\":") != 0 ||
        fp_buffer_append_json_string(&buffer, message ? message : "unknown") != 0 ||
        FP_APPEND_LITERAL(&buffer, "}") != 0) {
        fp_buffer_free(&buffer);
        return fp_send_text(fd, 500, "Error", "Internal error");
    }
    int rc = fp_send_http(fd, status, "Error", "application/json", buffer.data, buffer.size);
    fp_buffer_free(&buffer);
    return rc;
}

static int fp_send_json_with_cookies(int fd, int status, const char *status_text, const char *json_body,
                                     const char **cookies, size_t cookie_count) {
    const char *body = json_body ? json_body : "{}";
    fp_buffer header_lines = {0};
    for (size_t i = 0; i < cookie_count; ++i) {
        if (cookies[i] && cookies[i][0]) {
            if (fp_buffer_appendf(&header_lines, "Set-Cookie: %s\r\n", cookies[i]) != 0) {
                fp_buffer_free(&header_lines);
                return -1;
            }
        }
    }
    int rc = fp_send_http_with_headers(fd,
                                       status,
                                       status_text ? status_text : "OK",
                                       "application/json",
                                       header_lines.data ? header_lines.data : "",
                                       body,
                                       strlen(body));
    fp_buffer_free(&header_lines);
    return rc;
}

static int fp_send_result_payload(int fd, const fp_result *result, const char *filename) {
    fp_buffer body = {0};
    if (FP_APPEND_LITERAL(&body, "{\"status\":\"ok\",\"jobId\":") != 0 ||
        fp_buffer_appendf(&body, "%llu", (unsigned long long)result->id) != 0 ||
        FP_APPEND_LITERAL(&body, ",\"message\":") != 0 ||
        fp_buffer_append_json_string(&body, result->message) != 0 ||
        FP_APPEND_LITERAL(&body, ",\"inputBytes\":") != 0 ||
        fp_buffer_appendf(&body, "%zu", result->input_size) != 0 ||
        FP_APPEND_LITERAL(&body, ",\"durationMs\":") != 0 ||
        fp_buffer_appendf(&body, "%.3f", fp_duration_ms(result)) != 0 ||
        FP_APPEND_LITERAL(&body, ",\"filename\":") != 0 ||
        fp_buffer_append_json_string(&body, filename) != 0 ||
        FP_APPEND_LITERAL(&body, ",\"results\":[") != 0) {
        fp_buffer_free(&body);
        return fp_send_json_error(fd, 500, "Failed to build payload");
    }

    for (size_t i = 0; i < result->output_count; ++i) {
        if (i > 0) {
            if (fp_buffer_append(&body, ",", 1) != 0) {
                fp_buffer_free(&body);
                return fp_send_json_error(fd, 500, "Failed to build payload");
            }
        }
        fp_encoded_image output = result->outputs[i];
        const uint8_t *raw = output.data ? output.data : (const uint8_t *)"";
        size_t raw_size = output.data ? output.size : 0;
        char *encoded = fp_base64_encode(raw, raw_size);
        if (!encoded) {
            fp_buffer_free(&body);
            return fp_send_json_error(fd, 500, "Encoding error");
        }
        if (FP_APPEND_LITERAL(&body, "{\"format\":") != 0 ||
            fp_buffer_append_json_string(&body, output.format) != 0 ||
            FP_APPEND_LITERAL(&body, ",\"label\":") != 0 ||
            fp_buffer_append_json_string(&body, output.label) != 0 ||
            FP_APPEND_LITERAL(&body, ",\"bytes\":") != 0 ||
            fp_buffer_appendf(&body, "%zu", output.size) != 0 ||
            FP_APPEND_LITERAL(&body, ",\"mime\":") != 0 ||
            fp_buffer_append_json_string(&body, output.mime) != 0 ||
            FP_APPEND_LITERAL(&body, ",\"extension\":") != 0 ||
            fp_buffer_append_json_string(&body, output.extension) != 0 ||
            FP_APPEND_LITERAL(&body, ",\"tuning\":") != 0 ||
            fp_buffer_append_json_string(&body, output.tuning) != 0 ||
            FP_APPEND_LITERAL(&body, ",\"data\":") != 0 ||
            fp_buffer_append_json_string(&body, encoded) != 0 ||
            FP_APPEND_LITERAL(&body, "}") != 0) {
            free(encoded);
            fp_buffer_free(&body);
            return fp_send_json_error(fd, 500, "Failed to build payload");
        }
        free(encoded);
    }

    if (FP_APPEND_LITERAL(&body, "]}") != 0) {
        fp_buffer_free(&body);
        return fp_send_json_error(fd, 500, "Failed to build payload");
    }

    int rc = fp_send_http(fd, 200, "OK", "application/json", body.data, body.size);
    fp_buffer_free(&body);
    return rc;
}

static int fp_append_params_used(fp_buffer *body, const fp_expert_options *opts, const fp_encoded_image *output) {
    if (!body) {
        return -1;
    }
    if (FP_APPEND_LITERAL(body, "\"params_used\":{") != 0) {
        return -1;
    }
    int wrote = 0;
    if (opts && output && output->format[0] != '\0') {
        if (strcasecmp(output->format, "png") == 0) {
            if (fp_buffer_appendf(body, "\"level\":%d", opts->png_level) != 0) {
                return -1;
            }
            wrote = 1;
        } else if (strcasecmp(output->format, "pngquant") == 0) {
            if (fp_buffer_appendf(body, "\"colors\":%d", opts->png_quant_colors) != 0) {
                return -1;
            }
            wrote = 1;
        } else if (strcasecmp(output->format, "webp") == 0) {
            if (fp_buffer_appendf(body, "\"quality\":%d", opts->webp_quality) != 0) {
                return -1;
            }
            wrote = 1;
        } else if (strcasecmp(output->format, "avif") == 0) {
            if (fp_buffer_appendf(body, "\"quality\":%d", opts->avif_quality) != 0) {
                return -1;
            }
            wrote = 1;
        }
    }
    if (opts && opts->trim_enabled) {
        if (wrote && fp_buffer_append(body, ",", 1) != 0) {
            return -1;
        }
        if (fp_buffer_appendf(body, "\"trimTolerance\":%.3f", opts->trim_tolerance) != 0) {
            return -1;
        }
        wrote = 1;
    }
    if (opts && opts->crop.enabled) {
        if (wrote && fp_buffer_append(body, ",", 1) != 0) {
            return -1;
        }
        if (fp_buffer_appendf(body,
                              "\"crop\":{\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d}",
                              opts->crop.x,
                              opts->crop.y,
                              opts->crop.width,
                              opts->crop.height) != 0) {
            return -1;
        }
        wrote = 1;
    }
    if (!wrote) {
        if (FP_APPEND_LITERAL(body, "\"default\":true") != 0) {
            return -1;
        }
    }
    return FP_APPEND_LITERAL(body, "}") == 0 ? 0 : -1;
}

static int fp_send_expert_payload(int fd,
                                  fp_result **results,
                                  char filenames[][FP_FILENAME_MAX],
                                  const fp_expert_options *opts,
                                  size_t file_count,
                                  double request_elapsed_ms) {
    if (!results || file_count == 0) {
        return fp_send_json_error(fd, 400, "No files processed");
    }
    fp_buffer body = {0};
    if (FP_APPEND_LITERAL(&body, "{\"status\":\"ok\",\"message\":\"ok\",\"files\":[") != 0) {
        fp_buffer_free(&body);
        return fp_send_json_error(fd, 500, "Failed to build payload");
    }
    size_t total_input = 0;
    size_t total_output = 0;
    for (size_t i = 0; i < file_count; ++i) {
        fp_result *res = results[i];
        if (!res) {
            continue;
        }
        if (i > 0) {
            if (fp_buffer_append(&body, ",", 1) != 0) {
                fp_buffer_free(&body);
                return fp_send_json_error(fd, 500, "Failed to build payload");
            }
        }
        double duration = fp_duration_ms(res);
        size_t best_output = res->input_size;
        total_input += res->input_size;
        if (FP_APPEND_LITERAL(&body, "{\"jobId\":") != 0 ||
            fp_buffer_appendf(&body, "%llu", (unsigned long long)res->id) != 0 ||
            FP_APPEND_LITERAL(&body, ",\"filename\":") != 0 ||
            fp_buffer_append_json_string(&body, filenames[i]) != 0 ||
            FP_APPEND_LITERAL(&body, ",\"inputBytes\":") != 0 ||
            fp_buffer_appendf(&body, "%zu", res->input_size) != 0 ||
            FP_APPEND_LITERAL(&body, ",\"durationMs\":") != 0 ||
            fp_buffer_appendf(&body, "%.3f", duration) != 0 ||
            FP_APPEND_LITERAL(&body, ",\"geometry\":{") != 0 ||
            FP_APPEND_LITERAL(&body, "\"inputWidth\":") != 0 ||
            fp_buffer_appendf(&body, "%u", res->input_width) != 0 ||
            FP_APPEND_LITERAL(&body, ",\"inputHeight\":") != 0 ||
            fp_buffer_appendf(&body, "%u", res->input_height) != 0 ||
            FP_APPEND_LITERAL(&body, ",\"outputWidth\":") != 0 ||
            fp_buffer_appendf(&body, "%u", res->output_width) != 0 ||
            FP_APPEND_LITERAL(&body, ",\"outputHeight\":") != 0 ||
            fp_buffer_appendf(&body, "%u", res->output_height) != 0 ||
            FP_APPEND_LITERAL(&body, "}") != 0 ||
            FP_APPEND_LITERAL(&body, ",\"trimApplied\":") != 0 ||
            fp_buffer_append(&body,
                             res->trim_applied ? "true" : "false",
                             res->trim_applied ? 4 : 5) != 0 ||
            FP_APPEND_LITERAL(&body, ",\"trims_applied\":") != 0 ||
            fp_buffer_append(&body,
                             res->trim_applied ? "true" : "false",
                             res->trim_applied ? 4 : 5) != 0 ||
            FP_APPEND_LITERAL(&body, ",\"cropApplied\":") != 0 ||
            fp_buffer_append(&body,
                             res->crop_applied ? "true" : "false",
                             res->crop_applied ? 4 : 5) != 0 ||
            FP_APPEND_LITERAL(&body, ",\"crops_applied\":") != 0 ||
            fp_buffer_append(&body,
                             res->crop_applied ? "true" : "false",
                             res->crop_applied ? 4 : 5) != 0 ||
            FP_APPEND_LITERAL(&body, ",\"results\":[") != 0) {
            fp_buffer_free(&body);
            return fp_send_json_error(fd, 500, "Failed to build payload");
        }

        for (size_t j = 0; j < res->output_count; ++j) {
            if (j > 0) {
                if (fp_buffer_append(&body, ",", 1) != 0) {
                    fp_buffer_free(&body);
                    return fp_send_json_error(fd, 500, "Failed to build payload");
                }
            }
            fp_encoded_image output = res->outputs[j];
            const uint8_t *raw = output.data ? output.data : (const uint8_t *)"";
            size_t raw_size = output.data ? output.size : 0;
            if (output.size < best_output) {
                best_output = output.size;
            }
            char *encoded = fp_base64_encode(raw, raw_size);
            if (!encoded) {
                fp_buffer_free(&body);
                return fp_send_json_error(fd, 500, "Encoding error");
            }
            if (FP_APPEND_LITERAL(&body, "{\"format\":") != 0 ||
                fp_buffer_append_json_string(&body, output.format) != 0 ||
                FP_APPEND_LITERAL(&body, ",\"label\":") != 0 ||
                fp_buffer_append_json_string(&body, output.label) != 0 ||
                FP_APPEND_LITERAL(&body, ",\"size_bytes\":") != 0 ||
                fp_buffer_appendf(&body, "%zu", output.size) != 0 ||
                FP_APPEND_LITERAL(&body, ",\"mime\":") != 0 ||
                fp_buffer_append_json_string(&body, output.mime) != 0 ||
                FP_APPEND_LITERAL(&body, ",\"extension\":") != 0 ||
                fp_buffer_append_json_string(&body, output.extension) != 0 ||
                FP_APPEND_LITERAL(&body, ",\"tuning\":") != 0 ||
                fp_buffer_append_json_string(&body, output.tuning) != 0 ||
                FP_APPEND_LITERAL(&body, ",\"data\":") != 0 ||
                fp_buffer_append_json_string(&body, encoded) != 0 ||
                FP_APPEND_LITERAL(&body, ",") != 0 ||
                fp_append_params_used(&body, opts ? &opts[i] : NULL, &output) != 0 ||
                FP_APPEND_LITERAL(&body, "}") != 0) {
                free(encoded);
                fp_buffer_free(&body);
                return fp_send_json_error(fd, 500, "Failed to build payload");
            }
            free(encoded);
        }

        size_t saved = res->input_size > best_output ? res->input_size - best_output : 0;
        total_output += best_output;
        if (FP_APPEND_LITERAL(&body, "],\"bytes_saved\":") != 0 ||
            fp_buffer_appendf(&body, "%zu", saved) != 0 ||
            FP_APPEND_LITERAL(&body, "}") != 0) {
            fp_buffer_free(&body);
            return fp_send_json_error(fd, 500, "Failed to build payload");
        }
    }

    size_t aggregate_saved = total_input > total_output ? total_input - total_output : 0;
    if (FP_APPEND_LITERAL(&body, "],\"bytes_saved\":") != 0 ||
        fp_buffer_appendf(&body, "%zu", aggregate_saved) != 0 ||
        FP_APPEND_LITERAL(&body, ",\"total_input_bytes\":") != 0 ||
        fp_buffer_appendf(&body, "%zu", total_input) != 0 ||
        FP_APPEND_LITERAL(&body, ",\"total_output_bytes\":") != 0 ||
        fp_buffer_appendf(&body, "%zu", total_output) != 0 ||
        FP_APPEND_LITERAL(&body, ",\"elapsed_ms\":") != 0 ||
        fp_buffer_appendf(&body, "%.3f", request_elapsed_ms) != 0 ||
        FP_APPEND_LITERAL(&body, "}") != 0) {
        fp_buffer_free(&body);
        return fp_send_json_error(fd, 500, "Failed to build payload");
    }

    int rc = fp_send_http(fd, 200, "OK", "application/json", body.data, body.size);
    fp_buffer_free(&body);
    return rc;
}

static int fp_send_sse_headers(int fd) {
    const char *headers =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";
    size_t len = strlen(headers);
    ssize_t sent = send(fd, headers, len, 0);
    return sent == (ssize_t)len ? 0 : -1;
}

static int fp_send_sse_event(int fd, const char *name, const char *payload) {
    if (!payload) {
        return 0;
    }
    if (!name || !*name) {
        name = "message";
    }
    char header[64];
    int header_len = snprintf(header, sizeof(header), "event: %s\n", name);
    if (header_len < 0) {
        return -1;
    }
    ssize_t sent = send(fd, header, (size_t)header_len, 0);
    if (sent != (ssize_t)header_len) {
        return -1;
    }
    if (send(fd, "data: ", 6, 0) != 6) {
        return -1;
    }
    size_t payload_len = strlen(payload);
    if (send(fd, payload, payload_len, 0) != (ssize_t)payload_len) {
        return -1;
    }
    if (send(fd, "\n\n", 2, 0) != 2) {
        return -1;
    }
    return 0;
}

static void fp_handle_event_stream(int fd, uint64_t job_id, fp_progress_registry *registry) {
    fp_progress_channel *channel = NULL;
    for (int attempt = 0; attempt < 200 && !channel; ++attempt) {
        channel = fp_progress_acquire(registry, job_id);
        if (channel) {
            break;
        }
        struct timespec ts = {0, 50000000}; // 50ms
        nanosleep(&ts, NULL);
    }
    if (!channel) {
        fp_send_text(fd, 404, "Not Found", "Unknown job");
        return;
    }

    if (fp_send_sse_headers(fd) != 0) {
        fp_progress_release(channel);
        return;
    }

    bool open = true;
    while (true) {
        fp_progress_event *event = fp_progress_next_event(channel, &open);
        if (!event) {
            if (!open) {
                break;
            }
            continue;
        }
        fp_send_sse_event(fd, event->event_name, event->payload);
        fp_progress_event_free(event);
        if (!open) {
            break;
        }
    }

    fp_progress_release(channel);
}

#define FP_RESULT_CACHE_MAX 16
static fp_result *g_result_cache[FP_RESULT_CACHE_MAX];
static pthread_mutex_t g_result_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

static fp_result *fp_result_cache_take(uint64_t job_id) {
    pthread_mutex_lock(&g_result_cache_mutex);
    fp_result *match = NULL;
    for (size_t i = 0; i < FP_RESULT_CACHE_MAX; ++i) {
        fp_result *candidate = g_result_cache[i];
        if (candidate && candidate->id == job_id) {
            match = candidate;
            g_result_cache[i] = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&g_result_cache_mutex);
    return match;
}

static void fp_result_cache_store(fp_result *result) {
    if (!result) {
        return;
    }
    pthread_mutex_lock(&g_result_cache_mutex);
    for (size_t i = 0; i < FP_RESULT_CACHE_MAX; ++i) {
        if (!g_result_cache[i]) {
            g_result_cache[i] = result;
            result = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&g_result_cache_mutex);
    if (result) {
        fp_free_result(result);
        free(result);
    }
}

static int fp_clamp_int_server(int value, int min_val, int max_val) {
    if (value < min_val) {
        return min_val;
    }
    if (value > max_val) {
        return max_val;
    }
    return value;
}

static void fp_populate_expert_outputs(fp_job *job, const fp_expert_options *opts) {
    if (!job || !opts) {
        return;
    }
    job->is_expert = 1;
    job->requested_output_count = 0;
    int png_level = fp_clamp_int_server(opts->png_level > 0 ? opts->png_level : 6, 1, 9);
    int png_quant = fp_clamp_int_server(opts->png_quant_colors > 0 ? opts->png_quant_colors : 128, 8, 256);
    int webp_quality = fp_clamp_int_server(opts->webp_quality > 0 ? opts->webp_quality : 90, 10, 100);
    int avif_quality = fp_clamp_int_server(opts->avif_quality, 0, 63);
    job->requested_outputs[job->requested_output_count++] = (fp_requested_output){
        .format = "png",
        .label = "lossless",
        .compression_level = png_level,
    };
    job->requested_outputs[job->requested_output_count++] = (fp_requested_output){
        .format = "pngquant",
        .label = "pngquant q80",
        .quality = png_quant,
    };
    job->requested_outputs[job->requested_output_count++] = (fp_requested_output){
        .format = "webp",
        .label = "high",
        .quality = webp_quality,
    };
    job->requested_outputs[job->requested_output_count++] = (fp_requested_output){
        .format = "avif",
        .label = "medium",
        .quality = avif_quality,
    };
    if (job->requested_output_count > FP_MAX_OUTPUTS) {
        job->requested_output_count = FP_MAX_OUTPUTS;
    }
    job->trim_options.enabled = opts->trim_enabled;
    job->trim_options.tolerance = opts->trim_tolerance;
    if (opts->crop.enabled && opts->crop.width > 0 && opts->crop.height > 0) {
        job->crop_options.enabled = 1;
        job->crop_options.x = opts->crop.x;
        job->crop_options.y = opts->crop.y;
        job->crop_options.width = opts->crop.width;
        job->crop_options.height = opts->crop.height;
    }
}

static fp_result *fp_wait_for_result(fp_queue *result_queue, uint64_t job_id) {
    fp_result *cached = fp_result_cache_take(job_id);
    if (cached) {
        return cached;
    }
    while (true) {
        fp_result *result = (fp_result *)fp_queue_pop(result_queue);
        if (result) {
            if (result->id == job_id) {
                return result;
            }
            fp_result_cache_store(result);
        }
        struct timespec ts = {0, FP_SLEEP_NS};
        nanosleep(&ts, NULL);
    }
}

static fp_result *fp_submit_job(fp_job *job,
                                const char *response_filename,
                                size_t content_length,
                                fp_queue *job_queue,
                                fp_queue *result_queue,
                                fp_progress_registry *progress_registry,
                                int *http_status,
                                char *error_buf,
                                size_t error_buf_len) {
    if (!job || !job_queue || !result_queue || !progress_registry) {
        if (http_status) {
            *http_status = 400;
        }
        if (error_buf && error_buf_len) {
            snprintf(error_buf, error_buf_len, "Invalid job");
        }
        return NULL;
    }

    fp_progress_channel *progress_channel = fp_progress_register(progress_registry, job->id);
    if (!progress_channel) {
        if (http_status) {
            *http_status = 503;
        }
        if (error_buf && error_buf_len) {
            snprintf(error_buf, error_buf_len, "Unable to track progress");
        }
        fp_free_job(job);
        free(job);
        return NULL;
    }
    fp_progress_retain(progress_channel);
    job->progress = progress_channel;

    fp_log_info("🧾 Enqueued job #%llu (%s, %zu bytes)", (unsigned long long)job->id, response_filename, job->size);

    int pushed = 0;
    for (int attempt = 0; attempt < 5000 && !pushed; ++attempt) {
        if (fp_queue_push(job_queue, job) == 0) {
            pushed = 1;
            break;
        }
        struct timespec ts = {0, FP_SLEEP_NS};
        nanosleep(&ts, NULL);
    }

    if (!pushed) {
        fp_log_warn("⏱️  Job queue full; rejecting #%llu", (unsigned long long)job->id);
        job->progress = NULL;
        fp_free_job(job);
        free(job);
        fp_progress_emit_status(progress_channel, "error", "server_busy", 0.0, content_length);
        fp_progress_close(progress_channel);
        fp_progress_release(progress_channel);
        if (http_status) {
            *http_status = 503;
        }
        if (error_buf && error_buf_len) {
            snprintf(error_buf, error_buf_len, "Server busy");
        }
        return NULL;
    }

    fp_result *result = fp_wait_for_result(result_queue, job->id);
    if (!result) {
        fp_progress_emit_status(progress_channel, "error", "no_result", 0.0, content_length);
        fp_progress_close(progress_channel);
        fp_progress_release(progress_channel);
        if (http_status) {
            *http_status = 500;
        }
        if (error_buf && error_buf_len) {
            snprintf(error_buf, error_buf_len, "No result");
        }
        return NULL;
    }

    const char *status_label = result->status == 0 ? "ok" : "error";
    fp_progress_emit_status(progress_channel, status_label, result->message, fp_duration_ms(result), result->input_size);
    fp_progress_close(progress_channel);
    fp_progress_release(progress_channel);
    return result;
}

static int fp_handle_google_auth(int fd, const fp_http_request *request, const uint8_t *body, size_t body_len,
                                 fp_auth_store *auth_store) {
    (void)request;
    if (!auth_store) {
        return fp_send_json_error(fd, 500, "Auth storage unavailable");
    }
    if (!body || body_len == 0) {
        return fp_send_json_error(fd, 400, "Missing body");
    }
    char *json = strndup((const char *)body, body_len);
    if (!json) {
        return fp_send_json_error(fd, 500, "Out of memory");
    }
    char credential[4096];
    int found = fp_extract_json_string(json, "credential", credential, sizeof(credential));
    if (found <= 0) {
        free(json);
        return fp_send_json_error(fd, 400, "Missing credential");
    }
    const char *client_id = getenv("FP_GOOGLE_CLIENT_ID");
    if (!client_id || strlen(client_id) < 8) {
        free(json);
        return fp_send_json_error(fd, 500, "Server missing FP_GOOGLE_CLIENT_ID");
    }

    char *token = credential;
    char *dot1 = strchr(token, '.');
    char *dot2 = dot1 ? strchr(dot1 + 1, '.') : NULL;
    if (!dot1 || !dot2) {
        free(json);
        return fp_send_json_error(fd, 400, "Invalid token");
    }
    *dot2 = '\0';
    *dot1 = '\0';
    uint8_t *payload = NULL;
    size_t payload_len = 0;
    if (fp_base64url_decode(dot1 + 1, &payload, &payload_len) != 0 || !payload) {
        free(json);
        return fp_send_json_error(fd, 400, "Unable to decode token");
    }
    char *payload_json = strndup((const char *)payload, payload_len);
    free(payload);
    if (!payload_json) {
        free(json);
        return fp_send_json_error(fd, 500, "Out of memory");
    }

    char aud[256] = {0};
    if (fp_extract_json_string(payload_json, "aud", aud, sizeof(aud)) <= 0) {
        free(payload_json);
        free(json);
        return fp_send_json_error(fd, 400, "Token missing aud");
    }
    if (strcmp(aud, client_id) != 0) {
        free(payload_json);
        free(json);
        return fp_send_json_error(fd, 401, "Invalid audience");
    }

    char issuer[256] = {0};
    fp_extract_json_string(payload_json, "iss", issuer, sizeof(issuer));
    if (issuer[0] != '\0' &&
        strncasecmp(issuer, "https://accounts.google.com", 28) != 0 &&
        strncasecmp(issuer, "accounts.google.com", 20) != 0) {
        free(payload_json);
        free(json);
        return fp_send_json_error(fd, 401, "Invalid issuer");
    }

    char email[256] = {0};
    char name[256] = {0};
    char picture[512] = {0};
    char sub[128] = {0};
    fp_extract_json_string(payload_json, "email", email, sizeof(email));
    fp_extract_json_string(payload_json, "name", name, sizeof(name));
    fp_extract_json_string(payload_json, "picture", picture, sizeof(picture));
    fp_extract_json_string(payload_json, "sub", sub, sizeof(sub));
    if (sub[0] == '\0') {
        free(payload_json);
        free(json);
        return fp_send_json_error(fd, 400, "Token missing subject");
    }

    fp_auth_user user = {0};
    if (fp_auth_upsert_user(auth_store, "google", sub, email, name, picture, payload_json, &user) != 0) {
        free(payload_json);
        free(json);
        return fp_send_json_error(fd, 500, "Unable to persist user");
    }

    fp_auth_tokens tokens = {0};
    if (fp_auth_issue_tokens(auth_store, &user, &tokens) != 0) {
        free(payload_json);
        free(json);
        return fp_send_json_error(fd, 500, "Unable to issue tokens");
    }

    fp_auth_record_audit(auth_store, user.id, "login_google", payload_json);

    fp_buffer resp = {0};
    if (FP_APPEND_LITERAL(&resp, "{\"status\":\"ok\",\"user\":{") != 0 ||
        FP_APPEND_LITERAL(&resp, "\"id\":") != 0 ||
        fp_buffer_appendf(&resp, "%llu", (unsigned long long)user.id) != 0 ||
        FP_APPEND_LITERAL(&resp, ",\"email\":") != 0 ||
        fp_buffer_append_json_string(&resp, user.email) != 0 ||
        FP_APPEND_LITERAL(&resp, ",\"name\":") != 0 ||
        fp_buffer_append_json_string(&resp, user.name) != 0 ||
        FP_APPEND_LITERAL(&resp, ",\"provider\":") != 0 ||
        fp_buffer_append_json_string(&resp, user.provider) != 0 ||
        FP_APPEND_LITERAL(&resp, ",\"picture\":") != 0 ||
        fp_buffer_append_json_string(&resp, user.picture) != 0 ||
        FP_APPEND_LITERAL(&resp, "},\"accessToken\":") != 0 ||
        fp_buffer_append_json_string(&resp, tokens.access_token) != 0 ||
        FP_APPEND_LITERAL(&resp, ",\"refreshToken\":") != 0 ||
        fp_buffer_append_json_string(&resp, tokens.refresh_token) != 0 ||
        FP_APPEND_LITERAL(&resp, ",\"accessExpires\":") != 0 ||
        fp_buffer_appendf(&resp, "%lld", (long long)tokens.access_expires_at) != 0 ||
        FP_APPEND_LITERAL(&resp, ",\"refreshExpires\":") != 0 ||
        fp_buffer_appendf(&resp, "%lld", (long long)tokens.refresh_expires_at) != 0 ||
        FP_APPEND_LITERAL(&resp, "}") != 0) {
        fp_buffer_free(&resp);
        free(payload_json);
        free(json);
        return fp_send_json_error(fd, 500, "Internal error");
    }

    char access_cookie[1024];
    char refresh_cookie[1024];
    snprintf(access_cookie,
             sizeof(access_cookie),
             "fp_access=%s; HttpOnly; Path=/; Max-Age=%d; SameSite=Lax; Secure",
             tokens.access_token,
             auth_store->access_ttl_seconds);
    snprintf(refresh_cookie,
             sizeof(refresh_cookie),
             "fp_refresh=%s; HttpOnly; Path=/; Max-Age=%d; SameSite=Lax; Secure",
             tokens.refresh_token,
             auth_store->refresh_ttl_seconds);
    const char *cookies[] = {access_cookie, refresh_cookie};

    int rc = fp_send_json_with_cookies(fd, 200, "OK", resp.data, cookies, sizeof(cookies) / sizeof(cookies[0]));
    fp_buffer_free(&resp);
    free(payload_json);
    free(json);
    return rc;
}

static int fp_handle_facebook_auth(int fd, const fp_http_request *request, const uint8_t *body, size_t body_len,
                                   fp_auth_store *auth_store) {
    (void)request;
    if (!auth_store) {
        return fp_send_json_error(fd, 500, "Auth storage unavailable");
    }
    if (!body || body_len == 0) {
        return fp_send_json_error(fd, 400, "Missing body");
    }
    char *json = strndup((const char *)body, body_len);
    if (!json) {
        return fp_send_json_error(fd, 500, "Out of memory");
    }

    char access_token[4096] = {0};
    char user_id[256] = {0};
    char email[256] = {0};
    char name[256] = {0};
    char picture[512] = {0};
    fp_extract_json_string(json, "accessToken", access_token, sizeof(access_token));
    fp_extract_json_string(json, "userID", user_id, sizeof(user_id));
    fp_extract_json_string(json, "userId", user_id, sizeof(user_id));
    fp_extract_json_string(json, "email", email, sizeof(email));
    fp_extract_json_string(json, "name", name, sizeof(name));
    fp_extract_json_string(json, "picture", picture, sizeof(picture));

    if (access_token[0] == '\0') {
        free(json);
        return fp_send_json_error(fd, 400, "Missing accessToken");
    }
    if (user_id[0] == '\0') {
        free(json);
        return fp_send_json_error(fd, 400, "Missing userID");
    }
    const char *app_id = getenv("FP_FACEBOOK_APP_ID");
    if (!app_id || strlen(app_id) < 3) {
        free(json);
        return fp_send_json_error(fd, 500, "Server missing FP_FACEBOOK_APP_ID");
    }
    if (!strstr(access_token, app_id)) {
        free(json);
        return fp_send_json_error(fd, 401, "Invalid audience");
    }

    fp_auth_user user = {0};
    if (fp_auth_upsert_user(auth_store, "facebook", user_id, email, name, picture, json, &user) != 0) {
        free(json);
        return fp_send_json_error(fd, 500, "Unable to persist user");
    }

    fp_auth_tokens tokens = {0};
    if (fp_auth_issue_tokens(auth_store, &user, &tokens) != 0) {
        free(json);
        return fp_send_json_error(fd, 500, "Unable to issue tokens");
    }

    fp_auth_record_audit(auth_store, user.id, "login_facebook", json);

    fp_buffer resp = {0};
    if (FP_APPEND_LITERAL(&resp, "{\"status\":\"ok\",\"user\":{") != 0 ||
        FP_APPEND_LITERAL(&resp, "\"id\":") != 0 ||
        fp_buffer_appendf(&resp, "%llu", (unsigned long long)user.id) != 0 ||
        FP_APPEND_LITERAL(&resp, ",\"email\":") != 0 ||
        fp_buffer_append_json_string(&resp, user.email) != 0 ||
        FP_APPEND_LITERAL(&resp, ",\"name\":") != 0 ||
        fp_buffer_append_json_string(&resp, user.name) != 0 ||
        FP_APPEND_LITERAL(&resp, ",\"provider\":") != 0 ||
        fp_buffer_append_json_string(&resp, user.provider) != 0 ||
        FP_APPEND_LITERAL(&resp, ",\"picture\":") != 0 ||
        fp_buffer_append_json_string(&resp, user.picture) != 0 ||
        FP_APPEND_LITERAL(&resp, "},\"accessToken\":") != 0 ||
        fp_buffer_append_json_string(&resp, tokens.access_token) != 0 ||
        FP_APPEND_LITERAL(&resp, ",\"refreshToken\":") != 0 ||
        fp_buffer_append_json_string(&resp, tokens.refresh_token) != 0 ||
        FP_APPEND_LITERAL(&resp, ",\"accessExpires\":") != 0 ||
        fp_buffer_appendf(&resp, "%lld", (long long)tokens.access_expires_at) != 0 ||
        FP_APPEND_LITERAL(&resp, ",\"refreshExpires\":") != 0 ||
        fp_buffer_appendf(&resp, "%lld", (long long)tokens.refresh_expires_at) != 0 ||
        FP_APPEND_LITERAL(&resp, "}") != 0) {
        fp_buffer_free(&resp);
        free(json);
        return fp_send_json_error(fd, 500, "Internal error");
    }

    char access_cookie[1024];
    char refresh_cookie[1024];
    snprintf(access_cookie,
             sizeof(access_cookie),
             "fp_access=%s; HttpOnly; Path=/; Max-Age=%d; SameSite=Lax; Secure",
             tokens.access_token,
             auth_store->access_ttl_seconds);
    snprintf(refresh_cookie,
             sizeof(refresh_cookie),
             "fp_refresh=%s; HttpOnly; Path=/; Max-Age=%d; SameSite=Lax; Secure",
             tokens.refresh_token,
             auth_store->refresh_ttl_seconds);
    const char *cookies[] = {access_cookie, refresh_cookie};

    int rc = fp_send_json_with_cookies(fd, 200, "OK", resp.data, cookies, sizeof(cookies) / sizeof(cookies[0]));
    fp_buffer_free(&resp);
    free(json);
    return rc;
}

static int fp_handle_api_key_issue(int fd, const fp_http_request *request, const uint8_t *body, size_t body_len,
                                   fp_auth_store *auth_store) {
    if (!auth_store) {
        return fp_send_json_error(fd, 500, "Auth storage unavailable");
    }
    fp_auth_user user = {0};
    int auth_state = fp_authenticate_request(auth_store, request, &user);
    if (auth_state <= 0) {
        return fp_send_json_error(fd, 401, "Missing or invalid access token");
    }

    char scope[64] = "expert";
    char label[128] = "";
    char *json_body = NULL;
    if (body && body_len > 0) {
        json_body = strndup((const char *)body, body_len);
        if (!json_body) {
            return fp_send_json_error(fd, 500, "Out of memory");
        }
        char tmp_scope[64] = {0};
        if (fp_extract_json_string(json_body, "scope", tmp_scope, sizeof(tmp_scope)) > 0) {
            snprintf(scope, sizeof(scope), "%s", tmp_scope);
            fp_trim_token(scope);
            if (scope[0] == '\0') {
                snprintf(scope, sizeof(scope), "expert");
            }
        }
        fp_extract_json_string(json_body, "label", label, sizeof(label));
        fp_trim_token(label);
    }

    char api_key[256];
    if (fp_auth_generate_api_key(auth_store, user.id, scope, label, api_key, sizeof(api_key)) != 0) {
        free(json_body);
        return fp_send_json_error(fd, 500, "Unable to issue API key");
    }

    fp_buffer resp = {0};
    if (FP_APPEND_LITERAL(&resp, "{\"status\":\"ok\",\"apiKey\":") != 0 ||
        fp_buffer_append_json_string(&resp, api_key) != 0 ||
        FP_APPEND_LITERAL(&resp, ",\"scope\":") != 0 ||
        fp_buffer_append_json_string(&resp, scope) != 0 ||
        FP_APPEND_LITERAL(&resp, ",\"userId\":") != 0 ||
        fp_buffer_appendf(&resp, "%llu", (unsigned long long)user.id) != 0 ||
        FP_APPEND_LITERAL(&resp, ",\"label\":") != 0 ||
        fp_buffer_append_json_string(&resp, label) != 0 ||
        FP_APPEND_LITERAL(&resp, "}") != 0) {
        fp_buffer_free(&resp);
        free(json_body);
        return fp_send_json_error(fd, 500, "Internal error");
    }

    fp_buffer audit_meta = {0};
    if (FP_APPEND_LITERAL(&audit_meta, "{\"scope\":") == 0 &&
        fp_buffer_append_json_string(&audit_meta, scope) == 0 &&
        FP_APPEND_LITERAL(&audit_meta, ",\"label\":") == 0 &&
        fp_buffer_append_json_string(&audit_meta, label) == 0 &&
        FP_APPEND_LITERAL(&audit_meta, "}") == 0) {
        fp_auth_record_audit(auth_store, user.id, "api_key_issued", audit_meta.data);
    } else {
        fp_auth_record_audit(auth_store, user.id, "api_key_issued", NULL);
    }
    fp_buffer_free(&audit_meta);

    int rc = fp_send_http(fd, 200, "OK", "application/json", resp.data, resp.size);
    fp_buffer_free(&resp);
    free(json_body);
    return rc;
}

static int fp_handle_checkout_session(int fd, const fp_http_request *request, const uint8_t *body, size_t body_len,
                                      fp_auth_store *auth_store) {
    if (!auth_store) {
        return fp_send_json_error(fd, 500, "Billing unavailable");
    }
    fp_auth_user user = {0};
    int auth_state = fp_authenticate_request(auth_store, request, &user);
    if (auth_state != 1) {
        return fp_send_json_error(fd, 401, "Authentication required for billing");
    }

    char *json_body = NULL;
    char price_requested[128] = {0};
    if (body && body_len > 0) {
        json_body = strndup((const char *)body, body_len);
        if (!json_body) {
            return fp_send_json_error(fd, 500, "Out of memory");
        }
        if (fp_extract_json_string(json_body, "priceId", price_requested, sizeof(price_requested)) <= 0) {
            fp_extract_json_string(json_body, "price", price_requested, sizeof(price_requested));
        }
        fp_trim_token(price_requested);
    }

    char price_monthly[128];
    char price_annual[128];
    fp_load_price_ids(price_monthly, sizeof(price_monthly), price_annual, sizeof(price_annual));
    char interval[16] = {0};
    const char *price_id = fp_pick_price_id(price_requested, price_monthly, price_annual, interval, sizeof(interval));
    if (interval[0] == '\0') {
        snprintf(interval, sizeof(interval), "monthly");
    }

    char session_id[96];
    char subscription_id[96];
    char customer_id[96];
    fp_generate_stub_id("cs_test_", session_id, sizeof(session_id));
    fp_generate_stub_id("sub_local_", subscription_id, sizeof(subscription_id));
    fp_generate_stub_id("cus_local_", customer_id, sizeof(customer_id));

    time_t now = time(NULL);
    time_t period_end = now + fp_period_for_price(price_id, price_monthly, price_annual);
    fp_auth_sync_subscription(auth_store, user.id, "active", customer_id, subscription_id, period_end);

    fp_buffer meta = {0};
    if (FP_APPEND_LITERAL(&meta, "{\"priceId\":") == 0 &&
        fp_buffer_append_json_string(&meta, price_id) == 0 &&
        FP_APPEND_LITERAL(&meta, ",\"sessionId\":") == 0 &&
        fp_buffer_append_json_string(&meta, session_id) == 0 &&
        FP_APPEND_LITERAL(&meta, ",\"subscriptionId\":") == 0 &&
        fp_buffer_append_json_string(&meta, subscription_id) == 0 &&
        FP_APPEND_LITERAL(&meta, ",\"customerId\":") == 0 &&
        fp_buffer_append_json_string(&meta, customer_id) == 0 &&
        FP_APPEND_LITERAL(&meta, ",\"interval\":") == 0 &&
        fp_buffer_append_json_string(&meta, interval) == 0 &&
        FP_APPEND_LITERAL(&meta, "}") == 0) {
        fp_auth_record_audit(auth_store, user.id, "checkout_session_created", meta.data);
    } else {
        fp_auth_record_audit(auth_store, user.id, "checkout_session_created", NULL);
    }
    fp_buffer_free(&meta);

    char checkout_url[256];
    const char *billing_base = getenv("FP_CHECKOUT_BASE_URL");
    if (billing_base && *billing_base) {
        snprintf(checkout_url, sizeof(checkout_url), "%s?session_id=%s", billing_base, session_id);
    } else {
        snprintf(checkout_url, sizeof(checkout_url), "https://billing.stripe.com/test/session/%s", session_id);
    }

    fp_buffer resp = {0};
    if (FP_APPEND_LITERAL(&resp, "{\"status\":\"ok\",\"checkoutUrl\":") != 0 ||
        fp_buffer_append_json_string(&resp, checkout_url) != 0 ||
        FP_APPEND_LITERAL(&resp, ",\"sessionId\":") != 0 ||
        fp_buffer_append_json_string(&resp, session_id) != 0 ||
        FP_APPEND_LITERAL(&resp, ",\"subscriptionId\":") != 0 ||
        fp_buffer_append_json_string(&resp, subscription_id) != 0 ||
        FP_APPEND_LITERAL(&resp, ",\"customerId\":") != 0 ||
        fp_buffer_append_json_string(&resp, customer_id) != 0 ||
        FP_APPEND_LITERAL(&resp, ",\"priceId\":") != 0 ||
        fp_buffer_append_json_string(&resp, price_id) != 0 ||
        FP_APPEND_LITERAL(&resp, ",\"interval\":") != 0 ||
        fp_buffer_append_json_string(&resp, interval) != 0 ||
        FP_APPEND_LITERAL(&resp, ",\"currentPeriodEnd\":") != 0 ||
        fp_buffer_appendf(&resp, "%lld", (long long)period_end) != 0 ||
        FP_APPEND_LITERAL(&resp, "}") != 0) {
        fp_buffer_free(&resp);
        if (json_body) {
            free(json_body);
        }
        return fp_send_json_error(fd, 500, "Internal error");
    }

    int rc = fp_send_http(fd, 200, "OK", "application/json", resp.data, resp.size);
    fp_buffer_free(&resp);
    if (json_body) {
        free(json_body);
    }
    return rc;
}

static int fp_handle_billing_portal(int fd, const fp_http_request *request, fp_auth_store *auth_store) {
    if (!auth_store) {
        return fp_send_json_error(fd, 500, "Billing unavailable");
    }
    fp_auth_user user = {0};
    if (fp_authenticate_request(auth_store, request, &user) != 1) {
        return fp_send_json_error(fd, 401, "Authentication required for billing");
    }
    fp_auth_subscription sub;
    if (fp_auth_get_subscription(auth_store, user.id, &sub) != 0) {
        return fp_send_json_error(fd, 404, "No subscription on file");
    }

    char portal_url[256];
    const char *portal_base = getenv("FP_PORTAL_BASE_URL");
    const char *id = sub.stripe_customer_id[0] ? sub.stripe_customer_id : sub.stripe_subscription_id;
    if (portal_base && *portal_base) {
        snprintf(portal_url, sizeof(portal_url), "%s?customer_id=%s", portal_base, id);
    } else {
        snprintf(portal_url, sizeof(portal_url), "https://billing.stripe.com/p/portal/%s", id);
    }

    fp_buffer audit_meta = {0};
    if (FP_APPEND_LITERAL(&audit_meta, "{\"customerId\":") == 0 &&
        fp_buffer_append_json_string(&audit_meta, sub.stripe_customer_id) == 0 &&
        FP_APPEND_LITERAL(&audit_meta, ",\"subscriptionId\":") == 0 &&
        fp_buffer_append_json_string(&audit_meta, sub.stripe_subscription_id) == 0 &&
        FP_APPEND_LITERAL(&audit_meta, "}") == 0) {
        fp_auth_record_audit(auth_store, user.id, "billing_portal_link", audit_meta.data);
    } else {
        fp_auth_record_audit(auth_store, user.id, "billing_portal_link", NULL);
    }
    fp_buffer_free(&audit_meta);

    fp_buffer resp = {0};
    if (FP_APPEND_LITERAL(&resp, "{\"status\":\"ok\",\"portalUrl\":") != 0 ||
        fp_buffer_append_json_string(&resp, portal_url) != 0 ||
        FP_APPEND_LITERAL(&resp, ",\"subscriptionId\":") != 0 ||
        fp_buffer_append_json_string(&resp, sub.stripe_subscription_id) != 0 ||
        FP_APPEND_LITERAL(&resp, ",\"customerId\":") != 0 ||
        fp_buffer_append_json_string(&resp, sub.stripe_customer_id) != 0 ||
        FP_APPEND_LITERAL(&resp, ",\"subscriptionStatus\":") != 0 ||
        fp_buffer_append_json_string(&resp, sub.status) != 0 ||
        FP_APPEND_LITERAL(&resp, ",\"currentPeriodEnd\":") != 0 ||
        fp_buffer_appendf(&resp, "%lld", (long long)sub.current_period_end) != 0 ||
        FP_APPEND_LITERAL(&resp, "}") != 0) {
        fp_buffer_free(&resp);
        return fp_send_json_error(fd, 500, "Internal error");
    }

    int rc = fp_send_http(fd, 200, "OK", "application/json", resp.data, resp.size);
    fp_buffer_free(&resp);
    return rc;
}

static int fp_handle_stripe_webhook(int fd, const fp_http_request *request, const uint8_t *body, size_t body_len,
                                    fp_auth_store *auth_store) {
    (void)request;
    if (!auth_store) {
        return fp_send_json_error(fd, 500, "Billing unavailable");
    }
    if (!body || body_len == 0) {
        return fp_send_json_error(fd, 400, "Missing body");
    }
    char *json = strndup((const char *)body, body_len);
    if (!json) {
        return fp_send_json_error(fd, 500, "Out of memory");
    }
    char event_type[128] = {0};
    char status[64] = {0};
    char customer[128] = {0};
    char subscription[128] = {0};
    fp_extract_json_string(json, "type", event_type, sizeof(event_type));
    fp_extract_json_string(json, "status", status, sizeof(status));
    fp_extract_json_string(json, "customer", customer, sizeof(customer));
    fp_extract_json_string(json, "subscription", subscription, sizeof(subscription));
    if (subscription[0] == '\0') {
        fp_extract_json_string(json, "subscriptionId", subscription, sizeof(subscription));
    }
    if (customer[0] == '\0') {
        fp_extract_json_string(json, "customerId", customer, sizeof(customer));
    }
    int parsed_period_end = 0;
    time_t period_end = 0;
    if (fp_json_parse_int(json, "currentPeriodEnd", &parsed_period_end) == 1 ||
        fp_json_parse_int(json, "current_period_end", &parsed_period_end) == 1) {
        period_end = (time_t)parsed_period_end;
    }
    int parsed_user_id = 0;
    uint64_t user_id = 0;
    if (fp_json_parse_int(json, "userId", &parsed_user_id) == 1 && parsed_user_id > 0) {
        user_id = (uint64_t)parsed_user_id;
    } else {
        fp_auth_find_user_by_stripe(auth_store, customer, subscription, &user_id);
    }

    char inferred_status[32] = {0};
    if (status[0] == '\0') {
        if (strstr(event_type, "deleted")) {
            snprintf(inferred_status, sizeof(inferred_status), "canceled");
        } else if (strstr(event_type, "payment_failed")) {
            snprintf(inferred_status, sizeof(inferred_status), "past_due");
        } else if (strstr(event_type, "checkout.session.completed") || strstr(event_type, "payment_succeeded")) {
            snprintf(inferred_status, sizeof(inferred_status), "active");
        }
    }
    const char *final_status = status[0] ? status : (inferred_status[0] ? inferred_status : "active");

    if (user_id == 0) {
        free(json);
        return fp_send_json_error(fd, 202, "No matching user for webhook");
    }

    fp_auth_sync_subscription(auth_store, user_id, final_status, customer, subscription, period_end);
    if (!fp_auth_has_active_subscription(auth_store, user_id)) {
        fp_auth_revoke_api_keys(auth_store, user_id, "subscription_inactive");
    }

    fp_buffer audit_meta = {0};
    if (FP_APPEND_LITERAL(&audit_meta, "{\"event\":") == 0 &&
        fp_buffer_append_json_string(&audit_meta, event_type) == 0 &&
        FP_APPEND_LITERAL(&audit_meta, ",\"status\":") == 0 &&
        fp_buffer_append_json_string(&audit_meta, final_status) == 0 &&
        FP_APPEND_LITERAL(&audit_meta, ",\"customer\":") == 0 &&
        fp_buffer_append_json_string(&audit_meta, customer) == 0 &&
        FP_APPEND_LITERAL(&audit_meta, ",\"subscription\":") == 0 &&
        fp_buffer_append_json_string(&audit_meta, subscription) == 0 &&
        FP_APPEND_LITERAL(&audit_meta, "}") == 0) {
        fp_auth_record_audit(auth_store, user_id, "stripe_webhook", audit_meta.data);
    } else {
        fp_auth_record_audit(auth_store, user_id, "stripe_webhook", NULL);
    }
    fp_buffer_free(&audit_meta);

    fp_buffer resp = {0};
    FP_APPEND_LITERAL(&resp, "{\"status\":\"ok\"}");
    int rc = fp_send_http(fd, 200, "OK", "application/json", resp.data, resp.size);
    fp_buffer_free(&resp);
    free(json);
    return rc;
}

static int fp_handle_compress(int fd, const fp_http_request *request, uint8_t *body,
                              fp_queue *job_queue, fp_queue *result_queue,
                              fp_progress_registry *progress_registry) {
    if (!request || !body) {
        free(body);
        return fp_send_json_error(fd, 400, "Invalid request");
    }

    fp_job *job = calloc(1, sizeof(fp_job));
    if (!job) {
        free(body);
        return fp_send_json_error(fd, 500, "Out of memory");
    }

    uint64_t assigned_id = request->client_job_id ? request->client_job_id : atomic_fetch_add(&g_job_counter, 1);
    if (assigned_id == 0) {
        assigned_id = atomic_fetch_add(&g_job_counter, 1);
        if (assigned_id == 0) {
            assigned_id = 1;
        }
    }
    job->id = assigned_id;
    clock_gettime(CLOCK_MONOTONIC, &job->enqueue_ts);
    job->size = request->content_length;
    job->data = body;

    fp_sanitize_filename(job->filename, sizeof(job->filename), request->filename);
    if (job->filename[0] == '\0') {
        snprintf(job->filename, sizeof(job->filename), "upload-%llu.png", (unsigned long long)job->id);
    }
    snprintf(job->tune_format, sizeof(job->tune_format), "%s", request->tune_format);
    snprintf(job->tune_label, sizeof(job->tune_label), "%s", request->tune_label);
    job->tune_direction = request->tune_direction;
    char response_filename[FP_FILENAME_MAX];
    strncpy(response_filename, job->filename, sizeof(response_filename) - 1);
    response_filename[sizeof(response_filename) - 1] = '\0';
    uint64_t job_id = job->id;

    if (job->tune_direction != 0 && job->tune_format[0] != '\0') {
        if (!fp_is_known_target(job->tune_format, job->tune_label)) {
            fp_log_warn("🚫 Unknown tune target: %s / %s", job->tune_format, job->tune_label);
            fp_free_job(job);
            free(job);
            return fp_send_json_error(fd, 400, "Unknown tune target");
        }
        const char *intent = job->tune_direction > 0 ? "smaller" : "more_detail";
        fp_log_info("🎛️  Tuning request → %s (%s)", job->tune_format, intent);
    }
    int status_code = 200;
    char error_buf[128] = {0};
    fp_result *result = fp_submit_job(job,
                                      response_filename,
                                      request->content_length,
                                      job_queue,
                                      result_queue,
                                      progress_registry,
                                      &status_code,
                                      error_buf,
                                      sizeof(error_buf));
    if (!result) {
        if (error_buf[0] == '\0') {
            strncpy(error_buf, "Compression failed", sizeof(error_buf) - 1);
        }
        return fp_send_json_error(fd, status_code, error_buf);
    }

    int rc;
    if (result->status != 0) {
        fp_log_warn("❌ Job #%llu failed: %s", (unsigned long long)job_id, result->message);
        rc = fp_send_json_error(fd, 500, result->message);
    } else {
        fp_log_info("✅ Job #%llu completed in %.2f ms", (unsigned long long)job_id, fp_duration_ms(result));
        rc = fp_send_result_payload(fd, result, response_filename);
    }

    fp_free_result(result);
    free(result);
    return rc;
}

static int fp_handle_expert_compress(int fd, const fp_http_request *request, uint8_t *body,
                                     fp_queue *job_queue, fp_queue *result_queue,
                                     fp_progress_registry *progress_registry,
                                     fp_auth_store *auth_store) {
    if (!request || !body) {
        free(body);
        return fp_send_json_error(fd, 400, "Invalid request");
    }

    struct timespec request_start;
    clock_gettime(CLOCK_MONOTONIC, &request_start);

    fp_auth_user authed_user = {0};
    char auth_source[32] = {0};
    char deny_reason[128] = {0};
    if (!fp_is_expert_authorized(request, auth_store, &authed_user, auth_source, sizeof(auth_source), deny_reason, sizeof(deny_reason))) {
        fp_log_warn("🚫 Expert auth failed for %s (%s)", request->path, deny_reason[0] ? deny_reason : "unauthorized");
        free(body);
        return fp_send_json_error(fd, 401, deny_reason[0] ? deny_reason : "Expert mode requires Authorization: ApiKey <token>");
    }

    char boundary[128];
    if (fp_parse_boundary(request->content_type, boundary, sizeof(boundary)) != 0) {
        free(body);
        return fp_send_json_error(fd, 400, "Missing multipart boundary");
    }

    fp_form_part parts[FP_EXPERT_MAX_FILES + 4];
    size_t part_count = 0;
    if (fp_parse_multipart(body, request->content_length, boundary, parts, sizeof(parts) / sizeof(parts[0]), &part_count) != 0) {
        free(body);
        return fp_send_json_error(fd, 400, "Malformed multipart body");
    }

    fp_expert_options opts;
    fp_set_default_expert_options(&opts);
    fp_expert_options file_opts[FP_EXPERT_MAX_FILES];
    int file_opts_set[FP_EXPERT_MAX_FILES] = {0};

    const fp_form_part *file_parts[FP_EXPERT_MAX_FILES];
    char filenames[FP_EXPERT_MAX_FILES][FP_FILENAME_MAX];
    size_t file_count = 0;
    size_t total_bytes = 0;
    size_t total_input_bytes = 0;
    size_t total_output_bytes = 0;
    size_t total_saved_bytes = 0;
    double request_elapsed_ms = 0.0;

    for (size_t i = 0; i < part_count; ++i) {
        fp_form_part part = parts[i];
        int metadata_index = fp_metadata_index_from_part_name(part.name);
        if (metadata_index >= 0 && (size_t)metadata_index < FP_EXPERT_MAX_FILES) {
            fp_copy_expert_options(&file_opts[metadata_index], &opts);
            fp_parse_expert_metadata(part.data, part.size, &file_opts[metadata_index]);
            file_opts_set[metadata_index] = 1;
            continue;
        }
        if (strcasecmp(part.name, "metadata") == 0) {
            fp_parse_expert_metadata(part.data, part.size, &opts);
            continue;
        }
        if (strncasecmp(part.name, "files", 5) == 0 || strncasecmp(part.name, "file", 4) == 0) {
            if (file_count >= FP_EXPERT_MAX_FILES) {
                free(body);
                return fp_send_json_error(fd, 400, "Too many files (max 10)");
            }
            if (part.size == 0) {
                free(body);
                return fp_send_json_error(fd, 400, "Empty file in upload");
            }
            if (part.size > FP_EXPERT_MAX_FILE) {
                free(body);
                return fp_send_json_error(fd, 413, "File too large for Expert mode (max 20MB)");
            }
            if (total_bytes + part.size > FP_EXPERT_MAX_TOTAL) {
                free(body);
                return fp_send_json_error(fd, 413, "Total payload too large for Expert mode (max 100MB)");
            }
            fp_sanitize_filename(filenames[file_count], sizeof(filenames[file_count]), part.filename);
            if (filenames[file_count][0] == '\0') {
                snprintf(filenames[file_count], sizeof(filenames[file_count]), "upload-%zu.png", file_count + 1);
            }
            total_bytes += part.size;
            file_parts[file_count++] = &parts[i];
        }
    }

    fp_log_info("📦 Expert request: user=%llu source=%s parts=%zu files=%zu total=%zu bytes",
                (unsigned long long)authed_user.id,
                auth_source[0] ? auth_source : "unknown",
                part_count,
                file_count,
                total_bytes);

    if (file_count == 0) {
        free(body);
        return fp_send_json_error(fd, 400, "No files provided");
    }

    for (size_t i = 0; i < file_count; ++i) {
        if (!file_opts_set[i]) {
            fp_copy_expert_options(&file_opts[i], &opts);
            file_opts_set[i] = 1;
        }
    }

    char usage_err[128] = {0};
    if (fp_track_expert_usage(authed_user.id, file_count, total_bytes, usage_err, sizeof(usage_err)) != 0) {
        free(body);
        return fp_send_json_error(fd, 429, usage_err[0] ? usage_err : "Daily limit reached");
    }

    fp_result *results[FP_EXPERT_MAX_FILES] = {0};
    char response_names[FP_EXPERT_MAX_FILES][FP_FILENAME_MAX];
    int rc = 200;
    for (size_t i = 0; i < file_count; ++i) {
        const fp_form_part *part = file_parts[i];
        fp_job *job = calloc(1, sizeof(fp_job));
        if (!job) {
            rc = fp_send_json_error(fd, 500, "Out of memory");
            goto expert_cleanup;
        }
        job->size = part->size;
        job->data = malloc(part->size);
        if (!job->data) {
            fp_free_job(job);
            free(job);
            rc = fp_send_json_error(fd, 500, "Out of memory");
            goto expert_cleanup;
        }
        memcpy(job->data, part->data, part->size);
        uint64_t assigned_id = atomic_fetch_add(&g_job_counter, 1);
        if (assigned_id == 0) {
            assigned_id = atomic_fetch_add(&g_job_counter, 1);
            if (assigned_id == 0) {
                assigned_id = 1;
            }
        }
        job->id = assigned_id;
        clock_gettime(CLOCK_MONOTONIC, &job->enqueue_ts);
        fp_sanitize_filename(job->filename, sizeof(job->filename), filenames[i]);
        snprintf(response_names[i], sizeof(response_names[i]), "%s", job->filename);

        fp_populate_expert_outputs(job, &file_opts[i]);

        int status_code = 200;
        char error_buf[128] = {0};
        fp_result *result = fp_submit_job(job,
                                          response_names[i],
                                          part->size,
                                          job_queue,
                                          result_queue,
                                          progress_registry,
                                          &status_code,
                                          error_buf,
                                          sizeof(error_buf));
        if (!result) {
            rc = fp_send_json_error(fd, status_code, error_buf[0] ? error_buf : "Compression failed");
            goto expert_cleanup;
        }
        if (result->status != 0) {
            fp_log_warn("❌ Expert job #%llu failed: %s", (unsigned long long)result->id, result->message);
            rc = fp_send_json_error(fd, 500, result->message);
            fp_free_result(result);
            free(result);
            goto expert_cleanup;
        }
        results[i] = result;
    }

    struct timespec request_end;
    clock_gettime(CLOCK_MONOTONIC, &request_end);
    request_elapsed_ms = fp_elapsed_ms(&request_start, &request_end);

    total_input_bytes = 0;
    total_output_bytes = 0;
    for (size_t i = 0; i < file_count; ++i) {
        fp_result *res = results[i];
        if (!res) {
            continue;
        }
        size_t best_output = res->input_size;
        total_input_bytes += res->input_size;
        for (size_t j = 0; j < res->output_count; ++j) {
            if (res->outputs[j].size < best_output) {
                best_output = res->outputs[j].size;
            }
        }
        total_output_bytes += best_output;
    }
    total_saved_bytes = total_input_bytes > total_output_bytes ? total_input_bytes - total_output_bytes : 0;

    rc = fp_send_expert_payload(fd, results, response_names, file_opts, file_count, request_elapsed_ms);

expert_cleanup:
    for (size_t i = 0; i < file_count; ++i) {
        if (results[i]) {
            fp_free_result(results[i]);
            free(results[i]);
            results[i] = NULL;
        }
    }
    free(body);
    atomic_fetch_add(&g_expert_request_count, 1);
    atomic_fetch_add(&g_expert_request_files, file_count);
    atomic_fetch_add(&g_expert_request_bytes, total_input_bytes);
    fp_log_info("📊 Expert usage user=%llu files=%zu in=%zu out=%zu saved=%zu elapsed=%.2fms",
                (unsigned long long)authed_user.id,
                file_count,
                total_input_bytes,
                total_output_bytes,
                total_saved_bytes,
                request_elapsed_ms);
    if (auth_store && authed_user.id) {
        fp_buffer audit = {0};
        if (FP_APPEND_LITERAL(&audit, "{") == 0 &&
            fp_buffer_appendf(&audit, "\"files\":%zu,\"bytes_in\":%zu,\"bytes_out\":%zu,\"saved\":%zu,\"elapsed_ms\":%.3f",
                              file_count,
                              total_input_bytes,
                              total_output_bytes,
                              total_saved_bytes,
                              request_elapsed_ms) == 0) {
            FP_APPEND_LITERAL(&audit, "}");
        }
        fp_auth_record_audit(auth_store, authed_user.id, "expert_request", audit.data);
        fp_buffer_free(&audit);
    }
    return rc;
}

static void fp_handle_client(int client_fd, fp_queue *job_queue, fp_queue *result_queue,
                             fp_progress_registry *progress_registry, fp_auth_store *auth_store) {
    char *header_buffer = NULL;
    size_t header_len = 0;
    size_t total_len = 0;
    if (fp_read_header_block(client_fd, &header_buffer, &header_len, &total_len) != 0) {
        fp_send_text(client_fd, 400, "Bad Request", "Malformed request");
        return;
    }

    char *header_copy = malloc(header_len + 1);
    if (!header_copy) {
        free(header_buffer);
        fp_send_text(client_fd, 500, "Error", "Out of memory");
        return;
    }
    memcpy(header_copy, header_buffer, header_len);
    header_copy[header_len] = '\0';

    fp_http_request request;
    if (fp_parse_request(header_copy, &request) != 0) {
        fp_log_warn("📵 Unable to parse request");
        free(header_copy);
        free(header_buffer);
        fp_send_text(client_fd, 400, "Bad Request", "Unable to parse request");
        return;
    }

    size_t body_in_buffer = total_len > header_len ? total_len - header_len : 0;
    uint8_t *body = NULL;
    if (request.content_length > 0) {
        if (request.content_length > FP_MAX_UPLOAD) {
            free(header_copy);
            free(header_buffer);
            fp_send_json_error(client_fd, 413, "File too large (max 100 MB)");
            return;
        }
        body = malloc(request.content_length);
        if (!body) {
            free(header_copy);
            free(header_buffer);
            fp_send_text(client_fd, 500, "Error", "Out of memory");
            return;
        }
        size_t copy_len = body_in_buffer;
        if (copy_len > request.content_length) {
            copy_len = request.content_length;
        }
        memcpy(body, header_buffer + header_len, copy_len);
        size_t offset = copy_len;
        while (offset < request.content_length) {
            ssize_t received = recv(client_fd, body + offset, request.content_length - offset, 0);
            if (received <= 0) {
                free(body);
                free(header_copy);
                free(header_buffer);
                fp_send_text(client_fd, 400, "Bad Request", "Unexpected EOF");
                return;
            }
            offset += (size_t)received;
        }
    }

    free(header_copy);
    free(header_buffer);

    fp_log_info("📨 %s %s (%zu bytes)", request.method, request.path, request.content_length);

    if (strcmp(request.method, "GET") == 0) {
        uint64_t stream_job_id = 0;
        if (fp_parse_stream_path(request.path, &stream_job_id)) {
            fp_log_info("📡 Streaming progress for job #%llu", (unsigned long long)stream_job_id);
            fp_handle_event_stream(client_fd, stream_job_id, progress_registry);
            free(body);
            return;
        }
        if (strcmp(request.path, "/env.js") == 0) {
            fp_send_env_js(client_fd);
            free(body);
            return;
        }
        fp_send_static_file(client_fd, request.path);
        free(body);
        return;
    }

    if (strcmp(request.method, "POST") == 0 && strcmp(request.path, "/api/compress") == 0) {
        if (!body || request.content_length == 0) {
            fp_log_warn("🚫 POST /api/compress missing body");
            free(body);
            fp_send_json_error(client_fd, 400, "Missing body");
            return;
        }
        fp_handle_compress(client_fd, &request, body, job_queue, result_queue, progress_registry);
        return;
    }

    if (strcmp(request.method, "POST") == 0 && strcmp(request.path, "/api/expert/compress") == 0) {
        if (!body || request.content_length == 0) {
            fp_log_warn("🚫 POST /api/expert/compress missing body");
            free(body);
            fp_send_json_error(client_fd, 400, "Missing body");
            return;
        }
        fp_handle_expert_compress(client_fd, &request, body, job_queue, result_queue, progress_registry, auth_store);
        return;
    }

    if (strcmp(request.method, "POST") == 0 && strcmp(request.path, "/api/stripe/checkout") == 0) {
        int rc = fp_handle_checkout_session(client_fd, &request, body, request.content_length, auth_store);
        free(body);
        (void)rc;
        return;
    }

    if (strcmp(request.method, "POST") == 0 && strcmp(request.path, "/api/stripe/portal") == 0) {
        int rc = fp_handle_billing_portal(client_fd, &request, auth_store);
        free(body);
        (void)rc;
        return;
    }

    if (strcmp(request.method, "POST") == 0 && strcmp(request.path, "/webhook/stripe") == 0) {
        int rc = fp_handle_stripe_webhook(client_fd, &request, body, request.content_length, auth_store);
        free(body);
        (void)rc;
        return;
    }

    if (strcmp(request.method, "POST") == 0 && strcmp(request.path, "/auth/google") == 0) {
        int rc = fp_handle_google_auth(client_fd, &request, body, request.content_length, auth_store);
        free(body);
        (void)rc;
        return;
    }

    if (strcmp(request.method, "POST") == 0 && strcmp(request.path, "/auth/facebook") == 0) {
        int rc = fp_handle_facebook_auth(client_fd, &request, body, request.content_length, auth_store);
        free(body);
        (void)rc;
        return;
    }

    if (strcmp(request.method, "POST") == 0 && strcmp(request.path, "/api/keys") == 0) {
        if (!body) {
            fp_send_json_error(client_fd, 400, "Missing body");
            return;
        }
        int rc = fp_handle_api_key_issue(client_fd, &request, body, request.content_length, auth_store);
        free(body);
        (void)rc;
        return;
    }

    free(body);
    fp_send_text(client_fd, 404, "Not Found", "Not Found");
}

typedef struct {
    int fd;
    fp_queue *job_queue;
    fp_queue *result_queue;
    fp_progress_registry *progress_registry;
    fp_auth_store *auth_store;
} fp_client_context;

static void *fp_client_thread(void *arg) {
    fp_client_context *ctx = (fp_client_context *)arg;
    if (!ctx) {
        return NULL;
    }
    fp_handle_client(ctx->fd, ctx->job_queue, ctx->result_queue, ctx->progress_registry, ctx->auth_store);
    close(ctx->fd);
    free(ctx);
    return NULL;
}

int fp_server_run(const char *host, int port, size_t worker_count, fp_queue *job_queue,
                  fp_queue *result_queue, fp_progress_registry *progress_registry,
                  fp_auth_store *auth_store) {
    (void)worker_count;
    if (!job_queue || !result_queue || !progress_registry || !auth_store) {
        return -1;
    }

#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    int listen_fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (!host || strcmp(host, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid host %s\n", host);
        close(listen_fd);
        return -1;
    }

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, 64) != 0) {
        perror("listen");
        close(listen_fd);
        return -1;
    }

    const char *listen_host = host && *host ? host : "0.0.0.0";
    fp_log_info("🚀 ferretptimize listening on %s:%d", listen_host, port);
    if (strcmp(listen_host, "0.0.0.0") == 0) {
        fp_log_info("🌐 Open http://127.0.0.1:%d/ or http://wsl.localhost:%d/", port, port);
    } else {
        fp_log_info("🌐 Open http://%s:%d/ in your browser", listen_host, port);
    }

    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            fp_log_error("💥 accept failed: %s", strerror(errno));
            break;
        }
        fp_client_context *ctx = calloc(1, sizeof(fp_client_context));
        if (!ctx) {
            fp_log_error("🔥 Out of memory for client context");
            close(client_fd);
            continue;
        }
        ctx->fd = client_fd;
        ctx->job_queue = job_queue;
        ctx->result_queue = result_queue;
        ctx->progress_registry = progress_registry;
        ctx->auth_store = auth_store;

        pthread_t thread;
        if (pthread_create(&thread, NULL, fp_client_thread, ctx) != 0) {
            fp_log_warn("⚠️  Failed to spawn client thread; handling inline");
            fp_handle_client(client_fd, job_queue, result_queue, progress_registry, auth_store);
            close(client_fd);
            free(ctx);
        } else {
            pthread_detach(thread);
        }
    }

    close(listen_fd);
    return 0;
}
