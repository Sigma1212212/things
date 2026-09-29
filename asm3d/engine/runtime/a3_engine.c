/*
 * ASM3D - a3_engine.c
 */
#include "a3_engine.h"
#include "../core/a3_log.h"
#include "../core/a3_memory.h"
#include "../core/a3_string.h"
#include "../core/a3_image.h"
#include "../ecs/a3_reflect.h"
#include "../scene/a3_components.h"
#include "../resource/a3_assets.h"
#include "../platform/a3_platform.h"
#include "../jobs/a3_jobs.h"
#include "../audio/a3_audio.h"
#include "../script/a3_script_engine.h"
#include "../script/a3_script_hud.h"
#include "../ui/a3_ui.h"

#define MAX_SYSTEMS 128

static struct {
    A3SystemDesc systems[MAX_SYSTEMS];
    f64 time_ms[MAX_SYSTEMS];
    u32 count;
} g_sys;

struct A3Engine {
    A3Window *window;
    A3Renderer *renderer;
    A3InputMap input_map;
    u64 last_ns;
    u64 frame;
    f64 play_time;
    f64 accumulator;
    f32 fixed_dt;
    b32 playing;
    const A3InputState *input_override; /* editor: game input only while the viewport has focus */
    A3Ui *hud_ui;                        /* script HUD overlay (created on first use) */
};

static const A3InputState *game_input(A3Engine *e) { return e->input_override ? e->input_override : a3_window_input(e->window); }

void a3_engine_set_input_override(A3Engine *e, const A3InputState *input) { if (e) e->input_override = input; }

void a3_systems_register(const A3SystemDesc *d) {
    if (!d || !d->name) return;
    for (u32 i = 0; i < g_sys.count; ++i) {
        if (a3_streq(g_sys.systems[i].name, d->name)) { g_sys.systems[i] = *d; return; } /* replace (custom override) */
    }
    if (g_sys.count >= MAX_SYSTEMS) { A3_ERROR("engine", "too many systems"); return; }
    /* insertion by (phase, order) keeps execution order deterministic */
    u32 at = g_sys.count;
    while (at > 0) {
        const A3SystemDesc *p = &g_sys.systems[at - 1];
        if (p->phase < d->phase || (p->phase == d->phase && p->order <= d->order)) break;
        g_sys.systems[at] = *p;
        --at;
    }
    g_sys.systems[at] = *d;
    g_sys.count++;
}

void a3_systems_clear(void) { g_sys.count = 0; }
u32 a3_systems_count(void) { return g_sys.count; }
const A3SystemDesc *a3_systems_get(u32 i) { return i < g_sys.count ? &g_sys.systems[i] : 0; }
f64 a3_systems_time_ms(u32 i) { return i < g_sys.count ? g_sys.time_ms[i] : 0; }

/* Registration of every module lives in a3_modules.c so build-time stripping
 * can exclude modules a game does not use. */
void a3_modules_register_all(void);

void a3_engine_register_all(void) {
    static b32 done;
    if (done) return;
    done = 1;
    a3_register_core_components();
    a3_modules_register_all();
}

A3Engine *a3_engine_create(const A3EngineDesc *desc) {
    A3EngineDesc d;
    a3_zero_struct(&d);
    if (desc) d = *desc;
    a3_platform_init();
    a3_engine_register_all();
    A3Engine *e = A3_NEW(A3Engine, A3_MEM_CORE);
    if (!e) return 0;
    A3WindowDesc wd;
    a3_zero_struct(&wd);
    wd.title = d.title ? d.title : "ASM3D";
    wd.width = d.width ? d.width : 1280;
    wd.height = d.height ? d.height : 720;
    wd.vsync = d.vsync;
    wd.hidden = d.hidden;
    wd.resizable = 1;
    e->window = a3_window_create(&wd);
    if (!e->window) { a3_free(e); return 0; }
    if (!a3_rhi_init(a3_window_gl_proc)) { a3_window_destroy(e->window); a3_free(e); return 0; }
    a3_assets_init(d.project_root ? d.project_root : "");
    a3_assets_gpu_ready();
    e->renderer = a3_renderer_create();
    if (!e->renderer) { a3_engine_destroy(e); return 0; }
    a3_input_map_defaults(&e->input_map);
    a3_jobs_init(0);
    a3_audio_init(0); /* silent (not an error) when there is no audio device */
    e->fixed_dt = 1.0f / (d.fixed_hz > 0 ? d.fixed_hz : 60.0f);
    e->last_ns = a3_time_ns();
    return e;
}

void a3_engine_destroy(A3Engine *e) {
    if (!e) return;
    a3_audio_shutdown();
    a3_ui_destroy(e->hud_ui);
    a3_renderer_destroy(e->renderer);
    a3_assets_shutdown();
    a3_rhi_shutdown();
    a3_window_destroy(e->window);
    a3_jobs_shutdown();
    a3_free(e);
}

A3Window *a3_engine_window(A3Engine *e) { return e ? e->window : 0; }
A3Renderer *a3_engine_renderer(A3Engine *e) { return e ? e->renderer : 0; }
A3InputMap *a3_engine_input_map(A3Engine *e) { return e ? &e->input_map : 0; }
const A3InputState *a3_engine_input(A3Engine *e) { return e ? a3_window_input(e->window) : 0; }
u64 a3_engine_frame_index(A3Engine *e) { return e ? e->frame : 0; }
f64 a3_engine_play_time(A3Engine *e) { return e ? e->play_time : 0; }
f32 a3_engine_fixed_dt(A3Engine *e) { return e ? e->fixed_dt : 1.0f / 60.0f; }

b32 a3_engine_begin_frame(A3Engine *e, f32 *out_dt) {
    if (!e) return 0;
    b32 alive = a3_window_poll(e->window);
    u64 now = a3_time_ns();
    f32 dt = (f32)((f64)(now - e->last_ns) * 1e-9);
    e->last_ns = now;
    if (dt > 0.1f) dt = 0.1f; /* clamp after hitches / breakpoints */
    if (dt < 0.0f) dt = 0.0f;
    if (out_dt) *out_dt = dt;
    a3_frame_alloc_begin();
    a3_rhi_begin_frame();
    return alive;
}

static void run_phase(A3Engine *e, A3World *w, A3SystemPhase phase, f32 dt, b32 editor_only) {
    A3SystemContext ctx;
    ctx.engine = e;
    ctx.world = w;
    ctx.dt = dt;
    ctx.time = e->play_time;
    ctx.frame = e->frame;
    ctx.input = game_input(e);
    ctx.actions = &e->input_map;
    ctx.editor_preview = editor_only;
    for (u32 i = 0; i < g_sys.count; ++i) {
        A3SystemDesc *s = &g_sys.systems[i];
        if (s->phase != phase || !s->update) continue;
        if (editor_only && !s->run_in_editor) continue;
        u64 t0 = a3_time_ns();
        s->update(&ctx, s->user);
        g_sys.time_ms[i] += (f64)(a3_time_ns() - t0) / 1e6;
    }
}

void a3_engine_start_play(A3Engine *e, A3World *w) {
    if (!e) return;
    e->playing = 1;
    e->play_time = 0;
    e->accumulator = 0;
    A3SystemContext ctx = { e, w, 0, 0, e->frame, game_input(e), &e->input_map, 0 };
    for (u32 i = 0; i < g_sys.count; ++i) if (g_sys.systems[i].on_start) g_sys.systems[i].on_start(&ctx, g_sys.systems[i].user);
}

void a3_engine_stop_play(A3Engine *e, A3World *w) {
    if (!e) return;
    A3SystemContext ctx = { e, w, 0, e->play_time, e->frame, game_input(e), &e->input_map, 0 };
    for (u32 i = 0; i < g_sys.count; ++i) if (g_sys.systems[i].on_stop) g_sys.systems[i].on_stop(&ctx, g_sys.systems[i].user);
    e->playing = 0;
}

void a3_engine_simulate(A3Engine *e, A3World *w, f32 dt, b32 paused, b32 step) {
    if (!e || !w) return;
    for (u32 i = 0; i < g_sys.count; ++i) g_sys.time_ms[i] = 0;
    b32 editor_only = !e->playing;
    if (paused && !step) {
        /* paused: keep transforms current for inspection, advance nothing */
        a3_transform_system_update(w);
        return;
    }
    if (step) dt = e->fixed_dt;
    run_phase(e, w, A3_PHASE_EARLY, dt, editor_only);
    if (step) {
        run_phase(e, w, A3_PHASE_FIXED, e->fixed_dt, editor_only);
    } else {
        e->accumulator += dt;
        int steps = 0;
        while (e->accumulator >= e->fixed_dt && steps < 8) { /* spiral-of-death guard */
            run_phase(e, w, A3_PHASE_FIXED, e->fixed_dt, editor_only);
            e->accumulator -= e->fixed_dt;
            steps++;
        }
        if (steps == 8) e->accumulator = 0;
    }
    run_phase(e, w, A3_PHASE_UPDATE, dt, editor_only);
    run_phase(e, w, A3_PHASE_LATE, dt, editor_only);
    a3_transform_system_update(w);
    a3_world_flush(w);
    if (e->playing) e->play_time += dt;
}

void a3_engine_render_world(A3Engine *e, A3World *w, const A3RenderView *view) {
    if (!e || !w) return;
    i32 ww, wh;
    a3_window_size(e->window, &ww, &wh);
    A3RenderView v;
    if (view) v = *view;
    else {
        a3_transform_system_update(w);
        A3Entity cam = a3_find_primary_camera(w);
        if (!a3_render_view_from_camera(w, cam, ww, wh, &v)) {
            v = a3_render_view_look_at(a3_v3(6, 5, 8), a3_v3(0, 0.5f, 0), 60.0f, ww, wh);
        }
    }
    v.time = (f32)e->play_time;
    a3_renderer_draw_world(e->renderer, w, &v);
    if (!view) a3_engine_render_hud(e, w);
}

void a3_engine_render_hud(A3Engine *e, A3World *w) {
    const A3HudCmd *cmds;
    if (!e || !w || !a3_scripts_hud(w, &cmds)) return;
    if (!e->hud_ui && !(e->hud_ui = a3_ui_create())) return;
    i32 ww, wh;
    a3_window_size(e->window, &ww, &wh);
    static const A3InputState no_input;
    a3_ui_begin_frame(e->hud_ui, &no_input, ww, wh, 0);
    a3_scripts_draw_hud(e->hud_ui, w, a3_rect(0, 0, (f32)ww, (f32)wh));
    a3_ui_end_frame(e->hud_ui);
    a3_rhi_target_bind((A3RhiTarget){ 0 }, ww, wh);
    a3_ui_render(e->hud_ui);
}

void a3_engine_end_frame(A3Engine *e) {
    if (!e) return;
    a3_rhi_end_frame();
    a3_window_swap(e->window);
    e->frame++;
}

b32 a3_engine_screenshot(A3Engine *e, const char *path) {
    if (!e) return 0;
    i32 w, h;
    a3_window_size(e->window, &w, &h);
    u8 *px = (u8 *)a3_malloc((usize)w * h * 4, A3_MEM_TEMP);
    if (!px) return 0;
    a3_rhi_target_bind((A3RhiTarget){ 0 }, w, h);
    a3_rhi_read_pixels(0, 0, w, h, px);
    A3Result r = a3_png_write_file(path, px, w, h, 1);
    a3_free(px);
    if (r == A3_OK) A3_INFO("engine", "screenshot saved to %s", path);
    else A3_ERROR("engine", "failed to save screenshot %s", path);
    return r == A3_OK;
}
