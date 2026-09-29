/*
 * ASM3D - a3_physics_kernels.h
 * Hot loops of the physics engine. Desktop x86-64 builds use hand-written
 * SSE assembly (a3_physics_x64.S); other targets use the C reference
 * (a3_physics_ref.c). Both perform exactly the same IEEE operations in the
 * same order, so simulations are bit-identical across implementations
 * (verified by tests/test_physics_kernels.c).
 *
 * Conventions: every vector is an A3Vec4 with w = 0 unless noted. Body index
 * 0 is the static world: zero velocity, zero inverse mass, never modified.
 */
#ifndef A3_PHYSICS_KERNELS_H
#define A3_PHYSICS_KERNELS_H

#include "../core/a3_base.h"
#include "../core/a3_math.h"

A3_EXTERN_C_BEGIN

/* One solver row (normal or friction direction of a contact). 128 bytes. */
typedef struct A3_ALIGNAS(16) A3SolverRow {
    A3Vec4 lin;        /* impulse direction n (applied +n to B, -n to A) */
    A3Vec4 ang_a;      /* ra x n */
    A3Vec4 ang_b;      /* rb x n */
    A3Vec4 ia;         /* Ia^-1 (ra x n), world space */
    A3Vec4 ib;         /* Ib^-1 (rb x n) */
    f32 inv_mass_a, inv_mass_b, eff_mass, bias;
    f32 acc, lo, hi, mu;
    i32 body_a, body_b;
    i32 friction_of;   /* -1: normal row; otherwise index of the normal row bounding this friction row */
    i32 pad;
} A3SolverRow;

A3_STATIC_ASSERT(sizeof(A3SolverRow) == 128, "A3SolverRow layout is shared with assembly");

/* v = (v + accel*dt) * damp.x ; w = w * damp.y */
void a3_phys_integrate_velocities(A3Vec4 *lin_vel, A3Vec4 *ang_vel, const A3Vec4 *accel, const A3Vec4 *damp, f32 dt, u32 count);
/* p += v*dt ; q = normalize(q + 0.5*dt*(w (x) q)) */
void a3_phys_integrate_transforms(A3Vec4 *pos, A3Quat *rot, const A3Vec4 *lin_vel, const A3Vec4 *ang_vel, f32 dt, u32 count);
/* One Gauss-Seidel sweep over all rows (call once per solver iteration). */
void a3_phys_solve_rows(A3SolverRow *rows, u32 count, A3Vec4 *lin_vel, A3Vec4 *ang_vel);
/* World AABBs of oriented boxes: ext = |c0|*h.x + |c1|*h.y + |c2|*h.z.
 * cols holds 3 rotation columns per body (scaled rotation allowed). */
void a3_phys_compute_aabbs(const A3Vec4 *centers, const A3Vec4 *cols, const A3Vec4 *half, A3Vec4 *out_min, A3Vec4 *out_max, u32 count);
/* Sweep-and-prune over boxes sorted by min.x. Writes overlapping index pairs
 * (i < j in sorted order) and returns the number found; stops at max_pairs
 * (returned value == max_pairs means the buffer may have been too small). */
u32  a3_phys_sap_pairs(const A3Vec4 *mins, const A3Vec4 *maxs, u32 count, u32 *out_pairs, u32 max_pairs);
/* Ray vs many AABBs. out_t[i] = entry distance or -1 on miss. Returns hits. */
u32  a3_phys_ray_aabbs(const A3Vec4 *origin, const A3Vec4 *inv_dir, f32 t_max, const A3Vec4 *mins, const A3Vec4 *maxs, u32 count, f32 *out_t);

const char *a3_phys_kernel_backend(void);

/* C reference versions (always compiled; tests compare against them). */
void a3_phys_ref_integrate_velocities(A3Vec4 *lin_vel, A3Vec4 *ang_vel, const A3Vec4 *accel, const A3Vec4 *damp, f32 dt, u32 count);
void a3_phys_ref_integrate_transforms(A3Vec4 *pos, A3Quat *rot, const A3Vec4 *lin_vel, const A3Vec4 *ang_vel, f32 dt, u32 count);
void a3_phys_ref_solve_rows(A3SolverRow *rows, u32 count, A3Vec4 *lin_vel, A3Vec4 *ang_vel);
void a3_phys_ref_compute_aabbs(const A3Vec4 *centers, const A3Vec4 *cols, const A3Vec4 *half, A3Vec4 *out_min, A3Vec4 *out_max, u32 count);
u32  a3_phys_ref_sap_pairs(const A3Vec4 *mins, const A3Vec4 *maxs, u32 count, u32 *out_pairs, u32 max_pairs);
u32  a3_phys_ref_ray_aabbs(const A3Vec4 *origin, const A3Vec4 *inv_dir, f32 t_max, const A3Vec4 *mins, const A3Vec4 *maxs, u32 count, f32 *out_t);

A3_EXTERN_C_END

#endif
