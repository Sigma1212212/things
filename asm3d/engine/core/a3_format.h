/*
 * ASM3D - a3_format.h
 * Freestanding printf-style formatting (no libc dependency).
 * Supported: %d %i %u %x %X %o %c %s %p %f %g %e %% with flags - 0 + space,
 * width, precision (.N / .*), length modifiers l ll z h hh.
 * Print an A3Str with: a3_snprintf(buf, n, "%.*s", A3_STR_ARG(s));
 */
#ifndef A3_FORMAT_H
#define A3_FORMAT_H

#include "a3_base.h"

A3_EXTERN_C_BEGIN

/* Returns the number of characters that would have been written (excluding
 * NUL), like C99 snprintf. Always NUL terminates when cap > 0. */
int a3_vsnprintf(char *buf, usize cap, const char *fmt, va_list args);
int a3_snprintf(char *buf, usize cap, const char *fmt, ...) A3_PRINTF_LIKE(3, 4);

/* Formats a float with fixed decimals into buf; used by JSON writer & UI. */
int a3_format_f64(char *buf, usize cap, f64 v, int decimals);

A3_EXTERN_C_END

#endif
