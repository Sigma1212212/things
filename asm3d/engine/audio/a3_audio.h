/*
 * ASM3D - a3_audio.h
 * Audio: software mixer (SSE assembly kernels), WAV clips, a built-in sound
 * effect synthesizer, 3D sound, and the AudioSource / AudioListener
 * components. Output goes to WASAPI on Windows and ALSA on Linux; when no
 * audio device exists the engine keeps running silently.
 */
#ifndef A3_AUDIO_H
#define A3_AUDIO_H

#include "../core/a3_base.h"
#include "../core/a3_math.h"
#include "../ecs/a3_ecs.h"

A3_EXTERN_C_BEGIN

#define A3_AUDIO_MAX_VOICES 64
#define A3_AUDIO_MAX_CLIPS 256

typedef struct A3AudioDesc {
    u32 sample_rate;     /* 0 = device default (48000 when choosable) */
    b32 null_device;     /* tests / servers: mix only when a3_audio_render is called */
} A3AudioDesc;

b32  a3_audio_init(const A3AudioDesc *desc);  /* idempotent; false only on out-of-memory */
void a3_audio_shutdown(void);
b32  a3_audio_device_ok(void);                /* a real output device is running */
const char *a3_audio_device_name(void);
u32  a3_audio_sample_rate(void);
/* Mixes `frames` interleaved stereo float frames. Called by the device thread;
 * public for offline rendering and tests. */
void a3_audio_render(f32 *out, u32 frames);
void a3_audio_set_master_volume(f32 v);
f32  a3_audio_master_volume(void);

/* ---- clips ---- */
/* "Assets/Audio/x.wav" (project relative) or "builtin:coin". 0 on failure
 * (logged once with a plain-language hint). Clips are cached by path. */
u32  a3_audio_clip(const char *path);
u32  a3_audio_clip_from_samples(const char *name, const f32 *samples, u32 frames, u32 channels, u32 rate);
f32  a3_audio_clip_seconds(u32 clip);
u32  a3_audio_clip_channels(u32 clip);

/* Built-in synthesized sounds (no files needed). */
typedef enum A3BuiltinSound {
    A3_SOUND_NONE = 0, A3_SOUND_JUMP, A3_SOUND_COIN, A3_SOUND_HIT, A3_SOUND_EXPLOSION, A3_SOUND_LASER, A3_SOUND_CLICK,
    A3_SOUND_FOOTSTEP, A3_SOUND_POWERUP, A3_SOUND_HURT, A3_SOUND_WIND, A3_SOUND_ENGINE, A3_SOUND_BEEP, A3_SOUND_COUNT
} A3BuiltinSound;
extern const char *const a3_builtin_sound_names[A3_SOUND_COUNT];
/* Renders a built-in sound to mono float samples (a3_malloc'd). */
f32 *a3_audio_synth(u32 sound, u32 rate, u32 *out_frames);

/* ---- voices (playing sounds) ---- */
typedef u32 A3Voice;   /* 0 = invalid */
A3Voice a3_audio_play(u32 clip, f32 volume, f32 pitch, b32 loop);
void a3_audio_stop(A3Voice v);
b32  a3_audio_playing(A3Voice v);
/* left/right gains (after volume, attenuation and panning); ramped over one mix block */
void a3_audio_set_gains(A3Voice v, f32 left, f32 right);
void a3_audio_set_pitch(A3Voice v, f32 pitch);
void a3_audio_stop_all(void);
u32  a3_audio_active_voices(void);
/* Equal-power panning: pan -1 (left) .. 1 (right). */
void a3_audio_pan_gains(f32 volume, f32 pan, f32 *left, f32 *right);

/* ---- WAV ---- */
b32 a3_wav_decode(const u8 *data, usize size, f32 **out_samples, u32 *out_frames, u32 *out_channels, u32 *out_rate,
                  char *error, usize error_cap);
/* Writes 16-bit PCM WAV (used by tests and the "export sound" tool). */
b32 a3_wav_encode(const f32 *samples, u32 frames, u32 channels, u32 rate, u8 **out_data, usize *out_size);

/* ---- components ---- */
extern u32 A3_T_AUDIO_SOURCE;
extern u32 A3_T_AUDIO_LISTENER;

typedef struct A3CAudioSource {
    A3AssetRef clip;          /* .wav file (optional) */
    i32 builtin;              /* A3BuiltinSound used when clip is empty */
    f32 volume;
    f32 pitch;
    b32 loop;
    b32 play_on_start;
    b32 spatial;              /* 3D: quieter with distance, panned by direction */
    f32 min_distance;         /* full volume inside this radius */
    f32 max_distance;         /* silent beyond this radius */
    b32 play;                 /* set to true to (re)start the sound; cleared by the engine */
    b32 started;              /* runtime */
    u32 voice;                /* runtime */
} A3CAudioSource;

typedef struct A3CAudioListener {
    f32 volume;               /* master volume while this listener is active */
    b32 active;
} A3CAudioListener;

void a3_audio_register(void);          /* components (called by a3_modules) */
void a3_audio_update_world(A3World *w); /* starts sources, updates 3D gains (Audio system, late phase) */
void a3_audio_stop_world(A3World *w);   /* stops the world's sounds (when play stops) */
/* Plays the source's sound once in 2D (editor preview button). */
A3Voice a3_audio_preview_source(const A3CAudioSource *src);

A3_EXTERN_C_END

#endif
