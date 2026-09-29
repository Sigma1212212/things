/*
 * ASM3D - a3_script_engine.h
 * Runs A3Script files on objects: the Script component, the game API that
 * scripts can call (objects, input, physics, sound, particles, HUD...) and
 * the bridge that lets scripts read and write any component field by name.
 *
 *   fn on_start() { }                 // once, when play starts (or the object appears)
 *   fn on_update(dt) { }              // every frame
 *   fn on_fixed_update(dt) { }        // every physics step
 *   fn on_trigger_enter(other) { }    // another object entered this trigger
 *   fn on_trigger_exit(other) { }
 *   fn on_collision(other) { }        // this object hit another one
 *
 * One instance runs per object, so top-level `let` variables belong to that
 * object. A script error is logged once (file, line, plain explanation) and
 * stops that object's script until the file is fixed; the game keeps going.
 */
#ifndef A3_SCRIPT_ENGINE_H
#define A3_SCRIPT_ENGINE_H

#include "a3_script.h"
#include "../ecs/a3_ecs.h"
#include "../input/a3_input.h"

A3_EXTERN_C_BEGIN

extern u32 A3_T_SCRIPT;

typedef struct A3CScript {
    A3AssetRef script;        /* .a3script file */
    b32 active;
} A3CScript;

/* HUD drawn over the game view, on a virtual 1280 x 720 canvas scaled to the window. */
#define A3_HUD_WIDTH 1280.0f
#define A3_HUD_HEIGHT 720.0f
typedef enum A3HudKind { A3_HUD_TEXT = 0, A3_HUD_RECT } A3HudKind;
typedef struct A3HudCmd {
    u32 kind;
    f32 x, y, w, h;           /* text: w = size multiplier (1 = normal), h = alignment (0 left, 1 center, 2 right) */
    A3Vec4 color;
    char text[120];
} A3HudCmd;

typedef struct A3ScriptFrame {
    f32 dt;
    f64 time;                 /* seconds since play started */
    const A3InputState *input;
    const A3InputMap *actions;
    i32 screen_w, screen_h;   /* game view size in pixels (for mouse_position) */
} A3ScriptFrame;

typedef struct A3ScriptErrorInfo {
    char path[A3_PATH_MAX];
    char object[A3_NAME_MAX];
    A3SError error;
} A3ScriptErrorInfo;

void a3_scripts_register(void);   /* component, natives, host bindings (called by a3_modules) */

/* Play mode: creates instances, calls on_start / on_update. Per frame. */
void a3_scripts_update(A3World *w, const A3ScriptFrame *f);
/* Physics events (trigger / collision callbacks) and on_fixed_update. After each physics step. */
void a3_scripts_fixed(A3World *w, f32 dt);
/* Play stopped: destroys the world's instances and writes saved values. */
void a3_scripts_stop(A3World *w);
void a3_scripts_release(A3World *w);

/* Called after a script file changes on disk: running objects reload it, keeping
 * the values of top-level variables that still exist. */
void a3_scripts_invalidate(const char *path);
/* Uses `source` instead of the file at `path` (tests, tools, unsaved editor buffers).
 * NULL removes the override. Running objects reload. */
void a3_scripts_set_source(const char *path, const char *source);
/* Compiles a script file without running it (editor error markers). */
b32  a3_scripts_check(const char *name, const char *source, usize len, A3SError *err);

u32  a3_scripts_instance_count(A3World *w);
u32  a3_scripts_hud(A3World *w, const A3HudCmd **out);
/* Errors reported since the last clear (newest last; at most 64 kept). */
u32  a3_scripts_error_count(void);
const A3ScriptErrorInfo *a3_scripts_error(u32 i);
void a3_scripts_clear_errors(void);
/* load_scene("...") requests; the player applies them between frames. */
b32  a3_scripts_take_scene_request(A3World *w, char *out, usize cap);
/* Calls fn(args) on the script of an object (false if it has none). */
b32  a3_scripts_send(A3World *w, A3Entity e, const char *fn, const A3SValue *args, u32 argc);
/* Saved values (save_value / load_value) persist in <project>/saves.a3save. */
void a3_scripts_saves_flush(void);
void a3_scripts_saves_reset(void);   /* forget in-memory values (tests) */

A3_EXTERN_C_END

#endif
