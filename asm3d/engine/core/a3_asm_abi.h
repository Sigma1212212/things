/*
 * ASM3D - a3_asm_abi.h (included by the .S kernel files)
 *
 * Kernel bodies are written once for the System V x86-64 ABI (Linux, macOS).
 * On Windows (Microsoft x64 ABI) each kernel gets a small entry stub that:
 *   - saves rdi, rsi and xmm6-xmm15 (callee-saved on Windows only),
 *   - moves the arguments from Windows positions (rcx, rdx, r8, r9, xmm0-3,
 *     stack) into System V positions (rdi, rsi, rdx, rcx, r8, r9, xmm0),
 *   - calls the unchanged body and restores the registers.
 * The math is identical on every OS, so results stay bit-exact.
 *
 * Usage:  A3_ASM_FUNC(name, ARGS_xxx)   followed by the System V body.
 * ARGS_ letters: P pointer/integer, I u32, F f32 (in Windows argument order).
 */
#ifndef A3_ASM_ABI_H
#define A3_ASM_ABI_H

#if defined(__APPLE__)
#  define SYM(x) _##x
#else
#  define SYM(x) x
#endif

#if defined(_WIN32)

/* stack argument n (1-based position, n >= 5) after WIN64_SAVE */
#define A3_WIN_ARG(n) [rsp + 224 + 8 * ((n) - 5)]

.macro WIN64_SAVE
    push    rdi
    push    rsi
    sub     rsp, 168
    movdqu  [rsp + 0], xmm6
    movdqu  [rsp + 16], xmm7
    movdqu  [rsp + 32], xmm8
    movdqu  [rsp + 48], xmm9
    movdqu  [rsp + 64], xmm10
    movdqu  [rsp + 80], xmm11
    movdqu  [rsp + 96], xmm12
    movdqu  [rsp + 112], xmm13
    movdqu  [rsp + 128], xmm14
    movdqu  [rsp + 144], xmm15
.endm

.macro WIN64_RESTORE_RET
    movdqu  xmm6, [rsp + 0]
    movdqu  xmm7, [rsp + 16]
    movdqu  xmm8, [rsp + 32]
    movdqu  xmm9, [rsp + 48]
    movdqu  xmm10, [rsp + 64]
    movdqu  xmm11, [rsp + 80]
    movdqu  xmm12, [rsp + 96]
    movdqu  xmm13, [rsp + 112]
    movdqu  xmm14, [rsp + 128]
    movdqu  xmm15, [rsp + 144]
    add     rsp, 168
    pop     rsi
    pop     rdi
    ret
.endm

/* (p, p, p, p) -> rdi, rsi, rdx, rcx */
.macro ARGS_PPPP
    mov     rdi, rcx
    mov     rsi, rdx
    mov     rdx, r8
    mov     rcx, r9
.endm
/* (p, p, f, u32) -> rdi, rsi, xmm0, edx */
.macro ARGS_PPFI
    mov     rdi, rcx
    mov     rsi, rdx
    movaps  xmm0, xmm2
    mov     edx, r9d
.endm
/* (p, p, p, p, f, u32) -> rdi, rsi, rdx, rcx, xmm0, r8d */
.macro ARGS_PPPPFI
    ARGS_PPPP
    movss   xmm0, dword ptr A3_WIN_ARG(5)
    mov     r8d, dword ptr A3_WIN_ARG(6)
.endm
/* (p, p, p, p, p, u32) -> rdi, rsi, rdx, rcx, r8, r9d */
.macro ARGS_PPPPPI
    ARGS_PPPP
    mov     r8, qword ptr A3_WIN_ARG(5)
    mov     r9d, dword ptr A3_WIN_ARG(6)
.endm
/* (p, p, u32, p, u32) -> rdi, rsi, edx, rcx, r8d */
.macro ARGS_PPIPI
    ARGS_PPPP
    mov     r8d, dword ptr A3_WIN_ARG(5)
.endm
/* (p, p, f, p, p, u32, p) -> rdi, rsi, xmm0, rdx, rcx, r8d, r9 */
.macro ARGS_PPFPPIP
    mov     rdi, rcx
    mov     rsi, rdx
    movaps  xmm0, xmm2
    mov     rdx, r9
    mov     rcx, qword ptr A3_WIN_ARG(5)
    mov     r8d, dword ptr A3_WIN_ARG(6)
    mov     r9, qword ptr A3_WIN_ARG(7)
.endm

#define A3_ASM_FUNC(name, shuffle) \
    .globl name ; .p2align 4 ; name: ; WIN64_SAVE ; shuffle ; call name##_sysv ; WIN64_RESTORE_RET ; \
    .p2align 4 ; name##_sysv:

#define A3_ASM_END_FILE

#else /* System V */

#define A3_ASM_FUNC(name, shuffle) .globl SYM(name) ; .p2align 4 ; SYM(name):
#define A3_ASM_END_FILE .section .note.GNU-stack,"",@progbits

#endif

#endif
