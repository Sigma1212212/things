/*
 * ASM3D - a3_modules.c
 * Registers the components and systems of every engine module compiled into
 * this build. Exporters generate a trimmed version of this list when a game
 * does not use a module (build-time stripping).
 */
#include "a3_engine.h"
#include "../physics/a3_physics.h"
#include "../physics/a3_character.h"
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
        a3_character_move(ctx->world, e, cc, move, jump, sprint, ctx->dt);
    }
}

void a3_modules_register_all(void) {
    a3_physics_register();
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
}
