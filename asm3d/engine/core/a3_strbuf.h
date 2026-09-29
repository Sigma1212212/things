/*
 * ASM3D - a3_strbuf.h
 * Growable string builder (heap backed).
 */
#ifndef A3_STRBUF_H
#define A3_STRBUF_H

#include "a3_base.h"
#include "a3_memory.h"

A3_EXTERN_C_BEGIN

typedef struct A3StrBuf {
    char *data;   /* always NUL terminated when non-NULL */
    usize len;
    usize cap;
    A3MemTag tag;
    b32 failed;   /* set when an allocation failed; content truncated */
} A3StrBuf;

void a3_strbuf_init(A3StrBuf *sb, A3MemTag tag);
void a3_strbuf_free(A3StrBuf *sb);
void a3_strbuf_clear(A3StrBuf *sb);
b32  a3_strbuf_reserve(A3StrBuf *sb, usize extra);
void a3_strbuf_append(A3StrBuf *sb, const char *s);
void a3_strbuf_append_n(A3StrBuf *sb, const char *s, usize n);
void a3_strbuf_append_char(A3StrBuf *sb, char c);
void a3_strbuf_appendf(A3StrBuf *sb, const char *fmt, ...) A3_PRINTF_LIKE(2, 3);
void a3_strbuf_append_repeat(A3StrBuf *sb, char c, usize n);
const char *a3_strbuf_cstr(const A3StrBuf *sb); /* "" when empty */

A3_EXTERN_C_END

#endif
