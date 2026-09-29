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
#include "../../engine/runtime/a3_project.h"
#include "../../engine/physics/a3_physics.h"
#include "../../engine/scene/a3_scene_io.h"
#include "../../engine/resource/a3_assets.h"
#include "../../engine/core/a3_log.h"
#include "../../engine/core/a3_string.h"
#include "../../engine/core/a3_format.h"
#include "../../engine/platform/a3_platform.h"

#if A3_PLATFORM_WINDOWS
#  define EXE_SUFFIX ".exe"
#  define TARGET_NAME "Desktop (Windows x64)"
#else
#  define EXE_SUFFIX ""
#  define TARGET_NAME "Desktop (Linux x86-64)"
#endif

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
/* Validation and build (shared with asm3d_cli: engine/runtime/a3_project.c) */
/* ======================================================================== */

b32 ed_validate_project(A3Editor *ed, A3StrBuf *report, u32 *errors, u32 *warnings) {
    A3StrBuf local;
    A3StrBuf *sb = report;
    if (!sb) { a3_strbuf_init(&local, A3_MEM_EDITOR); sb = &local; }
    u32 ne = 0, nw = 0;
    if (!ed->has_project) { a3_strbuf_append(sb, "ERROR: No project is open.\n"); ne = 1; }
    else {
        A3ProjectReport rep;
        a3_project_validate(ed->project_dir, ed->scene_path, &rep);
        if (ed->dirty) a3_strbuf_append(sb, "Warning: The open scene has unsaved changes; the build uses the saved file (building saves it first).\n");
        if (a3_assets_failed_count()) a3_strbuf_appendf(sb, "Warning: %u asset(s) failed to load in the editor; see the Console for details.\n", a3_assets_failed_count());
        a3_project_report_text(&rep, sb);
        ne = rep.errors;
        nw = rep.warnings + (ed->dirty ? 1 : 0) + (a3_assets_failed_count() ? 1 : 0);
        a3_project_report_free(&rep);
    }
    a3_strcpy(ed->build_report, sizeof(ed->build_report), a3_strbuf_cstr(sb));
    if (errors) *errors = ne;
    if (warnings) *warnings = nw;
    if (sb == &local) a3_strbuf_free(&local);
    return 1;
}

static void build_log_line(void *user, const char *line) {
    A3Editor *ed = (A3Editor *)user;
    u32 n = (u32)a3_strlen(line);
    if (ed->build_log_len + n + 2 >= sizeof(ed->build_log)) return;
    a3_memcpy(ed->build_log + ed->build_log_len, line, n);
    ed->build_log_len += n;
    ed->build_log[ed->build_log_len++] = '\n';
    ed->build_log[ed->build_log_len] = 0;
}

b32 ed_build(A3Editor *ed, b32 run_after) {
    ed->build_log_len = 0;
    ed->build_log[0] = 0;
    ed->build_ok = 0;
    if (!ed->has_project) return 0;
    if (ed->mode != ED_EDIT) ed_stop(ed);
    if (ed->dirty && !ed_scene_save(ed)) { blog(ed, "ERROR: the scene could not be saved"); return 0; }
#if A3_PLATFORM_WEB
    A3_UNUSED(run_after);
    blog(ed, "ERROR: games cannot be built inside the browser (no compiler or executables here). "
             "Use Download .zip above the editor, open the project in the desktop editor and build it there.");
    return 0;
#endif
    if (ed->build_target != 0) {
        blog(ed, "ERROR: only the Desktop target is available in this version (see docs/STATUS.md).");
        return 0;
    }
    ed_validate_project(ed, 0, 0, 0);
    char out[ED_PATH];
    if (!a3_project_build(ed->project_dir, 0, 0, ed->build_config ? "Release" : "Debug", build_log_line, ed, out, sizeof(out))) {
        a3_ui_notify(ed->ui, colr(ed->ui, A3_UIC_ERROR), "Build failed - see the Build panel");
        return 0;
    }
    a3_strcpy(ed->build_output, sizeof(ed->build_output), out);
    ed->build_ok = 1;
    a3_ui_notify(ed->ui, colr(ed->ui, A3_UIC_SUCCESS), "Build succeeded");
    if (run_after) {
        char exe[ED_PATH], name[80];
        A3ProjectInfo info;
        a3_project_read(ed->project_dir, &info);
        a3_strcpy(name, sizeof(name), info.name);
        for (char *p = name; *p; ++p) if (!a3_is_alnum(*p) && *p != '-' && *p != '_') *p = '_';
        a3_strcat(name, sizeof(name), EXE_SUFFIX);
        a3_path_join(exe, sizeof(exe), out, name);
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
    static const char *const targets[] = { TARGET_NAME };
    static const char *const configs[] = { "Debug", "Release" };
    a3_ui_heading(ui, "Build Game");
    a3_ui_property(ui, "Target", "Builds for the system the editor runs on (Windows or Linux)");
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
