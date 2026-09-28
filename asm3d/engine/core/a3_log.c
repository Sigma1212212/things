/*
 * ASM3D - a3_log.c
 */
#include "a3_log.h"
#include "a3_atomic.h"
#include "a3_format.h"
#include "a3_string.h"
#include "../platform/a3_platform.h"

#define LOG_RING 1024
#define LOG_SINKS 8

static struct {
    A3LogEntry ring[LOG_RING];
    u32 head;      /* next write index */
    u32 count;
    u32 seq;
    u32 errors;
    u32 warnings;
    A3LogLevel min_level;
    b32 console_disabled;
    struct { A3LogSinkFn fn; void *user; } sinks[LOG_SINKS];
    A3Spinlock lock;
} g_log = { .min_level = A3_LOG_INFO };

static const char *g_level_names[A3_LOG_LEVEL_COUNT] = { "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL" };

void a3_log_set_min_level(A3LogLevel level) { g_log.min_level = level; }
A3LogLevel a3_log_min_level(void) { return g_log.min_level; }
void a3_log_set_console(b32 enabled) { g_log.console_disabled = !enabled; }

int a3_log_add_sink(A3LogSinkFn fn, void *user) {
    a3_spin_lock(&g_log.lock);
    for (int i = 0; i < LOG_SINKS; ++i) {
        if (!g_log.sinks[i].fn) {
            g_log.sinks[i].fn = fn;
            g_log.sinks[i].user = user;
            a3_spin_unlock(&g_log.lock);
            return i;
        }
    }
    a3_spin_unlock(&g_log.lock);
    return -1;
}

void a3_log_remove_sink(int handle) {
    if (handle < 0 || handle >= LOG_SINKS) return;
    a3_spin_lock(&g_log.lock);
    g_log.sinks[handle].fn = 0;
    g_log.sinks[handle].user = 0;
    a3_spin_unlock(&g_log.lock);
}

static void log_emit(A3LogLevel level, const char *category, const char *hint, const char *fmt, va_list args) {
    if (level < g_log.min_level) return;
    A3LogEntry e;
    e.time_ns = a3_time_ns();
    e.level = level;
    a3_strcpy(e.category, sizeof(e.category), category ? category : "engine");
    a3_vsnprintf(e.message, sizeof(e.message), fmt, args);
    a3_strcpy(e.hint, sizeof(e.hint), hint ? hint : "");

    a3_spin_lock(&g_log.lock);
    e.seq = ++g_log.seq;
    g_log.ring[g_log.head] = e;
    g_log.head = (g_log.head + 1) % LOG_RING;
    if (g_log.count < LOG_RING) g_log.count++;
    if (level >= A3_LOG_ERROR) g_log.errors++;
    else if (level == A3_LOG_WARN) g_log.warnings++;
    struct { A3LogSinkFn fn; void *user; } sinks[LOG_SINKS];
    for (int i = 0; i < LOG_SINKS; ++i) { sinks[i].fn = g_log.sinks[i].fn; sinks[i].user = g_log.sinks[i].user; }
    a3_spin_unlock(&g_log.lock);

    if (!g_log.console_disabled) {
        char line[A3_LOG_MSG_MAX + 256];
        int n = a3_snprintf(line, sizeof(line), "[%s][%s] %s%s%s\n", g_level_names[level], e.category, e.message,
                            e.hint[0] ? "\n    hint: " : "", e.hint);
        if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;
        a3_console_write(level, line, (usize)n);
    }
    for (int i = 0; i < LOG_SINKS; ++i) if (sinks[i].fn) sinks[i].fn(&e, sinks[i].user);
}

void a3_logv(A3LogLevel level, const char *category, const char *fmt, va_list args) {
    log_emit(level, category, 0, fmt, args);
}

void a3_log(A3LogLevel level, const char *category, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_emit(level, category, 0, fmt, args);
    va_end(args);
}

void a3_log_hint(A3LogLevel level, const char *category, const char *hint, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_emit(level, category, hint, fmt, args);
    va_end(args);
}

u32 a3_log_count(void) { return g_log.count; }

const A3LogEntry *a3_log_get(u32 index) {
    if (index >= g_log.count) return 0;
    u32 start = (g_log.head + LOG_RING - g_log.count) % LOG_RING;
    return &g_log.ring[(start + index) % LOG_RING];
}

u32 a3_log_error_count(void) { return g_log.errors; }
u32 a3_log_warning_count(void) { return g_log.warnings; }

void a3_log_clear(void) {
    a3_spin_lock(&g_log.lock);
    g_log.count = 0;
    g_log.errors = 0;
    g_log.warnings = 0;
    a3_spin_unlock(&g_log.lock);
}

void a3_assert_failed(const char *expr, const char *file, int line, const char *msg) {
    a3_log(A3_LOG_FATAL, "assert", "%s:%d: assertion failed: %s%s%s", file, line, expr, msg ? " - " : "", msg ? msg : "");
    a3_platform_abort("assertion failed");
}

b32 a3_verify_failed(const char *expr, const char *file, int line) {
    a3_log(A3_LOG_ERROR, "verify", "%s:%d: check failed: %s (recovered)", file, line, expr);
    return 0;
}
