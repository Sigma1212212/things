/*
 * ASM3D Editor - shared state.
 */
#ifndef ASM3D_EDITOR_H
#define ASM3D_EDITOR_H

#include "../../engine/runtime/a3_engine.h"
#include "../../engine/ui/a3_ui.h"
#include "../../engine/ui/a3_dock.h"
#include "../../engine/ecs/a3_ecs.h"
#include "../../engine/scene/a3_components.h"
#include "../../engine/core/a3_strbuf.h"

#define ED_PATH 512

typedef enum EdMode { ED_EDIT = 0, ED_PLAY, ED_PAUSED } EdMode;
typedef enum EdGizmo { ED_GIZMO_MOVE = 0, ED_GIZMO_ROTATE, ED_GIZMO_SCALE } EdGizmo;
typedef enum EdUserLevel { ED_LEVEL_BEGINNER = 0, ED_LEVEL_ADVANCED, ED_LEVEL_ENGINE } EdUserLevel;

/* ---- undo ---- */
typedef enum EdUndoKind { ED_UNDO_MODIFY = 0, ED_UNDO_CREATE, ED_UNDO_DELETE } EdUndoKind;
typedef struct EdUndoEntry {
    EdUndoKind kind;
    char label[64];
    u64 guid;             /* root entity of the change */
    char *before;         /* scene-format JSON of the subtree (NULL for create) */
    char *after;          /* NULL for delete */
} EdUndoEntry;

typedef struct EdUndo {
    A3_ARRAY_TYPE(EdUndoEntry) entries;
    u32 cursor;           /* entries[0..cursor) are applied */
    /* pending property edit capture */
    u64 pending_guid;
    char *pending_before;
    b32 pending_changed;
    char pending_label[64];
} EdUndo;

/* ---- code editor documents ---- */
#define ED_TEXT_UNDO 48
typedef struct EdDocument {
    char path[ED_PATH];   /* project relative */
    char *text;           /* NUL terminated */
    u32 len, cap;
    i32 cursor, anchor;   /* byte offsets; selection is [min, max) */
    f32 scroll_y, scroll_x;
    b32 dirty;
    i32 language;         /* 0 text, 1 script, 2 glsl, 3 json */
    char *undo_text[ED_TEXT_UNDO];
    i32 undo_cursor[ED_TEXT_UNDO];
    u32 undo_count;
    char *redo_text[ED_TEXT_UNDO];
    i32 redo_cursor[ED_TEXT_UNDO];
    u32 redo_count;
    f64 last_edit_time;
    i32 last_edit_kind;
    u32 *line_starts;
    u32 line_count, line_cap;
    b32 lines_dirty;
    f32 desired_x;        /* remembered column for up/down */
    b32 mouse_selecting;
    i32 error_lines[16];  /* 1-based */
    char error_msgs[16][160];
    u32 error_count;
} EdDocument;

typedef struct EdLogFilter { b32 info, warn, error; char text[64]; } EdLogFilter;

typedef struct A3Editor {
    A3Engine *engine;
    A3Ui *ui;
    A3Dock dock;
    b32 quit;
    /* project */
    b32 has_project;
    char project_dir[ED_PATH];
    char project_name[64];
    char scene_path[ED_PATH];     /* project relative */
    A3World *world;               /* edited world */
    b32 dirty;
    u64 last_autosave_ns;
    char recent[8][ED_PATH];
    u32 recent_count;
    b32 no_recent;                /* self test: leave the user's recent list alone */
    /* play mode */
    EdMode mode;
    A3World *play_world;
    b32 step_requested;
    b32 game_view;                /* viewport shows the game camera while playing */
    /* selection (by GUID so it survives undo/play) */
    u64 selected;
    /* editor camera */
    A3Vec3 cam_pos;
    f32 cam_yaw, cam_pitch, cam_speed;
    b32 cam_flying;
    /* viewport */
    A3RhiTexture vp_color, vp_depth;
    A3RhiTarget vp_target;
    i32 vp_w, vp_h;
    A3Rect vp_rect;
    b32 vp_hovered;
    EdGizmo gizmo;
    b32 gizmo_local;
    i32 gizmo_axis;               /* -1 none; 0..2 axes; 3..5 planes; 6 uniform */
    b32 gizmo_dragging;
    b32 gizmo_hot;                /* gizmo hovered/dragged this frame: blocks picking */
    b32 icon_clicked;             /* a scene icon took the click this frame */
    A3Vec2 vp_drag_start;
    A3Vec3 gizmo_start_pos, gizmo_start_scale, gizmo_grab_offset;
    A3Quat gizmo_start_rot;
    f32 gizmo_start_angle;
    b32 snap;
    b32 show_grid, show_colliders, show_bounds, show_icons, show_wireframe;
    /* modes */
    EdUserLevel level;
    b32 dark_theme;
    /* undo */
    EdUndo undo;
    /* panels state */
    char hierarchy_filter[64];
    char add_component_filter[64];
    char asset_dir[ED_PATH];      /* current folder in the asset browser (project relative) */
    char asset_filter[64];
    EdLogFilter log_filter;
    u32 log_seen;
    b32 console_autoscroll;
    char palette_query[128];
    i32 palette_selected;
    b32 palette_request;          /* opened at top level next draw (popup ids depend on the id stack) */
    char search_query[128];
    /* code editor */
    EdDocument docs[16];
    u32 doc_count;
    i32 doc_active;
    char find_text[128];
    char replace_text[128];
    b32 show_find;
    /* new project dialog */
    char new_name[64];
    char new_location[ED_PATH];
    i32 new_template;
    /* profiler */
    f32 frame_ms[240];
    u32 frame_ms_pos;
    f32 fps;
    /* build */
    i32 build_target;             /* 0 desktop, 1 html */
    i32 build_config;             /* 0 debug, 1 release */
    char build_output[ED_PATH];
    char build_log[16384];
    u32 build_log_len;
    b32 build_ok;
    char build_report[4096];
    /* guidance */
    char tip[256];
    char tip_action[64];
    u32 tip_component;
    b32 show_welcome;
    /* modals are opened at top level (UI ids depend on the id stack) */
    char pending_modal[32];
    u64 pending_create;           /* entity created this frame: CREATE undo step recorded at frame end */
    b32 want_start_screen;
    b32 quit_after_save;
    char open_path[ED_PATH];
    /* game input routing while playing */
    b32 game_focused;
    A3InputState game_input;
    /* shader maker state (editor_shader.c) */
    struct EdShader *shader;
    /* automation (screenshots / smoke tests) */
    i32 frame;
    i32 play_at_frame;
} A3Editor;

/* project (editor_project.c) */
extern const char *const g_ed_templates[];
extern const char *const g_ed_template_desc[];
extern const u32 g_ed_template_count;
b32  ed_project_create(A3Editor *ed, const char *location, const char *name, i32 template_index);
b32  ed_project_open(A3Editor *ed, const char *dir);
void ed_project_close(A3Editor *ed);
b32  ed_scene_save(A3Editor *ed);
b32  ed_scene_open(A3Editor *ed, const char *rel_path);
void ed_scene_new(A3Editor *ed);
void ed_autosave_tick(A3Editor *ed);
void ed_recent_load(A3Editor *ed);
void ed_recent_add(A3Editor *ed, const char *dir);
void ed_project_path(A3Editor *ed, const char *rel, char *out, usize cap);
void ed_build_template_scene(A3World *w, i32 template_index);

/* selection helpers (editor_main.c) */
A3World *ed_active_world(A3Editor *ed);
A3Entity ed_selected(A3Editor *ed);
void ed_select(A3Editor *ed, A3Entity e);
A3Entity ed_create_entity(A3Editor *ed, const char *name, A3Primitive prim, const char *component);
void ed_delete_selected(A3Editor *ed);
void ed_duplicate_selected(A3Editor *ed);
void ed_mark_dirty(A3Editor *ed);
void ed_play(A3Editor *ed);
void ed_stop(A3Editor *ed);
void ed_pause(A3Editor *ed);
void ed_focus_selected(A3Editor *ed);
void ed_show_tip_for_component(A3Editor *ed, u32 type_id);
void ed_apply_theme(A3Editor *ed);
void ed_layout_preset(A3Editor *ed, const char *name);
void ed_open_modal(A3Editor *ed, const char *id);
extern const char *const g_ed_layouts[];
extern const u32 g_ed_layout_count;

/* undo (editor_undo.c) */
void ed_undo_init(EdUndo *u);
void ed_undo_free(EdUndo *u);
void ed_undo_clear(EdUndo *u);
char *ed_snapshot(A3World *w, u64 guid);  /* heap string, caller frees */
void ed_undo_push(A3Editor *ed, EdUndoKind kind, const char *label, u64 guid, char *before, char *after); /* takes ownership */
void ed_undo_begin_frame(A3Editor *ed);   /* property-edit capture */
void ed_undo_mark_changed(A3Editor *ed, const char *label);
void ed_undo_end_frame(A3Editor *ed);
b32  ed_undo(A3Editor *ed);
b32  ed_redo(A3Editor *ed);
b32  ed_restore_snapshot(A3World *w, u64 guid, const char *json);

/* viewport (editor_viewport.c) */
void ed_viewport_panel(void *user, A3Ui *ui, A3Rect r);
void ed_viewport_render(A3Editor *ed);
void ed_viewport_shutdown(A3Editor *ed);
A3Mat4 ed_camera_view(A3Editor *ed);

/* panels (editor_panels.c) */
void ed_hierarchy_panel(void *user, A3Ui *ui, A3Rect r);
void ed_inspector_panel(void *user, A3Ui *ui, A3Rect r);
void ed_console_panel(void *user, A3Ui *ui, A3Rect r);
void ed_assets_panel(void *user, A3Ui *ui, A3Rect r);
void ed_profiler_panel(void *user, A3Ui *ui, A3Rect r);
void ed_docs_panel(void *user, A3Ui *ui, A3Rect r);
void ed_settings_panel(void *user, A3Ui *ui, A3Rect r);
void ed_build_panel(void *user, A3Ui *ui, A3Rect r);
void ed_draw_field(A3Editor *ed, A3Ui *ui, const A3FieldDesc *f, void *component, const char *comp_name);
void ed_custom_components_load(A3Editor *ed);   /* Assets/Components/NAME.a3comp, before scenes load */
void ed_custom_component_save(A3Editor *ed, const char *name, const A3CustomFieldDef *fields, u32 count);

/* animation timeline (editor_anim.c) */
void ed_anim_panel(void *user, A3Ui *ui, A3Rect r);
void ed_anim_flush(A3Editor *ed);      /* save the clip and end any preview (before play/save/close) */
void ed_anim_shutdown(A3Editor *ed);

/* code editor (editor_code.c) */
void ed_code_panel(void *user, A3Ui *ui, A3Rect r);
i32  ed_code_open(A3Editor *ed, const char *path);
b32  ed_code_save(A3Editor *ed, i32 doc);
void ed_code_free(A3Editor *ed);

/* command palette & search (editor_palette.c) */
void ed_palette_open(A3Editor *ed);
void ed_palette_draw(A3Editor *ed, A3Ui *ui);
void ed_run_command(A3Editor *ed, const char *name);

/* build (editor_build.c) */
b32  ed_validate_project(A3Editor *ed, A3StrBuf *report, u32 *errors, u32 *warnings);
b32  ed_build(A3Editor *ed, b32 run_after);

/* shader maker (editor_shader.c) */
void ed_shader_panel(void *user, A3Ui *ui, A3Rect r);
void ed_shader_init(A3Editor *ed);
void ed_shader_shutdown(A3Editor *ed);
void ed_shader_new(A3Editor *ed);
void ed_shader_preset(A3Editor *ed, u32 preset);
b32  ed_shader_open(A3Editor *ed, const char *rel_path);
void ed_shader_render_preview(A3Editor *ed); /* offscreen preview, called after the UI frame */
int  ed_shader_selftest(A3Editor *ed);          /* returns the number of failures */

#endif
