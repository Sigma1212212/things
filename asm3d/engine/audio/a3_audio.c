/*
 * ASM3D - a3_audio.c
 * Mixer, clips, WAV codec, sound synthesizer, AudioSource / AudioListener.
 *
 * Threading: the device thread calls a3_audio_render(); the game thread
 * changes voices through the functions below. Both take one mutex for a few
 * microseconds per call, so the mixer never waits on the game.
 */
#include "a3_audio.h"
#include "a3_audio_kernels.h"
#include "a3_audio_backend.h"
#include "../core/a3_log.h"
#include "../core/a3_memory.h"
#include "../core/a3_string.h"
#include "../core/a3_format.h"
#include "../core/a3_hash.h"
#include "../platform/a3_platform.h"
#include "../scene/a3_components.h"
#include "../resource/a3_assets.h"

#define MIX_BLOCK 512

typedef struct Clip {
    b32 used;
    u64 key;
    char path[A3_PATH_MAX];
    f32 *data;          /* interleaved */
    u32 frames, channels, rate;
    b32 failed;
} Clip;

typedef struct Voice {
    b32 active;
    u32 gen;
    u32 clip;
    f64 pos;            /* in source frames */
    f32 pitch;
    b32 loop;
    f32 gl, gr;         /* current gains */
    f32 tl, tr;         /* target gains */
} Voice;

static struct {
    b32 init;
    b32 device;
    char device_name[96];
    u32 rate;
    f32 master;
    A3Mutex *lock;
    Clip clips[A3_AUDIO_MAX_CLIPS];
    Voice voices[A3_AUDIO_MAX_VOICES];
    f32 scratch_l[MIX_BLOCK], scratch_r[MIX_BLOCK];
} g_au;

static void lock(void) { if (g_au.lock) a3_mutex_lock(g_au.lock); }
static void unlock(void) { if (g_au.lock) a3_mutex_unlock(g_au.lock); }

/* ======================================================================== */
/* Mixer                                                                    */
/* ======================================================================== */

/* Resamples `n` output frames of one channel with linear interpolation. */
static u32 resample(const Clip *c, u32 ch, f64 pos, f64 step, b32 loop, f32 *out, u32 n) {
    u32 produced = 0;
    for (; produced < n; ++produced) {
        if (pos >= (f64)c->frames) {
            if (!loop) break;
            pos -= (f64)c->frames * a3_floorf((f32)(pos / (f64)c->frames));
            if (pos >= (f64)c->frames) pos = 0;
        }
        u32 i0 = (u32)pos;
        u32 i1 = i0 + 1;
        if (i1 >= c->frames) i1 = loop ? 0 : i0;
        f32 frac = (f32)(pos - (f64)i0);
        f32 a = c->data[(usize)i0 * c->channels + ch], b = c->data[(usize)i1 * c->channels + ch];
        out[produced] = a + (b - a) * frac;
        pos += step;
    }
    return produced;
}

void a3_audio_render(f32 *out, u32 frames) {
    a3_memset(out, 0, sizeof(f32) * 2 * frames);
    if (!g_au.init) return;
    lock();
    for (u32 done = 0; done < frames;) {
        u32 n = frames - done < MIX_BLOCK ? frames - done : MIX_BLOCK;
        f32 *dst = out + (usize)done * 2;
        for (u32 v = 0; v < A3_AUDIO_MAX_VOICES; ++v) {
            Voice *vo = &g_au.voices[v];
            if (!vo->active) continue;
            const Clip *c = &g_au.clips[vo->clip];
            if (!c->used || !c->data || !c->frames) { vo->active = 0; continue; }
            f64 step = (f64)vo->pitch * (f64)c->rate / (f64)g_au.rate;
            f32 inv = 1.0f / (f32)n;
            u32 got;
            if (c->channels == 1) {
                got = resample(c, 0, vo->pos, step, vo->loop, g_au.scratch_l, n);
                f32 g[4] = { vo->gl, vo->gr, (vo->tl - vo->gl) * inv, (vo->tr - vo->gr) * inv };
                a3_audio_mix_ramp(dst, g_au.scratch_l, got, g);
            } else {
                got = resample(c, 0, vo->pos, step, vo->loop, g_au.scratch_l, n);
                resample(c, 1, vo->pos, step, vo->loop, g_au.scratch_r, n);
                f32 gl[4] = { vo->gl, 0, (vo->tl - vo->gl) * inv, 0 };
                f32 gr[4] = { 0, vo->gr, 0, (vo->tr - vo->gr) * inv };
                a3_audio_mix_ramp(dst, g_au.scratch_l, got, gl);
                a3_audio_mix_ramp(dst, g_au.scratch_r, got, gr);
            }
            vo->gl = vo->tl;
            vo->gr = vo->tr;
            vo->pos += step * (f64)got;
            if (vo->loop && vo->pos >= (f64)c->frames) vo->pos -= (f64)c->frames * a3_floorf((f32)(vo->pos / (f64)c->frames));
            if (got < n) vo->active = 0; /* reached the end */
        }
        done += n;
    }
    unlock();
    /* master volume + hard limit */
    f32 m = g_au.master;
    for (u32 i = 0; i < frames * 2; ++i) {
        f32 s = out[i] * m;
        out[i] = s > 1.0f ? 1.0f : (s < -1.0f ? -1.0f : s);
    }
}

/* ======================================================================== */
/* Init                                                                     */
/* ======================================================================== */

b32 a3_audio_init(const A3AudioDesc *desc) {
    if (g_au.init) return 1;
    A3AudioDesc d;
    a3_zero_struct(&d);
    if (desc) d = *desc;
    g_au.lock = a3_mutex_create(); /* NULL on the web build: single threaded there */
    g_au.master = 1.0f;
    g_au.rate = d.sample_rate ? d.sample_rate : 48000;
    g_au.init = 1;
    a3_strcpy(g_au.device_name, sizeof(g_au.device_name), "none");
    if (!d.null_device) {
        u32 rate = g_au.rate;
        char name[96] = "";
        if (a3_audio_backend_open(&rate, a3_audio_render, name, sizeof(name))) {
            g_au.device = 1;
            g_au.rate = rate;
            a3_strcpy(g_au.device_name, sizeof(g_au.device_name), name);
            A3_INFO("audio", "output: %s, %u Hz, mixer: %s", name, rate, a3_audio_kernel_backend());
        } else {
            a3_log_hint(A3_LOG_INFO, "audio", "Connect speakers or headphones and restart to hear sound.", "no audio device found - sound is disabled");
        }
    }
    return 1;
}

void a3_audio_shutdown(void) {
    if (!g_au.init) return;
    if (g_au.device) a3_audio_backend_close();
    for (u32 i = 0; i < A3_AUDIO_MAX_CLIPS; ++i) a3_free(g_au.clips[i].data);
    if (g_au.lock) a3_mutex_destroy(g_au.lock);
    a3_zero_struct(&g_au);
}

b32 a3_audio_device_ok(void) { return g_au.device; }
const char *a3_audio_device_name(void) { return g_au.device_name; }
u32 a3_audio_sample_rate(void) { return g_au.rate ? g_au.rate : 48000; }
void a3_audio_set_master_volume(f32 v) { g_au.master = a3_clampf(v, 0, 4); }
f32 a3_audio_master_volume(void) { return g_au.master; }

/* ======================================================================== */
/* Clips                                                                    */
/* ======================================================================== */

static u32 clip_slot(u64 key, const char *path) {
    for (u32 i = 1; i < A3_AUDIO_MAX_CLIPS; ++i) if (g_au.clips[i].used && g_au.clips[i].key == key) return i;
    for (u32 i = 1; i < A3_AUDIO_MAX_CLIPS; ++i) if (!g_au.clips[i].used) {
        Clip *c = &g_au.clips[i];
        a3_zero_struct(c);
        c->used = 1;
        c->key = key;
        a3_strcpy(c->path, sizeof(c->path), path);
        return i;
    }
    A3_ERROR("audio", "too many sound clips (max %d)", A3_AUDIO_MAX_CLIPS - 1);
    return 0;
}

u32 a3_audio_clip_from_samples(const char *name, const f32 *samples, u32 frames, u32 channels, u32 rate) {
    if (!name || !samples || !frames || channels < 1 || channels > 2 || !rate) return 0;
    if (!g_au.init) a3_audio_init(0);
    f32 *copy = (f32 *)a3_malloc(sizeof(f32) * frames * channels, A3_MEM_AUDIO);
    if (!copy) return 0;
    a3_memcpy(copy, samples, sizeof(f32) * frames * channels);
    lock();
    u32 id = clip_slot(a3_hash_str(name), name);
    if (id) {
        Clip *c = &g_au.clips[id];
        for (u32 v = 0; v < A3_AUDIO_MAX_VOICES; ++v) if (g_au.voices[v].clip == id) g_au.voices[v].active = 0;
        a3_free(c->data);
        c->data = copy;
        c->frames = frames;
        c->channels = channels;
        c->rate = rate;
        c->failed = 0;
    } else a3_free(copy);
    unlock();
    return id;
}

u32 a3_audio_clip(const char *path) {
    if (!path || !*path) return 0;
    if (!g_au.init) a3_audio_init(0);
    u64 key = a3_hash_str(path);
    lock();
    for (u32 i = 1; i < A3_AUDIO_MAX_CLIPS; ++i)
        if (g_au.clips[i].used && g_au.clips[i].key == key) { u32 r = g_au.clips[i].failed ? 0 : i; unlock(); return r; }
    unlock();
    f32 *samples = 0;
    u32 frames = 0, channels = 1, rate = 44100;
    char err[160] = "";
    if (a3_str_starts_with(path, "builtin:")) {
        const char *name = path + 8;
        u32 s = 0;
        for (u32 i = 1; i < A3_SOUND_COUNT; ++i) if (a3_streq(a3_builtin_sound_names[i], name)) s = i;
        if (s) samples = a3_audio_synth(s, 44100, &frames);
        else a3_snprintf(err, sizeof(err), "there is no built-in sound called '%s'", name);
    } else {
        char abs[A3_PATH_MAX * 2];
        a3_assets_path(path, abs, sizeof(abs));
        A3FileData fd;
        if (a3_file_read_all(abs, A3_MEM_TEMP, &fd) != A3_OK) a3_snprintf(err, sizeof(err), "file not found");
        else {
            if (!a3_wav_decode(fd.data, fd.size, &samples, &frames, &channels, &rate, err, sizeof(err))) samples = 0;
            a3_free(fd.data);
        }
    }
    u32 id = 0;
    if (samples) {
        id = a3_audio_clip_from_samples(path, samples, frames, channels, rate);
        a3_free(samples);
    } else {
        a3_log_hint(A3_LOG_ERROR, "audio", "Use a .wav file (16-bit or float) or pick a Built-in Sound on the Audio Source.",
                    "could not load sound '%s': %s", path, err);
        lock();
        u32 slot = clip_slot(key, path); /* remember the failure: log once */
        if (slot) g_au.clips[slot].failed = 1;
        unlock();
    }
    return id;
}

f32 a3_audio_clip_seconds(u32 clip) {
    if (clip == 0 || clip >= A3_AUDIO_MAX_CLIPS || !g_au.clips[clip].used || !g_au.clips[clip].rate) return 0;
    return (f32)g_au.clips[clip].frames / (f32)g_au.clips[clip].rate;
}
u32 a3_audio_clip_channels(u32 clip) { return clip && clip < A3_AUDIO_MAX_CLIPS ? g_au.clips[clip].channels : 0; }

/* ======================================================================== */
/* Voices                                                                   */
/* ======================================================================== */

static Voice *voice_get(A3Voice v) {
    u32 idx = v & 0xFF, gen = v >> 8;
    if (!v || idx >= A3_AUDIO_MAX_VOICES) return 0;
    Voice *vo = &g_au.voices[idx];
    return vo->active && vo->gen == gen ? vo : 0;
}

A3Voice a3_audio_play(u32 clip, f32 volume, f32 pitch, b32 loop) {
    if (!g_au.init || clip == 0 || clip >= A3_AUDIO_MAX_CLIPS || !g_au.clips[clip].used || g_au.clips[clip].failed) return 0;
    lock();
    u32 slot = A3_AUDIO_MAX_VOICES;
    for (u32 i = 0; i < A3_AUDIO_MAX_VOICES; ++i) if (!g_au.voices[i].active) { slot = i; break; }
    if (slot == A3_AUDIO_MAX_VOICES) {
        /* all voices busy: replace the quietest one-shot */
        f32 best = 1e30f;
        for (u32 i = 0; i < A3_AUDIO_MAX_VOICES; ++i) {
            f32 lvl = g_au.voices[i].tl + g_au.voices[i].tr;
            if (!g_au.voices[i].loop && lvl < best) { best = lvl; slot = i; }
        }
        if (slot == A3_AUDIO_MAX_VOICES) { unlock(); return 0; }
    }
    Voice *vo = &g_au.voices[slot];
    u32 gen = (vo->gen + 1) & 0xFFFFFF;
    if (!gen) gen = 1;
    a3_zero_struct(vo);
    vo->active = 1;
    vo->gen = gen;
    vo->clip = clip;
    vo->pitch = a3_clampf(pitch, 0.05f, 8.0f);
    vo->loop = loop;
    f32 l, r;
    a3_audio_pan_gains(volume, 0, &l, &r);
    vo->gl = vo->tl = l;
    vo->gr = vo->tr = r;
    unlock();
    return (gen << 8) | slot;
}

void a3_audio_stop(A3Voice v) { lock(); Voice *vo = voice_get(v); if (vo) vo->active = 0; unlock(); }
b32 a3_audio_playing(A3Voice v) { lock(); b32 r = voice_get(v) != 0; unlock(); return r; }
void a3_audio_set_gains(A3Voice v, f32 left, f32 right) { lock(); Voice *vo = voice_get(v); if (vo) { vo->tl = left; vo->tr = right; } unlock(); }
void a3_audio_set_pitch(A3Voice v, f32 pitch) { lock(); Voice *vo = voice_get(v); if (vo) vo->pitch = a3_clampf(pitch, 0.05f, 8.0f); unlock(); }
void a3_audio_stop_all(void) { lock(); for (u32 i = 0; i < A3_AUDIO_MAX_VOICES; ++i) g_au.voices[i].active = 0; unlock(); }
u32 a3_audio_active_voices(void) { u32 n = 0; lock(); for (u32 i = 0; i < A3_AUDIO_MAX_VOICES; ++i) n += g_au.voices[i].active; unlock(); return n; }

void a3_audio_pan_gains(f32 volume, f32 pan, f32 *left, f32 *right) {
    f32 a = (a3_clampf(pan, -1, 1) + 1.0f) * (A3_PI * 0.25f);
    f32 s, c;
    a3_sincosf(a, &s, &c);
    /* equal power, normalised so pan 0 gives `volume` on both sides */
    *left = volume * c * 1.41421356f;
    *right = volume * s * 1.41421356f;
}

/* ======================================================================== */
/* WAV                                                                      */
/* ======================================================================== */

static u32 rd16(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8); }
static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

b32 a3_wav_decode(const u8 *d, usize size, f32 **out, u32 *out_frames, u32 *out_ch, u32 *out_rate, char *err, usize cap) {
    *out = 0;
    if (size < 12 || a3_memcmp(d, "RIFF", 4) || a3_memcmp(d + 8, "WAVE", 4)) { a3_snprintf(err, cap, "not a WAV file"); return 0; }
    u32 fmt = 0, ch = 0, rate = 0, bits = 0;
    const u8 *data = 0;
    u32 data_size = 0;
    usize p = 12;
    while (p + 8 <= size) {
        u32 len = rd32(d + p + 4);
        const u8 *body = d + p + 8;
        if (p + 8 + (usize)len > size) len = (u32)(size - p - 8); /* tolerate truncated files */
        if (!a3_memcmp(d + p, "fmt ", 4) && len >= 16) {
            fmt = rd16(body);
            ch = rd16(body + 2);
            rate = rd32(body + 4);
            bits = rd16(body + 14);
            if (fmt == 0xFFFE && len >= 26) fmt = rd16(body + 24); /* WAVE_FORMAT_EXTENSIBLE: sub-format GUID */
        } else if (!a3_memcmp(d + p, "data", 4)) {
            data = body;
            data_size = len;
        }
        p += 8 + (usize)len + (len & 1);
    }
    if (!fmt || !data) { a3_snprintf(err, cap, "WAV file has no %s", fmt ? "sound data" : "format chunk"); return 0; }
    if (!(fmt == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) && !(fmt == 3 && bits == 32)) {
        a3_snprintf(err, cap, "unsupported WAV encoding (format %u, %u-bit); save it as 16-bit PCM", fmt, bits);
        return 0;
    }
    if (ch < 1 || ch > 8 || rate < 1000 || rate > 384000) { a3_snprintf(err, cap, "unusual WAV (%u channels, %u Hz)", ch, rate); return 0; }
    u32 bps = bits / 8, frames = data_size / (bps * ch), nch = ch >= 2 ? 2 : 1;
    if (!frames) { a3_snprintf(err, cap, "WAV file is empty"); return 0; }
    f32 *s = (f32 *)a3_malloc(sizeof(f32) * frames * nch, A3_MEM_AUDIO);
    if (!s) { a3_snprintf(err, cap, "out of memory"); return 0; }
    for (u32 f = 0; f < frames; ++f) {
        for (u32 c = 0; c < nch; ++c) {
            const u8 *q = data + ((usize)f * ch + c) * bps;
            f32 v;
            if (fmt == 3) { u32 u = rd32(q); a3_memcpy(&v, &u, 4); }
            else if (bits == 8) v = ((f32)q[0] - 128.0f) / 128.0f;
            else if (bits == 16) v = (f32)(i16)rd16(q) / 32768.0f;
            else if (bits == 24) { i32 x = (i32)(((u32)q[0] << 8) | ((u32)q[1] << 16) | ((u32)q[2] << 24)) >> 8; v = (f32)x / 8388608.0f; }
            else v = (f32)((f64)(i32)rd32(q) / 2147483648.0);
            s[(usize)f * nch + c] = v;
        }
    }
    *out = s;
    *out_frames = frames;
    *out_ch = nch;
    *out_rate = rate;
    return 1;
}

b32 a3_wav_encode(const f32 *samples, u32 frames, u32 channels, u32 rate, u8 **out_data, usize *out_size) {
    usize bytes = (usize)frames * channels * 2, total = 44 + bytes;
    u8 *d = (u8 *)a3_malloc(total, A3_MEM_AUDIO);
    if (!d) return 0;
    a3_memcpy(d, "RIFF", 4);
    u32 v = (u32)(total - 8); a3_memcpy(d + 4, &v, 4);
    a3_memcpy(d + 8, "WAVEfmt ", 8);
    v = 16; a3_memcpy(d + 16, &v, 4);
    u16 w = 1; a3_memcpy(d + 20, &w, 2);
    w = (u16)channels; a3_memcpy(d + 22, &w, 2);
    a3_memcpy(d + 24, &rate, 4);
    v = rate * channels * 2; a3_memcpy(d + 28, &v, 4);
    w = (u16)(channels * 2); a3_memcpy(d + 32, &w, 2);
    w = 16; a3_memcpy(d + 34, &w, 2);
    a3_memcpy(d + 36, "data", 4);
    v = (u32)bytes; a3_memcpy(d + 40, &v, 4);
    a3_audio_f32_to_s16((i16 *)(d + 44), samples, frames * channels);
    *out_data = d;
    *out_size = total;
    return 1;
}

/* ======================================================================== */
/* Synthesizer                                                              */
/* ======================================================================== */

const char *const a3_builtin_sound_names[A3_SOUND_COUNT] = {
    "none", "jump", "coin", "hit", "explosion", "laser", "click", "footstep", "powerup", "hurt", "wind", "engine", "beep", "skid", "rain",
};

enum { W_SQUARE = 0, W_SAW, W_SINE, W_NOISE, W_TRIANGLE };
typedef struct Synth {
    int wave;
    f32 f0, f1;               /* start / end frequency (exponential slide) */
    f32 attack, sustain, decay;
    f32 duty;
    f32 vib_depth, vib_speed;
    f32 arp_time, arp_mul;
    f32 lowpass;              /* one-pole coefficient (1 = off) */
    f32 volume;
    u32 seed;
} Synth;

static const Synth g_synth[A3_SOUND_COUNT] = {
    [A3_SOUND_JUMP] = { W_SQUARE, 280, 620, 0.0f, 0.06f, 0.16f, 0.5f, 0, 0, 0, 1, 1, 0.45f, 1 },
    [A3_SOUND_COIN] = { W_SQUARE, 988, 988, 0.0f, 0.05f, 0.30f, 0.5f, 0, 0, 0.07f, 1.335f, 1, 0.4f, 2 },
    [A3_SOUND_HIT] = { W_NOISE, 900, 180, 0.0f, 0.02f, 0.16f, 0.5f, 0, 0, 0, 1, 0.6f, 0.7f, 3 },
    [A3_SOUND_EXPLOSION] = { W_NOISE, 420, 40, 0.0f, 0.10f, 0.90f, 0.5f, 0, 0, 0, 1, 0.35f, 0.9f, 4 },
    [A3_SOUND_LASER] = { W_SAW, 1400, 180, 0.0f, 0.04f, 0.16f, 0.5f, 0, 0, 0, 1, 0.8f, 0.35f, 5 },
    [A3_SOUND_CLICK] = { W_SINE, 1200, 900, 0.0f, 0.0f, 0.04f, 0.5f, 0, 0, 0, 1, 1, 0.5f, 6 },
    [A3_SOUND_FOOTSTEP] = { W_NOISE, 320, 120, 0.0f, 0.01f, 0.09f, 0.5f, 0, 0, 0, 1, 0.25f, 0.6f, 7 },
    [A3_SOUND_POWERUP] = { W_SQUARE, 330, 990, 0.0f, 0.30f, 0.30f, 0.35f, 0.08f, 12, 0, 1, 1, 0.35f, 8 },
    [A3_SOUND_HURT] = { W_SAW, 420, 140, 0.0f, 0.05f, 0.22f, 0.5f, 0.05f, 30, 0, 1, 0.7f, 0.45f, 9 },
    [A3_SOUND_WIND] = { W_NOISE, 2000, 2000, 0.0f, 4.0f, 0.0f, 0.5f, 0, 0, 0, 1, 0.02f, 1.2f, 10 },
    [A3_SOUND_ENGINE] = { W_SAW, 60, 60, 0.0f, 1.0f, 0.0f, 0.5f, 0.03f, 6, 0, 1, 0.15f, 0.5f, 11 },
    [A3_SOUND_BEEP] = { W_SQUARE, 880, 880, 0.005f, 0.10f, 0.05f, 0.5f, 0, 0, 0, 1, 1, 0.3f, 12 },
    [A3_SOUND_SKID] = { W_NOISE, 1000, 1000, 0.0f, 1.0f, 0.0f, 0.5f, 0, 0, 0, 1, 1, 0.4f, 13 },   /* see synth_skid */
    [A3_SOUND_RAIN] = { W_NOISE, 1000, 1000, 0.0f, 1.0f, 0.0f, 0.5f, 0, 0, 0, 1, 1, 0.4f, 14 },   /* see synth_rain */
};

/* two-pole resonator (band-pass) */
typedef struct Reso { f32 a1, a2, y1, y2; } Reso;
static void reso_init(Reso *r, f32 freq, f32 bandwidth, u32 rate) {
    f32 rr = a3_expf(-A3_PI * bandwidth / (f32)rate);
    r->a1 = 2.0f * rr * a3_cosf(A3_TAU * freq / (f32)rate);
    r->a2 = -rr * rr;
    r->y1 = r->y2 = 0;
}
static f32 reso_step(Reso *r, f32 x) { f32 y = x + r->a1 * r->y1 + r->a2 * r->y2; r->y2 = r->y1; r->y1 = y; return y; }

/* A 4-cylinder engine at 2000 rpm: every firing (rpm / 30 per second) kicks
 * the exhaust resonances and a short burst of combustion noise; the
 * cylinders differ a little in strength, which gives the lumpy crank-rate
 * rhythm of a real engine. 80 firings = exactly 1.2 s, rendered twice so
 * the resonators are warm and the second pass loops seamlessly. */
static f32 *synth_engine(u32 rate, u32 *out_frames) {
    const f32 fire_hz = 2000.0f / 30.0f;
    u32 n = (u32)(1.2f * (f32)rate + 0.5f);
    f32 *s = (f32 *)a3_malloc(sizeof(f32) * n, A3_MEM_AUDIO);
    if (!s) return 0;
    static const f32 cyl[4] = { 1.0f, 0.82f, 0.94f, 0.76f };
    Reso pipe, body, roar;
    reso_init(&pipe, 105.0f, 38.0f, rate);
    reso_init(&body, 290.0f, 90.0f, rate);
    reso_init(&roar, 900.0f, 700.0f, rate);
    f32 peak = 0;
    for (u32 pass = 0; pass < 2; ++pass) {
        A3Rng rng;
        a3_rng_seed(&rng, 4242, 7);
        f64 phase = 0;
        u32 firing = 0;
        f32 burst = 0, lp = 0;
        for (u32 i = 0; i < n; ++i) {
            phase += (f64)fire_hz / (f64)rate;
            f32 kick = 0;
            if (phase >= 1.0) { phase -= 1.0; kick = cyl[firing & 3] * (0.92f + 0.16f * a3_rng_f32(&rng)); burst = kick; firing++; }
            burst *= 0.9965f;
            f32 noise = (a3_rng_f32(&rng) * 2.0f - 1.0f) * burst;
            f32 x = reso_step(&pipe, kick * 0.9f) * 0.055f + reso_step(&body, kick * 0.6f + noise * 0.05f) * 0.035f + reso_step(&roar, noise) * 0.02f;
            lp += 0.35f * (x - lp);
            f32 y = lp / (1.0f + a3_absf(lp));                  /* soft saturation */
            if (pass == 1) { s[i] = y; peak = a3_maxf(peak, a3_absf(y)); }
        }
    }
    if (peak > 0) for (u32 i = 0; i < n; ++i) s[i] *= 0.6f / peak;
    *out_frames = n;
    return s;
}

/* Tire squeal: two tones (integer Hz, so a 1 s loop is seamless) with a
 * slow wobble plus band-limited noise; the noise tail is cross-faded into
 * the head. */
/* Rain: a bed of soft filtered noise (distant rain) plus thousands of tiny
 * droplet ticks, each a short resonant click at a random pitch. The tail is
 * cross-faded into the head for a seamless 2 s loop. */
static f32 *synth_rain(u32 rate, u32 *out_frames) {
    u32 fade = rate / 4, n = rate * 2 + fade;
    f32 *s = (f32 *)a3_malloc(sizeof(f32) * n, A3_MEM_AUDIO);
    if (!s) return 0;
    A3Rng rng;
    a3_rng_seed(&rng, 2024, 11);
    Reso bed;
    reso_init(&bed, 2600.0f, 3500.0f, rate);
    f32 lp = 0, peak = 0;
    for (u32 i = 0; i < n; ++i) {
        f32 x = a3_rng_f32(&rng) * 2.0f - 1.0f;
        lp += 0.25f * (x - lp);
        s[i] = reso_step(&bed, lp) * 0.02f + lp * 0.25f;
    }
    u32 drops = n / 30;
    for (u32 d = 0; d < drops; ++d) {
        u32 at = a3_rng_range_u32(&rng, n);
        f32 freq = a3_rng_range_f32(&rng, 1800.0f, 7000.0f), amp = a3_rng_range_f32(&rng, 0.05f, 0.6f);
        amp *= amp;
        Reso tick;
        reso_init(&tick, freq, freq * 0.25f, rate);
        u32 len = rate / 120;
        for (u32 k = 0; k < len && at + k < n; ++k) s[at + k] += reso_step(&tick, k == 0 ? amp : 0.0f) * 0.08f;
    }
    for (u32 i = 0; i < fade; ++i) { f32 k = (f32)i / (f32)fade; s[i] = s[i] * k + s[n - fade + i] * (1.0f - k); }
    n -= fade;
    for (u32 i = 0; i < n; ++i) peak = a3_maxf(peak, a3_absf(s[i]));
    if (peak > 0) for (u32 i = 0; i < n; ++i) s[i] *= 0.55f / peak;
    *out_frames = n;
    return s;
}

static f32 *synth_skid(u32 rate, u32 *out_frames) {
    u32 fade = rate / 8, n = rate + fade;
    f32 *s = (f32 *)a3_malloc(sizeof(f32) * n, A3_MEM_AUDIO);
    if (!s) return 0;
    A3Rng rng;
    a3_rng_seed(&rng, 99, 3);
    Reso hiss;
    reso_init(&hiss, 1500.0f, 900.0f, rate);
    f32 peak = 0;
    for (u32 i = 0; i < n; ++i) {
        f32 t = (f32)i / (f32)rate;
        f32 wob = 2.2f * a3_sinf(A3_TAU * 3.0f * t) + 1.1f * a3_sinf(A3_TAU * 7.0f * t + 1.3f);
        f32 am = 0.75f + 0.25f * a3_sinf(A3_TAU * 5.0f * t);
        f32 tone = a3_sinf(A3_TAU * 780.0f * t + wob) * 0.6f + a3_sinf(A3_TAU * 1170.0f * t + wob * 1.5f) * 0.3f;
        f32 nz = reso_step(&hiss, a3_rng_f32(&rng) * 2.0f - 1.0f) * 0.05f;
        s[i] = tone * am + nz;
    }
    for (u32 i = 0; i < fade; ++i) { f32 k = (f32)i / (f32)fade; s[i] = s[i] * k + s[n - fade + i] * (1.0f - k); }
    n -= fade;
    for (u32 i = 0; i < n; ++i) peak = a3_maxf(peak, a3_absf(s[i]));
    if (peak > 0) for (u32 i = 0; i < n; ++i) s[i] *= 0.5f / peak;
    *out_frames = n;
    return s;
}

f32 *a3_audio_synth(u32 sound, u32 rate, u32 *out_frames) {
    *out_frames = 0;
    if (sound == 0 || sound >= A3_SOUND_COUNT || !rate) return 0;
    if (sound == A3_SOUND_ENGINE) return synth_engine(rate, out_frames);
    if (sound == A3_SOUND_SKID) return synth_skid(rate, out_frames);
    if (sound == A3_SOUND_RAIN) return synth_rain(rate, out_frames);
    const Synth *p = &g_synth[sound];
    f32 total = p->attack + p->sustain + p->decay;
    u32 n = (u32)(total * (f32)rate);
    if (!n) return 0;
    f32 *s = (f32 *)a3_malloc(sizeof(f32) * n, A3_MEM_AUDIO);
    if (!s) return 0;
    A3Rng rng;
    a3_rng_seed(&rng, p->seed, 77);
    f64 phase = 0;
    f32 hold = 0, lp = 0;
    b32 looped = sound == A3_SOUND_WIND || sound == A3_SOUND_ENGINE;
    for (u32 i = 0; i < n; ++i) {
        f32 t = (f32)i / (f32)rate;
        f32 prog = t / total;
        f32 freq = p->f0 * a3_powf(p->f1 / p->f0, prog);
        if (p->arp_time > 0 && t >= p->arp_time) freq *= p->arp_mul;
        if (p->vib_depth > 0) freq *= 1.0f + p->vib_depth * a3_sinf(A3_TAU * p->vib_speed * t);
        f64 prev = phase;
        phase += (f64)freq / (f64)rate;
        f32 fr = (f32)(phase - (f64)a3_floorf((f32)phase));
        f32 x;
        switch (p->wave) {
        case W_SQUARE: x = fr < p->duty ? 1.0f : -1.0f; break;
        case W_SAW: x = 2.0f * fr - 1.0f; break;
        case W_SINE: x = a3_sinf(A3_TAU * fr); break;
        case W_TRIANGLE: x = 1.0f - 4.0f * a3_absf(fr - 0.5f); break;
        default: /* noise: sample-and-hold at `freq` */
            if (i == 0 || a3_floorf((f32)phase) != a3_floorf((f32)prev)) hold = a3_rng_f32(&rng) * 2.0f - 1.0f;
            x = hold;
            break;
        }
        lp += p->lowpass * (x - lp);
        f32 env;
        if (t < p->attack) env = t / p->attack;
        else if (t < p->attack + p->sustain) env = 1.0f;
        else { f32 k = 1.0f - (t - p->attack - p->sustain) / (p->decay > 0 ? p->decay : 1e-3f); env = k > 0 ? k * k : 0; }
        if (looped) env = sound == A3_SOUND_WIND ? 0.6f + 0.4f * a3_sinf(A3_TAU * t / total) : 1.0f;
        s[i] = lp * env * p->volume;
    }
    if (sound == A3_SOUND_WIND) {
        /* seamless loop: cross-fade the tail into the head */
        u32 fade = rate / 5;
        for (u32 i = 0; i < fade && i < n / 2; ++i) {
            f32 k = (f32)i / (f32)fade;
            s[i] = s[i] * k + s[n - fade + i] * (1.0f - k);
        }
        n -= fade;
    }
    /* soft attack/release against clicks for one-shots */
    if (!looped) for (u32 i = 0; i < 32 && i < n; ++i) { s[i] *= (f32)i / 32.0f; s[n - 1 - i] *= (f32)i / 32.0f; }
    *out_frames = n;
    return s;
}

/* ======================================================================== */
/* Components & system                                                      */
/* ======================================================================== */

u32 A3_T_AUDIO_SOURCE = 0xFFFFFFFFu;
u32 A3_T_AUDIO_LISTENER = 0xFFFFFFFFu;

static u32 source_clip(const A3CAudioSource *s) {
    if (s->clip.path[0]) return a3_audio_clip(s->clip.path);
    if (s->builtin > 0 && s->builtin < A3_SOUND_COUNT) {
        char name[48];
        a3_snprintf(name, sizeof(name), "builtin:%s", a3_builtin_sound_names[s->builtin]);
        return a3_audio_clip(name);
    }
    return 0;
}

A3Voice a3_audio_preview_source(const A3CAudioSource *src) {
    u32 clip = source_clip(src);
    return clip ? a3_audio_play(clip, src->volume, src->pitch, 0) : 0;
}

/* Distance attenuation: full volume inside min, smooth inverse falloff, silent at max. */
static f32 attenuation(f32 d, f32 mn, f32 mx) {
    if (mx <= mn) mx = mn + 0.01f;
    if (d <= mn) return 1.0f;
    if (d >= mx) return 0.0f;
    f32 inv = mn / (mn + (d - mn));
    f32 edge = 1.0f - (d - mn) / (mx - mn);  /* fade to exactly 0 at max */
    return inv * a3_clampf(edge * 4.0f, 0, 1);
}

void a3_audio_update_world(A3World *w) {
    if (!w) return;
    /* listener: an active AudioListener, else the primary camera */
    A3Vec3 lpos = a3_v3_zero(), lright = a3_v3(1, 0, 0);
    f32 lvol = 1.0f;
    b32 have = 0;
    u32 nl = 0;
    const A3Entity *lents = 0;
    A3CAudioListener *ls = (A3CAudioListener *)a3_component_array(w, A3_T_AUDIO_LISTENER, &nl, &lents);
    for (u32 i = 0; i < nl && !have; ++i) {
        if (!ls[i].active || !a3_entity_active(w, lents[i])) continue;
        A3Mat4 m = a3_transform_compute_world(w, lents[i]);
        lpos = a3_mat4_get_translation(&m);
        lright = a3_v3_norm(a3_mat4_mul_dir(&m, a3_v3(1, 0, 0)));
        lvol = ls[i].volume;
        have = 1;
    }
    if (!have) {
        A3Entity cam = a3_find_primary_camera(w);
        if (a3_entity_valid(w, cam)) {
            A3Mat4 m = a3_transform_compute_world(w, cam);
            lpos = a3_mat4_get_translation(&m);
            lright = a3_v3_norm(a3_mat4_mul_dir(&m, a3_v3(1, 0, 0)));
        }
    }
    u32 n = 0;
    const A3Entity *ents = 0;
    A3CAudioSource *src = (A3CAudioSource *)a3_component_array(w, A3_T_AUDIO_SOURCE, &n, &ents);
    for (u32 i = 0; i < n; ++i) {
        A3CAudioSource *s = &src[i];
        b32 active = a3_entity_active(w, ents[i]);
        if (!active) { if (s->voice) { a3_audio_stop(s->voice); s->voice = 0; } continue; }
        if ((!s->started && s->play_on_start) || s->play) {
            if (s->voice) a3_audio_stop(s->voice);
            u32 clip = source_clip(s);
            s->voice = clip ? a3_audio_play(clip, 0, s->pitch, s->loop) : 0;
            s->play = 0;
        }
        s->started = 1;
        if (s->voice && !a3_audio_playing(s->voice)) s->voice = 0;
        if (!s->voice) continue;
        f32 l, r;
        if (s->spatial) {
            A3Vec3 p = a3_transform_world_position(w, ents[i]);
            A3Vec3 d = a3_v3_sub(p, lpos);
            f32 dist = a3_v3_len(d);
            f32 att = attenuation(dist, s->min_distance, s->max_distance);
            f32 pan = dist > 1e-3f ? a3_v3_dot(a3_v3_scale(d, 1.0f / dist), lright) * 0.85f : 0.0f;
            a3_audio_pan_gains(s->volume * att * lvol, pan, &l, &r);
        } else {
            a3_audio_pan_gains(s->volume * lvol, 0, &l, &r);
        }
        a3_audio_set_gains(s->voice, l, r);
        a3_audio_set_pitch(s->voice, s->pitch);
    }
}

void a3_audio_stop_world(A3World *w) {
    if (!w) return;
    u32 n = 0;
    const A3Entity *ents = 0;
    A3CAudioSource *src = (A3CAudioSource *)a3_component_array(w, A3_T_AUDIO_SOURCE, &n, &ents);
    for (u32 i = 0; i < n; ++i) { if (src[i].voice) a3_audio_stop(src[i].voice); src[i].voice = 0; src[i].started = 0; }
}

static const char *const g_builtin_labels[A3_SOUND_COUNT] = {
    "None (use Clip)", "Jump", "Coin", "Hit", "Explosion", "Laser", "Click", "Footstep", "Power Up", "Hurt", "Wind (loop)", "Engine (loop)", "Beep", "Tire Skid (loop)", "Rain (loop)",
};

void a3_audio_register(void) {
    if (A3_T_AUDIO_SOURCE != 0xFFFFFFFFu && a3_component_type(A3_T_AUDIO_SOURCE)) return;
    A3CAudioSource d;
    a3_zero_struct(&d);
    d.volume = 1.0f; d.pitch = 1.0f; d.play_on_start = 1; d.spatial = 1; d.min_distance = 2.0f; d.max_distance = 40.0f;
    d.builtin = A3_SOUND_BEEP;
    d.clip.kind = A3_ASSET_SOUND;
    u32 t = a3_component_register("AudioSource", "Audio", sizeof(A3CAudioSource), 16, &d, A3_COMP_BUILTIN,
        "Plays a sound. Pick a built-in sound or drag a .wav file into Clip. 3D sounds get quieter with distance and come from their direction.");
    A3_T_AUDIO_SOURCE = t;
    a3_component_type(t)->icon = "audio";
    a3_component_require(t, "Transform");
    A3FieldDesc *f = A3_REFLECT_FIELD(t, A3CAudioSource, clip, A3_FIELD_ASSET, "Clip", "A .wav sound file from your project (overrides Built-in Sound).");
    f->asset_kind = A3_ASSET_SOUND;
    f = A3_REFLECT_FIELD(t, A3CAudioSource, builtin, A3_FIELD_ENUM, "Built-in Sound", "Ready-made sounds, no files needed.");
    f->enum_names = g_builtin_labels; f->enum_count = A3_SOUND_COUNT;
    a3_field_range(A3_REFLECT_FIELD(t, A3CAudioSource, volume, A3_FIELD_F32, "Volume", "0 = silent, 1 = normal."), 0, 2, 0.01f)->flags |= A3_FIELD_FLAG_SLIDER;
    a3_field_range(A3_REFLECT_FIELD(t, A3CAudioSource, pitch, A3_FIELD_F32, "Pitch", "Playback speed: 0.5 = lower and slower, 2 = higher and faster."), 0.1f, 4, 0.01f)->flags |= A3_FIELD_FLAG_SLIDER;
    A3_REFLECT_FIELD(t, A3CAudioSource, loop, A3_FIELD_BOOL, "Loop", "Repeat forever (music, engines, wind).");
    A3_REFLECT_FIELD(t, A3CAudioSource, play_on_start, A3_FIELD_BOOL, "Play On Start", "Start playing when the game starts.");
    A3_REFLECT_FIELD(t, A3CAudioSource, spatial, A3_FIELD_BOOL, "3D Sound", "Quieter far away and heard from its direction. Turn off for music and menus.");
    a3_field_range(A3_REFLECT_FIELD(t, A3CAudioSource, min_distance, A3_FIELD_F32, "Full Volume Radius", "Meters around the object with full volume."), 0.1f, 1000, 0.1f)->flags |= A3_FIELD_FLAG_ADVANCED;
    a3_field_range(A3_REFLECT_FIELD(t, A3CAudioSource, max_distance, A3_FIELD_F32, "Hearing Distance", "Silent beyond this many meters."), 0.2f, 5000, 0.5f)->flags |= A3_FIELD_FLAG_ADVANCED;
    A3_REFLECT_FIELD(t, A3CAudioSource, play, A3_FIELD_BOOL, "Play Now", "Set from gameplay to play the sound again.")->flags |= A3_FIELD_FLAG_TRANSIENT | A3_FIELD_FLAG_ADVANCED;

    A3CAudioListener ld;
    ld.volume = 1.0f;
    ld.active = 1;
    t = a3_component_register("AudioListener", "Audio", sizeof(A3CAudioListener), 4, &ld, A3_COMP_BUILTIN,
        "The 'ears' of the game. Without one, sound is heard from the main camera.");
    A3_T_AUDIO_LISTENER = t;
    a3_component_type(t)->icon = "ear";
    a3_component_require(t, "Transform");
    a3_field_range(A3_REFLECT_FIELD(t, A3CAudioListener, volume, A3_FIELD_F32, "Volume", "Overall volume heard by this listener."), 0, 2, 0.01f)->flags |= A3_FIELD_FLAG_SLIDER;
    A3_REFLECT_FIELD(t, A3CAudioListener, active, A3_FIELD_BOOL, "Active", "The first active listener is used.");
}
