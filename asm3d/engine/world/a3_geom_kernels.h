/*
 * ASM3D - a3_geom_kernels.h
 * Geometry kernels of the procedural model builder (cars, palms, people,
 * street furniture): x86-64 SSE assembly (a3_geom_x64.S) with C references
 * (a3_geom_ref.c) that give bit-identical results. The C versions run on
 * other CPUs (WebAssembly) and are used to test the assembly.
 *
 * Vertices use the engine layout A3Vertex { position, normal, uv } (32 bytes).
 */
#ifndef A3_GEOM_KERNELS_H
#define A3_GEOM_KERNELS_H

#include "../core/a3_base.h"
#include "../core/a3_math.h"
#include "../render/a3_mesh.h"

A3_EXTERN_C_BEGIN

/* Catmull-Rom samples through `n` control points, `sub` samples per segment.
 * closed: n * sub samples (wraps around). open: (n - 1) * sub + 1 samples,
 * end tangents from clamped neighbours. Per component, with t = k / sub:
 *   a = p1 * 2, b = p2 - p0, c = ((p0*2 - p1*5) + p2*4) - p3,
 *   d = ((p1*3 - p0) - p2*3) + p3, r = (((a + b*t) + c*t*t) + d*(t*t*t)) * 0.5
 * Returns the number of samples written. */
u32  a3_gk_catmull_rom(const A3Vec2 *ctrl, u32 n, u32 sub, u32 closed, A3Vec2 *out);
u32  a3_gk_ref_catmull_rom(const A3Vec2 *ctrl, u32 n, u32 sub, u32 closed, A3Vec2 *out);

/* 2D points placed in a 3D frame: p = (origin + axis_a * x) + axis_b * y.
 * frame = { ox, oy, oz, 0, ax, ay, az, 0, bx, by, bz, 0 }. Writes the 12-byte
 * position at dst + i * stride (dst usually &vertex.position). */
void a3_gk_frame_points(const A3Vec2 *pts, u32 count, const f32 *frame, void *dst, u32 stride);
void a3_gk_ref_frame_points(const A3Vec2 *pts, u32 count, const f32 *frame, void *dst, u32 stride);

/* Triangle indices of a grid of `rows` rings of `k` vertices starting at
 * `base`: quad (a, b, c, d) -> (a, b, c), (a, c, d); the ring wraps when
 * `wrap`. Returns the number of indices written. */
u32  a3_gk_grid_indices(u32 rows, u32 k, u32 wrap, u32 base, u32 *out);
u32  a3_gk_ref_grid_indices(u32 rows, u32 k, u32 wrap, u32 base, u32 *out);

/* Adds cross(b - a, c - a) of every triangle to the normals of its three
 * vertices (in the order a, b, c). */
void a3_gk_vertex_normals(A3Vertex *v, const u32 *idx, u32 tri_count);
void a3_gk_ref_vertex_normals(A3Vertex *v, const u32 *idx, u32 tri_count);

/* n = n / sqrt((x*x + y*y) + z*z), or (0, 1, 0) when that is <= 1e-12. */
void a3_gk_normalize(A3Vertex *v, u32 count);
void a3_gk_ref_normalize(A3Vertex *v, u32 count);

/* Six times the signed volume enclosed by the triangles: the sum over
 * triangles of dot(a, cross(b, c)), accumulated in order (f32). Positive
 * when the winding is outward. */
f32  a3_gk_signed_volume(const A3Vertex *v, const u32 *idx, u32 tri_count);
f32  a3_gk_ref_signed_volume(const A3Vertex *v, const u32 *idx, u32 tri_count);

/* Walk / run pose of n people. For person i with phase ph and amplitude a,
 * S = sin4(ph, ph + 0.6, ph + pi/2) with the kernel's own sine (a3_gk_sin4),
 * and out[i*10 + 0..9] =
 *   thigh L, thigh R, shin L, shin R, arm L, arm R, forearm L, forearm R
 *   (radians about +X; + swings forward), body bob (m), hip twist (radians). */
void a3_gk_walk_pose(const f32 *phase, const f32 *amp, u32 n, f32 *out);
void a3_gk_ref_walk_pose(const f32 *phase, const f32 *amp, u32 n, f32 *out);

/* The kernels' sine (4 lanes): range reduction to [-pi, pi] with
 * k = trunc(x / 2pi + copysign(0.5, x)), folding to [-pi/2, pi/2] and an odd
 * degree-11 polynomial. Max error about 1e-6. */
void a3_gk_ref_sin4(const f32 *in4, f32 *out4);

const char *a3_gk_backend(void);

A3_EXTERN_C_END

#endif
