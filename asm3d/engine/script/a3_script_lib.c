/*
 * ASM3D - a3_script_lib.c
 * A3Script core library: printing, math, vectors, text and lists.
 */
#include "a3_script_internal.h"
#include "../core/a3_string.h"
#include "../core/a3_format.h"
#include "../core/a3_log.h"
#include "../core/a3_hash.h"

#define NATIVE(fname) static b32 fname(A3SVM *vm, A3SValue *a, u32 n, A3SValue *r)
#define UNUSED_ARGS() ((void)vm, (void)a, (void)n, (void)r)

static A3Rng g_rng;
static b32 g_rng_ready;

static A3Rng *rng(void) {
    if (!g_rng_ready) { a3_rng_seed(&g_rng, 0xA35C817Du, 7); g_rng_ready = 1; }
    return &g_rng;
}

/* ---- output ---- */

NATIVE(n_print) {
    char line[1024];
    usize len = 0;
    line[0] = 0;
    for (u32 i = 0; i < n; ++i) {
        char part[512];
        a3s_to_string(&a[i], part, sizeof(part));
        if (i && len + 1 < sizeof(line)) line[len++] = ' ';
        len += a3_strcpy(line + len, sizeof(line) - len, part);
        if (len >= sizeof(line) - 1) break;
    }
    line[len < sizeof(line) ? len : sizeof(line) - 1] = 0;
    const A3SHost *h = a3s_host();
    if (h->print) h->print(a3s_vm_instance(vm), line);
    else a3_log(A3_LOG_INFO, "script", "%s", line);
    *r = a3s_nil();
    return 1;
}

/* ---- conversion ---- */

NATIVE(n_str) {
    (void)vm; (void)n;
    char buf[1024];
    if (a[0].type == A3S_STR) { *r = a[0]; a3s_retain(r); return 1; }
    a3s_to_string(&a[0], buf, sizeof(buf));
    *r = a3s_str(buf);
    return 1;
}

NATIVE(n_num) {
    (void)vm; (void)n;
    if (a[0].type == A3S_NUM) { *r = a[0]; return 1; }
    if (a[0].type == A3S_BOOL) { *r = a3s_num(a[0].as.b ? 1 : 0); return 1; }
    if (a[0].type == A3S_STR) {
        f64 v;
        A3Str s = a3_str_trim(a3_str_n(a[0].as.str->chars, a[0].as.str->len));
        if (s.len && a3_parse_f64(s.ptr, s.len, &v)) { *r = a3s_num(v); return 1; }
    }
    *r = a3s_nil();
    return 1;
}

NATIVE(n_type) { (void)vm; (void)n; *r = a3s_str(a[0].type == A3S_BOOL ? "bool" : a3s_type_name(a[0].type)); return 1; }

/* ---- math (scalars) ---- */

typedef f32 (*F1)(f32);
static b32 math1(A3SVM *vm, A3SValue *a, A3SValue *r, F1 fn) {
    f64 x;
    if (!a3s_arg_num(vm, a, 0, &x)) return 0;
    *r = a3s_num((f64)fn((f32)x));
    return 1;
}

NATIVE(n_sin) { (void)n; return math1(vm, a, r, a3_sinf); }
NATIVE(n_cos) { (void)n; return math1(vm, a, r, a3_cosf); }
NATIVE(n_tan) { (void)n; return math1(vm, a, r, a3_tanf); }
NATIVE(n_atan) { (void)n; return math1(vm, a, r, a3_atanf); }
NATIVE(n_exp) { (void)n; return math1(vm, a, r, a3_expf); }

NATIVE(n_asin) {
    (void)n; f64 x;
    if (!a3s_arg_num(vm, a, 0, &x)) return 0;
    *r = a3s_num(a3_asinf(a3_clampf((f32)x, -1, 1)));
    return 1;
}
NATIVE(n_acos) {
    (void)n; f64 x;
    if (!a3s_arg_num(vm, a, 0, &x)) return 0;
    *r = a3s_num(a3_acosf(a3_clampf((f32)x, -1, 1)));
    return 1;
}
NATIVE(n_log) {
    (void)n; f64 x;
    if (!a3s_arg_num(vm, a, 0, &x)) return 0;
    if (x <= 0) return a3s_fail(vm, "log() needs a number above 0, but got %g", x);
    *r = a3s_num(a3_logf((f32)x));
    return 1;
}
NATIVE(n_atan2) {
    (void)n; f64 y, x;
    if (!a3s_arg_num(vm, a, 0, &y) || !a3s_arg_num(vm, a, 1, &x)) return 0;
    *r = a3s_num(a3_atan2f((f32)y, (f32)x));
    return 1;
}
NATIVE(n_pow) {
    (void)n; f64 x, y;
    if (!a3s_arg_num(vm, a, 0, &x) || !a3s_arg_num(vm, a, 1, &y)) return 0;
    if (y == __builtin_floor(y) && y >= 0 && y <= 64) { f64 p = 1; for (i32 i = 0; i < (i32)y; ++i) p *= x; *r = a3s_num(p); return 1; }
    *r = a3s_num(a3_powf((f32)x, (f32)y));
    return 1;
}
NATIVE(n_sqrt) {
    (void)n; f64 x;
    if (!a3s_arg_num(vm, a, 0, &x)) return 0;
    if (x < 0) return a3s_fail(vm, "sqrt() of a negative number (%g)", x);
    *r = a3s_num(__builtin_sqrt(x));
    return 1;
}
NATIVE(n_abs) {
    (void)n;
    if (a[0].type == A3S_VEC3) { *r = a3s_vec3(a3_absf(a[0].as.v[0]), a3_absf(a[0].as.v[1]), a3_absf(a[0].as.v[2])); return 1; }
    f64 x;
    if (!a3s_arg_num(vm, a, 0, &x)) return 0;
    *r = a3s_num(x < 0 ? -x : x);
    return 1;
}
NATIVE(n_floor) { (void)n; f64 x; if (!a3s_arg_num(vm, a, 0, &x)) return 0; *r = a3s_num(__builtin_floor(x)); return 1; }
NATIVE(n_ceil) { (void)n; f64 x; if (!a3s_arg_num(vm, a, 0, &x)) return 0; *r = a3s_num(__builtin_ceil(x)); return 1; }
NATIVE(n_round) {
    f64 x, d = 0;
    if (!a3s_arg_num(vm, a, 0, &x) || (n > 1 && !a3s_arg_num(vm, a, 1, &d))) return 0;
    f64 s = 1;
    for (i32 i = 0; i < (i32)d && i < 12; ++i) s *= 10;
    *r = a3s_num(__builtin_floor(x * s + 0.5) / s);
    return 1;
}
NATIVE(n_sign) { (void)n; f64 x; if (!a3s_arg_num(vm, a, 0, &x)) return 0; *r = a3s_num(x > 0 ? 1 : x < 0 ? -1 : 0); return 1; }
NATIVE(n_radians) { (void)n; f64 x; if (!a3s_arg_num(vm, a, 0, &x)) return 0; *r = a3s_num(x * (3.14159265358979323846 / 180.0)); return 1; }
NATIVE(n_degrees) { (void)n; f64 x; if (!a3s_arg_num(vm, a, 0, &x)) return 0; *r = a3s_num(x * (180.0 / 3.14159265358979323846)); return 1; }

static b32 minmax(A3SVM *vm, A3SValue *a, u32 n, A3SValue *r, b32 want_max) {
    A3SValue *items = a;
    u32 count = n;
    if (n == 1 && a[0].type == A3S_LIST) { items = a[0].as.list->items; count = a[0].as.list->count; }
    if (!count) return a3s_fail(vm, "%s() needs at least one number", want_max ? "max" : "min");
    f64 best = 0;
    for (u32 i = 0; i < count; ++i) {
        f64 x;
        if (!a3s_arg_num(vm, items, i, &x)) return 0;
        if (i == 0 || (want_max ? x > best : x < best)) best = x;
    }
    *r = a3s_num(best);
    return 1;
}
NATIVE(n_min) { return minmax(vm, a, n, r, 0); }
NATIVE(n_max) { return minmax(vm, a, n, r, 1); }

NATIVE(n_clamp) {
    (void)n; f64 x, lo, hi;
    if (!a3s_arg_num(vm, a, 0, &x) || !a3s_arg_num(vm, a, 1, &lo) || !a3s_arg_num(vm, a, 2, &hi)) return 0;
    *r = a3s_num(x < lo ? lo : x > hi ? hi : x);
    return 1;
}

NATIVE(n_lerp) {
    (void)n; f64 t;
    if (!a3s_arg_num(vm, a, 2, &t)) return 0;
    if (a[0].type == A3S_VEC3 && a[1].type == A3S_VEC3) {
        f32 ft = (f32)t;
        *r = a3s_vec3(a3_lerpf(a[0].as.v[0], a[1].as.v[0], ft), a3_lerpf(a[0].as.v[1], a[1].as.v[1], ft), a3_lerpf(a[0].as.v[2], a[1].as.v[2], ft));
        return 1;
    }
    f64 x, y;
    if (!a3s_arg_num(vm, a, 0, &x) || !a3s_arg_num(vm, a, 1, &y)) return 0;
    *r = a3s_num(x + (y - x) * t);
    return 1;
}

NATIVE(n_move_toward) {
    (void)n; f64 x, target, step;
    if (!a3s_arg_num(vm, a, 0, &x) || !a3s_arg_num(vm, a, 1, &target) || !a3s_arg_num(vm, a, 2, &step)) return 0;
    if (step < 0) step = -step;
    *r = a3s_num(x < target ? (x + step > target ? target : x + step) : (x - step < target ? target : x - step));
    return 1;
}

NATIVE(n_random) {
    f32 u = a3_rng_f32(rng());
    if (n == 0) { *r = a3s_num(u); return 1; }
    f64 lo, hi;
    if (!a3s_arg_num(vm, a, 0, &lo) || !a3s_arg_num(vm, a, 1, &hi)) return 0;
    *r = a3s_num(lo + (hi - lo) * u);
    return 1;
}

NATIVE(n_random_int) {
    (void)n; f64 lo, hi;
    if (!a3s_arg_num(vm, a, 0, &lo) || !a3s_arg_num(vm, a, 1, &hi)) return 0;
    if (hi < lo) { f64 t = lo; lo = hi; hi = t; }
    *r = a3s_num(a3_rng_range_i32(rng(), (i32)__builtin_floor(lo), (i32)__builtin_floor(hi)));
    return 1;
}

NATIVE(n_random_seed) {
    (void)n; f64 s;
    if (!a3s_arg_num(vm, a, 0, &s)) return 0;
    a3_rng_seed(rng(), (u64)(i64)s, 7);
    *r = a3s_nil();
    return 1;
}

/* ---- vectors ---- */

NATIVE(n_vec3) {
    f64 c[3] = { 0, 0, 0 };
    if (n == 1) { if (!a3s_arg_num(vm, a, 0, &c[0])) return 0; c[1] = c[2] = c[0]; }
    else if (n == 3) { for (u32 i = 0; i < 3; ++i) if (!a3s_arg_num(vm, a, i, &c[i])) return 0; }
    else if (n != 0) return a3s_fail(vm, "vec3() needs 0, 1 or 3 numbers, but was given %u", n);
    *r = a3s_vec3((f32)c[0], (f32)c[1], (f32)c[2]);
    return 1;
}

NATIVE(n_length) {
    (void)n; A3Vec3 v;
    if (!a3s_arg_vec3(vm, a, 0, &v)) return 0;
    *r = a3s_num(a3_v3_len(v));
    return 1;
}

NATIVE(n_normalize) {
    (void)n; A3Vec3 v;
    if (!a3s_arg_vec3(vm, a, 0, &v)) return 0;
    f32 l = a3_v3_len(v);
    if (l < 1e-8f) *r = a3s_vec3(0, 0, 0);
    else *r = a3s_vec3(v.x / l, v.y / l, v.z / l);
    return 1;
}

NATIVE(n_dot) {
    (void)n; A3Vec3 x, y;
    if (!a3s_arg_vec3(vm, a, 0, &x) || !a3s_arg_vec3(vm, a, 1, &y)) return 0;
    *r = a3s_num(a3_v3_dot(x, y));
    return 1;
}

NATIVE(n_cross) {
    (void)n; A3Vec3 x, y;
    if (!a3s_arg_vec3(vm, a, 0, &x) || !a3s_arg_vec3(vm, a, 1, &y)) return 0;
    A3Vec3 c = a3_v3_cross(x, y);
    *r = a3s_vec3(c.x, c.y, c.z);
    return 1;
}

NATIVE(n_distance) {
    (void)n; A3Vec3 x, y;
    if (!a3s_arg_vec3(vm, a, 0, &x) || !a3s_arg_vec3(vm, a, 1, &y)) return 0;
    *r = a3s_num(a3_v3_len(a3_v3_sub(x, y)));
    return 1;
}

/* ---- text and lists ---- */

NATIVE(n_len) {
    (void)n;
    if (a[0].type == A3S_STR) *r = a3s_num(a[0].as.str->len);
    else if (a[0].type == A3S_LIST) *r = a3s_num(a[0].as.list->count);
    else if (a[0].type == A3S_VEC3) { A3Vec3 v = a3_v3(a[0].as.v[0], a[0].as.v[1], a[0].as.v[2]); *r = a3s_num(a3_v3_len(v)); }
    else return a3s_fail(vm, "len() works on texts and lists, not on %s", a3s_type_name(a[0].type));
    return 1;
}

static b32 need_list(A3SVM *vm, A3SValue *a, u32 i) {
    if (a[i].type == A3S_LIST) return 1;
    return a3s_fail(vm, "value %u should be a list, but it is %s", i + 1, a3s_type_name(a[i].type));
}

NATIVE(n_list) {
    f64 count = 0;
    if (n >= 1 && !a3s_arg_num(vm, a, 0, &count)) return 0;
    if (count < 0 || count > 100000) return a3s_fail(vm, "list() size must be between 0 and 100000");
    A3SValue fill = n >= 2 ? a[1] : a3s_nil();
    *r = a3s_list((u32)count);
    for (u32 i = 0; i < (u32)count; ++i) a3s_list_push(r, &fill);
    return 1;
}

NATIVE(n_push) {
    (void)n;
    if (!need_list(vm, a, 0)) return 0;
    if (!a3s_list_push(&a[0], &a[1])) return a3s_fail(vm, "the list is too long");
    *r = a[0];
    a3s_retain(r);
    return 1;
}

NATIVE(n_pop) {
    (void)n;
    if (!need_list(vm, a, 0)) return 0;
    A3SList *l = a[0].as.list;
    if (!l->count) { *r = a3s_nil(); return 1; }
    *r = l->items[--l->count];
    return 1;
}

NATIVE(n_insert) {
    (void)n; f64 fi;
    if (!need_list(vm, a, 0) || !a3s_arg_num(vm, a, 1, &fi)) return 0;
    A3SList *l = a[0].as.list;
    i64 i = (i64)fi;
    if (i < 0) i += l->count + 1;
    if (i < 0 || i > (i64)l->count) return a3s_fail(vm, "insert() position %lld is outside the list (0 to %u)", (long long)(i64)fi, l->count);
    if (!a3s_list_push(&a[0], &a[2])) return a3s_fail(vm, "the list is too long");
    A3SValue v = l->items[l->count - 1];
    for (u32 k = l->count - 1; k > (u32)i; --k) l->items[k] = l->items[k - 1];
    l->items[i] = v;
    *r = a3s_nil();
    return 1;
}

NATIVE(n_remove_at) {
    (void)n; f64 fi;
    if (!need_list(vm, a, 0) || !a3s_arg_num(vm, a, 1, &fi)) return 0;
    A3SList *l = a[0].as.list;
    i64 i = (i64)fi;
    if (i < 0) i += l->count;
    if (i < 0 || i >= (i64)l->count) return a3s_fail(vm, "remove_at() index %lld is outside the list (it has %u items)", (long long)(i64)fi, l->count);
    *r = l->items[i];
    for (u32 k = (u32)i; k + 1 < l->count; ++k) l->items[k] = l->items[k + 1];
    l->count--;
    return 1;
}

static i64 find_in(const A3SValue *hay, const A3SValue *needle) {
    if (hay->type == A3S_LIST) {
        for (u32 i = 0; i < hay->as.list->count; ++i) if (a3s_equal(&hay->as.list->items[i], needle)) return i;
        return -1;
    }
    if (hay->type == A3S_STR && needle->type == A3S_STR) {
        u32 hl = hay->as.str->len, nl = needle->as.str->len;
        if (!nl) return 0;
        for (u32 i = 0; i + nl <= hl; ++i) if (!a3_memcmp(hay->as.str->chars + i, needle->as.str->chars, nl)) return i;
    }
    return -1;
}

NATIVE(n_remove) {
    (void)n;
    if (!need_list(vm, a, 0)) return 0;
    i64 i = find_in(&a[0], &a[1]);
    if (i >= 0) {
        A3SList *l = a[0].as.list;
        a3s_release(&l->items[i]);
        for (u32 k = (u32)i; k + 1 < l->count; ++k) l->items[k] = l->items[k + 1];
        l->count--;
    }
    *r = a3s_bool(i >= 0);
    return 1;
}

NATIVE(n_contains) {
    (void)n;
    if (a[0].type != A3S_LIST && a[0].type != A3S_STR) return a3s_fail(vm, "contains() looks inside a list or a text, not %s", a3s_type_name(a[0].type));
    *r = a3s_bool(find_in(&a[0], &a[1]) >= 0);
    return 1;
}

NATIVE(n_index_of) {
    (void)n;
    if (a[0].type != A3S_LIST && a[0].type != A3S_STR) return a3s_fail(vm, "index_of() looks inside a list or a text, not %s", a3s_type_name(a[0].type));
    *r = a3s_num((f64)find_in(&a[0], &a[1]));
    return 1;
}

NATIVE(n_clear) {
    (void)n;
    if (!need_list(vm, a, 0)) return 0;
    A3SList *l = a[0].as.list;
    for (u32 i = 0; i < l->count; ++i) a3s_release(&l->items[i]);
    l->count = 0;
    *r = a3s_nil();
    return 1;
}

NATIVE(n_copy) {
    (void)vm; (void)n;
    if (a[0].type != A3S_LIST) { *r = a[0]; a3s_retain(r); return 1; }
    *r = a3s_list(a[0].as.list->count);
    for (u32 i = 0; i < a[0].as.list->count; ++i) a3s_list_push(r, &a[0].as.list->items[i]);
    return 1;
}

NATIVE(n_range) {
    f64 lo = 0, hi, step = 1;
    if (n == 1) { if (!a3s_arg_num(vm, a, 0, &hi)) return 0; }
    else if (!a3s_arg_num(vm, a, 0, &lo) || !a3s_arg_num(vm, a, 1, &hi) || (n > 2 && !a3s_arg_num(vm, a, 2, &step))) return 0;
    if (step == 0) return a3s_fail(vm, "range() step cannot be 0");
    f64 count = (hi - lo) / step;
    if (count > 100000) return a3s_fail(vm, "range() would make more than 100000 numbers");
    *r = a3s_list(count > 0 ? (u32)count + 1 : 0);
    for (f64 x = lo; step > 0 ? x < hi : x > hi; x += step) { A3SValue v = a3s_num(x); a3s_list_push(r, &v); }
    return 1;
}

NATIVE(n_join) {
    const char *sep = "";
    if (!need_list(vm, a, 0) || (n > 1 && !a3s_arg_str(vm, a, 1, &sep))) return 0;
    A3_ARRAY_TYPE(char) buf = { 0 };
    usize seplen = a3_strlen(sep);
    for (u32 i = 0; i < a[0].as.list->count; ++i) {
        char part[512];
        const A3SValue *it = &a[0].as.list->items[i];
        const char *p = part;
        usize pl;
        if (it->type == A3S_STR) { p = it->as.str->chars; pl = it->as.str->len; }
        else { a3s_to_string(it, part, sizeof(part)); pl = a3_strlen(part); }
        if (i) for (usize k = 0; k < seplen; ++k) a3_array_push(buf, sep[k], A3_MEM_SCRIPT);
        for (usize k = 0; k < pl; ++k) a3_array_push(buf, p[k], A3_MEM_SCRIPT);
    }
    *r = a3s_str_n(buf.data ? buf.data : "", buf.count);
    a3_array_free(buf);
    return 1;
}

NATIVE(n_split) {
    const char *s, *sep = " ";
    if (!a3s_arg_str(vm, a, 0, &s) || (n > 1 && !a3s_arg_str(vm, a, 1, &sep))) return 0;
    usize sl = a[0].as.str->len, pl = a3_strlen(sep);
    *r = a3s_list(4);
    usize start = 0;
    for (usize i = 0; i <= sl; ++i) {
        b32 at_sep = pl && i + pl <= sl && !a3_memcmp(s + i, sep, pl);
        if (at_sep || i == sl) {
            A3SValue part = a3s_str_n(s + start, i - start);
            a3s_list_push(r, &part);
            a3s_release(&part);
            if (at_sep) { i += pl - 1; start = i + 1; }
        }
    }
    return 1;
}

static b32 map_case(A3SVM *vm, A3SValue *a, A3SValue *r, b32 upper) {
    const char *s;
    if (!a3s_arg_str(vm, a, 0, &s)) return 0;
    *r = a3s_str_n(s, a[0].as.str->len);
    if (r->type != A3S_STR) return a3s_fail(vm, "out of memory");
    for (u32 i = 0; i < r->as.str->len; ++i) {
        char c = r->as.str->chars[i];
        if (upper && c >= 'a' && c <= 'z') c = (char)(c - 32);
        else if (!upper && c >= 'A' && c <= 'Z') c = (char)(c + 32);
        r->as.str->chars[i] = c;
    }
    return 1;
}
NATIVE(n_upper) { (void)n; return map_case(vm, a, r, 1); }
NATIVE(n_lower) { (void)n; return map_case(vm, a, r, 0); }

NATIVE(n_substr) {
    const char *s;
    f64 fs, fl = -1;
    if (!a3s_arg_str(vm, a, 0, &s) || !a3s_arg_num(vm, a, 1, &fs) || (n > 2 && !a3s_arg_num(vm, a, 2, &fl))) return 0;
    i64 len = a[0].as.str->len, st = (i64)fs;
    if (st < 0) st += len;
    if (st < 0) st = 0;
    if (st > len) st = len;
    i64 cnt = fl < 0 ? len - st : (i64)fl;
    if (st + cnt > len) cnt = len - st;
    *r = a3s_str_n(s + st, (usize)cnt);
    return 1;
}

NATIVE(n_trim) {
    (void)n; const char *s;
    if (!a3s_arg_str(vm, a, 0, &s)) return 0;
    A3Str t = a3_str_trim(a3_str_n(s, a[0].as.str->len));
    *r = a3s_str_n(t.ptr, t.len);
    return 1;
}

NATIVE(n_starts_with) {
    (void)n; const char *s, *p;
    if (!a3s_arg_str(vm, a, 0, &s) || !a3s_arg_str(vm, a, 1, &p)) return 0;
    *r = a3s_bool(a[1].as.str->len <= a[0].as.str->len && !a3_memcmp(s, p, a[1].as.str->len));
    return 1;
}

NATIVE(n_format_number) {
    f64 x, d = 0;
    if (!a3s_arg_num(vm, a, 0, &x) || (n > 1 && !a3s_arg_num(vm, a, 1, &d))) return 0;
    char buf[64];
    a3_snprintf(buf, sizeof(buf), "%.*f", a3_clampi((i32)d, 0, 10), x);
    *r = a3s_str(buf);
    return 1;
}

NATIVE(n_assert) {
    if (a3s_truthy(&a[0])) { *r = a3s_nil(); return 1; }
    char msg[256] = "assertion failed";
    if (n > 1) { char m2[200]; a3s_to_string(&a[1], m2, sizeof(m2)); a3_snprintf(msg, sizeof(msg), "assertion failed: %s", m2); }
    return a3s_fail(vm, "%s", msg);
}

void a3s_register_core_lib(void) {
    static b32 done;
    if (done) return;
    done = 1;
    static const A3SNative lib[] = {
        { "print", n_print, 0, -1, "Basics", "print(a, b, ...)", "Writes values to the console." },
        { "str", n_str, 1, 1, "Basics", "str(x)", "Turns any value into text." },
        { "num", n_num, 1, 1, "Basics", "num(text)", "Turns text like \"3.5\" into a number (nil if it is not a number)." },
        { "type", n_type, 1, 1, "Basics", "type(x)", "The kind of value: \"number\", \"text\", \"list\", \"vec3\", \"bool\", \"object\"..." },
        { "assert", n_assert, 1, 2, "Basics", "assert(condition, message)", "Stops the script with an error if the condition is false." },
        { "sin", n_sin, 1, 1, "Math", "sin(radians)", "Sine of an angle in radians." },
        { "cos", n_cos, 1, 1, "Math", "cos(radians)", "Cosine of an angle in radians." },
        { "tan", n_tan, 1, 1, "Math", "tan(radians)", "Tangent of an angle in radians." },
        { "asin", n_asin, 1, 1, "Math", "asin(x)", "Inverse sine, in radians." },
        { "acos", n_acos, 1, 1, "Math", "acos(x)", "Inverse cosine, in radians." },
        { "atan", n_atan, 1, 1, "Math", "atan(x)", "Inverse tangent, in radians." },
        { "atan2", n_atan2, 2, 2, "Math", "atan2(y, x)", "Angle of the direction (x, y), in radians." },
        { "sqrt", n_sqrt, 1, 1, "Math", "sqrt(x)", "Square root." },
        { "pow", n_pow, 2, 2, "Math", "pow(x, y)", "x to the power y." },
        { "exp", n_exp, 1, 1, "Math", "exp(x)", "e to the power x." },
        { "log", n_log, 1, 1, "Math", "log(x)", "Natural logarithm." },
        { "abs", n_abs, 1, 1, "Math", "abs(x)", "Distance from zero (works on vec3 too)." },
        { "floor", n_floor, 1, 1, "Math", "floor(x)", "Rounds down." },
        { "ceil", n_ceil, 1, 1, "Math", "ceil(x)", "Rounds up." },
        { "round", n_round, 1, 2, "Math", "round(x, decimals)", "Rounds to the nearest number (optionally keeping some decimals)." },
        { "sign", n_sign, 1, 1, "Math", "sign(x)", "-1, 0 or 1." },
        { "min", n_min, 1, -1, "Math", "min(a, b, ...)", "The smallest number (also accepts one list)." },
        { "max", n_max, 1, -1, "Math", "max(a, b, ...)", "The largest number (also accepts one list)." },
        { "clamp", n_clamp, 3, 3, "Math", "clamp(x, lo, hi)", "Keeps x between lo and hi." },
        { "lerp", n_lerp, 3, 3, "Math", "lerp(a, b, t)", "Blends from a (t = 0) to b (t = 1). Works on numbers and vec3." },
        { "move_toward", n_move_toward, 3, 3, "Math", "move_toward(x, target, step)", "Moves x toward target by at most step." },
        { "radians", n_radians, 1, 1, "Math", "radians(degrees)", "Converts degrees to radians." },
        { "degrees", n_degrees, 1, 1, "Math", "degrees(radians)", "Converts radians to degrees." },
        { "random", n_random, 0, 2, "Math", "random() or random(lo, hi)", "A random number from 0 to 1, or between lo and hi." },
        { "random_int", n_random_int, 2, 2, "Math", "random_int(lo, hi)", "A random whole number from lo to hi (both included)." },
        { "random_seed", n_random_seed, 1, 1, "Math", "random_seed(n)", "Makes the random numbers repeat the same way every run." },
        { "vec3", n_vec3, 0, 3, "Vectors", "vec3(x, y, z)", "A 3D vector (position, direction or color). vec3(s) fills all three." },
        { "length", n_length, 1, 1, "Vectors", "length(v)", "Length of a vec3." },
        { "normalize", n_normalize, 1, 1, "Vectors", "normalize(v)", "The same direction with length 1." },
        { "dot", n_dot, 2, 2, "Vectors", "dot(a, b)", "Dot product of two vec3 values." },
        { "cross", n_cross, 2, 2, "Vectors", "cross(a, b)", "Cross product of two vec3 values." },
        { "distance", n_distance, 2, 2, "Vectors", "distance(a, b)", "Distance between two points." },
        { "len", n_len, 1, 1, "Lists and text", "len(x)", "Number of items in a list, or letters in a text." },
        { "list", n_list, 0, 2, "Lists and text", "list(count, fill)", "A new list with count copies of fill." },
        { "range", n_range, 1, 3, "Lists and text", "range(lo, hi, step)", "A list of numbers from lo up to (not including) hi." },
        { "push", n_push, 2, 2, "Lists and text", "push(list, value)", "Adds a value at the end of a list." },
        { "pop", n_pop, 1, 1, "Lists and text", "pop(list)", "Removes and returns the last item (nil if empty)." },
        { "insert", n_insert, 3, 3, "Lists and text", "insert(list, index, value)", "Inserts a value at a position." },
        { "remove_at", n_remove_at, 2, 2, "Lists and text", "remove_at(list, index)", "Removes and returns the item at a position." },
        { "remove", n_remove, 2, 2, "Lists and text", "remove(list, value)", "Removes the first matching item. Returns true if found." },
        { "contains", n_contains, 2, 2, "Lists and text", "contains(list_or_text, value)", "True if the value is in the list (or the text contains it)." },
        { "index_of", n_index_of, 2, 2, "Lists and text", "index_of(list_or_text, value)", "Position of the value, or -1." },
        { "clear", n_clear, 1, 1, "Lists and text", "clear(list)", "Removes every item." },
        { "copy", n_copy, 1, 1, "Lists and text", "copy(list)", "A new list with the same items (lists are shared otherwise)." },
        { "join", n_join, 1, 2, "Lists and text", "join(list, separator)", "Joins items into one text." },
        { "split", n_split, 1, 2, "Lists and text", "split(text, separator)", "Splits text into a list." },
        { "upper", n_upper, 1, 1, "Lists and text", "upper(text)", "UPPER CASE copy." },
        { "lower", n_lower, 1, 1, "Lists and text", "lower(text)", "lower case copy." },
        { "substr", n_substr, 2, 3, "Lists and text", "substr(text, start, count)", "Part of a text." },
        { "trim", n_trim, 1, 1, "Lists and text", "trim(text)", "Removes spaces at both ends." },
        { "starts_with", n_starts_with, 2, 2, "Lists and text", "starts_with(text, prefix)", "True if text begins with prefix." },
        { "format_number", n_format_number, 1, 2, "Lists and text", "format_number(x, decimals)", "Text with a fixed number of decimals, e.g. \"3.50\"." },
    };
    for (u32 i = 0; i < A3_ARRAY_COUNT(lib); ++i) a3s_register(&lib[i]);
}
