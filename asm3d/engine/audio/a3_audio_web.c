/*
 * ASM3D - a3_audio_web.c
 * Browser output through WebAudio (web/asm3d.js). The page pulls audio on
 * its main thread (the only thread of the web build): each time WebAudio
 * needs samples it calls a3_web_audio_render(), which runs the mixer into a
 * buffer the page copies out. Browsers start audio after the first click
 * or key press.
 */
#include "a3_audio_backend.h"
#include "../core/a3_string.h"

#if A3_PLATFORM_WEB
A3_WASM_IMPORT("audio", "open") i32 js_audio_open(i32 rate);
A3_WASM_IMPORT("audio", "close") void js_audio_close(void);

#define WEB_AUDIO_MAX_FRAMES 8192
static A3AudioRenderFn g_render;
static f32 g_buf[WEB_AUDIO_MAX_FRAMES * 2];

b32 a3_audio_backend_open(u32 *rate, A3AudioRenderFn render, char *name, usize name_cap) {
    i32 r = js_audio_open((i32)*rate);
    if (r <= 0) return 0;
    *rate = (u32)r;
    g_render = render;
    a3_strcpy(name, name_cap, "WebAudio");
    return 1;
}

void a3_audio_backend_close(void) {
    g_render = 0;
    js_audio_close();
}

A3_WASM_EXPORT("a3_web_audio_render") f32 *a3_web_audio_render(u32 frames) {
    if (frames > WEB_AUDIO_MAX_FRAMES) frames = WEB_AUDIO_MAX_FRAMES;
    if (g_render) g_render(g_buf, frames);
    else a3_memset(g_buf, 0, sizeof(f32) * 2 * frames);
    return g_buf;
}
#endif
