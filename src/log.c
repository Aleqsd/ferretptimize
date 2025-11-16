#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include "log.h"

static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_log_file = NULL;
static pthread_once_t g_log_file_once = PTHREAD_ONCE_INIT;

static void fp_log_open_file(void) {
    const char *path = getenv("FERRET_LOG_PATH");
    if (!path || path[0] == '\0') {
        path = "ferretptimize.log";
    }
    g_log_file = fopen(path, "a");
    if (g_log_file) {
        setvbuf(g_log_file, NULL, _IOLBF, 0);
    }
}

static void fp_log_close_file(void) {
    pthread_mutex_lock(&g_log_mutex);
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
    pthread_mutex_unlock(&g_log_mutex);
}

__attribute__((destructor)) static void fp_log_shutdown(void) {
    fp_log_close_file();
}

static void fp_log_write(const char *emoji, const char *label, const char *fmt, va_list args) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    char timebuf[32];
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &tm);

    char message[1024];
    vsnprintf(message, sizeof(message), fmt, args);

    pthread_once(&g_log_file_once, fp_log_open_file);

    pthread_mutex_lock(&g_log_mutex);
    fprintf(stderr, "%s %s %s | %s\n", emoji, label, timebuf, message);
    if (g_log_file) {
        fprintf(g_log_file, "%s %s %s | %s\n", emoji, label, timebuf, message);
        fflush(g_log_file);
    }
    pthread_mutex_unlock(&g_log_mutex);
}

void fp_log_info(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fp_log_write("🌀", "INFO", fmt, args);
    va_end(args);
}

void fp_log_warn(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fp_log_write("⚠️", "WARN", fmt, args);
    va_end(args);
}

void fp_log_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fp_log_write("🔥", "ERR ", fmt, args);
    va_end(args);
}
