/*
 * ASM3D - a3_script_engine.c
 * Script component, object/component bindings and the game API for A3Script.
 */
#include "../physics/a3_vehicle.h"
#include "../world/a3_traffic.h"
#include "../world/a3_citygen.h"
#include "a3_script_engine.h"
#include "a3_script_internal.h"
#include "../scene/a3_components.h"
#include "../physics/a3_physics.h"
#include "../audio/a3_audio.h"
#include "../particles/a3_particles.h"
#include "../anim/a3_anim.h"
#include "../resource/a3_assets.h"
#include "../platform/a3_platform.h"
#include "../core/a3_string.h"
#include "../core/a3_format.h"
#include "../core/a3_strbuf.h"
#include "../core/a3_hash.h"
#include "../core/a3_log.h"

u32 A3_T_SCRIPT = 0xFFFFFFFFu;

/* ======================================================================== */
/* State                                                                    */
/* ======================================================================== */

typedef struct ModEntry {
    char path[A3_PATH_MAX];
    A3SModule *module;        /* NULL when the file failed to load/compile */
    u32 version;
} ModEntry;

typedef struct Inst {
    A3World *world;
    u64 guid;
    A3Entity entity;
    char path[A3_PATH_MAX];
    u32 version;              /* module version it was created from */
    A3SInstance *inst;
    b32 started;
    b32 failed;               /* stopped after an error, until the file changes */
    b32 seen;
} Inst;

typedef struct WorldState {
    A3World *world;
    A3_ARRAY_TYPE(A3HudCmd) hud;
    char scene_request[A3_PATH_MAX];
    const A3ScriptFrame *frame;
    A3ScriptFrame frame_copy;
} WorldState;

typedef struct SourceOverride { char path[A3_PATH_MAX]; char *source; } SourceOverride;

static struct {
    A3_ARRAY_TYPE(SourceOverride) sources;
    A3_ARRAY_TYPE(ModEntry) mods;
    A3_ARRAY_TYPE(Inst *) insts;
    A3_ARRAY_TYPE(WorldState *) worlds;
    A3ScriptErrorInfo errors[64];
    u32 error_count;
    u32 version_counter;
} g_se;

static WorldState *world_state(A3World *w, b32 create) {
    for (u32 i = 0; i < g_se.worlds.count; ++i) if (g_se.worlds.data[i]->world == w) return g_se.worlds.data[i];
    if (!create) return 0;
    WorldState *ws = A3_NEW(WorldState, A3_MEM_SCRIPT);
    if (!ws) return 0;
    ws->world = w;
    if (!a3_array_push(g_se.worlds, ws, A3_MEM_SCRIPT)) { a3_free(ws); return 0; }
    return ws;
}

static A3World *vm_world(A3SVM *vm) {
    Inst *in = (Inst *)a3s_instance_user(a3s_vm_instance(vm));
    return in ? in->world : 0;
}

static Inst *find_inst(A3World *w, u64 guid) {
    for (u32 i = 0; i < g_se.insts.count; ++i) {
        Inst *in = g_se.insts.data[i];
        if (in->world == w && in->guid == guid) return in;
    }
    return 0;
}

/* guid -> entity with a small cache (a3_entity_find_by_guid scans the world) */
typedef struct GuidCache { const A3World *w; u64 guid; A3Entity e; } GuidCache;
static GuidCache g_guid_cache[256];

static A3Entity resolve(A3World *w, u64 guid) {
    if (!w || !guid) return A3_ENTITY_NULL;
    GuidCache *c = &g_guid_cache[(guid ^ (guid >> 17)) & 255];
    if (c->w == w && c->guid == guid && a3_entity_valid(w, c->e) && a3_entity_guid(w, c->e) == guid) return c->e;
    A3Entity e = a3_entity_find_by_guid(w, guid);
    if (!a3_entity_is_null(e)) { c->w = w; c->guid = guid; c->e = e; }
    return e;
}

static void record_error(const char *path, const char *object, const A3SError *err) {
    if (g_se.error_count == A3_ARRAY_COUNT(g_se.errors)) {
        for (u32 i = 1; i < g_se.error_count; ++i) g_se.errors[i - 1] = g_se.errors[i];
        g_se.error_count--;
    }
    A3ScriptErrorInfo *e = &g_se.errors[g_se.error_count++];
    a3_strcpy(e->path, sizeof(e->path), path);
    a3_strcpy(e->object, sizeof(e->object), object ? object : "");
    e->error = *err;
    char where[160];
    if (err->function[0] && err->function[0] != '(') a3_snprintf(where, sizeof(where), "%s line %d, in %s()", path, err->line, err->function);
    else a3_snprintf(where, sizeof(where), "%s line %d", path, err->line);
    if (object && object[0]) a3_log_hint(A3_LOG_ERROR, "script", err->hint[0] ? err->hint : 0, "%s [%s]: %s", where, object, err->message);
    else a3_log_hint(A3_LOG_ERROR, "script", err->hint[0] ? err->hint : 0, "%s: %s", where, err->message);
}

u32 a3_scripts_error_count(void) { return g_se.error_count; }
const A3ScriptErrorInfo *a3_scripts_error(u32 i) { return i < g_se.error_count ? &g_se.errors[i] : 0; }
void a3_scripts_clear_errors(void) { g_se.error_count = 0; }

/* ======================================================================== */
/* Modules                                                                  */
/* ======================================================================== */

static ModEntry *module_get(const char *path) {
    for (u32 i = 0; i < g_se.mods.count; ++i) if (a3_streq(g_se.mods.data[i].path, path)) return &g_se.mods.data[i];
    ModEntry me;
    a3_zero_struct(&me);
    a3_strcpy(me.path, sizeof(me.path), path);
    me.version = ++g_se.version_counter;
    char abs[A3_PATH_MAX * 2];
    a3_assets_path(path, abs, sizeof(abs));
    A3FileData fd;
    A3SError err;
    a3_zero_struct(&err);
    const char *override = 0;
    for (u32 i = 0; i < g_se.sources.count; ++i) if (a3_streq(g_se.sources.data[i].path, path)) override = g_se.sources.data[i].source;
    if (override) {
        me.module = a3s_compile(path, override, a3_strlen(override), &err);
        if (!me.module) record_error(path, 0, &err);
    } else if (a3_file_read_all(abs, A3_MEM_TEMP, &fd) != A3_OK) {
        a3_strcpy(err.message, sizeof(err.message), "the script file was not found");
        a3_strcpy(err.hint, sizeof(err.hint), "Check the Script field of the Script component (it should point to a .a3script file).");
        record_error(path, 0, &err);
    } else {
        me.module = a3s_compile(path, (const char *)fd.data, fd.size, &err);
        a3_free(fd.data);
        if (!me.module) record_error(path, 0, &err);
    }
    if (!a3_array_push(g_se.mods, me, A3_MEM_SCRIPT)) { a3s_module_release(me.module); return 0; }
    return &a3_array_last(g_se.mods);
}

void a3_scripts_invalidate(const char *path) {
    for (u32 i = 0; i < g_se.mods.count; ++i) {
        if (!a3_streq(g_se.mods.data[i].path, path)) continue;
        a3s_module_release(g_se.mods.data[i].module);
        a3_array_remove_swap(g_se.mods, i);
        break;
    }
    /* instances notice the new version on their next update */
    for (u32 i = 0; i < g_se.insts.count; ++i) if (a3_streq(g_se.insts.data[i]->path, path)) g_se.insts.data[i]->failed = 0;
}

void a3_scripts_set_source(const char *path, const char *source) {
    if (!path) return;
    for (u32 i = 0; i < g_se.sources.count; ++i) {
        if (!a3_streq(g_se.sources.data[i].path, path)) continue;
        a3_free(g_se.sources.data[i].source);
        a3_array_remove_swap(g_se.sources, i);
        break;
    }
    if (source) {
        SourceOverride so;
        a3_zero_struct(&so);
        a3_strcpy(so.path, sizeof(so.path), path);
        usize n = a3_strlen(source);
        so.source = (char *)a3_malloc(n + 1, A3_MEM_SCRIPT);
        if (so.source) {
            a3_memcpy(so.source, source, n + 1);
            if (!a3_array_push(g_se.sources, so, A3_MEM_SCRIPT)) a3_free(so.source);
        }
    }
    a3_scripts_invalidate(path);
}

b32 a3_scripts_check(const char *name, const char *source, usize len, A3SError *err) {
    A3SModule *m = a3s_compile(name, source, len, err);
    if (!m) return 0;
    a3s_module_release(m);
    return 1;
}

/* ======================================================================== */
/* Value conversion for component fields                                    */
/* ======================================================================== */

static int strcmp_nocase(const char *a, const char *b) {
    for (;; ++a, ++b) {
        int ca = a3_to_lower((unsigned char)*a), cb = a3_to_lower((unsigned char)*b);
        if (ca != cb || !ca) return ca - cb;
    }
}

#define RAD2DEG 57.29577951308232f
#define DEG2RAD 0.017453292519943295f

static A3Vec3 quat_to_degrees(A3Quat q) { A3Vec3 e = a3_quat_to_euler(q); return a3_v3(e.x * RAD2DEG, e.y * RAD2DEG, e.z * RAD2DEG); }
static A3Quat degrees_to_quat(A3Vec3 d) { return a3_quat_euler(d.x * DEG2RAD, d.y * DEG2RAD, d.z * DEG2RAD); }

static A3SValue read_field(A3World *w, const A3FieldDesc *f, const u8 *p) {
    switch (f->type) {
    case A3_FIELD_BOOL: return a3s_bool(*(const b32 *)p);
    case A3_FIELD_I32: return a3s_num(*(const i32 *)p);
    case A3_FIELD_U32: return a3s_num(*(const u32 *)p);
    case A3_FIELD_F32: return a3s_num(*(const f32 *)p);
    case A3_FIELD_VEC2: { const f32 *v = (const f32 *)p; return a3s_vec3(v[0], v[1], 0); }
    case A3_FIELD_VEC3: case A3_FIELD_VEC4: case A3_FIELD_COLOR: { const f32 *v = (const f32 *)p; return a3s_vec3(v[0], v[1], v[2]); }
    case A3_FIELD_QUAT: { A3Vec3 d = quat_to_degrees(*(const A3Quat *)p); return a3s_vec3(d.x, d.y, d.z); }
    case A3_FIELD_STRING: return a3s_str((const char *)p);
    case A3_FIELD_ENUM: {
        i32 i = *(const i32 *)p;
        if (f->enum_names && i >= 0 && (u32)i < f->enum_count) return a3s_str(f->enum_names[i]);
        return a3s_num(i);
    }
    case A3_FIELD_ASSET: return a3s_str(((const A3AssetRef *)p)->path);
    case A3_FIELD_ENTITY: {
        u64 g = ((const A3EntityRef *)p)->guid;
        A3_UNUSED(w);
        return g ? a3s_entity(g) : a3s_nil();
    }
    default: return a3s_nil();
    }
}

static b32 write_field(A3SVM *vm, const A3FieldDesc *f, u8 *p, const A3SValue *v) {
    char label[A3_NAME_MAX];
    a3_strcpy(label, sizeof(label), f->name);
    switch (f->type) {
    case A3_FIELD_BOOL: *(b32 *)p = a3s_truthy(v); return 1;
    case A3_FIELD_I32: case A3_FIELD_U32: case A3_FIELD_F32: {
        f64 x;
        if (v->type == A3S_NUM) x = v->as.num;
        else if (v->type == A3S_BOOL) x = v->as.b;
        else return a3s_fail(vm, "'%s' must be a number, not %s", label, a3s_type_name(v->type));
        if (f->min < f->max) { if (x < f->min) x = f->min; if (x > f->max) x = f->max; }
        if (f->type == A3_FIELD_F32) *(f32 *)p = (f32)x;
        else if (f->type == A3_FIELD_I32) *(i32 *)p = (i32)x;
        else *(u32 *)p = x < 0 ? 0 : (u32)x;
        return 1;
    }
    case A3_FIELD_VEC2: case A3_FIELD_VEC3: case A3_FIELD_VEC4: case A3_FIELD_COLOR: case A3_FIELD_QUAT: {
        if (v->type != A3S_VEC3) return a3s_fail(vm, "'%s' must be a vec3, like vec3(1, 2, 3), not %s", label, a3s_type_name(v->type));
        f32 *d = (f32 *)p;
        if (f->type == A3_FIELD_QUAT) { *(A3Quat *)p = degrees_to_quat(a3_v3(v->as.v[0], v->as.v[1], v->as.v[2])); return 1; }
        d[0] = v->as.v[0];
        d[1] = v->as.v[1];
        if (f->type != A3_FIELD_VEC2) d[2] = v->as.v[2];
        return 1;
    }
    case A3_FIELD_STRING: {
        if (v->type != A3S_STR) return a3s_fail(vm, "'%s' must be a text, not %s", label, a3s_type_name(v->type));
        a3_strcpy((char *)p, f->size ? f->size : A3_NAME_MAX, v->as.str->chars);
        return 1;
    }
    case A3_FIELD_ENUM: {
        if (v->type == A3S_NUM) {
            i32 i = (i32)v->as.num;
            if (i < 0 || (f->enum_count && (u32)i >= f->enum_count)) return a3s_fail(vm, "'%s' has no option number %d", label, i);
            *(i32 *)p = i;
            return 1;
        }
        if (v->type == A3S_STR) {
            for (u32 i = 0; i < f->enum_count; ++i) {
                if (strcmp_nocase(f->enum_names[i], v->as.str->chars) == 0) { *(i32 *)p = (i32)i; return 1; }
            }
            char hint[160];
            a3s_suggest(v->as.str->chars, f->enum_names, f->enum_count, hint, sizeof(hint));
            a3s_fail(vm, "'%s' has no option \"%s\"", label, v->as.str->chars);
            if (hint[0]) a3s_set_error_hint(vm, hint);
            return 0;
        }
        return a3s_fail(vm, "'%s' must be an option name (text) or number", label);
    }
    case A3_FIELD_ASSET: {
        if (v->type != A3S_STR) return a3s_fail(vm, "'%s' must be a file path (text)", label);
        A3AssetRef *r = (A3AssetRef *)p;
        a3_strcpy(r->path, sizeof(r->path), v->as.str->chars);
        r->handle = 0;
        return 1;
    }
    case A3_FIELD_ENTITY: {
        A3EntityRef *r = (A3EntityRef *)p;
        if (v->type == A3S_NIL) { a3_zero_struct(r); return 1; }
        if (v->type != A3S_ENTITY) return a3s_fail(vm, "'%s' must be an object", label);
        r->guid = v->as.guid;
        r->cached_index = 0xFFFFFFFFu;
        return 1;
    }
    default: return a3s_fail(vm, "'%s' cannot be changed from a script", label);
    }
}

/* ======================================================================== */
/* Host bindings: obj.field                                                 */
/* ======================================================================== */

static const char *const g_entity_props[] = { "position", "rotation", "scale", "forward", "right", "up", "name", "active", "velocity", "parent", "local_position" };

static b32 entity_or_fail(A3SVM *vm, A3World *w, u64 guid, A3Entity *out) {
    *out = resolve(w, guid);
    if (!a3_entity_is_null(*out)) return 1;
    return a3s_fail(vm, "this object no longer exists (it was destroyed)");
}

static b32 missing_member(A3SVM *vm, A3World *w, A3Entity e, const char *name) {
    const char *cands[128];
    u32 n = 0;
    for (u32 i = 0; i < A3_ARRAY_COUNT(g_entity_props); ++i) cands[n++] = g_entity_props[i];
    for (u32 t = 0; t < a3_component_type_count() && n < 120; ++t) {
        A3ComponentType *ct = a3_component_type(t);
        if (ct) cands[n++] = ct->name;
    }
    Inst *other = find_inst(w, a3_entity_guid(w, e));
    if (other && other->inst)
        for (u32 i = 0; i < a3s_instance_global_count(other->inst) && n < 128; ++i) cands[n++] = a3s_instance_global_name(other->inst, i);
    char hint[160];
    a3s_suggest(name, cands, n, hint, sizeof(hint));
    A3ComponentType *ct = a3_component_type_by_name(name);
    if (ct) {
        a3s_fail(vm, "'%s' has no %s component", a3_entity_name(w, e), name);
        a3_snprintf(hint, sizeof(hint), "Add it in the Inspector, or call add_component(obj, \"%s\") first.", name);
    } else a3s_fail(vm, "'%s' has no field or component called '%s'", a3_entity_name(w, e), name);
    if (!hint[0]) a3_strcpy(hint, sizeof(hint), "Objects have position, rotation, scale, name, active, velocity and their components (e.g. obj.Light).");
    a3s_set_error_hint(vm, hint);
    return 0;
}

static A3Vec3 entity_axis(A3World *w, A3Entity e, A3Vec3 local) {
    A3Quat q = a3_transform_world_rotation(w, e);
    return a3_quat_rotate(q, local);
}

static b32 host_get(A3SVM *vm, const A3SValue *obj, const char *name, A3SValue *out) {
    A3World *w = vm_world(vm);
    if (!w) return a3s_fail(vm, "objects are not available here");
    A3Entity e;
    if (!entity_or_fail(vm, w, obj->as.guid, &e)) return 0;
    if (obj->type == A3S_COMP) {
        A3ComponentType *ct = a3_component_type(obj->aux);
        u8 *data = ct ? (u8 *)a3_component_get(w, e, obj->aux) : 0;
        if (!data) return a3s_fail(vm, "'%s' no longer has this component", a3_entity_name(w, e));
        const A3FieldDesc *f = a3_component_find_field(ct, name);
        if (!f) {
            const char *cands[A3_MAX_FIELDS];
            for (u32 i = 0; i < ct->field_count; ++i) cands[i] = ct->fields[i].name;
            char hint[160];
            a3s_suggest(name, cands, ct->field_count, hint, sizeof(hint));
            a3s_fail(vm, "the %s component has no field '%s'", ct->name, name);
            if (!hint[0]) {
                usize l = a3_strcpy(hint, sizeof(hint), "Its fields are: ");
                for (u32 i = 0; i < ct->field_count && l < sizeof(hint) - 24; ++i)
                    l += a3_snprintf(hint + l, sizeof(hint) - l, "%s%s", i ? ", " : "", ct->fields[i].name);
            }
            a3s_set_error_hint(vm, hint);
            return 0;
        }
        *out = read_field(w, f, data + f->offset);
        return 1;
    }
    /* entity shortcuts */
    A3CTransform *tr = a3_transform(w, e);
    if (a3_streq(name, "position")) { A3Vec3 p = tr ? a3_transform_world_position(w, e) : a3_v3(0, 0, 0); *out = a3s_vec3(p.x, p.y, p.z); return 1; }
    if (a3_streq(name, "local_position")) { A3Vec3 p = tr ? tr->position : a3_v3(0, 0, 0); *out = a3s_vec3(p.x, p.y, p.z); return 1; }
    if (a3_streq(name, "rotation")) { A3Vec3 d = tr ? quat_to_degrees(tr->rotation) : a3_v3(0, 0, 0); *out = a3s_vec3(d.x, d.y, d.z); return 1; }
    if (a3_streq(name, "scale")) { A3Vec3 s = tr ? tr->scale : a3_v3_one(); *out = a3s_vec3(s.x, s.y, s.z); return 1; }
    if (a3_streq(name, "forward")) { A3Vec3 v = entity_axis(w, e, a3_v3(0, 0, -1)); *out = a3s_vec3(v.x, v.y, v.z); return 1; }
    if (a3_streq(name, "right")) { A3Vec3 v = entity_axis(w, e, a3_v3(1, 0, 0)); *out = a3s_vec3(v.x, v.y, v.z); return 1; }
    if (a3_streq(name, "up")) { A3Vec3 v = entity_axis(w, e, a3_v3(0, 1, 0)); *out = a3s_vec3(v.x, v.y, v.z); return 1; }
    if (a3_streq(name, "name")) { *out = a3s_str(a3_entity_name(w, e)); return 1; }
    if (a3_streq(name, "active")) { *out = a3s_bool(a3_entity_active(w, e)); return 1; }
    if (a3_streq(name, "velocity")) {
        A3CRigidBody *rb = (A3CRigidBody *)a3_component_get(w, e, A3_T_RIGIDBODY);
        A3Vec3 v = rb ? rb->velocity : a3_v3(0, 0, 0);
        *out = a3s_vec3(v.x, v.y, v.z);
        return 1;
    }
    if (a3_streq(name, "parent")) { A3Entity p = a3_entity_parent(w, e); *out = a3_entity_is_null(p) ? a3s_nil() : a3s_entity(a3_entity_guid(w, p)); return 1; }
    /* components */
    u32 tid = a3_component_id(name);
    if (tid != 0xFFFFFFFFu && a3_component_has(w, e, tid)) { *out = a3s_comp(obj->as.guid, tid); return 1; }
    /* top-level variables of the object's own script (other.health) */
    Inst *other = find_inst(w, obj->as.guid);
    if (other && other->inst) {
        for (u32 i = 0; i < a3s_instance_global_count(other->inst); ++i) {
            if (a3_streq(a3s_instance_global_name(other->inst, i), name)) {
                *out = *a3s_instance_global(other->inst, i);
                a3s_retain(out);
                return 1;
            }
        }
    }
    return missing_member(vm, w, e, name);
}

static b32 host_set(A3SVM *vm, A3SValue *obj, const char *name, const A3SValue *v) {
    A3World *w = vm_world(vm);
    if (!w) return a3s_fail(vm, "objects are not available here");
    A3Entity e;
    if (!entity_or_fail(vm, w, obj->as.guid, &e)) return 0;
    if (obj->type == A3S_COMP) {
        A3ComponentType *ct = a3_component_type(obj->aux);
        u8 *data = ct ? (u8 *)a3_component_get(w, e, obj->aux) : 0;
        if (!data) return a3s_fail(vm, "'%s' no longer has this component", a3_entity_name(w, e));
        const A3FieldDesc *f = a3_component_find_field(ct, name);
        if (!f) { A3SValue tmp; return host_get(vm, obj, name, &tmp); } /* reports the error */
        if (f->flags & A3_FIELD_FLAG_READONLY) return a3s_fail(vm, "'%s' of %s can only be read", name, ct->name);
        if (!write_field(vm, f, data + f->offset, v)) return 0;
        if (obj->aux == A3_T_RIGIDBODY) a3_physics_wake(w, e);
        return 1;
    }
    A3CTransform *tr = a3_transform(w, e);
    b32 is_vec = v->type == A3S_VEC3;
    A3Vec3 vv = is_vec ? a3_v3(v->as.v[0], v->as.v[1], v->as.v[2]) : a3_v3(0, 0, 0);
    if (a3_streq(name, "position") || a3_streq(name, "local_position") || a3_streq(name, "rotation") || a3_streq(name, "scale") || a3_streq(name, "velocity")) {
        if (!is_vec) return a3s_fail(vm, "'%s' must be a vec3, like vec3(0, 1, 0), not %s", name, a3s_type_name(v->type));
        if (a3_streq(name, "velocity")) {
            if (!a3_component_has(w, e, A3_T_RIGIDBODY)) return a3s_fail(vm, "'%s' needs a RigidBody to have a velocity", a3_entity_name(w, e));
            a3_physics_set_velocity(w, e, vv);
            return 1;
        }
        if (!tr) tr = (A3CTransform *)a3_component_add(w, e, A3_T_TRANSFORM);
        if (!tr) return a3s_fail(vm, "could not add a Transform");
        if (a3_streq(name, "position")) a3_transform_set_world_position(w, e, vv);
        else if (a3_streq(name, "local_position")) tr->position = vv;
        else if (a3_streq(name, "rotation")) tr->rotation = degrees_to_quat(vv);
        else tr->scale = vv;
        a3_physics_wake(w, e);
        return 1;
    }
    if (a3_streq(name, "name")) {
        if (v->type != A3S_STR) return a3s_fail(vm, "'name' must be a text");
        a3_entity_set_name(w, e, v->as.str->chars);
        return 1;
    }
    if (a3_streq(name, "active")) { a3_entity_set_active(w, e, a3s_truthy(v)); return 1; }
    if (a3_streq(name, "forward") || a3_streq(name, "right") || a3_streq(name, "up"))
        return a3s_fail(vm, "'%s' can only be read (change rotation instead)", name);
    if (a3_streq(name, "parent")) {
        A3Entity p = A3_ENTITY_NULL;
        if (v->type == A3S_ENTITY) { if (!entity_or_fail(vm, w, v->as.guid, &p)) return 0; }
        else if (v->type != A3S_NIL) return a3s_fail(vm, "'parent' must be an object or nil");
        if (!a3_entity_set_parent(w, e, p)) return a3s_fail(vm, "cannot make an object its own parent");
        return 1;
    }
    u32 tid = a3_component_id(name);
    if (tid != 0xFFFFFFFFu && a3_component_has(w, e, tid)) {
        if (v->type == A3S_COMP && v->aux == tid && v->as.guid == obj->as.guid) return 1; /* write-back of obj.Comp.field = x */
        return a3s_fail(vm, "cannot replace the %s component; change its fields instead (obj.%s.field = ...)", name, name);
    }
    Inst *other = find_inst(w, obj->as.guid);
    if (other && other->inst && a3s_instance_set_global(other->inst, name, v)) return 1;
    return missing_member(vm, w, e, name);
}

static void host_print(A3SInstance *inst, const char *text) {
    Inst *in = (Inst *)a3s_instance_user(inst);
    if (in && in->world) {
        A3Entity e = resolve(in->world, in->guid);
        a3_log(A3_LOG_INFO, "script", "[%s] %s", a3_entity_is_null(e) ? in->path : a3_entity_name(in->world, e), text);
    } else a3_log(A3_LOG_INFO, "script", "%s", text);
}

/* ======================================================================== */
/* Game API natives                                                         */
/* ======================================================================== */

#define NATIVE(fname) static b32 fname(A3SVM *vm, A3SValue *a, u32 n, A3SValue *r)
#define WORLD_OR_FAIL() A3World *w = vm_world(vm); if (!w) return a3s_fail(vm, "this only works on objects in a scene")

static b32 arg_entity(A3SVM *vm, A3SValue *a, u32 i, A3World *w, A3Entity *out) {
    if (a[i].type == A3S_ENTITY) return entity_or_fail(vm, w, a[i].as.guid, out);
    if (a[i].type == A3S_STR) {
        *out = a3_entity_find_by_name(w, a[i].as.str->chars);
        if (!a3_entity_is_null(*out)) return 1;
        return a3s_fail(vm, "there is no object named \"%s\"", a[i].as.str->chars);
    }
    return a3s_fail(vm, "value %u should be an object (like self or find(\"Name\")), but it is %s", i + 1, a3s_type_name(a[i].type));
}

static A3SValue entity_value(A3World *w, A3Entity e) { return a3_entity_is_null(e) ? a3s_nil() : a3s_entity(a3_entity_guid(w, e)); }

NATIVE(n_find) {
    (void)n; WORLD_OR_FAIL();
    const char *name;
    if (!a3s_arg_str(vm, a, 0, &name)) return 0;
    *r = entity_value(w, a3_entity_find_by_name(w, name));
    return 1;
}

typedef struct FindAll { A3World *w; const char *prefix; u32 comp; A3SValue *list; } FindAll;
static b32 find_all_visit(A3World *w, A3Entity e, u32 depth, void *user) {
    (void)depth;
    FindAll *fa = (FindAll *)user;
    b32 ok = fa->comp != 0xFFFFFFFFu ? a3_component_has(w, e, fa->comp) : a3_str_starts_with(a3_entity_name(w, e), fa->prefix);
    if (ok) { A3SValue v = a3s_entity(a3_entity_guid(w, e)); a3s_list_push(fa->list, &v); }
    return 1;
}

NATIVE(n_find_all) {
    (void)n; WORLD_OR_FAIL();
    const char *prefix;
    if (!a3s_arg_str(vm, a, 0, &prefix)) return 0;
    *r = a3s_list(8);
    FindAll fa = { w, prefix, 0xFFFFFFFFu, r };
    a3_world_visit_hierarchy(w, find_all_visit, &fa);
    return 1;
}

NATIVE(n_find_with) {
    (void)n; WORLD_OR_FAIL();
    const char *comp;
    if (!a3s_arg_str(vm, a, 0, &comp)) return 0;
    u32 t = a3_component_id(comp);
    if (t == 0xFFFFFFFFu) return a3s_fail(vm, "there is no component type called \"%s\"", comp);
    *r = a3s_list(8);
    FindAll fa = { w, "", t, r };
    a3_world_visit_hierarchy(w, find_all_visit, &fa);
    return 1;
}

NATIVE(n_exists) {
    (void)n; WORLD_OR_FAIL();
    *r = a3s_bool(a[0].type == A3S_ENTITY && !a3_entity_is_null(resolve(w, a[0].as.guid)));
    return 1;
}

NATIVE(n_spawn) {
    WORLD_OR_FAIL();
    const char *name = "Object";
    if (n > 0 && !a3s_arg_str(vm, a, 0, &name)) return 0;
    A3Entity e = a3_entity_create(w, name);
    A3CTransform *tr = (A3CTransform *)a3_component_add(w, e, A3_T_TRANSFORM);
    if (n > 1) { A3Vec3 p; if (!a3s_arg_vec3(vm, a, 1, &p)) return 0; if (tr) tr->position = p; }
    *r = entity_value(w, e);
    return 1;
}

NATIVE(n_clone) {
    WORLD_OR_FAIL();
    A3Entity src;
    if (!arg_entity(vm, a, 0, w, &src)) return 0;
    A3Entity e = a3_entity_duplicate(w, src);
    if (a3_entity_is_null(e)) return a3s_fail(vm, "could not copy the object");
    if (n > 1) { A3Vec3 p; if (!a3s_arg_vec3(vm, a, 1, &p)) return 0; a3_transform_set_world_position(w, e, p); }
    *r = entity_value(w, e);
    return 1;
}

NATIVE(n_destroy) {
    (void)n; WORLD_OR_FAIL();
    if (a[0].type == A3S_ENTITY) {
        A3Entity e = resolve(w, a[0].as.guid);
        if (!a3_entity_is_null(e)) a3_entity_destroy_deferred(w, e);
        *r = a3s_nil();
        return 1;
    }
    return a3s_fail(vm, "destroy() needs an object, not %s", a3s_type_name(a[0].type));
}

NATIVE(n_add_component) {
    (void)n; WORLD_OR_FAIL();
    A3Entity e;
    const char *comp;
    if (!arg_entity(vm, a, 0, w, &e) || !a3s_arg_str(vm, a, 1, &comp)) return 0;
    u32 t = a3_component_id(comp);
    if (t == 0xFFFFFFFFu) {
        const char *cands[128];
        u32 k = 0;
        for (u32 i = 0; i < a3_component_type_count() && k < 128; ++i) if (a3_component_type(i)) cands[k++] = a3_component_type(i)->name;
        char hint[160];
        a3s_suggest(comp, cands, k, hint, sizeof(hint));
        a3s_fail(vm, "there is no component type called \"%s\"", comp);
        if (hint[0]) a3s_set_error_hint(vm, hint);
        return 0;
    }
    if (!a3_component_add(w, e, t)) return a3s_fail(vm, "could not add %s", comp);
    *r = a3s_comp(a3_entity_guid(w, e), t);
    return 1;
}

NATIVE(n_has_component) {
    (void)n; WORLD_OR_FAIL();
    A3Entity e;
    const char *comp;
    if (!arg_entity(vm, a, 0, w, &e) || !a3s_arg_str(vm, a, 1, &comp)) return 0;
    u32 t = a3_component_id(comp);
    *r = a3s_bool(t != 0xFFFFFFFFu && a3_component_has(w, e, t));
    return 1;
}

NATIVE(n_remove_component) {
    (void)n; WORLD_OR_FAIL();
    A3Entity e;
    const char *comp;
    if (!arg_entity(vm, a, 0, w, &e) || !a3s_arg_str(vm, a, 1, &comp)) return 0;
    u32 t = a3_component_id(comp);
    *r = a3s_bool(t != 0xFFFFFFFFu && a3_component_remove(w, e, t));
    return 1;
}

NATIVE(n_look_at) {
    (void)n; WORLD_OR_FAIL();
    A3Entity e;
    A3Vec3 target;
    if (!arg_entity(vm, a, 0, w, &e)) return 0;
    if (a[1].type == A3S_ENTITY) { A3Entity t; if (!entity_or_fail(vm, w, a[1].as.guid, &t)) return 0; target = a3_transform_world_position(w, t); }
    else if (!a3s_arg_vec3(vm, a, 1, &target)) return 0;
    A3CTransform *tr = a3_transform(w, e);
    if (!tr) return a3s_fail(vm, "the object has no Transform");
    A3Vec3 d = a3_v3_sub(target, a3_transform_world_position(w, e));
    f32 flat = a3_sqrtf(d.x * d.x + d.z * d.z);
    if (flat < 1e-6f && a3_absf(d.y) < 1e-6f) { *r = a3s_nil(); return 1; }
    f32 yaw = a3_atan2f(-d.x, -d.z);
    f32 pitch = a3_atan2f(d.y, flat);
    tr->rotation = a3_quat_euler(pitch, yaw, 0);
    *r = a3s_nil();
    return 1;
}

NATIVE(n_send) {
    WORLD_OR_FAIL();
    A3Entity e;
    const char *fn;
    if (!arg_entity(vm, a, 0, w, &e) || !a3s_arg_str(vm, a, 1, &fn)) return 0;
    Inst *in = find_inst(w, a3_entity_guid(w, e));
    *r = a3s_nil();
    if (!in || !in->inst || in->failed) return 1;
    if (!a3s_module_has_function(a3s_instance_module(in->inst), fn)) {
        return a3s_fail(vm, "the script on '%s' has no function '%s'", a3_entity_name(w, e), fn);
    }
    A3SError err;
    if (!a3s_call(in->inst, fn, a + 2, n - 2, r, 0, &err)) {
        A3Entity oe = resolve(w, in->guid);
        record_error(in->path, a3_entity_is_null(oe) ? "" : a3_entity_name(w, oe), &err);
        in->failed = 1;
        return a3s_fail(vm, "send(): the script on '%s' failed in %s()", a3_entity_name(w, e), fn);
    }
    return 1;
}

/* ---- input ---- */

static const A3ScriptFrame *vm_frame(A3SVM *vm) {
    A3World *w = vm_world(vm);
    WorldState *ws = w ? world_state(w, 0) : 0;
    return ws ? ws->frame : 0;
}

static int key_lookup(const char *name) {
    char k[32];
    usize n = 0;
    for (const char *p = name; *p && n < sizeof(k) - 1; ++p) if (*p != ' ' && *p != '_' && *p != '-') k[n++] = (char)a3_to_lower(*p);
    k[n] = 0;
    static const struct { const char *alias, *real; } al[] = {
        { "shift", "leftshift" }, { "ctrl", "leftctrl" }, { "control", "leftctrl" }, { "alt", "leftalt" }, { "return", "enter" },
        { "esc", "escape" }, { "arrowleft", "left" }, { "arrowright", "right" }, { "arrowup", "up" }, { "arrowdown", "down" },
    };
    for (u32 i = 0; i < A3_ARRAY_COUNT(al); ++i) if (a3_streq(k, al[i].alias)) { a3_strcpy(k, sizeof(k), al[i].real); break; }
    if (n == 1) return a3_key_from_name(k);
    for (int key = 1; key < A3_KEY_COUNT; ++key) {
        const char *kn = a3_key_name(key);
        if (kn[0] != '?' && strcmp_nocase(kn, k) == 0) return key;
    }
    return 0;
}

static b32 key_state(A3SVM *vm, A3SValue *a, A3SValue *r, int which) {
    const char *name;
    if (!a3s_arg_str(vm, a, 0, &name)) return 0;
    int key = key_lookup(name);
    if (!key) return a3s_fail(vm, "unknown key \"%s\"", name);
    const A3ScriptFrame *f = vm_frame(vm);
    b32 v = 0;
    if (f && f->input) v = which == 0 ? f->input->keys[key] : which == 1 ? f->input->keys_pressed[key] : f->input->keys_released[key];
    *r = a3s_bool(v);
    return 1;
}
NATIVE(n_key_down) { (void)n; return key_state(vm, a, r, 0); }
NATIVE(n_key_pressed) { (void)n; return key_state(vm, a, r, 1); }
NATIVE(n_key_released) { (void)n; return key_state(vm, a, r, 2); }

static b32 action_state(A3SVM *vm, A3SValue *a, A3SValue *r, int which) {
    const char *name;
    if (!a3s_arg_str(vm, a, 0, &name)) return 0;
    const A3ScriptFrame *f = vm_frame(vm);
    if (!f || !f->actions || !f->input) { *r = which == 3 ? a3s_num(0) : a3s_bool(0); return 1; }
    if (!a3_input_map_find((A3InputMap *)f->actions, name)) {
        const char *cands[A3_ACTION_MAX];
        u32 k = 0;
        for (u32 i = 0; i < f->actions->count && k < A3_ACTION_MAX; ++i) cands[k++] = f->actions->actions[i].name;
        char hint[160];
        a3s_suggest(name, cands, k, hint, sizeof(hint));
        a3s_fail(vm, "there is no input action called \"%s\"", name);
        a3s_set_error_hint(vm, hint[0] ? hint : "Built-in actions: move_x, move_y, jump, sprint, crouch, interact, fire, aim, pause.");
        return 0;
    }
    if (which == 0) *r = a3s_bool(a3_action_down(f->actions, f->input, name));
    else if (which == 1) *r = a3s_bool(a3_action_pressed(f->actions, f->input, name));
    else if (which == 2) *r = a3s_bool(a3_action_released(f->actions, f->input, name));
    else *r = a3s_num(a3_action_value(f->actions, f->input, name));
    return 1;
}
NATIVE(n_action_down) { (void)n; return action_state(vm, a, r, 0); }
NATIVE(n_action_pressed) { (void)n; return action_state(vm, a, r, 1); }
NATIVE(n_action_released) { (void)n; return action_state(vm, a, r, 2); }
NATIVE(n_action_value) { (void)n; return action_state(vm, a, r, 3); }

static b32 mouse_state(A3SVM *vm, A3SValue *a, u32 n, A3SValue *r, int which) {
    f64 b = 0;
    if (n > 0 && !a3s_arg_num(vm, a, 0, &b)) return 0;
    if (b < 0 || b >= A3_MOUSE_BUTTON_COUNT) return a3s_fail(vm, "mouse buttons are 0 (left), 1 (right) and 2 (middle)");
    const A3ScriptFrame *f = vm_frame(vm);
    b32 v = 0;
    if (f && f->input) v = which == 0 ? f->input->mouse[(int)b] : which == 1 ? f->input->mouse_pressed[(int)b] : f->input->mouse_released[(int)b];
    *r = a3s_bool(v);
    return 1;
}
NATIVE(n_mouse_down) { return mouse_state(vm, a, n, r, 0); }
NATIVE(n_mouse_pressed) { return mouse_state(vm, a, n, r, 1); }
NATIVE(n_mouse_released) { return mouse_state(vm, a, n, r, 2); }

NATIVE(n_mouse_position) {
    (void)a; (void)n;
    const A3ScriptFrame *f = vm_frame(vm);
    if (!f || !f->input || f->screen_w <= 0 || f->screen_h <= 0) { *r = a3s_vec3(0, 0, 0); return 1; }
    /* in HUD units (1280 x 720) so it matches hud_* coordinates */
    *r = a3s_vec3(f->input->mouse_pos.x * A3_HUD_WIDTH / (f32)f->screen_w, f->input->mouse_pos.y * A3_HUD_HEIGHT / (f32)f->screen_h, 0);
    return 1;
}

NATIVE(n_mouse_delta) {
    (void)a; (void)n;
    const A3ScriptFrame *f = vm_frame(vm);
    *r = f && f->input ? a3s_vec3(f->input->mouse_delta.x, f->input->mouse_delta.y, 0) : a3s_vec3(0, 0, 0);
    return 1;
}

/* ---- time ---- */

NATIVE(n_time) { (void)a; (void)n; const A3ScriptFrame *f = vm_frame(vm); *r = a3s_num(f ? f->time : 0); return 1; }
NATIVE(n_delta_time) { (void)a; (void)n; const A3ScriptFrame *f = vm_frame(vm); *r = a3s_num(f ? f->dt : 0); return 1; }

/* ---- physics ---- */

NATIVE(n_add_impulse) {
    (void)n; WORLD_OR_FAIL();
    A3Entity e;
    A3Vec3 imp;
    if (!arg_entity(vm, a, 0, w, &e) || !a3s_arg_vec3(vm, a, 1, &imp)) return 0;
    if (!a3_component_has(w, e, A3_T_RIGIDBODY)) return a3s_fail(vm, "'%s' needs a RigidBody to be pushed", a3_entity_name(w, e));
    a3_physics_add_impulse(w, e, imp);
    *r = a3s_nil();
    return 1;
}

static b32 do_raycast(A3SVM *vm, A3SValue *a, u32 n, A3RaycastHit *hit, b32 *found) {
    WORLD_OR_FAIL();
    A3Vec3 o, d;
    f64 maxd = 1000;
    if (!a3s_arg_vec3(vm, a, 0, &o) || !a3s_arg_vec3(vm, a, 1, &d) || (n > 2 && !a3s_arg_num(vm, a, 2, &maxd))) return 0;
    f32 l = a3_v3_len(d);
    if (l < 1e-8f) return a3s_fail(vm, "the ray direction cannot be vec3(0, 0, 0)");
    d = a3_v3_scale(d, 1.0f / l);
    Inst *self = (Inst *)a3s_instance_user(a3s_vm_instance(vm));
    A3Entity ignore = self ? resolve(w, self->guid) : A3_ENTITY_NULL;
    *found = a3_physics_raycast_ignore(w, o, d, (f32)maxd, 0xFFFFFFFFu, ignore, hit);
    return 1;
}

NATIVE(n_raycast) {
    WORLD_OR_FAIL();
    A3RaycastHit hit;
    b32 found = 0;
    if (!do_raycast(vm, a, n, &hit, &found)) return 0;
    *r = found ? entity_value(w, hit.entity) : a3s_nil();
    return 1;
}

NATIVE(n_raycast_hit) {
    WORLD_OR_FAIL();
    A3RaycastHit hit;
    b32 found = 0;
    if (!do_raycast(vm, a, n, &hit, &found)) return 0;
    if (!found) { *r = a3s_nil(); return 1; }
    *r = a3s_list(4);
    A3SValue items[4] = { entity_value(w, hit.entity), a3s_vec3(hit.point.x, hit.point.y, hit.point.z), a3s_vec3(hit.normal.x, hit.normal.y, hit.normal.z), a3s_num(hit.distance) };
    for (u32 i = 0; i < 4; ++i) a3s_list_push(r, &items[i]);
    return 1;
}

NATIVE(n_overlap_sphere) {
    (void)n; WORLD_OR_FAIL();
    A3Vec3 c;
    f64 rad;
    if (!a3s_arg_vec3(vm, a, 0, &c) || !a3s_arg_num(vm, a, 1, &rad)) return 0;
    A3Entity found[64];
    u32 k = a3_physics_overlap_sphere(w, c, (f32)rad, 0xFFFFFFFFu, found, 64);
    *r = a3s_list(k);
    for (u32 i = 0; i < k; ++i) { A3SValue v = entity_value(w, found[i]); a3s_list_push(r, &v); }
    return 1;
}

/* ---- vehicles and the city ---- */

NATIVE(n_spawn_car) {
    WORLD_OR_FAIL();
    const char *name = "Car";
    A3Vec3 p = a3_v3_zero(), col = a3_v3(0.8f, 0.1f, 0.1f);
    f64 yaw = 0;
    if (n > 0 && !a3s_arg_str(vm, a, 0, &name)) return 0;
    if (n > 1 && !a3s_arg_vec3(vm, a, 1, &p)) return 0;
    if (n > 2 && !a3s_arg_num(vm, a, 2, &yaw)) return 0;
    if (n > 3 && !a3s_arg_vec3(vm, a, 3, &col)) return 0;
    const char *style = "sedan";
    if (n > 4 && !a3s_arg_str(vm, a, 4, &style)) return 0;
    static const char *const styles[] = { "sedan", "sports", "suv", "hatch", "taxi", "police" };
    b32 known = 0;
    for (u32 i = 0; i < A3_ARRAY_COUNT(styles); ++i) if (a3_streq(style, styles[i])) known = 1;
    if (!known) return a3s_fail(vm, "unknown car style \"%s\" (use sedan, sports, suv, hatch, taxi or police)", style);
    *r = entity_value(w, a3_vehicle_spawn_car_style(w, name, p, (f32)yaw, a3_v4(col.x, col.y, col.z, 1), style));
    return 1;
}

NATIVE(n_road_point) {
    WORLD_OR_FAIL();
    A3Vec3 c = a3_v3_zero(), out, dir;
    f64 lo = 0, hi = 1e9;
    if (n > 0 && !a3s_arg_vec3(vm, a, 0, &c)) return 0;
    if (n > 1 && !a3s_arg_num(vm, a, 1, &lo)) return 0;
    if (n > 2 && !a3s_arg_num(vm, a, 2, &hi)) return 0;
    *r = a3_traffic_random_road_point(w, c, (f32)lo, (f32)hi, &out, &dir) ? a3s_vec3(out.x, out.y, out.z) : a3s_nil();
    return 1;
}

NATIVE(n_nearest_road) {
    (void)n; WORLD_OR_FAIL();
    A3Vec3 p, out, dir;
    if (!a3s_arg_vec3(vm, a, 0, &p)) return 0;
    if (!a3_traffic_nearest_road_point(w, p, &out, &dir)) { *r = a3s_nil(); return 1; }
    *r = a3s_list(2);
    A3SValue items[2] = { a3s_vec3(out.x, out.y, out.z), a3s_vec3(dir.x, dir.y, dir.z) };
    for (u32 i = 0; i < 2; ++i) a3s_list_push(r, &items[i]);
    return 1;
}

NATIVE(n_city_time) {
    (void)n; WORLD_OR_FAIL();
    const char *name;
    if (!a3s_arg_str(vm, a, 0, &name)) return 0;
    a3_city_apply_time(w, a3_city_time_from_name(name));
    *r = a3s_nil();
    return 1;
}

/* ---- sound, particles, animation ---- */

NATIVE(n_play_sound) {
    const char *path;
    f64 vol = 1, pitch = 1;
    if (!a3s_arg_str(vm, a, 0, &path) || (n > 1 && !a3s_arg_num(vm, a, 1, &vol)) || (n > 2 && !a3s_arg_num(vm, a, 2, &pitch))) return 0;
    char full[A3_PATH_MAX];
    /* "coin" is short for "builtin:coin" */
    b32 builtin = 0;
    for (u32 i = 1; i < A3_SOUND_COUNT; ++i) if (a3_streq(path, a3_builtin_sound_names[i])) builtin = 1;
    if (builtin) a3_snprintf(full, sizeof(full), "builtin:%s", path); else a3_strcpy(full, sizeof(full), path);
    u32 clip = a3_audio_clip(full);
    if (!clip) {
        a3s_fail(vm, "could not load the sound \"%s\"", path);
        a3s_set_error_hint(vm, "Use a .wav file from your project, or a built-in sound: jump, coin, hit, explosion, laser, click, powerup, hurt, beep...");
        return 0;
    }
    a3_audio_play(clip, (f32)vol, (f32)pitch, 0);
    *r = a3s_nil();
    return 1;
}

NATIVE(n_burst) {
    (void)n; WORLD_OR_FAIL();
    A3Entity e;
    if (!arg_entity(vm, a, 0, w, &e)) return 0;
    A3CParticleEmitter *em = (A3CParticleEmitter *)a3_component_get(w, e, A3_T_PARTICLE_EMITTER);
    if (!em) return a3s_fail(vm, "'%s' has no ParticleEmitter", a3_entity_name(w, e));
    em->emitting = 1;
    a3_particles_restart(w, e);
    *r = a3s_nil();
    return 1;
}

static b32 anim_ctl(A3SVM *vm, A3SValue *a, A3SValue *r, b32 play) {
    WORLD_OR_FAIL();
    A3Entity e;
    if (!arg_entity(vm, a, 0, w, &e)) return 0;
    A3CAnimator *an = (A3CAnimator *)a3_component_get(w, e, A3_T_ANIMATOR);
    if (!an) return a3s_fail(vm, "'%s' has no Animator", a3_entity_name(w, e));
    an->playing = play;
    if (play) { an->time = 0; an->direction = 1; an->started = 1; }
    *r = a3s_nil();
    return 1;
}
NATIVE(n_play_animation) { (void)n; return anim_ctl(vm, a, r, 1); }
NATIVE(n_stop_animation) { (void)n; return anim_ctl(vm, a, r, 0); }

/* ---- HUD ---- */

static b32 hud_color(A3SVM *vm, A3SValue *a, u32 n, u32 ci, u32 ai, A3Vec4 *out) {
    *out = a3_v4(1, 1, 1, 1);
    if (n > ci) { A3Vec3 c; if (!a3s_arg_vec3(vm, a, ci, &c)) return 0; out->x = c.x; out->y = c.y; out->z = c.z; }
    if (n > ai) { f64 al; if (!a3s_arg_num(vm, a, ai, &al)) return 0; out->w = (f32)al; }
    return 1;
}

static A3HudCmd *hud_push(A3SVM *vm) {
    A3World *w = vm_world(vm);
    WorldState *ws = w ? world_state(w, 1) : 0;
    if (!ws || ws->hud.count >= 1024) return 0;
    A3HudCmd c;
    a3_zero_struct(&c);
    if (!a3_array_push(ws->hud, c, A3_MEM_SCRIPT)) return 0;
    return &a3_array_last(ws->hud);
}

NATIVE(n_hud_text) {
    f64 x, y, size = 1, align = 0;
    if (!a3s_arg_num(vm, a, 1, &x) || !a3s_arg_num(vm, a, 2, &y) || (n > 3 && !a3s_arg_num(vm, a, 3, &size))) return 0;
    A3Vec4 col;
    if (!hud_color(vm, a, n, 4, 5, &col)) return 0;
    if (n > 6 && a[6].type == A3S_STR) align = a3_streq(a[6].as.str->chars, "center") ? 1 : a3_streq(a[6].as.str->chars, "right") ? 2 : 0;
    A3HudCmd *c = hud_push(vm);
    if (c) {
        c->kind = A3_HUD_TEXT;
        c->x = (f32)x; c->y = (f32)y; c->w = (f32)size; c->h = (f32)align;
        c->color = col;
        a3s_to_string(&a[0], c->text, sizeof(c->text));
    }
    *r = a3s_nil();
    return 1;
}

NATIVE(n_hud_rect) {
    f64 x, y, ww, hh;
    if (!a3s_arg_num(vm, a, 0, &x) || !a3s_arg_num(vm, a, 1, &y) || !a3s_arg_num(vm, a, 2, &ww) || !a3s_arg_num(vm, a, 3, &hh)) return 0;
    A3Vec4 col;
    if (!hud_color(vm, a, n, 4, 5, &col)) return 0;
    A3HudCmd *c = hud_push(vm);
    if (c) { c->kind = A3_HUD_RECT; c->x = (f32)x; c->y = (f32)y; c->w = (f32)ww; c->h = (f32)hh; c->color = col; }
    *r = a3s_nil();
    return 1;
}

NATIVE(n_hud_bar) {
    f64 x, y, ww, hh, frac;
    if (!a3s_arg_num(vm, a, 0, &x) || !a3s_arg_num(vm, a, 1, &y) || !a3s_arg_num(vm, a, 2, &ww) || !a3s_arg_num(vm, a, 3, &hh) || !a3s_arg_num(vm, a, 4, &frac)) return 0;
    A3Vec4 col;
    if (!hud_color(vm, a, n, 5, 99, &col)) return 0;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    A3HudCmd *bg = hud_push(vm);
    if (bg) { bg->kind = A3_HUD_RECT; bg->x = (f32)x; bg->y = (f32)y; bg->w = (f32)ww; bg->h = (f32)hh; bg->color = a3_v4(0, 0, 0, 0.55f); }
    A3HudCmd *fg = hud_push(vm);
    if (fg) { fg->kind = A3_HUD_RECT; fg->x = (f32)x + 2; fg->y = (f32)y + 2; fg->w = (f32)((ww - 4) * frac); fg->h = (f32)hh - 4; fg->color = col; }
    *r = a3s_nil();
    return 1;
}

/* ---- scenes and saved values ---- */

NATIVE(n_load_scene) {
    (void)n; WORLD_OR_FAIL();
    const char *path;
    if (!a3s_arg_str(vm, a, 0, &path)) return 0;
    WorldState *ws = world_state(w, 1);
    if (ws) a3_strcpy(ws->scene_request, sizeof(ws->scene_request), path);
    *r = a3s_nil();
    return 1;
}

typedef struct SaveEntry { char key[64]; A3SValue v; } SaveEntry;
static A3_ARRAY_TYPE(SaveEntry) g_saves;
static b32 g_saves_loaded, g_saves_dirty;

static void saves_path(char *out, usize cap) { a3_assets_path("saves.a3save", out, cap); }

static void saves_set(const char *key, const A3SValue *v) {
    for (u32 i = 0; i < g_saves.count; ++i) {
        if (a3_streq(g_saves.data[i].key, key)) { a3s_release(&g_saves.data[i].v); g_saves.data[i].v = *v; a3s_retain(v); return; }
    }
    SaveEntry e;
    a3_zero_struct(&e);
    a3_strcpy(e.key, sizeof(e.key), key);
    e.v = *v;
    a3s_retain(v);
    if (!a3_array_push(g_saves, e, A3_MEM_SCRIPT)) a3s_release(&e.v);
}

static void saves_load(void) {
    if (g_saves_loaded) return;
    g_saves_loaded = 1;
    char path[A3_PATH_MAX * 2];
    saves_path(path, sizeof(path));
    A3FileData fd;
    if (a3_file_read_all(path, A3_MEM_TEMP, &fd) != A3_OK) return;
    /* one value per line: <type> <key> <value>   (n = number, b = bool, s = text) */
    A3Str rest = a3_str_n((const char *)fd.data, fd.size);
    while (rest.len) {
        A3Str line = a3_str_split_next(&rest, '\n');
        if (line.len < 4 || line.ptr[1] != ' ') continue;
        char type = line.ptr[0];
        A3Str body = a3_str_sub(line, 2, line.len - 2);
        isize sp = a3_str_find(body, ' ');
        if (sp <= 0) continue;
        char key[64];
        a3_str_to_buf(a3_str_sub(body, 0, (usize)sp), key, sizeof(key));
        A3Str val = a3_str_sub(body, (usize)sp + 1, body.len - (usize)sp - 1);
        if (val.len && val.ptr[val.len - 1] == '\r') val.len--;
        A3SValue v = a3s_nil();
        f64 num;
        if (type == 'n' && a3_parse_f64(val.ptr, val.len, &num)) v = a3s_num(num);
        else if (type == 'b') v = a3s_bool(val.len && val.ptr[0] == '1');
        else if (type == 's') {
            char buf[512];
            usize k = 0;
            for (usize i = 0; i < val.len && k < sizeof(buf) - 1; ++i) {
                char c = val.ptr[i];
                if (c == '\\' && i + 1 < val.len) { c = val.ptr[++i]; if (c == 'n') c = '\n'; }
                buf[k++] = c;
            }
            v = a3s_str_n(buf, k);
        } else continue;
        saves_set(key, &v);
        a3s_release(&v);
    }
    a3_free(fd.data);
}

void a3_scripts_saves_flush(void) {
    if (!g_saves_dirty) return;
    g_saves_dirty = 0;
    A3StrBuf sb;
    a3_strbuf_init(&sb, A3_MEM_TEMP);
    for (u32 i = 0; i < g_saves.count; ++i) {
        const SaveEntry *e = &g_saves.data[i];
        if (e->v.type == A3S_NUM) a3_strbuf_appendf(&sb, "n %s %.17g\n", e->key, e->v.as.num);
        else if (e->v.type == A3S_BOOL) a3_strbuf_appendf(&sb, "b %s %d\n", e->key, e->v.as.b ? 1 : 0);
        else if (e->v.type == A3S_STR) {
            a3_strbuf_appendf(&sb, "s %s ", e->key);
            for (u32 k = 0; k < e->v.as.str->len; ++k) {
                char c = e->v.as.str->chars[k];
                if (c == '\n') a3_strbuf_append(&sb, "\\n");
                else if (c == '\\') a3_strbuf_append(&sb, "\\\\");
                else a3_strbuf_append_char(&sb, c);
            }
            a3_strbuf_append_char(&sb, '\n');
        }
    }
    char path[A3_PATH_MAX * 2];
    saves_path(path, sizeof(path));
    if (a3_file_write_atomic(path, a3_strbuf_cstr(&sb), sb.len) != A3_OK)
        a3_log_hint(A3_LOG_WARN, "script", "The game folder may be read-only.", "could not write saved values to %s", path);
    a3_strbuf_free(&sb);
}

void a3_scripts_saves_reset(void) {
    for (u32 i = 0; i < g_saves.count; ++i) a3s_release(&g_saves.data[i].v);
    a3_array_free(g_saves);
    g_saves_loaded = g_saves_dirty = 0;
}

static b32 valid_key(A3SVM *vm, const char *key) {
    usize l = a3_strlen(key);
    if (!l || l >= 64) return a3s_fail(vm, "a save key must be 1 to 63 letters long");
    for (usize i = 0; i < l; ++i) if (!a3_is_ident(key[i]) && key[i] != '.' && key[i] != '-') return a3s_fail(vm, "a save key can only use letters, digits, _ . and -");
    return 1;
}

NATIVE(n_save_value) {
    (void)n;
    const char *key;
    if (!a3s_arg_str(vm, a, 0, &key) || !valid_key(vm, key)) return 0;
    if (a[1].type != A3S_NUM && a[1].type != A3S_BOOL && a[1].type != A3S_STR) return a3s_fail(vm, "only numbers, true/false and texts can be saved, not %s", a3s_type_name(a[1].type));
    saves_load();
    saves_set(key, &a[1]);
    g_saves_dirty = 1;
    *r = a3s_nil();
    return 1;
}

NATIVE(n_load_value) {
    const char *key;
    if (!a3s_arg_str(vm, a, 0, &key)) return 0;
    saves_load();
    for (u32 i = 0; i < g_saves.count; ++i) if (a3_streq(g_saves.data[i].key, key)) { *r = g_saves.data[i].v; a3s_retain(r); return 1; }
    *r = n > 1 ? a[1] : a3s_nil();
    a3s_retain(r);
    return 1;
}

static void register_natives(void) {
    static const A3SNative api[] = {
        { "find", n_find, 1, 1, "Objects", "find(name)", "The object with this name, or nil." },
        { "find_all", n_find_all, 1, 1, "Objects", "find_all(name_start)", "A list of every object whose name starts with this text." },
        { "find_with", n_find_with, 1, 1, "Objects", "find_with(component)", "A list of every object that has this component, e.g. find_with(\"Light\")." },
        { "exists", n_exists, 1, 1, "Objects", "exists(obj)", "True if the object is still in the scene." },
        { "spawn", n_spawn, 0, 2, "Objects", "spawn(name, position)", "Creates an empty object (add components with add_component)." },
        { "clone", n_clone, 1, 2, "Objects", "clone(obj, position)", "Copies an object with all its components and children." },
        { "destroy", n_destroy, 1, 1, "Objects", "destroy(obj)", "Removes an object at the end of the frame." },
        { "add_component", n_add_component, 2, 2, "Objects", "add_component(obj, name)", "Adds a component, e.g. add_component(self, \"RigidBody\")." },
        { "has_component", n_has_component, 2, 2, "Objects", "has_component(obj, name)", "True if the object has this component." },
        { "remove_component", n_remove_component, 2, 2, "Objects", "remove_component(obj, name)", "Removes a component." },
        { "look_at", n_look_at, 2, 2, "Objects", "look_at(obj, target)", "Turns obj to face a position or another object." },
        { "send", n_send, 2, -1, "Objects", "send(obj, \"function\", values...)", "Calls a function in another object's script." },
        { "key_down", n_key_down, 1, 1, "Input", "key_down(\"space\")", "True while the key is held." },
        { "key_pressed", n_key_pressed, 1, 1, "Input", "key_pressed(\"e\")", "True on the frame the key goes down." },
        { "key_released", n_key_released, 1, 1, "Input", "key_released(\"e\")", "True on the frame the key goes up." },
        { "action_down", n_action_down, 1, 1, "Input", "action_down(\"jump\")", "True while an input action is held (Project Settings > Input)." },
        { "action_pressed", n_action_pressed, 1, 1, "Input", "action_pressed(\"fire\")", "True on the frame the action starts." },
        { "action_released", n_action_released, 1, 1, "Input", "action_released(\"fire\")", "True on the frame the action ends." },
        { "action_value", n_action_value, 1, 1, "Input", "action_value(\"move_x\")", "Axis value from -1 to 1." },
        { "mouse_down", n_mouse_down, 0, 1, "Input", "mouse_down(button)", "True while a mouse button is held (0 left, 1 right, 2 middle)." },
        { "mouse_pressed", n_mouse_pressed, 0, 1, "Input", "mouse_pressed(button)", "True on the frame the button goes down." },
        { "mouse_released", n_mouse_released, 0, 1, "Input", "mouse_released(button)", "True on the frame the button goes up." },
        { "mouse_position", n_mouse_position, 0, 0, "Input", "mouse_position()", "Mouse position in HUD units (1280 x 720)." },
        { "mouse_delta", n_mouse_delta, 0, 0, "Input", "mouse_delta()", "Mouse movement this frame, in pixels." },
        { "time", n_time, 0, 0, "Game", "time()", "Seconds since the game started." },
        { "delta_time", n_delta_time, 0, 0, "Game", "delta_time()", "Seconds since the last frame." },
        { "add_impulse", n_add_impulse, 2, 2, "Physics", "add_impulse(obj, force)", "Pushes a RigidBody, e.g. add_impulse(self, vec3(0, 5, 0))." },
        { "raycast", n_raycast, 2, 3, "Physics", "raycast(origin, direction, max_distance)", "The first object hit by a ray, or nil (ignores the object running the script)." },
        { "raycast_hit", n_raycast_hit, 2, 3, "Physics", "raycast_hit(origin, direction, max_distance)", "[object, point, normal, distance] of the first hit, or nil." },
        { "overlap_sphere", n_overlap_sphere, 2, 2, "Physics", "overlap_sphere(center, radius)", "A list of objects with colliders inside a sphere." },
        { "spawn_car", n_spawn_car, 0, 5, "City", "spawn_car(name, position, yaw, color, style)", "Creates a drivable car (Vehicle) facing yaw degrees; style is sedan, sports, suv, hatch, taxi or police. Set car.Vehicle.use_input = true to drive it." },
        { "road_point", n_road_point, 0, 3, "City", "road_point(center, min_distance, max_distance)", "A random point on a road of the city, between the distances from center (nil without roads)." },
        { "nearest_road", n_nearest_road, 1, 1, "City", "nearest_road(position)", "[point, direction] of the closest road center line, or nil." },
        { "city_time", n_city_time, 1, 1, "City", "city_time(\"night\")", "Changes the sky, sun and look to \"day\", \"sunset\" or \"night\"." },
        { "play_sound", n_play_sound, 1, 3, "Game", "play_sound(sound, volume, pitch)", "Plays a .wav file or a built-in sound such as \"coin\" or \"jump\"." },
        { "burst", n_burst, 1, 1, "Game", "burst(obj)", "Restarts the ParticleEmitter of an object (great with the Explosion preset)." },
        { "play_animation", n_play_animation, 1, 1, "Game", "play_animation(obj)", "Plays the Animator of an object from the start." },
        { "stop_animation", n_stop_animation, 1, 1, "Game", "stop_animation(obj)", "Stops the Animator of an object." },
        { "hud_text", n_hud_text, 3, 7, "HUD", "hud_text(text, x, y, size, color, alpha, align)", "Draws text this frame. The screen is 1280 x 720 units; align is \"left\", \"center\" or \"right\"." },
        { "hud_rect", n_hud_rect, 4, 6, "HUD", "hud_rect(x, y, width, height, color, alpha)", "Draws a filled rectangle this frame." },
        { "hud_bar", n_hud_bar, 5, 6, "HUD", "hud_bar(x, y, width, height, fraction, color)", "Draws a bar filled from 0 to 1 (health, stamina...)." },
        { "load_scene", n_load_scene, 1, 1, "Game", "load_scene(\"Scenes/Level2.a3scene\")", "Switches to another scene after this frame." },
        { "save_value", n_save_value, 2, 2, "Game", "save_value(key, value)", "Remembers a number, text or true/false between game sessions." },
        { "load_value", n_load_value, 1, 2, "Game", "load_value(key, default)", "A saved value, or the default when nothing was saved." },
    };
    for (u32 i = 0; i < A3_ARRAY_COUNT(api); ++i) a3s_register(&api[i]);
}

/* ======================================================================== */
/* Instances and update                                                     */
/* ======================================================================== */

static void inst_destroy(Inst *in) {
    a3s_instance_destroy(in->inst);
    a3_free(in);
}

static void inst_error(Inst *in, const A3SError *err) {
    A3Entity e = resolve(in->world, in->guid);
    record_error(in->path, a3_entity_is_null(e) ? "" : a3_entity_name(in->world, e), err);
    in->failed = 1;
}

/* Creates (or re-creates after a reload) the instance; keeps top-level values by name. */
static void inst_load(Inst *in, ModEntry *me) {
    A3SInstance *old = in->inst;
    in->inst = 0;
    in->version = me->version;
    in->failed = 0;
    if (!me->module) { in->failed = 1; a3s_instance_destroy(old); return; }
    A3SError err;
    A3SInstance *ni = a3s_instance_create(me->module, a3s_entity(in->guid), in, &err);
    if (!ni) { inst_error(in, &err); a3s_instance_destroy(old); return; }
    if (old) {
        for (u32 i = 0; i < a3s_instance_global_count(old); ++i)
            a3s_instance_set_global(ni, a3s_instance_global_name(old, i), a3s_instance_global(old, i));
        a3s_instance_destroy(old);
    }
    in->inst = ni;
}

static b32 inst_call(Inst *in, const char *fn, const A3SValue *args, u32 argc) {
    if (!in->inst || in->failed) return 0;
    if (!a3s_module_has_function(a3s_instance_module(in->inst), fn)) return 1;
    A3SError err;
    if (!a3s_call(in->inst, fn, args, argc, 0, 1, &err)) { inst_error(in, &err); return 0; }
    return 1;
}

void a3_scripts_update(A3World *w, const A3ScriptFrame *f) {
    if (!w || A3_T_SCRIPT == 0xFFFFFFFFu) return;
    WorldState *ws = world_state(w, 1);
    if (!ws) return;
    ws->frame_copy = *f;
    ws->frame = &ws->frame_copy;
    a3_array_clear(ws->hud);
    for (u32 i = 0; i < g_se.insts.count; ++i) if (g_se.insts.data[i]->world == w) g_se.insts.data[i]->seen = 0;
    u32 n = 0;
    const A3Entity *ents = 0;
    a3_component_array(w, A3_T_SCRIPT, &n, &ents);
    /* entities may be created by scripts during this loop: work on a copy */
    A3Entity *list = n ? (A3Entity *)a3_malloc(sizeof(A3Entity) * n, A3_MEM_TEMP) : 0;
    if (n && !list) return;
    if (n) a3_memcpy(list, ents, sizeof(A3Entity) * n);
    for (u32 i = 0; i < n; ++i) {
        A3Entity e = list[i];
        A3CScript *sc = (A3CScript *)a3_component_get(w, e, A3_T_SCRIPT);
        if (!sc || !sc->script.path[0]) continue;
        u64 guid = a3_entity_guid(w, e);
        Inst *in = find_inst(w, guid);
        if (in && !a3_streq(in->path, sc->script.path)) {
            /* the Script field now points to another file */
            for (u32 k = 0; k < g_se.insts.count; ++k) if (g_se.insts.data[k] == in) { a3_array_remove_swap(g_se.insts, k); break; }
            inst_destroy(in);
            in = 0;
        }
        if (!in) {
            in = A3_NEW(Inst, A3_MEM_SCRIPT);
            if (!in) continue;
            in->world = w;
            in->guid = guid;
            in->entity = e;
            a3_strcpy(in->path, sizeof(in->path), sc->script.path);
            if (!a3_array_push(g_se.insts, in, A3_MEM_SCRIPT)) { a3_free(in); continue; }
        }
        in->seen = 1;
        if (!sc->active || !a3_entity_active(w, e)) continue;
        ModEntry *me = module_get(in->path);
        if (!me) continue;
        if (in->version != me->version) {
            b32 reload = in->inst != 0;
            inst_load(in, me);
            if (reload && in->inst) a3_log(A3_LOG_INFO, "script", "reloaded %s on '%s'", in->path, a3_entity_name(w, e));
        }
        if (in->failed || !in->inst) continue;
        if (!in->started) {
            in->started = 1;
            if (!inst_call(in, "on_start", 0, 0)) continue;
        }
        A3SValue dt = a3s_num(f->dt);
        inst_call(in, "on_update", &dt, 1);
    }
    a3_free(list);
    /* objects that were destroyed or lost their Script component */
    for (u32 i = 0; i < g_se.insts.count;) {
        Inst *in = g_se.insts.data[i];
        if (in->world == w && !in->seen) { a3_array_remove_swap(g_se.insts, i); inst_destroy(in); continue; }
        ++i;
    }
}

static Inst *event_inst(A3World *w, A3Entity e) {
    Inst *in = find_inst(w, a3_entity_guid(w, e));
    return in && in->inst && !in->failed && in->started ? in : 0;
}

void a3_scripts_fixed(A3World *w, f32 dt) {
    if (!w || !g_se.insts.count) return;
    const A3ContactEvent *ev = 0;
    u32 ne = a3_physics_events(w, &ev);
    for (u32 i = 0; i < ne; ++i) {
        const A3ContactEvent *c = &ev[i];
        if (c->type == A3_CONTACT_END) continue;
        const char *fn = c->type == A3_TRIGGER_ENTER ? "on_trigger_enter" : c->type == A3_TRIGGER_EXIT ? "on_trigger_exit" : "on_collision";
        for (u32 side = 0; side < 2; ++side) {
            A3Entity me = side ? c->b : c->a, other = side ? c->a : c->b;
            if (!a3_entity_valid(w, me)) continue;
            Inst *in = event_inst(w, me);
            if (!in) continue;
            A3SValue arg = a3s_entity(a3_entity_guid(w, other));
            inst_call(in, fn, &arg, 1);
        }
    }
    A3SValue d = a3s_num(dt);
    for (u32 i = 0; i < g_se.insts.count; ++i) {
        Inst *in = g_se.insts.data[i];
        if (in->world == w && in->started) inst_call(in, "on_fixed_update", &d, 1);
    }
}

void a3_scripts_stop(A3World *w) {
    for (u32 i = 0; i < g_se.insts.count;) {
        Inst *in = g_se.insts.data[i];
        if (in->world == w) { a3_array_remove_swap(g_se.insts, i); inst_destroy(in); continue; }
        ++i;
    }
    WorldState *ws = world_state(w, 0);
    if (ws) { a3_array_clear(ws->hud); ws->scene_request[0] = 0; ws->frame = 0; }
    a3_scripts_saves_flush();
}

void a3_scripts_release(A3World *w) {
    for (u32 i = 0; i < g_se.insts.count;) {
        Inst *in = g_se.insts.data[i];
        if (in->world == w) { a3_array_remove_swap(g_se.insts, i); inst_destroy(in); continue; }
        ++i;
    }
    for (u32 i = 0; i < g_se.worlds.count; ++i) {
        WorldState *ws = g_se.worlds.data[i];
        if (ws->world != w) continue;
        a3_array_free(ws->hud);
        a3_free(ws);
        a3_array_remove_swap(g_se.worlds, i);
        break;
    }
    for (u32 i = 0; i < A3_ARRAY_COUNT(g_guid_cache); ++i) if (g_guid_cache[i].w == w) a3_zero_struct(&g_guid_cache[i]);
}

u32 a3_scripts_instance_count(A3World *w) {
    u32 c = 0;
    for (u32 i = 0; i < g_se.insts.count; ++i) if (g_se.insts.data[i]->world == w && g_se.insts.data[i]->inst) c++;
    return c;
}

u32 a3_scripts_hud(A3World *w, const A3HudCmd **out) {
    WorldState *ws = world_state(w, 0);
    *out = ws ? ws->hud.data : 0;
    return ws ? ws->hud.count : 0;
}

b32 a3_scripts_take_scene_request(A3World *w, char *out, usize cap) {
    WorldState *ws = world_state(w, 0);
    if (!ws || !ws->scene_request[0]) return 0;
    a3_strcpy(out, cap, ws->scene_request);
    ws->scene_request[0] = 0;
    return 1;
}

b32 a3_scripts_send(A3World *w, A3Entity e, const char *fn, const A3SValue *args, u32 argc) {
    Inst *in = find_inst(w, a3_entity_guid(w, e));
    if (!in || !in->inst || in->failed) return 0;
    return inst_call(in, fn, args, argc);
}

static const char *const g_script_doc =
    "Runs an A3Script file on this object. Use on_start(), on_update(dt) and on_trigger_enter(other) in the script. "
    "Create one with Assets > New Script, then double-click it to edit.";

void a3_scripts_register(void) {
    a3_world_on_destroy(a3_scripts_release);
    a3s_register_core_lib();
    register_natives();
    A3SHost host = { host_get, host_set, host_print };
    a3s_set_host(&host);
    if (A3_T_SCRIPT != 0xFFFFFFFFu && a3_component_type(A3_T_SCRIPT)) return;
    A3CScript d;
    a3_zero_struct(&d);
    d.active = 1;
    d.script.kind = A3_ASSET_SCRIPT;
    u32 t = a3_component_register("Script", "Gameplay", sizeof(A3CScript), 8, &d, A3_COMP_BUILTIN, g_script_doc);
    A3_T_SCRIPT = t;
    a3_component_type(t)->icon = "script";
    A3FieldDesc *f = A3_REFLECT_FIELD(t, A3CScript, script, A3_FIELD_ASSET, "Script", "The .a3script file to run.");
    f->asset_kind = A3_ASSET_SCRIPT;
    A3_REFLECT_FIELD(t, A3CScript, active, A3_FIELD_BOOL, "Active", "Turn off to pause this script.");
}
