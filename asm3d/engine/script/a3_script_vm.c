/*
 * ASM3D - a3_script_vm.c
 * A3Script values, natives registry and the bytecode virtual machine.
 */
#include "a3_script_internal.h"
#include "../core/a3_string.h"
#include "../core/a3_format.h"
#include "../core/a3_log.h"

/* ======================================================================== */
/* Values                                                                   */
/* ======================================================================== */

static i64 g_live;

A3SValue a3s_nil(void) { A3SValue v; a3_zero_struct(&v); return v; }
A3SValue a3s_bool(b32 b) { A3SValue v = a3s_nil(); v.type = A3S_BOOL; v.as.b = b ? 1 : 0; return v; }
A3SValue a3s_num(f64 n) { A3SValue v = a3s_nil(); v.type = A3S_NUM; v.as.num = n; return v; }
A3SValue a3s_vec3(f32 x, f32 y, f32 z) { A3SValue v = a3s_nil(); v.type = A3S_VEC3; v.as.v[0] = x; v.as.v[1] = y; v.as.v[2] = z; return v; }
A3SValue a3s_entity(u64 guid) { A3SValue v = a3s_nil(); v.type = A3S_ENTITY; v.as.guid = guid; return v; }
A3SValue a3s_comp(u64 guid, u32 type_id) { A3SValue v = a3s_nil(); v.type = A3S_COMP; v.aux = type_id; v.as.guid = guid; return v; }

A3SValue a3s_str_n(const char *s, usize n) {
    if (n > 0x3FFFFFFF) n = 0x3FFFFFFF;
    A3SStr *str = (A3SStr *)a3_malloc(sizeof(A3SStr) + n, A3_MEM_SCRIPT);
    if (!str) return a3s_nil();
    str->refs = 1;
    str->len = (u32)n;
    if (n) a3_memcpy(str->chars, s, n);
    str->chars[n] = 0;
    g_live++;
    A3SValue v = a3s_nil();
    v.type = A3S_STR;
    v.as.str = str;
    return v;
}

A3SValue a3s_str(const char *s) { return a3s_str_n(s ? s : "", s ? a3_strlen(s) : 0); }

A3SValue a3s_list(u32 capacity) {
    A3SList *l = A3_NEW(A3SList, A3_MEM_SCRIPT);
    if (!l) return a3s_nil();
    l->refs = 1;
    if (capacity) {
        l->items = (A3SValue *)a3_malloc(sizeof(A3SValue) * capacity, A3_MEM_SCRIPT);
        if (l->items) l->cap = capacity;
    }
    g_live++;
    A3SValue v = a3s_nil();
    v.type = A3S_LIST;
    v.as.list = l;
    return v;
}

void a3s_retain(const A3SValue *v) {
    if (v->type == A3S_STR && v->as.str) v->as.str->refs++;
    else if (v->type == A3S_LIST && v->as.list) v->as.list->refs++;
}

void a3s_release(A3SValue *v) {
    if (v->type == A3S_STR && v->as.str) {
        if (--v->as.str->refs == 0) { a3_free(v->as.str); g_live--; }
    } else if (v->type == A3S_LIST && v->as.list) {
        A3SList *l = v->as.list;
        if (--l->refs == 0) {
            for (u32 i = 0; i < l->count; ++i) a3s_release(&l->items[i]);
            a3_free(l->items);
            a3_free(l);
            g_live--;
        }
    }
    *v = a3s_nil();
}

i64 a3s_live_objects(void) { return g_live; }

b32 a3s_truthy(const A3SValue *v) {
    switch (v->type) {
    case A3S_NIL: return 0;
    case A3S_BOOL: return v->as.b != 0;
    case A3S_NUM: return v->as.num != 0;
    case A3S_STR: return v->as.str && v->as.str->len != 0;
    case A3S_LIST: return v->as.list && v->as.list->count != 0;
    case A3S_ENTITY: case A3S_COMP: return v->as.guid != 0;
    default: return 1;
    }
}

static b32 equal_depth(const A3SValue *a, const A3SValue *b, u32 depth) {
    if (a->type != b->type) return 0;
    switch (a->type) {
    case A3S_NIL: return 1;
    case A3S_BOOL: return a->as.b == b->as.b;
    case A3S_NUM: return a->as.num == b->as.num;
    case A3S_VEC3: return a->as.v[0] == b->as.v[0] && a->as.v[1] == b->as.v[1] && a->as.v[2] == b->as.v[2];
    case A3S_STR: return a->as.str->len == b->as.str->len && !a3_memcmp(a->as.str->chars, b->as.str->chars, a->as.str->len);
    case A3S_LIST: {
        if (a->as.list == b->as.list) return 1;
        if (depth > 16 || a->as.list->count != b->as.list->count) return 0;
        for (u32 i = 0; i < a->as.list->count; ++i)
            if (!equal_depth(&a->as.list->items[i], &b->as.list->items[i], depth + 1)) return 0;
        return 1;
    }
    case A3S_FUNC: case A3S_NATIVE: return a->aux == b->aux;
    case A3S_ENTITY: return a->as.guid == b->as.guid;
    case A3S_COMP: return a->as.guid == b->as.guid && a->aux == b->aux;
    default: return 0;
    }
}

b32 a3s_equal(const A3SValue *a, const A3SValue *b) { return equal_depth(a, b, 0); }

b32 a3s_list_push(A3SValue *list, const A3SValue *item) {
    if (list->type != A3S_LIST || !list->as.list) return 0;
    A3SList *l = list->as.list;
    if (l->count >= 1000000) return 0;
    if (l->count == l->cap) {
        u32 nc = l->cap ? l->cap * 2 : 8;
        A3SValue *ni = (A3SValue *)a3_realloc(l->items, sizeof(A3SValue) * nc, A3_MEM_SCRIPT);
        if (!ni) return 0;
        l->items = ni;
        l->cap = nc;
    }
    l->items[l->count++] = *item;
    a3s_retain(item);
    return 1;
}

const char *a3s_type_name(u32 type) {
    static const char *const names[A3S_TYPE_COUNT] = { "nil", "true/false", "number", "vec3", "text", "list", "function", "function", "object", "component" };
    return type < A3S_TYPE_COUNT ? names[type] : "?";
}

typedef struct StrBuf { char *buf; usize cap, len; } StrBuf;

static void sb_put(StrBuf *sb, const char *s, usize n) {
    for (usize i = 0; i < n && sb->len + 1 < sb->cap; ++i) sb->buf[sb->len++] = s[i];
    if (sb->cap) sb->buf[sb->len < sb->cap ? sb->len : sb->cap - 1] = 0;
}

static void num_to_str(f64 n, char *out, usize cap) {
    if (n == (f64)(i64)n && n > -1e15 && n < 1e15) a3_snprintf(out, cap, "%lld", (long long)(i64)n);
    else a3_snprintf(out, cap, "%.6g", n);
}

static void to_string_rec(const A3SValue *v, StrBuf *sb, u32 depth, b32 quote) {
    char tmp[96];
    switch (v->type) {
    case A3S_NIL: sb_put(sb, "nil", 3); break;
    case A3S_BOOL: if (v->as.b) sb_put(sb, "true", 4); else sb_put(sb, "false", 5); break;
    case A3S_NUM: num_to_str(v->as.num, tmp, sizeof(tmp)); sb_put(sb, tmp, a3_strlen(tmp)); break;
    case A3S_VEC3: {
        char a[32], b[32], c[32];
        num_to_str(v->as.v[0], a, sizeof(a));
        num_to_str(v->as.v[1], b, sizeof(b));
        num_to_str(v->as.v[2], c, sizeof(c));
        a3_snprintf(tmp, sizeof(tmp), "(%s, %s, %s)", a, b, c);
        sb_put(sb, tmp, a3_strlen(tmp));
    } break;
    case A3S_STR:
        if (quote) sb_put(sb, "\"", 1);
        sb_put(sb, v->as.str->chars, v->as.str->len);
        if (quote) sb_put(sb, "\"", 1);
        break;
    case A3S_LIST: {
        if (depth > 4) { sb_put(sb, "[...]", 5); break; }
        sb_put(sb, "[", 1);
        for (u32 i = 0; i < v->as.list->count; ++i) {
            if (i) sb_put(sb, ", ", 2);
            if (i >= 64) { sb_put(sb, "...", 3); break; }
            to_string_rec(&v->as.list->items[i], sb, depth + 1, 1);
        }
        sb_put(sb, "]", 1);
    } break;
    case A3S_FUNC: sb_put(sb, "<function>", 10); break;
    case A3S_NATIVE: {
        const A3SNative *n = a3s_native_get(v->aux);
        a3_snprintf(tmp, sizeof(tmp), "<function %s>", n ? n->name : "?");
        sb_put(sb, tmp, a3_strlen(tmp));
    } break;
    case A3S_ENTITY: a3_snprintf(tmp, sizeof(tmp), "<object %llx>", (unsigned long long)v->as.guid); sb_put(sb, tmp, a3_strlen(tmp)); break;
    case A3S_COMP: a3_snprintf(tmp, sizeof(tmp), "<component of %llx>", (unsigned long long)v->as.guid); sb_put(sb, tmp, a3_strlen(tmp)); break;
    default: sb_put(sb, "?", 1); break;
    }
}

void a3s_to_string(const A3SValue *v, char *buf, usize cap) {
    if (!cap) return;
    buf[0] = 0;
    StrBuf sb = { buf, cap, 0 };
    to_string_rec(v, &sb, 0, 0);
}

A3SValue a3s_str_concat(const A3SValue *a, const A3SValue *b) {
    char sa[512], sbuf[512];
    const char *pa, *pb;
    usize la, lb;
    if (a->type == A3S_STR) { pa = a->as.str->chars; la = a->as.str->len; }
    else { a3s_to_string(a, sa, sizeof(sa)); pa = sa; la = a3_strlen(sa); }
    if (b->type == A3S_STR) { pb = b->as.str->chars; lb = b->as.str->len; }
    else { a3s_to_string(b, sbuf, sizeof(sbuf)); pb = sbuf; lb = a3_strlen(sbuf); }
    A3SValue r = a3s_nil();
    usize n = la + lb;
    A3SStr *s = (A3SStr *)a3_malloc(sizeof(A3SStr) + n, A3_MEM_SCRIPT);
    if (!s) return a3s_nil();
    s->refs = 1;
    s->len = (u32)n;
    a3_memcpy(s->chars, pa, la);
    a3_memcpy(s->chars + la, pb, lb);
    s->chars[n] = 0;
    g_live++;
    r.type = A3S_STR;
    r.as.str = s;
    return r;
}

/* ======================================================================== */
/* Natives and host                                                         */
/* ======================================================================== */

static A3_ARRAY_TYPE(A3SNative) g_natives;
static A3SHost g_host;

void a3s_register(const A3SNative *n) {
    if (!n || !n->name || !n->fn) return;
    for (u32 i = 0; i < g_natives.count; ++i)
        if (a3_streq(g_natives.data[i].name, n->name)) { g_natives.data[i] = *n; return; }
    a3_array_push(g_natives, *n, A3_MEM_SCRIPT);
}

u32 a3s_native_count(void) { return g_natives.count; }
const A3SNative *a3s_native_get(u32 i) { return i < g_natives.count ? &g_natives.data[i] : 0; }

i32 a3s_native_find(const char *name) {
    for (u32 i = 0; i < g_natives.count; ++i) if (a3_streq(g_natives.data[i].name, name)) return (i32)i;
    return -1;
}

void a3s_set_host(const A3SHost *host) { if (host) g_host = *host; else a3_zero_struct(&g_host); }
const A3SHost *a3s_host(void) { return &g_host; }

/* ======================================================================== */
/* VM                                                                       */
/* ======================================================================== */

typedef struct Frame { u32 func; u32 ip; u32 base; } Frame;

struct A3SVM {
    A3SInstance *inst;
    A3SValue stack[A3S_STACK];
    u32 sp;
    Frame frames[A3S_MAX_FRAMES];
    u32 nframes;
    u32 op_ip;              /* start of the running instruction */
    const A3SNative *native; /* native being called (for argument errors) */
    b32 failed;
    char message[256];
    char hint[192];
};

static u64 g_budget = 5000000;
void a3s_set_budget(u64 instructions) { g_budget = instructions ? instructions : 1; }

/* VMs are reused; nesting (a native that calls back into a script) takes the next one. */
#define A3S_MAX_NESTED 8
static A3SVM *g_vms[A3S_MAX_NESTED];
static u32 g_vm_depth;

A3SInstance *a3s_vm_instance(A3SVM *vm) { return vm ? vm->inst : 0; }

static b32 vm_error(A3SVM *vm, const char *msg, const char *hint) {
    if (vm->failed) return 0;
    vm->failed = 1;
    a3_strcpy(vm->message, sizeof(vm->message), msg);
    a3_strcpy(vm->hint, sizeof(vm->hint), hint ? hint : "");
    return 0;
}

b32 a3s_fail(A3SVM *vm, const char *fmt, ...) {
    if (!vm || vm->failed) return 0;
    char msg[256];
    va_list args;
    va_start(args, fmt);
    a3_vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    vm_error(vm, msg, vm->native && vm->native->signature ? "" : 0);
    if (vm->native && vm->native->signature) a3_snprintf(vm->hint, sizeof(vm->hint), "Use it like this: %s", vm->native->signature);
    return 0;
}

void a3s_set_error_hint(A3SVM *vm, const char *hint) {
    if (vm && hint) a3_strcpy(vm->hint, sizeof(vm->hint), hint);
}

static b32 arg_type_fail(A3SVM *vm, A3SValue *args, u32 i, const char *want) {
    const char *fname = vm->native ? vm->native->name : "this function";
    return a3s_fail(vm, "value %u given to '%s' should be %s, but it is %s", i + 1, fname, want, a3s_type_name(args[i].type));
}

b32 a3s_arg_num(A3SVM *vm, A3SValue *args, u32 i, f64 *out) {
    if (args[i].type == A3S_NUM) { *out = args[i].as.num; return 1; }
    if (args[i].type == A3S_BOOL) { *out = args[i].as.b ? 1 : 0; return 1; }
    return arg_type_fail(vm, args, i, "a number");
}

b32 a3s_arg_vec3(A3SVM *vm, A3SValue *args, u32 i, A3Vec3 *out) {
    if (args[i].type == A3S_VEC3) { *out = a3_v3(args[i].as.v[0], args[i].as.v[1], args[i].as.v[2]); return 1; }
    return arg_type_fail(vm, args, i, "a vec3 (like vec3(1, 2, 3))");
}

b32 a3s_arg_str(A3SVM *vm, A3SValue *args, u32 i, const char **out) {
    if (args[i].type == A3S_STR) { *out = args[i].as.str->chars; return 1; }
    return arg_type_fail(vm, args, i, "a text in quotes");
}

static const char *func_name(A3SModule *m, u32 f) { return f < m->funcs.count ? m->funcs.data[f].name : "?"; }

static void describe(const A3SValue *v, char *out, usize cap) {
    if (v->type == A3S_STR || v->type == A3S_LIST || v->type == A3S_VEC3 || v->type == A3S_NUM) {
        char s[64];
        a3s_to_string(v, s, sizeof(s));
        a3_snprintf(out, cap, "%s %s%s%s", a3s_type_name(v->type), v->type == A3S_STR ? "\"" : "", s, v->type == A3S_STR ? "\"" : "");
    } else a3_snprintf(out, cap, "%s", a3s_type_name(v->type));
}

static b32 op_type_error(A3SVM *vm, const char *what, const A3SValue *a, const A3SValue *b) {
    char da[96], db[96], msg[256];
    describe(a, da, sizeof(da));
    if (b) { describe(b, db, sizeof(db)); a3_snprintf(msg, sizeof(msg), "cannot %s %s and %s", what, da, db); }
    else a3_snprintf(msg, sizeof(msg), "cannot %s %s", what, da);
    const char *hint = 0;
    if (b && (a->type == A3S_NIL || b->type == A3S_NIL)) hint = "One of the values is nil (empty). Check that the variable was given a value.";
    else if (b && (a->type == A3S_STR || b->type == A3S_STR)) hint = "Use str(x) to turn a value into text, or num(\"3\") to turn text into a number.";
    return vm_error(vm, msg, hint);
}

static const char *const g_vec_fields[] = { "x", "y", "z", "r", "g", "b" };

static b32 vec_field_index(const char *name, u32 *out) {
    for (u32 i = 0; i < A3_ARRAY_COUNT(g_vec_fields); ++i) if (a3_streq(name, g_vec_fields[i])) { *out = i % 3; return 1; }
    return 0;
}

static b32 field_error(A3SVM *vm, const A3SValue *obj, const char *name) {
    char msg[200], hint[160];
    hint[0] = 0;
    if (obj->type == A3S_NIL) {
        a3_snprintf(msg, sizeof(msg), "tried to read '.%s' of nil", name);
        return vm_error(vm, msg, "The value is empty (nil). Check it was set, or that the object you looked for exists.");
    }
    if (obj->type == A3S_VEC3) {
        a3_snprintf(msg, sizeof(msg), "a vec3 has no field '%s'", name);
        a3s_suggest(name, g_vec_fields, 3, hint, sizeof(hint));
        return vm_error(vm, msg, hint[0] ? hint : "A vec3 has x, y and z.");
    }
    if (obj->type == A3S_LIST || obj->type == A3S_STR) {
        a3_snprintf(msg, sizeof(msg), "a %s has no field '%s'", a3s_type_name(obj->type), name);
        return vm_error(vm, msg, "Use len(x) for the size, and x[0] for the first item.");
    }
    a3_snprintf(msg, sizeof(msg), "a %s has no field '%s'", a3s_type_name(obj->type), name);
    return vm_error(vm, msg, 0);
}

static b32 get_field(A3SVM *vm, const A3SValue *obj, const char *name, A3SValue *out) {
    *out = a3s_nil();
    if (obj->type == A3S_VEC3) {
        u32 i;
        if (!vec_field_index(name, &i)) return field_error(vm, obj, name);
        *out = a3s_num(obj->as.v[i]);
        return 1;
    }
    if (obj->type == A3S_ENTITY || obj->type == A3S_COMP) {
        if (!g_host.get_field) return vm_error(vm, "objects are not available here", "This script is running outside of a scene.");
        return g_host.get_field(vm, obj, name, out) && !vm->failed;
    }
    return field_error(vm, obj, name);
}

static b32 set_field(A3SVM *vm, A3SValue *obj, const char *name, const A3SValue *v) {
    if (obj->type == A3S_VEC3) {
        u32 i;
        if (!vec_field_index(name, &i)) return field_error(vm, obj, name);
        if (v->type != A3S_NUM) { char msg[160]; a3_snprintf(msg, sizeof(msg), "'.%s' of a vec3 must be a number, not %s", name, a3s_type_name(v->type)); return vm_error(vm, msg, 0); }
        obj->as.v[i] = (f32)v->as.num;
        return 1;
    }
    if (obj->type == A3S_ENTITY || obj->type == A3S_COMP) {
        if (!g_host.set_field) return vm_error(vm, "objects are not available here", "This script is running outside of a scene.");
        return g_host.set_field(vm, obj, name, v) && !vm->failed;
    }
    if (obj->type == A3S_NIL) {
        char msg[160];
        a3_snprintf(msg, sizeof(msg), "tried to set '.%s' of nil", name);
        return vm_error(vm, msg, "The value is empty (nil). Check that the object exists.");
    }
    char msg[160];
    a3_snprintf(msg, sizeof(msg), "cannot set '.%s' on a %s", name, a3s_type_name(obj->type));
    return vm_error(vm, msg, 0);
}

static b32 index_of(A3SVM *vm, const A3SValue *idx, u32 count, const char *what, u32 *out) {
    if (idx->type != A3S_NUM) {
        char msg[160];
        a3_snprintf(msg, sizeof(msg), "a %s index must be a number, not %s", what, a3s_type_name(idx->type));
        return vm_error(vm, msg, 0);
    }
    f64 f = idx->as.num;
    if (f != __builtin_floor(f)) return vm_error(vm, "an index must be a whole number", "Use floor(x) to round it down.");
    i64 i = (i64)f;
    if (i < 0) i += count; /* -1 = last */
    if (i < 0 || i >= (i64)count) {
        char msg[200];
        a3_snprintf(msg, sizeof(msg), "index %lld is outside the %s (it has %u item%s)", (long long)(i64)f, what, count, count == 1 ? "" : "s");
        return vm_error(vm, msg, count ? "The first item is [0] and the last is [-1]." : "It is empty.");
    }
    *out = (u32)i;
    return 1;
}

static f64 num_mod(f64 a, f64 b) { return a - b * __builtin_floor(a / b); }

static b32 arith(A3SVM *vm, u8 op, const A3SValue *a, const A3SValue *b, A3SValue *out) {
    u32 ta = a->type, tb = b->type;
    if (ta == A3S_NUM && tb == A3S_NUM) {
        f64 x = a->as.num, y = b->as.num;
        switch (op) {
        case OP_ADD: *out = a3s_num(x + y); return 1;
        case OP_SUB: *out = a3s_num(x - y); return 1;
        case OP_MUL: *out = a3s_num(x * y); return 1;
        case OP_DIV: if (y == 0) return vm_error(vm, "division by zero", "Check the number you divide by is not 0."); *out = a3s_num(x / y); return 1;
        case OP_MOD: if (y == 0) return vm_error(vm, "remainder (%) by zero", "Check the number after % is not 0."); *out = a3s_num(num_mod(x, y)); return 1;
        }
    }
    if (ta == A3S_VEC3 && tb == A3S_VEC3) {
        const f32 *p = a->as.v, *q = b->as.v;
        switch (op) {
        case OP_ADD: *out = a3s_vec3(p[0] + q[0], p[1] + q[1], p[2] + q[2]); return 1;
        case OP_SUB: *out = a3s_vec3(p[0] - q[0], p[1] - q[1], p[2] - q[2]); return 1;
        case OP_MUL: *out = a3s_vec3(p[0] * q[0], p[1] * q[1], p[2] * q[2]); return 1;
        case OP_DIV:
            if (q[0] == 0 || q[1] == 0 || q[2] == 0) return vm_error(vm, "division of a vec3 by a vec3 containing 0", 0);
            *out = a3s_vec3(p[0] / q[0], p[1] / q[1], p[2] / q[2]); return 1;
        }
    }
    if ((ta == A3S_VEC3 && tb == A3S_NUM) || (ta == A3S_NUM && tb == A3S_VEC3)) {
        const f32 *p = ta == A3S_VEC3 ? a->as.v : b->as.v;
        f32 s = (f32)(ta == A3S_NUM ? a->as.num : b->as.num);
        if (op == OP_MUL) { *out = a3s_vec3(p[0] * s, p[1] * s, p[2] * s); return 1; }
        if (op == OP_DIV && ta == A3S_VEC3) {
            if (s == 0) return vm_error(vm, "division by zero", "Check the number you divide by is not 0.");
            *out = a3s_vec3(p[0] / s, p[1] / s, p[2] / s); return 1;
        }
    }
    if (op == OP_ADD && (ta == A3S_STR || tb == A3S_STR)) {
        *out = a3s_str_concat(a, b);
        return out->type == A3S_STR ? 1 : vm_error(vm, "out of memory", 0);
    }
    if (op == OP_ADD && ta == A3S_LIST && tb == A3S_LIST) {
        *out = a3s_list(a->as.list->count + b->as.list->count);
        for (u32 i = 0; i < a->as.list->count; ++i) a3s_list_push(out, &a->as.list->items[i]);
        for (u32 i = 0; i < b->as.list->count; ++i) a3s_list_push(out, &b->as.list->items[i]);
        return 1;
    }
    static const char *const verbs[] = { [OP_ADD] = "add", [OP_SUB] = "subtract", [OP_MUL] = "multiply", [OP_DIV] = "divide", [OP_MOD] = "take the remainder of" };
    return op_type_error(vm, verbs[op], a, b);
}

static b32 compare(A3SVM *vm, u8 op, const A3SValue *a, const A3SValue *b, b32 *out) {
    i32 c;
    if (a->type == A3S_NUM && b->type == A3S_NUM) c = a->as.num < b->as.num ? -1 : a->as.num > b->as.num ? 1 : 0;
    else if (a->type == A3S_STR && b->type == A3S_STR) {
        u32 n = a3_minu(a->as.str->len, b->as.str->len);
        c = a3_memcmp(a->as.str->chars, b->as.str->chars, n);
        if (!c) c = a->as.str->len < b->as.str->len ? -1 : a->as.str->len > b->as.str->len ? 1 : 0;
    } else return op_type_error(vm, "compare", a, b);
    switch (op) {
    case OP_LT: *out = c < 0; break;
    case OP_LE: *out = c <= 0; break;
    case OP_GT: *out = c > 0; break;
    default: *out = c >= 0; break;
    }
    return 1;
}

static A3SVM *vm_acquire(void) {
    if (g_vm_depth >= A3S_MAX_NESTED) return 0;
    if (!g_vms[g_vm_depth]) g_vms[g_vm_depth] = A3_NEW(A3SVM, A3_MEM_SCRIPT);
    A3SVM *vm = g_vms[g_vm_depth];
    if (vm) g_vm_depth++;
    return vm;
}

static void vm_release(A3SVM *vm) {
    while (vm->sp) a3s_release(&vm->stack[--vm->sp]);
    vm->nframes = 0;
    vm->inst = 0;
    g_vm_depth--;
}

/* Pushes a frame for script function `fi`; its arguments are already on the stack. */
static b32 push_frame(A3SVM *vm, u32 fi, u32 argc) {
    A3SModule *m = vm->inst->module;
    A3SFunc *f = &m->funcs.data[fi];
    if (vm->nframes >= A3S_MAX_FRAMES) return vm_error(vm, "too many nested function calls", "A function probably calls itself forever. Make sure it has a way to stop.");
    u32 base = vm->sp - argc;
    u32 nloc = f->max_locals > argc ? f->max_locals : argc;
    if (base + nloc + 64 >= A3S_STACK) return vm_error(vm, "the script ran out of stack space", "A function probably calls itself too many times.");
    while (vm->sp < base + nloc) vm->stack[vm->sp++] = a3s_nil();
    Frame *fr = &vm->frames[vm->nframes++];
    fr->func = fi;
    fr->ip = f->start;
    fr->base = base;
    return 1;
}

#define READ8() (code[ip++])
#define READ16() (ip += 2, (u32)code[ip - 2] | ((u32)code[ip - 1] << 8))
#define PUSH(v) do { if (vm->sp >= A3S_STACK) { vm_error(vm, "the script ran out of stack space", 0); goto error; } vm->stack[vm->sp++] = (v); } while (0)
#define POPV() (vm->stack[--vm->sp])
#define PEEK(n) (vm->stack[vm->sp - 1 - (n)])

static b32 run(A3SVM *vm, A3SValue *result) {
    A3SInstance *inst = vm->inst;
    A3SModule *m = inst->module;
    const u8 *code = m->code.data;
    Frame *fr = &vm->frames[vm->nframes - 1];
    u32 ip = fr->ip;
    u64 budget = g_budget;
    u32 stop_frames = vm->nframes - 1;
    for (;;) {
        if (!budget--) {
            char hint[160];
            a3_snprintf(hint, sizeof(hint), "Check that every while loop can end. A script may run %llu steps per call.", (unsigned long long)g_budget);
            vm_error(vm, "the script ran too long without finishing (an endless loop?)", hint);
            goto error;
        }
        vm->op_ip = ip;
        u8 op = READ8();
        switch (op) {
        case OP_CONST: { A3SValue v = m->consts.data[READ16()]; a3s_retain(&v); PUSH(v); } break;
        case OP_NIL: PUSH(a3s_nil()); break;
        case OP_TRUE: PUSH(a3s_bool(1)); break;
        case OP_FALSE: PUSH(a3s_bool(0)); break;
        case OP_SELF: { A3SValue v = inst->self; a3s_retain(&v); PUSH(v); } break;
        case OP_POP: a3s_release(&vm->stack[--vm->sp]); break;
        case OP_DUP: { A3SValue v = PEEK(0); a3s_retain(&v); PUSH(v); } break;
        case OP_SWAP: { A3SValue t = PEEK(0); PEEK(0) = PEEK(1); PEEK(1) = t; } break;
        case OP_ROT3: { A3SValue a = PEEK(2); PEEK(2) = PEEK(1); PEEK(1) = PEEK(0); PEEK(0) = a; } break;
        case OP_GET_LOCAL: { A3SValue v = vm->stack[fr->base + READ16()]; a3s_retain(&v); PUSH(v); } break;
        case OP_SET_LOCAL: { A3SValue *slot = &vm->stack[fr->base + READ16()]; a3s_release(slot); *slot = POPV(); } break;
        case OP_GET_GLOBAL: { A3SValue v = inst->globals[READ16()]; a3s_retain(&v); PUSH(v); } break;
        case OP_SET_GLOBAL: { A3SValue *slot = &inst->globals[READ16()]; a3s_release(slot); *slot = POPV(); } break;
        case OP_GET_FIELD: {
            const char *name = m->consts.data[READ16()].as.str->chars;
            A3SValue obj = POPV(), out;
            b32 ok = get_field(vm, &obj, name, &out);
            a3s_release(&obj);
            if (!ok) goto error;
            PUSH(out);
        } break;
        case OP_SET_FIELD: {
            const char *name = m->consts.data[READ16()].as.str->chars;
            A3SValue v = POPV();
            b32 ok = set_field(vm, &PEEK(0), name, &v);
            a3s_release(&v);
            if (!ok) goto error;
        } break;
        case OP_GET_INDEX: {
            A3SValue idx = POPV(), obj = POPV(), out = a3s_nil();
            u32 i;
            b32 ok = 1;
            if (obj.type == A3S_LIST) { if ((ok = index_of(vm, &idx, obj.as.list->count, "list", &i)) != 0) { out = obj.as.list->items[i]; a3s_retain(&out); } }
            else if (obj.type == A3S_STR) { if ((ok = index_of(vm, &idx, obj.as.str->len, "text", &i)) != 0) out = a3s_str_n(obj.as.str->chars + i, 1); }
            else if (obj.type == A3S_VEC3) { if ((ok = index_of(vm, &idx, 3, "vec3", &i)) != 0) out = a3s_num(obj.as.v[i]); }
            else {
                char msg[160];
                a3_snprintf(msg, sizeof(msg), "cannot use [ ] on %s", a3s_type_name(obj.type));
                ok = vm_error(vm, msg, obj.type == A3S_NIL ? "The value is empty (nil)." : "Only lists, texts and vec3 values have items.");
            }
            a3s_release(&idx);
            a3s_release(&obj);
            if (!ok) goto error;
            PUSH(out);
        } break;
        case OP_SET_INDEX: {
            A3SValue v = POPV(), idx = POPV();
            A3SValue *obj = &PEEK(0);
            u32 i;
            b32 ok = 1;
            if (obj->type == A3S_LIST) {
                if ((ok = index_of(vm, &idx, obj->as.list->count, "list", &i)) != 0) {
                    A3SValue *slot = &obj->as.list->items[i];
                    a3s_release(slot);
                    *slot = v;
                    v = a3s_nil();
                }
            } else if (obj->type == A3S_VEC3) {
                if ((ok = index_of(vm, &idx, 3, "vec3", &i)) != 0) {
                    if (v.type != A3S_NUM) ok = vm_error(vm, "vec3 items must be numbers", 0);
                    else obj->as.v[i] = (f32)v.as.num;
                }
            } else {
                char msg[160];
                a3_snprintf(msg, sizeof(msg), "cannot change items of %s", a3s_type_name(obj->type));
                ok = vm_error(vm, msg, obj->type == A3S_STR ? "Texts cannot be changed in place. Build a new text instead." : 0);
            }
            a3s_release(&v);
            a3s_release(&idx);
            if (!ok) goto error;
        } break;
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD: {
            A3SValue b = POPV(), a = POPV(), out;
            b32 ok = arith(vm, op, &a, &b, &out);
            a3s_release(&a);
            a3s_release(&b);
            if (!ok) goto error;
            PUSH(out);
        } break;
        case OP_NEG: {
            A3SValue *a = &PEEK(0);
            if (a->type == A3S_NUM) a->as.num = -a->as.num;
            else if (a->type == A3S_VEC3) { a->as.v[0] = -a->as.v[0]; a->as.v[1] = -a->as.v[1]; a->as.v[2] = -a->as.v[2]; }
            else { op_type_error(vm, "make negative", a, 0); goto error; }
        } break;
        case OP_NOT: { A3SValue a = POPV(); b32 t = a3s_truthy(&a); a3s_release(&a); PUSH(a3s_bool(!t)); } break;
        case OP_EQ: case OP_NE: {
            A3SValue b = POPV(), a = POPV();
            b32 eq = a3s_equal(&a, &b);
            a3s_release(&a);
            a3s_release(&b);
            PUSH(a3s_bool(op == OP_EQ ? eq : !eq));
        } break;
        case OP_LT: case OP_LE: case OP_GT: case OP_GE: {
            A3SValue b = POPV(), a = POPV();
            b32 r = 0;
            b32 ok = compare(vm, op, &a, &b, &r);
            a3s_release(&a);
            a3s_release(&b);
            if (!ok) goto error;
            PUSH(a3s_bool(r));
        } break;
        case OP_JUMP: { u32 off = READ16(); ip += off; } break;
        case OP_JUMP_IF_FALSE: { u32 off = READ16(); A3SValue c = POPV(); if (!a3s_truthy(&c)) ip += off; a3s_release(&c); } break;
        case OP_JUMP_IF_FALSE_KEEP: { u32 off = READ16(); if (!a3s_truthy(&PEEK(0))) ip += off; } break;
        case OP_JUMP_IF_TRUE_KEEP: { u32 off = READ16(); if (a3s_truthy(&PEEK(0))) ip += off; } break;
        case OP_LOOP: { u32 off = READ16(); ip -= off; } break;
        case OP_CALL: {
            u32 argc = READ8();
            A3SValue *callee = &vm->stack[vm->sp - argc - 1];
            if (callee->type == A3S_FUNC) {
                u32 fi = callee->aux;
                A3SFunc *f = &m->funcs.data[fi];
                if (argc != f->arity) {
                    char msg[200];
                    a3_snprintf(msg, sizeof(msg), "'%s' needs %u value%s, but was given %u", f->name, f->arity, f->arity == 1 ? "" : "s", argc);
                    vm_error(vm, msg, "Pass one value for each name between the ( ) of the fn line.");
                    goto error;
                }
                fr->ip = ip;
                if (!push_frame(vm, fi, argc)) goto error;
                fr = &vm->frames[vm->nframes - 1];
                ip = fr->ip;
            } else if (callee->type == A3S_NATIVE) {
                const A3SNative *n = a3s_native_get(callee->aux);
                if (!n) { vm_error(vm, "unknown built-in function", 0); goto error; }
                if ((i32)argc < n->min_args || (n->max_args >= 0 && (i32)argc > n->max_args)) {
                    char msg[200];
                    a3_snprintf(msg, sizeof(msg), "'%s' was given %u value%s", n->name, argc, argc == 1 ? "" : "s");
                    vm_error(vm, msg, 0);
                    if (n->signature) a3_snprintf(vm->hint, sizeof(vm->hint), "Use it like this: %s", n->signature);
                    goto error;
                }
                A3SValue out = a3s_nil();
                fr->ip = ip;
                vm->native = n;
                b32 ok = n->fn(vm, callee + 1, argc, &out);
                vm->native = 0;
                if (!ok || vm->failed) {
                    a3s_release(&out);
                    if (!vm->failed) { char msg[160]; a3_snprintf(msg, sizeof(msg), "'%s' failed", n->name); vm_error(vm, msg, 0); }
                    goto error;
                }
                /* natives never move the stack, but may re-enter another VM */
                for (u32 i = 0; i <= argc; ++i) a3s_release(&vm->stack[--vm->sp]);
                PUSH(out);
            } else {
                char msg[160];
                describe(callee, msg + 100, 60);
                a3_snprintf(msg, 100, "tried to call %s, which is not a function", msg + 100);
                vm_error(vm, msg, callee->type == A3S_NIL ? "The name holds nothing (nil). Check the spelling of the function." : 0);
                goto error;
            }
        } break;
        case OP_RETURN: {
            A3SValue r = POPV();
            u32 base = fr->base;
            /* release locals and the callee slot */
            while (vm->sp > base) a3s_release(&vm->stack[--vm->sp]);
            if (vm->sp) a3s_release(&vm->stack[--vm->sp]);
            vm->nframes--;
            if (vm->nframes == stop_frames) {
                if (result) *result = r; else a3s_release(&r);
                return 1;
            }
            PUSH(r);
            fr = &vm->frames[vm->nframes - 1];
            ip = fr->ip;
        } break;
        case OP_LIST: {
            u32 n = READ16();
            A3SValue l = a3s_list(n);
            if (l.type != A3S_LIST) { vm_error(vm, "out of memory", 0); goto error; }
            for (u32 i = 0; i < n; ++i) l.as.list->items[i] = vm->stack[vm->sp - n + i];
            l.as.list->count = n;
            vm->sp -= n;
            PUSH(l);
        } break;
        case OP_ITER: {
            u32 slot = READ16();
            u32 off = READ16();
            A3SValue *it = &vm->stack[fr->base + slot];
            A3SValue *idx = it + 1, *var = it + 2;
            u32 i = (u32)idx->as.num;
            u32 count;
            if (it->type == A3S_LIST) count = it->as.list->count;
            else if (it->type == A3S_STR) count = it->as.str->len;
            else if (it->type == A3S_NUM && it->as.num >= 0) count = (u32)it->as.num;
            else {
                char msg[160];
                a3_snprintf(msg, sizeof(msg), "cannot loop over %s", a3s_type_name(it->type));
                vm_error(vm, msg, "Loop over a list (for x in items), or numbers (for i in 0..10).");
                goto error;
            }
            if (i >= count) { ip += off; break; }
            a3s_release(var);
            if (it->type == A3S_LIST) { *var = it->as.list->items[i]; a3s_retain(var); }
            else if (it->type == A3S_STR) *var = a3s_str_n(it->as.str->chars + i, 1);
            else *var = a3s_num(i);
            idx->as.num = i + 1;
        } break;
        default:
            vm_error(vm, "corrupted script code", "Recompile the script.");
            goto error;
        }
        if (vm->failed) goto error;
    }
error:
    fr->ip = ip;
    return 0;
}

b32 a3s_vm_run(A3SInstance *inst, u32 func, const A3SValue *args, u32 argc, A3SValue *result, A3SError *err) {
    if (result) *result = a3s_nil();
    A3SModule *m = inst->module;
    if (func >= m->funcs.count) return 0;
    A3SVM *vm = vm_acquire();
    if (!vm) {
        if (err) { a3_zero_struct(err); a3_strcpy(err->message, sizeof(err->message), "scripts call each other too deeply"); }
        return 0;
    }
    vm->inst = inst;
    vm->sp = 0;
    vm->nframes = 0;
    vm->failed = 0;
    vm->native = 0;
    vm->message[0] = vm->hint[0] = 0;
    A3SFunc *f = &m->funcs.data[func];
    vm->stack[vm->sp++] = a3s_nil(); /* callee slot */
    for (u32 i = 0; i < f->arity; ++i) {
        A3SValue v = i < argc ? args[i] : a3s_nil();
        a3s_retain(&v);
        vm->stack[vm->sp++] = v;
    }
    b32 ok = push_frame(vm, func, f->arity) && run(vm, result);
    if (!ok && err) {
        a3_zero_struct(err);
        a3_strcpy(err->message, sizeof(err->message), vm->message[0] ? vm->message : "script error");
        a3_strcpy(err->hint, sizeof(err->hint), vm->hint);
        if (vm->nframes) {
            Frame *fr = &vm->frames[vm->nframes - 1];
            u32 at = vm->op_ip < m->lines.count ? vm->op_ip : 0;
            err->line = m->lines.count ? m->lines.data[at] : 0;
            a3_strcpy(err->function, sizeof(err->function), func_name(m, fr->func));
        }
    }
    vm_release(vm);
    return ok;
}

/* ======================================================================== */
/* Instances                                                                */
/* ======================================================================== */

A3SInstance *a3s_instance_create(A3SModule *m, A3SValue self, void *user, A3SError *err) {
    if (err) a3_zero_struct(err);
    if (!m) return 0;
    A3SInstance *inst = A3_NEW(A3SInstance, A3_MEM_SCRIPT);
    if (!inst) return 0;
    inst->globals = (A3SValue *)a3_calloc(sizeof(A3SValue) * (m->global_count + 1), A3_MEM_SCRIPT);
    if (!inst->globals) { a3_free(inst); return 0; }
    inst->module = m;
    a3s_module_retain(m);
    inst->self = self;
    a3s_retain(&inst->self);
    inst->user = user;
    if (!a3s_vm_run(inst, 0, 0, 0, 0, err)) { a3s_instance_destroy(inst); return 0; }
    return inst;
}

void a3s_instance_destroy(A3SInstance *inst) {
    if (!inst) return;
    for (u32 i = 0; i < inst->module->global_count; ++i) a3s_release(&inst->globals[i]);
    a3s_release(&inst->self);
    a3_free(inst->globals);
    a3s_module_release(inst->module);
    a3_free(inst);
}

void *a3s_instance_user(A3SInstance *inst) { return inst ? inst->user : 0; }
A3SModule *a3s_instance_module(A3SInstance *inst) { return inst ? inst->module : 0; }

b32 a3s_call(A3SInstance *inst, const char *fn, const A3SValue *args, u32 argc, A3SValue *result, b32 optional, A3SError *err) {
    if (err) a3_zero_struct(err);
    if (result) *result = a3s_nil();
    if (!inst) return 0;
    A3SModule *m = inst->module;
    for (u32 i = 1; i < m->funcs.count; ++i)
        if (a3_streq(m->funcs.data[i].name, fn)) return a3s_vm_run(inst, i, args, argc, result, err);
    if (optional) return 1;
    if (err) {
        a3_snprintf(err->message, sizeof(err->message), "the script has no function '%s'", fn);
        const char *cands[256];
        u32 n = 0;
        for (u32 i = 1; i < m->funcs.count && n < 256; ++i) cands[n++] = m->funcs.data[i].name;
        a3s_suggest(fn, cands, n, err->hint, sizeof(err->hint));
    }
    return 0;
}

u32 a3s_instance_global_count(A3SInstance *inst) { return inst ? inst->module->global_count : 0; }
const char *a3s_instance_global_name(A3SInstance *inst, u32 i) { return inst && i < inst->module->global_count ? inst->module->global_names[i] : ""; }
const A3SValue *a3s_instance_global(A3SInstance *inst, u32 i) { return inst && i < inst->module->global_count ? &inst->globals[i] : 0; }

b32 a3s_instance_set_global(A3SInstance *inst, const char *name, const A3SValue *v) {
    if (!inst) return 0;
    for (u32 i = 0; i < inst->module->global_count; ++i) {
        if (a3_streq(inst->module->global_names[i], name)) {
            a3s_release(&inst->globals[i]);
            inst->globals[i] = *v;
            a3s_retain(&inst->globals[i]);
            return 1;
        }
    }
    return 0;
}
