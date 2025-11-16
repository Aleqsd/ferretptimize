#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

#include "progress.h"

typedef struct fp_progress_entry {
    uint64_t job_id;
    fp_progress_channel *channel;
} fp_progress_entry;

struct fp_progress_registry {
    pthread_mutex_t mutex;
    fp_progress_entry *entries;
    size_t capacity;
};

struct fp_progress_channel {
    uint64_t job_id;
    fp_progress_registry *registry;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    fp_progress_event *head;
    fp_progress_event *tail;
    int ref_count;
    bool closed;
};

static fp_progress_event *fp_progress_event_create(fp_progress_event_type type, const char *event_name, char *payload) {
    fp_progress_event *event = calloc(1, sizeof(fp_progress_event));
    if (!event) {
        free(payload);
        return NULL;
    }
    event->type = type;
    if (event_name && *event_name) {
        strncpy(event->event_name, event_name, sizeof(event->event_name) - 1);
    } else if (type == FP_PROGRESS_EVENT_OUTPUT) {
        strncpy(event->event_name, "result", sizeof(event->event_name) - 1);
    } else {
        strncpy(event->event_name, "status", sizeof(event->event_name) - 1);
    }
    event->payload = payload;
    event->next = NULL;
    return event;
}

static void fp_progress_event_destroy(fp_progress_event *event) {
    if (!event) {
        return;
    }
    free(event->payload);
    free(event);
}

static size_t fp_progress_registry_index(const fp_progress_registry *registry, uint64_t job_id) {
    if (registry->capacity == 0) {
        return 0;
    }
    return (size_t)(job_id % registry->capacity);
}

fp_progress_registry *fp_progress_registry_create(size_t capacity) {
    if (capacity == 0) {
        capacity = 64;
    }
    fp_progress_registry *registry = calloc(1, sizeof(fp_progress_registry));
    if (!registry) {
        return NULL;
    }
    registry->entries = calloc(capacity, sizeof(fp_progress_entry));
    if (!registry->entries) {
        free(registry);
        return NULL;
    }
    registry->capacity = capacity;
    pthread_mutex_init(&registry->mutex, NULL);
    return registry;
}

static void fp_progress_registry_forget(fp_progress_registry *registry, uint64_t job_id) {
    if (!registry) {
        return;
    }
    pthread_mutex_lock(&registry->mutex);
    for (size_t i = 0; i < registry->capacity; ++i) {
        fp_progress_entry *entry = &registry->entries[i];
        if (entry->channel && entry->job_id == job_id) {
            entry->job_id = 0;
            entry->channel = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&registry->mutex);
}

void fp_progress_registry_destroy(fp_progress_registry *registry) {
    if (!registry) {
        return;
    }
    pthread_mutex_lock(&registry->mutex);
    for (size_t i = 0; i < registry->capacity; ++i) {
        if (registry->entries[i].channel) {
            fp_progress_channel *channel = registry->entries[i].channel;
            registry->entries[i].channel = NULL;
            registry->entries[i].job_id = 0;
            pthread_mutex_unlock(&registry->mutex);
            fp_progress_close(channel);
            fp_progress_release(channel);
            pthread_mutex_lock(&registry->mutex);
        }
    }
    pthread_mutex_unlock(&registry->mutex);
    pthread_mutex_destroy(&registry->mutex);
    free(registry->entries);
    free(registry);
}

static fp_progress_channel *fp_progress_channel_create(fp_progress_registry *registry, uint64_t job_id) {
    fp_progress_channel *channel = calloc(1, sizeof(fp_progress_channel));
    if (!channel) {
        return NULL;
    }
    channel->job_id = job_id;
    channel->registry = registry;
    pthread_mutex_init(&channel->mutex, NULL);
    pthread_cond_init(&channel->cond, NULL);
    channel->ref_count = 1;
    channel->closed = false;
    channel->head = NULL;
    channel->tail = NULL;
    return channel;
}

static void fp_progress_channel_destroy(fp_progress_channel *channel) {
    if (!channel) {
        return;
    }
    fp_progress_event *node = channel->head;
    while (node) {
        fp_progress_event *next = node->next;
        fp_progress_event_destroy(node);
        node = next;
    }
    pthread_mutex_destroy(&channel->mutex);
    pthread_cond_destroy(&channel->cond);
    free(channel);
}

fp_progress_channel *fp_progress_register(fp_progress_registry *registry, uint64_t job_id) {
    if (!registry || job_id == 0) {
        return NULL;
    }
    pthread_mutex_lock(&registry->mutex);
    fp_progress_channel *channel = NULL;
    size_t start = fp_progress_registry_index(registry, job_id);
    for (size_t i = 0; i < registry->capacity; ++i) {
        size_t idx = (start + i) % registry->capacity;
        fp_progress_entry *entry = &registry->entries[idx];
        if (!entry->channel) {
            channel = fp_progress_channel_create(registry, job_id);
            if (channel) {
                entry->job_id = job_id;
                entry->channel = channel;
            }
            break;
        }
    }
    pthread_mutex_unlock(&registry->mutex);
    return channel;
}

static fp_progress_channel *fp_progress_registry_find(fp_progress_registry *registry, uint64_t job_id) {
    if (!registry || job_id == 0) {
        return NULL;
    }
    fp_progress_channel *channel = NULL;
    pthread_mutex_lock(&registry->mutex);
    size_t start = fp_progress_registry_index(registry, job_id);
    for (size_t i = 0; i < registry->capacity; ++i) {
        size_t idx = (start + i) % registry->capacity;
        fp_progress_entry *entry = &registry->entries[idx];
        if (!entry->channel) {
            continue;
        }
        if (entry->job_id == job_id) {
            channel = entry->channel;
            break;
        }
    }
    pthread_mutex_unlock(&registry->mutex);
    return channel;
}

fp_progress_channel *fp_progress_acquire(fp_progress_registry *registry, uint64_t job_id) {
    fp_progress_channel *channel = fp_progress_registry_find(registry, job_id);
    if (!channel) {
        return NULL;
    }
    pthread_mutex_lock(&channel->mutex);
    channel->ref_count++;
    pthread_mutex_unlock(&channel->mutex);
    return channel;
}

void fp_progress_retain(fp_progress_channel *channel) {
    if (!channel) {
        return;
    }
    pthread_mutex_lock(&channel->mutex);
    channel->ref_count++;
    pthread_mutex_unlock(&channel->mutex);
}

void fp_progress_release(fp_progress_channel *channel) {
    if (!channel) {
        return;
    }
    bool destroy = false;
    pthread_mutex_lock(&channel->mutex);
    if (--channel->ref_count == 0) {
        destroy = true;
    }
    pthread_mutex_unlock(&channel->mutex);
    if (destroy) {
        fp_progress_registry_forget(channel->registry, channel->job_id);
        fp_progress_channel_destroy(channel);
    }
}

static void fp_progress_channel_push(fp_progress_channel *channel, fp_progress_event *event) {
    if (!channel || !event) {
        fp_progress_event_destroy(event);
        return;
    }
    pthread_mutex_lock(&channel->mutex);
    if (channel->closed) {
        pthread_mutex_unlock(&channel->mutex);
        fp_progress_event_destroy(event);
        return;
    }
    if (channel->tail) {
        channel->tail->next = event;
    } else {
        channel->head = event;
    }
    channel->tail = event;
    pthread_cond_broadcast(&channel->cond);
    pthread_mutex_unlock(&channel->mutex);
}

fp_progress_event *fp_progress_next_event(fp_progress_channel *channel, bool *is_open) {
    if (!channel) {
        if (is_open) {
            *is_open = false;
        }
        return NULL;
    }
    pthread_mutex_lock(&channel->mutex);
    while (!channel->head && !channel->closed) {
        pthread_cond_wait(&channel->cond, &channel->mutex);
    }
    fp_progress_event *event = channel->head;
    if (event) {
        channel->head = event->next;
        if (!channel->head) {
            channel->tail = NULL;
        }
    }
    if (is_open) {
        *is_open = !channel->closed || channel->head != NULL;
    }
    pthread_mutex_unlock(&channel->mutex);
    return event;
}

void fp_progress_event_free(fp_progress_event *event) {
    fp_progress_event_destroy(event);
}

void fp_progress_close(fp_progress_channel *channel) {
    if (!channel) {
        return;
    }
    pthread_mutex_lock(&channel->mutex);
    channel->closed = true;
    pthread_cond_broadcast(&channel->cond);
    pthread_mutex_unlock(&channel->mutex);
}

static char *fp_progress_json_escape(const char *value) {
    if (!value) {
        value = "";
    }
    size_t len = 0;
    for (const unsigned char *ptr = (const unsigned char *)value; *ptr; ++ptr) {
        unsigned char c = *ptr;
        if (c == '"' || c == '\\' || c < 0x20) {
            len += 2;
        } else {
            len += 1;
        }
    }
    char *out = malloc(len + 3);
    if (!out) {
        return NULL;
    }
    char *dst = out;
    *dst++ = '"';
    for (const unsigned char *ptr = (const unsigned char *)value; *ptr; ++ptr) {
        unsigned char c = *ptr;
        switch (c) {
            case '"':
            case '\\':
                *dst++ = '\\';
                *dst++ = (char)c;
                break;
            case '\n':
                *dst++ = '\\';
                *dst++ = 'n';
                break;
            case '\r':
                *dst++ = '\\';
                *dst++ = 'r';
                break;
            case '\t':
                *dst++ = '\\';
                *dst++ = 't';
                break;
            default:
                if (c < 0x20) {
                    static const char hex[] = "0123456789abcdef";
                    *dst++ = '\\';
                    *dst++ = 'u';
                    *dst++ = '0';
                    *dst++ = '0';
                    *dst++ = hex[(c >> 4) & 0xF];
                    *dst++ = hex[c & 0xF];
                } else {
                    *dst++ = (char)c;
                }
                break;
        }
    }
    *dst++ = '"';
    *dst = '\0';
    return out;
}

static char *fp_progress_base64_encode(const uint8_t *data, size_t len) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t encoded_len = len ? (4 * ((len + 2) / 3)) : 0;
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

static int fp_progress_emit(fp_progress_channel *channel,
                            fp_progress_event_type type,
                            const char *event_name,
                            char *payload) {
    fp_progress_event *event = fp_progress_event_create(type, event_name, payload);
    if (!event) {
        return -1;
    }
    fp_progress_channel_push(channel, event);
    return 0;
}

int fp_progress_emit_output(fp_progress_channel *channel,
                            const fp_encoded_image *output,
                            size_t input_size,
                            double duration_ms,
                            double avg_duration_ms) {
    if (!channel || !output || !output->data) {
        return -1;
    }
    char *format = fp_progress_json_escape(output->format);
    char *label = fp_progress_json_escape(output->label);
    char *mime = fp_progress_json_escape(output->mime);
    char *extension = fp_progress_json_escape(output->extension);
    char *data = fp_progress_base64_encode(output->data, output->size);
    if (!format || !label || !mime || !extension || !data) {
        free(format);
        free(label);
        free(mime);
        free(extension);
        free(data);
        return -1;
    }

    const char *suffix_template = "\",\"inputBytes\":%zu,\"durationMs\":%.3f,\"avgDurationMs\":%.3f}";
    int prefix_len = snprintf(NULL,
                              0,
                              "{\"jobId\":%llu,\"type\":\"result\",\"format\":%s,\"label\":%s,"
                              "\"bytes\":%zu,\"mime\":%s,\"extension\":%s,\"data\":\"",
                              (unsigned long long)channel->job_id,
                              format,
                              label,
                              output->size,
                              mime,
                              extension);
    if (prefix_len < 0) {
        free(format);
        free(label);
        free(mime);
        free(extension);
        free(data);
        return -1;
    }
    int suffix_len = snprintf(NULL, 0, suffix_template, input_size, duration_ms, avg_duration_ms);
    size_t total_len = (size_t)prefix_len + strlen(data) + (size_t)suffix_len;
    char *payload = malloc(total_len + 1);
    if (!payload) {
        free(format);
        free(label);
        free(mime);
        free(extension);
        free(data);
        return -1;
    }
    int written = snprintf(payload,
                           total_len + 1,
                           "{\"jobId\":%llu,\"type\":\"result\",\"format\":%s,\"label\":%s,"
                           "\"bytes\":%zu,\"mime\":%s,\"extension\":%s,\"data\":\"",
                           (unsigned long long)channel->job_id,
                           format,
                           label,
                           output->size,
                           mime,
                           extension);
    if (written < 0) {
        free(payload);
        free(format);
        free(label);
        free(mime);
        free(extension);
        free(data);
        return -1;
    }
    size_t offset = (size_t)written;
    strcpy(payload + offset, data);
    offset += strlen(data);
    snprintf(payload + offset, (size_t)suffix_len + 1, suffix_template, input_size, duration_ms, avg_duration_ms);

    free(format);
    free(label);
    free(mime);
    free(extension);
    free(data);

    return fp_progress_emit(channel, FP_PROGRESS_EVENT_OUTPUT, "result", payload);
}

int fp_progress_emit_status(fp_progress_channel *channel, const char *status, const char *message, double duration_ms, size_t input_size) {
    if (!channel) {
        return -1;
    }
    char *status_json = fp_progress_json_escape(status);
    char *message_json = fp_progress_json_escape(message);
    if (!status_json || !message_json) {
        free(status_json);
        free(message_json);
        return -1;
    }
    char payload[256];
    int written = snprintf(payload, sizeof(payload),
                           "{\"jobId\":%llu,\"type\":\"status\",\"status\":%s,\"message\":%s,"
                           "\"durationMs\":%.3f,\"inputBytes\":%zu}",
                           (unsigned long long)channel->job_id,
                           status_json,
                           message_json,
                           duration_ms,
                           input_size);
    free(status_json);
    free(message_json);
    if (written < 0) {
        return -1;
    }
    char *copy = strdup(payload);
    if (!copy) {
        return -1;
    }
    return fp_progress_emit(channel, FP_PROGRESS_EVENT_STATUS, "status", copy);
}
