/*
 * ASM3D - a3_reflect.c
 */
#include "a3_reflect.h"
#include "../core/a3_log.h"
#include "../core/a3_memory.h"
#include "../core/a3_string.h"

static A3ComponentType *g_types[A3_MAX_COMPONENT_TYPES];
static u32 g_type_count;

void a3_reflect_init(void) {}

void a3_reflect_shutdown(void) {
    for (u32 i = 0; i < g_type_count; ++i) {
        if (!g_types[i]) continue;
        a3_free(g_types[i]->defaults);
        a3_free(g_types[i]);
        g_types[i] = 0;
    }
    g_type_count = 0;
}

u32 a3_component_type_count(void) { return g_type_count; }

A3ComponentType *a3_component_type(u32 id) { return id < g_type_count ? g_types[id] : 0; }

A3ComponentType *a3_component_type_by_name(const char *name) {
    if (!name) return 0;
    for (u32 i = 0; i < g_type_count; ++i) if (g_types[i] && a3_streq(g_types[i]->name, name)) return g_types[i];
    return 0;
}

u32 a3_component_id(const char *name) {
    A3ComponentType *t = a3_component_type_by_name(name);
    return t ? t->id : 0xFFFFFFFFu;
}

u32 a3_component_register(const char *name, const char *category, u32 size, u32 align,
                          const void *defaults, u32 flags, const char *doc) {
    if (!name || !*name) { A3_ERROR("reflect", "component registration without a name"); return 0xFFFFFFFFu; }
    if (a3_component_type_by_name(name)) {
        A3_ERROR("reflect", "component '%s' is already registered", name);
        return 0xFFFFFFFFu;
    }
    if (g_type_count >= A3_MAX_COMPONENT_TYPES) {
        A3_ERROR("reflect", "too many component types (max %d)", A3_MAX_COMPONENT_TYPES);
        return 0xFFFFFFFFu;
    }
    A3ComponentType *t = A3_NEW(A3ComponentType, A3_MEM_ECS);
    if (!t) return 0xFFFFFFFFu;
    if (size == 0) size = 4; /* tag components still get a slot */
    t->id = g_type_count;
    a3_strcpy(t->name, sizeof(t->name), name);
    a3_strcpy(t->category, sizeof(t->category), category ? category : "General");
    t->doc = doc;
    t->size = size;
    t->align = align ? align : 4;
    t->flags = flags;
    t->defaults = a3_calloc(size, A3_MEM_ECS);
    if (!t->defaults) { a3_free(t); return 0xFFFFFFFFu; }
    if (defaults) a3_memcpy(t->defaults, defaults, size);
    g_types[g_type_count++] = t;
    return t->id;
}

u32 a3_field_type_size(A3FieldType t) {
    switch (t) {
    case A3_FIELD_BOOL: case A3_FIELD_I32: case A3_FIELD_U32: case A3_FIELD_F32: case A3_FIELD_ENUM: return 4;
    case A3_FIELD_VEC2: return 8;
    case A3_FIELD_VEC3: return 12;
    case A3_FIELD_VEC4: case A3_FIELD_COLOR: case A3_FIELD_QUAT: return 16;
    case A3_FIELD_STRING: return A3_NAME_MAX;
    case A3_FIELD_ASSET: return sizeof(A3AssetRef);
    case A3_FIELD_ENTITY: return sizeof(A3EntityRef);
    default: return 0;
    }
}

static const char *g_field_type_names[A3_FIELD_TYPE_COUNT] = {
    "bool", "int", "uint", "float", "vec2", "vec3", "vec4", "color", "rotation", "string", "enum", "asset", "entity",
};

const char *a3_field_type_name(A3FieldType t) { return (u32)t < A3_FIELD_TYPE_COUNT ? g_field_type_names[t] : "?"; }

A3FieldType a3_field_type_from_name(const char *name) {
    for (u32 i = 0; i < A3_FIELD_TYPE_COUNT; ++i) if (a3_streq(g_field_type_names[i], name)) return (A3FieldType)i;
    if (a3_streq(name, "number")) return A3_FIELD_F32;
    if (a3_streq(name, "text")) return A3_FIELD_STRING;
    return A3_FIELD_TYPE_COUNT;
}

void a3_make_label(const char *name, char *out, usize cap) {
    usize n = 0;
    if (!cap) return;
    for (usize i = 0; name && name[i] && n + 2 < cap; ++i) {
        char c = name[i];
        if (c == '_') { if (n && out[n - 1] != ' ') out[n++] = ' '; continue; }
        if (i == 0) c = (char)a3_to_upper(c);
        else if (c >= 'A' && c <= 'Z' && name[i - 1] >= 'a' && name[i - 1] <= 'z') out[n++] = ' ';
        else if (name[i - 1] == '_') c = (char)a3_to_upper(c);
        out[n++] = c;
    }
    out[n] = 0;
}

A3FieldDesc *a3_component_add_field(u32 type_id, const char *name, const char *label, A3FieldType type,
                                    u32 offset, const char *doc) {
    A3ComponentType *t = a3_component_type(type_id);
    if (!t) { A3_ERROR("reflect", "add_field('%s'): invalid component type id %u", name ? name : "?", type_id); return 0; }
    if (t->field_count >= A3_MAX_FIELDS) { A3_ERROR("reflect", "component '%s' has too many fields", t->name); return 0; }
    u32 fsize = a3_field_type_size(type);
    if (offset + fsize > t->size) {
        A3_ERROR("reflect", "field '%s.%s' (offset %u, size %u) exceeds component size %u", t->name, name, offset, fsize, t->size);
        return 0;
    }
    A3FieldDesc *f = &t->fields[t->field_count++];
    a3_zero_struct(f);
    a3_strcpy(f->name, sizeof(f->name), name);
    if (label && *label) a3_strcpy(f->label, sizeof(f->label), label);
    else a3_make_label(name, f->label, sizeof(f->label));
    f->type = type;
    f->offset = offset;
    f->size = fsize;
    f->doc = doc;
    f->step = (type == A3_FIELD_I32 || type == A3_FIELD_U32) ? 1.0f : 0.1f;
    return f;
}

static A3FieldDesc g_dummy_field; /* absorbs chained writes after a failed add */

A3FieldDesc *a3__field_or_dummy(A3FieldDesc *f) { return f ? f : &g_dummy_field; }

A3FieldDesc *a3_field_range(A3FieldDesc *f, f32 min, f32 max, f32 step) {
    if (!f) return &g_dummy_field;
    f->min = min;
    f->max = max;
    if (step > 0) f->step = step;
    return f;
}

void a3_component_require(u32 type_id, const char *required) {
    A3ComponentType *t = a3_component_type(type_id);
    if (!t) return;
    for (int i = 0; i < 4; ++i) {
        if (!t->requires[i][0]) { a3_strcpy(t->requires[i], A3_NAME_MAX, required); return; }
    }
    A3_WARN("reflect", "component '%s' has too many requirements", t->name);
}

void a3_component_suggest(u32 type_id, const char *next, const char *reason) {
    A3ComponentType *t = a3_component_type(type_id);
    if (!t) return;
    t->suggest_next = next;
    t->suggest_reason = reason;
}

const A3FieldDesc *a3_component_find_field(const A3ComponentType *t, const char *name) {
    if (!t || !name) return 0;
    for (u32 i = 0; i < t->field_count; ++i) if (a3_streq(t->fields[i].name, name)) return &t->fields[i];
    return 0;
}

static void write_custom_default(u8 *base, const A3FieldDesc *f, const A3CustomFieldDef *d) {
    u8 *p = base + f->offset;
    switch (f->type) {
    case A3_FIELD_BOOL: { b32 v = d->default_value[0] != 0.0f; a3_memcpy(p, &v, 4); } break;
    case A3_FIELD_I32: case A3_FIELD_ENUM: { i32 v = (i32)d->default_value[0]; a3_memcpy(p, &v, 4); } break;
    case A3_FIELD_U32: { u32 v = (u32)d->default_value[0]; a3_memcpy(p, &v, 4); } break;
    case A3_FIELD_F32: case A3_FIELD_VEC2: case A3_FIELD_VEC3: case A3_FIELD_VEC4: case A3_FIELD_COLOR:
        a3_memcpy(p, d->default_value, f->size); break;
    case A3_FIELD_QUAT: {
        A3Quat q = a3_quat_euler(d->default_value[0] * A3_DEG2RAD, d->default_value[1] * A3_DEG2RAD, d->default_value[2] * A3_DEG2RAD);
        a3_memcpy(p, &q, sizeof(q));
    } break;
    case A3_FIELD_STRING: a3_strcpy((char *)p, A3_NAME_MAX, d->default_string); break;
    case A3_FIELD_ASSET: a3_strcpy(((A3AssetRef *)p)->path, sizeof(((A3AssetRef *)p)->path), d->default_string); break;
    default: break;
    }
}

u32 a3_custom_component_define(const char *name, const A3CustomFieldDef *fields, u32 field_count, const char *doc) {
    if (field_count > A3_MAX_FIELDS) {
        A3_ERROR("reflect", "custom component '%s': too many fields (%u, max %d)", name, field_count, A3_MAX_FIELDS);
        return 0xFFFFFFFFu;
    }
    /* layout */
    u32 offset = 0;
    u32 offsets[A3_MAX_FIELDS];
    for (u32 i = 0; i < field_count; ++i) {
        u32 fs = a3_field_type_size(fields[i].type);
        if (!fs) { A3_ERROR("reflect", "custom component '%s': field '%s' has an invalid type", name, fields[i].name); return 0xFFFFFFFFu; }
        u32 al = fs >= 16 ? 16 : 4;
        offset = A3_ALIGN_UP(offset, al);
        offsets[i] = offset;
        offset += fs;
    }
    u32 size = A3_ALIGN_UP(offset ? offset : 4, 16);

    A3ComponentType *t = a3_component_type_by_name(name);
    if (t) {
        if (!(t->flags & A3_COMP_CUSTOM)) {
            A3_ERROR("reflect", "cannot redefine built-in component '%s'", name);
            return 0xFFFFFFFFu;
        }
        void *nd = a3_calloc(size, A3_MEM_ECS);
        if (!nd) return 0xFFFFFFFFu;
        a3_free(t->defaults);
        t->defaults = nd;
        t->size = size;
        t->field_count = 0;
        t->layout_version++;
    } else {
        u32 id = a3_component_register(name, "Custom", size, 16, 0, A3_COMP_CUSTOM, doc);
        if (id == 0xFFFFFFFFu) return id;
        t = a3_component_type(id);
    }
    t->doc = doc;
    for (u32 i = 0; i < field_count; ++i) {
        A3FieldDesc *f = a3_component_add_field(t->id, fields[i].name, 0, fields[i].type, offsets[i], fields[i].doc);
        if (!f) return 0xFFFFFFFFu;
        if (fields[i].min != fields[i].max) a3_field_range(f, fields[i].min, fields[i].max, 0);
        write_custom_default((u8 *)t->defaults, f, &fields[i]);
    }
    return t->id;
}
