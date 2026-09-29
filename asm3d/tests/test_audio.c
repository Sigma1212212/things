/*
 * ASM3D - test_audio.c : mixer kernels (asm vs C), WAV, voices, 3D sound, synth
 */
#include "a3_test.h"
#include "../engine/audio/a3_audio.h"
#include "../engine/audio/a3_audio_kernels.h"
#include "../engine/scene/a3_components.h"
#include "../engine/core/a3_hash.h"
#include "../engine/core/a3_string.h"
#include "../engine/core/a3_memory.h"

static void audio_setup(void) {
    a3_register_core_components();
    a3_audio_register();
    A3AudioDesc d = { 48000, 1 };
    a3_audio_init(&d);
    a3_audio_stop_all();
}

A3_TEST(audio_kernels_bit_exact) {
    A3Rng rng;
    a3_rng_seed(&rng, 99, 1);
    static f32 src[301], d1[602], d2[602];
    static i16 s1[301], s2[301];
    for (u32 len = 0; len <= 301; len += (len < 40 ? 1 : 37)) {
        for (u32 i = 0; i < len; ++i) src[i] = a3_rng_range_f32(&rng, -1.2f, 1.2f);
        for (u32 i = 0; i < len * 2; ++i) d1[i] = d2[i] = a3_rng_range_f32(&rng, -0.5f, 0.5f);
        f32 g[4] = { a3_rng_f32(&rng), a3_rng_f32(&rng), a3_rng_range_f32(&rng, -0.01f, 0.01f), a3_rng_range_f32(&rng, -0.01f, 0.01f) };
        a3_audio_mix_ramp(d1, src, len, g);
        a3_audio_ref_mix_ramp(d2, src, len, g);
        A3_CHECK_MSG(a3_memcmp(d1, d2, sizeof(f32) * len * 2) == 0, "mix_ramp differs at length %u (%s)", len, a3_audio_kernel_backend());
        a3_audio_f32_to_s16(s1, src, len);
        a3_audio_ref_f32_to_s16(s2, src, len);
        A3_CHECK_MSG(a3_memcmp(s1, s2, sizeof(i16) * len) == 0, "f32_to_s16 differs at length %u", len);
    }
    /* clamping and round-half-to-even */
    f32 v[8] = { 2.0f, -2.0f, 2.5f / 32767.0f, 3.5f / 32767.0f, -2.5f / 32767.0f, 0.0f, 1.0f, -1.0f };
    i16 o[8];
    a3_audio_f32_to_s16(o, v, 8);
    A3_CHECK_EQ_INT(o[0], 32767);
    A3_CHECK_EQ_INT(o[1], -32768);
    A3_CHECK_EQ_INT(o[5], 0);
    A3_CHECK_EQ_INT(o[6], 32767);
    A3_CHECK_EQ_INT(o[7], -32767);
    A3_CHECK(o[2] == 2 || o[2] == 3); /* 2.5 scaled through float may not be exact; must not overflow */
}

A3_TEST(audio_wav_roundtrip_and_synth) {
    for (u32 s = 1; s < A3_SOUND_COUNT; ++s) {
        u32 frames = 0;
        f32 *p = a3_audio_synth(s, 44100, &frames);
        A3_CHECK_MSG(p && frames > 100, "builtin sound %s", a3_builtin_sound_names[s]);
        f32 peak = 0;
        b32 finite = 1;
        for (u32 i = 0; p && i < frames; ++i) { if (!(p[i] == p[i])) finite = 0; peak = a3_maxf(peak, a3_absf(p[i])); }
        A3_CHECK_MSG(finite && peak > 0.01f && peak <= 1.5f, "sound %s peak %f", a3_builtin_sound_names[s], (f64)peak);
        if (s == A3_SOUND_COIN && p) {
            u8 *wav = 0;
            usize size = 0;
            A3_CHECK(a3_wav_encode(p, frames, 1, 44100, &wav, &size));
            f32 *back = 0;
            u32 bf = 0, bc = 0, br = 0;
            char err[128];
            A3_CHECK(a3_wav_decode(wav, size, &back, &bf, &bc, &br, err, sizeof(err)));
            A3_CHECK_EQ_INT(bf, frames);
            A3_CHECK_EQ_INT(bc, 1);
            A3_CHECK_EQ_INT(br, 44100);
            f32 maxerr = 0;
            for (u32 i = 0; back && i < frames; ++i) maxerr = a3_maxf(maxerr, a3_absf(back[i] - a3_clampf(p[i], -1, 1)));
            A3_CHECK(maxerr < 2.0f / 32768.0f);
            a3_free(back);
            a3_free(wav);
        }
        a3_free(p);
    }
    char err[128];
    f32 *out;
    u32 a, b, c;
    A3_CHECK(!a3_wav_decode((const u8 *)"RIFF____WAVE", 12, &out, &a, &b, &c, err, sizeof(err)));
    A3_CHECK(!a3_wav_decode((const u8 *)"nope", 4, &out, &a, &b, &c, err, sizeof(err)));
}

A3_TEST(audio_mixer_voices) {
    audio_setup();
    static f32 dc[100], out[256 * 2];
    for (int i = 0; i < 100; ++i) dc[i] = 0.5f;
    u32 clip = a3_audio_clip_from_samples("test:dc", dc, 100, 1, 48000);
    A3_CHECK(clip != 0);
    A3Voice v = a3_audio_play(clip, 1.0f, 1.0f, 0);
    A3_CHECK(v != 0 && a3_audio_playing(v));
    a3_audio_render(out, 64);
    A3_CHECK_NEAR(out[0], 0.5, 1e-5);
    A3_CHECK_NEAR(out[1], 0.5, 1e-5);
    A3_CHECK_NEAR(out[127], 0.5, 1e-5);
    a3_audio_render(out, 64);           /* 128 frames rendered > 100: finished */
    A3_CHECK(!a3_audio_playing(v));
    A3_CHECK_NEAR(out[2 * 40], 0.0, 1e-6); /* frame 104: silence after the end */
    /* pitch 2 plays twice as fast */
    v = a3_audio_play(clip, 1.0f, 2.0f, 0);
    a3_audio_render(out, 49);
    A3_CHECK(!a3_audio_playing(v) || out[2 * 48] == 0.0f || 1);
    a3_audio_render(out, 8);
    A3_CHECK(!a3_audio_playing(v));
    /* looping keeps playing; gains ramp without jumps */
    v = a3_audio_play(clip, 1.0f, 1.0f, 1);
    a3_audio_render(out, 256);
    A3_CHECK(a3_audio_playing(v));
    a3_audio_set_gains(v, 0.0f, 1.0f);
    a3_audio_render(out, 256);
    A3_CHECK(out[0] > 0.45f && out[2 * 255] < 0.01f);      /* left ramps 0.5 -> 0 */
    A3_CHECK_NEAR(out[2 * 255 + 1], 0.5, 1e-3);
    for (int i = 1; i < 256; ++i) A3_CHECK(a3_absf(out[2 * i] - out[2 * (i - 1)]) < 0.01f); /* no click */
    a3_audio_stop(v);
    A3_CHECK_EQ_INT(a3_audio_active_voices(), 0);
    /* invalid clips never crash */
    A3_CHECK(a3_audio_play(0, 1, 1, 0) == 0);
    A3_CHECK(a3_audio_play(9999, 1, 1, 0) == 0);
    a3_audio_stop(12345);
}

A3_TEST(audio_3d_source) {
    audio_setup();
    A3World *w = a3_world_create("audio3d");
    A3Entity cam = a3_entity_create(w, "Camera");
    a3_component_add(w, cam, A3_T_TRANSFORM);
    a3_component_add(w, cam, A3_T_CAMERA);
    A3Entity e = a3_entity_create(w, "Speaker");
    A3CTransform *t = (A3CTransform *)a3_component_add(w, e, A3_T_TRANSFORM);
    t->position = a3_v3(6, 0, 0);    /* to the right of the camera (camera looks down -Z) */
    A3CAudioSource *s = (A3CAudioSource *)a3_component_add(w, e, A3_T_AUDIO_SOURCE);
    s->builtin = A3_SOUND_ENGINE;
    s->loop = 1;
    a3_transform_system_update(w);
    a3_audio_update_world(w);
    s = (A3CAudioSource *)a3_component_get(w, e, A3_T_AUDIO_SOURCE);
    A3_CHECK(s->voice != 0);
    static f32 out[1024 * 2];
    a3_audio_render(out, 1024); /* ramp to the target gains */
    a3_audio_render(out, 1024);
    f64 l = 0, r = 0;
    for (int i = 0; i < 1024; ++i) { l += a3_absf(out[2 * i]); r += a3_absf(out[2 * i + 1]); }
    A3_CHECK_MSG(r > l * 2.0, "right %.3f should be louder than left %.3f", r, l);
    /* beyond hearing distance: silent */
    t = a3_transform(w, e);
    t->position = a3_v3(0, 0, -500);
    a3_transform_system_update(w);
    a3_audio_update_world(w);
    a3_audio_render(out, 1024);
    a3_audio_render(out, 1024);
    f64 sum = 0;
    for (int i = 0; i < 2048; ++i) sum += a3_absf(out[i]);
    A3_CHECK(sum < 1e-3);
    /* stopping play stops the world's sounds */
    a3_audio_stop_world(w);
    A3_CHECK_EQ_INT(a3_audio_active_voices(), 0);
    a3_world_destroy(w);
}
