#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "server.h"
#include "ferret.h"
#include "log.h"
#include "progress.h"

#define FP_MAX_HEADER (64 * 1024)
#define FP_MAX_UPLOAD (32 * 1024 * 1024)
#define FP_MIN_BUFFER 4096
#define FP_SLEEP_NS 2000000

static const char *FP_PUBLIC_ROOT = "public";
static _Atomic uint64_t g_job_counter = 1;

typedef struct {
    char method[8];
    char path[256];
    char content_type[128];
    char filename[FP_FILENAME_MAX];
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
        } else if (strcmp(name, "x-job-id") == 0) {
            request->client_job_id = strtoull(value, NULL, 10);
        }
    }
    return 0;
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
        ssize_t wrote = send(fd, bytes + sent, len - sent, 0);
        if (wrote <= 0) {
            return -1;
        }
        sent += (size_t)wrote;
    }
    return 0;
}

static int fp_send_http(int fd, int status, const char *status_text, const char *content_type, const void *body, size_t body_len) {
    if (!status_text) {
        status_text = "OK";
    }
    if (!content_type) {
        content_type = "text/plain; charset=utf-8";
    }
    char header[512];
    int header_len = snprintf(header,
                              sizeof(header),
                              "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
                              status,
                              status_text,
                              content_type,
                              body_len);
    if (header_len < 0 || (size_t)header_len >= sizeof(header)) {
        return -1;
    }
    if (fp_send_buffer(fd, header, (size_t)header_len) != 0) {
        return -1;
    }
    if (body && body_len > 0) {
        if (fp_send_buffer(fd, body, body_len) != 0) {
            return -1;
        }
    }
    return 0;
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

static fp_result *fp_wait_for_result(fp_queue *result_queue, uint64_t job_id) {
    while (true) {
        fp_result *result = (fp_result *)fp_queue_pop(result_queue);
        if (result) {
            if (result->id == job_id) {
                return result;
            }
            fp_free_result(result);
            free(result);
        }
        struct timespec ts = {0, FP_SLEEP_NS};
        nanosleep(&ts, NULL);
    }
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
    char response_filename[FP_FILENAME_MAX];
    strncpy(response_filename, job->filename, sizeof(response_filename) - 1);
    response_filename[sizeof(response_filename) - 1] = '\0';
    uint64_t job_id = job->id;

    fp_progress_channel *progress_channel = fp_progress_register(progress_registry, job_id);
    if (!progress_channel) {
        fp_free_job(job);
        free(job);
        return fp_send_json_error(fd, 503, "Unable to track progress");
    }
    fp_progress_retain(progress_channel);
    job->progress = progress_channel;

    fp_log_info("🧾 Enqueued job #%llu (%s, %zu bytes)", (unsigned long long)job_id, response_filename, job->size);

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
        fp_log_warn("⏱️  Job queue full; rejecting #%llu", (unsigned long long)job_id);
        job->progress = NULL;
        fp_free_job(job);
        free(job);
        fp_progress_emit_status(progress_channel, "error", "server_busy", 0.0, request->content_length);
        fp_progress_close(progress_channel);
        fp_progress_release(progress_channel);
        return fp_send_json_error(fd, 503, "Server busy");
    }

    fp_result *result = fp_wait_for_result(result_queue, job_id);
    if (!result) {
        fp_progress_emit_status(progress_channel, "error", "no_result", 0.0, request->content_length);
        fp_progress_close(progress_channel);
        fp_progress_release(progress_channel);
        return fp_send_json_error(fd, 500, "No result");
    }

    int rc;
    if (result->status != 0) {
        fp_log_warn("❌ Job #%llu failed: %s", (unsigned long long)job_id, result->message);
        rc = fp_send_json_error(fd, 500, result->message);
    } else {
        fp_log_info("✅ Job #%llu completed in %.2f ms", (unsigned long long)job_id, fp_duration_ms(result));
        rc = fp_send_result_payload(fd, result, response_filename);
    }

    const char *status_label = result->status == 0 ? "ok" : "error";
    fp_progress_emit_status(progress_channel, status_label, result->message, fp_duration_ms(result), result->input_size);
    fp_progress_close(progress_channel);
    fp_progress_release(progress_channel);

    fp_free_result(result);
    free(result);
    return rc;
}

static void fp_handle_client(int client_fd, fp_queue *job_queue, fp_queue *result_queue,
                             fp_progress_registry *progress_registry) {
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
            fp_send_json_error(client_fd, 413, "File too large");
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

    free(body);
    fp_send_text(client_fd, 404, "Not Found", "Not Found");
}

typedef struct {
    int fd;
    fp_queue *job_queue;
    fp_queue *result_queue;
    fp_progress_registry *progress_registry;
} fp_client_context;

static void *fp_client_thread(void *arg) {
    fp_client_context *ctx = (fp_client_context *)arg;
    if (!ctx) {
        return NULL;
    }
    fp_handle_client(ctx->fd, ctx->job_queue, ctx->result_queue, ctx->progress_registry);
    close(ctx->fd);
    free(ctx);
    return NULL;
}

int fp_server_run(const char *host, int port, size_t worker_count, fp_queue *job_queue,
                  fp_queue *result_queue, fp_progress_registry *progress_registry) {
    (void)worker_count;
    if (!job_queue || !result_queue || !progress_registry) {
        return -1;
    }

    signal(SIGPIPE, SIG_IGN);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

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

        pthread_t thread;
        if (pthread_create(&thread, NULL, fp_client_thread, ctx) != 0) {
            fp_log_warn("⚠️  Failed to spawn client thread; handling inline");
            fp_handle_client(client_fd, job_queue, result_queue, progress_registry);
            close(client_fd);
            free(ctx);
        } else {
            pthread_detach(thread);
        }
    }

    close(listen_fd);
    return 0;
}
