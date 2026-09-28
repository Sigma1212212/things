/*
 * ASM3D - a3_simd_x64.S
 * x86-64 SSE assembly kernels (System V ABI: Linux, macOS).
 *
 * Each kernel performs the exact same sequence of IEEE single precision
 * multiplies and adds as its C reference in a3_simd_ref.c, so results are
 * bit-identical; tests/test_simd.c verifies this with exact comparisons.
 * Unaligned loads (movups) are used so callers need not guarantee 16-byte
 * alignment; on modern cores this costs nothing for aligned data.
 */
#if defined(__APPLE__)
#  define SYM(x) _##x
#else
#  define SYM(x) x
#endif

    .intel_syntax noprefix
    .text

/* ------------------------------------------------------------------------
 * void a3_asm_mat4_mul_batch(A3Mat4 *out, const A3Mat4 *a, const A3Mat4 *b, u32 count)
 *   rdi = out, rsi = a, rdx = b, ecx = count
 *   out[i] = a[i] * b[i] (column-major)
 * --------------------------------------------------------------------- */
    .globl SYM(a3_asm_mat4_mul_batch)
    .p2align 4
SYM(a3_asm_mat4_mul_batch):
    test    ecx, ecx
    jz      2f
1:
    movups  xmm0, [rsi]          /* a col 0 */
    movups  xmm1, [rsi + 16]     /* a col 1 */
    movups  xmm2, [rsi + 32]     /* a col 2 */
    movups  xmm3, [rsi + 48]     /* a col 3 */

    .irp col, 0, 16, 32, 48
    movss   xmm4, [rdx + \col]
    shufps  xmm4, xmm4, 0
    mulps   xmm4, xmm0
    movss   xmm5, [rdx + \col + 4]
    shufps  xmm5, xmm5, 0
    mulps   xmm5, xmm1
    addps   xmm4, xmm5
    movss   xmm5, [rdx + \col + 8]
    shufps  xmm5, xmm5, 0
    mulps   xmm5, xmm2
    addps   xmm4, xmm5
    movss   xmm5, [rdx + \col + 12]
    shufps  xmm5, xmm5, 0
    mulps   xmm5, xmm3
    addps   xmm4, xmm5
    movups  [rdi + \col], xmm4
    .endr

    add     rsi, 64
    add     rdx, 64
    add     rdi, 64
    dec     ecx
    jnz     1b
2:
    ret

/* ------------------------------------------------------------------------
 * void a3_asm_mat4_mul_one_many(A3Mat4 *out, const A3Mat4 *parent, const A3Mat4 *locals, u32 count)
 *   out[i] = parent * locals[i]
 * --------------------------------------------------------------------- */
    .globl SYM(a3_asm_mat4_mul_one_many)
    .p2align 4
SYM(a3_asm_mat4_mul_one_many):
    test    ecx, ecx
    jz      2f
    movups  xmm0, [rsi]
    movups  xmm1, [rsi + 16]
    movups  xmm2, [rsi + 32]
    movups  xmm3, [rsi + 48]
1:
    .irp col, 0, 16, 32, 48
    movss   xmm4, [rdx + \col]
    shufps  xmm4, xmm4, 0
    mulps   xmm4, xmm0
    movss   xmm5, [rdx + \col + 4]
    shufps  xmm5, xmm5, 0
    mulps   xmm5, xmm1
    addps   xmm4, xmm5
    movss   xmm5, [rdx + \col + 8]
    shufps  xmm5, xmm5, 0
    mulps   xmm5, xmm2
    addps   xmm4, xmm5
    movss   xmm5, [rdx + \col + 12]
    shufps  xmm5, xmm5, 0
    mulps   xmm5, xmm3
    addps   xmm4, xmm5
    movups  [rdi + \col], xmm4
    .endr
    add     rdx, 64
    add     rdi, 64
    dec     ecx
    jnz     1b
2:
    ret

/* ------------------------------------------------------------------------
 * void a3_asm_transform_points(A3Vec4 *out, const A3Mat4 *m, const A3Vec4 *in, u32 count)
 *   out[i] = m * in[i]
 * --------------------------------------------------------------------- */
    .globl SYM(a3_asm_transform_points)
    .p2align 4
SYM(a3_asm_transform_points):
    test    ecx, ecx
    jz      2f
    movups  xmm0, [rsi]
    movups  xmm1, [rsi + 16]
    movups  xmm2, [rsi + 32]
    movups  xmm3, [rsi + 48]
1:
    movups  xmm6, [rdx]
    movaps  xmm4, xmm6
    shufps  xmm4, xmm4, 0x00
    mulps   xmm4, xmm0
    movaps  xmm5, xmm6
    shufps  xmm5, xmm5, 0x55
    mulps   xmm5, xmm1
    addps   xmm4, xmm5
    movaps  xmm5, xmm6
    shufps  xmm5, xmm5, 0xAA
    mulps   xmm5, xmm2
    addps   xmm4, xmm5
    movaps  xmm5, xmm6
    shufps  xmm5, xmm5, 0xFF
    mulps   xmm5, xmm3
    addps   xmm4, xmm5
    movups  [rdi], xmm4
    add     rdx, 16
    add     rdi, 16
    dec     ecx
    jnz     1b
2:
    ret

/* ------------------------------------------------------------------------
 * u32 a3_asm_cull_spheres_soa(const f32 *soa, const A3Vec4 *spheres, u8 *visible, u32 count)
 *   soa: 8 vectors of 4 floats: X0 Y0 Z0 W0 (planes 0-3), X1 Y1 Z1 W1 (planes 4-7)
 *   A sphere is culled when any plane distance d < -radius, where
 *   d = ((px*cx + py*cy) + pz*cz) + pw.
 *   Returns number of visible spheres.
 * --------------------------------------------------------------------- */
    .globl SYM(a3_asm_cull_spheres_soa)
    .p2align 4
SYM(a3_asm_cull_spheres_soa):
    xor     eax, eax
    test    ecx, ecx
    jz      3f
    movups  xmm8,  [rdi]
    movups  xmm9,  [rdi + 16]
    movups  xmm10, [rdi + 32]
    movups  xmm11, [rdi + 48]
    movups  xmm12, [rdi + 64]
    movups  xmm13, [rdi + 80]
    movups  xmm14, [rdi + 96]
    movups  xmm15, [rdi + 112]
    /* sign mask for negating the radius */
    mov     r8d, 0x80000000
    movd    xmm7, r8d
    shufps  xmm7, xmm7, 0
1:
    movups  xmm6, [rsi]               /* cx cy cz r */
    movaps  xmm0, xmm6
    shufps  xmm0, xmm0, 0x00          /* cx */
    movaps  xmm1, xmm6
    shufps  xmm1, xmm1, 0x55          /* cy */
    movaps  xmm2, xmm6
    shufps  xmm2, xmm2, 0xAA          /* cz */
    movaps  xmm3, xmm6
    shufps  xmm3, xmm3, 0xFF          /* r  */
    xorps   xmm3, xmm7                /* -r */

    /* planes 0-3 */
    movaps  xmm4, xmm8
    mulps   xmm4, xmm0
    movaps  xmm5, xmm9
    mulps   xmm5, xmm1
    addps   xmm4, xmm5
    movaps  xmm5, xmm10
    mulps   xmm5, xmm2
    addps   xmm4, xmm5
    addps   xmm4, xmm11
    cmpltps xmm4, xmm3                /* d < -r */
    movmskps r9d, xmm4

    /* planes 4-7 */
    movaps  xmm4, xmm12
    mulps   xmm4, xmm0
    movaps  xmm5, xmm13
    mulps   xmm5, xmm1
    addps   xmm4, xmm5
    movaps  xmm5, xmm14
    mulps   xmm5, xmm2
    addps   xmm4, xmm5
    addps   xmm4, xmm15
    cmpltps xmm4, xmm3
    movmskps r10d, xmm4

    or      r9d, r10d
    setz    r11b                      /* visible when no plane rejects */
    mov     [rdx], r11b
    movzx   r11d, r11b
    add     eax, r11d

    add     rsi, 16
    inc     rdx
    dec     ecx
    jnz     1b
3:
    ret

/* ------------------------------------------------------------------------
 * void a3_asm_integrate_v4(A3Vec4 *pos, const A3Vec4 *vel, f32 dt, u32 count)
 *   rdi = pos, rsi = vel, xmm0 = dt, edx = count
 *   pos[i] = pos[i] + vel[i] * dt
 * --------------------------------------------------------------------- */
    .globl SYM(a3_asm_integrate_v4)
    .p2align 4
SYM(a3_asm_integrate_v4):
    test    edx, edx
    jz      2f
    shufps  xmm0, xmm0, 0
1:
    movups  xmm1, [rsi]
    mulps   xmm1, xmm0
    movups  xmm2, [rdi]
    addps   xmm2, xmm1
    movups  [rdi], xmm2
    add     rsi, 16
    add     rdi, 16
    dec     edx
    jnz     1b
2:
    ret

#if defined(__linux__) && defined(__ELF__)
    .section .note.GNU-stack,"",@progbits
#endif
