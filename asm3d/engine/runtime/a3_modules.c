/*
 * ASM3D - a3_modules.c
 * Registers the components and systems of every engine module compiled into
 * this build. Exporters generate a trimmed version of this list when a game
 * does not use a module (build-time stripping).
 */
#include "../world/a3_procmeshes.h"
#include "a3_engine.h"
#include "../physics/a3_physics.h"
#include "../physics/a3_character.h"
#include "../audio/a3_audio.h"
#include "../particles/a3_particles.h"
#include "../anim/a3_anim.h"
#include "../script/a3_script_engine.h"
#include "../scene/a3_components.h"
#include "../platform/a3_window.h"
#include "../core/a3_string.h"

/* ---- physics ---- */

static void sys_physics(A3SystemContext *ctx, void *user) {
    A3_UNUSED(user);
    a3_physics_step(ctx->world, ctx->dt);
}

static void sys_physics_stop(A3SystemContext *ctx, void *user) {
    A3_UNUSED(user);
    a3_physics_release(ctx->world);
}

/* ---- player character: input latch (per frame), movement (fixed), look (per frame) ---- */

static void sys_character_input(A3SystemContext *ctx, void *user) {
    A3_UNUSED(user);
    u32 n = 0;
    const A3Entity *ents = 0;
    A3CCharacterController *ccs = (A3CCharacterController *)a3_component_array(ctx->world, A3_T_CHARACTER, &n, &ents);
    b32 any_player = 0;
    for (u32 i = 0; i < n; ++i) {
        A3CCharacterController *cc = &ccs[i];
        if (!cc->use_input || !a3_entity_active(ctx->world, ents[i])) continue;
        any_player = 1;
        if (a3_action_pressed(ctx->actions, ctx->input, "jump")) cc->jump_buffer = 0.15f; /* jump buffering */
        if (ctx->input->mouse_captured || cc->camera_mode == A3_CAM_NONE)
            a3_character_look(ctx->world, ents[i], cc, ctx->input->mouse_delta);
        else
            a3_character_look(ctx->world, ents[i], cc, a3_v2(0, 0));
    }
    /* mouse capture: click to look around, Escape to release */
    A3Window *win = a3_engine_window(ctx->engine);
    if (any_player && win && !ctx->editor_preview) {
        if (!ctx->input->mouse_captured && ctx->input->mouse_pressed[A3_MOUSE_LEFT]) a3_window_capture_mouse(win, 1);
        if (ctx->input->mouse_captured && ctx->input->keys_pressed[A3_KEY_ESCAPE]) a3_window_capture_mouse(win, 0);
    }
}

/* Built-in footsteps (every ~2 m, slightly varied pitch), jump and landing. */
static void character_sounds(A3CCharacterController *cc, b32 jumped, b32 was_grounded, f32 fall_speed, f32 dt, u64 salt) {
    static u32 steps, jumps;
    if (!steps) { steps = a3_audio_clip("builtin:footstep"); jumps = a3_audio_clip("builtin:jump"); }
    f32 pitch = 0.9f + 0.2f * (f32)((salt * 2654435761u) % 1000u) / 1000.0f;
    if (jumped) { a3_audio_play(jumps, 0.35f, pitch, 0); cc->step_distance = 0; return; }
    if (!was_grounded && cc->grounded && fall_speed > 3.0f) { a3_audio_play(steps, a3_minf(0.25f + fall_speed * 0.05f, 0.8f), 0.8f, 0); cc->step_distance = 0; return; }
    if (!cc->grounded) return;
    f32 speed = a3_sqrtf(cc->velocity.x * cc->velocity.x + cc->velocity.z * cc->velocity.z);
    if (speed < 0.5f) { cc->step_distance = 1.2f; return; } /* the first step after stopping comes quickly */
    cc->step_distance += speed * dt;
    f32 stride = speed > cc->walk_speed * 1.2f ? 2.4f : 1.9f;
    if (cc->step_distance >= stride) { a3_audio_play(steps, 0.3f, pitch, 0); cc->step_distance = 0; }
}

static void sys_character_move(A3SystemContext *ctx, void *user) {
    A3_UNUSED(user);
    u32 n = 0;
    const A3Entity *ents = 0;
    a3_component_array(ctx->world, A3_T_CHARACTER, &n, &ents);
    for (u32 i = 0; i < n; ++i) {
        A3Entity e = ents[i];
        A3CCharacterController *cc = (A3CCharacterController *)a3_component_get(ctx->world, e, A3_T_CHARACTER);
        if (!cc || !a3_entity_active(ctx->world, e)) continue;
        A3Vec2 move = a3_v2(0, 0);
        b32 sprint = 0, jump = 0;
        if (cc->use_input) {
            move = a3_v2(a3_action_value(ctx->actions, ctx->input, "move_x"), a3_action_value(ctx->actions, ctx->input, "move_y"));
            sprint = a3_action_down(ctx->actions, ctx->input, "sprint");
            jump = cc->jump_buffer > 0 && cc->grounded;
            if (jump) cc->jump_buffer = 0;
            cc->jump_buffer = a3_maxf(cc->jump_buffer - ctx->dt, 0.0f);
        }
        b32 was_grounded = cc->grounded;
        f32 fall_speed = -cc->velocity.y;
        a3_character_move(ctx->world, e, cc, move, jump, sprint, ctx->dt);
        if (cc->sounds) character_sounds(cc, jump, was_grounded, fall_speed, ctx->dt, ctx->frame + i);
    }
}

/* ---- particles (also simulated while editing, for live preview) ---- */

static void sys_particles(A3SystemContext *ctx, void *user) { A3_UNUSED(user); a3_particles_update(ctx->world, ctx->dt); }

/* ---- animation (play mode only: never changes the edited scene) ---- */

static void sys_anim(A3SystemContext *ctx, void *user) { A3_UNUSED(user); a3_anim_update(ctx->world, ctx->dt); }

/* ---- scripts (play mode only) ---- */

static void sys_scripts(A3SystemContext *ctx, void *user) {
    A3_UNUSED(user);
    A3ScriptFrame f;
    a3_zero_struct(&f);
    f.dt = ctx->dt;
    f.time = ctx->time;
    f.input = ctx->input;
    f.actions = ctx->actions;
    A3Window *win = a3_engine_window(ctx->engine);
    if (win) a3_window_size(win, &f.screen_w, &f.screen_h);
    a3_scripts_update(ctx->world, &f);
}
static void sys_script_events(A3SystemContext *ctx, void *user) { A3_UNUSED(user); a3_scripts_fixed(ctx->world, ctx->dt); }
static void sys_scripts_stop(A3SystemContext *ctx, void *user) { A3_UNUSED(user); a3_scripts_stop(ctx->world); }

/* ---- audio ---- */

static void sys_audio(A3SystemContext *ctx, void *user) { A3_UNUSED(user); a3_audio_update_world(ctx->world); }
static void sys_audio_stop(A3SystemContext *ctx, void *user) { A3_UNUSED(user); a3_audio_stop_world(ctx->world); }

void a3_modules_register_all(void) {
    a3_procmeshes_register();
    a3_physics_register();
    a3_audio_register();
    a3_particles_register();
    a3_anim_register();
    a3_scripts_register();
    A3SystemDesc d;
    a3_zero_struct(&d);
    d.name = "Character Input";
    d.phase = A3_PHASE_EARLY;
    d.order = 10;
    d.update = sys_character_input;
    a3_systems_register(&d);

    a3_zero_struct(&d);
    d.name = "Character Movement";
    d.phase = A3_PHASE_FIXED;
    d.order = 50;
    d.update = sys_character_move;
    a3_systems_register(&d);

    a3_zero_struct(&d);
    d.name = "Physics";
    d.phase = A3_PHASE_FIXED;
    d.order = 100;
    d.update = sys_physics;
    d.on_stop = sys_physics_stop;
    a3_systems_register(&d);

    a3_zero_struct(&d);
    d.name = "Script Events";
    d.phase = A3_PHASE_FIXED;
    d.order = 110;
    d.update = sys_script_events;
    a3_systems_register(&d);

    a3_zero_struct(&d);
    d.name = "Scripts";
    d.phase = A3_PHASE_UPDATE;
    d.order = 5;
    d.update = sys_scripts;
    d.on_stop = sys_scripts_stop;
    a3_systems_register(&d);

    a3_zero_struct(&d);
    d.name = "Animation";
    d.phase = A3_PHASE_UPDATE;
    d.order = 10;
    d.update = sys_anim;
    a3_systems_register(&d);

    a3_zero_struct(&d);
    d.name = "Particles";
    d.phase = A3_PHASE_UPDATE;
    d.order = 50;
    d.update = sys_particles;
    d.run_in_editor = 1;
    a3_systems_register(&d);

    a3_zero_struct(&d);
    d.name = "Audio";
    d.phase = A3_PHASE_LATE;
    d.order = 100;
    d.update = sys_audio;
    d.on_stop = sys_audio_stop;
    a3_systems_register(&d);
}
