/*
 * ASM3D - a3_string.c
 */
#include "a3_string.h"

void *a3_memcpy(void *dst, const void *src, usize n) {
#if defined(__clang__) || defined(__GNUC__)
    return __builtin_memcpy(dst, src, n);
#else
    u8 *d = (u8 *)dst; const u8 *s = (const u8 *)src;
    while (n--) *d++ = *s++;
    return dst;
#endif
}

void *a3_memmove(void *dst, const void *src, usize n) {
#if defined(__clang__) || defined(__GNUC__)
    return __builtin_memmove(dst, src, n);
#else
    u8 *d = (u8 *)dst; const u8 *s = (const u8 *)src;
    if (d < s) { while (n--) *d++ = *s++; }
    else { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
#endif
}

void *a3_memset(void *dst, int value, usize n) {
#if defined(__clang__) || defined(__GNUC__)
    return __builtin_memset(dst, value, n);
#else
    u8 *d = (u8 *)dst;
    while (n--) *d++ = (u8)value;
    return dst;
#endif
}

int a3_memcmp(const void *a, const void *b, usize n) {
    const u8 *x = (const u8 *)a, *y = (const u8 *)b;
    for (usize i = 0; i < n; ++i) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

usize a3_strlen(const char *s) {
    if (!s) return 0;
    const char *p = s;
    while (*p) ++p;
    return (usize)(p - s);
}

int a3_strcmp(const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";
    while (*a && *a == *b) { ++a; ++b; }
    return (int)(u8)*a - (int)(u8)*b;
}

int a3_strncmp(const char *a, const char *b, usize n) {
    if (!a) a = "";
    if (!b) b = "";
    for (usize i = 0; i < n; ++i) {
        if (a[i] != b[i] || !a[i]) return (int)(u8)a[i] - (int)(u8)b[i];
    }
    return 0;
}

b32 a3_streq(const char *a, const char *b) { return a3_strcmp(a, b) == 0; }

usize a3_strcpy(char *dst, usize cap, const char *src) {
    usize n = a3_strlen(src);
    if (cap == 0 || !dst) return n;
    usize c = n < cap - 1 ? n : cap - 1;
    if (c) a3_memcpy(dst, src, c);
    dst[c] = 0;
    return n;
}

usize a3_strcat(char *dst, usize cap, const char *src) {
    usize dl = a3_strlen(dst);
    if (dl >= cap) return dl + a3_strlen(src);
    return dl + a3_strcpy(dst + dl, cap - dl, src);
}

const char *a3_strchr(const char *s, int c) {
    if (!s) return 0;
    for (; *s; ++s) if (*s == (char)c) return s;
    return c == 0 ? s : 0;
}

const char *a3_strrchr(const char *s, int c) {
    const char *r = 0;
    if (!s) return 0;
    for (; *s; ++s) if (*s == (char)c) r = s;
    return r;
}

const char *a3_strstr(const char *hay, const char *needle) {
    if (!hay || !needle) return 0;
    usize nl = a3_strlen(needle);
    if (nl == 0) return hay;
    for (; *hay; ++hay) {
        if (*hay == *needle && a3_strncmp(hay, needle, nl) == 0) return hay;
    }
    return 0;
}

const char *a3_stristr(const char *hay, const char *needle) {
    if (!hay || !needle) return 0;
    usize nl = a3_strlen(needle);
    if (nl == 0) return hay;
    for (; *hay; ++hay) {
        usize i = 0;
        while (i < nl && hay[i] && a3_to_lower(hay[i]) == a3_to_lower(needle[i])) ++i;
        if (i == nl) return hay;
    }
    return 0;
}

int a3_fuzzy_score(const char *text, const char *pattern) {
    if (!text || !pattern) return 0;
    if (!*pattern) return 1;
    int score = 0, streak = 0;
    const char *t = text, *p = pattern;
    b32 prev_sep = 1;
    while (*t && *p) {
        if (a3_to_lower(*t) == a3_to_lower(*p)) {
            score += 1 + streak * 2 + (prev_sep ? 3 : 0);
            ++streak; ++p;
        } else {
            streak = 0;
        }
        prev_sep = (*t == ' ' || *t == '_' || *t == '/' || *t == '.');
        ++t;
    }
    if (*p) return 0; /* not all pattern chars matched */
    /* Prefer shorter candidates. */
    score = score * 100 / (int)(a3_strlen(text) + 10);
    return score > 0 ? score : 1;
}

b32 a3_str_starts_with(const char *s, const char *prefix) {
    return a3_strncmp(s, prefix, a3_strlen(prefix)) == 0;
}

b32 a3_str_ends_with(const char *s, const char *suffix) {
    usize sl = a3_strlen(s), xl = a3_strlen(suffix);
    return xl <= sl && a3_memcmp(s + sl - xl, suffix, xl) == 0;
}

b32 a3_parse_i64(const char *s, usize len, i64 *out) {
    usize i = 0;
    b32 neg = 0;
    if (!s || len == 0) return 0;
    if (s[0] == '-' || s[0] == '+') { neg = s[0] == '-'; i = 1; }
    if (i >= len) return 0;
    u64 v = 0;
    if (len - i > 2 && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        i += 2;
        for (; i < len; ++i) {
            int c = s[i], d;
            if (a3_is_digit(c)) d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return 0;
            v = v * 16 + (u64)d;
        }
    } else {
        for (; i < len; ++i) {
            if (!a3_is_digit(s[i])) return 0;
            v = v * 10 + (u64)(s[i] - '0');
        }
    }
    *out = neg ? -(i64)v : (i64)v;
    return 1;
}

b32 a3_parse_f64(const char *s, usize len, f64 *out) {
    usize i = 0;
    b32 neg = 0, any = 0;
    if (!s || len == 0) return 0;
    if (s[0] == '-' || s[0] == '+') { neg = s[0] == '-'; i = 1; }
    f64 v = 0.0;
    while (i < len && a3_is_digit(s[i])) { v = v * 10.0 + (s[i] - '0'); ++i; any = 1; }
    if (i < len && s[i] == '.') {
        ++i;
        f64 scale = 0.1;
        while (i < len && a3_is_digit(s[i])) { v += (s[i] - '0') * scale; scale *= 0.1; ++i; any = 1; }
    }
    if (!any) return 0;
    if (i < len && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        b32 eneg = 0;
        int e = 0;
        if (i < len && (s[i] == '-' || s[i] == '+')) { eneg = s[i] == '-'; ++i; }
        if (i >= len || !a3_is_digit(s[i])) return 0;
        while (i < len && a3_is_digit(s[i])) { e = e * 10 + (s[i] - '0'); ++i; if (e > 400) e = 400; }
        f64 p = 1.0, base = 10.0;
        while (e) { if (e & 1) p *= base; base *= base; e >>= 1; }
        v = eneg ? v / p : v * p;
    }
    if (i != len) return 0;
    *out = neg ? -v : v;
    return 1;
}

/* ---- String view ---- */
A3Str a3_str(const char *cstr) { A3Str s = { cstr ? cstr : "", a3_strlen(cstr) }; return s; }
A3Str a3_str_n(const char *ptr, usize len) { A3Str s = { ptr, len }; return s; }

b32 a3_str_eq(A3Str a, A3Str b) {
    return a.len == b.len && (a.len == 0 || a3_memcmp(a.ptr, b.ptr, a.len) == 0);
}

b32 a3_str_eq_cstr(A3Str a, const char *b) { return a3_str_eq(a, a3_str(b)); }

A3Str a3_str_trim(A3Str s) {
    while (s.len && a3_is_space(s.ptr[0])) { ++s.ptr; --s.len; }
    while (s.len && a3_is_space(s.ptr[s.len - 1])) --s.len;
    return s;
}

A3Str a3_str_sub(A3Str s, usize start, usize len) {
    if (start > s.len) start = s.len;
    if (len > s.len - start) len = s.len - start;
    return a3_str_n(s.ptr + start, len);
}

isize a3_str_find(A3Str s, char c) {
    for (usize i = 0; i < s.len; ++i) if (s.ptr[i] == c) return (isize)i;
    return -1;
}

A3Str a3_str_split_next(A3Str *s, char sep) {
    isize i = a3_str_find(*s, sep);
    A3Str head;
    if (i < 0) { head = *s; s->ptr += s->len; s->len = 0; }
    else { head = a3_str_n(s->ptr, (usize)i); s->ptr += i + 1; s->len -= (usize)i + 1; }
    return head;
}

void a3_str_to_buf(A3Str s, char *buf, usize cap) {
    if (!cap) return;
    usize n = s.len < cap - 1 ? s.len : cap - 1;
    if (n) a3_memcpy(buf, s.ptr, n);
    buf[n] = 0;
}

/* ---- Paths ---- */
const char *a3_path_filename(const char *path) {
    const char *a = a3_strrchr(path, '/');
    const char *b = a3_strrchr(path, '\\');
    const char *s = a > b ? a : b;
    return s ? s + 1 : (path ? path : "");
}

const char *a3_path_extension(const char *path) {
    const char *f = a3_path_filename(path);
    const char *d = a3_strrchr(f, '.');
    return (d && d != f) ? d : f + a3_strlen(f);
}

void a3_path_dirname(const char *path, char *out, usize cap) {
    const char *f = a3_path_filename(path);
    usize n = (usize)(f - path);
    if (n > 0) --n; /* drop separator */
    if (n >= cap) n = cap ? cap - 1 : 0;
    if (cap) { if (n) a3_memcpy(out, path, n); out[n] = 0; }
}

void a3_path_join(char *out, usize cap, const char *a, const char *b) {
    char tmp[1024];
    a3_strcpy(tmp, sizeof(tmp), a ? a : "");
    usize l = a3_strlen(tmp);
    if (l && tmp[l - 1] != '/' && b && *b) a3_strcat(tmp, sizeof(tmp), "/");
    if (b) a3_strcat(tmp, sizeof(tmp), b[0] == '/' && l ? b + 1 : b);
    a3_strcpy(out, cap, tmp);
    a3_path_normalize(out);
}

void a3_path_normalize(char *path) {
    if (!path) return;
    char *w = path;
    for (char *r = path; *r; ++r) {
        char c = *r == '\\' ? '/' : *r;
        if (c == '/' && w > path && w[-1] == '/') continue;
        *w++ = c;
    }
    *w = 0;
}

void a3_path_stem(const char *path, char *out, usize cap) {
    const char *f = a3_path_filename(path);
    const char *e = a3_path_extension(f);
    a3_str_to_buf(a3_str_n(f, (usize)(e - f)), out, cap);
}

const char *a3_result_str(A3Result r) {
    switch (r) {
    case A3_OK: return "ok";
    case A3_ERR_UNKNOWN: return "unknown error";
    case A3_ERR_OUT_OF_MEMORY: return "out of memory";
    case A3_ERR_INVALID_ARG: return "invalid argument";
    case A3_ERR_NOT_FOUND: return "not found";
    case A3_ERR_IO: return "I/O error";
    case A3_ERR_PARSE: return "parse error";
    case A3_ERR_UNSUPPORTED: return "unsupported";
    case A3_ERR_VERSION: return "version mismatch";
    case A3_ERR_FULL: return "container full";
    case A3_ERR_BUSY: return "busy";
    case A3_ERR_COMPILE: return "compile error";
    }
    return "?";
}

/* WebAssembly has no libc. The compiler may still emit calls to these symbols
 * (e.g. for struct copies), so we provide them here. */
#if A3_PLATFORM_WEB
#if !defined(__wasm_bulk_memory__)
#error "ASM3D wasm builds require -mbulk-memory (memcpy/memset lower to memory.copy/fill)"
#endif
void *memcpy(void *dst, const void *src, usize n);
void *memmove(void *dst, const void *src, usize n);
void *memset(void *dst, int value, usize n);
int memcmp(const void *a, const void *b, usize n);

/* With bulk-memory enabled these builtins lower to single memory.copy /
 * memory.fill instructions, so they never recurse into these functions. */
void *memcpy(void *dst, const void *src, usize n) { return __builtin_memcpy(dst, src, n); }
void *memmove(void *dst, const void *src, usize n) { return __builtin_memmove(dst, src, n); }
void *memset(void *dst, int value, usize n) { return __builtin_memset(dst, value, n); }
int memcmp(const void *a, const void *b, usize n) { return a3_memcmp(a, b, n); }
#endif
