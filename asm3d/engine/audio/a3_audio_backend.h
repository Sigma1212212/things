/*
 * ASM3D - a3_audio_backend.h (internal)
 * Output device interface. Implementations: a3_audio_wasapi.c (Windows),
 * a3_audio_alsa.c (Linux, libasound loaded at runtime), a3_audio_web.c
 * (browser, WebAudio), a3_audio_null.c.
 */
#ifndef A3_AUDIO_BACKEND_H
#define A3_AUDIO_BACKEND_H

#include "../core/a3_base.h"

typedef void (*A3AudioRenderFn)(f32 *out_stereo, u32 frames);

/* Starts a device thread that calls `render` whenever the device needs audio.
 * *rate: requested rate in, actual rate out. Returns false when no device. */
b32  a3_audio_backend_open(u32 *rate, A3AudioRenderFn render, char *name, usize name_cap);
void a3_audio_backend_close(void);

#endif
