/*
 * ASM3D - a3_vehicle_kernels.h
 * Wheel kernel of the realistic vehicle model: suspension load and tire
 * forces for the four wheels of a car in one pass (one SSE lane per wheel).
 * x86-64 builds use assembly (a3_vehicle_x64.S); the C reference
 * (a3_vehicle_ref.c) has the same operation order and gives bit-identical
 * results (used on other CPUs, e.g. WebAssembly, and by the tests).
 *
 * Per wheel (lane), with mx(a, b) = a > b ? a : b and mn(a, b) = a < b ? a : b:
 *   load  = 0 < comp ? mx(stiffness*comp + damping*comp_vel, 0) : 0
 *   limit = load * mu                                (friction circle radius)
 *   alpha = atan(v_lat / mx(|v_long|, 3))            (slip angle)
 *   fy0   = -(limit * sin(1.35 * atan(16 * alpha)))  (Pacejka-style curve:
 *           peak near 8 degrees, 85% when fully sliding)
 *   req   = lock > 0.5 ? -copysign(limit * 0.9, v_long) : drive
 *   fx    = mn(mx(req, -limit), limit)
 *   cap   = sqrt(mx(limit*limit - fx*fx, 0))         (grip left for cornering)
 *   fy    = mn(mx(lock > 0.5 ? fy0 * 0.4 : fy0, -cap), cap)
 *   slip  = 0 < comp ? clamp01(mx(mx((|req| - limit) / mx(limit, 1),
 *                                      (|alpha| - 0.12) * 5), lock)) : 0
 * atan and sin are the kernels' own polynomials (a3_vk_ref_atan4 /
 * a3_vk_ref_sin4), so the C and assembly versions agree to the bit.
 */
#ifndef A3_VEHICLE_KERNELS_H
#define A3_VEHICLE_KERNELS_H

#include "../core/a3_base.h"

A3_EXTERN_C_BEGIN

typedef struct A3_ALIGNAS(16) A3WheelQuad {
    /* inputs, one lane per wheel (FL, FR, RL, RR) */
    f32 comp[4];        /* suspension compression (m); <= 0: wheel in the air */
    f32 comp_vel[4];    /* compression speed (m/s, + = compressing) */
    f32 stiffness[4];   /* spring (N/m) */
    f32 damping[4];     /* damper (N s/m) */
    f32 v_long[4];      /* contact patch speed along the wheel (m/s) */
    f32 v_lat[4];       /* contact patch speed sideways (m/s, + = wheel right) */
    f32 mu[4];          /* tire friction coefficient */
    f32 drive[4];       /* requested longitudinal force (N): engine - brakes */
    f32 lock[4];        /* 1 = wheel locked (handbrake), 0 = rolling */
    /* outputs */
    f32 load[4];        /* normal force (N) */
    f32 fx[4];          /* longitudinal tire force (N) */
    f32 fy[4];          /* lateral tire force (N) */
    f32 slip[4];        /* 0 = gripping .. 1 = sliding (skid sound, smoke) */
} A3WheelQuad;

void a3_vk_wheels(A3WheelQuad *quads, u32 count);
void a3_vk_ref_wheels(A3WheelQuad *quads, u32 count);

/* The kernels' atan (|error| < 2e-7 rad) and sine, 4 lanes. */
void a3_vk_ref_atan4(const f32 *in4, f32 *out4);
void a3_vk_ref_sin4(const f32 *in4, f32 *out4);

const char *a3_vk_backend(void);

A3_EXTERN_C_END

#endif
