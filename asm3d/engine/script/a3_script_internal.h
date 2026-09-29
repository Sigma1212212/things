/*
 * ASM3D - a3_script_internal.h (shared by the compiler and the VM)
 */
#ifndef A3_SCRIPT_INTERNAL_H
#define A3_SCRIPT_INTERNAL_H

#include "a3_script.h"
#include "../core/a3_memory.h"

#define A3S_MAX_LOCALS 200
#define A3S_MAX_FRAMES 128
#define A3S_STACK 2048
#define A3S_NAME 48

typedef enum A3SOp {
    OP_CONST = 0,     /* u16 const */
    OP_NIL, OP_TRUE, OP_FALSE, OP_SELF,
    OP_POP, OP_DUP, OP_SWAP, OP_ROT3,     /* ROT3: a b c -> b c a */
    OP_GET_LOCAL, OP_SET_LOCAL,           /* u16 slot (SET pops) */
    OP_GET_GLOBAL, OP_SET_GLOBAL,         /* u16 global */
    OP_GET_FIELD, OP_SET_FIELD,           /* u16 name const; SET: [obj v] -> [obj'] */
    OP_GET_INDEX, OP_SET_INDEX,           /* SET: [obj idx v] -> [obj'] */
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_NEG, OP_NOT,
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE,
    OP_JUMP, OP_JUMP_IF_FALSE,            /* u16 forward offset; JIF pops */
    OP_JUMP_IF_FALSE_KEEP, OP_JUMP_IF_TRUE_KEEP, /* and / or */
    OP_LOOP,                              /* u16 backward offset */
    OP_CALL,                              /* u8 argc */
    OP_RETURN,
    OP_LIST,                              /* u16 count */
    OP_ITER,                              /* u16 slot (hidden: iterable, index, var), u16 exit offset */
    OP_RANGE,                             /* u16 slot (hidden: end, var), u16 exit offset: numeric for */
    OP_COUNT
} A3SOp;

typedef struct A3SFunc {
    char name[A3S_NAME];
    u32 arity;
    u32 max_locals;
    u32 start;            /* offset in module code */
    i32 line;
} A3SFunc;

struct A3SModule {
    u32 refs;
    char name[128];
    A3_ARRAY_TYPE(u8) code;
    A3_ARRAY_TYPE(i32) lines;     /* line per code byte */
    A3_ARRAY_TYPE(A3SValue) consts;
    A3_ARRAY_TYPE(A3SFunc) funcs; /* funcs[0] = top-level code */
    char (*global_names)[A3S_NAME];
    u32 global_count;
};

struct A3SInstance {
    A3SModule *module;
    A3SValue *globals;
    A3SValue self;
    void *user;
};

/* VM entry used by a3s_instance_create / a3s_call */
b32 a3s_vm_run(A3SInstance *inst, u32 func, const A3SValue *args, u32 argc, A3SValue *result, A3SError *err);
i32 a3s_native_find(const char *name);
void a3s_suggest(const char *name, const char *const *candidates, u32 count, char *out, usize cap);
const A3SHost *a3s_host(void);
A3SValue a3s_str_concat(const A3SValue *a, const A3SValue *b);

#endif
