/*
 * ASM3D - a3_audio_alsa.c
 * Linux audio output through ALSA (which also reaches PulseAudio/PipeWire).
 * libasound is loaded at runtime, so the engine starts even without it.
 */
#include "a3_audio_backend.h"
#include "../core/a3_log.h"
#include "../core/a3_string.h"
#include "../core/a3_memory.h"
#include "../platform/a3_platform.h"

#if A3_PLATFORM_LINUX

#include <dlfcn.h>
#include <errno.h>

typedef struct snd_pcm snd_pcm_t;
#define A3_SND_PCM_STREAM_PLAYBACK 0
#define A3_SND_PCM_FORMAT_FLOAT_LE 14
#define A3_SND_PCM_ACCESS_RW_INTERLEAVED 3

static struct {
    void *lib;
    int (*open)(snd_pcm_t **, const char *, int, int);
    int (*set_params)(snd_pcm_t *, int, int, unsigned, unsigned, int, unsigned);
    long (*writei)(snd_pcm_t *, const void *, unsigned long);
    int (*recover)(snd_pcm_t *, int, int);
    int (*close)(snd_pcm_t *);
    int (*drop)(snd_pcm_t *);
    snd_pcm_t *pcm;
    A3Thread *thread;
    volatile int quit;
    A3AudioRenderFn render;
} g_al;

#define BLOCK 480  /* 10 ms at 48 kHz */

static void alsa_quiet(const char *file, int line, const char *fn, int err, const char *fmt, ...) {
    A3_UNUSED(file); A3_UNUSED(line); A3_UNUSED(fn); A3_UNUSED(err); A3_UNUSED(fmt);
}

static void audio_thread(void *user) {
    A3_UNUSED(user);
    static f32 buf[BLOCK * 2];
    while (!g_al.quit) {
        g_al.render(buf, BLOCK);
        long left = BLOCK;
        const f32 *p = buf;
        while (left > 0 && !g_al.quit) {
            long w = g_al.writei(g_al.pcm, p, (unsigned long)left);
            if (w < 0) {
                if (g_al.recover(g_al.pcm, (int)w, 1) < 0) { a3_sleep_ms(10); break; }
                continue;
            }
            left -= w;
            p += w * 2;
        }
    }
}

b32 a3_audio_backend_open(u32 *rate, A3AudioRenderFn render, char *name, usize name_cap) {
    a3_zero_struct(&g_al);
    g_al.lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!g_al.lib) return 0;
    g_al.open = (int (*)(snd_pcm_t **, const char *, int, int))dlsym(g_al.lib, "snd_pcm_open");
    g_al.set_params = (int (*)(snd_pcm_t *, int, int, unsigned, unsigned, int, unsigned))dlsym(g_al.lib, "snd_pcm_set_params");
    g_al.writei = (long (*)(snd_pcm_t *, const void *, unsigned long))dlsym(g_al.lib, "snd_pcm_writei");
    g_al.recover = (int (*)(snd_pcm_t *, int, int))dlsym(g_al.lib, "snd_pcm_recover");
    g_al.close = (int (*)(snd_pcm_t *))dlsym(g_al.lib, "snd_pcm_close");
    g_al.drop = (int (*)(snd_pcm_t *))dlsym(g_al.lib, "snd_pcm_drop");
    if (!g_al.open || !g_al.set_params || !g_al.writei || !g_al.recover || !g_al.close) goto fail;
    {
        /* libasound prints raw errors to stderr when there is no sound card;
         * the engine reports that itself in plain language */
        int (*set_handler)(void (*)(const char *, int, const char *, int, const char *, ...)) =
            (int (*)(void (*)(const char *, int, const char *, int, const char *, ...)))dlsym(g_al.lib, "snd_lib_error_set_handler");
        if (set_handler) set_handler(alsa_quiet);
    }
    if (g_al.open(&g_al.pcm, "default", A3_SND_PCM_STREAM_PLAYBACK, 0) < 0) { g_al.pcm = 0; goto fail; }
    if (g_al.set_params(g_al.pcm, A3_SND_PCM_FORMAT_FLOAT_LE, A3_SND_PCM_ACCESS_RW_INTERLEAVED, 2, *rate, 1, 40000) < 0) goto fail;
    g_al.render = render;
    g_al.thread = a3_thread_create(audio_thread, 0, "a3-audio");
    if (!g_al.thread) goto fail;
    a3_strcpy(name, name_cap, "ALSA (default device)");
    return 1;
fail:
    if (g_al.pcm) g_al.close(g_al.pcm);
    dlclose(g_al.lib);
    a3_zero_struct(&g_al);
    return 0;
}

void a3_audio_backend_close(void) {
    if (!g_al.thread) return;
    g_al.quit = 1;
    a3_thread_join(g_al.thread);
    if (g_al.drop) g_al.drop(g_al.pcm);
    g_al.close(g_al.pcm);
    dlclose(g_al.lib);
    a3_zero_struct(&g_al);
}

#endif /* A3_PLATFORM_LINUX */
