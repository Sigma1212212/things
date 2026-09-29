/*
 * ASM3D Editor - project checks and one-click desktop builds.
 *
 * A desktop build is a self-contained folder:
 *   Builds/Desktop/<Game>/
 *     <Game>            the ASM3D player (finds ./data automatically)
 *     data/             project.a3proj + Assets/
 *     README.txt
 * Zip the folder to share the game.
 */
#include "editor.h"
#include "../../engine/physics/a3_physics.h"
#include "../../engine/scene/a3_scene_io.h"
#include "../../engine/resource/a3_assets.h"
#include "../../engine/core/a3_log.h"
#include "../../engine/core/a3_string.h"
#include "../../engine/core/a3_format.h"
#include "../../engine/platform/a3_platform.h"

static u32 colr(A3Ui *ui, A3UiColor c) { return a3_ui_theme(ui)->colors[c]; }

static void blog(A3Editor *ed, const char *fmt, ...) A3_PRINTF_LIKE(2, 3);
static void blog(A3Editor *ed, const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    a3_vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    usize n = a3_strlen(line);
    if (ed->build_log_len + n + 2 >= sizeof(ed->build_log)) return;
    a3_memcpy(ed->build_log + ed->build_log_len, line, n);
    ed->build_log_len += (u32)n;
    ed->build_log[ed->build_log_len++] = '\n';
    ed->build_log[ed->build_log_len] = 0;
    A3_INFO("build", "%s", line);
}

/* ======================================================================== */
/* Validation                                                               */
/* ======================================================================== */

typedef struct Check { A3StrBuf *sb; u32 errors, warnings; } Check;

static void issue(Check *c, b32 error, const char *fmt, ...) A3_PRINTF_LIKE(3, 4);
static void issue(Check *c, b32 error, const char *fmt, ...) {
    char msg[400];
    va_list ap;
    va_start(ap, fmt);
    a3_vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (error) c->errors++; else c->warnings++;
    a3_strbuf_appendf(c->sb, "%s %s\n", error ? "ERROR:" : "Warning:", msg);
}

static b32 asset_exists(A3Editor *ed, const char *rel) {
    char abs[ED_PATH];
    ed_project_path(ed, rel, abs, sizeof(abs));
    return a3_file_exists(abs);
}

static void check_world(A3Editor *ed, A3World *w, const char *scene, Check *c) {
    b32 camera = a3_component_count(w, A3_T_CAMERA) > 0 || a3_component_count(w, A3_T_CHARACTER) > 0;
    if (!camera) issue(c, 0, "%s has no Camera or player, so the game starts with a default view. Add a Camera.", scene);
    if (a3_component_count(w, A3_T_LIGHT) == 0) issue(c, 0, "%s has no lights; everything will look flat. Add a Directional Light.", scene);
    u32 primaries = 0;
    for (u32 i = 0; i < w->high_water; ++i) {
        A3EntityRecord *rec = &w->entities[i];
        if (!rec->alive) continue;
        A3Entity e = { i, rec->gen };
        A3CCamera *cam = (A3CCamera *)a3_component_get(w, e, A3_T_CAMERA);
        if (cam && cam->primary) primaries++;
        A3CMeshRenderer *mr = (A3CMeshRenderer *)a3_component_get(w, e, A3_T_MESH_RENDERER);
        if (mr) {
            if (mr->mesh.path[0] && !asset_exists(ed, mr->mesh.path)) issue(c, 1, "'%s' uses a missing model: %s", rec->name, mr->mesh.path);
            if (mr->texture.path[0] && !asset_exists(ed, mr->texture.path)) issue(c, 1, "'%s' uses a missing texture: %s", rec->name, mr->texture.path);
            if (mr->material.path[0] && !asset_exists(ed, mr->material.path)) issue(c, 1, "'%s' uses a missing material: %s", rec->name, mr->material.path);
            if (mr->primitive == A3_PRIM_NONE && !mr->mesh.path[0]) issue(c, 0, "'%s' has a Mesh Renderer with no shape or model.", rec->name);
        }
        if (a3_component_has(w, e, A3_T_RIGIDBODY) && !a3_component_has(w, e, A3_T_COLLIDER))
            issue(c, 0, "'%s' has a RigidBody but no Collider; it will fall through the floor.", rec->name);
        A3CTransform *t = a3_transform(w, e);
        if (t && (t->scale.x == 0 || t->scale.y == 0 || t->scale.z == 0)) issue(c, 0, "'%s' has a scale of zero and is invisible.", rec->name);
        if (rec->unknown_components) issue(c, 0, "'%s' has components from a plugin that is not loaded.", rec->name);
    }
    if (primaries > 1) issue(c, 0, "%s has %u cameras marked Primary; only the first is used.", scene, primaries);
}

typedef struct ScanCtx { A3Editor *ed; Check *c; u32 scripts, big, files; char rel[ED_PATH]; } ScanCtx;

static b32 scan_assets(const char *dir, const A3DirEntry *e, void *user) {
    ScanCtx *s = (ScanCtx *)user;
    if (e->name[0] == '.') return 1;
    if (e->is_dir) {
        char sub[ED_PATH], saved[ED_PATH];
        a3_path_join(sub, sizeof(sub), dir, e->name);
        a3_strcpy(saved, sizeof(saved), s->rel);
        a3_snprintf(s->rel, sizeof(s->rel), "%s/%s", saved, e->name);
        a3_dir_list(sub, scan_assets, s);
        a3_strcpy(s->rel, sizeof(s->rel), saved);
        return 1;
    }
    s->files++;
    if (a3_str_ends_with(e->name, ".a3script")) s->scripts++;
    if (e->size > 32ull * 1024 * 1024) { s->big++; issue(s->c, 0, "%s/%s is %.0f MB; large files make downloads slow.", s->rel, e->name, (f64)e->size / (1024.0 * 1024.0)); }
    return 1;
}

b32 ed_validate_project(A3Editor *ed, A3StrBuf *report, u32 *errors, u32 *warnings) {
    A3StrBuf local;
    A3StrBuf *sb = report;
    if (!sb) { a3_strbuf_init(&local, A3_MEM_EDITOR); sb = &local; }
    Check c = { sb, 0, 0 };
    if (!ed->has_project) { issue(&c, 1, "No project is open."); }
    else {
        /* startup scene on disk */
        char path[ED_PATH];
        ed_project_path(ed, ed->scene_path, path, sizeof(path));
        A3World *w = a3_world_create("Check");
        A3SceneLoadReport rep;
        if (!a3_file_exists(path)) issue(&c, 1, "The startup scene %s has not been saved yet. Press Ctrl+S.", ed->scene_path);
        else if (a3_scene_load_file(w, path, &rep) != A3_OK) issue(&c, 1, "The startup scene cannot be loaded: %s", rep.error);
        else check_world(ed, w, ed->scene_path, &c);
        a3_world_destroy(w);
        if (ed->dirty) issue(&c, 0, "The open scene has unsaved changes; the build uses the saved file (building saves it first).");
        /* assets */
        ScanCtx s;
        a3_zero_struct(&s);
        s.ed = ed;
        s.c = &c;
        a3_strcpy(s.rel, sizeof(s.rel), "Assets");
        char adir[ED_PATH];
        ed_project_path(ed, "Assets", adir, sizeof(adir));
        a3_dir_list(adir, scan_assets, &s);
        if (s.scripts) issue(&c, 0, "%u script file(s) found. The scripting runtime is still in development, so scripts are packaged but not run yet.", s.scripts);
        if (a3_assets_failed_count()) issue(&c, 0, "%u asset(s) failed to load in the editor; see the Console for details.", a3_assets_failed_count());
        a3_strbuf_appendf(sb, "Checked %u asset files.\n", s.files);
    }
    a3_strbuf_appendf(sb, "%u error(s), %u warning(s).\n", c.errors, c.warnings);
    a3_strcpy(ed->build_report, sizeof(ed->build_report), a3_strbuf_cstr(sb));
    if (errors) *errors = c.errors;
    if (warnings) *warnings = c.warnings;
    if (sb == &local) a3_strbuf_free(&local);
    return 1;
}

/* ======================================================================== */
/* Build                                                                    */
/* ======================================================================== */

typedef struct CopyCtx { A3Editor *ed; char src[ED_PATH]; char dst[ED_PATH]; u32 files; u64 bytes; b32 failed; } CopyCtx;

static b32 copy_visit(const char *dir, const A3DirEntry *e, void *user) {
    CopyCtx *c = (CopyCtx *)user;
    A3_UNUSED(dir);
    if (e->name[0] == '.') return 1;   /* skip hidden files (.asm3d, .git) */
    char s[ED_PATH], d[ED_PATH];
    a3_path_join(s, sizeof(s), c->src, e->name);
    a3_path_join(d, sizeof(d), c->dst, e->name);
    if (e->is_dir) {
        a3_dir_create(d);
        char ss[ED_PATH], sd[ED_PATH];
        a3_strcpy(ss, sizeof(ss), c->src);
        a3_strcpy(sd, sizeof(sd), c->dst);
        a3_strcpy(c->src, sizeof(c->src), s);
        a3_strcpy(c->dst, sizeof(c->dst), d);
        a3_dir_list(s, copy_visit, c);
        a3_strcpy(c->src, sizeof(c->src), ss);
        a3_strcpy(c->dst, sizeof(c->dst), sd);
    } else {
        if (a3_file_copy(s, d) != A3_OK) { blog(c->ed, "ERROR: could not copy %s", s); c->failed = 1; return 0; }
        c->files++;
        c->bytes += e->size;
    }
    return 1;
}

static b32 find_player(char *out, usize cap) {
    char dir[ED_PATH];
    if (a3_get_exe_dir(dir, sizeof(dir))) {
        a3_path_join(out, cap, dir, "asm3d_player");
        if (a3_file_exists(out)) return 1;
    }
    return a3_find_executable("asm3d_player", out, cap);
}

b32 ed_build(A3Editor *ed, b32 run_after) {
    ed->build_log_len = 0;
    ed->build_log[0] = 0;
    ed->build_ok = 0;
    if (!ed->has_project) return 0;
    u64 t0 = a3_time_ns();
    if (ed->mode != ED_EDIT) ed_stop(ed);
    if (ed->dirty && !ed_scene_save(ed)) { blog(ed, "ERROR: the scene could not be saved"); return 0; }
    A3StrBuf rep;
    a3_strbuf_init(&rep, A3_MEM_EDITOR);
    u32 errors = 0, warnings = 0;
    ed_validate_project(ed, &rep, &errors, &warnings);
    a3_strbuf_free(&rep);
    blog(ed, "Checked project: %u error(s), %u warning(s)", errors, warnings);
    if (errors) {
        blog(ed, "Build stopped: fix the errors listed in the report first.");
        a3_ui_notify(ed->ui, colr(ed->ui, A3_UIC_ERROR), "Build failed - see the Build panel");
        return 0;
    }
    if (ed->build_target != 0) {
        blog(ed, "ERROR: only the Desktop target is available in this version (see docs/STATUS.md).");
        return 0;
    }
    char player[ED_PATH];
    if (!find_player(player, sizeof(player))) {
        blog(ed, "ERROR: asm3d_player was not found next to the editor. Rebuild ASM3D with CMake (target asm3d_player).");
        return 0;
    }
    /* output folder */
    char name[64];
    a3_strcpy(name, sizeof(name), ed->project_name);
    for (char *p = name; *p; ++p) if (!a3_is_alnum(*p) && *p != '-' && *p != '_') *p = '_';
    char rel_out[ED_PATH], out[ED_PATH], data[ED_PATH];
    a3_snprintf(rel_out, sizeof(rel_out), "Builds/Desktop-%s/%s", ed->build_config ? "Release" : "Debug", name);
    ed_project_path(ed, rel_out, out, sizeof(out));
    if (a3_dir_exists(out)) a3_dir_delete_recursive(out);
    a3_path_join(data, sizeof(data), out, "data");
    if (a3_dir_create(data) != A3_OK) { blog(ed, "ERROR: cannot create %s", data); return 0; }
    blog(ed, "Output: %s", out);
    /* program */
    char exe[ED_PATH];
    a3_path_join(exe, sizeof(exe), out, name);
    if (a3_file_copy(player, exe) != A3_OK) { blog(ed, "ERROR: could not copy the player to %s", exe); return 0; }
    blog(ed, "Copied player program (%s)", a3_path_filename(player));
    /* data */
    char src[ED_PATH], dst[ED_PATH];
    ed_project_path(ed, "project.a3proj", src, sizeof(src));
    a3_path_join(dst, sizeof(dst), data, "project.a3proj");
    if (a3_file_copy(src, dst) != A3_OK) { blog(ed, "ERROR: could not copy project.a3proj"); return 0; }
    CopyCtx cc;
    a3_zero_struct(&cc);
    cc.ed = ed;
    ed_project_path(ed, "Assets", cc.src, sizeof(cc.src));
    a3_path_join(cc.dst, sizeof(cc.dst), data, "Assets");
    a3_dir_create(cc.dst);
    a3_dir_list(cc.src, copy_visit, &cc);
    if (cc.failed) return 0;
    blog(ed, "Packaged %u asset files (%.2f MB)", cc.files, (f64)cc.bytes / (1024.0 * 1024.0));
    char readme[1024], rp[ED_PATH];
    a3_snprintf(readme, sizeof(readme),
        "%s\n\nMade with ASM3D %s.\n\nRun ./%s to play.\nControls: WASD to move, mouse to look (click to capture, Escape to release), Space to jump.\n",
        ed->project_name, A3_VERSION_STRING, name);
    a3_path_join(rp, sizeof(rp), out, "README.txt");
    a3_file_write_atomic(rp, readme, a3_strlen(readme));
    a3_strcpy(ed->build_output, sizeof(ed->build_output), out);
    f64 secs = (f64)(a3_time_ns() - t0) * 1e-9;
    blog(ed, "Build succeeded in %.2f s", secs);
    ed->build_ok = 1;
    a3_ui_notify(ed->ui, colr(ed->ui, A3_UIC_SUCCESS), "Build succeeded (%.1f s)", secs);
    if (run_after) {
        const char *argv[] = { exe, 0 };
        if (a3_process_spawn_detached(argv)) blog(ed, "Started %s", exe);
        else blog(ed, "ERROR: could not start %s", exe);
    }
    return 1;
}

/* ======================================================================== */
/* Panel                                                                    */
/* ======================================================================== */

void ed_build_panel(void *user, A3Ui *ui, A3Rect r) {
    A3Editor *ed = (A3Editor *)user;
    A3_UNUSED(r);
    static const char *const targets[] = { "Desktop (Linux x86-64)" };
    static const char *const configs[] = { "Debug", "Release" };
    a3_ui_heading(ui, "Build Game");
    a3_ui_property(ui, "Target", "Windows and macOS players are planned (docs/STATUS.md)");
    a3_ui_combo(ui, "target", &ed->build_target, targets, A3_ARRAY_COUNT(targets));
    a3_ui_property(ui, "Configuration", "Folder name only for now: both use the player program this editor was built with");
    a3_ui_combo(ui, "config", &ed->build_config, configs, 2);
    a3_ui_property(ui, "Startup Scene", 0);
    a3_ui_label(ui, "%s", ed->scene_path);
    a3_ui_spacing(ui, 6);
    b32 editing = ed->mode == ED_EDIT;
    if (a3_ui_button_ex(ui, "Check Project", 130, 0)) ed_validate_project(ed, 0, 0, 0);
    a3_ui_same_line(ui);
    if (a3_ui_button_ex(ui, "Build Game", 120, A3_BUTTON_PRIMARY | (editing ? 0 : A3_BUTTON_DISABLED)) && editing) ed_build(ed, 0);
    a3_ui_same_line(ui);
    if (a3_ui_button_ex(ui, "Build and Run", 130, editing ? 0 : A3_BUTTON_DISABLED) && editing) ed_build(ed, 1);
    if (ed->build_output[0]) {
        a3_ui_label_colored(ui, ed->build_ok ? colr(ui, A3_UIC_SUCCESS) : colr(ui, A3_UIC_TEXT_DIM), "Last output: %s", ed->build_output);
    }
    if (ed->build_report[0]) {
        a3_ui_spacing(ui, 6);
        a3_ui_heading(ui, "Project Check");
        const char *p = ed->build_report;
        while (*p) {
            const char *e = a3_strchr(p, '\n');
            i32 n = e ? (i32)(e - p) : (i32)a3_strlen(p);
            char line[420];
            a3_str_to_buf(a3_str_n(p, (usize)n), line, sizeof(line));
            u32 c = a3_str_starts_with(line, "ERROR") ? colr(ui, A3_UIC_ERROR) : a3_str_starts_with(line, "Warning") ? colr(ui, A3_UIC_WARNING) : colr(ui, A3_UIC_TEXT_DIM);
            f32 wdt = a3_ui_content_width(ui);
            A3Rect tr = a3_ui_next_rect(ui, wdt, a3_ui_text_wrapped_height(A3_FONT_UI, wdt, line));
            a3_ui_text_wrapped(ui, A3_FONT_UI, tr, c, line);
            p = e ? e + 1 : p + n;
        }
    }
    if (ed->build_log_len) {
        a3_ui_spacing(ui, 6);
        a3_ui_heading(ui, "Build Log");
        f32 wdt = a3_ui_content_width(ui);
        A3Rect tr = a3_ui_next_rect(ui, wdt, a3_ui_text_wrapped_height(A3_FONT_MONO, wdt, ed->build_log) + 8);
        a3_ui_rect(ui, tr, colr(ui, A3_UIC_BG), 6);
        a3_ui_text_wrapped(ui, A3_FONT_MONO, a3_rect_shrink(tr, 4), colr(ui, A3_UIC_TEXT), ed->build_log);
    }
}
