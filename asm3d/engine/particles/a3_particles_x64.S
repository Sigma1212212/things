/*
 * ASM3D - a3_particles_x64.S
 * Particle integration, one particle (two vec4) per iteration.
 * Same operation order as a3_particles_ref_integrate: bit-identical results.
 */
#include "../core/a3_asm_abi.h"

    .intel_syntax noprefix

    .section .rodata
    .p2align 4
K_XYZ:   .long 0xffffffff, 0xffffffff, 0xffffffff, 0
K_W:     .long 0, 0, 0, 0xffffffff
K_ONE_W: .float 0.0, 0.0, 0.0, 1.0

    .text

/* void a3_particles_asm_integrate(A3Vec4 *pos_age, A3Vec4 *vel_life, u32 count, const f32 *p)
 *   rdi pos_age, rsi vel_life, edx count, rcx p = { k, gx*dt, gy*dt, gz*dt, dt } */
A3_ASM_FUNC(a3_particles_asm_integrate, ARGS_PPPP)
    test    edx, edx
    jz      2f
    movss   xmm4, dword ptr [rcx]
    shufps  xmm4, xmm4, 0
    andps   xmm4, xmmword ptr [rip + K_XYZ]
    orps    xmm4, xmmword ptr [rip + K_ONE_W]   /* damp = (k, k, k, 1) */
    movups  xmm5, xmmword ptr [rcx + 4]
    andps   xmm5, xmmword ptr [rip + K_XYZ]     /* gdt = (gx, gy, gz, 0) * dt */
    movss   xmm6, dword ptr [rcx + 16]
    shufps  xmm6, xmm6, 0
    movaps  xmm7, xmm6
    andps   xmm6, xmmword ptr [rip + K_XYZ]     /* dtv = (dt, dt, dt, 0) */
    andps   xmm7, xmmword ptr [rip + K_W]       /* age = (0, 0, 0, dt) */
1:
    movups  xmm0, xmmword ptr [rsi]
    mulps   xmm0, xmm4
    addps   xmm0, xmm5
    movups  xmmword ptr [rsi], xmm0
    mulps   xmm0, xmm6
    movups  xmm1, xmmword ptr [rdi]
    addps   xmm1, xmm0
    addps   xmm1, xmm7
    movups  xmmword ptr [rdi], xmm1
    add     rsi, 16
    add     rdi, 16
    dec     edx
    jnz     1b
2:
    ret

A3_ASM_END_FILE
