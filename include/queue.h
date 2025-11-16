#pragma once

#include <stddef.h>
#include <stdatomic.h>

typedef struct {
    _Atomic size_t seq;
    void *data;
} fp_queue_slot;

typedef struct {
    size_t capacity;
    fp_queue_slot *slots;
    _Atomic size_t head;
    _Atomic size_t tail;
} fp_queue;

fp_queue *fp_queue_create(size_t capacity);
void fp_queue_destroy(fp_queue *queue);
int fp_queue_push(fp_queue *queue, void *data);
void *fp_queue_pop(fp_queue *queue);

