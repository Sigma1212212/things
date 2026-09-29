/*
 * ASM3D - a3_audio_kernels.h
 * Hot loops of the software mixer. x86-64 builds use SSE assembly
 * (a3_audio_x64.S); every kernel has a C reference with the exact same
 * operation order, and tests require bit-identical output.
 */
#ifndef A3_AUDIO_KERNELS_H
#define A3_AUDIO_KERNELS_H

#include "../core/a3_base.h"

A3_EXTERN_C_BEGIN

/* Mixes a mono source into an interleaved stereo buffer with gains that ramp
 * linearly (no clicks when a sound moves or changes volume):
 *   g = { gl0, gr0, dl, dr }:  left gain at frame i = gl0 + dl * i
 *   dst[2i]   = dst[2i]   + src[i] * (gl0 + dl * i)
 *   dst[2i+1] = dst[2i+1] + src[i] * (gr0 + dr * i)                        */
void a3_audio_mix_ramp(f32 *dst, const f32 *src, u32 frames, const f32 *g);
/* Float [-1, 1] -> 16-bit PCM with clamping and round-half-even. */
void a3_audio_f32_to_s16(i16 *dst, const f32 *src, u32 count);
const char *a3_audio_kernel_backend(void);

/* C references (always available, used by tests) */
void a3_audio_ref_mix_ramp(f32 *dst, const f32 *src, u32 frames, const f32 *g);
void a3_audio_ref_f32_to_s16(i16 *dst, const f32 *src, u32 count);

A3_EXTERN_C_END

#endif
