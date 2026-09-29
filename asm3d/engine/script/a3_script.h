/*
 * ASM3D - a3_script.h
 * A3Script: the ASM3D scripting language.
 *
 *   let speed = 4                      // top-level lets: per-object state
 *   fn on_update(dt) {
 *       self.Transform.position.x += speed * dt
 *       if key_down("space") and self.position.y < 1 { jump() }
 *   }
 *
 * Source is compiled to bytecode for a small stack VM. Scripts are
 * sandboxed: they can only call the functions registered here, every call
 * has an instruction budget (an endless loop stops that script, not the
 * game), and errors report the line plus a plain-language explanation with
 * "did you mean" suggestions. Strings and lists are reference counted.
 *
 * The core language (this header) knows nothing about the ECS; the engine
 * binds entities and components through A3SHost (a3_script_engine.c).
 */
#ifndef A3_SCRIPT_H
#define A3_SCRIPT_H

#include "../core/a3_base.h"
#include "../core/a3_math.h"

A3_EXTERN_C_BEGIN

typedef enum A3SType {
    A3S_NIL = 0, A3S_BOOL, A3S_NUM, A3S_VEC3, A3S_STR, A3S_LIST, A3S_FUNC, A3S_NATIVE, A3S_ENTITY, A3S_COMP, A3S_TYPE_COUNT
} A3SType;

typedef struct A3SStr { u32 refs; u32 len; char chars[1]; } A3SStr;
typedef struct A3SValue A3SValue;
typedef struct A3SList { u32 refs; u32 count, cap; A3SValue *items; } A3SList;

struct A3SValue {
    u32 type;       /* A3SType */
    u32 aux;        /* A3S_COMP: component type id; A3S_FUNC/NATIVE: index */
    union {
        f64 num;
        b32 b;
        f32 v[3];
        A3SStr *str;
        A3SList *list;
        u64 guid;   /* A3S_ENTITY / A3S_COMP */
    } as;
};

typedef struct A3SError {
    i32 line;
    i32 col;
    char message[256];
    char hint[192];
    char function[48];  /* function running when a runtime error happened */
} A3SError;

typedef struct A3SModule A3SModule;
typedef struct A3SInstance A3SInstance;
typedef struct A3SVM A3SVM;

/* ---- values ---- */
A3SValue a3s_nil(void);
A3SValue a3s_bool(b32 b);
A3SValue a3s_num(f64 n);
A3SValue a3s_vec3(f32 x, f32 y, f32 z);
A3SValue a3s_str(const char *s);                 /* new string, refs = 1 */
A3SValue a3s_str_n(const char *s, usize n);
A3SValue a3s_list(u32 capacity);
A3SValue a3s_entity(u64 guid);
A3SValue a3s_comp(u64 guid, u32 type_id);
void a3s_retain(const A3SValue *v);
void a3s_release(A3SValue *v);                   /* sets v to nil */
b32  a3s_truthy(const A3SValue *v);
b32  a3s_equal(const A3SValue *a, const A3SValue *b);
b32  a3s_list_push(A3SValue *list, const A3SValue *item); /* retains item */
/* Human readable form ("12", "3.5", "(1, 2, 3)", "hello"...) into buf. */
void a3s_to_string(const A3SValue *v, char *buf, usize cap);
const char *a3s_type_name(u32 type);

/* ---- natives (standard library and engine API) ---- */
typedef b32 (*A3SNativeFn)(A3SVM *vm, A3SValue *args, u32 argc, A3SValue *result);
typedef struct A3SNative {
    const char *name;
    A3SNativeFn fn;
    i32 min_args, max_args;   /* max -1 = any */
    const char *category;     /* for the documentation */
    const char *signature;    /* e.g. "clamp(x, lo, hi)" */
    const char *doc;
} A3SNative;
/* Registers a native (the table is copied). Re-registering a name replaces it. */
void a3s_register(const A3SNative *n);
u32  a3s_native_count(void);
const A3SNative *a3s_native_get(u32 i);
/* Registers the core library (math, strings, lists, print). Idempotent. */
void a3s_register_core_lib(void);
/* Inside a native: report an error ("expected a number" ...). Always returns false. */
b32  a3s_fail(A3SVM *vm, const char *fmt, ...) A3_PRINTF_LIKE(2, 3);
/* Replaces the hint of the error just reported with a3s_fail (e.g. a suggestion). */
void a3s_set_error_hint(A3SVM *vm, const char *hint);
A3SInstance *a3s_vm_instance(A3SVM *vm);
/* Argument helpers: return false (with an error) when the type is wrong. */
b32  a3s_arg_num(A3SVM *vm, A3SValue *args, u32 i, f64 *out);
b32  a3s_arg_vec3(A3SVM *vm, A3SValue *args, u32 i, A3Vec3 *out);
b32  a3s_arg_str(A3SVM *vm, A3SValue *args, u32 i, const char **out);

/* ---- host bindings (entities and components) ---- */
typedef struct A3SHost {
    /* obj.name  (obj is an entity or component value) */
    b32 (*get_field)(A3SVM *vm, const A3SValue *obj, const char *name, A3SValue *out);
    /* obj.name = v; may modify *obj for value semantics */
    b32 (*set_field)(A3SVM *vm, A3SValue *obj, const char *name, const A3SValue *v);
    /* print() output */
    void (*print)(A3SInstance *inst, const char *text);
} A3SHost;
void a3s_set_host(const A3SHost *host);

/* ---- compile & run ---- */
A3SModule *a3s_compile(const char *name, const char *source, usize len, A3SError *err); /* NULL on error */
void a3s_module_retain(A3SModule *m);
void a3s_module_release(A3SModule *m);
const char *a3s_module_name(const A3SModule *m);
b32  a3s_module_has_function(const A3SModule *m, const char *fn);
u32  a3s_module_function_arity(const A3SModule *m, const char *fn);

/* Creates an instance (its own globals) and runs the top-level code.
 * `self` is the value of the `self` keyword (an entity, or nil). */
A3SInstance *a3s_instance_create(A3SModule *m, A3SValue self, void *user, A3SError *err);
void a3s_instance_destroy(A3SInstance *inst);
void *a3s_instance_user(A3SInstance *inst);
A3SModule *a3s_instance_module(A3SInstance *inst);
/* Calls a script function. Missing functions are not an error (returns true,
 * result nil) when `optional` is set. */
b32  a3s_call(A3SInstance *inst, const char *fn, const A3SValue *args, u32 argc, A3SValue *result, b32 optional, A3SError *err);
u32  a3s_instance_global_count(A3SInstance *inst);
const char *a3s_instance_global_name(A3SInstance *inst, u32 i);
const A3SValue *a3s_instance_global(A3SInstance *inst, u32 i);
b32  a3s_instance_set_global(A3SInstance *inst, const char *name, const A3SValue *v);
/* Instruction budget per call (default 5,000,000). */
void a3s_set_budget(u64 instructions);
/* Live objects (strings + lists), for leak tests. */
i64  a3s_live_objects(void);

A3_EXTERN_C_END

#endif
