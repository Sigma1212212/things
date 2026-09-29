/*
 * ASM3D Editor - command palette and global search (Ctrl+P).
 *
 * One fuzzy search box over editor commands, scene objects, project assets,
 * components (adds to the selection) and documentation topics.
 */
#include "editor.h"
#include "../../engine/core/a3_log.h"
#include "../../engine/core/a3_string.h"
#include "../../engine/core/a3_format.h"
#include "../../engine/platform/a3_platform.h"

typedef void (*EdCommandFn)(A3Editor *ed);
typedef struct EdCommand { const char *name, *shortcut, *category; EdCommandFn fn; } EdCommand;

static void c_save(A3Editor *ed) { ed_scene_save(ed); }
static void c_undo(A3Editor *ed) { ed_undo(ed); }
static void c_redo(A3Editor *ed) { ed_redo(ed); }
static void c_play(A3Editor *ed) { if (ed->mode == ED_EDIT) ed_play(ed); else ed_stop(ed); }
static void c_pause(A3Editor *ed) { ed_pause(ed); }
static void c_dup(A3Editor *ed) { ed_duplicate_selected(ed); }
static void c_del(A3Editor *ed) { ed_delete_selected(ed); }
static void c_focus(A3Editor *ed) { ed_focus_selected(ed); }
static void c_cube(A3Editor *ed) { ed_create_entity(ed, "Cube", A3_PRIM_CUBE, 0); }
static void c_sphere(A3Editor *ed) { ed_create_entity(ed, "Sphere", A3_PRIM_SPHERE, 0); }
static void c_plane(A3Editor *ed) { ed_create_entity(ed, "Plane", A3_PRIM_PLANE, 0); }
static void c_empty(A3Editor *ed) { ed_create_entity(ed, "Empty", A3_PRIM_NONE, 0); }
static void c_light(A3Editor *ed) {
    A3Entity e = ed_create_entity(ed, "Point Light", A3_PRIM_NONE, "Light");
    A3CLight *l = ed->world ? (A3CLight *)a3_component_get(ed->world, e, A3_T_LIGHT) : 0;
    if (l) { l->type = A3_LIGHT_POINT; A3CTransform *t = a3_transform(ed->world, e); if (t) t->position.y += 2.5f; }
}
static void c_camera(A3Editor *ed) { ed_create_entity(ed, "Camera", A3_PRIM_NONE, "Camera"); }
static void c_player(A3Editor *ed) { ed_create_entity(ed, "Player", A3_PRIM_NONE, "CharacterController"); }
static void c_grid(A3Editor *ed) { ed->show_grid = !ed->show_grid; }
static void c_colliders(A3Editor *ed) { ed->show_colliders = !ed->show_colliders; }
static void c_wire(A3Editor *ed) { ed->show_wireframe = !ed->show_wireframe; }
static void c_snap(A3Editor *ed) { ed->snap = !ed->snap; }
static void c_theme(A3Editor *ed) { ed->dark_theme = !ed->dark_theme; ed_apply_theme(ed); }
static void c_beginner(A3Editor *ed) { ed->level = ED_LEVEL_BEGINNER; }
static void c_advanced(A3Editor *ed) { ed->level = ED_LEVEL_ADVANCED; }
static void c_build(A3Editor *ed) { a3_dock_show(&ed->dock, "Build"); ed_build(ed, 0); }
static void c_build_run(A3Editor *ed) { a3_dock_show(&ed->dock, "Build"); ed_build(ed, 1); }
static void c_check(A3Editor *ed) { a3_dock_show(&ed->dock, "Build"); ed_validate_project(ed, 0, 0, 0); }
static void c_new_shader(A3Editor *ed) { ed_shader_new(ed); a3_dock_show(&ed->dock, "Shader Maker"); }
static void c_move(A3Editor *ed) { ed->gizmo = ED_GIZMO_MOVE; }
static void c_rotate(A3Editor *ed) { ed->gizmo = ED_GIZMO_ROTATE; }
static void c_scale(A3Editor *ed) { ed->gizmo = ED_GIZMO_SCALE; }
static void c_new_scene(A3Editor *ed) { ed_scene_new(ed); }
static void c_close(A3Editor *ed) { ed->want_start_screen = 1; }
static void c_shortcuts(A3Editor *ed) { ed_open_modal(ed, "shortcuts"); }
static void c_clear_log(A3Editor *ed) { A3_UNUSED(ed); a3_log_clear(); }
static void c_layout_default(A3Editor *ed) { ed_layout_preset(ed, "Default"); }
static void c_layout_beginner(A3Editor *ed) { ed_layout_preset(ed, "Beginner"); }
static void c_layout_code(A3Editor *ed) { ed_layout_preset(ed, "Programming"); }
static void c_layout_level(A3Editor *ed) { ed_layout_preset(ed, "Level Design"); }
static void c_layout_shader(A3Editor *ed) { ed_layout_preset(ed, "Shader Design"); }

static const EdCommand g_commands[] = {
    { "Save Scene", "Ctrl+S", "File", c_save },
    { "New Scene", 0, "File", c_new_scene },
    { "Close Project", 0, "File", c_close },
    { "Undo", "Ctrl+Z", "Edit", c_undo },
    { "Redo", "Ctrl+Y", "Edit", c_redo },
    { "Duplicate Selection", "Ctrl+D", "Edit", c_dup },
    { "Delete Selection", "Del", "Edit", c_del },
    { "Focus Selection", "F", "Edit", c_focus },
    { "Play / Stop", "F5", "Play", c_play },
    { "Pause / Resume", "F6", "Play", c_pause },
    { "Create Cube", 0, "Create", c_cube },
    { "Create Sphere", 0, "Create", c_sphere },
    { "Create Plane", 0, "Create", c_plane },
    { "Create Empty Object", 0, "Create", c_empty },
    { "Create Point Light", 0, "Create", c_light },
    { "Create Camera", 0, "Create", c_camera },
    { "Create Player", 0, "Create", c_player },
    { "New Shader", 0, "Create", c_new_shader },
    { "Move Tool", "W", "Tools", c_move },
    { "Rotate Tool", "E", "Tools", c_rotate },
    { "Scale Tool", "R", "Tools", c_scale },
    { "Toggle Grid", 0, "View", c_grid },
    { "Toggle Colliders", 0, "View", c_colliders },
    { "Toggle Wireframe", 0, "View", c_wire },
    { "Toggle Snap", 0, "View", c_snap },
    { "Toggle Dark Theme", 0, "View", c_theme },
    { "Beginner Mode", 0, "View", c_beginner },
    { "Advanced Mode", 0, "View", c_advanced },
    { "Layout: Default", 0, "Window", c_layout_default },
    { "Layout: Beginner", 0, "Window", c_layout_beginner },
    { "Layout: Programming", 0, "Window", c_layout_code },
    { "Layout: Level Design", 0, "Window", c_layout_level },
    { "Layout: Shader Design", 0, "Window", c_layout_shader },
    { "Check Project", 0, "Build", c_check },
    { "Build Game", "Ctrl+B", "Build", c_build },
    { "Build and Run", "Ctrl+Shift+B", "Build", c_build_run },
    { "Keyboard Shortcuts", 0, "Help", c_shortcuts },
    { "Clear Console", 0, "Help", c_clear_log },
};

void ed_run_command(A3Editor *ed, const char *name) {
    for (u32 i = 0; i < A3_ARRAY_COUNT(g_commands); ++i)
        if (a3_streq(g_commands[i].name, name)) { g_commands[i].fn(ed); return; }
    for (u32 i = 0; i < ed->dock.panel_count; ++i)
        if (a3_streq(ed->dock.panels[i].name, name)) { a3_dock_show(&ed->dock, name); return; }
    A3_WARN("editor", "unknown command '%s'", name);
}

void ed_palette_open(A3Editor *ed) {
    ed->palette_query[0] = 0;
    ed->palette_selected = 0;
    ed->palette_request = 1;
}

/* ---- results ---- */

typedef enum ResKind { RK_COMMAND = 0, RK_PANEL, RK_ENTITY, RK_ASSET, RK_COMPONENT } ResKind;
typedef struct Result { ResKind kind; int score; u32 index; u64 guid; char label[128]; const char *detail; } Result;

static struct { char names[256][ED_PATH]; u32 count; f64 at; char project[ED_PATH]; } g_files;

static void scan_dir(A3Editor *ed, const char *rel, int depth);
static b32 scan_visit(const char *dir, const A3DirEntry *e, void *user) {
    A3Editor *ed = (A3Editor *)user;
    A3_UNUSED(dir);
    if (e->name[0] == '.' || g_files.count >= A3_ARRAY_COUNT(g_files.names)) return g_files.count < A3_ARRAY_COUNT(g_files.names);
    char rel[ED_PATH];
    a3_snprintf(rel, sizeof(rel), "%s/%s", g_files.names[A3_ARRAY_COUNT(g_files.names) - 1], e->name);
    if (e->is_dir) {
        char saved[ED_PATH];
        a3_strcpy(saved, sizeof(saved), g_files.names[A3_ARRAY_COUNT(g_files.names) - 1]);
        a3_strcpy(g_files.names[A3_ARRAY_COUNT(g_files.names) - 1], ED_PATH, rel);
        scan_dir(ed, rel, 1);
        a3_strcpy(g_files.names[A3_ARRAY_COUNT(g_files.names) - 1], ED_PATH, saved);
    } else if (g_files.count < A3_ARRAY_COUNT(g_files.names) - 1) {
        a3_strcpy(g_files.names[g_files.count++], ED_PATH, rel);
    }
    return 1;
}

static void scan_dir(A3Editor *ed, const char *rel, int depth) {
    A3_UNUSED(depth);
    char abs[ED_PATH];
    ed_project_path(ed, rel, abs, sizeof(abs));
    a3_dir_list(abs, scan_visit, ed);
}

static void refresh_files(A3Editor *ed) {
    f64 now = a3_time_seconds();
    if (a3_streq(g_files.project, ed->project_dir) && now - g_files.at < 3.0) return;
    g_files.count = 0;
    g_files.at = now;
    a3_strcpy(g_files.project, sizeof(g_files.project), ed->project_dir);
    /* the last slot holds the current directory prefix during the walk */
    a3_strcpy(g_files.names[A3_ARRAY_COUNT(g_files.names) - 1], ED_PATH, "Assets");
    scan_dir(ed, "Assets", 0);
}

static void add_result(Result *res, u32 *n, u32 max, Result r) {
    if (r.score <= 0) return;
    if (*n < max) { res[(*n)++] = r; return; }
    u32 worst = 0;
    for (u32 i = 1; i < *n; ++i) if (res[i].score < res[worst].score) worst = i;
    if (res[worst].score < r.score) res[worst] = r;
}

static int score(const char *text, const char *q) {
    if (!q[0]) return 1;
    int s = a3_fuzzy_score(text, q);
    if (s > 0 && a3_stristr(text, q)) s += 50;
    return s;
}

void ed_palette_draw(A3Editor *ed, A3Ui *ui) {
    if (ed->palette_request) {
        A3Rect area = ed->dock.nodes[a3_dock_root(&ed->dock)].rect;
        a3_ui_open_popup_at(ui, "palette", a3_v2(area.x + area.w * 0.5f - 280, area.y + 20));
        ed->palette_request = 0;
    }
    if (!a3_ui_popup_open(ui, "palette")) return;
    /* centered at the top of the window like a search bar */
    const A3InputState *in = a3_ui_input(ui);
    if (!a3_ui_begin_popup(ui, "palette", 560)) return;
    A3UiTheme *th = a3_ui_theme(ui);
    a3_ui_input_text(ui, "palette_q", ed->palette_query, sizeof(ed->palette_query), A3_INPUT_SEARCH | A3_INPUT_FOCUS, "Type a command, object, asset or component...");
    Result res[40];
    u32 n = 0;
    const char *q = ed->palette_query;
    for (u32 i = 0; i < A3_ARRAY_COUNT(g_commands); ++i) {
        Result r = { RK_COMMAND, score(g_commands[i].name, q) + 20, i, 0, { 0 }, g_commands[i].shortcut };
        a3_snprintf(r.label, sizeof(r.label), "%s", g_commands[i].name);
        add_result(res, &n, A3_ARRAY_COUNT(res), r);
    }
    for (u32 i = 0; i < ed->dock.panel_count; ++i) {
        Result r = { RK_PANEL, score(ed->dock.panels[i].name, q) + 10, i, 0, { 0 }, "Panel" };
        a3_snprintf(r.label, sizeof(r.label), "Open %s", ed->dock.panels[i].name);
        add_result(res, &n, A3_ARRAY_COUNT(res), r);
    }
    if (q[0]) {
        A3World *w = ed_active_world(ed);
        for (u32 i = 0; w && i < w->high_water; ++i) {
            A3EntityRecord *rec = &w->entities[i];
            if (!rec->alive || (rec->flags & A3_ENTITY_EDITOR_ONLY)) continue;
            Result r = { RK_ENTITY, score(rec->name, q), i, rec->guid, { 0 }, "Object" };
            a3_snprintf(r.label, sizeof(r.label), "%s", rec->name);
            add_result(res, &n, A3_ARRAY_COUNT(res), r);
        }
        refresh_files(ed);
        for (u32 i = 0; i < g_files.count; ++i) {
            Result r = { RK_ASSET, score(a3_path_filename(g_files.names[i]), q), i, 0, { 0 }, "Asset" };
            a3_snprintf(r.label, sizeof(r.label), "%s", g_files.names[i]);
            add_result(res, &n, A3_ARRAY_COUNT(res), r);
        }
        if (a3_entity_valid(ed_active_world(ed), ed_selected(ed)) && ed->mode == ED_EDIT)
            for (u32 id = 0; id < A3_MAX_COMPONENT_TYPES; ++id) {
                A3ComponentType *t = a3_component_type(id);
                if (!t || !t->name[0] || (t->flags & A3_COMP_HIDDEN) || a3_component_has(ed->world, ed_selected(ed), id)) continue;
                Result r = { RK_COMPONENT, score(t->name, q), id, 0, { 0 }, "Add to selection" };
                a3_snprintf(r.label, sizeof(r.label), "Add Component: %s", t->name);
                add_result(res, &n, A3_ARRAY_COUNT(res), r);
            }
    }
    /* sort by score */
    for (u32 i = 1; i < n; ++i) { Result t = res[i]; u32 j = i; while (j > 0 && res[j - 1].score < t.score) { res[j] = res[j - 1]; --j; } res[j] = t; }
    u32 shown = a3_minu(n, 14);
    if (in->keys_repeat[A3_KEY_DOWN]) ed->palette_selected = (ed->palette_selected + 1) % (i32)a3_maxu(shown, 1);
    if (in->keys_repeat[A3_KEY_UP]) ed->palette_selected = (ed->palette_selected + (i32)shown - 1) % (i32)a3_maxu(shown, 1);
    if (ed->palette_selected >= (i32)shown) ed->palette_selected = 0;
    i32 run = -1;
    for (u32 i = 0; i < shown; ++i) {
        a3_ui_push_id_int(ui, (i64)i);
        static const u32 icons[] = { A3_ICON_BOLT, A3_ICON_MENU, A3_ICON_CUBE, 0x4, A3_ICON_GEAR };
        if (a3_ui_selectable_ex(ui, "res", res[i].label, (i32)i == ed->palette_selected, icons[res[i].kind], th->colors[A3_UIC_TEXT_DIM])) run = (i32)i;
        if (res[i].detail) {
            A3Rect lr = a3_ui_last_rect(ui);
            a3_ui_text_in_rect(ui, A3_FONT_UI, a3_rect(lr.x, lr.y, lr.w - 8, lr.h), A3_ALIGN_RIGHT, th->colors[A3_UIC_TEXT_DISABLED], res[i].detail);
        }
        a3_ui_pop_id(ui);
    }
    if (!shown) a3_ui_label_colored(ui, th->colors[A3_UIC_TEXT_DIM], "No results.");
    if (in->keys_pressed[A3_KEY_ENTER] && shown) run = ed->palette_selected;
    if (in->keys_pressed[A3_KEY_ESCAPE]) a3_ui_close_popup(ui);
    a3_ui_end_popup(ui);
    if (run >= 0) {
        Result r = res[run];
        a3_ui_close_popup(ui);
        switch (r.kind) {
        case RK_COMMAND: g_commands[r.index].fn(ed); break;
        case RK_PANEL: a3_dock_show(&ed->dock, ed->dock.panels[r.index].name); break;
        case RK_ENTITY: ed->selected = r.guid; ed_focus_selected(ed); break;
        case RK_ASSET: {
            const char *path = g_files.names[r.index];
            const char *ext = a3_path_extension(path);
            if (a3_streq(ext, ".a3scene")) { if (ed->mode != ED_EDIT) ed_stop(ed); ed_scene_open(ed, path); }
            else if (a3_streq(ext, ".a3shader")) { if (ed_shader_open(ed, path)) a3_dock_show(&ed->dock, "Shader Maker"); }
            else if (ed_code_open(ed, path) >= 0) a3_dock_show(&ed->dock, "Code");
        } break;
        case RK_COMPONENT: {
            A3Entity e = ed_selected(ed);
            ed_undo_begin_frame(ed);
            if (a3_component_add(ed->world, e, r.index)) { ed_undo_mark_changed(ed, "Add Component"); ed_show_tip_for_component(ed, r.index); }
        } break;
        }
    }
}
