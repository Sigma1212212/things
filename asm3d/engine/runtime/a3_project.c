/*
 * ASM3D - a3_project.c
 */
#include "a3_project.h"
#include "../core/a3_string.h"
#include "../core/a3_format.h"
#include "../core/a3_strbuf.h"
#include "../core/a3_json.h"
#include "../core/a3_log.h"
#include "../platform/a3_platform.h"
#include "../ecs/a3_ecs.h"
#include "../scene/a3_components.h"
#include "../scene/a3_scene_io.h"
#include "../physics/a3_physics.h"
#include "../script/a3_script_engine.h"

#define PP (A3_PATH_MAX * 2)

/* ======================================================================== */
/* project.a3proj                                                           */
/* ======================================================================== */

b32 a3_project_read(const char *dir, A3ProjectInfo *out) {
    a3_zero_struct(out);
    a3_strcpy(out->name, sizeof(out->name), a3_path_filename(dir));
    a3_strcpy(out->startup_scene, sizeof(out->startup_scene), "Assets/Scenes/Main.a3scene");
    out->width = 1280;
    out->height = 720;
    char path[PP];
    a3_path_join(path, sizeof(path), dir, "project.a3proj");
    A3FileData fd;
    if (a3_file_read_all(path, A3_MEM_TEMP, &fd) != A3_OK) return 0;
    A3Arena ar;
    a3_arena_init(&ar, A3_MEM_TEMP, A3_KB(16));
    A3JsonError err;
    A3Json *root = a3_json_parse((const char *)fd.data, fd.size, &ar, &err);
    b32 ok = root != 0;
    if (root) {
        a3_strcpy(out->name, sizeof(out->name), a3_json_get_string(root, "name", out->name));
        a3_strcpy(out->startup_scene, sizeof(out->startup_scene), a3_json_get_string(root, "startupScene", out->startup_scene));
        a3_strcpy(out->template_name, sizeof(out->template_name), a3_json_get_string(root, "template", ""));
        a3_strcpy(out->engine, sizeof(out->engine), a3_json_get_string(root, "engine", ""));
        A3Json *win = a3_json_get(root, "window");
        if (win) {
            out->width = (i32)a3_json_get_number(win, "width", 1280);
            out->height = (i32)a3_json_get_number(win, "height", 720);
        }
    }
    a3_arena_release(&ar);
    a3_free(fd.data);
    return ok;
}

b32 a3_project_write(const char *dir, const A3ProjectInfo *info) {
    A3StrBuf sb;
    a3_strbuf_init(&sb, A3_MEM_TEMP);
    A3JsonWriter jw;
    a3_jw_init(&jw, &sb, 0);
    a3_jw_begin_object(&jw);
    a3_jw_kv_string(&jw, "format", "asm3d.project");
    a3_jw_kv_int(&jw, "version", A3_FORMAT_VERSION);
    a3_jw_kv_string(&jw, "name", info->name);
    a3_jw_kv_string(&jw, "engine", A3_VERSION_STRING);
    a3_jw_kv_string(&jw, "startupScene", info->startup_scene);
    if (info->template_name[0]) a3_jw_kv_string(&jw, "template", info->template_name);
    a3_jw_key(&jw, "window");
    a3_jw_begin_object(&jw);
    a3_jw_kv_string(&jw, "title", info->name);
    a3_jw_kv_int(&jw, "width", info->width > 0 ? info->width : 1280);
    a3_jw_kv_int(&jw, "height", info->height > 0 ? info->height : 720);
    a3_jw_kv_bool(&jw, "vsync", 1);
    a3_jw_end_object(&jw);
    a3_jw_key(&jw, "buildTargets");
    a3_jw_begin_array(&jw);
    a3_jw_string(&jw, "Desktop");
    a3_jw_end_array(&jw);
    a3_jw_end_object(&jw);
    a3_strbuf_append_char(&sb, '\n');
    char path[PP];
    a3_path_join(path, sizeof(path), dir, "project.a3proj");
    b32 ok = a3_file_write_atomic(path, a3_strbuf_cstr(&sb), sb.len) == A3_OK;
    a3_strbuf_free(&sb);
    return ok;
}

/* ======================================================================== */
/* Validation                                                               */
/* ======================================================================== */

static void issue(A3ProjectReport *r, b32 error, const char *file, i32 line, const char *fmt, ...) A3_PRINTF_LIKE(5, 6);
static void issue(A3ProjectReport *r, b32 error, const char *file, i32 line, const char *fmt, ...) {
    A3ProjectIssue is;
    a3_zero_struct(&is);
    is.error = error;
    is.line = line;
    a3_strcpy(is.file, sizeof(is.file), file ? file : "");
    va_list ap;
    va_start(ap, fmt);
    a3_vsnprintf(is.text, sizeof(is.text), fmt, ap);
    va_end(ap);
    if (error) r->errors++; else r->warnings++;
    a3_array_push(r->issues, is, A3_MEM_TEMP);
}

static b32 asset_exists(const char *dir, const char *rel) {
    if (!rel[0] || a3_str_starts_with(rel, "builtin:")) return 1;
    char abs[PP];
    a3_path_join(abs, sizeof(abs), dir, rel);
    return a3_file_exists(abs);
}

static void check_world(const char *dir, A3World *w, const char *scene, b32 startup, A3ProjectReport *r) {
    if (startup) {
        b32 camera = a3_component_count(w, A3_T_CAMERA) > 0 || a3_component_count(w, A3_T_CHARACTER) > 0;
        if (!camera) issue(r, 0, scene, 0, "%s has no Camera or player, so the game starts with a default view. Add a Camera.", scene);
        if (a3_component_count(w, A3_T_LIGHT) == 0) issue(r, 0, scene, 0, "%s has no lights; everything will look flat. Add a Directional Light.", scene);
    }
    u32 primaries = 0;
    for (u32 i = 0; i < w->high_water; ++i) {
        A3EntityRecord *rec = &w->entities[i];
        if (!rec->alive) continue;
        A3Entity e = { i, rec->gen };
        A3CCamera *cam = (A3CCamera *)a3_component_get(w, e, A3_T_CAMERA);
        if (cam && cam->primary) primaries++;
        /* every asset field of every component (models, textures, scripts, sounds, clips...) */
        for (u32 t = 0; t < a3_component_type_count(); ++t) {
            A3ComponentType *ct = a3_component_type(t);
            if (!ct) continue;
            u8 *data = (u8 *)a3_component_get(w, e, t);
            if (!data) continue;
            for (u32 f = 0; f < ct->field_count; ++f) {
                if (ct->fields[f].type != A3_FIELD_ASSET) continue;
                const A3AssetRef *ref = (const A3AssetRef *)(data + ct->fields[f].offset);
                if (!asset_exists(dir, ref->path)) issue(r, 1, scene, 0, "'%s' (%s %s) uses a missing file: %s", rec->name, ct->name, ct->fields[f].label, ref->path);
            }
        }
        A3CMeshRenderer *mr = (A3CMeshRenderer *)a3_component_get(w, e, A3_T_MESH_RENDERER);
        if (mr && mr->primitive == A3_PRIM_NONE && !mr->mesh.path[0]) issue(r, 0, scene, 0, "'%s' has a Mesh Renderer with no shape or model.", rec->name);
        if (a3_component_has(w, e, A3_T_RIGIDBODY) && !a3_component_has(w, e, A3_T_COLLIDER))
            issue(r, 0, scene, 0, "'%s' has a RigidBody but no Collider; it will fall through the floor.", rec->name);
        A3CScript *sc = (A3CScript *)a3_component_get(w, e, A3_T_SCRIPT);
        if (sc && !sc->script.path[0]) issue(r, 0, scene, 0, "'%s' has a Script component with no script file.", rec->name);
        A3CTransform *tr = a3_transform(w, e);
        if (tr && (tr->scale.x == 0 || tr->scale.y == 0 || tr->scale.z == 0)) issue(r, 0, scene, 0, "'%s' has a scale of zero and is invisible.", rec->name);
        if (rec->unknown_components) issue(r, 0, scene, 0, "'%s' has components from a plugin that is not loaded.", rec->name);
    }
    if (primaries > 1) issue(r, 0, scene, 0, "%s has %u cameras marked Primary; only the first is used.", scene, primaries);
}

typedef struct Scan { const char *root; A3ProjectReport *r; const char *startup; char rel[PP]; } Scan;

static void check_scene_file(Scan *s, const char *rel, b32 startup) {
    char abs[PP];
    a3_path_join(abs, sizeof(abs), s->root, rel);
    A3World *w = a3_world_create("Check");
    A3SceneLoadReport rep;
    if (a3_scene_load_file(w, abs, &rep) != A3_OK) issue(s->r, 1, rel, 0, "%s cannot be loaded: %s", rel, rep.error);
    else check_world(s->root, w, rel, startup, s->r);
    a3_world_destroy(w);
    s->r->scenes++;
}

static b32 scan_visit(const char *dir, const A3DirEntry *e, void *user) {
    Scan *s = (Scan *)user;
    if (e->name[0] == '.') return 1;
    char saved[PP], abs[PP];
    a3_strcpy(saved, sizeof(saved), s->rel);
    a3_snprintf(s->rel, sizeof(s->rel), "%s/%s", saved, e->name);
    a3_path_join(abs, sizeof(abs), dir, e->name);
    if (e->is_dir) a3_dir_list(abs, scan_visit, s);
    else {
        s->r->files++;
        if (e->size > 32ull * 1024 * 1024) issue(s->r, 0, s->rel, 0, "%s is %.0f MB; large files make downloads slow.", s->rel, (f64)e->size / (1024.0 * 1024.0));
        if (a3_str_ends_with(e->name, ".a3script")) {
            s->r->scripts++;
            A3FileData fd;
            if (a3_file_read_all(abs, A3_MEM_TEMP, &fd) == A3_OK) {
                A3SError err;
                if (!a3_scripts_check(s->rel, (const char *)fd.data, fd.size, &err))
                    issue(s->r, 1, s->rel, err.line, "%s line %d: %s%s%s", s->rel, err.line, err.message, err.hint[0] ? ". " : "", err.hint);
                a3_free(fd.data);
            }
        } else if (a3_str_ends_with(e->name, ".a3scene") && !a3_streq(s->rel, s->startup)) {
            check_scene_file(s, s->rel, 0);
        }
    }
    a3_strcpy(s->rel, sizeof(s->rel), saved);
    return 1;
}

void a3_project_validate(const char *dir, const char *scene_rel, A3ProjectReport *r) {
    a3_zero_struct(r);
    A3ProjectInfo info;
    char proj[PP];
    a3_path_join(proj, sizeof(proj), dir, "project.a3proj");
    if (!a3_file_exists(proj)) { issue(r, 1, "project.a3proj", 0, "%s is not an ASM3D project (project.a3proj is missing).", dir); return; }
    if (!a3_project_read(dir, &info)) issue(r, 1, "project.a3proj", 0, "project.a3proj is not valid JSON.");
    const char *startup = scene_rel && scene_rel[0] ? scene_rel : info.startup_scene;
    Scan s;
    a3_zero_struct(&s);
    s.root = dir;
    s.r = r;
    s.startup = startup;
    if (!asset_exists(dir, startup)) issue(r, 1, startup, 0, "The startup scene %s does not exist (save the scene first).", startup);
    else check_scene_file(&s, startup, 1);
    a3_strcpy(s.rel, sizeof(s.rel), "Assets");
    char adir[PP];
    a3_path_join(adir, sizeof(adir), dir, "Assets");
    a3_dir_list(adir, scan_visit, &s);
}

void a3_project_report_free(A3ProjectReport *r) { a3_array_free(r->issues); }

void a3_project_report_text(const A3ProjectReport *r, A3StrBuf *out) {
    for (u32 i = 0; i < r->issues.count; ++i) a3_strbuf_appendf(out, "%s %s\n", r->issues.data[i].error ? "ERROR:" : "Warning:", r->issues.data[i].text);
    a3_strbuf_appendf(out, "Checked %u scene(s), %u script(s), %u asset file(s): %u error(s), %u warning(s).\n", r->scenes, r->scripts, r->files, r->errors, r->warnings);
}

/* ======================================================================== */
/* Build                                                                    */
/* ======================================================================== */

typedef struct Copy { char src[PP], dst[PP]; u32 files; u64 bytes; b32 failed; A3ProjectLogFn log; void *user; } Copy;

static void plog(A3ProjectLogFn log, void *user, const char *fmt, ...) A3_PRINTF_LIKE(3, 4);
static void plog(A3ProjectLogFn log, void *user, const char *fmt, ...) {
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    a3_vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (log) log(user, line);
    A3_INFO("build", "%s", line);
}

static b32 copy_visit(const char *dir, const A3DirEntry *e, void *user) {
    Copy *c = (Copy *)user;
    A3_UNUSED(dir);
    if (e->name[0] == '.') return 1;
    char s[PP], d[PP];
    a3_path_join(s, sizeof(s), c->src, e->name);
    a3_path_join(d, sizeof(d), c->dst, e->name);
    if (e->is_dir) {
        a3_dir_create(d);
        Copy sub = *c;
        a3_strcpy(sub.src, sizeof(sub.src), s);
        a3_strcpy(sub.dst, sizeof(sub.dst), d);
        a3_dir_list(s, copy_visit, &sub);
        c->files = sub.files;
        c->bytes = sub.bytes;
        c->failed |= sub.failed;
    } else {
        if (a3_file_copy(s, d) != A3_OK) { plog(c->log, c->user, "ERROR: could not copy %s", s); c->failed = 1; return 0; }
        c->files++;
        c->bytes += e->size;
    }
    return !c->failed;
}

b32 a3_project_find_player(char *out, usize cap) {
    char dir[PP];
    if (a3_get_exe_dir(dir, sizeof(dir))) {
        a3_path_join(out, cap, dir, "asm3d_player" A3_EXE_SUFFIX);
        if (a3_file_exists(out)) return 1;
    }
    return a3_find_executable("asm3d_player", out, cap);
}

b32 a3_project_build(const char *dir, const char *player_exe, const char *out_dir, const char *config,
                     A3ProjectLogFn log, void *user, char *out_path, usize out_cap) {
    u64 t0 = a3_time_ns();
    if (out_path && out_cap) out_path[0] = 0;
    A3ProjectReport rep;
    a3_project_validate(dir, 0, &rep);
    plog(log, user, "Checked project: %u error(s), %u warning(s)", rep.errors, rep.warnings);
    for (u32 i = 0; i < rep.issues.count; ++i) plog(log, user, "%s %s", rep.issues.data[i].error ? "ERROR:" : "Warning:", rep.issues.data[i].text);
    u32 errors = rep.errors;
    a3_project_report_free(&rep);
    if (errors) { plog(log, user, "Build stopped: fix the errors listed above first."); return 0; }
    char player[PP];
    if (player_exe && player_exe[0]) a3_strcpy(player, sizeof(player), player_exe);
    else if (!a3_project_find_player(player, sizeof(player))) {
        plog(log, user, "ERROR: asm3d_player was not found next to this program. Rebuild ASM3D with CMake (target asm3d_player).");
        return 0;
    }
    A3ProjectInfo info;
    a3_project_read(dir, &info);
    char name[64];
    a3_strcpy(name, sizeof(name), info.name);
    for (char *p = name; *p; ++p) if (!a3_is_alnum(*p) && *p != '-' && *p != '_') *p = '_';
    char out[PP], data[PP];
    if (out_dir && out_dir[0]) a3_strcpy(out, sizeof(out), out_dir);
    else {
        char rel[PP];
        a3_snprintf(rel, sizeof(rel), "Builds/Desktop-%s/%s", config && config[0] ? config : "Debug", name);
        a3_path_join(out, sizeof(out), dir, rel);
    }
    if (a3_dir_exists(out)) a3_dir_delete_recursive(out);
    a3_path_join(data, sizeof(data), out, "data");
    if (a3_dir_create(data) != A3_OK) { plog(log, user, "ERROR: cannot create %s", data); return 0; }
    plog(log, user, "Output: %s", out);
    char exe[PP], exe_name[96];
    a3_snprintf(exe_name, sizeof(exe_name), "%s" A3_EXE_SUFFIX, name);
    a3_path_join(exe, sizeof(exe), out, exe_name);
    if (a3_file_copy(player, exe) != A3_OK) { plog(log, user, "ERROR: could not copy the player to %s", exe); return 0; }
    plog(log, user, "Copied player program (%s)", a3_path_filename(player));
    char src[PP], dst[PP];
    a3_path_join(src, sizeof(src), dir, "project.a3proj");
    a3_path_join(dst, sizeof(dst), data, "project.a3proj");
    if (a3_file_copy(src, dst) != A3_OK) { plog(log, user, "ERROR: could not copy project.a3proj"); return 0; }
    Copy c;
    a3_zero_struct(&c);
    c.log = log;
    c.user = user;
    a3_path_join(c.src, sizeof(c.src), dir, "Assets");
    a3_path_join(c.dst, sizeof(c.dst), data, "Assets");
    a3_dir_create(c.dst);
    a3_dir_list(c.src, copy_visit, &c);
    if (c.failed) return 0;
    plog(log, user, "Packaged %u asset files (%.2f MB)", c.files, (f64)c.bytes / (1024.0 * 1024.0));
    char readme[1024], rp[PP];
    a3_snprintf(readme, sizeof(readme),
        "%s\n\nMade with ASM3D %s.\n\nRun %s to play.\nControls: WASD to move, mouse to look (click to capture, Escape to release), Space to jump.\n",
        info.name, A3_VERSION_STRING, exe_name);
    a3_path_join(rp, sizeof(rp), out, "README.txt");
    a3_file_write_atomic(rp, readme, a3_strlen(readme));
    if (out_path) a3_strcpy(out_path, out_cap, out);
    plog(log, user, "Build succeeded in %.2f s", (f64)(a3_time_ns() - t0) * 1e-9);
    return 1;
}
