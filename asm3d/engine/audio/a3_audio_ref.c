/*
 * ASM3D - a3_audio_ref.c
 * C reference mixer kernels and the dispatch to assembly.
 */
#include "a3_audio_kernels.h"

void a3_audio_ref_mix_ramp(f32 *dst, const f32 *src, u32 frames, const f32 *g) {
    for (u32 i = 0; i < frames; ++i) {
        f32 fi = (f32)i;
        f32 gl = g[0] + g[2] * fi;
        f32 gr = g[1] + g[3] * fi;
        dst[2 * i] = dst[2 * i] + src[i] * gl;
        dst[2 * i + 1] = dst[2 * i + 1] + src[i] * gr;
    }
}

void a3_audio_ref_f32_to_s16(i16 *dst, const f32 *src, u32 count) {
    const f32 magic = 12582912.0f; /* 1.5 * 2^23: adding and subtracting rounds half to even */
    for (u32 i = 0; i < count; ++i) {
        f32 v = src[i] * 32767.0f;
        v = v > -32768.0f ? v : -32768.0f;   /* same semantics as SSE maxss (NaN -> -32768) */
        v = v < 32767.0f ? v : 32767.0f;     /* same semantics as SSE minss */
        v = (v + magic) - magic;             /* IEEE rules forbid folding this away */
        dst[i] = (i16)(i32)v;
    }
}

#if A3_HAS_X64_ASM
void a3_audio_asm_mix_ramp(f32 *dst, const f32 *src, u32 frames, const f32 *g);
void a3_audio_asm_f32_to_s16(i16 *dst, const f32 *src, u32 count);
void a3_audio_mix_ramp(f32 *dst, const f32 *src, u32 frames, const f32 *g) { a3_audio_asm_mix_ramp(dst, src, frames, g); }
void a3_audio_f32_to_s16(i16 *dst, const f32 *src, u32 count) { a3_audio_asm_f32_to_s16(dst, src, count); }
const char *a3_audio_kernel_backend(void) { return "x86-64 SSE assembly"; }
#else
void a3_audio_mix_ramp(f32 *dst, const f32 *src, u32 frames, const f32 *g) { a3_audio_ref_mix_ramp(dst, src, frames, g); }
void a3_audio_f32_to_s16(i16 *dst, const f32 *src, u32 count) { a3_audio_ref_f32_to_s16(dst, src, count); }
const char *a3_audio_kernel_backend(void) { return "C reference"; }
#endif
