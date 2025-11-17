#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "queue.h"

#define TEST_EXTERN(name) void name(void)
TEST_EXTERN(run_image_ops_tests);

#define PRODUCER_COUNT 4
#define CONSUMER_COUNT 4
#define ITEMS_PER_PRODUCER 512
#define QUEUE_CAPACITY 128

#define TEST_ASSERT(cond)                                                                         \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "Assertion failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);         \
            exit(EXIT_FAILURE);                                                                    \
        }                                                                                          \
    } while (0)

typedef _Atomic unsigned char atomic_uchar;

typedef struct {
    fp_queue *queue;
    size_t start;
    size_t count;
    size_t *values;
} producer_args;

typedef struct {
    fp_queue *queue;
    atomic_size_t *consumed;
    size_t total;
    atomic_uchar *seen;
    atomic_bool *failed;
} consumer_args;

static void *producer_thread(void *arg) {
    producer_args *args = (producer_args *)arg;
    for (size_t i = 0; i < args->count; ++i) {
        size_t idx = args->start + i;
        args->values[idx] = idx;
        while (fp_queue_push(args->queue, &args->values[idx]) != 0) {
            sched_yield();
        }
    }
    return NULL;
}

static void *consumer_thread(void *arg) {
    consumer_args *args = (consumer_args *)arg;
    while (atomic_load_explicit(args->consumed, memory_order_acquire) < args->total) {
        size_t *value_ptr = (size_t *)fp_queue_pop(args->queue);
        if (!value_ptr) {
            sched_yield();
            continue;
        }
        size_t value = *value_ptr;
        if (value >= args->total) {
            atomic_store(args->failed, true);
            continue;
        }
        unsigned char previous = atomic_exchange_explicit(&args->seen[value], 1, memory_order_acq_rel);
        if (previous != 0) {
            atomic_store(args->failed, true);
        }
        atomic_fetch_add_explicit(args->consumed, 1, memory_order_acq_rel);
    }
    return NULL;
}

static double fp_elapsed_ms(struct timespec start, struct timespec end) {
    return (double)(end.tv_sec - start.tv_sec) * 1000.0 +
           (double)(end.tv_nsec - start.tv_nsec) / 1e6;
}

static void test_queue_mpmc(void) {
    size_t total_items = PRODUCER_COUNT * ITEMS_PER_PRODUCER;
    printf("\n🧪 [queue-mpmc] Stressing %zu items (%d producers x %d consumers)\n",
           total_items,
           PRODUCER_COUNT,
           CONSUMER_COUNT);
    fp_queue *queue = fp_queue_create(QUEUE_CAPACITY);
    TEST_ASSERT(queue != NULL);

    size_t *values = calloc(total_items, sizeof(size_t));
    TEST_ASSERT(values != NULL);

    atomic_uchar *seen = calloc(total_items, sizeof(atomic_uchar));
    TEST_ASSERT(seen != NULL);

    atomic_size_t consumed;
    atomic_init(&consumed, 0);

    atomic_bool failed;
    atomic_init(&failed, false);

    pthread_t producers[PRODUCER_COUNT];
    pthread_t consumers[CONSUMER_COUNT];

    producer_args prod_args[PRODUCER_COUNT];
    consumer_args cons_args[CONSUMER_COUNT];

    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    for (size_t i = 0; i < PRODUCER_COUNT; ++i) {
        prod_args[i].queue = queue;
        prod_args[i].start = i * ITEMS_PER_PRODUCER;
        prod_args[i].count = ITEMS_PER_PRODUCER;
        prod_args[i].values = values;
        TEST_ASSERT(pthread_create(&producers[i], NULL, producer_thread, &prod_args[i]) == 0);
    }

    for (size_t i = 0; i < CONSUMER_COUNT; ++i) {
        cons_args[i].queue = queue;
        cons_args[i].consumed = &consumed;
        cons_args[i].total = total_items;
        cons_args[i].seen = seen;
        cons_args[i].failed = &failed;
        TEST_ASSERT(pthread_create(&consumers[i], NULL, consumer_thread, &cons_args[i]) == 0);
    }

    for (size_t i = 0; i < PRODUCER_COUNT; ++i) {
        pthread_join(producers[i], NULL);
    }
    for (size_t i = 0; i < CONSUMER_COUNT; ++i) {
        pthread_join(consumers[i], NULL);
    }

    TEST_ASSERT(!atomic_load(&failed));
    TEST_ASSERT(atomic_load(&consumed) == total_items);

    for (size_t i = 0; i < total_items; ++i) {
        TEST_ASSERT(atomic_load(&seen[i]) == 1);
    }

    struct timespec end_time;
    clock_gettime(CLOCK_MONOTONIC, &end_time);

    double ms = fp_elapsed_ms(start_time, end_time);
    printf("✅ [queue-mpmc] %zu items verified in %.2f ms\n", total_items, ms);
    printf("   • Producers: %d | Consumers: %d | Queue capacity: %d\n",
           PRODUCER_COUNT,
           CONSUMER_COUNT,
           QUEUE_CAPACITY);

    fp_queue_destroy(queue);
    free(values);
    free(seen);
}

static void test_queue_fifo_order(void) {
    printf("\n🧪 [queue-fifo] Verifying basic FIFO ordering\n");
    fp_queue *queue = fp_queue_create(2);
    TEST_ASSERT(queue != NULL);

    int a = 1, b = 2;
    TEST_ASSERT(fp_queue_push(queue, &a) == 0);
    TEST_ASSERT(fp_queue_push(queue, &b) == 0);

    int *first = (int *)fp_queue_pop(queue);
    int *second = (int *)fp_queue_pop(queue);

    TEST_ASSERT(first && *first == 1);
    TEST_ASSERT(second && *second == 2);
    TEST_ASSERT(fp_queue_pop(queue) == NULL); // empty now

    fp_queue_destroy(queue);
    printf("✅ [queue-fifo] Basic FIFO push/pop passed\n");
}

static void test_queue_capacity_backpressure(void) {
    printf("\n🧪 [queue-capacity] Ensuring push fails when full\n");
    fp_queue *queue = fp_queue_create(2);
    TEST_ASSERT(queue != NULL);

    int a = 1, b = 2, c = 3;
    TEST_ASSERT(fp_queue_push(queue, &a) == 0);
    TEST_ASSERT(fp_queue_push(queue, &b) == 0);

    errno = 0;
    TEST_ASSERT(fp_queue_push(queue, &c) == -1);
    TEST_ASSERT(errno == EAGAIN);

    int *first = (int *)fp_queue_pop(queue);
    int *second = (int *)fp_queue_pop(queue);
    TEST_ASSERT(first && *first == 1);
    TEST_ASSERT(second && *second == 2);
    fp_queue_destroy(queue);
    printf("✅ [queue-capacity] Backpressure path reported EAGAIN and preserved data\n");
}

int main(void) {
    test_queue_mpmc();
    test_queue_fifo_order();
    test_queue_capacity_backpressure();
    run_image_ops_tests();
    printf("[tests] queue suite passed\n");
    return 0;
}
