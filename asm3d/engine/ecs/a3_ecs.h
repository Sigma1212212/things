/*
 * ASM3D - a3_ecs.h
 * Entity Component System.
 *
 * - Entities are generational handles {index, gen}; stale handles are
 *   detected (a3_entity_valid) instead of touching recycled slots.
 * - Each entity also has a stable 64-bit GUID used for serialization,
 *   prefabs, entity references and networking.
 * - Components live in per-type sparse sets: dense, contiguous arrays that
 *   iterate linearly (cache friendly) with O(1) add/remove/lookup.
 * - Entities form a hierarchy (parent / children) used by transforms.
 */
#ifndef A3_ECS_H
#define A3_ECS_H

#include "a3_reflect.h"
#include "../core/a3_memory.h"

A3_EXTERN_C_BEGIN

#define A3_MASK_WORDS (A3_MAX_COMPONENT_TYPES / 64)

typedef enum A3EntityFlags {
    A3_ENTITY_ACTIVE = 1 << 0,         /* inactive entities are skipped by systems */
    A3_ENTITY_EDITOR_ONLY = 1 << 1,    /* never saved into builds */
    A3_ENTITY_DONT_SAVE = 1 << 2,      /* runtime-spawned, not written to scenes */
    A3_ENTITY_PENDING_DESTROY = 1 << 3,
    A3_ENTITY_STATIC = 1 << 4,         /* never moves: enables batching/baking */
} A3EntityFlags;

typedef struct A3EntityRecord {
    u32 gen;
    b32 alive;
    u64 guid;
    u32 flags;
    char name[A3_NAME_MAX];
    A3Entity parent, first_child, next_sibling, prev_sibling;
    u64 mask[A3_MASK_WORDS];
    char *unknown_components; /* JSON of components from missing plugins, preserved on save */
    char prefab[A3_PATH_MAX - 8];   /* source prefab path, if instantiated from one */
} A3EntityRecord;

typedef struct A3ComponentStore {
    u32 type_id;
    u32 elem_size;
    u32 layout_version;
    u32 count, capacity;
    u8 *data;
    A3Entity *entities;   /* dense -> entity */
    u32 *sparse;          /* entity index -> dense index + 1 (0 = absent) */
    u32 sparse_cap;
    /* snapshot of field layout for custom component migration */
    A3FieldDesc *layout;
    u32 layout_count;
} A3ComponentStore;

struct A3World {
    A3EntityRecord *entities;
    u32 entity_cap;
    u32 entity_count;      /* live entities */
    u32 *free_list;
    u32 free_count, free_cap;
    u32 high_water;        /* slots ever used */
    A3ComponentStore stores[A3_MAX_COMPONENT_TYPES];
    A3Entity *pending_destroy;
    u32 pending_count, pending_cap;
    u64 guid_seed;
    A3Entity first_root, last_root; /* ordered root list (hierarchy panel order) */
    u32 hierarchy_version; /* bumped on any parent/child change */
    u32 structure_version; /* bumped on create/destroy/add/remove */
    u32 loading;           /* > 0 while deserializing: on_add hooks are skipped (data comes from the file) */
    void *user;            /* owner (scene, editor) */
    char name[A3_NAME_MAX];
};

/* ---- World ---- */
A3World *a3_world_create(const char *name);
void     a3_world_destroy(A3World *w);
void     a3_world_clear(A3World *w);
/* Applies deferred destroys; call once per frame after systems run. */
void     a3_world_flush(A3World *w);
u32      a3_world_entity_count(const A3World *w);
/* Modules with per-world state (physics, particles) register a cleanup that
 * runs whenever any world is destroyed. */
typedef void (*A3WorldDestroyFn)(A3World *w);
void     a3_world_on_destroy(A3WorldDestroyFn fn);
/* Deep copy (used to enter Play mode without touching the edited scene). */
A3World *a3_world_clone(const A3World *src, const char *name);

/* ---- Entities ---- */
A3Entity a3_entity_create(A3World *w, const char *name);
A3Entity a3_entity_create_with_guid(A3World *w, const char *name, u64 guid);
void     a3_entity_destroy(A3World *w, A3Entity e);          /* immediate, recursive */
void     a3_entity_destroy_deferred(A3World *w, A3Entity e); /* safe during iteration */
b32      a3_entity_valid(const A3World *w, A3Entity e);
A3_INLINE b32 a3_entity_is_null(A3Entity e) { return e.index == 0xFFFFFFFFu; }
A3_INLINE b32 a3_entity_eq(A3Entity a, A3Entity b) { return a.index == b.index && a.gen == b.gen; }
A3EntityRecord *a3_entity_record(A3World *w, A3Entity e);   /* NULL if invalid */
const char *a3_entity_name(const A3World *w, A3Entity e);
void     a3_entity_set_name(A3World *w, A3Entity e, const char *name);
u64      a3_entity_guid(const A3World *w, A3Entity e);
A3Entity a3_entity_find_by_guid(const A3World *w, u64 guid);
A3Entity a3_entity_find_by_name(const A3World *w, const char *name);
b32      a3_entity_active(const A3World *w, A3Entity e);     /* self and all parents active */
void     a3_entity_set_active(A3World *w, A3Entity e, b32 active);
u64      a3_world_new_guid(A3World *w);
/* Duplicates an entity and its children (new GUIDs). */
A3Entity a3_entity_duplicate(A3World *w, A3Entity e);

/* ---- Hierarchy ---- */
b32      a3_entity_set_parent(A3World *w, A3Entity child, A3Entity parent); /* NULL parent = root; rejects cycles */
/* Moves e to position `index` among its siblings (clamped). */
void     a3_entity_set_sibling_index(A3World *w, A3Entity e, u32 index);
A3Entity a3_entity_parent(const A3World *w, A3Entity e);
A3Entity a3_entity_first_child(const A3World *w, A3Entity e);
A3Entity a3_entity_next_sibling(const A3World *w, A3Entity e);
u32      a3_entity_child_count(const A3World *w, A3Entity e);
b32      a3_entity_is_ancestor(const A3World *w, A3Entity ancestor, A3Entity e);
/* Visits roots and descendants depth-first. Return false to skip children. */
typedef b32 (*A3HierarchyVisitFn)(A3World *w, A3Entity e, u32 depth, void *user);
void     a3_world_visit_hierarchy(A3World *w, A3HierarchyVisitFn fn, void *user);

/* ---- Components ---- */
void *a3_component_add(A3World *w, A3Entity e, u32 type_id);     /* returns existing if present */
void *a3_component_get(const A3World *w, A3Entity e, u32 type_id);
b32   a3_component_has(const A3World *w, A3Entity e, u32 type_id);
b32   a3_component_remove(A3World *w, A3Entity e, u32 type_id);
u32   a3_component_count(const A3World *w, u32 type_id);
void *a3_component_add_by_name(A3World *w, A3Entity e, const char *type_name);
void *a3_component_get_by_name(const A3World *w, A3Entity e, const char *type_name);
/* Dense access for systems: data pointer + entity array of a store. */
void *a3_component_array(const A3World *w, u32 type_id, u32 *out_count, const A3Entity **out_entities);

#define A3_GET(world, e, Type, type_id) ((Type *)a3_component_get((world), (e), (type_id)))
#define A3_ADD(world, e, Type, type_id) ((Type *)a3_component_add((world), (e), (type_id)))

/* ---- Queries ----
 *   u32 types[] = { T_TRANSFORM, T_MESH };
 *   A3Query q = a3_query_begin(w, types, 2);
 *   while (a3_query_next(&q)) { A3Transform3D *t = q.components[0]; ... }
 */
#define A3_QUERY_MAX 8
typedef struct A3Query {
    A3World *world;
    u32 types[A3_QUERY_MAX];
    u32 type_count;
    u32 driver;          /* index into types of smallest store */
    u32 cursor;
    b32 include_inactive;
    A3Entity entity;
    void *components[A3_QUERY_MAX];
} A3Query;

A3Query a3_query_begin(A3World *w, const u32 *types, u32 count);
b32     a3_query_next(A3Query *q);

A3_EXTERN_C_END

#endif
