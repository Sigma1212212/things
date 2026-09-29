/*
 * ASM3D - a3_audio_x64.S
 * SSE mixer kernels (System V bodies; Windows entry stubs in a3_asm_abi.h).
 * Operation order matches a3_audio_ref.c exactly, so output is bit-identical;
 * tests/test_audio.c compares every output sample.
 */
#include "../core/a3_asm_abi.h"

    .intel_syntax noprefix

    .section .rodata
    .p2align 4
K_IDX0:   .long 0, 0, 1, 1
K_TWO:    .long 2, 2, 2, 2
K_SCALE:  .float 32767.0, 32767.0, 32767.0, 32767.0
K_LO:     .float -32768.0, -32768.0, -32768.0, -32768.0
K_HI:     .float 32767.0, 32767.0, 32767.0, 32767.0
K_MAGIC:  .float 12582912.0, 12582912.0, 12582912.0, 12582912.0

    .text

/* ------------------------------------------------------------------------
 * void a3_audio_asm_mix_ramp(f32 *dst, const f32 *src, u32 frames, const f32 *g)
 *   rdi dst (stereo), rsi src (mono), edx frames, rcx g = {gl0, gr0, dl, dr}
 *   Two frames per iteration: lanes = (L_i, R_i, L_i+1, R_i+1).
 * --------------------------------------------------------------------- */
A3_ASM_FUNC(a3_audio_asm_mix_ramp, ARGS_PPPP)
    movss   xmm4, dword ptr [rcx]
    movss   xmm5, dword ptr [rcx + 4]
    unpcklps xmm4, xmm5            /* gl0 gr0 . . */
    movlhps xmm4, xmm4             /* gl0 gr0 gl0 gr0 */
    movss   xmm5, dword ptr [rcx + 8]
    movss   xmm6, dword ptr [rcx + 12]
    unpcklps xmm5, xmm6
    movlhps xmm5, xmm5             /* dl dr dl dr */
    movdqa  xmm7, xmmword ptr [rip + K_IDX0]
    movdqa  xmm8, xmmword ptr [rip + K_TWO]
    mov     eax, edx
    shr     eax, 1
    jz      2f
1:
    cvtdq2ps xmm0, xmm7            /* i i i+1 i+1 */
    mulps   xmm0, xmm5             /* d * i */
    addps   xmm0, xmm4             /* g0 + d * i */
    movsd   xmm1, qword ptr [rsi]  /* s0 s1 */
    unpcklps xmm1, xmm1            /* s0 s0 s1 s1 */
    mulps   xmm1, xmm0
    movups  xmm2, xmmword ptr [rdi]
    addps   xmm2, xmm1
    movups  xmmword ptr [rdi], xmm2
    paddd   xmm7, xmm8
    add     rsi, 8
    add     rdi, 16
    dec     eax
    jnz     1b
2:
    test    edx, 1
    jz      3f
    cvtdq2ps xmm0, xmm7
    mulps   xmm0, xmm5
    addps   xmm0, xmm4
    movss   xmm1, dword ptr [rsi]
    unpcklps xmm1, xmm1
    mulps   xmm1, xmm0
    movsd   xmm2, qword ptr [rdi]
    addps   xmm2, xmm1
    movsd   qword ptr [rdi], xmm2
3:
    ret

/* ------------------------------------------------------------------------
 * void a3_audio_asm_f32_to_s16(i16 *dst, const f32 *src, u32 count)
 *   rdi dst, rsi src, edx count
 * --------------------------------------------------------------------- */
A3_ASM_FUNC(a3_audio_asm_f32_to_s16, ARGS_PPPP)
    movaps  xmm4, xmmword ptr [rip + K_SCALE]
    movaps  xmm5, xmmword ptr [rip + K_LO]
    movaps  xmm6, xmmword ptr [rip + K_HI]
    movaps  xmm7, xmmword ptr [rip + K_MAGIC]
    mov     eax, edx
    shr     eax, 2
    jz      2f
1:
    movups  xmm0, xmmword ptr [rsi]
    mulps   xmm0, xmm4
    maxps   xmm0, xmm5
    minps   xmm0, xmm6
    addps   xmm0, xmm7
    subps   xmm0, xmm7
    cvttps2dq xmm0, xmm0
    packssdw xmm0, xmm0
    movq    qword ptr [rdi], xmm0
    add     rsi, 16
    add     rdi, 8
    dec     eax
    jnz     1b
2:
    and     edx, 3
    jz      4f
3:
    movss   xmm0, dword ptr [rsi]
    mulss   xmm0, xmm4
    maxss   xmm0, xmm5
    minss   xmm0, xmm6
    addss   xmm0, xmm7
    subss   xmm0, xmm7
    cvttss2si eax, xmm0
    mov     word ptr [rdi], ax
    add     rsi, 4
    add     rdi, 2
    dec     edx
    jnz     3b
4:
    ret

A3_ASM_END_FILE
