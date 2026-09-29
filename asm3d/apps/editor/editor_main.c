/*
 * ASM3D Editor - entry point, start screen, menus, toolbar, play mode,
 * shortcuts and layouts.
 *
 *   asm3d_editor [--project DIR] [--new NAME --location DIR --template N]
 *                [--layout NAME] [--beginner|--advanced] [--light]
 *                [--select ENTITY] [--show PANEL] [--open FILE] [--play-at N]
 *                [--frames N --screenshot out.png] [--hidden] [--size WxH]
 *                [--selftest] [--build]
 *
 * --frames/--screenshot/--select/--show exist so the editor can be exercised
 * and captured automatically (docs screenshots, CI smoke tests).
 */
#include "editor.h"
#include "../../engine/modeling/a3_emesh.h"
#include "../../engine/resource/a3_assets.h"
#include "../../engine/script/a3_script_engine.h"
#include "../../engine/physics/a3_physics.h"
#include "../../engine/audio/a3_audio.h"
#include "../../engine/particles/a3_particles.h"
#include "../../engine/anim/a3_anim.h"
#include "../../engine/resource/a3_assets.h"
#include "../../engine/scene/a3_scene_io.h"
#include "../../engine/core/a3_log.h"
#include "../../engine/core/a3_string.h"
#include "../../engine/core/a3_format.h"
#include "../../engine/platform/a3_platform.h"

#define MENUBAR_H 28.0f
#define TOOLBAR_H 40.0f
#define STATUS_H 24.0f

const char *const g_ed_layouts[] = { "Default", "Beginner", "Programming", "Level Design", "Shader Design", "Modeling" };
const u32 g_ed_layout_count = A3_ARRAY_COUNT(g_ed_layouts);

/* ======================================================================== */
/* Selection & entity helpers                                               */
/* ======================================================================== */

A3World *ed_active_world(A3Editor *ed) { return (ed->mode != ED_EDIT && ed->play_world) ? ed->play_world : ed->world; }

A3Entity ed_selected(A3Editor *ed) {
    A3World *w = ed_active_world(ed);
    if (!w || !ed->selected) return A3_ENTITY_NULL;
    return a3_entity_find_by_guid(w, ed->selected);
}

void ed_select(A3Editor *ed, A3Entity e) {
    A3World *w = ed_active_world(ed);
    u64 g = a3_entity_valid(w, e) ? a3_entity_guid(w, e) : 0;
    if (g == ed->selected) return;
    ed->selected = g;
    ed->tip[0] = 0;
}

void ed_mark_dirty(A3Editor *ed) { ed->dirty = 1; }

void ed_open_modal(A3Editor *ed, const char *id) { a3_strcpy(ed->pending_modal, sizeof(ed->pending_modal), id); }

static A3Vec3 spawn_point(A3Editor *ed, f32 lift) {
    A3Vec3 fwd = a3_quat_rotate(a3_quat_euler(ed->cam_pitch, ed->cam_yaw, 0), a3_v3(0, 0, -1));
    A3Vec3 p = a3_v3_madd(ed->cam_pos, fwd, 6.0f);
    if (fwd.y < -0.05f) {
        f32 t = -ed->cam_pos.y / fwd.y;
        if (t > 0 && t < 30) p = a3_v3_madd(ed->cam_pos, fwd, t);
    }
    p.y = a3_maxf(p.y, 0) + lift;
    return p;
}

A3Entity ed_create_entity(A3Editor *ed, const char *name, A3Primitive prim, const char *component) {
    if (ed->mode != ED_EDIT || !ed->world) {
        a3_ui_notify(ed->ui, a3_ui_theme(ed->ui)->colors[A3_UIC_WARNING], "Stop the game to edit the scene");
        return A3_ENTITY_NULL;
    }
    A3World *w = ed->world;
    A3Entity e = a3_entity_create(w, name ? name : "Entity");
    A3CTransform *t = (A3CTransform *)a3_component_add(w, e, A3_T_TRANSFORM);
    if (t) t->position = spawn_point(ed, prim != A3_PRIM_NONE && prim != A3_PRIM_PLANE ? 0.5f : 0.0f);
    if (prim != A3_PRIM_NONE) {
        A3CMeshRenderer *mr = (A3CMeshRenderer *)a3_component_add(w, e, A3_T_MESH_RENDERER);
        if (mr) mr->primitive = prim;
    }
    if (component && *component && !a3_component_add_by_name(w, e, component))
        A3_WARN("editor", "component '%s' is not available", component);
    ed->pending_create = a3_entity_guid(w, e);
    ed->selected = ed->pending_create;
    ed->dirty = 1;
    return e;
}

void ed_delete_selected(A3Editor *ed) {
    if (ed->mode != ED_EDIT) return;
    A3Entity e = ed_selected(ed);
    if (!a3_entity_valid(ed->world, e)) return;
    char label[96];
    a3_snprintf(label, sizeof(label), "Delete %s", a3_entity_name(ed->world, e));
    u64 guid = ed->selected;
    char *before = ed_snapshot(ed->world, guid);
    a3_entity_destroy(ed->world, e);
    ed->selected = 0;
    if (before) ed_undo_push(ed, ED_UNDO_DELETE, label, guid, before, 0);
    ed->dirty = 1;
}

void ed_duplicate_selected(A3Editor *ed) {
    if (ed->mode != ED_EDIT) return;
    A3Entity e = ed_selected(ed);
    if (!a3_entity_valid(ed->world, e)) return;
    A3Entity d = a3_entity_duplicate(ed->world, e);
    if (!a3_entity_valid(ed->world, d)) return;
    A3CTransform *t = a3_transform(ed->world, d);
    if (t) t->position = a3_v3_add(t->position, a3_v3(1, 0, 0));
    ed->pending_create = a3_entity_guid(ed->world, d);
    ed->selected = ed->pending_create;
    ed->dirty = 1;
}

void ed_show_tip_for_component(A3Editor *ed, u32 type_id) {
    A3ComponentType *t = a3_component_type(type_id);
    if (!t || !t->suggest_next) { ed->tip[0] = 0; return; }
    a3_snprintf(ed->tip, sizeof(ed->tip), "%s", t->suggest_reason ? t->suggest_reason : "");
    a3_snprintf(ed->tip_action, sizeof(ed->tip_action), "%s", t->suggest_next);
    ed->tip_component = type_id;
}

/* ======================================================================== */
/* Play mode                                                                */
/* ======================================================================== */

void ed_play(A3Editor *ed) {
    if (ed->mode != ED_EDIT) return;
    if (!ed->world) return;
    ed_anim_flush(ed); /* the game must start from the real scene, not an animation preview */
    ed_model_exit(ed, 1); /* and with the edited mesh saved */
    ed_undo_end_frame(ed);
    ed->play_world = a3_world_clone(ed->world, "Play");
    if (!ed->play_world) { A3_ERROR("editor", "could not start play mode (out of memory)"); return; }
    ed->mode = ED_PLAY;
    ed->game_view = 1;
    a3_engine_start_play(ed->engine, ed->play_world);
    A3_INFO("editor", "play mode started - edits made now are temporary");
}

void ed_stop(A3Editor *ed) {
    if (ed->mode == ED_EDIT) return;
    a3_window_capture_mouse(a3_engine_window(ed->engine), 0);
    if (ed->play_world) {
        a3_engine_stop_play(ed->engine, ed->play_world);
        a3_physics_release(ed->play_world);
        a3_world_destroy(ed->play_world);
    }
    ed->play_world = 0;
    ed->mode = ED_EDIT;
    ed->game_focused = 0;
    A3_INFO("editor", "play mode stopped - scene restored");
}

void ed_pause(A3Editor *ed) {
    if (ed->mode == ED_PLAY) { ed->mode = ED_PAUSED; a3_window_capture_mouse(a3_engine_window(ed->engine), 0); }
    else if (ed->mode == ED_PAUSED) ed->mode = ED_PLAY;
}

/* ======================================================================== */
/* Theme & layouts                                                          */
/* ======================================================================== */

void ed_apply_theme(A3Editor *ed) {
    A3UiTheme *t = a3_ui_theme(ed->ui);
    if (ed->dark_theme) a3_ui_theme_dark(t); else a3_ui_theme_light(t);
}

void ed_layout_preset(A3Editor *ed, const char *name) {
    A3Dock *d = &ed->dock;
    a3_dock_reset(d);
    for (u32 i = 0; i < d->panel_count; ++i) d->panels[i].open = 0;
    i32 root = a3_dock_root(d);
#define P(n) a3_dock_find_panel(d, n)
    if (a3_streq(name, "Beginner")) {
        /* fewer panels, bigger viewport */
        i32 right = a3_dock_split(d, root, 1, 0.74f, 1);
        i32 left = root == right ? -1 : d->nodes[root].child[0];
        i32 bottom = a3_dock_split(d, left, 0, 0.78f, 1);
        i32 top = d->nodes[left].child[0];
        i32 hier = a3_dock_split(d, top, 1, 0.22f, 0);
        i32 view = d->nodes[top].child[1];
        a3_dock_add_tab(d, hier, P("Hierarchy"));
        a3_dock_add_tab(d, view, P("Viewport"));
        a3_dock_add_tab(d, right, P("Inspector"));
        a3_dock_add_tab(d, bottom, P("Assets"));
        a3_dock_add_tab(d, bottom, P("Console"));
        a3_dock_add_tab(d, bottom, P("Docs"));
        d->nodes[bottom].active = 0;
    } else if (a3_streq(name, "Programming")) {
        i32 right = a3_dock_split(d, root, 1, 0.62f, 1);
        i32 left = d->nodes[root].child[0];
        i32 bottom = a3_dock_split(d, left, 0, 0.72f, 1);
        i32 code = d->nodes[left].child[0];
        i32 rbottom = a3_dock_split(d, right, 0, 0.5f, 1);
        i32 rtop = d->nodes[right].child[0];
        a3_dock_add_tab(d, code, P("Code"));
        a3_dock_add_tab(d, bottom, P("Console"));
        a3_dock_add_tab(d, bottom, P("Assets"));
        d->nodes[bottom].active = 0;
        a3_dock_add_tab(d, rtop, P("Viewport"));
        a3_dock_add_tab(d, rbottom, P("Inspector"));
        a3_dock_add_tab(d, rbottom, P("Hierarchy"));
        a3_dock_add_tab(d, rbottom, P("Docs"));
        d->nodes[rbottom].active = 0;
    } else if (a3_streq(name, "Modeling")) {
        /* big viewport, tools on the right */
        i32 right = a3_dock_split(d, root, 1, 0.74f, 1);
        i32 left = d->nodes[root].child[0];
        i32 bottom = a3_dock_split(d, left, 0, 0.82f, 1);
        i32 main = d->nodes[left].child[0];
        a3_dock_add_tab(d, main, P("Viewport"));
        a3_dock_add_tab(d, bottom, P("Assets"));
        a3_dock_add_tab(d, bottom, P("Console"));
        d->nodes[bottom].active = 0;
        i32 rbottom = a3_dock_split(d, right, 0, 0.64f, 1);
        i32 rtop = d->nodes[right].child[0];
        a3_dock_add_tab(d, rtop, P("Modeling"));
        a3_dock_add_tab(d, rbottom, P("Inspector"));
        a3_dock_add_tab(d, rbottom, P("Hierarchy"));
        d->nodes[rbottom].active = 0;
    } else if (a3_streq(name, "Shader Design")) {
        i32 right = a3_dock_split(d, root, 1, 0.76f, 1);
        i32 left = d->nodes[root].child[0];
        i32 bottom = a3_dock_split(d, left, 0, 0.8f, 1);
        i32 main = d->nodes[left].child[0];
        a3_dock_add_tab(d, main, P("Shader Maker"));
        a3_dock_add_tab(d, main, P("Viewport"));
        d->nodes[main].active = 0;
        a3_dock_add_tab(d, bottom, P("Console"));
        a3_dock_add_tab(d, bottom, P("Assets"));
        d->nodes[bottom].active = 0;
        a3_dock_add_tab(d, right, P("Inspector"));
        a3_dock_add_tab(d, right, P("Hierarchy"));
        d->nodes[right].active = 0;
    } else { /* Default / Level Design */
        b32 level = a3_streq(name, "Level Design");
        i32 right = a3_dock_split(d, root, 1, level ? 0.8f : 0.78f, 1);
        i32 left = d->nodes[root].child[0];
        i32 bottom = a3_dock_split(d, left, 0, level ? 0.8f : 0.72f, 1);
        i32 top = d->nodes[left].child[0];
        i32 hier = a3_dock_split(d, top, 1, level ? 0.18f : 0.2f, 0);
        i32 view = d->nodes[top].child[1];
        a3_dock_add_tab(d, hier, P("Hierarchy"));
        a3_dock_add_tab(d, view, P("Viewport"));
        if (!level) { a3_dock_add_tab(d, view, P("Code")); a3_dock_add_tab(d, view, P("Shader Maker")); d->nodes[view].active = 0; }
        a3_dock_add_tab(d, right, P("Inspector"));
        a3_dock_add_tab(d, right, P("Modeling"));
        a3_dock_add_tab(d, right, P("Settings"));
        a3_dock_add_tab(d, right, P("Build"));
        d->nodes[right].active = 0;
        a3_dock_add_tab(d, bottom, P("Assets"));
        a3_dock_add_tab(d, bottom, P("Console"));
        a3_dock_add_tab(d, bottom, P("Animation"));
        a3_dock_add_tab(d, bottom, P("Profiler"));
        a3_dock_add_tab(d, bottom, P("Docs"));
        d->nodes[bottom].active = 0;
    }
#undef P
}

/* ======================================================================== */
/* Start screen                                                             */
/* ======================================================================== */

static u32 template_icon(i32 i) {
    static const u32 icons[] = { A3_ICON_SQUARE_O, A3_ICON_TARGET, A3_ICON_HOME, A3_ICON_UP, A3_ICON_BOLT,
                                 A3_ICON_WARNING, A3_ICON_DIAMOND, A3_ICON_BOX, A3_ICON_CLOUD, A3_ICON_HEX };
    return i >= 0 && i < (i32)A3_ARRAY_COUNT(icons) ? icons[i] : A3_ICON_CUBE;
}

static void start_screen(A3Editor *ed, A3Ui *ui, f32 W, f32 H) {
    A3UiTheme *th = a3_ui_theme(ui);
    a3_ui_rect_gradient(ui, a3_rect(0, 0, W, H), th->colors[A3_UIC_BG], a3_color_lerp(th->colors[A3_UIC_BG], th->colors[A3_UIC_ACCENT], 0.08f));
    f32 side_w = 320;
    /* sidebar */
    if (a3_ui_begin_panel(ui, "start_side", a3_rect(0, 0, side_w, H), 0)) {
        a3_ui_spacing(ui, 18);
        a3_ui_label_font(ui, A3_FONT_TITLE, th->colors[A3_UIC_TEXT], "ASM3D");
        a3_ui_label_colored(ui, th->colors[A3_UIC_TEXT_DIM], "Game Engine  %s", A3_VERSION_STRING);
        a3_ui_spacing(ui, 20);
        a3_ui_heading(ui, "Recent Projects");
        if (ed->recent_count == 0) a3_ui_label_colored(ui, th->colors[A3_UIC_TEXT_DIM], "No recent projects yet.");
        for (u32 i = 0; i < ed->recent_count; ++i) {
            a3_ui_push_id_int(ui, (i64)i);
            if (a3_ui_selectable_ex(ui, "recent", a3_path_filename(ed->recent[i]), 0, A3_ICON_HOME, th->colors[A3_UIC_WARNING])) {
                a3_ui_pop_id(ui);
                ed_project_open(ed, ed->recent[i]);
                a3_ui_end_panel(ui);
                return;
            }
            a3_ui_tooltip(ui, ed->recent[i]);
            a3_ui_pop_id(ui);
        }
        a3_ui_spacing(ui, 20);
        a3_ui_heading(ui, "Open a Project Folder");
        a3_ui_input_text(ui, "open_path", ed->open_path, sizeof(ed->open_path), 0, "/path/to/MyGame");
        if (a3_ui_button_ex(ui, "Open", 0, 0) && ed->open_path[0]) ed_project_open(ed, ed->open_path);
        a3_ui_spacing(ui, 20);
        a3_ui_heading(ui, "Experience");
        i32 lvl = ed->level == ED_LEVEL_BEGINNER ? 0 : 1;
        static const char *const levels[] = { "Beginner Mode", "Advanced Mode" };
        if (a3_ui_combo(ui, "level", &lvl, levels, 2)) ed->level = lvl == 0 ? ED_LEVEL_BEGINNER : ED_LEVEL_ADVANCED;
        a3_ui_label_wrapped(ui, ed->level == ED_LEVEL_BEGINNER
            ? "Beginner Mode shows the essentials with tips and plain-language explanations. Switch any time from the toolbar."
            : "Advanced Mode shows every setting, the profiler and engine internals.");
    }
    a3_ui_end_panel(ui);
    a3_ui_line(ui, a3_v2(side_w, 0), a3_v2(side_w, H), th->colors[A3_UIC_BORDER], 1);
    /* main: new project */
    if (a3_ui_begin_panel(ui, "start_main", a3_rect(side_w + 1, 0, W - side_w - 1, H), 0)) {
        a3_ui_spacing(ui, 22);
        a3_ui_label_font(ui, A3_FONT_HEADING, th->colors[A3_UIC_TEXT], "Create a New Project");
        a3_ui_label_colored(ui, th->colors[A3_UIC_TEXT_DIM], "Pick a template - every template is playable right away.");
        a3_ui_spacing(ui, 10);
        f32 avail = a3_ui_content_width(ui);
        i32 cols = avail > 900 ? 5 : (avail > 640 ? 4 : (avail > 420 ? 3 : 2));
        f32 gap = 10, cw = (avail - gap * (f32)(cols - 1)) / (f32)cols, ch = 0;
        for (u32 i = 0; i < g_ed_template_count; ++i) ch = a3_maxf(ch, a3_ui_text_wrapped_height(A3_FONT_UI, cw - 20, g_ed_template_desc[i]));
        ch += 62;
        A3Vec2 start = a3_ui_cursor_pos(ui);
        for (u32 i = 0; i < g_ed_template_count; ++i) {
            i32 c = (i32)i % cols, r = (i32)i / cols;
            A3Rect card = a3_rect(start.x + (f32)c * (cw + gap), start.y + (f32)r * (ch + gap), cw, ch);
            b32 sel = ed->new_template == (i32)i;
            a3_ui_push_id_int(ui, (i64)i);
            b32 clicked = a3_ui_invisible_button(ui, "card", card);
            b32 hov = a3_ui_item_hovered(ui);
            a3_ui_pop_id(ui);
            if (clicked) ed->new_template = (i32)i;
            a3_ui_shadow(ui, card, 8, 10, a3_color_alpha(th->colors[A3_UIC_SHADOW], 0.5f));
            a3_ui_rect(ui, card, sel ? a3_color_lerp(th->colors[A3_UIC_PANEL_ALT], th->colors[A3_UIC_ACCENT], 0.18f)
                                     : (hov ? th->colors[A3_UIC_WIDGET_HOVER] : th->colors[A3_UIC_PANEL_ALT]), 10);
            if (sel) a3_ui_rect_outline(ui, card, th->colors[A3_UIC_ACCENT], 10, 2);
            A3Rect band = a3_rect(card.x, card.y, card.w, 44);
            a3_ui_rect_corners(ui, band, a3_color_alpha(th->colors[A3_UIC_ACCENT], sel ? 0.55f : 0.22f), 10, 1 | 2);
            a3_ui_icon(ui, template_icon((i32)i), a3_v2(card.x + 24, card.y + 22), th->colors[A3_UIC_TEXT], A3_FONT_UI_LARGE);
            a3_ui_text_in_rect(ui, A3_FONT_UI_BOLD, a3_rect(card.x + 44, card.y, card.w - 50, 44), A3_ALIGN_LEFT, th->colors[A3_UIC_TEXT], g_ed_templates[i]);
            a3_ui_push_clip(ui, card);
            a3_ui_text_wrapped(ui, A3_FONT_UI, a3_rect(card.x + 10, card.y + 50, card.w - 20, ch - 54), th->colors[A3_UIC_TEXT_DIM], g_ed_template_desc[i]);
            a3_ui_pop_clip(ui);
        }
        i32 rows = ((i32)g_ed_template_count + cols - 1) / cols;
        f32 form_y = start.y + (f32)rows * (ch + gap) + 12;
        a3_ui_set_cursor_pos(ui, a3_v2(start.x, form_y));
        a3_ui_next_rect(ui, 1, 150);
        b32 create = 0;
        if (a3_ui_begin_panel(ui, "new_form", a3_rect(start.x, form_y, a3_minf(avail, 720), 150), A3_PANEL_NO_SCROLL | A3_PANEL_NO_BACKGROUND | A3_PANEL_NO_PADDING)) {
            a3_ui_label_font(ui, A3_FONT_UI_BOLD, th->colors[A3_UIC_TEXT], g_ed_templates[ed->new_template]);
            a3_ui_property(ui, "Project Name", "Used for the folder and the window title.");
            a3_ui_input_text(ui, "new_name", ed->new_name, sizeof(ed->new_name), 0, "My Game");
            a3_ui_property(ui, "Location", "The project folder is created inside this folder.");
            a3_ui_input_text(ui, "new_loc", ed->new_location, sizeof(ed->new_location), 0, "/home/me/Games");
            a3_ui_spacing(ui, 8);
            create = a3_ui_button_ex(ui, "Create Project", 200, A3_BUTTON_PRIMARY);
            a3_ui_same_line(ui);
            a3_ui_label_colored(ui, th->colors[A3_UIC_TEXT_DIM], "Saved as readable text files - great with Git.");
        }
        a3_ui_end_panel(ui);
        if (create && ed_project_create(ed, ed->new_location, ed->new_name, ed->new_template)) {
            ed_layout_preset(ed, ed->level == ED_LEVEL_BEGINNER ? "Beginner" : "Default");
            ed_open_modal(ed, "welcome");
            ed->show_welcome = 0;
        }
    }
    a3_ui_end_panel(ui);
}

/* ======================================================================== */
/* Menubar, toolbar, status bar                                             */
/* ======================================================================== */

static void create_menu_items(A3Editor *ed, A3Ui *ui) {
    if (a3_ui_menu_item(ui, "Empty Object", 0, 1)) ed_create_entity(ed, "Empty", A3_PRIM_NONE, 0);
    if (a3_ui_begin_menu(ui, "3D Shape")) {
        static const char *names[] = { "", "Cube", "Sphere", "Plane", "Cylinder", "Capsule", "Cone" };
        for (i32 p = A3_PRIM_CUBE; p < A3_PRIM_COUNT; ++p)
            if (a3_ui_menu_item(ui, names[p], 0, 1)) ed_create_entity(ed, names[p], (A3Primitive)p, 0);
        a3_ui_end_menu(ui);
    }
    if (a3_ui_begin_menu(ui, "Light")) {
        static const char *names[] = { "Directional Light", "Point Light", "Spot Light" };
        for (i32 i = 0; i < 3; ++i)
            if (a3_ui_menu_item(ui, names[i], 0, 1)) {
                A3Entity e = ed_create_entity(ed, names[i], A3_PRIM_NONE, "Light");
                A3CLight *l = ed->world ? (A3CLight *)a3_component_get(ed->world, e, A3_T_LIGHT) : 0;
                if (l) { l->type = i; if (i != A3_LIGHT_DIRECTIONAL) { A3CTransform *t = a3_transform(ed->world, e); if (t) t->position.y += 2.5f; } }
            }
        a3_ui_end_menu(ui);
    }
    if (a3_ui_menu_item(ui, "Camera", 0, 1)) ed_create_entity(ed, "Camera", A3_PRIM_NONE, "Camera");
    if (a3_ui_menu_item(ui, "Sound", 0, 1)) ed_create_entity(ed, "Sound", A3_PRIM_NONE, "AudioSource");
    if (a3_ui_begin_menu(ui, "Particles")) {
        for (u32 p = 0; p < A3_PARTICLES_PRESET_COUNT; ++p) {
            if (!a3_ui_menu_item(ui, a3_particle_preset_names[p], 0, 1)) continue;
            A3Entity e = ed_create_entity(ed, a3_particle_preset_names[p], A3_PRIM_NONE, "ParticleEmitter");
            A3CParticleEmitter *em = ed->world ? (A3CParticleEmitter *)a3_component_get(ed->world, e, A3_T_PARTICLE_EMITTER) : 0;
            if (em) {
                a3_particles_preset(em, p);
                A3CTransform *t = a3_transform(ed->world, e);
                if (t && (p == A3_PARTICLES_RAIN || p == A3_PARTICLES_SNOW)) t->position.y += 12.0f; /* weather falls from above */
            }
        }
        a3_ui_end_menu(ui);
    }
    a3_ui_menu_separator(ui);
    if (a3_ui_menu_item(ui, "Player (First Person)", 0, 1)) ed_create_entity(ed, "Player", A3_PRIM_NONE, "CharacterController");
    if (a3_ui_menu_item(ui, "Physics Crate", 0, 1)) {
        A3Entity e = ed_create_entity(ed, "Crate", A3_PRIM_CUBE, "RigidBody");
        if (ed->world && a3_entity_valid(ed->world, e)) a3_component_add(ed->world, e, A3_T_COLLIDER);
    }
    if (a3_ui_menu_item(ui, "Static Wall", 0, 1)) {
        A3Entity e = ed_create_entity(ed, "Wall", A3_PRIM_CUBE, "Collider");
        A3CTransform *t = ed->world ? a3_transform(ed->world, e) : 0;
        if (t) { t->scale = a3_v3(6, 3, 0.4f); t->position.y = 1.5f; }
    }
}

static void menubar(A3Editor *ed, A3Ui *ui, f32 W) {
    if (!a3_ui_begin_menubar(ui, a3_rect(0, 0, W, MENUBAR_H))) return;
    b32 editing = ed->mode == ED_EDIT;
    if (a3_ui_menubar_menu(ui, "File")) {
        if (a3_ui_menu_item(ui, "New Scene", 0, editing)) ed_scene_new(ed);
        if (a3_ui_menu_item(ui, "Save Scene", "Ctrl+S", editing)) ed_scene_save(ed);
        a3_ui_menu_separator(ui);
        if (a3_ui_menu_item(ui, "New / Open Project...", 0, 1)) ed->want_start_screen = 1;
        if (a3_ui_menu_item(ui, "Close Project", 0, 1)) ed->want_start_screen = 1;
        a3_ui_menu_separator(ui);
        if (a3_ui_menu_item(ui, "Exit", "Ctrl+Q", 1)) ed->quit = 1;
        a3_ui_end_popup(ui);
    }
    if (a3_ui_menubar_menu(ui, "Edit")) {
        char u[96], r[96];
        const EdUndo *un = &ed->undo;
        a3_snprintf(u, sizeof(u), "Undo %s", un->cursor > 0 ? un->entries.data[un->cursor - 1].label : "");
        a3_snprintf(r, sizeof(r), "Redo %s", un->cursor < un->entries.count ? un->entries.data[un->cursor].label : "");
        if (a3_ui_menu_item(ui, u, "Ctrl+Z", editing && un->cursor > 0)) ed_undo(ed);
        if (a3_ui_menu_item(ui, r, "Ctrl+Y", editing && un->cursor < un->entries.count)) ed_redo(ed);
        a3_ui_menu_separator(ui);
        b32 has_sel = a3_entity_valid(ed_active_world(ed), ed_selected(ed));
        if (a3_ui_menu_item(ui, "Duplicate", "Ctrl+D", editing && has_sel)) ed_duplicate_selected(ed);
        if (a3_ui_menu_item(ui, "Delete", "Del", editing && has_sel)) ed_delete_selected(ed);
        if (a3_ui_menu_item(ui, "Focus Selection", "F", has_sel)) ed_focus_selected(ed);
        a3_ui_menu_separator(ui);
        if (a3_ui_menu_item(ui, "Command Palette", "Ctrl+P", 1)) ed_palette_open(ed);
        if (a3_ui_menu_item(ui, "Preferences", 0, 1)) a3_dock_show(&ed->dock, "Settings");
        a3_ui_end_popup(ui);
    }
    if (a3_ui_menubar_menu(ui, "Create")) { create_menu_items(ed, ui); a3_ui_end_popup(ui); }
    if (a3_ui_menubar_menu(ui, "Play")) {
        if (a3_ui_menu_item(ui, ed->mode == ED_EDIT ? "Play" : "Stop", "F5", 1)) { if (ed->mode == ED_EDIT) ed_play(ed); else ed_stop(ed); }
        if (a3_ui_menu_item(ui, ed->mode == ED_PAUSED ? "Resume" : "Pause", "F6", !editing)) ed_pause(ed);
        if (a3_ui_menu_item(ui, "Step One Frame", "F10", ed->mode == ED_PAUSED)) ed->step_requested = 1;
        a3_ui_end_popup(ui);
    }
    if (a3_ui_menubar_menu(ui, "Window")) {
        for (u32 i = 0; i < ed->dock.panel_count; ++i) {
            A3DockPanel *p = &ed->dock.panels[i];
            if (a3_ui_menu_item_check(ui, p->name, 0, p->open)) {
                if (p->open) a3_dock_close_panel(&ed->dock, (i32)i); else a3_dock_show(&ed->dock, p->name);
            }
        }
        a3_ui_menu_separator(ui);
        if (a3_ui_begin_menu(ui, "Layout")) {
            for (u32 i = 0; i < g_ed_layout_count; ++i) if (a3_ui_menu_item(ui, g_ed_layouts[i], 0, 1)) ed_layout_preset(ed, g_ed_layouts[i]);
            a3_ui_end_menu(ui);
        }
        if (a3_ui_menu_item_check(ui, "Dark Theme", 0, ed->dark_theme)) { ed->dark_theme = !ed->dark_theme; ed_apply_theme(ed); }
        a3_ui_end_popup(ui);
    }
    if (a3_ui_menubar_menu(ui, "Build")) {
        if (a3_ui_menu_item(ui, "Check Project", 0, 1)) { a3_dock_show(&ed->dock, "Build"); ed_validate_project(ed, 0, 0, 0); }
        if (a3_ui_menu_item(ui, "Build Game", "Ctrl+B", editing)) { a3_dock_show(&ed->dock, "Build"); ed_build(ed, 0); }
        if (a3_ui_menu_item(ui, "Build and Run", "Ctrl+Shift+B", editing)) { a3_dock_show(&ed->dock, "Build"); ed_build(ed, 1); }
        if (a3_ui_menu_item(ui, "Build Settings...", 0, 1)) a3_dock_show(&ed->dock, "Build");
        a3_ui_end_popup(ui);
    }
    if (a3_ui_menubar_menu(ui, "Help")) {
        if (a3_ui_menu_item(ui, "Documentation", "F1", 1)) a3_dock_show(&ed->dock, "Docs");
        if (a3_ui_menu_item(ui, "Keyboard Shortcuts", 0, 1)) ed_open_modal(ed, "shortcuts");
        if (a3_ui_menu_item(ui, "Getting Started", 0, 1)) ed_open_modal(ed, "welcome");
        if (a3_ui_menu_item(ui, "About ASM3D", 0, 1)) ed_open_modal(ed, "about");
        a3_ui_end_popup(ui);
    }
    a3_ui_end_menubar(ui);
}

static void toolbar(A3Editor *ed, A3Ui *ui, f32 W) {
    A3UiTheme *th = a3_ui_theme(ui);
    A3Rect bar = a3_rect(0, MENUBAR_H, W, TOOLBAR_H);
    a3_ui_rect(ui, bar, th->colors[A3_UIC_PANEL], 0);
    a3_ui_line(ui, a3_v2(0, bar.y + bar.h - 0.5f), a3_v2(W, bar.y + bar.h - 0.5f), th->colors[A3_UIC_BORDER], 1);
    if (!a3_ui_begin_panel(ui, "toolbar", bar, A3_PANEL_NO_SCROLL | A3_PANEL_NO_BACKGROUND)) { a3_ui_end_panel(ui); return; }
    b32 editing = ed->mode == ED_EDIT;
    a3_ui_set_cursor_pos(ui, a3_v2(bar.x + 8, bar.y + 6));
    if (a3_ui_icon_button(ui, A3_ICON_UNDO, "Undo (Ctrl+Z)", editing && ed->undo.cursor > 0 ? 0 : A3_BUTTON_DISABLED)) ed_undo(ed);
    a3_ui_same_line(ui);
    if (a3_ui_icon_button(ui, A3_ICON_REDO, "Redo (Ctrl+Y)", editing && ed->undo.cursor < ed->undo.entries.count ? 0 : A3_BUTTON_DISABLED)) ed_redo(ed);
    a3_ui_same_line(ui);
    if (a3_ui_icon_button(ui, A3_ICON_CHECK, ed->dirty ? "Save scene (Ctrl+S) - unsaved changes" : "Save scene (Ctrl+S)", editing ? (ed->dirty ? A3_BUTTON_TOGGLED : 0) : A3_BUTTON_DISABLED)) ed_scene_save(ed);
    a3_ui_same_line(ui);
    if (a3_ui_button_ex(ui, "+ Add", 0, A3_BUTTON_SMALL)) a3_ui_open_popup(ui, "add_menu");
    a3_ui_tooltip(ui, "Add an object to the scene");
    if (a3_ui_begin_popup(ui, "add_menu", 230)) { create_menu_items(ed, ui); a3_ui_end_popup(ui); }
    /* play controls centered */
    f32 cx = W * 0.5f - 70;
    a3_ui_set_cursor_pos(ui, a3_v2(cx, bar.y + 6));
    if (a3_ui_icon_button(ui, editing ? A3_ICON_PLAY : A3_ICON_STOP, editing ? "Play (F5)" : "Stop (F5)", editing ? A3_BUTTON_PRIMARY : A3_BUTTON_DANGER)) {
        if (editing) ed_play(ed); else ed_stop(ed);
    }
    a3_ui_same_line(ui);
    if (a3_ui_icon_button(ui, 0x1, ed->mode == ED_PAUSED ? "Resume (F6)" : "Pause (F6)", editing ? A3_BUTTON_DISABLED : (ed->mode == ED_PAUSED ? A3_BUTTON_TOGGLED : 0))) ed_pause(ed);
    a3_ui_same_line(ui);
    if (a3_ui_icon_button(ui, 0x2, "Step one frame (F10)", ed->mode == ED_PAUSED ? 0 : A3_BUTTON_DISABLED)) ed->step_requested = 1;
    /* right side */
    f32 rx = W - 470;
    a3_ui_set_cursor_pos(ui, a3_v2(rx, bar.y + 6));
    if (a3_ui_button_ex(ui, "Beginner", 86, ed->level == ED_LEVEL_BEGINNER ? A3_BUTTON_TOGGLED : 0)) ed->level = ED_LEVEL_BEGINNER;
    a3_ui_tooltip(ui, "Essential settings, tips and plain-language help");
    a3_ui_same_line(ui);
    if (a3_ui_button_ex(ui, "Advanced", 86, ed->level != ED_LEVEL_BEGINNER ? A3_BUTTON_TOGGLED : 0)) ed->level = ED_LEVEL_ADVANCED;
    a3_ui_tooltip(ui, "Every setting, profiler and engine internals");
    a3_ui_same_line(ui);
    if (a3_ui_button_ex(ui, "Layout", 80, 0)) a3_ui_open_popup(ui, "layout_menu");
    if (a3_ui_begin_popup(ui, "layout_menu", 180)) {
        for (u32 i = 0; i < g_ed_layout_count; ++i) if (a3_ui_menu_item(ui, g_ed_layouts[i], 0, 1)) ed_layout_preset(ed, g_ed_layouts[i]);
        a3_ui_end_popup(ui);
    }
    a3_ui_same_line(ui);
    if (a3_ui_button_ex(ui, "Build", 70, 0)) { a3_dock_show(&ed->dock, "Build"); }
    a3_ui_same_line(ui);
    if (a3_ui_icon_button(ui, 0x3, "Command palette & search (Ctrl+P)", 0)) ed_palette_open(ed);
    a3_ui_end_panel(ui);
}

static void status_bar(A3Editor *ed, A3Ui *ui, f32 W, f32 H) {
    A3UiTheme *th = a3_ui_theme(ui);
    A3Rect r = a3_rect(0, H - STATUS_H, W, STATUS_H);
    u32 bg = ed->mode == ED_PLAY ? a3_color_lerp(th->colors[A3_UIC_HEADER], th->colors[A3_UIC_SUCCESS], 0.35f)
           : ed->mode == ED_PAUSED ? a3_color_lerp(th->colors[A3_UIC_HEADER], th->colors[A3_UIC_WARNING], 0.35f)
           : th->colors[A3_UIC_HEADER];
    a3_ui_rect(ui, r, bg, 0);
    char left[256];
    A3World *w = ed_active_world(ed);
    a3_snprintf(left, sizeof(left), "%s  |  %s%s  |  %u objects  |  %s", ed->project_name, a3_path_filename(ed->scene_path), ed->dirty ? " *" : "",
                w ? a3_world_entity_count(w) : 0, ed->mode == ED_EDIT ? "Editing" : (ed->mode == ED_PLAY ? "Playing (changes are temporary)" : "Paused"));
    a3_ui_text_in_rect(ui, A3_FONT_UI, a3_rect(r.x + 10, r.y, r.w * 0.6f, r.h), A3_ALIGN_LEFT, th->colors[A3_UIC_TEXT_DIM], left);
    u32 errs = a3_log_error_count(), warns = a3_log_warning_count();
    char right[128];
    a3_snprintf(right, sizeof(right), "%u errors   %u warnings   %.0f FPS", errs, warns, (f64)ed->fps);
    A3Rect rr = a3_rect(r.x + r.w - 300, r.y, 290, r.h);
    if (a3_ui_invisible_button(ui, "status_log", rr)) a3_dock_show(&ed->dock, "Console");
    a3_ui_tooltip(ui, "Open the Console");
    a3_ui_text_in_rect(ui, A3_FONT_UI, rr, A3_ALIGN_RIGHT, errs ? th->colors[A3_UIC_ERROR] : (warns ? th->colors[A3_UIC_WARNING] : th->colors[A3_UIC_TEXT_DIM]), right);
}

/* ======================================================================== */
/* Modals                                                                   */
/* ======================================================================== */

static void modals(A3Editor *ed, A3Ui *ui) {
    A3UiTheme *th = a3_ui_theme(ui);
    if (ed->pending_modal[0]) { a3_ui_open_modal(ui, ed->pending_modal); ed->pending_modal[0] = 0; }
    if (a3_ui_begin_modal(ui, "recover", "Recover unsaved work?", 460, 210)) {
        a3_ui_label_wrapped(ui, "ASM3D found an autosave that is newer than the scene file. The editor may have closed unexpectedly. Do you want to restore it?");
        a3_ui_spacing(ui, 10);
        if (a3_ui_button_ex(ui, "Recover", 120, A3_BUTTON_PRIMARY)) {
            char rp[ED_PATH];
            ed_project_path(ed, ".asm3d/recovery/autosave.a3scene", rp, sizeof(rp));
            A3World *w = a3_world_create("Recovered");
            A3SceneLoadReport rep;
            if (a3_scene_load_file(w, rp, &rep) == A3_OK) {
                if (ed->world) { a3_physics_release(ed->world); a3_world_destroy(ed->world); }
                ed->world = w;
                ed->dirty = 1;
                ed->selected = 0;
                ed_undo_clear(&ed->undo);
                a3_ui_notify(ui, th->colors[A3_UIC_SUCCESS], "Recovered autosave - save to keep it");
            } else {
                a3_world_destroy(w);
                A3_ERROR("editor", "the autosave could not be read: %s", rep.error);
            }
            a3_ui_close_modal(ui);
        }
        a3_ui_same_line(ui);
        if (a3_ui_button_ex(ui, "Discard", 120, 0)) {
            char rp[ED_PATH];
            ed_project_path(ed, ".asm3d/recovery/autosave.a3scene", rp, sizeof(rp));
            a3_file_delete(rp);
            a3_ui_close_modal(ui);
        }
        a3_ui_end_modal(ui);
    }
    if (a3_ui_begin_modal(ui, "quit", "Save changes before closing?", 460, 190)) {
        a3_ui_label_wrapped(ui, "The scene has unsaved changes. If you close without saving they will be lost (an autosave copy may still exist).");
        a3_ui_spacing(ui, 10);
        if (a3_ui_button_ex(ui, "Save and Close", 140, A3_BUTTON_PRIMARY)) { if (ed->mode != ED_EDIT) ed_stop(ed); if (ed_scene_save(ed)) ed->quit = 2; a3_ui_close_modal(ui); }
        a3_ui_same_line(ui);
        if (a3_ui_button_ex(ui, "Don't Save", 120, A3_BUTTON_DANGER)) { ed->quit = 2; a3_ui_close_modal(ui); }
        a3_ui_same_line(ui);
        if (a3_ui_button_ex(ui, "Cancel", 100, 0)) { ed->quit = 0; a3_ui_close_modal(ui); }
        a3_ui_end_modal(ui);
    }
    if (a3_ui_begin_modal(ui, "welcome", "Your project is ready!", 520, 330)) {
        a3_ui_label_wrapped(ui, "A few things to try first:");
        a3_ui_spacing(ui, 4);
        a3_ui_label(ui, "%s  Press Play (F5) to try the template.", "\xE2\x80\xA2");
        a3_ui_label(ui, "%s  Hold the right mouse button and use WASD to fly.", "\xE2\x80\xA2");
        a3_ui_label(ui, "%s  Click an object, then drag the colored arrows.", "\xE2\x80\xA2");
        a3_ui_label(ui, "%s  Use + Add to place shapes, lights and a player.", "\xE2\x80\xA2");
        a3_ui_label(ui, "%s  Ctrl+P searches every command and setting.", "\xE2\x80\xA2");
        a3_ui_label(ui, "%s  Ctrl+Z undoes anything. Work is autosaved.", "\xE2\x80\xA2");
        a3_ui_spacing(ui, 12);
        if (a3_ui_button_ex(ui, "Let's go", 140, A3_BUTTON_PRIMARY)) a3_ui_close_modal(ui);
        a3_ui_end_modal(ui);
    }
    if (a3_ui_begin_modal(ui, "shortcuts", "Keyboard Shortcuts", 520, 470)) {
        static const char *const rows[][2] = {
            { "Ctrl+S", "Save scene" }, { "Ctrl+Z / Ctrl+Y", "Undo / Redo" }, { "Ctrl+D", "Duplicate" }, { "Delete", "Delete selection" },
            { "F", "Focus selection" }, { "W / E / R", "Move / Rotate / Scale tool" }, { "Ctrl (while dragging)", "Snap" },
            { "Right mouse + WASD/QE", "Fly camera" }, { "Alt + left drag", "Orbit" }, { "Middle drag", "Pan" },
            { "F5 / F6 / F10", "Play-Stop / Pause / Step" }, { "Ctrl+P", "Command palette" }, { "Ctrl+B", "Build game" },
            { "Escape (playing)", "Release the mouse" }, { "F1", "Documentation" },
        };
        for (u32 i = 0; i < A3_ARRAY_COUNT(rows); ++i) {
            a3_ui_property(ui, rows[i][0], 0);
            a3_ui_label(ui, "%s", rows[i][1]);
        }
        a3_ui_end_modal(ui);
    }
    if (a3_ui_begin_modal(ui, "about", "About ASM3D", 460, 250)) {
        a3_ui_label_font(ui, A3_FONT_HEADING, th->colors[A3_UIC_TEXT], "ASM3D " A3_VERSION_STRING);
        a3_ui_label_wrapped(ui, "A desktop game engine and editor written in C with x86-64 assembly kernels for SIMD math, culling and physics.");
        a3_ui_spacing(ui, 6);
        a3_ui_label_colored(ui, th->colors[A3_UIC_TEXT_DIM], "UI font: DejaVu Sans (see THIRD_PARTY.md)");
        a3_ui_label_colored(ui, th->colors[A3_UIC_TEXT_DIM], "Platform: %s", a3_platform_name());
        a3_ui_end_modal(ui);
    }
}

/* ======================================================================== */
/* Shortcuts                                                                */
/* ======================================================================== */

static void shortcuts(A3Editor *ed, A3Ui *ui) {
    const A3InputState *in = a3_ui_input(ui);
    b32 ctrl = (in->mods & A3_MOD_CTRL) != 0, shift = (in->mods & A3_MOD_SHIFT) != 0;
    if (in->keys_pressed[A3_KEY_F5]) { if (ed->mode == ED_EDIT) ed_play(ed); else ed_stop(ed); }
    if (in->keys_pressed[A3_KEY_F6]) ed_pause(ed);
    if (in->keys_pressed[A3_KEY_F10] && ed->mode == ED_PAUSED) ed->step_requested = 1;
    if (in->keys_pressed[A3_KEY_F1]) a3_dock_show(&ed->dock, "Docs");
    if (ctrl && in->keys_pressed[A3_KEY_P]) ed_palette_open(ed);
    if (ctrl && in->keys_pressed[A3_KEY_Q]) ed->quit = 1;
    if (a3_ui_wants_keyboard(ui) || in->mouse_captured) return; /* text fields get the remaining keys */
    if (ctrl && in->keys_pressed[A3_KEY_S]) ed_scene_save(ed);
    if (ed_model_active(ed)) {
        /* Edit Mode: undo is per mesh edit; the viewport handles the modeling keys */
        if (ctrl && in->keys_pressed[A3_KEY_Z]) { if (shift) ed_model_redo(ed); else ed_model_undo(ed); }
        if (ctrl && in->keys_pressed[A3_KEY_Y]) ed_model_redo(ed);
        return;
    }
    if (ctrl && in->keys_pressed[A3_KEY_Z]) { if (shift) ed_redo(ed); else ed_undo(ed); }
    if (ctrl && in->keys_pressed[A3_KEY_Y]) ed_redo(ed);
    if (ctrl && in->keys_pressed[A3_KEY_D]) ed_duplicate_selected(ed);
    if (ctrl && in->keys_pressed[A3_KEY_B]) { a3_dock_show(&ed->dock, "Build"); ed_build(ed, shift); }
    if (in->keys_pressed[A3_KEY_DELETE]) ed_delete_selected(ed);
}

/* ======================================================================== */
/* Frame                                                                    */
/* ======================================================================== */

static void clip_set(void *u, const char *t) { a3_window_set_clipboard((A3Window *)u, t); }
static const char *clip_get(void *u) { return a3_window_get_clipboard((A3Window *)u); }

static void register_panels(A3Editor *ed) {
    A3Dock *d = &ed->dock;
    a3_dock_init(d);
    a3_dock_add_panel(d, "Viewport", A3_ICON_CUBE, ed_viewport_panel, ed, A3_PANEL_NO_SCROLL);
    a3_dock_add_panel(d, "Hierarchy", A3_ICON_HAMBURGER, ed_hierarchy_panel, ed, 0);
    a3_dock_add_panel(d, "Inspector", A3_ICON_GEAR, ed_inspector_panel, ed, 0);
    a3_dock_add_panel(d, "Assets", A3_ICON_BOX, ed_assets_panel, ed, A3_PANEL_NO_SCROLL);
    a3_dock_add_panel(d, "Console", A3_ICON_MENU, ed_console_panel, ed, A3_PANEL_NO_SCROLL);
    a3_dock_add_panel(d, "Profiler", A3_ICON_BOLT, ed_profiler_panel, ed, 0);
    a3_dock_add_panel(d, "Docs", A3_ICON_STAR, ed_docs_panel, ed, A3_PANEL_NO_SCROLL);
    a3_dock_add_panel(d, "Settings", A3_ICON_GEAR, ed_settings_panel, ed, 0);
    a3_dock_add_panel(d, "Build", A3_ICON_PLAY, ed_build_panel, ed, 0);
    a3_dock_add_panel(d, "Code", A3_ICON_PENCIL, ed_code_panel, ed, A3_PANEL_NO_SCROLL);
    a3_dock_add_panel(d, "Modeling", A3_ICON_CUBE, ed_model_panel, ed, 0);
    a3_dock_add_panel(d, "Shader Maker", A3_ICON_DIAMOND, ed_shader_panel, ed, A3_PANEL_NO_SCROLL);
    a3_dock_add_panel(d, "Animation", A3_ICON_RIGHT, ed_anim_panel, ed, A3_PANEL_NO_SCROLL);
}

static void route_game_input(A3Editor *ed, const A3InputState *real) {
    /* the running game only sees input while the viewport has focus, so
     * clicking editor buttons never shoots, jumps or grabs the mouse */
    if (ed->mode == ED_PLAY && (ed->game_focused || real->mouse_captured)) {
        ed->game_input = *real;
    } else {
        a3_zero_struct(&ed->game_input);
        ed->game_input.mouse_pos = real->mouse_pos;
        ed->game_input.focused = real->focused;
    }
    a3_engine_set_input_override(ed->engine, &ed->game_input);
}

static void editor_frame(A3Editor *ed, f32 dt) {
    A3Ui *ui = ed->ui;
    A3Window *win = a3_engine_window(ed->engine);
    i32 w, h;
    a3_window_size(win, &w, &h);
    const A3InputState *real = a3_engine_input(ed->engine);
    /* while the game owns the mouse the editor UI ignores it */
    A3InputState ui_in;
    const A3InputState *in = real;
    if (real->mouse_captured) {
        ui_in = *real;
        a3_zero(ui_in.mouse, sizeof(ui_in.mouse));
        a3_zero(ui_in.mouse_pressed, sizeof(ui_in.mouse_pressed));
        a3_zero(ui_in.mouse_released, sizeof(ui_in.mouse_released));
        ui_in.mouse_pos = a3_v2(-1000, -1000);
        in = &ui_in;
    }
    a3_ui_begin_frame(ui, in, w, h, dt);
    A3UiTheme *th = a3_ui_theme(ui);
    f32 W = (f32)w, H = (f32)h;
    a3_ui_rect(ui, a3_rect(0, 0, W, H), th->colors[A3_UIC_BG], 0);
    if (ed->want_start_screen) {
        ed->want_start_screen = 0;
        if (ed->dirty && ed->mode == ED_EDIT) ed_scene_save(ed);
        ed_project_close(ed);
    }
    if (!ed->has_project) {
        start_screen(ed, ui, W, H);
    } else {
        ed_undo_begin_frame(ed);
        menubar(ed, ui, W);
        toolbar(ed, ui, W);
        a3_dock_draw(&ed->dock, ui, a3_rect(0, MENUBAR_H + TOOLBAR_H, W, H - MENUBAR_H - TOOLBAR_H - STATUS_H));
        status_bar(ed, ui, W, H);
        ed_palette_draw(ed, ui);
        shortcuts(ed, ui);
        ed_undo_end_frame(ed);
    }
    modals(ed, ui);
    a3_ui_end_frame(ui);
    /* simulation */
    route_game_input(ed, real);
    if (ed->has_project) {
        if (ed->mode == ED_EDIT) a3_engine_simulate(ed->engine, ed->world, dt, 0, 0);
        else {
            a3_engine_simulate(ed->engine, ed->play_world, dt, ed->mode == ED_PAUSED, ed->step_requested);
            ed->step_requested = 0;
        }
        /* keep the edit world's matrices current for gizmos and picking */
        if (ed->mode == ED_EDIT) a3_transform_system_update(ed->world);
        if (ed->mode != ED_EDIT && real->mouse_captured && real->keys_pressed[A3_KEY_ESCAPE]) ed->game_focused = 0;
        ed_shader_render_preview(ed);   /* before the viewport: debug lines are per draw */
        ed_viewport_render(ed);
        ed_autosave_tick(ed);
    }
    a3_rhi_target_bind((A3RhiTarget){ 0 }, w, h);
    a3_rhi_clear(1, a3_v4(0, 0, 0, 1), 1, 1);
    a3_ui_render(ui);
    a3_window_set_cursor(win, a3_ui_cursor(ui));
    /* title */
    char title[256];
    if (ed->has_project) a3_snprintf(title, sizeof(title), "%s%s - %s - ASM3D Editor", a3_path_filename(ed->scene_path), ed->dirty ? " *" : "", ed->project_name);
    else a3_snprintf(title, sizeof(title), "ASM3D Editor");
    static char last_title[256];
    if (!a3_streq(title, last_title)) { a3_window_set_title(win, title); a3_strcpy(last_title, sizeof(last_title), title); }
    /* profiler history */
    ed->frame_ms[ed->frame_ms_pos] = dt * 1000.0f;
    ed->frame_ms_pos = (ed->frame_ms_pos + 1) % A3_ARRAY_COUNT(ed->frame_ms);
    f32 sum = 0;
    for (u32 i = 0; i < 60; ++i) sum += ed->frame_ms[(ed->frame_ms_pos + A3_ARRAY_COUNT(ed->frame_ms) - 1 - i) % A3_ARRAY_COUNT(ed->frame_ms)];
    ed->fps = sum > 0 ? 60000.0f / sum : 0;
}

/* ======================================================================== */
/* Self test: drives the editor API end to end without a user              */
/* ======================================================================== */

#define ST_CHECK(cond) do { if (!(cond)) { A3_ERROR("selftest", "FAILED: %s (line %d)", #cond, __LINE__); return 1; } else ++passed; } while (0)

static int selftest_run(A3Editor *ed, const char *tmp) {
    int passed = 0;
    ST_CHECK(ed_project_create(ed, tmp, "SelfTest", 7));
    ST_CHECK(ed->world && a3_world_entity_count(ed->world) > 10);
    u32 n0 = a3_world_entity_count(ed->world);
    /* create + undo + redo */
    A3Entity e = ed_create_entity(ed, "Test Cube", A3_PRIM_CUBE, 0);
    ST_CHECK(a3_entity_valid(ed->world, e));
    ed_undo_end_frame(ed);
    ST_CHECK(ed->undo.entries.count == 1);
    ST_CHECK(a3_world_entity_count(ed->world) == n0 + 1);
    ST_CHECK(ed_undo(ed));
    ST_CHECK(a3_world_entity_count(ed->world) == n0);
    ST_CHECK(ed_redo(ed));
    ST_CHECK(a3_world_entity_count(ed->world) == n0 + 1);
    e = ed_selected(ed);
    ST_CHECK(a3_entity_valid(ed->world, e) && a3_streq(a3_entity_name(ed->world, e), "Test Cube"));
    /* property edit captured as one undo step */
    ed->frame = 1;
    ed_undo_begin_frame(ed);
    A3CTransform *t = a3_transform(ed->world, e);
    A3Vec3 old = t->position;
    t->position = a3_v3(5, 6, 7);
    ed_undo_mark_changed(ed, "Move");
    ed_undo_end_frame(ed);
    ST_CHECK(ed->undo.entries.count == 2);
    ST_CHECK(ed_undo(ed));
    t = a3_transform(ed->world, ed_selected(ed));
    ST_CHECK(t && a3_absf(t->position.x - old.x) < 1e-5f && a3_absf(t->position.z - old.z) < 1e-5f);
    ST_CHECK(ed_redo(ed));
    t = a3_transform(ed->world, ed_selected(ed));
    ST_CHECK(t && t->position.x == 5 && t->position.y == 6 && t->position.z == 7);
    /* delete + undo keeps GUID and sibling order */
    u64 guid = ed->selected;
    ed_delete_selected(ed);
    ST_CHECK(!a3_entity_valid(ed->world, a3_entity_find_by_guid(ed->world, guid)));
    ST_CHECK(ed_undo(ed));
    ST_CHECK(a3_entity_valid(ed->world, a3_entity_find_by_guid(ed->world, guid)));
    /* duplicate */
    ed->selected = guid;
    ed_duplicate_selected(ed);
    ed_undo_end_frame(ed);
    ST_CHECK(a3_world_entity_count(ed->world) == n0 + 2);
    /* play mode runs on a copy and restores */
    A3Entity crate = a3_entity_find_by_name(ed->world, "Crate");
    ST_CHECK(a3_entity_valid(ed->world, crate));
    A3Vec3 crate_pos = a3_transform(ed->world, crate)->position;
    ed->selected = a3_entity_guid(ed->world, crate);
    ed_play(ed);
    ST_CHECK(ed->mode == ED_PLAY && ed->play_world);
    route_game_input(ed, a3_engine_input(ed->engine));
    for (int i = 0; i < 90; ++i) a3_engine_simulate(ed->engine, ed->play_world, 1.0f / 60.0f, 0, 0);
    ST_CHECK(a3_entity_valid(ed->play_world, ed_selected(ed)));
    ed_pause(ed);
    ST_CHECK(ed->mode == ED_PAUSED);
    ed_stop(ed);
    ST_CHECK(ed->mode == ED_EDIT && !ed->play_world);
    A3Vec3 after = a3_transform(ed->world, crate)->position;
    ST_CHECK(after.x == crate_pos.x && after.y == crate_pos.y && after.z == crate_pos.z);
    /* keyframe animation plays in the game and never changes the edited scene */
    {
        const char *clip_json = "{\"format\":\"asm3d.animation\",\"version\":1,\"name\":\"lift\",\"duration\":1,"
                                "\"tracks\":[{\"component\":\"Transform\",\"field\":\"position\",\"keys\":[[0,0,0,0,0,1],[1,0,5,0,0,1]]}]}";
        char cp[ED_PATH];
        ed_project_path(ed, "Assets/Animations/lift.a3anim", cp, sizeof(cp));
        char cdir[ED_PATH];
        a3_path_dirname(cp, cdir, sizeof(cdir));
        a3_dir_create(cdir);
        ST_CHECK(a3_file_write_atomic(cp, clip_json, a3_strlen(clip_json)) == A3_OK);
        A3Entity lift = ed_create_entity(ed, "Lift", A3_PRIM_CUBE, "Animator");
        A3CAnimator *anm = (A3CAnimator *)a3_component_get(ed->world, lift, A3_T_ANIMATOR);
        ST_CHECK(anm != 0);
        a3_strcpy(anm->clip.path, sizeof(anm->clip.path), "Assets/Animations/lift.a3anim");
        anm->loop = A3_ANIM_ONCE;
        ed_undo_end_frame(ed);
        u64 lift_guid = a3_entity_guid(ed->world, lift);
        ed_play(ed);
        for (int i = 0; i < 90; ++i) a3_engine_simulate(ed->engine, ed->play_world, 1.0f / 60.0f, 0, 0);
        A3Entity pl = a3_entity_find_by_guid(ed->play_world, lift_guid);
        A3CTransform *lt = a3_transform(ed->play_world, pl);
        ST_CHECK(lt && a3_absf(lt->position.y - 5.0f) < 1e-3f);
        ed_stop(ed);
        lt = a3_transform(ed->world, a3_entity_find_by_guid(ed->world, lift_guid));
        ST_CHECK(lt && a3_absf(lt->position.y - 5.0f) > 1.0f);
    }
    /* scripts: run in play mode, hot reload, never touch the edited scene, error markers */
    {
        a3_scripts_clear_errors();
        char sp[ED_PATH], sdir[ED_PATH];
        ed_project_path(ed, "Assets/Scripts/Spin.a3script", sp, sizeof(sp));
        a3_path_dirname(sp, sdir, sizeof(sdir));
        a3_dir_create(sdir);
        const char *v1 = "let frames = 0\nfn on_update(dt) {\n    frames += 1\n    self.position = vec3(0, 3, 0)\n    self.name = \"Spun\"\n}\n";
        ST_CHECK(a3_file_write_atomic(sp, v1, a3_strlen(v1)) == A3_OK);
        A3Entity sc = ed_create_entity(ed, "Spinner", A3_PRIM_CUBE, "Script");
        A3CScript *scc = (A3CScript *)a3_component_get(ed->world, sc, A3_T_SCRIPT);
        ST_CHECK(scc != 0);
        a3_strcpy(scc->script.path, sizeof(scc->script.path), "Assets/Scripts/Spin.a3script");
        ed_undo_end_frame(ed);
        u64 sg = a3_entity_guid(ed->world, sc);
        ed_play(ed);
        for (int i = 0; i < 10; ++i) a3_engine_simulate(ed->engine, ed->play_world, 1.0f / 60.0f, 0, 0);
        A3Entity ps = a3_entity_find_by_guid(ed->play_world, sg);
        ST_CHECK(a3_streq(a3_entity_name(ed->play_world, ps), "Spun"));
        ST_CHECK(a3_absf(a3_transform(ed->play_world, ps)->position.y - 3.0f) < 1e-4f);
        const char *v2 = "let frames = 0\nfn on_update(dt) {\n    frames += 1\n    self.position = vec3(0, 7, 0)\n}\n";
        ST_CHECK(a3_file_write_atomic(sp, v2, a3_strlen(v2)) == A3_OK);
        a3_scripts_invalidate("Assets/Scripts/Spin.a3script");
        for (int i = 0; i < 3; ++i) a3_engine_simulate(ed->engine, ed->play_world, 1.0f / 60.0f, 0, 0);
        ST_CHECK(a3_absf(a3_transform(ed->play_world, ps)->position.y - 7.0f) < 1e-4f);
        ST_CHECK(a3_scripts_error_count() == 0);
        ed_stop(ed);
        ST_CHECK(a3_streq(a3_entity_name(ed->world, a3_entity_find_by_guid(ed->world, sg)), "Spinner"));
        char bp[ED_PATH];
        ed_project_path(ed, "Assets/Scripts/Bad.a3script", bp, sizeof(bp));
        const char *bad = "fn on_update(dt) {\n    prnt(dt)\n}\n";
        ST_CHECK(a3_file_write_atomic(bp, bad, a3_strlen(bad)) == A3_OK);
        i32 bd = ed_code_open(ed, "Assets/Scripts/Bad.a3script");
        ST_CHECK(bd >= 0 && ed->docs[bd].error_count == 1 && ed->docs[bd].error_lines[0] == 2);
        /* the project check compiles every script: a broken one is an error */
        u32 se = 0, sw = 0;
        ST_CHECK(ed_validate_project(ed, 0, &se, &sw) && se == 1);
        a3_file_delete(bp);
    }
    /* Edit Mode: convert a cube, extrude its top, undo/redo, save the model */
    {
        A3Entity mc = ed_create_entity(ed, "Tower", A3_PRIM_CUBE, 0);
        ed_undo_end_frame(ed);
        ed_select(ed, mc);
        u64 mg = a3_entity_guid(ed->world, mc);
        ST_CHECK(ed_model_enter(ed) && ed_model_active(ed));
        const A3EMesh *em = ed_model_mesh(ed);
        ST_CHECK(em && a3_emesh_vertex_count(em) == 8 && a3_emesh_face_count(em) == 6);
        ST_CHECK(ed_model_run(ed, "mode:face") && ed_model_run(ed, "select-none") && ed_model_run(ed, "select-normal:0,1,0"));
        ST_CHECK(ed_model_run(ed, "extrude:1") && a3_emesh_face_count(ed_model_mesh(ed)) == 10);
        ST_CHECK(ed_model_run(ed, "inset:0.3") && a3_emesh_face_count(ed_model_mesh(ed)) == 14);
        ST_CHECK(!ed_model_run(ed, "extrud:1"));                        /* unknown op: nothing changes */
        ST_CHECK(a3_emesh_face_count(ed_model_mesh(ed)) == 14);
        ST_CHECK(ed_model_undo(ed) && a3_emesh_face_count(ed_model_mesh(ed)) == 10);
        ST_CHECK(ed_model_redo(ed) && a3_emesh_face_count(ed_model_mesh(ed)) == 14);
        /* the viewport shows the edited mesh right away */
        A3CMeshRenderer *mmr = (A3CMeshRenderer *)a3_component_get(ed->world, a3_entity_find_by_guid(ed->world, mg), A3_T_MESH_RENDERER);
        ST_CHECK(mmr && mmr->primitive == A3_PRIM_NONE && a3_streq(mmr->mesh.path, "Assets/Models/Tower.obj"));
        const A3MeshAsset *ma = a3_assets_mesh_get(a3_assets_mesh_for_renderer(mmr));
        ST_CHECK(ma && ma->cpu.index_count == 28 * 3);                  /* 14 quads = 28 triangles */
        ed_model_exit(ed, 1);
        ST_CHECK(!ed_model_active(ed));
        char mp[ED_PATH];
        ed_project_path(ed, "Assets/Models/Tower.obj", mp, sizeof(mp));
        ST_CHECK(a3_file_exists(mp));
        /* reopening reads the saved polygons back */
        ed_select(ed, a3_entity_find_by_guid(ed->world, mg));
        ST_CHECK(ed_model_enter(ed) && a3_emesh_face_count(ed_model_mesh(ed)) == 14);
        ed_model_exit(ed, 0);
    }
    /* save + reopen */
    ST_CHECK(ed_scene_save(ed));
    u32 saved_count = a3_world_entity_count(ed->world);
    char dir[ED_PATH];
    a3_strcpy(dir, sizeof(dir), ed->project_dir);
    ST_CHECK(ed_project_open(ed, dir));
    ST_CHECK(a3_world_entity_count(ed->world) == saved_count);
    /* validation + build */
    A3StrBuf report;
    a3_strbuf_init(&report, A3_MEM_EDITOR);
    u32 errors = 0, warnings = 0;
    ST_CHECK(ed_validate_project(ed, &report, &errors, &warnings));
    ST_CHECK(errors == 0);
    a3_strbuf_free(&report);
#if !A3_PLATFORM_WEB   /* the browser editor cannot run compilers or write executables */
    ST_CHECK(ed_build(ed, 0));
    char exe[ED_PATH];
    a3_path_join(exe, sizeof(exe), ed->build_output, "data/project.a3proj");
    ST_CHECK(a3_file_exists(exe));
    {
        char game[ED_PATH], gname[96];
#if A3_PLATFORM_WINDOWS
        a3_snprintf(gname, sizeof(gname), "%s.exe", "SelfTest");
#else
        a3_snprintf(gname, sizeof(gname), "%s", "SelfTest");
#endif
        a3_path_join(game, sizeof(game), ed->build_output, gname);
        ST_CHECK(a3_file_exists(game));
    }
#endif
    /* code editor round trip */
    i32 doc = ed_code_open(ed, "Assets/Scripts/selftest.txt");
    ST_CHECK(doc >= 0);
    ST_CHECK(ed_code_save(ed, doc));
    /* Shader Maker: presets compile on the GPU, errors map to nodes, materials load */
    ST_CHECK(ed_shader_selftest(ed) == 0);
    /* palette commands resolve */
    ed_run_command(ed, "Toggle Grid");
    /* the Platformer template plays with its scripts: collecting an orb updates the HUD */
    {
        a3_scripts_clear_errors();
        ST_CHECK(ed_project_create(ed, tmp, "Plat", 3));
        ed_play(ed);
        for (int i = 0; i < 5; ++i) a3_engine_simulate(ed->engine, ed->play_world, 1.0f / 60.0f, 0, 0);
        const A3HudCmd *hud;
        u32 nh = a3_scripts_hud(ed->play_world, &hud);
        ST_CHECK(nh >= 3 && a3_streq(hud[1].text, "Orbs  0 / 12"));
        A3Entity orb = a3_entity_find_by_name(ed->play_world, "Orb");
        A3Entity pl = a3_entity_find_by_name(ed->play_world, "Player");
        ST_CHECK(a3_entity_valid(ed->play_world, orb) && a3_entity_valid(ed->play_world, pl));
        A3Vec3 op = a3_transform_world_position(ed->play_world, orb);
        a3_transform(ed->play_world, pl)->position = a3_v3(op.x, op.y - 0.8f, op.z);
        for (int i = 0; i < 10; ++i) a3_engine_simulate(ed->engine, ed->play_world, 1.0f / 60.0f, 0, 0);
        nh = a3_scripts_hud(ed->play_world, &hud);
        ST_CHECK(nh >= 3 && a3_streq(hud[1].text, "Orbs  1 / 12"));
        ST_CHECK(a3_scripts_error_count() == 0);
        ed_stop(ed);
    }
    A3_INFO("selftest", "editor self test passed (%d checks)", passed);
    return 0;
}

static int selftest(A3Editor *ed) {
    ed->no_recent = 1;
    char tmp[ED_PATH], base[ED_PATH];
    if (!a3_get_user_data_dir(base, sizeof(base))) a3_strcpy(base, sizeof(base), ".");
    a3_snprintf(tmp, sizeof(tmp), "%s/selftest-%llu", base, (unsigned long long)a3_time_ns());
    a3_dir_create(tmp);
    int rc = selftest_run(ed, tmp);
    ed_project_close(ed);
    a3_dir_delete_recursive(tmp);
    return rc;
}

/* ======================================================================== */
/* main                                                                     */
/* ======================================================================== */

/* ---- program: start, one frame per tick, shutdown ----
 * The desktop loops over editor_tick(); the browser build (web/asm3d.js)
 * calls it once per animation frame. */
typedef struct EdRun {
    A3Engine *eng;
    A3Editor *ed;
    int frames;
    const char *shot, *select, *mesh_ops;
    b32 edit_mesh;
    int rc;
    b32 running;
} EdRun;

static EdRun g_run;
static A3Editor g_editor;

/* atoi without libc: leading integer of s, or fallback when there is none */
static i64 a3_parse_int_or(const char *s, i64 fallback) {
    usize n = 0;
    if (s[n] == '-' || s[n] == '+') ++n;
    while (a3_is_digit(s[n])) ++n;
    i64 v;
    return n && a3_parse_i64(s, n, &v) ? v : fallback;
}

static int editor_start(EdRun *run, int argc, char **argv) {
    const char *project = 0, *new_name = 0, *new_loc = 0, *layout = 0, *shot = 0, *select = 0, *show = 0, *open_file = 0, *mesh_ops = 0;
    b32 edit_mesh = 0;
    int frames = -1, width = 1600, height = 900, tpl = 0;
    b32 hidden = 0, beginner = 0, advanced = 0, light = 0, run_selftest = 0, build_only = 0;
    i32 play_at = -1, shader_preset = -1;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        b32 more = i + 1 < argc;
        if (a3_streq(a, "--project") && more) project = argv[++i];
        else if (a3_streq(a, "--new") && more) new_name = argv[++i];
        else if (a3_streq(a, "--location") && more) new_loc = argv[++i];
        else if (a3_streq(a, "--template") && more) tpl = (int)a3_parse_int_or(argv[++i], 0);
        else if (a3_streq(a, "--layout") && more) layout = argv[++i];
        else if (a3_streq(a, "--frames") && more) frames = (int)a3_parse_int_or(argv[++i], -1);
        else if (a3_streq(a, "--screenshot") && more) shot = argv[++i];
        else if (a3_streq(a, "--select") && more) select = argv[++i];
        else if (a3_streq(a, "--show") && more) show = argv[++i];
        else if (a3_streq(a, "--open") && more) open_file = argv[++i];
        else if (a3_streq(a, "--play-at") && more) play_at = (i32)a3_parse_int_or(argv[++i], -1);
        else if (a3_streq(a, "--shader-preset") && more) shader_preset = (i32)a3_parse_int_or(argv[++i], -1);
        else if (a3_streq(a, "--edit-mesh")) edit_mesh = 1;
        else if (a3_streq(a, "--mesh-ops") && more) { mesh_ops = argv[++i]; edit_mesh = 1; }
        else if (a3_streq(a, "--hidden")) hidden = 1;
        else if (a3_streq(a, "--beginner")) beginner = 1;
        else if (a3_streq(a, "--advanced")) advanced = 1;
        else if (a3_streq(a, "--light")) light = 1;
        else if (a3_streq(a, "--selftest")) { run_selftest = 1; hidden = 1; }
        else if (a3_streq(a, "--build")) { build_only = 1; hidden = 1; }
        else if (a3_streq(a, "--size") && more) {
            const char *s = argv[++i];
            width = (int)a3_parse_int_or(s, width);
            const char *x = a3_strchr(s, 'x');
            if (x) height = (int)a3_parse_int_or(x + 1, height);
        } else if (a3_streq(a, "--help")) {
            static const char usage[] =
                "usage: asm3d_editor [--project DIR] [--new NAME --location DIR --template N] [--layout NAME]\n"
                "                    [--beginner|--advanced] [--light] [--select NAME] [--show PANEL] [--open FILE]\n"
                "                    [--play-at N] [--shader-preset N] [--frames N --screenshot FILE] [--hidden] [--size WxH]\n"
                "                    [--edit-mesh] [--mesh-ops \"op;op\"] [--selftest] [--build]\n";
            a3_console_write(A3_LOG_INFO, usage, sizeof(usage) - 1);
            return 0;
        }
    }
    A3EngineDesc d = { "ASM3D Editor", width, height, 1, hidden, 1, "", 60 };
    A3Engine *eng = a3_engine_create(&d);
    if (!eng) { A3_FATAL("editor", "could not start: no OpenGL 3.3 capable display was found"); return 1; }
    A3Editor *ed = &g_editor;
    ed->engine = eng;
    ed->ui = a3_ui_create();
    if (!ed->ui) { a3_engine_destroy(eng); return 1; }
    run->eng = eng;
    run->ed = ed;
    a3_ui_set_clipboard_fns(ed->ui, clip_set, clip_get, a3_engine_window(eng));
    ed->dark_theme = !light;
    ed_apply_theme(ed);
    ed->level = beginner ? ED_LEVEL_BEGINNER : ED_LEVEL_ADVANCED;
    if (advanced) ed->level = ED_LEVEL_ADVANCED;
    ed->cam_pos = a3_v3(0, 4, 10);
    ed->cam_pitch = -0.3f;
    ed->cam_speed = 8.0f;
    ed->gizmo_axis = -1;
    ed->show_grid = ed->show_icons = 1;
    ed->console_autoscroll = 1;
    ed->log_filter.info = ed->log_filter.warn = ed->log_filter.error = 1;
    ed->doc_active = -1;
    ed->play_at_frame = play_at;
    a3_strcpy(ed->new_name, sizeof(ed->new_name), "My Game");
    if (!a3_get_user_data_dir(ed->new_location, sizeof(ed->new_location))) a3_get_cwd(ed->new_location, sizeof(ed->new_location));
    else a3_strcat(ed->new_location, sizeof(ed->new_location), "/projects");
    ed_undo_init(&ed->undo);
    register_panels(ed);
    ed_layout_preset(ed, ed->level == ED_LEVEL_BEGINNER ? "Beginner" : "Default");
    ed_shader_init(ed);
    ed_recent_load(ed);
    if (run_selftest) return selftest(ed);
    if (new_name) {
        char loc[ED_PATH];
        a3_strcpy(loc, sizeof(loc), new_loc ? new_loc : ed->new_location);
        a3_dir_create(loc);
        if (ed_project_create(ed, loc, new_name, tpl)) ed->show_welcome = 0;
    } else if (project) {
        ed_project_open(ed, project);
    }
    if (build_only) {
        int rc = ed->has_project && ed_build(ed, 0) ? 0 : 1;
        if (rc) A3_ERROR("editor", "build failed:\n%s%s", ed->build_report, ed->build_log);
        return rc;
    }
    if (layout) ed_layout_preset(ed, layout);
    else if (ed->has_project && new_name) ed_layout_preset(ed, ed->level == ED_LEVEL_BEGINNER ? "Beginner" : "Default");
    if (show) a3_dock_show(&ed->dock, show);
    if (open_file && ed->has_project) {
        if (a3_str_ends_with(open_file, ".a3shader")) { if (ed_shader_open(ed, open_file)) a3_dock_show(&ed->dock, "Shader Maker"); }
        else { ed_code_open(ed, open_file); a3_dock_show(&ed->dock, "Code"); }
    }
    if (shader_preset >= 0 && ed->shader) ed_shader_preset(ed, shader_preset);
    if (ed->show_welcome && frames < 0) ed_open_modal(ed, "welcome");
    ed->show_welcome = 0;
    run->frames = frames;
    run->shot = shot;
    run->select = select;
    run->mesh_ops = mesh_ops;
    run->edit_mesh = edit_mesh;
    run->running = 1;
    return -1;   /* keep running: call editor_tick() every frame */
}

/* One editor frame. Returns false when the editor should shut down. */
static b32 editor_tick(EdRun *run) {
    A3Editor *ed = run->ed;
    A3Engine *eng = run->eng;
    if (!run->running) return 0;
    f32 dt;
    b32 alive = a3_engine_begin_frame(eng, &dt);
    if (!alive) {
        if (ed->quit != 2 && ed->has_project && ed->dirty) {
            a3_window_cancel_close(a3_engine_window(eng));
            ed_open_modal(ed, "quit");
        } else { run->running = 0; a3_engine_end_frame(eng); return 0; }
    }
    if (ed->quit == 1) {
        if (ed->has_project && ed->dirty) { ed->quit = 0; ed_open_modal(ed, "quit"); }
        else ed->quit = 2;
    }
    if (run->frames >= 0) dt = 1.0f / 60.0f;
    if (ed->frame == 2 && run->select && ed->world) {
        A3Entity e = a3_entity_find_by_name(ed->world, run->select);
        if (a3_entity_valid(ed->world, e)) { ed_select(ed, e); ed_focus_selected(ed); }
        else A3_WARN("editor", "--select: no entity named '%s'", run->select);
    }
    if (ed->frame == 3 && run->edit_mesh && ed->world && ed_model_enter(ed) && run->mesh_ops) {
        char buf[1024];
        a3_strcpy(buf, sizeof(buf), run->mesh_ops);
        for (char *tok = buf, *next; tok && *tok; tok = next) {
            next = (char *)a3_strchr(tok, ';');
            if (next) *next++ = 0;
            if (!ed_model_run(ed, tok)) A3_WARN("editor", "--mesh-ops: '%s' failed", tok);
        }
    }
    if (ed->frame == ed->play_at_frame && ed->has_project) ed_play(ed);
    editor_frame(ed, dt);
    ed->frame++;
    if (run->frames >= 0 && ed->frame >= run->frames) {
        if (run->shot) a3_engine_screenshot(eng, run->shot);
        a3_engine_end_frame(eng);
        run->running = 0;
        return 0;
    }
    a3_engine_end_frame(eng);
    if (ed->quit == 2) { run->running = 0; return 0; }
    return 1;
}

static void editor_shutdown(EdRun *run) {
    A3Editor *ed = run->ed;
    if (!ed) return;
    ed_project_close(ed);
    ed_viewport_shutdown(ed);
    ed_shader_shutdown(ed);
    ed_anim_shutdown(ed);
    ed_model_shutdown(ed);
    ed_code_free(ed);
    ed_undo_free(&ed->undo);
    a3_ui_destroy(ed->ui);
    a3_engine_destroy(run->eng);
    run->ed = 0;
}

#if A3_PLATFORM_WEB
/* Browser entry points (web/asm3d.js). `args` is a space-separated command line. */
A3_WASM_EXPORT("a3_web_editor_start") int a3_web_editor_start(char *args) {
    static char *argv[32];
    int argc = 0;
    argv[argc++] = (char *)"asm3d_editor";
    for (char *p = args; p && *p && argc < 31;) {   /* "double quotes" keep spaces */
        while (*p == ' ') *p++ = 0;
        if (!*p) break;
        if (*p == '"') {
            argv[argc++] = ++p;
            while (*p && *p != '"') ++p;
            if (*p) *p++ = 0;
            continue;
        }
        argv[argc++] = p;
        while (*p && *p != ' ') ++p;
    }
    a3_zero_struct(&g_run);
    int rc = editor_start(&g_run, argc, argv);
    if (rc >= 0) { editor_shutdown(&g_run); return rc; }
    return -1;
}
A3_WASM_EXPORT("a3_web_editor_frame") int a3_web_editor_frame(void) {
    if (editor_tick(&g_run)) return 1;
    editor_shutdown(&g_run);
    return 0;
}
#else
int main(int argc, char **argv) {
    int rc = editor_start(&g_run, argc, argv);
    if (rc < 0) {
        while (editor_tick(&g_run)) {}
        rc = 0;
    }
    editor_shutdown(&g_run);
    return rc;
}
#endif
