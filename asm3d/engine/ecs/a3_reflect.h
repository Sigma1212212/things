/*
 * ASM3D - a3_reflect.h
 * Component reflection. Every component type (built-in, plugin or custom
 * defined in the editor) is described by field descriptors. The descriptors
 * drive:
 *   - scene / save serialization (by field name, never raw memory)
 *   - the auto-generated Inspector UI (labels, ranges, docs, enums)
 *   - built-in documentation panel
 *   - scripting access (entity.Health.value)
 */
#ifndef A3_REFLECT_H
#define A3_REFLECT_H

#include "../core/a3_base.h"
#include "../core/a3_math.h"

A3_EXTERN_C_BEGIN

#define A3_MAX_COMPONENT_TYPES 128
#define A3_MAX_FIELDS 32
#define A3_NAME_MAX 64
#define A3_PATH_MAX 256

typedef enum A3FieldType {
    A3_FIELD_BOOL = 0,   /* b32 */
    A3_FIELD_I32,
    A3_FIELD_U32,
    A3_FIELD_F32,
    A3_FIELD_VEC2,
    A3_FIELD_VEC3,
    A3_FIELD_VEC4,
    A3_FIELD_COLOR,      /* A3Vec4 rgba, shown as a color picker */
    A3_FIELD_QUAT,       /* shown as euler degrees in the inspector */
    A3_FIELD_STRING,     /* char[A3_NAME_MAX] */
    A3_FIELD_ENUM,       /* i32 index into enum_names */
    A3_FIELD_ASSET,      /* A3AssetRef */
    A3_FIELD_ENTITY,     /* A3EntityRef */
    A3_FIELD_TYPE_COUNT
} A3FieldType;

typedef enum A3FieldFlags {
    A3_FIELD_FLAG_NONE = 0,
    A3_FIELD_FLAG_ADVANCED = 1 << 0,  /* hidden in Beginner Mode */
    A3_FIELD_FLAG_READONLY = 1 << 1,
    A3_FIELD_FLAG_HIDDEN = 1 << 2,    /* not shown, still serialized */
    A3_FIELD_FLAG_TRANSIENT = 1 << 3, /* shown, not serialized (runtime state) */
    A3_FIELD_FLAG_SLIDER = 1 << 4,    /* draw as slider using min/max */
} A3FieldFlags;

typedef enum A3AssetKind {
    A3_ASSET_ANY = 0,
    A3_ASSET_MESH,
    A3_ASSET_TEXTURE,
    A3_ASSET_MATERIAL,
    A3_ASSET_SHADER,
    A3_ASSET_SOUND,
    A3_ASSET_SCENE,
    A3_ASSET_SCRIPT,
    A3_ASSET_GRAPH,     /* visual script */
    A3_ASSET_PREFAB,
    A3_ASSET_ANIMATION,
    A3_ASSET_KIND_COUNT
} A3AssetKind;

/* Reference to an asset by project-relative path (stable, human readable).
 * `handle` caches the loaded resource and is never serialized. */
typedef struct A3AssetRef {
    char path[A3_PATH_MAX - 8];
    u32 handle;
    u32 kind;
} A3AssetRef;

/* Reference to another entity by GUID (stable across save/load). */
typedef struct A3EntityRef {
    u64 guid;
    u32 cached_index;  /* runtime cache, never serialized */
    u32 cached_gen;
} A3EntityRef;

typedef struct A3FieldDesc {
    char name[A3_NAME_MAX];     /* serialization key, e.g. "maxSpeed" */
    char label[A3_NAME_MAX];    /* inspector label, e.g. "Max Speed" */
    A3FieldType type;
    u32 offset;
    u32 size;
    f32 min, max, step;         /* min == max means unbounded */
    u32 flags;
    u32 asset_kind;             /* for A3_FIELD_ASSET */
    const char *doc;            /* plain-language description */
    const char *const *enum_names;
    u32 enum_count;
} A3FieldDesc;

typedef struct A3World A3World;
typedef struct A3Entity { u32 index; u32 gen; } A3Entity;
#define A3_ENTITY_NULL ((A3Entity){ 0xFFFFFFFFu, 0 })

typedef void (*A3ComponentHook)(A3World *world, A3Entity e, void *data);

typedef enum A3ComponentFlags {
    A3_COMP_BUILTIN = 1 << 0,
    A3_COMP_CUSTOM = 1 << 1,      /* defined by the user in the editor */
    A3_COMP_HIDDEN = 1 << 2,      /* not listed in "Add Component" */
    A3_COMP_UNIQUE = 1 << 3,      /* one per world, e.g. WorldSettings */
    A3_COMP_EDITOR_ONLY = 1 << 4, /* stripped from builds */
} A3ComponentFlags;

typedef struct A3ComponentType {
    u32 id;
    char name[A3_NAME_MAX];
    char category[32];          /* "Rendering", "Physics", "Gameplay"... */
    const char *doc;
    const char *icon;           /* icon name for the editor */
    u32 size;
    u32 align;
    u32 flags;
    A3FieldDesc fields[A3_MAX_FIELDS];
    u32 field_count;
    void *defaults;             /* size bytes: default values for new instances */
    char requires[4][A3_NAME_MAX]; /* components auto-added before this one */
    const char *suggest_next;   /* beginner guidance, e.g. "Camera" */
    const char *suggest_reason;
    A3ComponentHook on_add;     /* called after default init */
    A3ComponentHook on_remove;
    u32 module;                 /* engine module that implements it (build stripping) */
    u32 layout_version;         /* bumped when a custom component is redefined */
} A3ComponentType;

/* ---- Registry (process wide) ---- */
void a3_reflect_init(void);
void a3_reflect_shutdown(void);

/* Registers a type. `defaults` (optional) is copied. Returns the type id or
 * 0xFFFFFFFF on failure (duplicate name, table full). */
u32 a3_component_register(const char *name, const char *category, u32 size, u32 align,
                          const void *defaults, u32 flags, const char *doc);
A3ComponentType *a3_component_type(u32 id);
A3ComponentType *a3_component_type_by_name(const char *name);
u32  a3_component_id(const char *name); /* 0xFFFFFFFF when unknown */
u32  a3_component_type_count(void);

/* Adds a field descriptor to a registered type. */
A3FieldDesc *a3_component_add_field(u32 type_id, const char *name, const char *label, A3FieldType type,
                                    u32 offset, const char *doc);
A3FieldDesc *a3_field_range(A3FieldDesc *f, f32 min, f32 max, f32 step); /* returns f for chaining */
void a3_component_require(u32 type_id, const char *required_type);
void a3_component_suggest(u32 type_id, const char *next_component, const char *reason);

/* ---- Custom components (data-only, defined at runtime / in the editor) ---- */
typedef struct A3CustomFieldDef {
    char name[A3_NAME_MAX];
    A3FieldType type;
    f32 default_value[4];     /* numbers / vectors / colors / bool */
    char default_string[A3_NAME_MAX];
    f32 min, max;
    const char *doc;
} A3CustomFieldDef;

/* Creates or redefines a custom component. Field offsets are computed
 * automatically. Redefinition keeps the id so existing references stay valid;
 * existing instances are migrated field-by-field by name (see ECS). */
u32 a3_custom_component_define(const char *name, const A3CustomFieldDef *fields, u32 field_count, const char *doc);

/* ---- Field helpers ---- */
u32  a3_field_type_size(A3FieldType t);
const char *a3_field_type_name(A3FieldType t);
A3FieldType a3_field_type_from_name(const char *name);
const A3FieldDesc *a3_component_find_field(const A3ComponentType *t, const char *name);
/* Human readable label from a camelCase name: "maxSpeed" -> "Max Speed". */
void a3_make_label(const char *name, char *out, usize cap);

/* Never returns NULL: on failure (logged) a scratch descriptor is returned so
 * chained setters like A3_REFLECT_FIELD(...)->flags |= X stay safe. */
A3FieldDesc *a3__field_or_dummy(A3FieldDesc *f);
#define A3_REFLECT_FIELD(type_id, StructT, member, ftype, label, doc) \
    a3__field_or_dummy(a3_component_add_field((type_id), #member, (label), (ftype), (u32)A3_OFFSETOF(StructT, member), (doc)))

A3_EXTERN_C_END

#endif
