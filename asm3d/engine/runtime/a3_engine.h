/*
 * ASM3D - a3_engine.h
 * Ties the subsystems together: window, renderer, input, assets and the
 * system scheduler (variable-rate update + fixed-timestep simulation).
 * Used by the game player, the editor's Play mode and exported games.
 */
#ifndef A3_ENGINE_H
#define A3_ENGINE_H

#include "../core/a3_base.h"
#include "../ecs/a3_ecs.h"
#include "../input/a3_input.h"
#include "../platform/a3_window.h"
#include "../render/a3_renderer.h"

A3_EXTERN_C_BEGIN

typedef struct A3Engine A3Engine;

/* ---- Systems: gameplay code and engine modules register these ---- */
typedef enum A3SystemPhase {
    A3_PHASE_EARLY = 0,    /* input processing, scripts' "on frame start" */
    A3_PHASE_FIXED,        /* fixed timestep: physics, character controllers, AI */
    A3_PHASE_UPDATE,       /* variable timestep: gameplay, animation, particles */
    A3_PHASE_LATE,         /* cameras follow targets, audio listener */
    A3_PHASE_COUNT
} A3SystemPhase;

typedef struct A3SystemContext {
    A3Engine *engine;
    A3World *world;
    f32 dt;               /* seconds for this step */
    f64 time;             /* seconds since play started */
    u64 frame;
    const A3InputState *input;
    const A3InputMap *actions;
    b32 editor_preview;   /* true when running inside the editor viewport */
} A3SystemContext;

typedef void (*A3SystemFn)(A3SystemContext *ctx, void *user);

typedef struct A3SystemDesc {
    const char *name;
    A3SystemPhase phase;
    i32 order;            /* lower runs first within a phase */
    A3SystemFn update;
    A3SystemFn on_start;  /* when play starts (world loaded) */
    A3SystemFn on_stop;
    void *user;
    b32 run_in_editor;    /* also run while editing (e.g. transform, particles preview) */
} A3SystemDesc;

void a3_systems_register(const A3SystemDesc *desc);
void a3_systems_clear(void);
u32  a3_systems_count(void);
const A3SystemDesc *a3_systems_get(u32 i);
/* Per-system CPU time of the last frame (profiler). */
f64  a3_systems_time_ms(u32 i);

/* ---- Engine ---- */
typedef struct A3EngineDesc {
    const char *title;
    i32 width, height;
    b32 vsync;
    b32 hidden;
    b32 resizable;
    const char *project_root;
    f32 fixed_hz;         /* default 60 */
} A3EngineDesc;

/* Registers every built-in component type and system (idempotent). */
void a3_engine_register_all(void);

A3Engine *a3_engine_create(const A3EngineDesc *desc);
/* No window, GPU or audio device: simulation only (command-line tools, servers, CI). */
A3Engine *a3_engine_create_headless(const char *project_root, f32 fixed_hz);
b32 a3_engine_is_headless(A3Engine *e);
void a3_engine_destroy(A3Engine *e);
A3Window *a3_engine_window(A3Engine *e);
A3Renderer *a3_engine_renderer(A3Engine *e);
A3InputMap *a3_engine_input_map(A3Engine *e);
const A3InputState *a3_engine_input(A3Engine *e);

/* Frame: begin (events, dt) -> simulate -> render -> end (present). */
b32  a3_engine_begin_frame(A3Engine *e, f32 *out_dt);
void a3_engine_start_play(A3Engine *e, A3World *w);
void a3_engine_stop_play(A3Engine *e, A3World *w);
/* Runs all systems for one frame (fixed steps as needed). paused: only
 * editor-safe systems run; step: advance exactly one fixed step. */
void a3_engine_simulate(A3Engine *e, A3World *w, f32 dt, b32 paused, b32 step);
void a3_engine_render_world(A3Engine *e, A3World *w, const A3RenderView *view_or_null); /* game view (NULL) also draws the script HUD */
void a3_engine_render_hud(A3Engine *e, A3World *w);
void a3_engine_end_frame(A3Engine *e);
u64  a3_engine_frame_index(A3Engine *e);
f64  a3_engine_play_time(A3Engine *e);
f32  a3_engine_fixed_dt(A3Engine *e);
/* Input seen by systems (NULL = window input). The editor passes an empty
 * state while the game viewport does not have focus. */
void a3_engine_set_input_override(A3Engine *e, const A3InputState *input);
/* Saves the window contents as PNG. */
b32  a3_engine_screenshot(A3Engine *e, const char *path);

A3_EXTERN_C_END

#endif
