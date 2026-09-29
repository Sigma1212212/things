/*
 * ASM3D - a3_modeling_kernels.h
 * Hot loops of the modeling tools. x86-64 SSE assembly (a3_modeling_x64.S)
 * with C references (a3_modeling_ref.c) that give bit-identical results;
 * the C versions are used on other CPUs and to test the assembly.
 *
 * Positions are A3Vec4 with w = 1. Triangles are index triples.
 */
#ifndef A3_MODELING_KERNELS_H
#define A3_MODELING_KERNELS_H

#include "../core/a3_base.h"
#include "../core/a3_math.h"

A3_EXTERN_C_BEGIN

/* p.xyz = M * p (column-major 4x4, affine) for every point whose mask byte is
 * non-zero (mask NULL = all). p.w is left unchanged.
 * Order per component: ((m[c]*x + m[4+c]*y) + m[8+c]*z) + m[12+c]. */
void a3_mk_transform(A3Vec4 *pos, const u8 *mask, u32 count, const f32 *m16);
void a3_mk_ref_transform(A3Vec4 *pos, const u8 *mask, u32 count, const f32 *m16);

/* out[i] = cross(b - a, c - a) of triangle i (length = twice the area), w = 0. */
void a3_mk_tri_normals(const A3Vec4 *pos, const u32 *tris, u32 tri_count, A3Vec4 *out);
void a3_mk_ref_tri_normals(const A3Vec4 *pos, const u32 *tris, u32 tri_count, A3Vec4 *out);

/* Two-sided ray / triangle test (Moller-Trumbore). ray = { ox, oy, oz, 1, dx, dy, dz, 0 }.
 * *t_inout: in = maximum distance, out = nearest hit. Returns triangle index + 1, or 0. */
u32  a3_mk_raycast(const A3Vec4 *pos, const u32 *tris, u32 tri_count, const f32 *ray, f32 *t_inout);
u32  a3_mk_ref_raycast(const A3Vec4 *pos, const u32 *tris, u32 tri_count, const f32 *ray, f32 *t_inout);

/* Component-wise min / max of count >= 1 points. */
void a3_mk_bounds(const A3Vec4 *pos, u32 count, A3Vec4 *out_min, A3Vec4 *out_max);
void a3_mk_ref_bounds(const A3Vec4 *pos, u32 count, A3Vec4 *out_min, A3Vec4 *out_max);

const char *a3_mk_backend(void);

A3_EXTERN_C_END

#endif
