/*
 * ASM3D - a3_string.h
 * Freestanding memory/string utilities and a length-based string view.
 * The engine never relies on libc string functions so behavior is identical
 * on native and WebAssembly targets.
 */
#ifndef A3_STRING_H
#define A3_STRING_H

#include "a3_base.h"

A3_EXTERN_C_BEGIN

/* Memory primitives. On wasm these lower to bulk-memory instructions. */
void  *a3_memcpy(void *dst, const void *src, usize n);
void  *a3_memmove(void *dst, const void *src, usize n);
void  *a3_memset(void *dst, int value, usize n);
int    a3_memcmp(const void *a, const void *b, usize n);
#define a3_zero(ptr, n) a3_memset((ptr), 0, (n))
#define a3_zero_struct(p) a3_memset((p), 0, sizeof(*(p)))

usize  a3_strlen(const char *s);
int    a3_strcmp(const char *a, const char *b);
int    a3_strncmp(const char *a, const char *b, usize n);
b32    a3_streq(const char *a, const char *b);
/* Copies at most cap-1 chars and always NUL terminates (when cap > 0).
 * Returns the length of src (like strlcpy) so truncation is detectable. */
usize  a3_strcpy(char *dst, usize cap, const char *src);
usize  a3_strcat(char *dst, usize cap, const char *src);
const char *a3_strchr(const char *s, int c);
const char *a3_strrchr(const char *s, int c);
const char *a3_strstr(const char *hay, const char *needle);
b32    a3_str_starts_with(const char *s, const char *prefix);
b32    a3_str_ends_with(const char *s, const char *suffix);

A3_INLINE b32 a3_is_space(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
A3_INLINE b32 a3_is_digit(int c) { return c >= '0' && c <= '9'; }
A3_INLINE b32 a3_is_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
A3_INLINE b32 a3_is_alnum(int c) { return a3_is_alpha(c) || a3_is_digit(c); }
A3_INLINE b32 a3_is_ident(int c) { return a3_is_alnum(c) || c == '_'; }
A3_INLINE b32 a3_is_xdigit(int c) { return a3_is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
A3_INLINE int a3_to_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
A3_INLINE int a3_to_upper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

/* Case-insensitive substring search, used by global search / command palette. */
const char *a3_stristr(const char *hay, const char *needle);
/* Fuzzy subsequence match score (0 = no match). Higher is better. */
int    a3_fuzzy_score(const char *text, const char *pattern);

/* Number parsing. Return false on malformed input. */
b32    a3_parse_i64(const char *s, usize len, i64 *out);
b32    a3_parse_f64(const char *s, usize len, f64 *out);

/* ---- String view ---- */
typedef struct A3Str {
    const char *ptr;
    usize len;
} A3Str;

#define A3_STR(lit) ((A3Str){ (lit), sizeof(lit) - 1 })
#define A3_STR_ARG(s) (int)(s).len, (s).ptr
A3Str  a3_str(const char *cstr);
A3Str  a3_str_n(const char *ptr, usize len);
b32    a3_str_eq(A3Str a, A3Str b);
b32    a3_str_eq_cstr(A3Str a, const char *b);
A3Str  a3_str_trim(A3Str s);
A3Str  a3_str_sub(A3Str s, usize start, usize len);
isize  a3_str_find(A3Str s, char c);
/* Splits off the text before the first `sep`; advances *s past the separator. */
A3Str  a3_str_split_next(A3Str *s, char sep);
/* Copies into a NUL terminated buffer, truncating if needed. */
void   a3_str_to_buf(A3Str s, char *buf, usize cap);

/* ---- Path helpers (always use '/' internally; platform layer converts) ---- */
const char *a3_path_filename(const char *path);          /* "a/b/c.txt" -> "c.txt" */
const char *a3_path_extension(const char *path);         /* "c.txt" -> ".txt" or "" */
void   a3_path_dirname(const char *path, char *out, usize cap);
void   a3_path_join(char *out, usize cap, const char *a, const char *b);
void   a3_path_normalize(char *path);                    /* '\\' -> '/', collapse "//" */
void   a3_path_stem(const char *path, char *out, usize cap); /* "a/b/c.txt" -> "c" */

A3_EXTERN_C_END

#endif
