/*
 * ASM3D - a3_format.c
 */
#include "a3_format.h"
#include "a3_string.h"

typedef struct FmtOut {
    char *buf;
    usize cap;
    usize len;
} FmtOut;

static void out_char(FmtOut *o, char c) {
    if (o->len + 1 < o->cap) o->buf[o->len] = c;
    o->len++;
}

static void out_pad(FmtOut *o, char c, int n) {
    while (n-- > 0) out_char(o, c);
}

static void out_str(FmtOut *o, const char *s, usize n) {
    for (usize i = 0; i < n; ++i) out_char(o, s[i]);
}

enum { F_LEFT = 1, F_ZERO = 2, F_PLUS = 4, F_SPACE = 8, F_ALT = 16 };

static void out_field(FmtOut *o, const char *prefix, const char *body, usize body_len,
                      int width, int flags) {
    usize pl = a3_strlen(prefix);
    int pad = width - (int)(pl + body_len);
    if (!(flags & F_LEFT) && !(flags & F_ZERO)) out_pad(o, ' ', pad);
    out_str(o, prefix, pl);
    if (!(flags & F_LEFT) && (flags & F_ZERO)) out_pad(o, '0', pad);
    out_str(o, body, body_len);
    if (flags & F_LEFT) out_pad(o, ' ', pad);
}

static usize u64_to_str(char *tmp, u64 v, unsigned base, b32 upper) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char rev[32];
    usize n = 0;
    do { rev[n++] = digits[v % base]; v /= base; } while (v);
    for (usize i = 0; i < n; ++i) tmp[i] = rev[n - 1 - i];
    return n;
}

/* Float formatting: exact enough for editor/UI/JSON use (round-trips floats
 * printed with %.9g). Not a full Grisu/Ryu implementation. */
static usize f64_fixed(char *tmp, usize cap, f64 v, int prec) {
    usize n = 0;
    if (prec < 0) prec = 6;
    if (prec > 17) prec = 17;
    f64 scale = 1.0;
    for (int i = 0; i < prec; ++i) scale *= 10.0;
    f64 r = v * scale + 0.5;
    if (r >= 1.8e19) { /* too large for u64 path: fall back to exponent-less big print */
        f64 ip = v;
        int e = 0;
        while (ip >= 10.0) { ip /= 10.0; ++e; }
        char d[400];
        usize dn = 0;
        for (int i = 0; i <= e && dn < sizeof(d) - 1; ++i) {
            int digit = (int)ip;
            if (digit > 9) digit = 9;
            d[dn++] = (char)('0' + digit);
            ip = (ip - digit) * 10.0;
        }
        for (usize i = 0; i < dn && n < cap; ++i) tmp[n++] = d[i];
        return n;
    }
    u64 whole = (u64)r;
    u64 ipart = whole / (u64)scale;
    u64 fpart = whole % (u64)scale;
    n = u64_to_str(tmp, ipart, 10, 0);
    if (prec > 0 && n < cap) {
        tmp[n++] = '.';
        char fd[32];
        usize fl = u64_to_str(fd, fpart, 10, 0);
        for (usize i = fl; i < (usize)prec && n < cap; ++i) tmp[n++] = '0';
        for (usize i = 0; i < fl && n < cap; ++i) tmp[n++] = fd[i];
    }
    return n;
}

static usize f64_exp(char *tmp, usize cap, f64 v, int prec, b32 upper) {
    int e = 0;
    if (v != 0.0) {
        while (v >= 10.0) { v /= 10.0; ++e; }
        while (v < 1.0) { v *= 10.0; --e; }
    }
    usize n = f64_fixed(tmp, cap, v, prec);
    /* rounding may have produced 10.x */
    if (n >= 2 && tmp[0] == '1' && tmp[1] == '0') {
        n = f64_fixed(tmp, cap, v / 10.0, prec);
        ++e;
    }
    if (n + 5 < cap) {
        tmp[n++] = upper ? 'E' : 'e';
        tmp[n++] = e < 0 ? '-' : '+';
        if (e < 0) e = -e;
        if (e < 10) tmp[n++] = '0';
        n += u64_to_str(tmp + n, (u64)e, 10, 0);
    }
    return n;
}

static usize f64_general(char *tmp, usize cap, f64 v, int prec, b32 upper, b32 alt) {
    if (prec < 0) prec = 6;
    if (prec == 0) prec = 1;
    int e = 0;
    f64 t = v;
    if (t != 0.0) {
        while (t >= 10.0) { t /= 10.0; ++e; }
        while (t < 1.0) { t *= 10.0; --e; }
    }
    usize n;
    if (e < -4 || e >= prec) {
        n = f64_exp(tmp, cap, v, prec - 1, upper);
        if (!alt) { /* strip trailing zeros in mantissa */
            usize epos = 0;
            while (epos < n && tmp[epos] != 'e' && tmp[epos] != 'E') ++epos;
            usize dot = 0; b32 has_dot = 0;
            for (usize i = 0; i < epos; ++i) if (tmp[i] == '.') { dot = i; has_dot = 1; }
            if (has_dot) {
                usize end = epos;
                while (end > dot + 1 && tmp[end - 1] == '0') --end;
                if (end == dot + 1) end = dot;
                a3_memmove(tmp + end, tmp + epos, n - epos);
                n -= epos - end;
            }
        }
    } else {
        int decimals = prec - 1 - e;
        if (decimals < 0) decimals = 0;
        n = f64_fixed(tmp, cap, v, decimals);
        if (!alt) {
            b32 has_dot = 0;
            for (usize i = 0; i < n; ++i) if (tmp[i] == '.') has_dot = 1;
            if (has_dot) {
                while (n > 0 && tmp[n - 1] == '0') --n;
                if (n > 0 && tmp[n - 1] == '.') --n;
            }
        }
    }
    return n;
}

int a3_vsnprintf(char *buf, usize cap, const char *fmt, va_list args) {
    FmtOut o = { buf, cap, 0 };
    if (!fmt) fmt = "(null fmt)";
    for (const char *p = fmt; *p; ++p) {
        if (*p != '%') { out_char(&o, *p); continue; }
        ++p;
        int flags = 0;
        for (;; ++p) {
            if (*p == '-') flags |= F_LEFT;
            else if (*p == '0') flags |= F_ZERO;
            else if (*p == '+') flags |= F_PLUS;
            else if (*p == ' ') flags |= F_SPACE;
            else if (*p == '#') flags |= F_ALT;
            else break;
        }
        int width = 0;
        if (*p == '*') { width = va_arg(args, int); if (width < 0) { flags |= F_LEFT; width = -width; } ++p; }
        else while (a3_is_digit(*p)) width = width * 10 + (*p++ - '0');
        int prec = -1;
        if (*p == '.') {
            ++p; prec = 0;
            if (*p == '*') { prec = va_arg(args, int); ++p; }
            else while (a3_is_digit(*p)) prec = prec * 10 + (*p++ - '0');
        }
        int lng = 0; /* 0=int, 1=long, 2=long long, 3=size_t, -1 short, -2 char */
        for (;;) {
            if (*p == 'l') { ++lng; ++p; }
            else if (*p == 'z' || *p == 't' || *p == 'j') { lng = 3; ++p; }
            else if (*p == 'h') { --lng; ++p; }
            else break;
        }
        char tmp[512];
        usize tn = 0;
        char c = *p;
        switch (c) {
        case 'd': case 'i': {
            i64 v;
            if (lng == 2) v = va_arg(args, long long);
            else if (lng == 1) v = va_arg(args, long);
            else if (lng == 3) v = (i64)va_arg(args, isize);
            else v = va_arg(args, int);
            if (lng == -1) v = (short)v;
            if (lng <= -2) v = (signed char)v;
            const char *prefix = v < 0 ? "-" : (flags & F_PLUS) ? "+" : (flags & F_SPACE) ? " " : "";
            u64 uv = v < 0 ? (u64)0 - (u64)v : (u64)v;
            tn = u64_to_str(tmp, uv, 10, 0);
            if (prec >= 0) { flags &= ~F_ZERO; while ((int)tn < prec) { a3_memmove(tmp + 1, tmp, tn); tmp[0] = '0'; ++tn; } }
            out_field(&o, prefix, tmp, tn, width, flags);
        } break;
        case 'u': case 'x': case 'X': case 'o': {
            u64 v;
            if (lng == 2) v = va_arg(args, unsigned long long);
            else if (lng == 1) v = va_arg(args, unsigned long);
            else if (lng == 3) v = va_arg(args, usize);
            else v = va_arg(args, unsigned int);
            if (lng == -1) v = (unsigned short)v;
            if (lng <= -2) v = (unsigned char)v;
            unsigned base = c == 'u' ? 10 : c == 'o' ? 8 : 16;
            tn = u64_to_str(tmp, v, base, c == 'X');
            if (prec >= 0) { flags &= ~F_ZERO; while ((int)tn < prec) { a3_memmove(tmp + 1, tmp, tn); tmp[0] = '0'; ++tn; } }
            const char *prefix = (flags & F_ALT) && base == 16 && v ? (c == 'X' ? "0X" : "0x") : "";
            out_field(&o, prefix, tmp, tn, width, flags);
        } break;
        case 'p': {
            uptr v = (uptr)va_arg(args, void *);
            tn = u64_to_str(tmp, (u64)v, 16, 0);
            out_field(&o, "0x", tmp, tn, width, flags);
        } break;
        case 'c': {
            tmp[0] = (char)va_arg(args, int);
            out_field(&o, "", tmp, 1, width, flags & ~F_ZERO);
        } break;
        case 's': {
            const char *s = va_arg(args, const char *);
            if (!s) s = "(null)";
            usize l = a3_strlen(s);
            if (prec >= 0 && (usize)prec < l) l = (usize)prec;
            out_field(&o, "", s, l, width, flags & ~F_ZERO);
        } break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            f64 v = va_arg(args, f64);
            const char *prefix = "";
            if (v != v) { out_field(&o, "", "nan", 3, width, flags & ~F_ZERO); break; }
            if (v < 0 || (v == 0 && 1.0 / v < 0)) { prefix = "-"; v = -v; }
            else if (flags & F_PLUS) prefix = "+";
            else if (flags & F_SPACE) prefix = " ";
            if (v > 1.7976931348623157e308) { out_field(&o, prefix, "inf", 3, width, flags & ~F_ZERO); break; }
            if (c == 'f' || c == 'F') tn = f64_fixed(tmp, sizeof(tmp), v, prec);
            else if (c == 'e' || c == 'E') tn = f64_exp(tmp, sizeof(tmp), v, prec < 0 ? 6 : prec, c == 'E');
            else tn = f64_general(tmp, sizeof(tmp), v, prec, c == 'G', (flags & F_ALT) != 0);
            out_field(&o, prefix, tmp, tn, width, flags);
        } break;
        case '%': out_char(&o, '%'); break;
        case 0: --p; break;
        default: out_char(&o, '%'); out_char(&o, c); break;
        }
    }
    if (cap > 0) buf[o.len < cap ? o.len : cap - 1] = 0;
    return (int)o.len;
}

int a3_snprintf(char *buf, usize cap, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int r = a3_vsnprintf(buf, cap, fmt, args);
    va_end(args);
    return r;
}

int a3_format_f64(char *buf, usize cap, f64 v, int decimals) {
    return a3_snprintf(buf, cap, "%.*f", decimals, v);
}
