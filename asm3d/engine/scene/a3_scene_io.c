/*
 * ASM3D - a3_scene_io.c
 */
#include "a3_scene_io.h"
#include "../core/a3_log.h"
#include "../core/a3_string.h"
#include "../core/a3_hash.h"
#include "../core/a3_format.h"
#include "../platform/a3_platform.h"

#define SCENE_FORMAT_ID "asm3d.scene"

void a3_guid_to_string(u64 guid, char out[17]) { a3_hash_to_hex(guid, out); }

u64 a3_guid_from_string(const char *s) {
    u64 v = 0;
    if (!s) return 0;
    for (int i = 0; i < 16 && s[i]; ++i) {
        char c = s[i];
        int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) return 0;
        v = (v << 4) | (u64)d;
    }
    return v;
}

/* ======================================================================== */
/* Fields                                                                   */
/* ======================================================================== */

void a3_field_write_json(A3JsonWriter *jw, const A3FieldDesc *f, const void *component) {
    const u8 *p = (const u8 *)component + f->offset;
    a3_jw_key(jw, f->name);
    switch (f->type) {
    case A3_FIELD_BOOL: { b32 v; a3_memcpy(&v, p, 4); a3_jw_bool(jw, v); } break;
    case A3_FIELD_I32: { i32 v; a3_memcpy(&v, p, 4); a3_jw_int(jw, v); } break;
    case A3_FIELD_U32: { u32 v; a3_memcpy(&v, p, 4); a3_jw_int(jw, v); } break;
    case A3_FIELD_F32: { f32 v; a3_memcpy(&v, p, 4); a3_jw_number(jw, v); } break;
    case A3_FIELD_VEC2: a3_jw_floats(jw, (const f32 *)p, 2); break;
    case A3_FIELD_VEC3: a3_jw_floats(jw, (const f32 *)p, 3); break;
    case A3_FIELD_VEC4: case A3_FIELD_COLOR: a3_jw_floats(jw, (const f32 *)p, 4); break;
    case A3_FIELD_QUAT: {
        /* Stored as euler degrees: readable and diff-friendly. */
        A3Quat q; a3_memcpy(&q, p, sizeof(q));
        A3Vec3 e = a3_quat_to_euler(q);
        f32 deg[3] = { e.x * A3_RAD2DEG, e.y * A3_RAD2DEG, e.z * A3_RAD2DEG };
        for (int i = 0; i < 3; ++i) {  /* tidy tiny float noise */
            f32 r = a3_roundf(deg[i] * 10000.0f) / 10000.0f;
            deg[i] = (r == 0.0f) ? 0.0f : r;
        }
        a3_jw_floats(jw, deg, 3);
    } break;
    case A3_FIELD_STRING: a3_jw_string(jw, (const char *)p); break;
    case A3_FIELD_ENUM: {
        i32 v; a3_memcpy(&v, p, 4);
        if (f->enum_names && v >= 0 && (u32)v < f->enum_count) a3_jw_string(jw, f->enum_names[v]);
        else a3_jw_int(jw, v);
    } break;
    case A3_FIELD_ASSET: a3_jw_string(jw, ((const A3AssetRef *)p)->path); break;
    case A3_FIELD_ENTITY: {
        const A3EntityRef *r = (const A3EntityRef *)p;
        if (r->guid) { char g[17]; a3_guid_to_string(r->guid, g); a3_jw_string(jw, g); }
        else a3_jw_null(jw);
    } break;
    default: a3_jw_null(jw); break;
    }
}

b32 a3_field_read_json(const A3Json *v, const A3FieldDesc *f, void *component) {
    u8 *p = (u8 *)component + f->offset;
    if (!v) return 0;
    switch (f->type) {
    case A3_FIELD_BOOL: { b32 b = a3_json_bool(v, 0); a3_memcpy(p, &b, 4); } break;
    case A3_FIELD_I32: { i32 i = (i32)a3_json_number(v, 0); a3_memcpy(p, &i, 4); } break;
    case A3_FIELD_U32: { u32 i = (u32)a3_json_number(v, 0); a3_memcpy(p, &i, 4); } break;
    case A3_FIELD_F32: { f32 x = (f32)a3_json_number(v, 0); a3_memcpy(p, &x, 4); } break;
    case A3_FIELD_VEC2: a3_json_get_floats(v, (f32 *)p, 2); break;
    case A3_FIELD_VEC3: a3_json_get_floats(v, (f32 *)p, 3); break;
    case A3_FIELD_VEC4: case A3_FIELD_COLOR: a3_json_get_floats(v, (f32 *)p, 4); break;
    case A3_FIELD_QUAT: {
        f32 d[4] = { 0, 0, 0, 0 };
        u32 n = a3_json_get_floats(v, d, 4);
        A3Quat q = (n == 4) ? a3_quat_normalize(a3_quat(d[0], d[1], d[2], d[3]))   /* raw quaternion */
                            : a3_quat_euler(d[0] * A3_DEG2RAD, d[1] * A3_DEG2RAD, d[2] * A3_DEG2RAD);
        a3_memcpy(p, &q, sizeof(q));
    } break;
    case A3_FIELD_STRING: a3_strcpy((char *)p, A3_NAME_MAX, a3_json_string(v, "")); break;
    case A3_FIELD_ENUM: {
        i32 i = 0;
        if (v->type == A3_JSON_STRING && f->enum_names) {
            b32 found = 0;
            for (u32 k = 0; k < f->enum_count; ++k) if (a3_streq(f->enum_names[k], v->v.string)) { i = (i32)k; found = 1; break; }
            if (!found) { A3_WARN("scene", "unknown value '%s' for field '%s'; using default", v->v.string, f->name); return 0; }
        } else {
            i = (i32)a3_json_number(v, 0);
        }
        a3_memcpy(p, &i, 4);
    } break;
    case A3_FIELD_ASSET: {
        A3AssetRef *r = (A3AssetRef *)p;
        a3_strcpy(r->path, sizeof(r->path), a3_json_string(v, ""));
        r->handle = 0;
    } break;
    case A3_FIELD_ENTITY: {
        A3EntityRef *r = (A3EntityRef *)p;
        r->guid = a3_guid_from_string(a3_json_string(v, 0));
        r->cached_index = 0xFFFFFFFFu;
        r->cached_gen = 0;
    } break;
    default: return 0;
    }
    return 1;
}

void a3_component_write_json(A3JsonWriter *jw, const A3ComponentType *t, const void *data) {
    a3_jw_begin_object(jw);
    for (u32 i = 0; i < t->field_count; ++i) {
        if (t->fields[i].flags & A3_FIELD_FLAG_TRANSIENT) continue;
        a3_field_write_json(jw, &t->fields[i], data);
    }
    a3_jw_end_object(jw);
}

u32 a3_component_read_json(const A3Json *obj, const A3ComponentType *t, void *data) {
    u32 missing = 0;
    for (u32 i = 0; i < t->field_count; ++i) {
        const A3FieldDesc *f = &t->fields[i];
        if (f->flags & A3_FIELD_FLAG_TRANSIENT) continue;
        const A3Json *v = a3_json_get(obj, f->name);
        if (!v || !a3_field_read_json(v, f, data)) missing++;
    }
    return missing;
}

/* ======================================================================== */
/* Save                                                                     */
/* ======================================================================== */

typedef struct SaveCtx {
    A3JsonWriter *jw;
    u32 flags;
} SaveCtx;

static b32 entity_saveable(A3World *w, A3Entity e, u32 flags) {
    const A3EntityRecord *r = &w->entities[e.index];
    if ((r->flags & A3_ENTITY_DONT_SAVE) && !(flags & A3_SCENE_SAVE_INCLUDE_RUNTIME)) return 0;
    if ((r->flags & A3_ENTITY_EDITOR_ONLY) && (flags & A3_SCENE_SAVE_FOR_BUILD)) return 0;
    if (r->flags & A3_ENTITY_PENDING_DESTROY) return 0;
    return 1;
}

static void write_entity(A3World *w, A3Entity e, A3JsonWriter *jw, u32 flags, b32 write_parent) {
    const A3EntityRecord *r = &w->entities[e.index];
    char g[17];
    a3_jw_begin_object(jw);
    a3_guid_to_string(r->guid, g);
    a3_jw_kv_string(jw, "guid", g);
    a3_jw_kv_string(jw, "name", r->name);
    if (write_parent && a3_entity_valid(w, r->parent)) {
        a3_guid_to_string(w->entities[r->parent.index].guid, g);
        a3_jw_kv_string(jw, "parent", g);
    }
    if (!(r->flags & A3_ENTITY_ACTIVE)) a3_jw_kv_bool(jw, "active", 0);
    if (r->flags & A3_ENTITY_STATIC) a3_jw_kv_bool(jw, "static", 1);
    if ((r->flags & A3_ENTITY_EDITOR_ONLY)) a3_jw_kv_bool(jw, "editorOnly", 1);
    if (r->prefab[0]) a3_jw_kv_string(jw, "prefab", r->prefab);
    a3_jw_key(jw, "components");
    a3_jw_begin_object(jw);
    for (u32 t = 0; t < a3_component_type_count(); ++t) {
        const void *data = a3_component_get(w, e, t);
        if (!data) continue;
        const A3ComponentType *ct = a3_component_type(t);
        if ((ct->flags & A3_COMP_EDITOR_ONLY) && (flags & A3_SCENE_SAVE_FOR_BUILD)) continue;
        a3_jw_key(jw, ct->name);
        a3_component_write_json(jw, ct, data);
    }
    /* preserved components from plugins that are not loaded */
    if (r->unknown_components && r->unknown_components[0]) {
        A3Arena arena;
        a3_arena_init(&arena, A3_MEM_TEMP, A3_KB(16));
        A3Json *unk = a3_json_parse(r->unknown_components, a3_strlen(r->unknown_components), &arena, 0);
        A3_JSON_FOREACH(c, unk) { a3_jw_key(jw, c->key); a3_jw_node(jw, c); }
        a3_arena_release(&arena);
    }
    a3_jw_end_object(jw);
    a3_jw_end_object(jw);
}

static b32 save_visit(A3World *w, A3Entity e, u32 depth, void *user) {
    SaveCtx *c = (SaveCtx *)user;
    A3_UNUSED(depth);
    if (!entity_saveable(w, e, c->flags)) return 0; /* skip subtree */
    write_entity(w, e, c->jw, c->flags, 1);
    return 1;
}

A3Result a3_scene_save_json(A3World *w, A3StrBuf *out, u32 flags) {
    if (!A3_VERIFY(w && out)) return A3_ERR_INVALID_ARG;
    A3JsonWriter jw;
    a3_jw_init(&jw, out, (flags & A3_SCENE_SAVE_COMPACT) != 0);
    a3_jw_begin_object(&jw);
    a3_jw_kv_string(&jw, "format", SCENE_FORMAT_ID);
    a3_jw_kv_int(&jw, "version", A3_FORMAT_VERSION);
    a3_jw_kv_string(&jw, "engine", A3_VERSION_STRING);
    a3_jw_kv_string(&jw, "name", w->name);
    a3_jw_key(&jw, "entities");
    a3_jw_begin_array(&jw);
    SaveCtx ctx = { &jw, flags };
    a3_world_visit_hierarchy(w, save_visit, &ctx);
    a3_jw_end_array(&jw);
    a3_jw_end_object(&jw);
    a3_strbuf_append_char(out, '\n');
    return out->failed ? A3_ERR_OUT_OF_MEMORY : A3_OK;
}

A3Result a3_scene_save_file(A3World *w, const char *path, u32 flags) {
    A3StrBuf sb;
    a3_strbuf_init(&sb, A3_MEM_SCENE);
    A3Result r = a3_scene_save_json(w, &sb, flags);
    if (r == A3_OK) r = a3_file_write_atomic(path, sb.data, sb.len);
    if (r != A3_OK) A3_ERROR("scene", "failed to save scene '%s': %s", path, a3_result_str(r));
    a3_strbuf_free(&sb);
    return r;
}

/* ======================================================================== */
/* Load                                                                     */
/* ======================================================================== */

static void report_warn(A3SceneLoadReport *rep, const char *fmt, ...) {
    char msg[160];
    va_list a;
    va_start(a, fmt);
    a3_vsnprintf(msg, sizeof(msg), fmt, a);
    va_end(a);
    A3_WARN("scene", "%s", msg);
    if (rep && rep->warning_count < A3_SCENE_MAX_WARNINGS) a3_strcpy(rep->warnings[rep->warning_count++], 160, msg);
}

/* Migration steps: migrate_steps[v] upgrades a document from version v to v+1.
 * Format version 1 is the first public version, so none exist yet; adding a
 * format change means bumping A3_FORMAT_VERSION and appending a step here. */
typedef b32 (*MigrateFn)(A3Json *root, A3Arena *arena);
static const MigrateFn migrate_steps[1] = { 0 };

static void load_components(A3World *w, A3Entity e, const A3Json *comps, A3SceneLoadReport *rep) {
    A3StrBuf unknown;
    a3_strbuf_init(&unknown, A3_MEM_ECS);
    A3JsonWriter uw;
    a3_jw_init(&uw, &unknown, 1);
    b32 any_unknown = 0;
    A3_JSON_FOREACH(c, comps) {
        if (c->type != A3_JSON_OBJECT || !c->key) continue;
        A3ComponentType *ct = a3_component_type_by_name(c->key);
        if (!ct) {
            if (!any_unknown) { a3_jw_begin_object(&uw); any_unknown = 1; }
            a3_jw_key(&uw, c->key);
            a3_jw_node(&uw, c);
            if (rep) rep->unknown_components++;
            report_warn(rep, "'%s' uses component '%s', which is not available (missing plugin?). It is kept unchanged.",
                        a3_entity_name(w, e), c->key);
            continue;
        }
        void *data = a3_component_add(w, e, ct->id);
        if (!data) continue;
        u32 missing = a3_component_read_json(c, ct, data);
        if (rep) { rep->components_loaded++; rep->missing_fields += missing; }
    }
    if (any_unknown) {
        a3_jw_end_object(&uw);
        A3EntityRecord *r = a3_entity_record(w, e);
        if (r) { r->unknown_components = unknown.data; unknown.data = 0; }
    }
    a3_strbuf_free(&unknown);
}

static A3Result scene_load_json_impl(A3World *w, const char *text, usize len, A3SceneLoadReport *rep) {
    A3SceneLoadReport local;
    if (!rep) rep = &local;
    a3_zero_struct(rep);
    if (!A3_VERIFY(w && text)) return A3_ERR_INVALID_ARG;
    A3Arena arena;
    a3_arena_init(&arena, A3_MEM_TEMP, A3_KB(64));
    A3JsonError jerr;
    A3Json *root = a3_json_parse(text, len, &arena, &jerr);
    if (!root || root->type != A3_JSON_OBJECT) {
        if (root) a3_strcpy(jerr.message, sizeof(jerr.message), "top level is not an object");
        a3_snprintf(rep->error, sizeof(rep->error), "JSON parse error at line %d, column %d: %s", jerr.line, jerr.column, jerr.message);
        a3_snprintf(rep->hint, sizeof(rep->hint), "The scene file is damaged or was edited by hand with a mistake near line %d. "
                    "Open it in the code editor to fix it, or restore it from Backups.", jerr.line);
        A3_ERROR("scene", "%s", rep->error);
        a3_arena_release(&arena);
        return A3_ERR_PARSE;
    }
    const char *fmt = a3_json_get_string(root, "format", "");
    if (!a3_streq(fmt, SCENE_FORMAT_ID)) {
        a3_snprintf(rep->error, sizeof(rep->error), "not a scene file (format '%s')", fmt);
        a3_strcpy(rep->hint, sizeof(rep->hint), "This file is not an ASM3D scene.");
        a3_arena_release(&arena);
        return A3_ERR_PARSE;
    }
    int version = (int)a3_json_get_number(root, "version", 0);
    if (version > A3_FORMAT_VERSION) {
        a3_snprintf(rep->error, sizeof(rep->error), "scene format version %d is newer than this engine supports (%d)", version, A3_FORMAT_VERSION);
        a3_snprintf(rep->hint, sizeof(rep->hint), "This scene was saved by a newer ASM3D (%s). Update ASM3D to open it; "
                    "it was not modified.", a3_json_get_string(root, "engine", "?"));
        A3_ERROR("scene", "%s", rep->error);
        a3_arena_release(&arena);
        return A3_ERR_VERSION;
    }
    if (version < 1) version = 1;
    if (version < A3_FORMAT_VERSION) rep->migrated_from = (u32)version;
    for (int v = version; v < A3_FORMAT_VERSION; ++v) {
        if ((u32)v < A3_ARRAY_COUNT(migrate_steps) && migrate_steps[v] && !migrate_steps[v](root, &arena)) {
            a3_snprintf(rep->error, sizeof(rep->error), "migration from format %d failed", v);
            a3_arena_release(&arena);
            return A3_ERR_VERSION;
        }
    }
    const char *scene_name = a3_json_get_string(root, "name", 0);
    if (scene_name && w->entity_count == 0) a3_strcpy(w->name, sizeof(w->name), scene_name);

    const A3Json *ents = a3_json_get(root, "entities");
    u32 n = a3_json_count(ents);
    A3Entity *created = n ? A3_NEW_ARRAY(A3Entity, n, A3_MEM_TEMP) : 0;
    if (n && !created) { a3_arena_release(&arena); return A3_ERR_OUT_OF_MEMORY; }
    /* pass 1: create entities with their GUIDs */
    u32 i = 0;
    A3_JSON_FOREACH(je, ents) {
        u64 guid = a3_guid_from_string(a3_json_get_string(je, "guid", 0));
        if (guid && a3_entity_valid(w, a3_entity_find_by_guid(w, guid))) {
            report_warn(rep, "duplicate entity id %s; a new id was assigned", a3_json_get_string(je, "guid", "?"));
            guid = 0;
        }
        A3Entity e = a3_entity_create_with_guid(w, a3_json_get_string(je, "name", "Entity"), guid);
        created[i++] = e;
        A3EntityRecord *r = a3_entity_record(w, e);
        if (!r) continue;
        if (!a3_json_get_bool(je, "active", 1)) r->flags &= ~(u32)A3_ENTITY_ACTIVE;
        if (a3_json_get_bool(je, "static", 0)) r->flags |= A3_ENTITY_STATIC;
        if (a3_json_get_bool(je, "editorOnly", 0)) r->flags |= A3_ENTITY_EDITOR_ONLY;
        a3_strcpy(r->prefab, sizeof(r->prefab), a3_json_get_string(je, "prefab", ""));
        rep->entities_loaded++;
    }
    /* pass 2: hierarchy and components */
    i = 0;
    A3_JSON_FOREACH(je, ents) {
        A3Entity e = created[i++];
        if (!a3_entity_valid(w, e)) continue;
        const char *pg = a3_json_get_string(je, "parent", 0);
        if (pg) {
            A3Entity p = a3_entity_find_by_guid(w, a3_guid_from_string(pg));
            if (a3_entity_valid(w, p)) a3_entity_set_parent(w, e, p);
            else report_warn(rep, "'%s' refers to a missing parent; it was placed at the top level", a3_entity_name(w, e));
        }
        load_components(w, e, a3_json_get(je, "components"), rep);
    }
    a3_free(created);
    a3_arena_release(&arena);
    A3_INFO("scene", "loaded scene '%s': %u entities, %u components%s", w->name, rep->entities_loaded, rep->components_loaded,
            rep->unknown_components ? " (some components preserved from missing plugins)" : "");
    return A3_OK;
}

A3Result a3_scene_load_file(A3World *w, const char *path, A3SceneLoadReport *rep) {
    A3FileData fd;
    A3Result r = a3_file_read_all(path, A3_MEM_SCENE, &fd);
    if (r != A3_OK) {
        if (rep) {
            a3_zero_struct(rep);
            a3_snprintf(rep->error, sizeof(rep->error), "cannot read '%s': %s", path, a3_result_str(r));
            a3_snprintf(rep->hint, sizeof(rep->hint), "The scene file could not be opened. It may have been moved, renamed or deleted.");
        }
        A3_ERROR("scene", "cannot read scene '%s': %s", path, a3_result_str(r));
        return r;
    }
    r = a3_scene_load_json(w, (const char *)fd.data, fd.size, rep);
    a3_free(fd.data);
    return r;
}

/* ======================================================================== */
/* Prefab / clipboard                                                       */
/* ======================================================================== */

typedef struct SubtreeCtx { A3JsonWriter *jw; A3Entity root; b32 first; } SubtreeCtx;

static void write_subtree(A3World *w, A3Entity e, A3JsonWriter *jw, b32 is_root) {
    write_entity(w, e, jw, A3_SCENE_SAVE_DEFAULT, !is_root);
    for (A3Entity c = a3_entity_first_child(w, e); a3_entity_valid(w, c); c = a3_entity_next_sibling(w, c)) write_subtree(w, c, jw, 0);
}

A3Result a3_entities_save_json(A3World *w, const A3Entity *roots, u32 count, A3StrBuf *out) {
    A3JsonWriter jw;
    a3_jw_init(&jw, out, 0);
    a3_jw_begin_object(&jw);
    a3_jw_kv_string(&jw, "format", "asm3d.entities");
    a3_jw_kv_int(&jw, "version", A3_FORMAT_VERSION);
    a3_jw_key(&jw, "entities");
    a3_jw_begin_array(&jw);
    for (u32 i = 0; i < count; ++i) if (a3_entity_valid(w, roots[i])) write_subtree(w, roots[i], &jw, 1);
    a3_jw_end_array(&jw);
    a3_jw_end_object(&jw);
    return out->failed ? A3_ERR_OUT_OF_MEMORY : A3_OK;
}

static void write_subtree_keep_parent(A3World *w, A3Entity e, A3JsonWriter *jw) {
    write_entity(w, e, jw, A3_SCENE_SAVE_INCLUDE_RUNTIME, 1);
    for (A3Entity c = a3_entity_first_child(w, e); a3_entity_valid(w, c); c = a3_entity_next_sibling(w, c)) write_subtree_keep_parent(w, c, jw);
}

A3Result a3_scene_save_subtree_json(A3World *w, A3Entity root, A3StrBuf *out) {
    if (!a3_entity_valid(w, root)) return A3_ERR_INVALID_ARG;
    A3JsonWriter jw;
    a3_jw_init(&jw, out, 1);
    a3_jw_begin_object(&jw);
    a3_jw_kv_string(&jw, "format", SCENE_FORMAT_ID);
    a3_jw_kv_int(&jw, "version", A3_FORMAT_VERSION);
    /* position among siblings so undo restores the hierarchy order */
    u32 index = 0;
    A3Entity parent = a3_entity_parent(w, root);
    A3Entity it = a3_entity_valid(w, parent) ? a3_entity_first_child(w, parent) : w->first_root;
    while (a3_entity_valid(w, it) && !a3_entity_eq(it, root)) { ++index; it = a3_entity_next_sibling(w, it); }
    a3_jw_kv_int(&jw, "siblingIndex", index);
    a3_jw_key(&jw, "entities");
    a3_jw_begin_array(&jw);
    write_subtree_keep_parent(w, root, &jw);
    a3_jw_end_array(&jw);
    a3_jw_end_object(&jw);
    return out->failed ? A3_ERR_OUT_OF_MEMORY : A3_OK;
}

static void remap_entity_refs(A3World *w, A3Entity e, const u64 *old_ids, const u64 *new_ids, u32 n) {
    for (u32 t = 0; t < a3_component_type_count(); ++t) {
        u8 *data = (u8 *)a3_component_get(w, e, t);
        if (!data) continue;
        const A3ComponentType *ct = a3_component_type(t);
        for (u32 f = 0; f < ct->field_count; ++f) {
            if (ct->fields[f].type != A3_FIELD_ENTITY) continue;
            A3EntityRef *r = (A3EntityRef *)(data + ct->fields[f].offset);
            for (u32 k = 0; k < n; ++k) if (r->guid == old_ids[k]) { r->guid = new_ids[k]; r->cached_index = 0xFFFFFFFFu; break; }
        }
    }
}

static u32 entities_load_json_impl(A3World *w, const char *text, usize len, A3Entity parent, A3Entity *out_roots, u32 max_roots) {
    A3Arena arena;
    a3_arena_init(&arena, A3_MEM_TEMP, A3_KB(64));
    A3JsonError jerr;
    A3Json *root = a3_json_parse(text, len, &arena, &jerr);
    const A3Json *ents = a3_json_get(root, "entities");
    u32 n = a3_json_count(ents), roots = 0;
    if (!n) { a3_arena_release(&arena); return 0; }
    u64 *old_ids = A3_NEW_ARRAY(u64, n, A3_MEM_TEMP);
    u64 *new_ids = A3_NEW_ARRAY(u64, n, A3_MEM_TEMP);
    A3Entity *created = A3_NEW_ARRAY(A3Entity, n, A3_MEM_TEMP);
    if (!old_ids || !new_ids || !created) { a3_free(old_ids); a3_free(new_ids); a3_free(created); a3_arena_release(&arena); return 0; }
    u32 i = 0;
    A3_JSON_FOREACH(je, ents) {
        old_ids[i] = a3_guid_from_string(a3_json_get_string(je, "guid", 0));
        created[i] = a3_entity_create(w, a3_json_get_string(je, "name", "Entity"));
        new_ids[i] = a3_entity_guid(w, created[i]);
        ++i;
    }
    i = 0;
    A3_JSON_FOREACH(je, ents) {
        A3Entity e = created[i++];
        const char *pg = a3_json_get_string(je, "parent", 0);
        A3Entity p = A3_ENTITY_NULL;
        if (pg) {
            u64 old = a3_guid_from_string(pg);
            for (u32 k = 0; k < n; ++k) if (old_ids[k] == old) { p = created[k]; break; }
        }
        if (a3_entity_valid(w, p)) a3_entity_set_parent(w, e, p);
        else {
            if (a3_entity_valid(w, parent)) a3_entity_set_parent(w, e, parent);
            if (out_roots && roots < max_roots) out_roots[roots] = e;
            roots++;
        }
        if (!a3_json_get_bool(je, "active", 1)) a3_entity_set_active(w, e, 0);
        load_components(w, e, a3_json_get(je, "components"), 0);
    }
    for (u32 k = 0; k < n; ++k) remap_entity_refs(w, created[k], old_ids, new_ids, n);
    a3_free(old_ids); a3_free(new_ids); a3_free(created);
    a3_arena_release(&arena);
    return roots;
}

/* Hooks such as "Character Controller adds a camera child" must not run while
 * loading: the file already contains the result of those hooks. */
A3Result a3_scene_load_json(A3World *w, const char *text, usize len, A3SceneLoadReport *rep) {
    if (!w) return A3_ERR_INVALID_ARG;
    w->loading++;
    A3Result r = scene_load_json_impl(w, text, len, rep);
    w->loading--;
    return r;
}

u32 a3_entities_load_json(A3World *w, const char *text, usize len, A3Entity parent, A3Entity *out_roots, u32 max_roots) {
    if (!w) return 0;
    w->loading++;
    u32 n = entities_load_json_impl(w, text, len, parent, out_roots, max_roots);
    w->loading--;
    return n;
}
