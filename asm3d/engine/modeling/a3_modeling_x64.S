/*
 * ASM3D - a3_modeling_x64.S
 * Modeling kernels: point transform, triangle normals, ray picking, bounds.
 * Same operation order as a3_modeling_ref.c: bit-identical results.
 */
#include "../core/a3_asm_abi.h"

    .intel_syntax noprefix

    .section .rodata
    .p2align 4
K_XYZ:   .long 0xffffffff, 0xffffffff, 0xffffffff, 0
K_W:     .long 0, 0, 0, 0xffffffff
K_ABS:   .long 0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff
K_ONE:   .float 1.0
K_ZERO:  .float 0.0
K_EPS12: .float 1e-12
K_EPS6:  .float 1e-6

    .text

/* out = a x b  (lane x: a.y*b.z - a.z*b.y ...), temporaries t1 t2 */
.macro CROSS a, b, out, t1, t2
    movaps  \out, \a
    shufps  \out, \out, 0xC9        /* a.yzx */
    movaps  \t1, \b
    shufps  \t1, \t1, 0xD2          /* b.zxy */
    mulps   \out, \t1
    movaps  \t1, \a
    shufps  \t1, \t1, 0xD2          /* a.zxy */
    movaps  \t2, \b
    shufps  \t2, \t2, 0xC9          /* b.yzx */
    mulps   \t1, \t2
    subps   \out, \t1
.endm

/* out.x = (a.x*b.x + a.y*b.y) + a.z*b.z */
.macro DOT3 a, b, out, t
    movaps  \out, \a
    mulps   \out, \b
    movaps  \t, \out
    shufps  \t, \t, 0x55
    addss   \out, \t
    movaps  \t, \out
    shufps  \t, \t, 0xAA
    addss   \out, \t
.endm

/* void a3_mk_asm_transform(A3Vec4 *pos, const u8 *mask, u32 count, const f32 *m16)
 *   rdi pos, rsi mask (may be NULL), edx count, rcx matrix */
A3_ASM_FUNC(a3_mk_asm_transform, ARGS_PPPP)
    test    edx, edx
    jz      9f
    movups  xmm8, xmmword ptr [rcx]
    movups  xmm9, xmmword ptr [rcx + 16]
    movups  xmm10, xmmword ptr [rcx + 32]
    movups  xmm11, xmmword ptr [rcx + 48]
    movaps  xmm12, xmmword ptr [rip + K_XYZ]
    movaps  xmm13, xmmword ptr [rip + K_W]
    xor     r8d, r8d                /* r8 = 1 when there is a mask */
    test    rsi, rsi
    setnz   r8b
1:
    test    r8d, r8d
    jz      2f
    cmp     byte ptr [rsi], 0
    je      3f
2:
    movups  xmm0, xmmword ptr [rdi]
    movaps  xmm1, xmm0
    shufps  xmm1, xmm1, 0x00        /* x */
    movaps  xmm2, xmm0
    shufps  xmm2, xmm2, 0x55        /* y */
    movaps  xmm3, xmm0
    shufps  xmm3, xmm3, 0xAA        /* z */
    mulps   xmm1, xmm8
    mulps   xmm2, xmm9
    addps   xmm1, xmm2
    mulps   xmm3, xmm10
    addps   xmm1, xmm3
    addps   xmm1, xmm11
    andps   xmm1, xmm12
    andps   xmm0, xmm13             /* keep w */
    orps    xmm1, xmm0
    movups  xmmword ptr [rdi], xmm1
3:
    add     rdi, 16
    add     rsi, r8
    dec     edx
    jnz     1b
9:
    ret

/* void a3_mk_asm_tri_normals(const A3Vec4 *pos, const u32 *tris, u32 count, A3Vec4 *out)
 *   rdi pos, rsi tris, edx count, rcx out */
A3_ASM_FUNC(a3_mk_asm_tri_normals, ARGS_PPPP)
    test    edx, edx
    jz      9f
    movaps  xmm12, xmmword ptr [rip + K_XYZ]
1:
    mov     r8d, dword ptr [rsi]
    mov     r9d, dword ptr [rsi + 4]
    mov     r10d, dword ptr [rsi + 8]
    shl     r8, 4
    shl     r9, 4
    shl     r10, 4
    movups  xmm0, xmmword ptr [rdi + r8]
    movups  xmm1, xmmword ptr [rdi + r9]
    movups  xmm2, xmmword ptr [rdi + r10]
    subps   xmm1, xmm0
    subps   xmm2, xmm0
    CROSS   xmm1, xmm2, xmm3, xmm4, xmm5
    andps   xmm3, xmm12
    movups  xmmword ptr [rcx], xmm3
    add     rsi, 12
    add     rcx, 16
    dec     edx
    jnz     1b
9:
    ret

/* u32 a3_mk_asm_raycast(const A3Vec4 *pos, const u32 *tris, u32 count, const f32 *ray, f32 *t_inout)
 *   rdi pos, rsi tris, edx count, rcx ray {o, d}, r8 t_inout; returns triangle + 1 or 0 */
A3_ASM_FUNC(a3_mk_asm_raycast, ARGS_PPIPP)
    movups  xmm8, xmmword ptr [rcx]         /* o */
    movups  xmm9, xmmword ptr [rcx + 16]    /* d */
    movss   xmm10, dword ptr [r8]           /* best */
    xor     eax, eax
    xor     r9d, r9d
    test    edx, edx
    jz      9f
1:
    mov     r10d, dword ptr [rsi]
    mov     r11d, dword ptr [rsi + 4]
    mov     ecx, dword ptr [rsi + 8]
    shl     r10, 4
    shl     r11, 4
    shl     rcx, 4
    movups  xmm0, xmmword ptr [rdi + r10]   /* a */
    movups  xmm1, xmmword ptr [rdi + r11]
    movups  xmm2, xmmword ptr [rdi + rcx]
    subps   xmm1, xmm0                      /* e1 */
    subps   xmm2, xmm0                      /* e2 */
    CROSS   xmm9, xmm2, xmm3, xmm4, xmm5    /* p = d x e2 */
    DOT3    xmm1, xmm3, xmm6, xmm4          /* det */
    movaps  xmm7, xmm6
    andps   xmm7, xmmword ptr [rip + K_ABS]
    comiss  xmm7, dword ptr [rip + K_EPS12]
    jb      8f
    movss   xmm7, dword ptr [rip + K_ONE]
    divss   xmm7, xmm6                      /* inv */
    movaps  xmm11, xmm8
    subps   xmm11, xmm0                     /* s = o - a */
    DOT3    xmm11, xmm3, xmm6, xmm4
    mulss   xmm6, xmm7                      /* u */
    comiss  xmm6, dword ptr [rip + K_ZERO]
    jb      8f
    comiss  xmm6, dword ptr [rip + K_ONE]
    ja      8f
    CROSS   xmm11, xmm1, xmm12, xmm4, xmm5  /* q = s x e1 */
    DOT3    xmm9, xmm12, xmm13, xmm4
    mulss   xmm13, xmm7                     /* v */
    comiss  xmm13, dword ptr [rip + K_ZERO]
    jb      8f
    addss   xmm13, xmm6                     /* u + v */
    comiss  xmm13, dword ptr [rip + K_ONE]
    ja      8f
    DOT3    xmm2, xmm12, xmm14, xmm4
    mulss   xmm14, xmm7                     /* t */
    comiss  xmm14, dword ptr [rip + K_EPS6]
    jbe     8f
    comiss  xmm10, xmm14                    /* best > t ? */
    jbe     8f
    movss   xmm10, xmm14
    lea     eax, [r9 + 1]
8:
    add     rsi, 12
    inc     r9d
    cmp     r9d, edx
    jb      1b
9:
    movss   dword ptr [r8], xmm10
    ret

/* void a3_mk_asm_bounds(const A3Vec4 *pos, u32 count, A3Vec4 *out_min, A3Vec4 *out_max)
 *   rdi pos, esi count (>= 1), rdx min, rcx max */
A3_ASM_FUNC(a3_mk_asm_bounds, ARGS_PPPP)
    movups  xmm0, xmmword ptr [rdi]
    movaps  xmm1, xmm0
    dec     esi
    jz      2f
1:
    add     rdi, 16
    movups  xmm2, xmmword ptr [rdi]
    minps   xmm0, xmm2
    maxps   xmm1, xmm2
    dec     esi
    jnz     1b
2:
    movups  xmmword ptr [rdx], xmm0
    movups  xmmword ptr [rcx], xmm1
    ret

A3_ASM_END_FILE
