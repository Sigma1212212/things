/*
 * ASM3D - a3_weather.c
 */
#include "a3_weather.h"
#include "../scene/a3_components.h"
#include "../audio/a3_audio.h"
#include "../core/a3_hash.h"

void a3_weather_set(A3World *w, f32 rain) {
    A3CWorldSettings *ws = a3_world_settings(w);
    if (!ws) return;
    rain = a3_clampf(rain, 0.0f, 1.0f);
    ws->rain = rain;
    ws->wetness = rain > 0.0f ? a3_minf(1.0f, 0.35f + rain * 0.9f) : 0.0f;
    if (rain > 0.0f) ws->cloud_cover = a3_maxf(ws->cloud_cover, 0.55f + 0.45f * rain);
}

f32 a3_weather_lightning(f64 time, f32 rain) {
    if (rain < 0.6f || time < 0) return 0.0f;
    const f64 period = 7.0;
    u64 k = (u64)(time / period);
    f64 local = time - (f64)k * period;
    u64 h = a3_hash_mix64(k * 0x9E3779B97F4A7C15ull + 17);
    if ((h & 3u) == 0) return 0.0f;                               /* some windows stay dark */
    f64 start = 0.5 + (f64)((h >> 8) % 1000u) / 1000.0 * (period - 1.5);
    f64 t = local - start;
    if (t < 0 || t > 0.7) return 0.0f;
    f32 strength = (0.6f + 0.4f * (f32)((h >> 20) % 100u) / 100.0f) * a3_smoothstep(0.6f, 1.0f, rain);
    /* two quick flashes: a bright stroke and a flicker 0.15 s later */
    f32 a = t < 0.08 ? 1.0f - (f32)t / 0.08f : 0.0f;
    f32 b = t > 0.15 && t < 0.7 ? 0.7f * a3_expf(-(f32)(t - 0.15) * 9.0f) : 0.0f;
    return (a + b) * strength * 3.0f;
}

f32 a3_weather_sun_factor(f32 rain) { return a3_maxf(0.08f, 1.0f - 1.15f * a3_clampf(rain, 0, 1)); }   /* overcast: little direct sun */
f32 a3_weather_grip(f32 wetness) { return 1.0f - 0.3f * a3_clampf(wetness, 0, 1); }

static A3Voice g_rain_voice;
static u32 g_rain_clip;

void a3_weather_update(A3World *w) {
    A3CWorldSettings *ws = a3_world_settings(w);
    f32 rain = ws ? a3_clampf(ws->rain, 0, 1) : 0.0f;
    if (rain <= 0.0f) { a3_weather_stop(); return; }
    if (!g_rain_voice || !a3_audio_playing(g_rain_voice)) {
        if (!g_rain_clip) g_rain_clip = a3_audio_clip("builtin:rain");
        g_rain_voice = g_rain_clip ? a3_audio_play(g_rain_clip, 0.0f, 1.0f, 1) : 0;
    }
    if (g_rain_voice) {
        f32 g = 0.15f + 0.45f * rain;
        a3_audio_set_gains(g_rain_voice, g, g);
    }
}

void a3_weather_stop(void) {
    if (g_rain_voice) a3_audio_stop(g_rain_voice);
    g_rain_voice = 0;
}
