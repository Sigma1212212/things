/*
 * ASM3D - a3_emesh_ops.h
 * Modeling operations by name: "mode:face", "select-normal:0,1,0", "extrude:1"...
 */
#ifndef A3_EMESH_OPS_H
#define A3_EMESH_OPS_H

#include "a3_emesh.h"

A3_EXTERN_C_BEGIN

typedef struct A3EMeshOpInfo { const char *name; const char *args; const char *doc; } A3EMeshOpInfo;
typedef struct A3EMeshOpError { char message[200]; char hint[200]; u32 merged; } A3EMeshOpError;

/* Runs one operation ("name" or "name:arg1,arg2"). False with a message (and a
 * "did you mean" hint for unknown names) when it cannot be applied. */
b32 a3_emesh_run_op(A3EMesh *m, const char *spec, A3EMeshOpError *err);
u32 a3_emesh_op_count(void);
const A3EMeshOpInfo *a3_emesh_op_info(u32 i);

A3_EXTERN_C_END

#endif
