/*
 * ASM3D - a3_geom_x64.S
 * Geometry kernels of the procedural model builder: Catmull-Rom sampling,
 * frame placement of profile points, grid triangulation, vertex normal
 * accumulation, normalization and the signed volume (winding) test.
 * Same operation order as a3_geom_ref.c: bit-identical results.
 *
 * A3Vertex layout: position @0 (12 bytes), normal @12 (12 bytes), uv @24, stride 32.
 */
#include "../core/a3_asm_abi.h"

    .intel_syntax noprefix

    .section .rodata
    .p2align 4
G_TWO:   .float 2.0, 2.0, 2.0, 2.0
G_FIVE:  .float 5.0, 5.0, 5.0, 5.0
G_FOUR:  .float 4.0, 4.0, 4.0, 4.0
G_THREE: .float 3.0, 3.0, 3.0, 3.0
G_HALF:  .float 0.5, 0.5, 0.5, 0.5
G_UPY:   .float 0.0, 1.0, 0.0, 0.0
G_EPS12: .float 1e-12
    .p2align 4
W_OFFS:  .float 0.0, 0.6, 1.57079637, 0.0
W_INV2PI: .float 0.159154943, 0.159154943, 0.159154943, 0.159154943
W_2PI:   .float 6.28318548, 6.28318548, 6.28318548, 6.28318548
W_PI:    .float 3.14159274, 3.14159274, 3.14159274, 3.14159274
W_HPI:   .float 1.57079637, 1.57079637, 1.57079637, 1.57079637
W_NHPI:  .float -1.57079637, -1.57079637, -1.57079637, -1.57079637
W_NPI:   .float -3.14159274, -3.14159274, -3.14159274, -3.14159274
W_HALF:  .float 0.5, 0.5, 0.5, 0.5
W_NHALF: .float -0.5, -0.5, -0.5, -0.5
W_C11:   .float -2.50521084e-8, -2.50521084e-8, -2.50521084e-8, -2.50521084e-8
W_C9:    .float 2.75573192e-6, 2.75573192e-6, 2.75573192e-6, 2.75573192e-6
W_C7:    .float -1.98412698e-4, -1.98412698e-4, -1.98412698e-4, -1.98412698e-4
W_C5:    .float 8.33333333e-3, 8.33333333e-3, 8.33333333e-3, 8.33333333e-3
W_C3:    .float -1.66666667e-1, -1.66666667e-1, -1.66666667e-1, -1.66666667e-1
W_ONE:   .float 1.0, 1.0, 1.0, 1.0
W_SIGN:  .long 0x80000000, 0x80000000, 0x80000000, 0x80000000
W_ABS:   .long 0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff
W_K050:  .float 0.5
W_K065:  .float 0.65
W_K008:  .float 0.08
W_K042:  .float 0.42
W_K015:  .float 0.15
W_K020:  .float 0.2
W_K0035: .float 0.035
W_K008T: .float 0.08

    .text

/* reg.xyz = 12 bytes at addr, reg.w = 0 */
.macro LOAD3 reg, base, off, tmp
    movsd   \reg, qword ptr [\base + \off]
    movss   \tmp, dword ptr [\base + \off + 8]
    movlhps \reg, \tmp
.endm

/* 12 bytes at addr = reg.xyz */
.macro STORE3 base, off, reg, tmp
    movsd   qword ptr [\base + \off], \reg
    movhlps \tmp, \reg
    movss   dword ptr [\base + \off + 8], \tmp
.endm

/* out = a x b, temporaries t1 t2 (out differs from a and b) */
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

/* u32 a3_gk_asm_catmull_rom(const A3Vec2 *ctrl, u32 n, u32 sub, u32 closed, A3Vec2 *out)
 *   rdi ctrl, esi n, edx sub, ecx closed, r8 out; returns eax = samples */
A3_ASM_FUNC(a3_gk_asm_catmull_rom, ARGS_PIIIP)
    mov     esi, esi
    mov     edx, edx
    cvtsi2ss xmm15, rdx             /* sub as float */
    mov     r9d, esi                /* segments = n (closed) or n - 1 */
    test    ecx, ecx
    jnz     1f
    dec     r9d
1:
    xor     eax, eax                /* w */
    xor     r10d, r10d              /* i */
2:
    cmp     r10d, r9d
    jae     8f
    /* p1 = ctrl[i] */
    movsd   xmm1, qword ptr [rdi + r10 * 8]
    /* p0 = ctrl[i0] */
    test    ecx, ecx
    jz      3f
    mov     r11d, r10d              /* closed: i0 = i ? i - 1 : n - 1 */
    test    r11d, r11d
    jnz     31f
    mov     r11d, esi
31: dec     r11d
    jmp     4f
3:  mov     r11d, r10d              /* open: i0 = i ? i - 1 : 0 */
    test    r11d, r11d
    jz      4f
    dec     r11d
4:  movsd   xmm0, qword ptr [rdi + r11 * 8]
    /* p2 = ctrl[i2] */
    lea     r11d, [r10 + 1]
    cmp     r11d, esi
    jb      5f
    xor     r11d, r11d              /* closed wrap (open never reaches n here) */
5:  movsd   xmm2, qword ptr [rdi + r11 * 8]
    /* p3 = ctrl[i3] */
    inc     r11d
    test    ecx, ecx
    jz      6f
    cmp     r11d, esi               /* closed: wrap */
    jb      7f
    xor     r11d, r11d
    jmp     7f
6:  cmp     r11d, esi               /* open: clamp to n - 1 */
    jb      7f
    lea     r11d, [rsi - 1]
7:  movsd   xmm3, qword ptr [rdi + r11 * 8]
    /* coefficients */
    movaps  xmm4, xmm1
    mulps   xmm4, xmmword ptr [rip + G_TWO]      /* a = p1 * 2 */
    movaps  xmm5, xmm2
    subps   xmm5, xmm0                           /* b = p2 - p0 */
    movaps  xmm6, xmm0
    mulps   xmm6, xmmword ptr [rip + G_TWO]      /* c = p0 * 2 */
    movaps  xmm9, xmm1
    mulps   xmm9, xmmword ptr [rip + G_FIVE]
    subps   xmm6, xmm9                           /*   - p1 * 5 */
    movaps  xmm9, xmm2
    mulps   xmm9, xmmword ptr [rip + G_FOUR]
    addps   xmm6, xmm9                           /*   + p2 * 4 */
    subps   xmm6, xmm3                           /*   - p3 */
    movaps  xmm7, xmm1
    mulps   xmm7, xmmword ptr [rip + G_THREE]    /* d = p1 * 3 */
    subps   xmm7, xmm0                           /*   - p0 */
    movaps  xmm9, xmm2
    mulps   xmm9, xmmword ptr [rip + G_THREE]
    subps   xmm7, xmm9                           /*   - p2 * 3 */
    addps   xmm7, xmm3                           /*   + p3 */
    xor     r11d, r11d                           /* k */
71:
    cmp     r11d, edx
    jae     72f
    cvtsi2ss xmm8, r11
    divss   xmm8, xmm15                          /* t = k / sub */
    shufps  xmm8, xmm8, 0x00
    movaps  xmm10, xmm8
    mulps   xmm10, xmm8                          /* t2 */
    movaps  xmm11, xmm10
    mulps   xmm11, xmm8                          /* t3 */
    movaps  xmm12, xmm5
    mulps   xmm12, xmm8                          /* v = b * t */
    addps   xmm12, xmm4                          /* v = a + v */
    movaps  xmm13, xmm6
    mulps   xmm13, xmm10
    addps   xmm12, xmm13                         /* + c * t2 */
    movaps  xmm13, xmm7
    mulps   xmm13, xmm11
    addps   xmm12, xmm13                         /* + d * t3 */
    mulps   xmm12, xmmword ptr [rip + G_HALF]
    movsd   qword ptr [r8 + rax * 8], xmm12
    inc     eax
    inc     r11d
    jmp     71b
72:
    inc     r10d
    jmp     2b
8:
    test    ecx, ecx
    jnz     9f
    lea     r11d, [rsi - 1]                      /* open: append the last point */
    movsd   xmm0, qword ptr [rdi + r11 * 8]
    movsd   qword ptr [r8 + rax * 8], xmm0
    inc     eax
9:
    ret

/* void a3_gk_asm_frame_points(const A3Vec2 *pts, u32 count, const f32 *frame, void *dst, u32 stride)
 *   rdi pts, esi count, rdx frame, rcx dst, r8d stride */
A3_ASM_FUNC(a3_gk_asm_frame_points, ARGS_PIPPI)
    mov     r8d, r8d
    movups  xmm8, xmmword ptr [rdx]              /* origin */
    movups  xmm9, xmmword ptr [rdx + 16]         /* axis a */
    movups  xmm10, xmmword ptr [rdx + 32]        /* axis b */
1:
    test    esi, esi
    jz      2f
    movss   xmm0, dword ptr [rdi]
    shufps  xmm0, xmm0, 0x00                     /* x */
    movss   xmm1, dword ptr [rdi + 4]
    shufps  xmm1, xmm1, 0x00                     /* y */
    movaps  xmm2, xmm9
    mulps   xmm2, xmm0                           /* v = a * x */
    addps   xmm2, xmm8                           /* v = o + v */
    movaps  xmm3, xmm10
    mulps   xmm3, xmm1                           /* w = b * y */
    addps   xmm2, xmm3                           /* v + w */
    STORE3  rcx, 0, xmm2, xmm4
    add     rdi, 8
    add     rcx, r8
    dec     esi
    jmp     1b
2:
    ret

/* u32 a3_gk_asm_grid_indices(u32 rows, u32 k, u32 wrap, u32 base, u32 *out)
 *   edi rows, esi k, edx wrap, ecx base, r8 out; returns eax = indices */
A3_ASM_FUNC(a3_gk_asm_grid_indices, ARGS_IIIIP)
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15
    mov     r9d, esi                /* kk = wrap ? k : k - 1 */
    test    edx, edx
    jnz     1f
    dec     r9d
1:
    xor     eax, eax                /* w */
    xor     r10d, r10d              /* r */
    lea     r15d, [rdi - 1]         /* rows - 1 */
2:
    cmp     r10d, r15d
    jae     9f
    mov     r11d, r10d
    imul    r11d, esi
    add     r11d, ecx               /* row base = base + r * k */
    xor     edx, edx                /* j */
3:
    cmp     edx, r9d
    jae     8f
    lea     ebx, [rdx + 1]          /* j1 = j + 1 == k ? 0 : j + 1 */
    cmp     ebx, esi
    jne     4f
    xor     ebx, ebx
4:
    lea     r12d, [r11 + rdx]       /* a */
    lea     r13d, [r11 + rbx]       /* b */
    lea     r14d, [r13 + rsi]       /* c = b + k */
    mov     dword ptr [r8 + rax * 4], r12d
    mov     dword ptr [r8 + rax * 4 + 4], r13d
    mov     dword ptr [r8 + rax * 4 + 8], r14d
    mov     dword ptr [r8 + rax * 4 + 12], r12d
    mov     dword ptr [r8 + rax * 4 + 16], r14d
    lea     r13d, [r12 + rsi]       /* d = a + k */
    mov     dword ptr [r8 + rax * 4 + 20], r13d
    add     eax, 6
    inc     edx
    jmp     3b
8:
    inc     r10d
    jmp     2b
9:
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

/* void a3_gk_asm_vertex_normals(A3Vertex *v, const u32 *idx, u32 tri_count)
 *   rdi v, rsi idx, edx tri_count */
A3_ASM_FUNC(a3_gk_asm_vertex_normals, ARGS_PPPP)
1:
    test    edx, edx
    jz      2f
    mov     r8d, dword ptr [rsi]
    mov     r9d, dword ptr [rsi + 4]
    mov     r10d, dword ptr [rsi + 8]
    shl     r8, 5
    shl     r9, 5
    shl     r10, 5
    add     r8, rdi
    add     r9, rdi
    add     r10, rdi
    LOAD3   xmm0, r8, 0, xmm7        /* a */
    LOAD3   xmm1, r9, 0, xmm7        /* b */
    LOAD3   xmm2, r10, 0, xmm7       /* c */
    subps   xmm1, xmm0              /* e1 = b - a */
    subps   xmm2, xmm0              /* e2 = c - a */
    CROSS   xmm1, xmm2, xmm3, xmm5, xmm6
    LOAD3   xmm4, r8, 12, xmm7
    addps   xmm4, xmm3
    STORE3  r8, 12, xmm4, xmm7
    LOAD3   xmm4, r9, 12, xmm7
    addps   xmm4, xmm3
    STORE3  r9, 12, xmm4, xmm7
    LOAD3   xmm4, r10, 12, xmm7
    addps   xmm4, xmm3
    STORE3  r10, 12, xmm4, xmm7
    add     rsi, 12
    dec     edx
    jmp     1b
2:
    ret

/* void a3_gk_asm_normalize(A3Vertex *v, u32 count)
 *   rdi v, esi count */
A3_ASM_FUNC(a3_gk_asm_normalize, ARGS_PPPP)
    movss   xmm9, dword ptr [rip + G_EPS12]
    movaps  xmm10, xmmword ptr [rip + G_UPY]
1:
    test    esi, esi
    jz      3f
    LOAD3   xmm0, rdi, 12, xmm7
    DOT3    xmm0, xmm0, xmm1, xmm2
    comiss  xmm1, xmm9
    jbe     2f                      /* <= 1e-12 or NaN: straight up */
    sqrtss  xmm1, xmm1
    shufps  xmm1, xmm1, 0x00
    divps   xmm0, xmm1
    STORE3  rdi, 12, xmm0, xmm7
    jmp     4f
2:
    STORE3  rdi, 12, xmm10, xmm7
4:
    add     rdi, 32
    dec     esi
    jmp     1b
3:
    ret

/* f32 a3_gk_asm_signed_volume(const A3Vertex *v, const u32 *idx, u32 tri_count)
 *   rdi v, rsi idx, edx tri_count; returns xmm0 */
A3_ASM_FUNC(a3_gk_asm_signed_volume, ARGS_PPPP)
    xorps   xmm8, xmm8              /* sum */
1:
    test    edx, edx
    jz      2f
    mov     r8d, dword ptr [rsi]
    mov     r9d, dword ptr [rsi + 4]
    mov     r10d, dword ptr [rsi + 8]
    shl     r8, 5
    shl     r9, 5
    shl     r10, 5
    add     r8, rdi
    add     r9, rdi
    add     r10, rdi
    LOAD3   xmm0, r8, 0, xmm7        /* a */
    LOAD3   xmm1, r9, 0, xmm7        /* b */
    LOAD3   xmm2, r10, 0, xmm7       /* c */
    CROSS   xmm1, xmm2, xmm3, xmm5, xmm6
    DOT3    xmm0, xmm3, xmm4, xmm5
    addss   xmm8, xmm4              /* sum = sum + d */
    add     rsi, 12
    dec     edx
    jmp     1b
2:
    movaps  xmm0, xmm8
    ret

/* xmm0 = sin4(xmm0) with the kernels' polynomial (see a3_gk_ref_sin4).
 * clobbers xmm1-xmm4 */
.macro SIN4
    movaps  xmm1, xmm0
    mulps   xmm1, xmmword ptr [rip + W_INV2PI]      /* q = x / 2pi */
    xorps   xmm2, xmm2
    cmpltps xmm2, xmm1                              /* mask: 0 < q  (so q >= 0 region handled below) */
    movaps  xmm3, xmm1
    xorps   xmm4, xmm4
    cmpltps xmm3, xmm4                              /* mask: q < 0 */
    movaps  xmm4, xmm3
    andps   xmm4, xmmword ptr [rip + W_NHALF]
    andnps  xmm3, xmmword ptr [rip + W_HALF]
    orps    xmm3, xmm4                              /* q < 0 ? -0.5 : 0.5 */
    addps   xmm1, xmm3
    cvttps2dq xmm1, xmm1
    cvtdq2ps xmm1, xmm1                             /* k */
    mulps   xmm1, xmmword ptr [rip + W_2PI]         /* m = k * 2pi */
    subps   xmm0, xmm1                              /* r = x - m */
    /* fold: r > pi/2 -> pi - r ; r < -pi/2 -> -pi - r */
    movaps  xmm1, xmmword ptr [rip + W_HPI]
    cmpltps xmm1, xmm0                              /* hi mask */
    movaps  xmm2, xmm0
    cmpltps xmm2, xmmword ptr [rip + W_NHPI]        /* lo mask */
    movaps  xmm3, xmmword ptr [rip + W_PI]
    subps   xmm3, xmm0                              /* pi - r */
    movaps  xmm4, xmmword ptr [rip + W_NPI]
    subps   xmm4, xmm0                              /* -pi - r */
    andps   xmm3, xmm1
    andps   xmm4, xmm2
    orps    xmm1, xmm2                              /* folded lanes */
    andnps  xmm1, xmm0                              /* untouched lanes keep r */
    orps    xmm1, xmm3
    orps    xmm1, xmm4                              /* r folded */
    movaps  xmm0, xmm1
    movaps  xmm2, xmm0
    mulps   xmm2, xmm0                              /* s = r * r */
    movaps  xmm3, xmm2
    mulps   xmm3, xmmword ptr [rip + W_C11]         /* p = s * c11 */
    addps   xmm3, xmmword ptr [rip + W_C9]
    mulps   xmm3, xmm2
    addps   xmm3, xmmword ptr [rip + W_C7]
    mulps   xmm3, xmm2
    addps   xmm3, xmmword ptr [rip + W_C5]
    mulps   xmm3, xmm2
    addps   xmm3, xmmword ptr [rip + W_C3]
    mulps   xmm3, xmm2
    addps   xmm3, xmmword ptr [rip + W_ONE]
    mulps   xmm3, xmm0                              /* p * r */
    movaps  xmm0, xmm3
.endm

/* void a3_gk_asm_walk_pose(const f32 *phase, const f32 *amp, u32 n, f32 *out)
 *   rdi phase, rsi amp, edx n, rcx out */
A3_ASM_FUNC(a3_gk_asm_walk_pose, ARGS_PPPP)
    movaps  xmm15, xmmword ptr [rip + W_SIGN]
    xorps   xmm14, xmm14                            /* 0 */
1:
    test    edx, edx
    jz      2f
    movss   xmm0, dword ptr [rdi]
    shufps  xmm0, xmm0, 0x00
    addps   xmm0, xmmword ptr [rip + W_OFFS]        /* (ph + 0, ph + 0.6, ph + pi/2, ph + 0) */
    SIN4
    movaps  xmm5, xmm0                              /* s1 */
    movaps  xmm6, xmm0
    shufps  xmm6, xmm6, 0x55                        /* s2 */
    movaps  xmm7, xmm0
    shufps  xmm7, xmm7, 0xAA                        /* c */
    movss   xmm8, dword ptr [rsi]                   /* a */
    /* thighs */
    movaps  xmm9, xmm8
    mulss   xmm9, dword ptr [rip + W_K050]
    mulss   xmm9, xmm5                              /* o0 = (a*0.5) * s1 */
    movss   dword ptr [rcx], xmm9
    xorps   xmm9, xmm15
    movss   dword ptr [rcx + 4], xmm9               /* o1 = -o0 */
    /* shins */
    movaps  xmm10, xmm6
    maxss   xmm10, xmm14                            /* m2 = max(s2, 0) */
    movaps  xmm11, xmm6
    xorps   xmm11, xmm15
    maxss   xmm11, xmm14                            /* n2 = max(-s2, 0) */
    movaps  xmm9, xmm8
    mulss   xmm9, dword ptr [rip + W_K065]          /* t = a*0.65 */
    movaps  xmm12, xmm9
    mulss   xmm12, xmm10
    addss   xmm12, dword ptr [rip + W_K008]
    xorps   xmm12, xmm15
    movss   dword ptr [rcx + 8], xmm12              /* o2 = -(t*m2 + 0.08) */
    movaps  xmm12, xmm9
    mulss   xmm12, xmm11
    addss   xmm12, dword ptr [rip + W_K008]
    xorps   xmm12, xmm15
    movss   dword ptr [rcx + 12], xmm12             /* o3 */
    /* arms */
    movaps  xmm9, xmm8
    mulss   xmm9, dword ptr [rip + W_K042]
    mulss   xmm9, xmm5                              /* u = (a*0.42) * s1 */
    movss   dword ptr [rcx + 20], xmm9              /* o5 = u */
    xorps   xmm9, xmm15
    movss   dword ptr [rcx + 16], xmm9              /* o4 = -u */
    /* forearms */
    movaps  xmm9, xmm8
    mulss   xmm9, dword ptr [rip + W_K015]          /* t = a*0.15 */
    movaps  xmm12, xmm5
    addss   xmm12, dword ptr [rip + W_ONE]          /* s1 + 1 */
    mulss   xmm12, xmm9
    addss   xmm12, dword ptr [rip + W_K020]
    movss   dword ptr [rcx + 24], xmm12             /* o6 */
    movss   xmm12, dword ptr [rip + W_ONE]
    subss   xmm12, xmm5                             /* 1 - s1 */
    mulss   xmm12, xmm9
    addss   xmm12, dword ptr [rip + W_K020]
    movss   dword ptr [rcx + 28], xmm12             /* o7 */
    /* bob and twist */
    movaps  xmm9, xmm8
    mulss   xmm9, dword ptr [rip + W_K0035]
    movaps  xmm12, xmm7
    andps   xmm12, xmmword ptr [rip + W_ABS]
    mulss   xmm9, xmm12
    movss   dword ptr [rcx + 32], xmm9              /* o8 = (a*0.035) * |c| */
    movaps  xmm9, xmm8
    mulss   xmm9, dword ptr [rip + W_K008T]
    mulss   xmm9, xmm5
    movss   dword ptr [rcx + 36], xmm9              /* o9 = (a*0.08) * s1 */
    add     rdi, 4
    add     rsi, 4
    add     rcx, 40
    dec     edx
    jmp     1b
2:
    ret

A3_ASM_END_FILE
