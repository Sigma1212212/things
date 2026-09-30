/*
 * ASM3D - a3_weather.h
 * Weather, driven by two WorldSettings fields:
 *   Rain (0..1)     rain streaks on screen, raindrop ripples in puddles,
 *                   rain clouds that hide the sun (the sun's light is dimmed
 *                   and the fog thickens), lightning flashes in heavy rain,
 *                   and a rain sound loop while playing
 *   Wetness (0..1)  wet surfaces: rough materials darken, surfaces facing up
 *                   turn glossy and puddles spread (reflections of lights and
 *                   neon); tires grip up to 30% less on wet roads
 * Rendering is in the shaders (a3_renderer.c); this module holds the shared
 * rules so the renderer, vehicles and scripts agree.
 */
#ifndef A3_WEATHER_H
#define A3_WEATHER_H

#include "../ecs/a3_ecs.h"

A3_EXTERN_C_BEGIN

/* Sets rain (0..1); wetness follows (a little rain already wets the ground),
 * and the cloud cover rises. Used by the weather() script function and the CLI. */
void a3_weather_set(A3World *w, f32 rain);
/* Lightning brightness at `time` (seconds) for a rain amount: short double
 * flashes every few seconds in heavy rain, 0 otherwise. Deterministic. */
f32  a3_weather_lightning(f64 time, f32 rain);
/* Multiplier for the sun's light under rain clouds. */
f32  a3_weather_sun_factor(f32 rain);
/* Tire grip multiplier for a wetness. */
f32  a3_weather_grip(f32 wetness);
/* Keeps the rain sound in step with the world (play mode). */
void a3_weather_update(A3World *w);
void a3_weather_stop(void);

A3_EXTERN_C_END

#endif
