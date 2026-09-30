/*
 * ASM3D - a3_vehicle_x64.S
 * Wheel kernel of the realistic vehicle model (see a3_vehicle_kernels.h):
 * suspension load, slip angle, Pacejka-style lateral force, friction
 * circle and slip amount for the 4 wheels of a car, one SSE lane per wheel.
 * Same operation order as a3_vehicle_ref.c: bit-identical results.
 *
 * A3WheelQuad (16-byte aligned, 208 bytes): comp @0, comp_vel @16,
 * stiffness @32, damping @48, v_long @64, v_lat @80, mu @96, drive @112,
 * lock @128, load @144, fx @160, fy @176, slip @192.
 */
#include "../core/a3_asm_abi.h"

    .intel_syntax noprefix

    .section .rodata
    .p2align 4
V_SIGN:  .long 0x80000000, 0x80000000, 0x80000000, 0x80000000
V_ABS:   .long 0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff
V_ONE:   .float 1.0, 1.0, 1.0, 1.0
V_THREE: .float 3.0, 3.0, 3.0, 3.0
V_K16:   .float 16.0, 16.0, 16.0, 16.0
V_K135:  .float 1.35, 1.35, 1.35, 1.35
V_K09:   .float 0.9, 0.9, 0.9, 0.9
V_K04:   .float 0.4, 0.4, 0.4, 0.4
V_K05:   .float 0.5, 0.5, 0.5, 0.5
V_K012:  .float 0.12, 0.12, 0.12, 0.12
V_K5:    .float 5.0, 5.0, 5.0, 5.0
V_A11:   .float -0.01172120, -0.01172120, -0.01172120, -0.01172120
V_A9:    .float 0.05265332, 0.05265332, 0.05265332, 0.05265332
V_A7:    .float -0.11643287, -0.11643287, -0.11643287, -0.11643287
V_A5:    .float 0.19354346, 0.19354346, 0.19354346, 0.19354346
V_A3:    .float -0.33262347, -0.33262347, -0.33262347, -0.33262347
V_A1:    .float 0.99997726, 0.99997726, 0.99997726, 0.99997726
V_INV2PI: .float 0.159154943, 0.159154943, 0.159154943, 0.159154943
V_2PI:   .float 6.28318548, 6.28318548, 6.28318548, 6.28318548
V_PI:    .float 3.14159274, 3.14159274, 3.14159274, 3.14159274
V_HPI:   .float 1.57079637, 1.57079637, 1.57079637, 1.57079637
V_NHPI:  .float -1.57079637, -1.57079637, -1.57079637, -1.57079637
V_NPI:   .float -3.14159274, -3.14159274, -3.14159274, -3.14159274
V_HALF:  .float 0.5, 0.5, 0.5, 0.5
V_NHALF: .float -0.5, -0.5, -0.5, -0.5
V_C11:   .float -2.50521084e-8, -2.50521084e-8, -2.50521084e-8, -2.50521084e-8
V_C9:    .float 2.75573192e-6, 2.75573192e-6, 2.75573192e-6, 2.75573192e-6
V_C7:    .float -1.98412698e-4, -1.98412698e-4, -1.98412698e-4, -1.98412698e-4
V_C5:    .float 8.33333333e-3, 8.33333333e-3, 8.33333333e-3, 8.33333333e-3
V_C3:    .float -1.66666667e-1, -1.66666667e-1, -1.66666667e-1, -1.66666667e-1

    .text

/* xmm0 = atan(xmm0), 4 lanes; clobbers xmm1-xmm5 */
.macro ATAN4
    movaps  xmm5, xmm0
    andps   xmm5, xmmword ptr [rip + V_SIGN]        /* sign of x */
    andps   xmm0, xmmword ptr [rip + V_ABS]         /* a = |x| */
    movaps  xmm1, xmmword ptr [rip + V_ONE]
    divps   xmm1, xmm0                              /* r = 1 / a */
    movaps  xmm2, xmmword ptr [rip + V_ONE]
    cmpltps xmm2, xmm0                              /* big = 1 < a */
    movaps  xmm3, xmm2
    andps   xmm3, xmm1
    movaps  xmm4, xmm2
    andnps  xmm4, xmm0
    orps    xmm3, xmm4                              /* t = big ? r : a */
    movaps  xmm1, xmm3
    mulps   xmm1, xmm3                              /* t2 */
    movaps  xmm4, xmm1
    mulps   xmm4, xmmword ptr [rip + V_A11]
    addps   xmm4, xmmword ptr [rip + V_A9]
    mulps   xmm4, xmm1
    addps   xmm4, xmmword ptr [rip + V_A7]
    mulps   xmm4, xmm1
    addps   xmm4, xmmword ptr [rip + V_A5]
    mulps   xmm4, xmm1
    addps   xmm4, xmmword ptr [rip + V_A3]
    mulps   xmm4, xmm1
    addps   xmm4, xmmword ptr [rip + V_A1]
    mulps   xmm4, xmm3                              /* p */
    movaps  xmm0, xmmword ptr [rip + V_HPI]
    subps   xmm0, xmm4                              /* h = pi/2 - p */
    andps   xmm0, xmm2
    andnps  xmm2, xmm4
    orps    xmm0, xmm2                              /* big ? h : p */
    xorps   xmm0, xmm5                              /* sign back */
.endm

/* xmm0 = sin(xmm0), 4 lanes (same polynomial as the geometry kernels);
 * clobbers xmm1-xmm4 */
.macro SIN4
    movaps  xmm1, xmm0
    mulps   xmm1, xmmword ptr [rip + V_INV2PI]      /* q = x / 2pi */
    movaps  xmm3, xmm1
    xorps   xmm4, xmm4
    cmpltps xmm3, xmm4                              /* q < 0 */
    movaps  xmm4, xmm3
    andps   xmm4, xmmword ptr [rip + V_NHALF]
    andnps  xmm3, xmmword ptr [rip + V_HALF]
    orps    xmm3, xmm4                              /* q < 0 ? -0.5 : 0.5 */
    addps   xmm1, xmm3
    cvttps2dq xmm1, xmm1
    cvtdq2ps xmm1, xmm1                             /* k */
    mulps   xmm1, xmmword ptr [rip + V_2PI]
    subps   xmm0, xmm1                              /* r = x - k*2pi */
    movaps  xmm1, xmmword ptr [rip + V_HPI]
    cmpltps xmm1, xmm0                              /* r > pi/2 */
    movaps  xmm2, xmm0
    cmpltps xmm2, xmmword ptr [rip + V_NHPI]        /* r < -pi/2 */
    movaps  xmm3, xmmword ptr [rip + V_PI]
    subps   xmm3, xmm0
    movaps  xmm4, xmmword ptr [rip + V_NPI]
    subps   xmm4, xmm0
    andps   xmm3, xmm1
    andps   xmm4, xmm2
    orps    xmm1, xmm2
    andnps  xmm1, xmm0
    orps    xmm1, xmm3
    orps    xmm1, xmm4                              /* folded r */
    movaps  xmm0, xmm1
    movaps  xmm2, xmm0
    mulps   xmm2, xmm0                              /* s = r*r */
    movaps  xmm3, xmm2
    mulps   xmm3, xmmword ptr [rip + V_C11]
    addps   xmm3, xmmword ptr [rip + V_C9]
    mulps   xmm3, xmm2
    addps   xmm3, xmmword ptr [rip + V_C7]
    mulps   xmm3, xmm2
    addps   xmm3, xmmword ptr [rip + V_C5]
    mulps   xmm3, xmm2
    addps   xmm3, xmmword ptr [rip + V_C3]
    mulps   xmm3, xmm2
    addps   xmm3, xmmword ptr [rip + V_ONE]
    mulps   xmm3, xmm0
    movaps  xmm0, xmm3
.endm

/* void a3_vk_asm_wheels(A3WheelQuad *quads, u32 count)
 *   rdi quads, esi count */
A3_ASM_FUNC(a3_vk_asm_wheels, ARGS_PPPP)
1:
    test    esi, esi
    jz      2f
    /* suspension load */
    movaps  xmm8, xmmword ptr [rdi + 0]             /* comp */
    movaps  xmm0, xmmword ptr [rdi + 32]
    mulps   xmm0, xmm8                              /* stiffness * comp */
    movaps  xmm1, xmmword ptr [rdi + 48]
    mulps   xmm1, xmmword ptr [rdi + 16]            /* damping * comp_vel */
    addps   xmm0, xmm1
    xorps   xmm2, xmm2
    maxps   xmm0, xmm2                              /* mx(ld, 0) */
    xorps   xmm9, xmm9
    cmpltps xmm9, xmm8                              /* ground = 0 < comp */
    andps   xmm0, xmm9
    movaps  xmmword ptr [rdi + 144], xmm0           /* load */
    mulps   xmm0, xmmword ptr [rdi + 96]            /* limit = load * mu */
    movaps  xmm10, xmm0
    /* slip angle */
    movaps  xmm11, xmmword ptr [rdi + 64]           /* v_long */
    movaps  xmm1, xmm11
    andps   xmm1, xmmword ptr [rip + V_ABS]
    maxps   xmm1, xmmword ptr [rip + V_THREE]       /* den = mx(|vl|, 3) */
    movaps  xmm0, xmmword ptr [rdi + 80]
    divps   xmm0, xmm1                              /* v_lat / den */
    ATAN4
    movaps  xmm12, xmm0                             /* alpha */
    /* lateral force curve */
    mulps   xmm0, xmmword ptr [rip + V_K16]
    ATAN4
    mulps   xmm0, xmmword ptr [rip + V_K135]
    SIN4
    mulps   xmm0, xmm10                             /* sy * limit */
    xorps   xmm0, xmmword ptr [rip + V_SIGN]
    movaps  xmm13, xmm0                             /* fy0 */
    /* longitudinal request (locked wheels slide against the motion) */
    movaps  xmm1, xmm10
    mulps   xmm1, xmmword ptr [rip + V_K09]         /* lk = limit * 0.9 */
    movaps  xmm2, xmm11
    andps   xmm2, xmmword ptr [rip + V_SIGN]
    orps    xmm1, xmm2
    xorps   xmm1, xmmword ptr [rip + V_SIGN]        /* -copysign(lk, vl) */
    movaps  xmm3, xmmword ptr [rip + V_K05]
    cmpltps xmm3, xmmword ptr [rdi + 128]           /* locked = 0.5 < lock */
    movaps  xmm14, xmm3
    movaps  xmm4, xmm3
    andps   xmm4, xmm1
    andnps  xmm3, xmmword ptr [rdi + 112]
    orps    xmm3, xmm4                              /* req */
    movaps  xmm15, xmm3
    movaps  xmm5, xmm10
    xorps   xmm5, xmmword ptr [rip + V_SIGN]        /* -limit */
    movaps  xmm0, xmm3
    maxps   xmm0, xmm5
    minps   xmm0, xmm10                             /* fx */
    movaps  xmmword ptr [rdi + 160], xmm0
    /* grip left for cornering */
    movaps  xmm1, xmm10
    mulps   xmm1, xmm10                             /* limit * limit */
    mulps   xmm0, xmm0                              /* fx * fx */
    subps   xmm1, xmm0
    xorps   xmm2, xmm2
    maxps   xmm1, xmm2
    sqrtps  xmm1, xmm1                              /* cap */
    movaps  xmm2, xmm13
    mulps   xmm2, xmmword ptr [rip + V_K04]         /* fy0 * 0.4 */
    andps   xmm2, xmm14
    movaps  xmm3, xmm14
    andnps  xmm3, xmm13
    orps    xmm3, xmm2                              /* locked ? fy0*0.4 : fy0 */
    movaps  xmm4, xmm1
    xorps   xmm4, xmmword ptr [rip + V_SIGN]        /* -cap */
    maxps   xmm3, xmm4
    minps   xmm3, xmm1                              /* fy */
    movaps  xmmword ptr [rdi + 176], xmm3
    /* slip amount */
    movaps  xmm0, xmm15
    andps   xmm0, xmmword ptr [rip + V_ABS]
    subps   xmm0, xmm10                             /* |req| - limit */
    movaps  xmm1, xmm10
    maxps   xmm1, xmmword ptr [rip + V_ONE]
    divps   xmm0, xmm1                              /* s1 */
    movaps  xmm2, xmm12
    andps   xmm2, xmmword ptr [rip + V_ABS]
    subps   xmm2, xmmword ptr [rip + V_K012]
    mulps   xmm2, xmmword ptr [rip + V_K5]          /* s2 */
    maxps   xmm0, xmm2
    maxps   xmm0, xmmword ptr [rdi + 128]           /* mx(s, lock) */
    xorps   xmm1, xmm1
    maxps   xmm0, xmm1
    minps   xmm0, xmmword ptr [rip + V_ONE]
    andps   xmm0, xmm9
    movaps  xmmword ptr [rdi + 192], xmm0           /* slip */
    add     rdi, 208
    dec     esi
    jmp     1b
2:
    ret

A3_ASM_END_FILE
