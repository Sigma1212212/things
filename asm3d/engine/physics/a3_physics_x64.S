/*
 * ASM3D - a3_physics_x64.S
 * Physics hot loops in x86-64 SSE assembly (System V ABI: Linux, macOS).
 *
 * Every kernel reproduces the exact floating-point operation order of its C
 * reference in a3_physics_ref.c (see the comments there), which makes the
 * simulation bit-identical between the assembly and portable builds.
 *
 * Register conventions (SysV): integer args rdi, rsi, rdx, rcx, r8, r9;
 * float args xmm0..; rax/rcx/rdx/rsi/rdi/r8-r11 and all xmm are
 * caller-saved, so no callee-saved registers are used.
 */
#if defined(__APPLE__)
#  define SYM(x) _##x
#else
#  define SYM(x) x
#endif

    .intel_syntax noprefix

    .section .rodata
    .p2align 4
K_SIGN_W:   .long 0, 0, 0, 0x80000000
K_SIGN_ALL: .long 0x80000000, 0x80000000, 0x80000000, 0x80000000
K_ABS:      .long 0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff
K_MASK_XYZ: .long 0xffffffff, 0xffffffff, 0xffffffff, 0
K_MASK_W:   .long 0, 0, 0, 0xffffffff
K_HALF:     .float 0.5
K_EPS_LEN2: .float 1e-12
K_ONE:      .float 1.0
K_MINUS1:   .float -1.0

    .text

/* Horizontal sum of xmm\v into lane 0 of xmm\v: ((x + z) + (y + w)).
 * Clobbers xmm\t. */
.macro HSUM v, t
    movaps  \t, \v
    movhlps \t, \v          /* t.lo = (z, w) */
    addps   \t, \v          /* t0 = z + x, t1 = w + y */
    movaps  \v, \t
    shufps  \t, \t, 0x55    /* broadcast t1 */
    addss   \v, \t          /* v0 = (x + z) + (y + w) */
.endm

/* ------------------------------------------------------------------------
 * void a3_phys_integrate_velocities(A3Vec4 *lin_vel, A3Vec4 *ang_vel,
 *        const A3Vec4 *accel, const A3Vec4 *damp, f32 dt, u32 count)
 *   rdi lin_vel, rsi ang_vel, rdx accel, rcx damp, xmm0 dt, r8d count
 * --------------------------------------------------------------------- */
    .globl SYM(a3_phys_integrate_velocities)
    .p2align 4
SYM(a3_phys_integrate_velocities):
    test    r8d, r8d
    jz      2f
    shufps  xmm0, xmm0, 0
1:
    movups  xmm1, [rdi]         /* v */
    movups  xmm2, [rdx]         /* a */
    mulps   xmm2, xmm0          /* a * dt */
    addps   xmm1, xmm2          /* v + a*dt */
    movups  xmm3, [rcx]         /* damp (lin, ang, -, -) */
    movaps  xmm4, xmm3
    shufps  xmm4, xmm4, 0x00
    mulps   xmm1, xmm4
    movups  [rdi], xmm1
    movups  xmm5, [rsi]
    shufps  xmm3, xmm3, 0x55
    mulps   xmm5, xmm3
    movups  [rsi], xmm5
    add     rdi, 16
    add     rsi, 16
    add     rdx, 16
    add     rcx, 16
    dec     r8d
    jnz     1b
2:
    ret

/* ------------------------------------------------------------------------
 * void a3_phys_integrate_transforms(A3Vec4 *pos, A3Quat *rot,
 *        const A3Vec4 *lin_vel, const A3Vec4 *ang_vel, f32 dt, u32 count)
 *   rdi pos, rsi rot, rdx lin_vel, rcx ang_vel, xmm0 dt, r8d count
 * --------------------------------------------------------------------- */
    .globl SYM(a3_phys_integrate_transforms)
    .p2align 4
SYM(a3_phys_integrate_transforms):
    test    r8d, r8d
    jz      9f
    movss   xmm1, [rip + K_HALF]
    mulss   xmm1, xmm0          /* hdt = 0.5 * dt (scalar, like the C ref) */
    shufps  xmm1, xmm1, 0
    shufps  xmm0, xmm0, 0
    movaps  xmm15, [rip + K_SIGN_W]
1:
    /* position */
    movups  xmm2, [rdx]
    mulps   xmm2, xmm0
    movups  xmm3, [rdi]
    addps   xmm3, xmm2
    movups  [rdi], xmm3

    /* orientation */
    movups  xmm2, [rcx]         /* w = (wx, wy, wz, ww) */
    movups  xmm3, [rsi]         /* q = (qx, qy, qz, qw) */
    pshufd  xmm4, xmm2, 0x24    /* (wx, wy, wz, wx) */
    pshufd  xmm5, xmm3, 0x3F    /* (qw, qw, qw, qx) */
    mulps   xmm4, xmm5          /* A */
    pshufd  xmm5, xmm2, 0x49    /* (wy, wz, wx, wy) */
    pshufd  xmm6, xmm3, 0x52    /* (qz, qx, qy, qy) */
    mulps   xmm5, xmm6          /* B */
    addps   xmm4, xmm5          /* D = A + B */
    pshufd  xmm5, xmm2, 0x92    /* (wz, wx, wy, wz) */
    pshufd  xmm6, xmm3, 0x89    /* (qy, qz, qx, qz) */
    mulps   xmm5, xmm6          /* C */
    xorps   xmm5, xmm15         /* C with w lane negated */
    subps   xmm4, xmm5          /* xyz: D - C ; w: D + C */
    xorps   xmm4, xmm15         /* w: -(D + C) -> dq */
    mulps   xmm4, xmm1          /* dq * hdt */
    addps   xmm3, xmm4          /* q + dq*hdt */

    /* normalize */
    movaps  xmm6, xmm3
    mulps   xmm6, xmm3
    HSUM    xmm6, xmm7          /* len2 */
    ucomiss xmm6, [rip + K_EPS_LEN2]
    jb      3f                  /* len2 < eps (or NaN) -> identity */
    sqrtss  xmm6, xmm6
    shufps  xmm6, xmm6, 0
    divps   xmm3, xmm6
    movups  [rsi], xmm3
    jmp     4f
3:
    xorps   xmm3, xmm3
    movss   xmm6, [rip + K_ONE]
    shufps  xmm6, xmm6, 0x00
    andps   xmm6, [rip + K_MASK_W]   /* (0, 0, 0, 1) */
    movups  [rsi], xmm6
4:
    add     rdi, 16
    add     rsi, 16
    add     rdx, 16
    add     rcx, 16
    dec     r8d
    jnz     1b
9:
    ret

/* ------------------------------------------------------------------------
 * void a3_phys_solve_rows(A3SolverRow *rows, u32 count, A3Vec4 *lin_vel, A3Vec4 *ang_vel)
 *   rdi rows, esi count, rdx lin_vel, rcx ang_vel
 * Row layout (bytes): lin 0, ang_a 16, ang_b 32, ia 48, ib 64,
 *   inv_mass_a 80, inv_mass_b 84, eff_mass 88, bias 92,
 *   acc 96, lo 100, hi 104, mu 108, body_a 112, body_b 116, friction_of 120
 * --------------------------------------------------------------------- */
    .globl SYM(a3_phys_solve_rows)
    .p2align 4
SYM(a3_phys_solve_rows):
    test    esi, esi
    jz      9f
    mov     r9, rdi             /* rows base (friction rows index into it) */
    movaps  xmm15, [rip + K_SIGN_ALL]
1:
    movsxd  rax, dword ptr [rdi + 112]
    shl     rax, 4              /* body_a offset */
    movsxd  r8, dword ptr [rdi + 116]
    shl     r8, 4               /* body_b offset */

    /* bounds */
    movss   xmm12, [rdi + 100]  /* lo */
    movss   xmm13, [rdi + 104]  /* hi */
    movsxd  r10, dword ptr [rdi + 120]
    test    r10, r10
    js      2f
    shl     r10, 7              /* * 128 */
    movss   xmm13, [rdi + 108]  /* mu */
    mulss   xmm13, [r9 + r10 + 96] /* hi = mu * normal.acc */
    movaps  xmm12, xmm13
    xorps   xmm12, xmm15        /* lo = -hi */
2:
    /* vrel = ((dot(lin,vb) - dot(lin,va)) + dot(ang_b,wb)) - dot(ang_a,wa) */
    movups  xmm0, [rdi]
    movups  xmm1, [rdx + r8]
    mulps   xmm1, xmm0
    HSUM    xmm1, xmm8          /* d1 */
    movups  xmm2, [rdx + rax]
    mulps   xmm2, xmm0
    HSUM    xmm2, xmm8          /* d2 */
    movups  xmm3, [rdi + 32]
    movups  xmm4, [rcx + r8]
    mulps   xmm4, xmm3
    HSUM    xmm4, xmm8          /* d3 */
    movups  xmm3, [rdi + 16]
    movups  xmm5, [rcx + rax]
    mulps   xmm5, xmm3
    HSUM    xmm5, xmm8          /* d4 */
    subss   xmm1, xmm2
    addss   xmm1, xmm4
    subss   xmm1, xmm5          /* vrel */

    /* lambda = -(vrel + bias) * eff_mass */
    addss   xmm1, [rdi + 92]
    xorps   xmm1, xmm15
    mulss   xmm1, [rdi + 88]

    /* clamp accumulated impulse */
    movss   xmm2, [rdi + 96]    /* old */
    movaps  xmm3, xmm2
    addss   xmm3, xmm1          /* old + lambda */
    maxss   xmm3, xmm12
    minss   xmm3, xmm13
    movss   [rdi + 96], xmm3
    subss   xmm3, xmm2          /* lambda = acc - old */

    /* apply impulses (reload each body: safe even if body_a == body_b) */
    movaps  xmm4, xmm3
    mulss   xmm4, [rdi + 80]    /* la */
    shufps  xmm4, xmm4, 0
    movaps  xmm5, xmm0
    mulps   xmm5, xmm4
    movups  xmm6, [rdx + rax]
    subps   xmm6, xmm5
    movups  [rdx + rax], xmm6

    movaps  xmm4, xmm3
    mulss   xmm4, [rdi + 84]    /* lb */
    shufps  xmm4, xmm4, 0
    movaps  xmm5, xmm0
    mulps   xmm5, xmm4
    movups  xmm6, [rdx + r8]
    addps   xmm6, xmm5
    movups  [rdx + r8], xmm6

    shufps  xmm3, xmm3, 0       /* lambda broadcast */
    movups  xmm5, [rdi + 48]
    mulps   xmm5, xmm3
    movups  xmm6, [rcx + rax]
    subps   xmm6, xmm5
    movups  [rcx + rax], xmm6

    movups  xmm5, [rdi + 64]
    mulps   xmm5, xmm3
    movups  xmm6, [rcx + r8]
    addps   xmm6, xmm5
    movups  [rcx + r8], xmm6

    add     rdi, 128
    dec     esi
    jnz     1b
9:
    ret

/* ------------------------------------------------------------------------
 * void a3_phys_compute_aabbs(const A3Vec4 *centers, const A3Vec4 *cols,
 *        const A3Vec4 *half, A3Vec4 *out_min, A3Vec4 *out_max, u32 count)
 *   rdi centers, rsi cols, rdx half, rcx out_min, r8 out_max, r9d count
 * --------------------------------------------------------------------- */
    .globl SYM(a3_phys_compute_aabbs)
    .p2align 4
SYM(a3_phys_compute_aabbs):
    test    r9d, r9d
    jz      2f
    movaps  xmm15, [rip + K_ABS]
1:
    movups  xmm0, [rdx]         /* half extents */
    movaps  xmm1, xmm0
    shufps  xmm1, xmm1, 0x00    /* hx */
    movaps  xmm2, xmm0
    shufps  xmm2, xmm2, 0x55    /* hy */
    shufps  xmm0, xmm0, 0xAA    /* hz */
    movups  xmm3, [rsi]
    andps   xmm3, xmm15
    mulps   xmm3, xmm1          /* |c0| * hx */
    movups  xmm4, [rsi + 16]
    andps   xmm4, xmm15
    mulps   xmm4, xmm2          /* |c1| * hy */
    addps   xmm3, xmm4
    movups  xmm4, [rsi + 32]
    andps   xmm4, xmm15
    mulps   xmm4, xmm0          /* |c2| * hz */
    addps   xmm3, xmm4          /* extent */
    movups  xmm5, [rdi]
    movaps  xmm6, xmm5
    subps   xmm5, xmm3
    addps   xmm6, xmm3
    movups  [rcx], xmm5
    movups  [r8], xmm6
    add     rdi, 16
    add     rsi, 48
    add     rdx, 16
    add     rcx, 16
    add     r8, 16
    dec     r9d
    jnz     1b
2:
    ret

/* ------------------------------------------------------------------------
 * u32 a3_phys_sap_pairs(const A3Vec4 *mins, const A3Vec4 *maxs, u32 count,
 *        u32 *out_pairs, u32 max_pairs)
 *   rdi mins, rsi maxs, edx count, rcx out_pairs, r8d max_pairs -> eax
 * --------------------------------------------------------------------- */
    .globl SYM(a3_phys_sap_pairs)
    .p2align 4
SYM(a3_phys_sap_pairs):
    xor     eax, eax            /* found */
    xor     r9d, r9d            /* i */
1:
    cmp     r9d, edx
    jae     9f
    mov     r10, r9
    shl     r10, 4
    movups  xmm0, [rdi + r10]   /* min_i */
    movups  xmm1, [rsi + r10]   /* max_i */
    lea     r11d, [r9 + 1]      /* j */
2:
    cmp     r11d, edx
    jae     5f
    mov     r10, r11
    shl     r10, 4
    movups  xmm2, [rdi + r10]   /* min_j */
    ucomiss xmm2, xmm1          /* min_j.x > max_i.x ? */
    ja      5f
    movups  xmm3, [rsi + r10]   /* max_j */
    movaps  xmm4, xmm0
    cmpleps xmm4, xmm3          /* min_i <= max_j */
    movaps  xmm5, xmm2
    cmpleps xmm5, xmm1          /* min_j <= max_i */
    andps   xmm4, xmm5
    movmskps r10d, xmm4
    and     r10d, 7
    cmp     r10d, 7
    jne     4f
    cmp     eax, r8d
    jae     9f                  /* buffer full */
    mov     [rcx + rax * 8], r9d
    mov     [rcx + rax * 8 + 4], r11d
    inc     eax
4:
    inc     r11d
    jmp     2b
5:
    inc     r9d
    jmp     1b
9:
    ret

/* ------------------------------------------------------------------------
 * u32 a3_phys_ray_aabbs(const A3Vec4 *origin, const A3Vec4 *inv_dir, f32 t_max,
 *        const A3Vec4 *mins, const A3Vec4 *maxs, u32 count, f32 *out_t)
 *   rdi origin, rsi inv_dir, xmm0 t_max, rdx mins, rcx maxs, r8d count, r9 out_t -> eax
 * --------------------------------------------------------------------- */
    .globl SYM(a3_phys_ray_aabbs)
    .p2align 4
SYM(a3_phys_ray_aabbs):
    xor     eax, eax
    test    r8d, r8d
    jz      9f
    movups  xmm8, [rdi]         /* origin */
    movups  xmm9, [rsi]         /* inv_dir */
    shufps  xmm0, xmm0, 0
    andps   xmm0, [rip + K_MASK_W]   /* (0, 0, 0, t_max) */
    movaps  xmm10, xmm0
    movaps  xmm11, [rip + K_MASK_XYZ]
    movss   xmm12, [rip + K_MINUS1]
1:
    movups  xmm1, [rdx]
    subps   xmm1, xmm8
    mulps   xmm1, xmm9          /* t1 */
    movups  xmm2, [rcx]
    subps   xmm2, xmm8
    mulps   xmm2, xmm9          /* t2 */
    movaps  xmm3, xmm1
    minps   xmm3, xmm2          /* near */
    movaps  xmm4, xmm1
    maxps   xmm4, xmm2          /* far */
    andps   xmm3, xmm11         /* near.w = 0 */
    andps   xmm4, xmm11
    orps    xmm4, xmm10         /* far.w = t_max */
    /* tn = max(max(nx, nz), max(ny, nw)) */
    movaps  xmm5, xmm3
    movhlps xmm5, xmm3
    maxps   xmm3, xmm5
    movaps  xmm5, xmm3
    shufps  xmm5, xmm5, 0x55
    maxss   xmm3, xmm5
    /* tf = min(min(fx, fz), min(fy, fw)) */
    movaps  xmm5, xmm4
    movhlps xmm5, xmm4
    minps   xmm4, xmm5
    movaps  xmm5, xmm4
    shufps  xmm5, xmm5, 0x55
    minss   xmm4, xmm5
    ucomiss xmm4, xmm3          /* tf >= tn ? */
    jae     2f
    movss   [r9], xmm12
    jmp     3f
2:
    movss   [r9], xmm3
    inc     eax
3:
    add     rdx, 16
    add     rcx, 16
    add     r9, 4
    dec     r8d
    jnz     1b
9:
    ret

#if defined(__linux__) && defined(__ELF__)
    .section .note.GNU-stack,"",@progbits
#endif
