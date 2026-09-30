/*
 * ASM3D - asm3d_cli
 * Command-line access to the engine for automation, CI and AI assistants.
 *
 *   asm3d_cli <command> [subcommand] [arguments] [--options]
 *
 * Every command prints exactly one JSON object on stdout:
 *   {"ok": true,  "command": "...", "result": {...}, "log": [...]}
 *   {"ok": false, "command": "...", "error": "...", "hint": "...", "log": [...]}
 * and exits with 0 on success, 1 on failure. Engine logs go to stderr only
 * with --verbose; warnings and errors are always included in "log".
 * `asm3d_cli batch` reads one command per line from stdin and prints one JSON
 * line per command (JSON Lines), so a tool can drive many edits in one process.
 * `asm3d_cli help` lists every command with its usage as JSON.
 */
#include "../../engine/runtime/a3_engine.h"
#include "../../engine/runtime/a3_project.h"
#include "../../engine/scene/a3_components.h"
#include "../../engine/scene/a3_scene_io.h"
#include "../../engine/script/a3_script_engine.h"
#include "../../engine/physics/a3_physics.h"
#include "../../engine/physics/a3_physics_kernels.h"
#include "../../engine/render/a3_renderer.h"
#include "../../engine/modeling/a3_emesh.h"
#include "../../engine/modeling/a3_emesh_ops.h"
#include "../../engine/modeling/a3_modeling_kernels.h"
#include "../../engine/resource/a3_assets.h"
#include "../../engine/world/a3_citygen.h"
#include "../../engine/world/a3_weather.h"
#include "../../engine/world/a3_traffic.h"
#include "../../engine/core/a3_log.h"
#include "../../engine/core/a3_string.h"
#include "../../engine/core/a3_format.h"
#include "../../engine/core/a3_strbuf.h"
#include "../../engine/core/a3_json.h"
#include "../../engine/platform/a3_platform.h"
#include <stdio.h>
#include <stdlib.h>

#define PATHCAP 1024
#define MAX_ARGS 64

/* ======================================================================== */
/* Arguments                                                                */
/* ======================================================================== */

typedef struct Args {
    const char *pos[MAX_ARGS];
    u32 npos;
    const char *key[MAX_ARGS];
    const char *val[MAX_ARGS];      /* NULL for flags */
    u32 nopt;
} Args;

static const char *const g_flags[] = { "pretty", "verbose", "dry-run", "all", "no-save", "fields", "empty", "open", "startup",
                                        "no-lights", "no-neon", "no-traffic", "smooth", "render-all", "selected", "edit-mesh" };

static b32 is_flag(const char *k) {
    for (u32 i = 0; i < A3_ARRAY_COUNT(g_flags); ++i) if (a3_streq(k, g_flags[i])) return 1;
    return 0;
}

static void args_parse(Args *a, int argc, char **argv, int start) {
    a3_zero_struct(a);
    for (int i = start; i < argc; ++i) {
        const char *s = argv[i];
        if (s[0] == '-' && s[1] == '-' && s[2]) {
            if (a->nopt >= MAX_ARGS) continue;
            static char keys[MAX_ARGS][64];
            const char *eq = a3_strchr(s + 2, '=');
            if (eq) {
                usize n = (usize)(eq - (s + 2));
                if (n >= 64) n = 63;
                a3_memcpy(keys[a->nopt], s + 2, n);
                keys[a->nopt][n] = 0;
                a->key[a->nopt] = keys[a->nopt];
                a->val[a->nopt] = eq + 1;
            } else {
                a->key[a->nopt] = s + 2;
                a->val[a->nopt] = (!is_flag(s + 2) && i + 1 < argc) ? argv[++i] : 0;
            }
            a->nopt++;
        } else if (a->npos < MAX_ARGS) a->pos[a->npos++] = s;
    }
}

static const char *opt(const Args *a, const char *k, const char *def) {
    for (u32 i = 0; i < a->nopt; ++i) if (a3_streq(a->key[i], k)) return a->val[i] ? a->val[i] : "1";
    return def;
}
static b32 has_opt(const Args *a, const char *k) { return opt(a, k, 0) != 0; }
static const char *arg(const Args *a, u32 i) { return i < a->npos ? a->pos[i] : 0; }

/* ======================================================================== */
/* Output                                                                   */
/* ======================================================================== */

typedef struct Out {
    A3StrBuf body;
    A3JsonWriter jw;           /* writes the "result" object */
    char error[512];
    char hint[512];
    A3_ARRAY_TYPE(A3LogEntry) log;
    b32 verbose;
    b32 capture_info;          /* also keep info messages (script output) */
} Out;

static Out g_out;

static void log_sink(const A3LogEntry *e, void *user) {
    A3_UNUSED(user);
    if (g_out.verbose) {
        fprintf(stderr, "[%s] %s%s%s\n", e->category, e->message, e->hint[0] ? "\n    hint: " : "", e->hint);
        fflush(stderr);
    }
    if (e->level >= A3_LOG_WARN || (g_out.capture_info && a3_streq(e->category, "script")))
        a3_array_push(g_out.log, *e, A3_MEM_TEMP);
}

static A3JsonWriter *R(void) { return &g_out.jw; }

static b32 fail(const char *hint, const char *fmt, ...) A3_PRINTF_LIKE(2, 3);
static b32 fail(const char *hint, const char *fmt, ...) {
    if (g_out.error[0]) return 0;
    va_list ap;
    va_start(ap, fmt);
    a3_vsnprintf(g_out.error, sizeof(g_out.error), fmt, ap);
    va_end(ap);
    a3_strcpy(g_out.hint, sizeof(g_out.hint), hint ? hint : "");
    return 0;
}

static void out_begin(void) {
    a3_strbuf_clear(&g_out.body);
    a3_jw_init(&g_out.jw, &g_out.body, 1);
    a3_jw_begin_object(&g_out.jw);
    g_out.error[0] = g_out.hint[0] = 0;
    a3_array_clear(g_out.log);
}

static void out_end(const char *command, b32 ok, b32 pretty) {
    a3_jw_end_object(&g_out.jw);
    A3StrBuf sb;
    a3_strbuf_init(&sb, A3_MEM_TEMP);
    A3JsonWriter w;
    a3_jw_init(&w, &sb, !pretty);
    a3_jw_begin_object(&w);
    a3_jw_kv_bool(&w, "ok", ok);
    a3_jw_kv_string(&w, "command", command);
    A3Arena ar;
    a3_arena_init(&ar, A3_MEM_TEMP, A3_KB(64));
    A3JsonError jerr;
    A3Json *result = ok ? a3_json_parse(g_out.body.data ? g_out.body.data : "{}", g_out.body.len, &ar, &jerr) : 0;
    if (ok && !result) { ok = 0; a3_snprintf(g_out.error, sizeof(g_out.error), "internal error: invalid JSON result (%s)", jerr.message); }
    if (ok) {
        a3_jw_key(&w, "result");
        a3_jw_node(&w, result);
    } else {
        a3_jw_kv_string(&w, "error", g_out.error[0] ? g_out.error : "failed");
        if (g_out.hint[0]) a3_jw_kv_string(&w, "hint", g_out.hint);
    }
    a3_jw_key(&w, "log");
    a3_jw_begin_array(&w);
    for (u32 i = 0; i < g_out.log.count; ++i) {
        const A3LogEntry *e = &g_out.log.data[i];
        a3_jw_begin_object(&w);
        a3_jw_kv_string(&w, "level", e->level >= A3_LOG_ERROR ? "error" : e->level == A3_LOG_WARN ? "warning" : "info");
        a3_jw_kv_string(&w, "category", e->category);
        a3_jw_kv_string(&w, "message", e->message);
        if (e->hint[0]) a3_jw_kv_string(&w, "hint", e->hint);
        a3_jw_end_object(&w);
    }
    a3_jw_end_array(&w);
    a3_jw_end_object(&w);
    fputs(a3_strbuf_cstr(&sb), stdout);
    fputc('\n', stdout);
    fflush(stdout);
    a3_strbuf_free(&sb);
    a3_arena_release(&ar);
}

/* ======================================================================== */
/* Helpers                                                                  */
/* ======================================================================== */

static void guid_str(u64 g, char out[17]) { a3_guid_to_string(g, out); }

static b32 is_hex_guid(const char *s) {
    if (a3_strlen(s) != 16) return 0;
    for (const char *p = s; *p; ++p) if (!a3_is_xdigit(*p)) return 0;
    return 1;
}

static A3World *load_scene(const char *path) {
    if (!path) { fail("Give the path of a .a3scene file.", "missing scene path"); return 0; }
    if (!a3_file_exists(path)) { fail("Check the path. Scenes usually live in <project>/Assets/Scenes/.", "scene file not found: %s", path); return 0; }
    A3World *w = a3_world_create("cli");
    A3SceneLoadReport rep;
    if (a3_scene_load_file(w, path, &rep) != A3_OK) {
        fail(rep.hint[0] ? rep.hint : 0, "could not load %s: %s", path, rep.error);
        a3_world_destroy(w);
        return 0;
    }
    return w;
}

static b32 save_scene(A3World *w, const char *path, const Args *a) {
    if (has_opt(a, "dry-run") || has_opt(a, "no-save")) return 1;
    if (a3_scene_save_file(w, path, 0) != A3_OK) return fail("Check that the folder exists and is writable.", "could not save %s", path);
    return 1;
}

/* Entity by GUID (16 hex digits), hierarchy path ("Parent/Child") or name. */
static A3Entity find_entity(A3World *w, const char *ref) {
    if (!ref) { fail("Name the object (its name, Parent/Child path or 16-digit GUID).", "missing object"); return A3_ENTITY_NULL; }
    if (is_hex_guid(ref)) {
        A3Entity e = a3_entity_find_by_guid(w, a3_guid_from_string(ref));
        if (!a3_entity_is_null(e)) return e;
    }
    if (a3_strchr(ref, '/')) {
        A3Entity cur = A3_ENTITY_NULL;
        A3Str rest = a3_str(ref);
        while (rest.len) {
            A3Str part = a3_str_split_next(&rest, '/');
            char name[A3_NAME_MAX];
            a3_str_to_buf(part, name, sizeof(name));
            A3Entity found = A3_ENTITY_NULL;
            for (u32 i = 0; i < w->high_water; ++i) {
                A3EntityRecord *r = &w->entities[i];
                if (!r->alive || !a3_streq(r->name, name)) continue;
                A3Entity e = { i, r->gen };
                A3Entity p = a3_entity_parent(w, e);
                if ((a3_entity_is_null(cur) && a3_entity_is_null(p)) || (!a3_entity_is_null(cur) && a3_entity_eq(p, cur))) { found = e; break; }
            }
            if (a3_entity_is_null(found)) break;
            cur = found;
            if (!rest.len) return cur;
        }
    }
    A3Entity e = a3_entity_find_by_name(w, ref);
    if (!a3_entity_is_null(e)) return e;
    const char *cands[512];
    u32 n = 0;
    for (u32 i = 0; i < w->high_water && n < 512; ++i) if (w->entities[i].alive) cands[n++] = w->entities[i].name;
    char hint[200];
    a3s_suggest(ref, cands, n, hint, sizeof(hint));
    fail(hint[0] ? hint : "Run 'entity list <scene>' to see every object.", "no object called '%s' in the scene", ref);
    return A3_ENTITY_NULL;
}

static A3ComponentType *find_component_type(const char *name) {
    A3ComponentType *t = a3_component_type_by_name(name);
    if (t) return t;
    const char *cands[128];
    u32 n = 0;
    for (u32 i = 0; i < a3_component_type_count() && n < 128; ++i) if (a3_component_type(i)) cands[n++] = a3_component_type(i)->name;
    char hint[200];
    a3s_suggest(name, cands, n, hint, sizeof(hint));
    fail(hint[0] ? hint : "Run 'components' to list every component type.", "unknown component type '%s'", name);
    return 0;
}

static void write_entity(A3JsonWriter *jw, A3World *w, A3Entity e, b32 with_components) {
    A3EntityRecord *r = a3_entity_record(w, e);
    char g[17];
    a3_jw_begin_object(jw);
    guid_str(r->guid, g);
    a3_jw_kv_string(jw, "guid", g);
    a3_jw_kv_string(jw, "name", r->name);
    A3Entity p = a3_entity_parent(w, e);
    a3_jw_key(jw, "parent");
    if (a3_entity_is_null(p)) a3_jw_null(jw);
    else { guid_str(a3_entity_guid(w, p), g); a3_jw_string(jw, g); }
    a3_jw_kv_bool(jw, "active", a3_entity_active(w, e));
    if (with_components) {
        a3_jw_key(jw, "components");
        a3_jw_begin_object(jw);
        for (u32 t = 0; t < a3_component_type_count(); ++t) {
            A3ComponentType *ct = a3_component_type(t);
            void *data = ct ? a3_component_get(w, e, t) : 0;
            if (!data) continue;
            a3_jw_key(jw, ct->name);
            a3_component_write_json(jw, ct, data);
        }
        a3_jw_end_object(jw);
    } else {
        a3_jw_key(jw, "components");
        a3_jw_begin_array(jw);
        for (u32 t = 0; t < a3_component_type_count(); ++t) {
            A3ComponentType *ct = a3_component_type(t);
            if (ct && a3_component_has(w, e, t)) a3_jw_string(jw, ct->name);
        }
        a3_jw_end_array(jw);
    }
    a3_jw_end_object(jw);
}

/* Parses a command-line value into JSON: JSON as is, "1,2,3" as an array, anything else as text. */
static A3Json *parse_value(const char *text, A3Arena *ar) {
    A3JsonError err;
    A3Json *j = a3_json_parse(text, a3_strlen(text), ar, &err);
    if (j) return j;
    char buf[600];
    b32 numeric_list = a3_strchr(text, ',') != 0;
    for (const char *p = text; *p && numeric_list; ++p) if (!(a3_is_digit(*p) || *p == ',' || *p == '.' || *p == '-' || *p == ' ' || *p == 'e')) numeric_list = 0;
    if (numeric_list) a3_snprintf(buf, sizeof(buf), "[%s]", text);
    else {
        A3StrBuf sb;
        a3_strbuf_init(&sb, A3_MEM_TEMP);
        A3JsonWriter jw;
        a3_jw_init(&jw, &sb, 1);
        a3_jw_string(&jw, text);
        a3_strcpy(buf, sizeof(buf), a3_strbuf_cstr(&sb));
        a3_strbuf_free(&sb);
    }
    return a3_json_parse(buf, a3_strlen(buf), ar, &err);
}

/* Sets Component.field on e from text. */
static b32 set_field(A3World *w, A3Entity e, const char *path, const char *value) {
    const char *dot = a3_strchr(path, '.');
    if (!dot) return fail("Use Component.field, for example Transform.position or Light.intensity.", "'%s' is not Component.field", path);
    char cname[A3_NAME_MAX];
    usize n = (usize)(dot - path);
    if (n >= sizeof(cname)) n = sizeof(cname) - 1;
    a3_memcpy(cname, path, n);
    cname[n] = 0;
    A3ComponentType *ct = find_component_type(cname);
    if (!ct) return 0;
    const A3FieldDesc *f = a3_component_find_field(ct, dot + 1);
    if (!f) {
        const char *cands[A3_MAX_FIELDS];
        for (u32 i = 0; i < ct->field_count; ++i) cands[i] = ct->fields[i].name;
        char hint[200];
        a3s_suggest(dot + 1, cands, ct->field_count, hint, sizeof(hint));
        return fail(hint[0] ? hint : "Run 'components <Name>' to list its fields.", "the %s component has no field '%s'", ct->name, dot + 1);
    }
    void *data = a3_component_get(w, e, ct->id);
    if (!data) data = a3_component_add(w, e, ct->id);
    if (!data) return fail(0, "could not add %s", ct->name);
    A3Arena ar;
    a3_arena_init(&ar, A3_MEM_TEMP, A3_KB(8));
    A3Json *j = parse_value(value, &ar);
    b32 ok = j && a3_field_read_json(j, f, data);
    a3_arena_release(&ar);
    if (!ok) return fail("Vectors look like 1,2,3 or [1,2,3]; text can be written plainly; options by name.", "could not set %s.%s to %s", ct->name, f->name, value);
    return 1;
}

static void write_script_value(A3JsonWriter *jw, const A3SValue *v) {
    switch (v->type) {
    case A3S_NIL: a3_jw_null(jw); break;
    case A3S_BOOL: a3_jw_bool(jw, v->as.b); break;
    case A3S_NUM: a3_jw_number(jw, v->as.num); break;
    case A3S_VEC3: a3_jw_floats(jw, v->as.v, 3); break;
    case A3S_STR: a3_jw_string(jw, v->as.str->chars); break;
    case A3S_LIST:
        a3_jw_begin_array(jw);
        for (u32 i = 0; i < v->as.list->count; ++i) write_script_value(jw, &v->as.list->items[i]);
        a3_jw_end_array(jw);
        break;
    default: { char buf[128]; a3s_to_string(v, buf, sizeof(buf)); a3_jw_string(jw, buf); } break;
    }
}

static void write_script_error(A3JsonWriter *jw, const char *file, const A3SError *err) {
    a3_jw_begin_object(jw);
    if (file) a3_jw_kv_string(jw, "file", file);
    a3_jw_kv_int(jw, "line", err->line);
    a3_jw_kv_int(jw, "column", err->col);
    a3_jw_kv_string(jw, "message", err->message);
    if (err->hint[0]) a3_jw_kv_string(jw, "hint", err->hint);
    if (err->function[0]) a3_jw_kv_string(jw, "function", err->function);
    a3_jw_end_object(jw);
}

static b32 read_text(const char *path, A3FileData *fd) {
    if (a3_file_read_all(path, A3_MEM_TEMP, fd) != A3_OK) return fail("Check the path.", "cannot read %s", path);
    return 1;
}

static b32 parse_vec3(const char *s, A3Vec3 *out) {
    f32 v[3] = { 0, 0, 0 };
    A3Str rest = a3_str(s);
    for (u32 i = 0; i < 3; ++i) {
        A3Str part = a3_str_trim(a3_str_split_next(&rest, ','));
        f64 d;
        if (!part.len || !a3_parse_f64(part.ptr, part.len, &d)) return fail("Write vectors as x,y,z (for example 0,1.5,-2).", "'%s' is not a vector", s);
        v[i] = (f32)d;
    }
    *out = a3_v3(v[0], v[1], v[2]);
    return 1;
}

/* ======================================================================== */
/* Commands: info                                                           */
/* ======================================================================== */

typedef b32 (*CmdFn)(const Args *a);
typedef struct Cmd { const char *name; const char *sub; CmdFn fn; const char *usage; const char *doc; } Cmd;
static const Cmd g_cmds[];
static const u32 g_cmd_count;

static b32 c_help(const Args *a) {
    A3_UNUSED(a);
    a3_jw_kv_string(R(), "program", "asm3d_cli");
    a3_jw_kv_string(R(), "version", A3_VERSION_STRING);
    a3_jw_kv_string(R(), "output", "one JSON object per command: {ok, command, result | error + hint, log}");
    a3_jw_key(R(), "commands");
    a3_jw_begin_array(R());
    for (u32 i = 0; i < g_cmd_count; ++i) {
        a3_jw_begin_object(R());
        char full[64];
        a3_snprintf(full, sizeof(full), "%s%s%s", g_cmds[i].name, g_cmds[i].sub ? " " : "", g_cmds[i].sub ? g_cmds[i].sub : "");
        a3_jw_kv_string(R(), "command", full);
        a3_jw_kv_string(R(), "usage", g_cmds[i].usage);
        a3_jw_kv_string(R(), "description", g_cmds[i].doc);
        a3_jw_end_object(R());
    }
    a3_jw_end_array(R());
    a3_jw_key(R(), "global_options");
    a3_jw_begin_array(R());
    a3_jw_string(R(), "--pretty  indented JSON");
    a3_jw_string(R(), "--verbose  engine log on stderr");
    a3_jw_string(R(), "--dry-run  edit commands do not write files");
    a3_jw_end_array(R());
    return 1;
}

static b32 c_version(const Args *a) {
    A3_UNUSED(a);
    a3_jw_kv_string(R(), "version", A3_VERSION_STRING);
    a3_jw_kv_int(R(), "format_version", A3_FORMAT_VERSION);
#if A3_PLATFORM_WINDOWS
    a3_jw_kv_string(R(), "platform", "windows-x64");
#else
    a3_jw_kv_string(R(), "platform", "linux-x86_64");
#endif
    a3_jw_kv_string(R(), "physics_kernels", a3_phys_kernel_backend());
    return 1;
}

static void write_component_schema(A3JsonWriter *jw, const A3ComponentType *t, b32 fields) {
    a3_jw_begin_object(jw);
    a3_jw_kv_string(jw, "name", t->name);
    a3_jw_kv_string(jw, "category", t->category);
    if (t->doc) a3_jw_kv_string(jw, "description", t->doc);
    a3_jw_kv_bool(jw, "custom", (t->flags & A3_COMP_CUSTOM) != 0);
    if (fields) {
        a3_jw_key(jw, "fields");
        a3_jw_begin_array(jw);
        for (u32 i = 0; i < t->field_count; ++i) {
            const A3FieldDesc *f = &t->fields[i];
            if (f->flags & A3_FIELD_FLAG_HIDDEN) continue;
            a3_jw_begin_object(jw);
            a3_jw_kv_string(jw, "name", f->name);
            a3_jw_kv_string(jw, "label", f->label);
            a3_jw_kv_string(jw, "type", a3_field_type_name(f->type));
            if (f->min < f->max) { a3_jw_kv_number(jw, "min", f->min); a3_jw_kv_number(jw, "max", f->max); }
            if (f->enum_names) {
                a3_jw_key(jw, "options");
                a3_jw_begin_array(jw);
                for (u32 k = 0; k < f->enum_count; ++k) a3_jw_string(jw, f->enum_names[k]);
                a3_jw_end_array(jw);
            }
            if (f->flags & A3_FIELD_FLAG_TRANSIENT) a3_jw_kv_bool(jw, "runtime_only", 1);
            if (f->doc) a3_jw_kv_string(jw, "description", f->doc);
            a3_jw_end_object(jw);
        }
        a3_jw_end_array(jw);
    }
    a3_jw_end_object(jw);
}

static b32 c_components(const Args *a) {
    const char *name = arg(a, 0);
    if (name) {
        A3ComponentType *t = find_component_type(name);
        if (!t) return 0;
        a3_jw_key(R(), "component");
        write_component_schema(R(), t, 1);
        return 1;
    }
    a3_jw_key(R(), "components");
    a3_jw_begin_array(R());
    for (u32 i = 0; i < a3_component_type_count(); ++i) {
        A3ComponentType *t = a3_component_type(i);
        if (t && t->name[0] && !(t->flags & A3_COMP_HIDDEN)) write_component_schema(R(), t, has_opt(a, "fields"));
    }
    a3_jw_end_array(R());
    return 1;
}

static b32 c_api(const Args *a) {
    const char *cat = opt(a, "category", 0);
    a3_jw_key(R(), "functions");
    a3_jw_begin_array(R());
    for (u32 i = 0; i < a3s_native_count(); ++i) {
        const A3SNative *n = a3s_native_get(i);
        if (cat && (!n->category || !a3_streq(n->category, cat))) continue;
        a3_jw_begin_object(R());
        a3_jw_kv_string(R(), "name", n->name);
        a3_jw_kv_string(R(), "category", n->category ? n->category : "");
        a3_jw_kv_string(R(), "signature", n->signature ? n->signature : n->name);
        a3_jw_kv_string(R(), "description", n->doc ? n->doc : "");
        a3_jw_kv_int(R(), "min_args", n->min_args);
        a3_jw_kv_int(R(), "max_args", n->max_args);
        a3_jw_end_object(R());
    }
    a3_jw_end_array(R());
    a3_jw_key(R(), "events");
    a3_jw_begin_array(R());
    const char *events[] = { "on_start()", "on_update(dt)", "on_fixed_update(dt)", "on_trigger_enter(other)", "on_trigger_exit(other)", "on_collision(other)" };
    for (u32 i = 0; i < A3_ARRAY_COUNT(events); ++i) a3_jw_string(R(), events[i]);
    a3_jw_end_array(R());
    return 1;
}

/* ======================================================================== */
/* Commands: project                                                        */
/* ======================================================================== */

typedef struct ListCtx { const char *ext; char rel[PATHCAP]; A3JsonWriter *jw; u32 count; } ListCtx;

static b32 list_visit(const char *dir, const A3DirEntry *e, void *user) {
    ListCtx *c = (ListCtx *)user;
    if (e->name[0] == '.') return 1;
    char saved[PATHCAP], abs[PATHCAP];
    a3_strcpy(saved, sizeof(saved), c->rel);
    a3_snprintf(c->rel, sizeof(c->rel), "%s/%s", saved, e->name);
    a3_path_join(abs, sizeof(abs), dir, e->name);
    if (e->is_dir) a3_dir_list(abs, list_visit, c);
    else if (!c->ext || a3_str_ends_with(e->name, c->ext)) { a3_jw_string(c->jw, c->rel); c->count++; }
    a3_strcpy(c->rel, sizeof(c->rel), saved);
    return 1;
}

static void list_files(const char *project, const char *ext, const char *key) {
    ListCtx c;
    a3_zero_struct(&c);
    c.ext = ext;
    c.jw = R();
    a3_strcpy(c.rel, sizeof(c.rel), "Assets");
    char adir[PATHCAP];
    a3_path_join(adir, sizeof(adir), project, "Assets");
    a3_jw_key(R(), key);
    a3_jw_begin_array(R());
    a3_dir_list(adir, list_visit, &c);
    a3_jw_end_array(R());
}

static b32 need_project(const char *dir) {
    if (!dir) return fail("Give the project folder (the one containing project.a3proj).", "missing project folder");
    char p[PATHCAP];
    a3_path_join(p, sizeof(p), dir, "project.a3proj");
    if (!a3_file_exists(p)) return fail("Pick the folder that contains project.a3proj, or create one with 'project new'.", "%s is not an ASM3D project", dir);
    return 1;
}

static b32 c_project_info(const Args *a) {
    const char *dir = arg(a, 0);
    if (!need_project(dir)) return 0;
    A3ProjectInfo info;
    a3_project_read(dir, &info);
    a3_jw_kv_string(R(), "name", info.name);
    a3_jw_kv_string(R(), "startup_scene", info.startup_scene);
    a3_jw_kv_string(R(), "template", info.template_name);
    a3_jw_kv_string(R(), "engine", info.engine);
    a3_jw_kv_int(R(), "width", info.width);
    a3_jw_kv_int(R(), "height", info.height);
    list_files(dir, ".a3scene", "scenes");
    list_files(dir, ".a3script", "scripts");
    list_files(dir, ".a3shader", "shaders");
    list_files(dir, ".a3anim", "animations");
    list_files(dir, ".obj", "models");
    if (has_opt(a, "all")) list_files(dir, 0, "files");
    return 1;
}

static void default_scene(A3World *w) {
    A3Entity sun = a3_entity_create(w, "Sun");
    A3CTransform *t = (A3CTransform *)a3_component_add(w, sun, A3_T_TRANSFORM);
    t->position = a3_v3(0, 10, 0);
    t->rotation = a3_quat_euler(-50 * A3_DEG2RAD, 35 * A3_DEG2RAD, 0);
    A3CLight *l = (A3CLight *)a3_component_add(w, sun, A3_T_LIGHT);
    l->type = A3_LIGHT_DIRECTIONAL;
    l->cast_shadows = 1;
    A3Entity ground = a3_entity_create(w, "Ground");
    t = (A3CTransform *)a3_component_add(w, ground, A3_T_TRANSFORM);
    t->scale = a3_v3(40, 1, 40);
    A3CMeshRenderer *mr = (A3CMeshRenderer *)a3_component_add(w, ground, A3_T_MESH_RENDERER);
    mr->primitive = A3_PRIM_PLANE;
    mr->base_color = a3_v4(0.36f, 0.5f, 0.3f, 1);
    A3CCollider *c = (A3CCollider *)a3_component_add(w, ground, A3_T_COLLIDER);
    c->shape = A3_SHAPE_BOX;
    c->size = a3_v3(1, 0.02f, 1);
    A3Entity cam = a3_entity_create(w, "Main Camera");
    t = (A3CTransform *)a3_component_add(w, cam, A3_T_TRANSFORM);
    t->position = a3_v3(0, 3, 8);
    t->rotation = a3_quat_euler(-18 * A3_DEG2RAD, 0, 0);
    a3_component_add(w, cam, A3_T_CAMERA);
}

static b32 c_project_new(const Args *a) {
    const char *dir = arg(a, 0);
    if (!dir) return fail("Give a folder for the new project.", "missing folder");
    char p[PATHCAP];
    a3_path_join(p, sizeof(p), dir, "project.a3proj");
    if (a3_file_exists(p)) return fail("Pick an empty folder.", "%s already contains a project", dir);
    const char *sub[] = { "Assets/Scenes", "Assets/Scripts", "Assets/Models", "Assets/Textures", "Assets/Shaders", "Assets/Animations" };
    for (u32 i = 0; i < A3_ARRAY_COUNT(sub); ++i) {
        a3_path_join(p, sizeof(p), dir, sub[i]);
        if (a3_dir_create(p) != A3_OK) return fail("Check that the location is writable.", "cannot create %s", p);
    }
    A3ProjectInfo info;
    a3_zero_struct(&info);
    a3_strcpy(info.name, sizeof(info.name), opt(a, "name", a3_path_filename(dir)));
    a3_strcpy(info.startup_scene, sizeof(info.startup_scene), "Assets/Scenes/Main.a3scene");
    info.width = 1280;
    info.height = 720;
    if (!a3_project_write(dir, &info)) return fail(0, "cannot write project.a3proj");
    a3_path_join(p, sizeof(p), dir, ".gitignore");
    const char *gi = "# ASM3D\n.asm3d/cache/\n.asm3d/recovery/\nBuilds/\n";
    a3_file_write_atomic(p, gi, a3_strlen(gi));
    A3World *w = a3_world_create("Main");
    default_scene(w);
    a3_path_join(p, sizeof(p), dir, info.startup_scene);
    A3Result r = a3_scene_save_file(w, p, 0);
    a3_world_destroy(w);
    if (r != A3_OK) return fail(0, "cannot write %s", p);
    a3_jw_kv_string(R(), "project", dir);
    a3_jw_kv_string(R(), "name", info.name);
    a3_jw_kv_string(R(), "startup_scene", info.startup_scene);
    return 1;
}

static b32 c_validate(const Args *a) {
    const char *dir = arg(a, 0);
    if (!need_project(dir)) return 0;
    A3ProjectReport rep;
    a3_project_validate(dir, opt(a, "scene", 0), &rep);
    a3_jw_kv_int(R(), "errors", rep.errors);
    a3_jw_kv_int(R(), "warnings", rep.warnings);
    a3_jw_kv_int(R(), "scenes", rep.scenes);
    a3_jw_kv_int(R(), "scripts", rep.scripts);
    a3_jw_kv_int(R(), "files", rep.files);
    a3_jw_key(R(), "issues");
    a3_jw_begin_array(R());
    for (u32 i = 0; i < rep.issues.count; ++i) {
        const A3ProjectIssue *is = &rep.issues.data[i];
        a3_jw_begin_object(R());
        a3_jw_kv_string(R(), "severity", is->error ? "error" : "warning");
        if (is->file[0]) a3_jw_kv_string(R(), "file", is->file);
        if (is->line) a3_jw_kv_int(R(), "line", is->line);
        a3_jw_kv_string(R(), "message", is->text);
        a3_jw_end_object(R());
    }
    a3_jw_end_array(R());
    a3_jw_kv_bool(R(), "buildable", rep.errors == 0);
    a3_project_report_free(&rep);
    return 1;
}

typedef struct BuildLog { A3JsonWriter *jw; } BuildLog;
static void build_line(void *user, const char *line) { a3_jw_string(((BuildLog *)user)->jw, line); }

static b32 c_build(const Args *a) {
    const char *dir = arg(a, 0);
    if (!need_project(dir)) return 0;
    char out[PATHCAP];
    BuildLog bl = { R() };
    a3_jw_key(R(), "steps");
    a3_jw_begin_array(R());
    b32 ok = a3_project_build(dir, opt(a, "player", 0), opt(a, "out", 0), opt(a, "config", "Release"), build_line, &bl, out, sizeof(out));
    a3_jw_end_array(R());
    if (!ok) return fail("Run 'validate <project>' for the full list of problems.", "build failed (see steps)");
    a3_jw_kv_string(R(), "output", out);
    return 1;
}

/* ======================================================================== */
/* Commands: scenes and objects                                             */
/* ======================================================================== */

static b32 c_scene_new(const Args *a) {
    const char *path = arg(a, 0);
    if (!path) return fail("Give the path of the new .a3scene file.", "missing path");
    if (a3_file_exists(path) && !has_opt(a, "all")) return fail("Delete it first, or pass --all to overwrite.", "%s already exists", path);
    A3World *w = a3_world_create("Scene");
    if (!has_opt(a, "empty")) default_scene(w);
    char dir[PATHCAP];
    a3_path_dirname(path, dir, sizeof(dir));
    if (dir[0]) a3_dir_create(dir);
    b32 ok = save_scene(w, path, a);
    a3_jw_kv_string(R(), "scene", path);
    a3_jw_kv_int(R(), "objects", a3_world_entity_count(w));
    a3_world_destroy(w);
    return ok;
}

static b32 c_scene_dump(const Args *a) {
    A3World *w = load_scene(arg(a, 0));
    if (!w) return 0;
    a3_jw_kv_string(R(), "scene", arg(a, 0));
    a3_jw_kv_int(R(), "count", a3_world_entity_count(w));
    a3_jw_key(R(), "objects");
    a3_jw_begin_array(R());
    for (u32 i = 0; i < w->high_water; ++i) {
        if (!w->entities[i].alive) continue;
        A3Entity e = { i, w->entities[i].gen };
        write_entity(R(), w, e, 1);
    }
    a3_jw_end_array(R());
    a3_world_destroy(w);
    return 1;
}

static b32 tree_visit(A3World *w, A3Entity e, u32 depth, void *user) {
    A3JsonWriter *jw = (A3JsonWriter *)user;
    char g[17];
    a3_jw_begin_object(jw);
    guid_str(a3_entity_guid(w, e), g);
    a3_jw_kv_string(jw, "guid", g);
    a3_jw_kv_string(jw, "name", a3_entity_name(w, e));
    a3_jw_kv_int(jw, "depth", depth);
    a3_jw_key(jw, "components");
    a3_jw_begin_array(jw);
    for (u32 t = 0; t < a3_component_type_count(); ++t) {
        A3ComponentType *ct = a3_component_type(t);
        if (ct && a3_component_has(w, e, t)) a3_jw_string(jw, ct->name);
    }
    a3_jw_end_array(jw);
    a3_jw_end_object(jw);
    return 1;
}

static b32 c_entity_list(const Args *a) {
    A3World *w = load_scene(arg(a, 0));
    if (!w) return 0;
    a3_jw_kv_int(R(), "count", a3_world_entity_count(w));
    a3_jw_key(R(), "objects");
    a3_jw_begin_array(R());
    a3_world_visit_hierarchy(w, tree_visit, R());
    a3_jw_end_array(R());
    a3_world_destroy(w);
    return 1;
}

static b32 c_entity_get(const Args *a) {
    A3World *w = load_scene(arg(a, 0));
    if (!w) return 0;
    A3Entity e = find_entity(w, arg(a, 1));
    b32 ok = !a3_entity_is_null(e);
    const char *what = arg(a, 2);
    if (ok && !what) { a3_jw_key(R(), "object"); write_entity(R(), w, e, 1); }
    else if (ok) {
        char cname[A3_NAME_MAX];
        a3_strcpy(cname, sizeof(cname), what);
        char *dot = (char *)a3_strchr(cname, '.');
        if (dot) *dot = 0;
        A3ComponentType *ct = find_component_type(cname);
        void *data = ct ? a3_component_get(w, e, ct->id) : 0;
        if (ct && !data) ok = fail("Add it with 'component add'.", "'%s' has no %s component", a3_entity_name(w, e), ct->name);
        else if (!ct) ok = 0;
        else if (!dot) { a3_jw_key(R(), ct->name); a3_component_write_json(R(), ct, data); }
        else {
            const A3FieldDesc *f = a3_component_find_field(ct, dot + 1);
            if (!f) {
                const char *cands[A3_MAX_FIELDS];
                for (u32 i = 0; i < ct->field_count; ++i) cands[i] = ct->fields[i].name;
                char hint[200];
                a3s_suggest(dot + 1, cands, ct->field_count, hint, sizeof(hint));
                ok = fail(hint[0] ? hint : "Run 'components <Name>' to list its fields.", "the %s component has no field '%s'", ct->name, dot + 1);
            } else {
                /* a3_field_write_json writes "name": value; lift the value out */
                A3StrBuf tmp;
                a3_strbuf_init(&tmp, A3_MEM_TEMP);
                A3JsonWriter tw;
                a3_jw_init(&tw, &tmp, 1);
                a3_jw_begin_object(&tw);
                a3_field_write_json(&tw, f, data);
                a3_jw_end_object(&tw);
                A3Arena ar;
                a3_arena_init(&ar, A3_MEM_TEMP, A3_KB(8));
                A3Json *root = a3_json_parse(tmp.data, tmp.len, &ar, 0);
                a3_jw_kv_string(R(), "component", ct->name);
                a3_jw_kv_string(R(), "field", f->name);
                a3_jw_key(R(), "value");
                if (root && root->v.children.first) a3_jw_node(R(), root->v.children.first); else a3_jw_null(R());
                a3_jw_kv_string(R(), "type", a3_field_type_name(f->type));
                a3_arena_release(&ar);
                a3_strbuf_free(&tmp);
            }
        }
    }
    a3_world_destroy(w);
    return ok;
}

static b32 c_entity_add(const Args *a) {
    const char *path = arg(a, 0), *name = arg(a, 1);
    if (!name) return fail("Usage: entity add <scene> <name> [--parent P] [--at x,y,z] [--primitive cube] [--with A,B]", "missing object name");
    A3World *w = load_scene(path);
    if (!w) return 0;
    b32 ok = 1;
    A3Entity parent = A3_ENTITY_NULL;
    if (opt(a, "parent", 0)) { parent = find_entity(w, opt(a, "parent", 0)); ok = !a3_entity_is_null(parent); }
    A3Entity e = A3_ENTITY_NULL;
    if (ok) {
        e = a3_entity_create(w, name);
        A3CTransform *t = (A3CTransform *)a3_component_add(w, e, A3_T_TRANSFORM);
        if (!a3_entity_is_null(parent)) a3_entity_set_parent(w, e, parent);
        A3Vec3 v;
        if (opt(a, "at", 0)) { if (parse_vec3(opt(a, "at", 0), &v)) t->position = v; else ok = 0; }
        if (ok && opt(a, "scale", 0)) { if (parse_vec3(opt(a, "scale", 0), &v)) t->scale = v; else ok = 0; }
        if (ok && opt(a, "rotation", 0)) { if (parse_vec3(opt(a, "rotation", 0), &v)) t->rotation = a3_quat_euler(v.x * A3_DEG2RAD, v.y * A3_DEG2RAD, v.z * A3_DEG2RAD); else ok = 0; }
        const char *prim = opt(a, "primitive", 0);
        if (ok && prim) {
            static const char *const prims[] = { "none", "cube", "sphere", "plane", "cylinder", "capsule", "cone" };
            i32 pi = -1;
            for (u32 i = 0; i < A3_ARRAY_COUNT(prims); ++i) if (a3_streq(prims[i], prim)) pi = (i32)i;
            if (pi < 0) ok = fail("Shapes: cube, sphere, plane, cylinder, capsule, cone.", "unknown primitive '%s'", prim);
            else {
                A3CMeshRenderer *mr = (A3CMeshRenderer *)a3_component_add(w, e, A3_T_MESH_RENDERER);
                mr->primitive = pi;
            }
        }
        const char *with = opt(a, "with", 0);
        if (ok && with) {
            A3Str rest = a3_str(with);
            while (rest.len && ok) {
                char cn[A3_NAME_MAX];
                a3_str_to_buf(a3_str_trim(a3_str_split_next(&rest, ',')), cn, sizeof(cn));
                A3ComponentType *ct = find_component_type(cn);
                if (!ct) ok = 0;
                else a3_component_add(w, e, ct->id);
            }
        }
        const char *model = opt(a, "model", 0);
        if (ok && model) {
            A3CMeshRenderer *mr = (A3CMeshRenderer *)a3_component_add(w, e, A3_T_MESH_RENDERER);
            mr->primitive = A3_PRIM_NONE;
            a3_strcpy(mr->mesh.path, sizeof(mr->mesh.path), model);
        }
        const char *script = opt(a, "script", 0);
        if (ok && script) {
            A3CScript *sc = (A3CScript *)a3_component_add(w, e, A3_T_SCRIPT);
            a3_strcpy(sc->script.path, sizeof(sc->script.path), script);
        }
    }
    if (ok) ok = save_scene(w, path, a);
    if (ok) { a3_jw_key(R(), "object"); write_entity(R(), w, e, 1); }
    a3_world_destroy(w);
    return ok;
}

static b32 c_entity_remove(const Args *a) {
    A3World *w = load_scene(arg(a, 0));
    if (!w) return 0;
    A3Entity e = find_entity(w, arg(a, 1));
    b32 ok = !a3_entity_is_null(e);
    if (ok) {
        char g[17];
        guid_str(a3_entity_guid(w, e), g);
        a3_jw_kv_string(R(), "removed", g);
        a3_jw_kv_string(R(), "name", a3_entity_name(w, e));
        a3_entity_destroy(w, e);
        ok = save_scene(w, arg(a, 0), a);
    }
    a3_world_destroy(w);
    return ok;
}

static b32 c_entity_set(const Args *a) {
    if (a->npos < 4 || (a->npos - 2) % 2) return fail("Usage: entity set <scene> <object> <Component.field> <value> [<Component.field> <value> ...]", "wrong number of arguments");
    A3World *w = load_scene(arg(a, 0));
    if (!w) return 0;
    A3Entity e = find_entity(w, arg(a, 1));
    b32 ok = !a3_entity_is_null(e);
    for (u32 i = 2; ok && i + 1 < a->npos; i += 2) {
        const char *key = a->pos[i], *val = a->pos[i + 1];
        if (a3_streq(key, "active")) {   /* the object's own enabled flag */
            b32 on = a3_streq(val, "true") || a3_streq(val, "1") || a3_streq(val, "yes");
            if (!on && !a3_streq(val, "false") && !a3_streq(val, "0") && !a3_streq(val, "no")) ok = fail("Use true or false.", "'%s' is not true/false", val);
            else a3_entity_set_active(w, e, on);
        } else ok = set_field(w, e, key, val);
    }
    if (ok) ok = save_scene(w, arg(a, 0), a);
    if (ok) { a3_jw_key(R(), "object"); write_entity(R(), w, e, 1); }
    a3_world_destroy(w);
    return ok;
}

static b32 c_entity_rename(const Args *a) {
    A3World *w = load_scene(arg(a, 0));
    if (!w) return 0;
    A3Entity e = find_entity(w, arg(a, 1));
    b32 ok = !a3_entity_is_null(e);
    if (ok && !arg(a, 2)) ok = fail("Usage: entity rename <scene> <object> <new name>", "missing new name");
    if (ok) { a3_entity_set_name(w, e, arg(a, 2)); ok = save_scene(w, arg(a, 0), a); }
    if (ok) { a3_jw_key(R(), "object"); write_entity(R(), w, e, 0); }
    a3_world_destroy(w);
    return ok;
}

static b32 c_entity_duplicate(const Args *a) {
    A3World *w = load_scene(arg(a, 0));
    if (!w) return 0;
    A3Entity e = find_entity(w, arg(a, 1));
    b32 ok = !a3_entity_is_null(e);
    A3Entity d = A3_ENTITY_NULL;
    if (ok) {
        d = a3_entity_duplicate(w, e);
        ok = !a3_entity_is_null(d) || fail(0, "could not duplicate");
    }
    if (ok && opt(a, "name", 0)) a3_entity_set_name(w, d, opt(a, "name", 0));
    A3Vec3 at;
    if (ok && opt(a, "at", 0)) { if (parse_vec3(opt(a, "at", 0), &at)) { a3_transform_system_update(w); a3_transform_set_world_position(w, d, at); } else ok = 0; }
    if (ok) ok = save_scene(w, arg(a, 0), a);
    if (ok) { a3_jw_key(R(), "object"); write_entity(R(), w, d, 1); }
    a3_world_destroy(w);
    return ok;
}

static b32 component_edit(const Args *a, b32 add) {
    A3World *w = load_scene(arg(a, 0));
    if (!w) return 0;
    A3Entity e = find_entity(w, arg(a, 1));
    b32 ok = !a3_entity_is_null(e);
    A3ComponentType *ct = 0;
    if (ok) { ct = arg(a, 2) ? find_component_type(arg(a, 2)) : 0; ok = ct != 0 || fail("Name a component type.", "missing component"); }
    if (ok) {
        if (add) ok = a3_component_add(w, e, ct->id) != 0 || fail(0, "could not add %s", ct->name);
        else ok = a3_component_remove(w, e, ct->id) || fail(0, "'%s' has no %s component", a3_entity_name(w, e), ct->name);
    }
    if (ok) ok = save_scene(w, arg(a, 0), a);
    if (ok) { a3_jw_key(R(), "object"); write_entity(R(), w, e, 1); }
    a3_world_destroy(w);
    return ok;
}
static b32 c_component_add(const Args *a) { return component_edit(a, 1); }
static b32 c_component_remove(const Args *a) { return component_edit(a, 0); }

/* ======================================================================== */
/* Commands: scripts                                                        */
/* ======================================================================== */

static b32 c_script_check(const Args *a) {
    if (!a->npos) return fail("Give one or more .a3script files.", "missing file");
    u32 bad = 0;
    a3_jw_key(R(), "files");
    a3_jw_begin_array(R());
    for (u32 i = 0; i < a->npos; ++i) {
        A3FileData fd;
        a3_jw_begin_object(R());
        a3_jw_kv_string(R(), "file", a->pos[i]);
        if (a3_file_read_all(a->pos[i], A3_MEM_TEMP, &fd) != A3_OK) { a3_jw_kv_bool(R(), "ok", 0); a3_jw_kv_string(R(), "message", "cannot read the file"); bad++; }
        else {
            A3SError err;
            b32 ok = a3_scripts_check(a->pos[i], (const char *)fd.data, fd.size, &err);
            a3_free(fd.data);
            a3_jw_kv_bool(R(), "ok", ok);
            if (!ok) { a3_jw_key(R(), "error"); write_script_error(R(), 0, &err); bad++; }
        }
        a3_jw_end_object(R());
    }
    a3_jw_end_array(R());
    a3_jw_kv_int(R(), "failed", bad);
    if (bad) return fail("Each failing file lists the line and a suggestion.", "%u of %u script(s) have errors", bad, a->npos);
    return 1;
}

static b32 run_source(const char *name, const char *src, usize len, const char *fn, const char *args_json) {
    g_out.capture_info = 1;
    A3SError err;
    A3SModule *m = a3s_compile(name, src, len, &err);
    if (!m) { a3_jw_key(R(), "error"); write_script_error(R(), name, &err); return fail(err.hint[0] ? err.hint : 0, "line %d: %s", err.line, err.message); }
    A3SInstance *inst = a3s_instance_create(m, a3s_nil(), 0, &err);
    a3s_module_release(m);
    if (!inst) { a3_jw_key(R(), "error"); write_script_error(R(), name, &err); return fail(err.hint[0] ? err.hint : 0, "line %d: %s", err.line, err.message); }
    b32 ok = 1;
    if (fn) {
        A3SValue argv[16];
        u32 argc = 0;
        if (args_json) {
            A3Arena ar;
            a3_arena_init(&ar, A3_MEM_TEMP, A3_KB(8));
            A3JsonError je;
            A3Json *arr = a3_json_parse(args_json, a3_strlen(args_json), &ar, &je);
            for (u32 i = 0; arr && i < a3_json_count(arr) && argc < 16; ++i) {
                A3Json *v = a3_json_at(arr, i);
                if (v->type == A3_JSON_NUMBER) argv[argc++] = a3s_num(v->v.number);
                else if (v->type == A3_JSON_STRING) argv[argc++] = a3s_str(v->v.string);
                else if (v->type == A3_JSON_BOOL) argv[argc++] = a3s_bool(v->v.boolean);
                else if (v->type == A3_JSON_ARRAY && a3_json_count(v) == 3) { f32 f[3]; a3_json_get_floats(v, f, 3); argv[argc++] = a3s_vec3(f[0], f[1], f[2]); }
                else argv[argc++] = a3s_nil();
            }
            if (!arr) ok = fail("Pass arguments as a JSON array, e.g. --args '[1, \"two\", [0,1,0]]'.", "--args is not valid JSON");
            a3_arena_release(&ar);
        }
        A3SValue result;
        if (ok && !a3s_call(inst, fn, argv, argc, &result, 0, &err)) {
            a3_jw_key(R(), "error");
            write_script_error(R(), name, &err);
            ok = fail(err.hint[0] ? err.hint : 0, "line %d: %s", err.line, err.message);
        } else if (ok) {
            a3_jw_key(R(), "return");
            write_script_value(R(), &result);
            a3s_release(&result);
        }
        for (u32 i = 0; i < argc; ++i) a3s_release(&argv[i]);
    }
    a3_jw_key(R(), "globals");
    a3_jw_begin_object(R());
    for (u32 i = 0; i < a3s_instance_global_count(inst); ++i) {
        a3_jw_key(R(), a3s_instance_global_name(inst, i));
        write_script_value(R(), a3s_instance_global(inst, i));
    }
    a3_jw_end_object(R());
    a3s_instance_destroy(inst);
    return ok;
}

static void write_prints(void) {
    a3_jw_key(R(), "output");
    a3_jw_begin_array(R());
    for (u32 i = 0; i < g_out.log.count; ++i)
        if (g_out.log.data[i].level == A3_LOG_INFO && a3_streq(g_out.log.data[i].category, "script")) a3_jw_string(R(), g_out.log.data[i].message);
    a3_jw_end_array(R());
}

static b32 c_script_run(const Args *a) {
    const char *file = arg(a, 0);
    if (!file) return fail("Usage: script run <file.a3script> [--call fn] [--args '[1,2]']", "missing file");
    A3FileData fd;
    if (!read_text(file, &fd)) return 0;
    b32 ok = run_source(file, (const char *)fd.data, fd.size, opt(a, "call", 0), opt(a, "args", 0));
    a3_free(fd.data);
    write_prints();
    return ok;
}

static b32 c_script_eval(const Args *a) {
    const char *code = arg(a, 0);
    if (!code) return fail("Usage: script eval \"1 + 2\" or script eval --code \"let x = 3\\nprint(x)\"", "missing expression");
    A3StrBuf sb;
    a3_strbuf_init(&sb, A3_MEM_TEMP);
    a3_strbuf_appendf(&sb, "fn main() { return %s\n}\n", code);
    b32 ok = run_source("eval", a3_strbuf_cstr(&sb), sb.len, "main", 0);
    a3_strbuf_free(&sb);
    write_prints();
    return ok;
}

/* ======================================================================== */
/* Commands: simulate / screenshot                                          */
/* ======================================================================== */

typedef struct KeyPress { int key; i32 from, to; } KeyPress;

static int key_code(const char *name) {
    int k = a3_key_from_name(name);
    if (k) return k;
    char cap[32];
    a3_strcpy(cap, sizeof(cap), name);
    if (cap[0] >= 'a' && cap[0] <= 'z') cap[0] = (char)(cap[0] - 32);
    return a3_key_from_name(cap);
}

/* --keys "space@10-20,w@0-120" : key held during frames [from, to]. */
static u32 parse_keys(const char *spec, KeyPress *out, u32 cap) {
    u32 n = 0;
    A3Str rest = a3_str(spec ? spec : "");
    while (rest.len && n < cap) {
        A3Str item = a3_str_trim(a3_str_split_next(&rest, ','));
        isize at = a3_str_find(item, '@');
        char name[32];
        a3_str_to_buf(at >= 0 ? a3_str_sub(item, 0, (usize)at) : item, name, sizeof(name));
        KeyPress kp = { key_code(name), 0, 1 << 30 };
        if (!kp.key) { fail("Key names: a-z, 0-9, Space, Enter, Escape, Left, Right, Up, Down, LeftShift...", "unknown key '%s'", name); return 0; }
        if (at >= 0) {
            char range[32];
            a3_str_to_buf(a3_str_sub(item, (usize)at + 1, item.len - (usize)at - 1), range, sizeof(range));
            char *dash = (char *)a3_strchr(range, '-');
            if (dash) { *dash = 0; kp.from = atoi(range); kp.to = atoi(dash + 1); }
            else { kp.from = kp.to = atoi(range); }
        }
        out[n++] = kp;
    }
    return n;
}

static void write_watch(A3World *w, const char *list) {
    a3_jw_key(R(), "objects");
    a3_jw_begin_object(R());
    A3Str rest = a3_str(list);
    while (rest.len) {
        char name[A3_NAME_MAX];
        a3_str_to_buf(a3_str_trim(a3_str_split_next(&rest, ',')), name, sizeof(name));
        A3Entity e = a3_entity_find_by_name(w, name);
        if (!a3_entity_is_null(e) || is_hex_guid(name)) {
            if (a3_entity_is_null(e)) e = a3_entity_find_by_guid(w, a3_guid_from_string(name));
        }
        a3_jw_key(R(), name);
        if (a3_entity_is_null(e)) { a3_jw_null(R()); continue; }
        a3_jw_begin_object(R());
        A3Vec3 p = a3_transform_world_position(w, e);
        a3_jw_kv_floats(R(), "position", &p.x, 3);
        A3CTransform *t = a3_transform(w, e);
        if (t) {
            A3Vec3 eu = a3_quat_to_euler(t->rotation);
            f32 deg[3] = { eu.x * 57.2957795f, eu.y * 57.2957795f, eu.z * 57.2957795f };
            a3_jw_kv_floats(R(), "rotation", deg, 3);
            a3_jw_kv_floats(R(), "scale", &t->scale.x, 3);
        }
        A3CRigidBody *rb = (A3CRigidBody *)a3_component_get(w, e, A3_T_RIGIDBODY);
        if (rb) { a3_jw_kv_floats(R(), "velocity", &rb->velocity.x, 3); a3_jw_kv_bool(R(), "sleeping", rb->sleeping); }
        a3_jw_kv_bool(R(), "active", a3_entity_active(w, e));
        a3_jw_end_object(R());
    }
    a3_jw_end_object(R());
}

static b32 run_game(const Args *a, b32 render) {
    const char *dir = arg(a, 0);
    if (!need_project(dir)) return 0;
    A3ProjectInfo info;
    a3_project_read(dir, &info);
    const char *scene = opt(a, "scene", info.startup_scene);
    i32 frames = atoi(opt(a, "frames", render ? "30" : "120"));
    if (frames < 1 || frames > 1000000) return fail("Use between 1 and 1000000 frames.", "bad --frames");
    f32 dt = (f32)atof(opt(a, "dt", "0.016666667"));
    KeyPress keys[32];
    u32 nkeys = parse_keys(opt(a, "keys", 0), keys, 32);
    if (g_out.error[0]) return 0;
    A3Engine *eng = 0;
    i32 width = 1280, height = 720;
    if (render) {
        const char *size = opt(a, "size", 0);
        if (size) { width = atoi(size); const char *x = a3_strchr(size, 'x'); height = x ? atoi(x + 1) : height; }
        A3EngineDesc d = { info.name, width, height, 0, 1, 0, dir, 60 };
        eng = a3_engine_create(&d);
        if (!eng) return fail("Screenshots need a desktop session with OpenGL 3.3 (on Linux a running X server, e.g. Xvfb).", "could not create a window");
    } else {
        eng = a3_engine_create_headless(dir, 60);
        if (!eng) return fail(0, "could not start the engine");
    }
    char path[PATHCAP];
    a3_path_join(path, sizeof(path), dir, scene);
    A3World *w = a3_world_create("Game");
    A3SceneLoadReport rep;
    if (a3_scene_load_file(w, path, &rep) != A3_OK) {
        fail(rep.hint[0] ? rep.hint : 0, "could not load %s: %s", scene, rep.error);
        a3_world_destroy(w);
        a3_engine_destroy(eng);
        return 0;
    }
    a3_scripts_clear_errors();
    A3InputState input;
    a3_zero_struct(&input);
    a3_engine_set_input_override(eng, &input);
    a3_engine_start_play(eng, w);
    u64 t0 = a3_time_ns();
    const char *record = render ? opt(a, "record", 0) : 0;
    i32 record_from = atoi(opt(a, "record-from", "0")), recorded = 0;
    if (record) a3_dir_create(record);
    i32 trace_every = atoi(opt(a, "trace", "0"));
    const char *watch = opt(a, "watch", 0);
    if (trace_every > 0 && watch) { a3_jw_key(R(), "trace"); a3_jw_begin_array(R()); }
    for (i32 f = 0; f < frames; ++f) {
        u8 prev[A3_KEY_COUNT];
        a3_memcpy(prev, input.keys, sizeof(prev));
        a3_memset(input.keys, 0, sizeof(input.keys));
        for (u32 k = 0; k < nkeys; ++k) if (f >= keys[k].from && f <= keys[k].to) input.keys[keys[k].key] = 1;
        for (int k = 0; k < A3_KEY_COUNT; ++k) { input.keys_pressed[k] = input.keys[k] && !prev[k]; input.keys_released[k] = !input.keys[k] && prev[k]; input.keys_repeat[k] = input.keys_pressed[k]; }
        f32 real_dt;
        a3_engine_begin_frame(eng, &real_dt);
        a3_engine_simulate(eng, w, dt, 0, 0);
        /* rendering is slow without a GPU: only the last frames (or every frame when recording) */
        b32 draw = render && (f >= frames - 3 || (record && f >= record_from) || has_opt(a, "render-all"));
        if (draw) a3_engine_render_world(eng, w, 0);
        if (draw && record && f >= record_from) {
            a3_engine_render_hud(eng, w);
            char fp[PATHCAP], name[32];
            a3_snprintf(name, sizeof(name), "frame_%05d.png", recorded);
            a3_path_join(fp, sizeof(fp), record, name);
            if (a3_engine_screenshot(eng, fp)) recorded++;
            else { fail("Check that the record folder is writable.", "could not write %s", fp); a3_engine_end_frame(eng); break; }
        }
        char next[512];
        if (a3_scripts_take_scene_request(w, next, sizeof(next))) A3_WARN("cli", "load_scene(\"%s\") requested at frame %d (not followed by simulate)", next, f);
        if (render && f == frames - 1 && (!record || opt(a, "out", 0))) {
            const char *eye = opt(a, "camera", 0), *look = opt(a, "look", 0);
            if (eye) {
                A3Vec3 e3, l3 = a3_v3(0, 0, 0);
                if (parse_vec3(eye, &e3) && (!look || parse_vec3(look, &l3))) {
                    A3RenderView v = a3_render_view_look_at(e3, l3, (f32)atof(opt(a, "fov", "60")), width, height);
                    a3_engine_render_world(eng, w, &v);
                    a3_engine_render_hud(eng, w);
                }
            } else if (!record) a3_engine_render_hud(eng, w);
            const char *out = opt(a, "out", "screenshot.png");
            if (a3_engine_screenshot(eng, out)) a3_jw_kv_string(R(), "screenshot", out);
            else fail("Check that the output folder exists.", "could not write %s", out);
        }
        a3_engine_end_frame(eng);
        if (trace_every > 0 && watch && (f % trace_every == 0 || f == frames - 1)) {
            a3_jw_begin_object(R());
            a3_jw_kv_int(R(), "frame", f);
            write_watch(w, watch);
            a3_jw_end_object(R());
        }
    }
    if (trace_every > 0 && watch) a3_jw_end_array(R());
    f64 secs = (f64)(a3_time_ns() - t0) * 1e-9;
    a3_jw_kv_string(R(), "scene", scene);
    a3_jw_kv_int(R(), "frames", frames);
    a3_jw_kv_number(R(), "game_seconds", frames * dt);
    if (record) { a3_jw_kv_string(R(), "recorded_to", record); a3_jw_kv_int(R(), "recorded_frames", recorded); }
    a3_jw_kv_number(R(), "real_seconds", secs);
    a3_jw_kv_int(R(), "object_count", a3_world_entity_count(w));
    a3_jw_kv_int(R(), "scripts_running", a3_scripts_instance_count(w));
    if (watch) write_watch(w, watch);
    a3_jw_key(R(), "script_errors");
    a3_jw_begin_array(R());
    for (u32 i = 0; i < a3_scripts_error_count(); ++i) {
        const A3ScriptErrorInfo *er = a3_scripts_error(i);
        write_script_error(R(), er->path, &er->error);
    }
    a3_jw_end_array(R());
    const A3HudCmd *hud;
    u32 nh = a3_scripts_hud(w, &hud);
    a3_jw_key(R(), "hud_text");
    a3_jw_begin_array(R());
    for (u32 i = 0; i < nh; ++i) if (hud[i].kind == A3_HUD_TEXT) a3_jw_string(R(), hud[i].text);
    a3_jw_end_array(R());
    a3_engine_stop_play(eng, w);
    a3_world_destroy(w);
    a3_engine_destroy(eng);
    return !g_out.error[0];
}

static b32 c_simulate(const Args *a) { return run_game(a, 0); }
static b32 c_screenshot(const Args *a) { return run_game(a, 1); }

/* ======================================================================== */
/* Commands: modeling                                                       */
/* ======================================================================== */

/* ======================================================================== */
/* Commands: world generation                                               */
/* ======================================================================== */

static b32 c_world_city(const Args *a) {
    const char *dir = arg(a, 0);
    if (!need_project(dir)) return 0;
    A3CityDesc d;
    a3_city_desc_default(&d);
    d.seed = (u64)strtoull(opt(a, "seed", "1"), 0, 10);
    d.density = (f32)atof(opt(a, "density", "1"));
    if (d.density < 0.1f || d.density > 1.0f) return fail("Use a density between 0.1 and 1.", "bad density '%s'", opt(a, "density", ""));
    const char *time = opt(a, "time", "night");
    if (a3_streq(time, "day")) d.time = A3_CITY_DAY;
    else if (a3_streq(time, "sunset")) d.time = A3_CITY_SUNSET;
    else if (a3_streq(time, "night")) d.time = A3_CITY_NIGHT;
    else return fail("Use --time day, sunset or night.", "unknown time '%s'", time);
    if (has_opt(a, "no-lights")) d.street_lights = 0;
    if (has_opt(a, "no-neon")) d.neon = 0;
    const char *scene_rel = opt(a, "scene", "Assets/Scenes/City.a3scene");
    char scene_path[PATHCAP], roads_path[PATHCAP], folder[PATHCAP];
    a3_path_join(scene_path, sizeof(scene_path), dir, scene_rel);
    a3_path_join(roads_path, sizeof(roads_path), dir, "Assets/City/roads.json");
    if (a3_file_exists(scene_path) && !has_opt(a, "all")) return fail("Pass --all to overwrite it, or choose another --scene.", "%s already exists", scene_rel);

    A3World *w = a3_world_create("Sol Harbor");
    A3StrBuf roads;
    a3_strbuf_init(&roads, A3_MEM_TEMP);
    A3CityStats st;
    b32 ok = a3_city_generate(w, &d, &st, &roads);
    if (!ok) { a3_strbuf_free(&roads); a3_world_destroy(w); return fail(0, "city generation failed"); }
    if (has_opt(a, "rain")) {
        f32 rain = (f32)atof(opt(a, "rain", "0.8"));
        if (rain < 0.0f || rain > 1.0f) { a3_strbuf_free(&roads); a3_world_destroy(w); return fail("Use --rain between 0 and 1.", "bad rain '%s'", opt(a, "rain", "")); }
        a3_weather_set(w, rain);
    }
    i32 cars = atoi(opt(a, "cars", "40")), peds = atoi(opt(a, "pedestrians", "60"));
    if (!has_opt(a, "no-traffic") && (cars > 0 || peds > 0)) {
        A3Entity te = a3_entity_create(w, "City Traffic");
        a3_component_add(w, te, A3_T_TRANSFORM);
        A3CTraffic *tr = (A3CTraffic *)a3_component_add(w, te, A3_T_TRAFFIC);
        if (tr) { tr->cars = cars; tr->pedestrians = peds; tr->seed = (u32)d.seed; }
    }
    if (!has_opt(a, "dry-run")) {
        a3_path_dirname(scene_path, folder, sizeof(folder));
        a3_dir_create(folder);
        a3_path_dirname(roads_path, folder, sizeof(folder));
        a3_dir_create(folder);
        if (a3_file_write_atomic(roads_path, a3_strbuf_cstr(&roads), roads.len) != A3_OK) ok = fail("Check that the project folder is writable.", "cannot write %s", roads_path);
    }
    if (ok) ok = save_scene(w, scene_path, a);
    if (ok && has_opt(a, "startup") && !has_opt(a, "dry-run")) {
        A3ProjectInfo info;
        if (a3_project_read(dir, &info)) {
            a3_strcpy(info.startup_scene, sizeof(info.startup_scene), scene_rel);
            a3_project_write(dir, &info);
        }
    }
    a3_jw_kv_string(R(), "scene", scene_rel);
    a3_jw_kv_string(R(), "roads", "Assets/City/roads.json");
    a3_jw_kv_string(R(), "time", time);
    a3_jw_kv_int(R(), "objects", a3_world_entity_count(w));
    a3_jw_key(R(), "stats");
    a3_jw_begin_object(R());
    a3_jw_kv_int(R(), "buildings", st.buildings);
    a3_jw_kv_int(R(), "towers", st.towers);
    a3_jw_kv_int(R(), "hotels", st.hotels);
    a3_jw_kv_int(R(), "road_pieces", st.roads);
    a3_jw_kv_int(R(), "lights", st.lights);
    a3_jw_kv_int(R(), "palms", st.palms);
    a3_jw_kv_int(R(), "props", st.props);
    a3_jw_kv_int(R(), "road_nodes", st.road_nodes);
    a3_jw_kv_int(R(), "road_edges", st.road_edges);
    f32 ext[4] = { st.min_x, st.max_x, st.min_z, st.max_z };
    a3_jw_key(R(), "extent_xxzz");
    a3_jw_floats(R(), ext, 4);
    a3_jw_end_object(R());
    a3_strbuf_free(&roads);
    a3_world_destroy(w);
    return ok;
}

static b32 c_mesh_new(const Args *a);
static b32 c_mesh_info(const Args *a);
static b32 c_mesh_edit(const Args *a);
static b32 c_mesh_ops(const Args *a);

/* ======================================================================== */
/* Dispatch                                                                 */
/* ======================================================================== */

int a3_cli_serve(const char *root, int port, b32 open_browser, int max_requests);   /* cli_serve.c */

static const Cmd g_cmds[] = {
    { "help", 0, c_help, "help", "Lists every command (this output)." },
    { "version", 0, c_version, "version", "Engine version, platform and assembly kernel backend." },
    { "components", 0, c_components, "components [Name] [--fields]", "Component types; with a name, its fields (types, ranges, options, descriptions)." },
    { "api", 0, c_api, "api [--category Math]", "Every A3Script function and event." },
    { "project", "new", c_project_new, "project new <folder> [--name N]", "Creates a project with a starter scene (sun, ground, camera)." },
    { "project", "info", c_project_info, "project info <folder> [--all]", "Project settings and its scenes, scripts, shaders, animations and models." },
    { "validate", 0, c_validate, "validate <project> [--scene S]", "Checks scenes, missing files and compiles every script. Issues come with file and line." },
    { "build", 0, c_build, "build <project> [--out folder] [--config Release] [--player path]", "Packages a playable desktop build." },
    { "scene", "new", c_scene_new, "scene new <file.a3scene> [--empty]", "Creates a scene (with sun, ground and camera unless --empty)." },
    { "scene", "dump", c_scene_dump, "scene dump <scene>", "Every object with all component values." },
    { "entity", "list", c_entity_list, "entity list <scene>", "Objects in hierarchy order with depth and component names." },
    { "entity", "get", c_entity_get, "entity get <scene> <object> [Component[.field]]", "One object, component or field. Objects are found by name, Parent/Child path or GUID." },
    { "entity", "add", c_entity_add, "entity add <scene> <name> [--parent P] [--at x,y,z] [--rotation x,y,z] [--scale x,y,z] [--primitive cube] [--model path.obj] [--script path] [--with A,B]", "Creates an object." },
    { "entity", "set", c_entity_set, "entity set <scene> <object> <Component.field> <value> [...]  (or active true|false)", "Sets fields; adds the component if missing. Values: 5, true, 1,2,3, [1,2,3], text, option names." },
    { "entity", "remove", c_entity_remove, "entity remove <scene> <object>", "Deletes an object and its children." },
    { "entity", "rename", c_entity_rename, "entity rename <scene> <object> <new name>", "Renames an object." },
    { "entity", "duplicate", c_entity_duplicate, "entity duplicate <scene> <object> [--name N] [--at x,y,z]", "Copies an object with its children." },
    { "component", "add", c_component_add, "component add <scene> <object> <Component>", "Adds a component with default values." },
    { "component", "remove", c_component_remove, "component remove <scene> <object> <Component>", "Removes a component." },
    { "script", "check", c_script_check, "script check <file.a3script> [...]", "Compiles scripts and reports errors with line, column and suggestion." },
    { "script", "run", c_script_run, "script run <file> [--call fn] [--args JSON-array]", "Runs a script outside a scene; returns print output, the return value and top-level variables." },
    { "script", "eval", c_script_eval, "script eval <expression>", "Evaluates one A3Script expression, e.g. \"lerp(0, 10, 0.25)\"." },
    { "simulate", 0, c_simulate, "simulate <project> [--scene S] [--frames 120] [--dt 0.0166] [--keys space@10-20,w@0-60] [--watch A,B] [--trace N]", "Plays the game headless (no window) and reports object states, script errors and HUD text." },
    { "screenshot", 0, c_screenshot, "screenshot <project> [--scene S] [--frames 30] [--size 1280x720] [--camera x,y,z --look x,y,z] [--out file.png] [--record dir [--record-from N]]", "Plays for some frames in a hidden window and saves an image (needs OpenGL). --record saves every frame of the game camera (with HUD) as frame_00000.png... for videos." },
    { "serve", 0, 0, "serve [folder] [--port 8080] [--open]", "Serves the browser editor (build/web) on http://localhost:8080 until Ctrl+C. Prints one JSON line when listening." },
    { "world", "city", c_world_city, "world city <project> [--scene Assets/Scenes/City.a3scene] [--seed 1] [--time day|sunset|night] [--rain 0.8] [--density 1] [--no-lights] [--no-neon] [--cars 40] [--pedestrians 60] [--no-traffic] [--startup] [--all]", "Generates Sol Harbor, a coastal city (towers, Art Deco beachfront, causeways, port) plus its road graph in Assets/City/roads.json." },
    { "mesh", "new", c_mesh_new, "mesh new <cube|plane|grid|cylinder|sphere|cone|torus> --out file.obj [--size 1] [--segments 16] [--rings 8] [--smooth]", "Creates a model with the modeling kernels." },
    { "mesh", "info", c_mesh_info, "mesh info <file.obj>", "Vertex, edge, face counts, bounds and whether the mesh is closed." },
    { "mesh", "edit", c_mesh_edit, "mesh edit <in.obj> --out <out.obj> --op <operation> [op options]...", "Applies modeling operations in order (see 'mesh ops')." },
    { "mesh", "ops", c_mesh_ops, "mesh ops", "Lists modeling operations and their options." },
    { "batch", 0, 0, "batch  (commands on stdin, one per line)", "Runs many commands in one process; prints one JSON line per command." },
};
static const u32 g_cmd_count = A3_ARRAY_COUNT(g_cmds);

static const Cmd *find_cmd(const char *name, const char *sub) {
    for (u32 i = 0; i < g_cmd_count; ++i) {
        if (!a3_streq(g_cmds[i].name, name)) continue;
        if (!g_cmds[i].sub) return &g_cmds[i];
        if (sub && a3_streq(g_cmds[i].sub, sub)) return &g_cmds[i];
    }
    return 0;
}

/* Runs argv[0..argc) as one command; prints its JSON. Returns success. */
static b32 run_command(int argc, char **argv) {
    out_begin();
    char label[64] = "";
    if (argc < 1) { fail("Run 'asm3d_cli help'.", "no command given"); out_end("", 0, 0); return 0; }
    const char *name = argv[0], *sub = argc > 1 ? argv[1] : 0;
    const Cmd *c = find_cmd(name, sub);
    Args a;
    b32 ok = 0;
    if (!c || !c->fn) {
        const char *cands[64];
        u32 n = 0;
        for (u32 i = 0; i < g_cmd_count; ++i) cands[n++] = g_cmds[i].name;
        char hint[160];
        a3s_suggest(name, cands, n, hint, sizeof(hint));
        b32 known = 0;
        for (u32 i = 0; i < g_cmd_count; ++i) if (a3_streq(g_cmds[i].name, name)) known = 1;
        if (known) fail("Run 'asm3d_cli help' for the subcommands.", "'%s' needs a subcommand", name);
        else fail(hint[0] ? hint : "Run 'asm3d_cli help' for every command.", "unknown command '%s'", name);
        a3_strcpy(label, sizeof(label), name);
        args_parse(&a, argc, argv, 1);
    } else {
        a3_snprintf(label, sizeof(label), "%s%s%s", c->name, c->sub ? " " : "", c->sub ? c->sub : "");
        args_parse(&a, argc, argv, c->sub ? 2 : 1);
        g_out.verbose = has_opt(&a, "verbose");
        g_out.capture_info = 0;
        ok = c->fn(&a) && !g_out.error[0];
    }
    out_end(label, ok, has_opt(&a, "pretty"));
    return ok;
}

/* Splits a batch line into arguments ("quoted text" and 'quoted' supported). */
static int split_line(char *line, char **argv, int cap) {
    int n = 0;
    char *p = line;
    while (*p && n < cap) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        if (!*p || *p == '#') break;
        char q = (*p == '"' || *p == '\'') ? *p++ : 0;
        char *start = p, *w = p;
        while (*p && (q ? *p != q : (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n'))) {
            if (q == '"' && *p == '\\' && p[1]) { ++p; *w++ = *p == 'n' ? '\n' : *p; ++p; continue; }
            *w++ = *p++;
        }
        if (*p) ++p;
        *w = 0;
        argv[n++] = start;
    }
    return n;
}

int main(int argc, char **argv) {
    a3_log_set_console(0);
    a3_log_add_sink(log_sink, 0);
    for (int i = 1; i < argc; ++i) if (a3_streq(argv[i], "--verbose")) g_out.verbose = 1;
    a3_platform_init();
    a3_engine_register_all();
    a3_strbuf_init(&g_out.body, A3_MEM_TEMP);
    if (argc < 2) {
        run_command(1, (char *[]){ "help", 0 });
        return 0;
    }
    if (a3_streq(argv[1], "serve")) {
        /* long-running: prints its JSON line when listening, then serves */
        Args sa;
        args_parse(&sa, argc, argv, 2);
        const char *port = opt(&sa, "port", "8080");
        return a3_cli_serve(arg(&sa, 0) ? arg(&sa, 0) : ".", (int)atoi(port), has_opt(&sa, "open"), atoi(opt(&sa, "max-requests", "0")));
    }
    if (a3_streq(argv[1], "batch")) {
        char line[8192];
        u32 failed = 0;
        while (fgets(line, sizeof(line), stdin)) {
            char *av[MAX_ARGS];
            int ac = split_line(line, av, MAX_ARGS);
            if (!ac) continue;
            if (!run_command(ac, av)) failed++;
        }
        return failed ? 1 : 0;
    }
    return run_command(argc - 1, argv + 1) ? 0 : 1;
}

/* ======================================================================== */
/* Modeling commands (engine/modeling)                                      */
/* ======================================================================== */


static void write_mesh_stats(A3EMesh *m, const char *key) {
    A3EMeshStats st;
    a3_emesh_stats(m, &st);
    a3_jw_key(R(), key);
    a3_jw_begin_object(R());
    a3_jw_kv_int(R(), "vertices", st.vertices);
    a3_jw_kv_int(R(), "edges", st.edges);
    a3_jw_kv_int(R(), "faces", st.faces);
    a3_jw_kv_int(R(), "triangles", st.triangles);
    a3_jw_kv_int(R(), "selected_vertices", st.selected_vertices);
    a3_jw_kv_int(R(), "selected_faces", st.selected_faces);
    a3_jw_kv_int(R(), "boundary_edges", st.boundary_edges);
    a3_jw_kv_int(R(), "nonmanifold_edges", st.nonmanifold_edges);
    a3_jw_kv_bool(R(), "closed", st.closed);
    a3_jw_kv_floats(R(), "min", &st.min.x, 3);
    a3_jw_kv_floats(R(), "max", &st.max.x, 3);
    a3_jw_end_object(R());
}

static b32 load_mesh(const char *path, A3EMesh *m) {
    A3FileData fd;
    if (!path) return fail("Give an .obj file.", "missing mesh file");
    if (!read_text(path, &fd)) return 0;
    char err[200];
    b32 ok = a3_emesh_load_obj(m, (const char *)fd.data, fd.size, err, sizeof(err));
    a3_free(fd.data);
    if (!ok) return fail("Only v and f lines are read (polygons are kept).", "cannot read %s: %s", path, err[0] ? err : "no vertices");
    return 1;
}

static b32 save_mesh(A3EMesh *m, const char *path) {
    if (!path) return fail("Pass --out file.obj.", "missing --out");
    A3StrBuf sb;
    a3_strbuf_init(&sb, A3_MEM_TEMP);
    char name[64];
    a3_path_stem(path, name, sizeof(name));
    a3_emesh_save_obj(m, &sb, name);
    char dir[PATHCAP];
    a3_path_dirname(path, dir, sizeof(dir));
    if (dir[0]) a3_dir_create(dir);
    A3Result r = a3_file_write_atomic(path, a3_strbuf_cstr(&sb), sb.len);
    a3_strbuf_free(&sb);
    if (r != A3_OK) return fail("Check that the folder is writable.", "cannot write %s", path);
    a3_jw_kv_string(R(), "file", path);
    return 1;
}


static b32 apply_op(A3EMesh *m, const char *spec) {
    A3EMeshOpError e;
    if (a3_emesh_run_op(m, spec, &e)) return 1;
    return fail(e.hint[0] ? e.hint : 0, "%s", e.message);
}

static b32 c_mesh_new(const Args *a) {
    const char *pn = arg(a, 0);
    i32 pi = a3_emesh_primitive_from_name(pn);
    if (pi < 0) return fail("Primitives: cube, plane, grid, cylinder, cone, sphere, torus.", "unknown primitive '%s'", pn ? pn : "");
    A3EMesh m;
    a3_emesh_init(&m);
    a3_emesh_make(&m, (A3EPrimitive)pi, (f32)atof(opt(a, "size", "1")), (u32)atoi(opt(a, "segments", "0")), (u32)atoi(opt(a, "rings", "0")));
    m.smooth = has_opt(a, "smooth") || a3_streq(opt(a, "shading", ""), "smooth");
    b32 ok = save_mesh(&m, opt(a, "out", 0));
    if (ok) write_mesh_stats(&m, "mesh");
    a3_emesh_free(&m);
    return ok;
}

static b32 c_mesh_info(const Args *a) {
    A3EMesh m;
    a3_emesh_init(&m);
    b32 ok = load_mesh(arg(a, 0), &m);
    if (ok) {
        a3_jw_kv_string(R(), "file", arg(a, 0));
        write_mesh_stats(&m, "mesh");
        u32 counts[5] = { 0 };
        for (u32 f = 0; f < a3_emesh_face_count(&m); ++f) counts[a3_mini((i32)m.fsize.data[f], 5) - 1]++;
        a3_jw_kv_int(R(), "triangle_faces", counts[2]);
        a3_jw_kv_int(R(), "quad_faces", counts[3]);
        a3_jw_kv_int(R(), "ngon_faces", counts[4]);
        a3_jw_kv_bool(R(), "smooth", m.smooth);
        a3_jw_kv_string(R(), "kernels", a3_mk_backend());
    }
    a3_emesh_free(&m);
    return ok;
}

static b32 c_mesh_edit(const Args *a) {
    A3EMesh m;
    a3_emesh_init(&m);
    if (!load_mesh(arg(a, 0), &m)) { a3_emesh_free(&m); return 0; }
    b32 ok = 1;
    u32 nops = 0;
    a3_jw_key(R(), "steps");
    a3_jw_begin_array(R());
    for (u32 i = 0; i < a->nopt && ok; ++i) {
        if (!a3_streq(a->key[i], "op") || !a->val[i]) continue;
        nops++;
        ok = apply_op(&m, a->val[i]);
        a3_jw_begin_object(R());
        a3_jw_kv_string(R(), "op", a->val[i]);
        a3_jw_kv_bool(R(), "ok", ok);
        if (ok) {
            A3EMeshStats st;
            a3_emesh_stats(&m, &st);
            a3_jw_kv_int(R(), "vertices", st.vertices);
            a3_jw_kv_int(R(), "faces", st.faces);
            a3_jw_kv_int(R(), "selected", a3_emesh_selected_count(&m));
        }
        a3_jw_end_object(R());
    }
    a3_jw_end_array(R());
    if (ok && !nops) ok = fail("Add operations with --op, e.g. --op select-normal:0,1,0 --op extrude:1. Run 'mesh ops' for the list.", "no --op given");
    if (ok) ok = save_mesh(&m, opt(a, "out", arg(a, 0)));
    if (ok) write_mesh_stats(&m, "mesh");
    a3_emesh_free(&m);
    return ok;
}

static b32 c_mesh_ops(const Args *a) {
    A3_UNUSED(a);
    a3_jw_key(R(), "operations");
    a3_jw_begin_array(R());
    for (u32 i = 0; i < a3_emesh_op_count(); ++i) {
        const A3EMeshOpInfo *oi = a3_emesh_op_info(i);
        a3_jw_begin_object(R());
        a3_jw_kv_string(R(), "op", oi->name);
        a3_jw_kv_string(R(), "arguments", oi->args);
        a3_jw_kv_string(R(), "description", oi->doc);
        a3_jw_end_object(R());
    }
    a3_jw_end_array(R());
    a3_jw_kv_string(R(), "syntax", "--op name:arg1,arg2 (repeat --op; applied in order)");
    a3_jw_kv_string(R(), "not_available", "bevel, knife, boolean operations, UV unwrapping, sculpting");
    return 1;
}
