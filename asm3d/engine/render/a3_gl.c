/*
 * ASM3D - a3_gl.c
 */
#include "a3_gl.h"
#include "../core/a3_log.h"

#define A3_GL_DEFINE(ret, name, params) PFN_##name a3_##name;
A3_GL_FUNCTIONS(A3_GL_DEFINE)
#undef A3_GL_DEFINE

int a3_gl_load(A3GLGetProcFn get_proc) {
    int missing = 0;
    if (!get_proc) return -1;
#define A3_GL_LOAD(ret, name, params) \
    a3_##name = (PFN_##name)get_proc(#name); \
    if (!a3_##name) { A3_ERROR("gl", "missing OpenGL function %s", #name); missing++; }
    A3_GL_FUNCTIONS(A3_GL_LOAD)
#undef A3_GL_LOAD
    return missing;
}
