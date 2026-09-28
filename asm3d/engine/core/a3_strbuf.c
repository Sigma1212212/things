/*
 * ASM3D - a3_strbuf.c
 */
#include "a3_strbuf.h"
#include "a3_string.h"
#include "a3_format.h"

void a3_strbuf_init(A3StrBuf *sb, A3MemTag tag) { a3_zero_struct(sb); sb->tag = tag; }
void a3_strbuf_free(A3StrBuf *sb) { a3_free(sb->data); sb->data = 0; sb->len = sb->cap = 0; }
void a3_strbuf_clear(A3StrBuf *sb) { sb->len = 0; if (sb->data) sb->data[0] = 0; sb->failed = 0; }

b32 a3_strbuf_reserve(A3StrBuf *sb, usize extra) {
    if (sb->len + extra + 1 <= sb->cap) return 1;
    usize nc = sb->cap ? sb->cap * 2 : 256;
    while (nc < sb->len + extra + 1) nc *= 2;
    char *p = (char *)a3_realloc(sb->data, nc, sb->tag);
    if (!p) { sb->failed = 1; return 0; }
    sb->data = p;
    sb->cap = nc;
    return 1;
}

void a3_strbuf_append_n(A3StrBuf *sb, const char *s, usize n) {
    if (!n || !a3_strbuf_reserve(sb, n)) return;
    a3_memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = 0;
}

void a3_strbuf_append(A3StrBuf *sb, const char *s) { a3_strbuf_append_n(sb, s, a3_strlen(s)); }
void a3_strbuf_append_char(A3StrBuf *sb, char c) { a3_strbuf_append_n(sb, &c, 1); }

void a3_strbuf_append_repeat(A3StrBuf *sb, char c, usize n) {
    if (!a3_strbuf_reserve(sb, n)) return;
    a3_memset(sb->data + sb->len, c, n);
    sb->len += n;
    sb->data[sb->len] = 0;
}

void a3_strbuf_appendf(A3StrBuf *sb, const char *fmt, ...) {
    va_list a, b;
    va_start(a, fmt);
    va_copy(b, a);
    int n = a3_vsnprintf(0, 0, fmt, a);
    va_end(a);
    if (n > 0 && a3_strbuf_reserve(sb, (usize)n)) {
        a3_vsnprintf(sb->data + sb->len, (usize)n + 1, fmt, b);
        sb->len += (usize)n;
    }
    va_end(b);
}

const char *a3_strbuf_cstr(const A3StrBuf *sb) { return sb->data ? sb->data : ""; }
