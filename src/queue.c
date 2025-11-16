#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include "queue.h"

fp_queue *fp_queue_create(size_t capacity) {
    if (capacity == 0) {
        capacity = 1;
    }

    fp_queue *queue = calloc(1, sizeof(fp_queue));
    if (!queue) {
        return NULL;
    }

    queue->capacity = capacity;
    queue->slots = calloc(capacity, sizeof(fp_queue_slot));
    if (!queue->slots) {
        free(queue);
        return NULL;
    }

    for (size_t i = 0; i < capacity; ++i) {
        atomic_init(&queue->slots[i].seq, i);
    }

    atomic_init(&queue->head, 0);
    atomic_init(&queue->tail, 0);
    return queue;
}

void fp_queue_destroy(fp_queue *queue) {
    if (!queue) {
        return;
    }
    free(queue->slots);
    free(queue);
}

int fp_queue_push(fp_queue *queue, void *data) {
    if (!queue) {
        errno = EINVAL;
        return -1;
    }

    fp_queue_slot *slot = NULL;
    size_t pos = atomic_load_explicit(&queue->tail, memory_order_relaxed);

    for (;;) {
        slot = &queue->slots[pos % queue->capacity];
        size_t seq = atomic_load_explicit(&slot->seq, memory_order_acquire);
        intptr_t diff = (intptr_t)seq - (intptr_t)pos;
        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(&queue->tail, &pos, pos + 1,
                                                      memory_order_relaxed, memory_order_relaxed)) {
                slot->data = data;
                atomic_store_explicit(&slot->seq, pos + 1, memory_order_release);
                return 0;
            }
        } else if (diff < 0) {
            errno = EAGAIN;
            return -1;
        } else {
            pos = atomic_load_explicit(&queue->tail, memory_order_relaxed);
        }
    }
}

void *fp_queue_pop(fp_queue *queue) {
    if (!queue) {
        errno = EINVAL;
        return NULL;
    }

    fp_queue_slot *slot = NULL;
    size_t pos = atomic_load_explicit(&queue->head, memory_order_relaxed);

    for (;;) {
        slot = &queue->slots[pos % queue->capacity];
        size_t seq = atomic_load_explicit(&slot->seq, memory_order_acquire);
        intptr_t diff = (intptr_t)seq - (intptr_t)(pos + 1);
        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(&queue->head, &pos, pos + 1,
                                                      memory_order_relaxed, memory_order_relaxed)) {
                void *data = slot->data;
                atomic_store_explicit(&slot->seq, pos + queue->capacity, memory_order_release);
                return data;
            }
        } else if (diff < 0) {
            errno = EAGAIN;
            return NULL;
        } else {
            pos = atomic_load_explicit(&queue->head, memory_order_relaxed);
        }
    }
}

