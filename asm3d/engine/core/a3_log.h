/*
 * ASM3D - a3_log.h
 * Structured logging with categories, sinks and an in-memory ring buffer that
 * the editor console reads. Logging never allocates.
 */
#ifndef A3_LOG_H
#define A3_LOG_H

#include "a3_base.h"

A3_EXTERN_C_BEGIN

typedef enum A3LogLevel {
    A3_LOG_TRACE = 0,
    A3_LOG_DEBUG,
    A3_LOG_INFO,
    A3_LOG_WARN,
    A3_LOG_ERROR,
    A3_LOG_FATAL,
    A3_LOG_LEVEL_COUNT
} A3LogLevel;

#define A3_LOG_MSG_MAX 480
#define A3_LOG_CAT_MAX 16

typedef struct A3LogEntry {
    u64 time_ns;
    u32 seq;
    A3LogLevel level;
    char category[A3_LOG_CAT_MAX];
    char message[A3_LOG_MSG_MAX];
    /* Optional plain-language explanation for beginners (see a3_diag). */
    char hint[160];
} A3LogEntry;

typedef void (*A3LogSinkFn)(const A3LogEntry *entry, void *user);

void a3_log_set_min_level(A3LogLevel level);
A3LogLevel a3_log_min_level(void);
/* Up to 8 sinks. Returns a handle (>=0) or -1. */
int  a3_log_add_sink(A3LogSinkFn fn, void *user);
void a3_log_remove_sink(int handle);
/* Enables/disables writing to stdout/console (default on). */
void a3_log_set_console(b32 enabled);

void a3_log(A3LogLevel level, const char *category, const char *fmt, ...) A3_PRINTF_LIKE(3, 4);
void a3_logv(A3LogLevel level, const char *category, const char *fmt, va_list args);
/* Same as a3_log but attaches a beginner-friendly hint string. */
void a3_log_hint(A3LogLevel level, const char *category, const char *hint, const char *fmt, ...) A3_PRINTF_LIKE(4, 5);

/* Ring buffer access (for the editor console). Returns number of entries
 * currently stored; index 0 is the oldest. */
u32  a3_log_count(void);
const A3LogEntry *a3_log_get(u32 index);
u32  a3_log_error_count(void);
u32  a3_log_warning_count(void);
void a3_log_clear(void);

#define A3_TRACE(cat, ...) a3_log(A3_LOG_TRACE, cat, __VA_ARGS__)
#define A3_DEBUG(cat, ...) a3_log(A3_LOG_DEBUG, cat, __VA_ARGS__)
#define A3_INFO(cat, ...)  a3_log(A3_LOG_INFO, cat, __VA_ARGS__)
#define A3_WARN(cat, ...)  a3_log(A3_LOG_WARN, cat, __VA_ARGS__)
#define A3_ERROR(cat, ...) a3_log(A3_LOG_ERROR, cat, __VA_ARGS__)
#define A3_FATAL(cat, ...) a3_log(A3_LOG_FATAL, cat, __VA_ARGS__)

/* ---- Assertions ----
 * A3_ASSERT: debug-only invariant check (programmer error).
 * A3_VERIFY: always evaluated; logs and returns false instead of crashing so
 *            callers can bail out gracefully: `if (!A3_VERIFY(ptr)) return;`
 */
void a3_assert_failed(const char *expr, const char *file, int line, const char *msg);

#ifndef A3_ENABLE_ASSERTS
#  ifdef NDEBUG
#    define A3_ENABLE_ASSERTS 0
#  else
#    define A3_ENABLE_ASSERTS 1
#  endif
#endif

#if A3_ENABLE_ASSERTS
#  define A3_ASSERT(expr) do { if (A3_UNLIKELY(!(expr))) a3_assert_failed(#expr, __FILE__, __LINE__, 0); } while (0)
#  define A3_ASSERT_MSG(expr, msg) do { if (A3_UNLIKELY(!(expr))) a3_assert_failed(#expr, __FILE__, __LINE__, msg); } while (0)
#else
#  define A3_ASSERT(expr) ((void)0)
#  define A3_ASSERT_MSG(expr, msg) ((void)0)
#endif

b32 a3_verify_failed(const char *expr, const char *file, int line);
#define A3_VERIFY(expr) (A3_LIKELY(!!(expr)) ? 1 : a3_verify_failed(#expr, __FILE__, __LINE__))

A3_EXTERN_C_END

#endif
