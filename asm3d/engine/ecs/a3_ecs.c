/*
 * ASM3D - a3_ecs.c
 */
#include "a3_ecs.h"
#include "../core/a3_log.h"
#include "../core/a3_string.h"
#include "../core/a3_hash.h"
#include "../platform/a3_platform.h"

#define NULL_INDEX 0xFFFFFFFFu

static A3Entity ent(u32 index, u32 gen) { A3Entity e = { index, gen }; return e; }

/* ======================================================================== */
/* World                                                                    */
/* ======================================================================== */

A3World *a3_world_create(const char *name) {
    A3World *w = A3_NEW(A3World, A3_MEM_ECS);
    if (!w) return 0;
    a3_strcpy(w->name, sizeof(w->name), name ? name : "World");
    w->guid_seed = a3_hash_mix64(a3_time_ns() ^ (u64)(uptr)w ^ 0xA53D0000ull);
    w->first_root = A3_ENTITY_NULL;
    w->last_root = A3_ENTITY_NULL;
    for (u32 i = 0; i < A3_MAX_COMPONENT_TYPES; ++i) w->stores[i].type_id = i;
    return w;
}

static void store_free(A3ComponentStore *s) {
    a3_free(s->data);
    a3_free(s->entities);
    a3_free(s->sparse);
    a3_free(s->layout);
    u32 id = s->type_id;
    a3_zero_struct(s);
    s->type_id = id;
}

void a3_world_clear(A3World *w) {
    if (!w) return;
    for (u32 i = 0; i < w->high_water; ++i) {
        if (w->entities[i].alive) {
            /* on_remove hooks see a consistent world */
            A3Entity e = ent(i, w->entities[i].gen);
            for (u32 t = 0; t < a3_component_type_count(); ++t) {
                A3ComponentType *ct = a3_component_type(t);
                if (ct && ct->on_remove && a3_component_has(w, e, t)) ct->on_remove(w, e, a3_component_get(w, e, t));
            }
        }
        a3_free(w->entities[i].unknown_components);
        w->entities[i].unknown_components = 0;
    }
    for (u32 i = 0; i < A3_MAX_COMPONENT_TYPES; ++i) store_free(&w->stores[i]);
    a3_free(w->entities);
    a3_free(w->free_list);
    a3_free(w->pending_destroy);
    w->entities = 0;
    w->free_list = 0;
    w->pending_destroy = 0;
    w->entity_cap = w->entity_count = w->free_count = w->free_cap = w->high_water = 0;
    w->pending_count = w->pending_cap = 0;
    w->first_root = w->last_root = A3_ENTITY_NULL;
    w->hierarchy_version++;
    w->structure_version++;
}

void a3_world_destroy(A3World *w) {
    if (!w) return;
    a3_world_clear(w);
    a3_free(w);
}

u32 a3_world_entity_count(const A3World *w) { return w ? w->entity_count : 0; }

u64 a3_world_new_guid(A3World *w) {
    u64 g;
    do { w->guid_seed += 0x9e3779b97f4a7c15ull; g = a3_hash_mix64(w->guid_seed); } while (g == 0);
    return g;
}

/* ======================================================================== */
/* Entities                                                                 */
/* ======================================================================== */

b32 a3_entity_valid(const A3World *w, A3Entity e) {
    return w && e.index < w->high_water && w->entities[e.index].alive && w->entities[e.index].gen == e.gen;
}

A3EntityRecord *a3_entity_record(A3World *w, A3Entity e) {
    return a3_entity_valid(w, e) ? &w->entities[e.index] : 0;
}

static b32 grow_entities(A3World *w) {
    u32 nc = w->entity_cap ? w->entity_cap * 2 : 256;
    A3EntityRecord *n = (A3EntityRecord *)a3_realloc(w->entities, sizeof(A3EntityRecord) * nc, A3_MEM_ECS);
    if (!n) return 0;
    a3_zero(n + w->entity_cap, sizeof(A3EntityRecord) * (nc - w->entity_cap));
    w->entities = n;
    w->entity_cap = nc;
    return 1;
}

static void root_list_append(A3World *w, A3Entity e) {
    A3EntityRecord *r = &w->entities[e.index];
    r->prev_sibling = w->last_root;
    r->next_sibling = A3_ENTITY_NULL;
    if (a3_entity_valid(w, w->last_root)) w->entities[w->last_root.index].next_sibling = e;
    else w->first_root = e;
    w->last_root = e;
}

A3Entity a3_entity_create_with_guid(A3World *w, const char *name, u64 guid) {
    if (!A3_VERIFY(w)) return A3_ENTITY_NULL;
    u32 index;
    if (w->free_count) {
        index = w->free_list[--w->free_count];
    } else {
        if (w->high_water >= w->entity_cap && !grow_entities(w)) {
            A3_ERROR("ecs", "out of memory creating entity '%s'", name ? name : "");
            return A3_ENTITY_NULL;
        }
        index = w->high_water++;
    }
    A3EntityRecord *r = &w->entities[index];
    u32 gen = r->gen + 1;
    if (gen == 0) gen = 1;
    a3_zero_struct(r);
    r->gen = gen;
    r->alive = 1;
    r->guid = guid ? guid : a3_world_new_guid(w);
    r->flags = A3_ENTITY_ACTIVE;
    a3_strcpy(r->name, sizeof(r->name), name && *name ? name : "Entity");
    r->parent = r->first_child = r->next_sibling = r->prev_sibling = A3_ENTITY_NULL;
    A3Entity e = ent(index, gen);
    root_list_append(w, e);
    w->entity_count++;
    w->structure_version++;
    w->hierarchy_version++;
    return e;
}

A3Entity a3_entity_create(A3World *w, const char *name) { return a3_entity_create_with_guid(w, name, 0); }

static void detach(A3World *w, A3Entity e) {
    A3EntityRecord *r = &w->entities[e.index];
    A3Entity prev = r->prev_sibling, next = r->next_sibling;
    if (a3_entity_valid(w, prev)) w->entities[prev.index].next_sibling = next;
    else if (a3_entity_valid(w, r->parent)) w->entities[r->parent.index].first_child = next;
    else if (a3_entity_eq(w->first_root, e)) w->first_root = next;
    if (a3_entity_valid(w, next)) w->entities[next.index].prev_sibling = prev;
    else if (!a3_entity_valid(w, r->parent) && a3_entity_eq(w->last_root, e)) w->last_root = prev;
    r->prev_sibling = r->next_sibling = A3_ENTITY_NULL;
    r->parent = A3_ENTITY_NULL;
}

void a3_entity_destroy(A3World *w, A3Entity e) {
    if (!a3_entity_valid(w, e)) return;
    /* children first */
    A3Entity c = w->entities[e.index].first_child;
    while (a3_entity_valid(w, c)) {
        A3Entity next = w->entities[c.index].next_sibling;
        a3_entity_destroy(w, c);
        c = next;
    }
    for (u32 t = 0; t < a3_component_type_count(); ++t) if (a3_component_has(w, e, t)) a3_component_remove(w, e, t);
    detach(w, e);
    A3EntityRecord *r = &w->entities[e.index];
    a3_free(r->unknown_components);
    r->unknown_components = 0;
    r->alive = 0;
    if (w->free_count >= w->free_cap) {
        u32 nc = w->free_cap ? w->free_cap * 2 : 256;
        u32 *n = (u32 *)a3_realloc(w->free_list, sizeof(u32) * nc, A3_MEM_ECS);
        if (!n) { w->entity_count--; return; } /* slot leaks, world stays consistent */
        w->free_list = n;
        w->free_cap = nc;
    }
    w->free_list[w->free_count++] = e.index;
    w->entity_count--;
    w->structure_version++;
    w->hierarchy_version++;
}

void a3_entity_destroy_deferred(A3World *w, A3Entity e) {
    A3EntityRecord *r = a3_entity_record(w, e);
    if (!r || (r->flags & A3_ENTITY_PENDING_DESTROY)) return;
    r->flags |= A3_ENTITY_PENDING_DESTROY;
    if (w->pending_count >= w->pending_cap) {
        u32 nc = w->pending_cap ? w->pending_cap * 2 : 64;
        A3Entity *n = (A3Entity *)a3_realloc(w->pending_destroy, sizeof(A3Entity) * nc, A3_MEM_ECS);
        if (!n) { a3_entity_destroy(w, e); return; }
        w->pending_destroy = n;
        w->pending_cap = nc;
    }
    w->pending_destroy[w->pending_count++] = e;
}

void a3_world_flush(A3World *w) {
    if (!w) return;
    for (u32 i = 0; i < w->pending_count; ++i) a3_entity_destroy(w, w->pending_destroy[i]);
    w->pending_count = 0;
}

const char *a3_entity_name(const A3World *w, A3Entity e) {
    return a3_entity_valid(w, e) ? w->entities[e.index].name : "(invalid)";
}

void a3_entity_set_name(A3World *w, A3Entity e, const char *name) {
    A3EntityRecord *r = a3_entity_record(w, e);
    if (r) a3_strcpy(r->name, sizeof(r->name), name ? name : "");
}

u64 a3_entity_guid(const A3World *w, A3Entity e) { return a3_entity_valid(w, e) ? w->entities[e.index].guid : 0; }

A3Entity a3_entity_find_by_guid(const A3World *w, u64 guid) {
    if (!w || !guid) return A3_ENTITY_NULL;
    for (u32 i = 0; i < w->high_water; ++i)
        if (w->entities[i].alive && w->entities[i].guid == guid) return ent(i, w->entities[i].gen);
    return A3_ENTITY_NULL;
}

A3Entity a3_entity_find_by_name(const A3World *w, const char *name) {
    if (!w || !name) return A3_ENTITY_NULL;
    for (u32 i = 0; i < w->high_water; ++i)
        if (w->entities[i].alive && a3_streq(w->entities[i].name, name)) return ent(i, w->entities[i].gen);
    return A3_ENTITY_NULL;
}

b32 a3_entity_active(const A3World *w, A3Entity e) {
    for (int guard = 0; guard < 1024 && a3_entity_valid(w, e); ++guard) {
        const A3EntityRecord *r = &w->entities[e.index];
        if (!(r->flags & A3_ENTITY_ACTIVE) || (r->flags & A3_ENTITY_PENDING_DESTROY)) return 0;
        e = r->parent;
    }
    return 1;
}

void a3_entity_set_active(A3World *w, A3Entity e, b32 active) {
    A3EntityRecord *r = a3_entity_record(w, e);
    if (!r) return;
    if (active) r->flags |= A3_ENTITY_ACTIVE; else r->flags &= ~(u32)A3_ENTITY_ACTIVE;
}

/* ======================================================================== */
/* Hierarchy                                                                */
/* ======================================================================== */

b32 a3_entity_is_ancestor(const A3World *w, A3Entity ancestor, A3Entity e) {
    if (!a3_entity_valid(w, e)) return 0;
    A3Entity p = w->entities[e.index].parent;
    for (int guard = 0; guard < 4096 && a3_entity_valid(w, p); ++guard) {
        if (a3_entity_eq(p, ancestor)) return 1;
        p = w->entities[p.index].parent;
    }
    return 0;
}

b32 a3_entity_set_parent(A3World *w, A3Entity child, A3Entity parent) {
    if (!a3_entity_valid(w, child)) return 0;
    b32 to_root = !a3_entity_valid(w, parent);
    if (!to_root && (a3_entity_eq(child, parent) || a3_entity_is_ancestor(w, child, parent))) {
        A3_WARN("ecs", "cannot parent '%s' under '%s': that would create a loop", w->entities[child.index].name, w->entities[parent.index].name);
        return 0;
    }
    detach(w, child);
    A3EntityRecord *r = &w->entities[child.index];
    if (to_root) {
        root_list_append(w, child);
    } else {
        r->parent = parent;
        A3EntityRecord *p = &w->entities[parent.index];
        /* append at end of child list to preserve order */
        A3Entity last = p->first_child;
        if (!a3_entity_valid(w, last)) {
            p->first_child = child;
        } else {
            while (a3_entity_valid(w, w->entities[last.index].next_sibling)) last = w->entities[last.index].next_sibling;
            w->entities[last.index].next_sibling = child;
            r->prev_sibling = last;
        }
    }
    w->hierarchy_version++;
    return 1;
}

A3Entity a3_entity_parent(const A3World *w, A3Entity e) { return a3_entity_valid(w, e) ? w->entities[e.index].parent : A3_ENTITY_NULL; }
A3Entity a3_entity_first_child(const A3World *w, A3Entity e) { return a3_entity_valid(w, e) ? w->entities[e.index].first_child : A3_ENTITY_NULL; }
A3Entity a3_entity_next_sibling(const A3World *w, A3Entity e) { return a3_entity_valid(w, e) ? w->entities[e.index].next_sibling : A3_ENTITY_NULL; }

u32 a3_entity_child_count(const A3World *w, A3Entity e) {
    u32 n = 0;
    for (A3Entity c = a3_entity_first_child(w, e); a3_entity_valid(w, c); c = w->entities[c.index].next_sibling) ++n;
    return n;
}

static void visit_rec(A3World *w, A3Entity e, u32 depth, A3HierarchyVisitFn fn, void *user) {
    while (a3_entity_valid(w, e)) {
        A3Entity next = w->entities[e.index].next_sibling;
        if (fn(w, e, depth, user) && depth < 256) visit_rec(w, w->entities[e.index].first_child, depth + 1, fn, user);
        e = next;
    }
}

void a3_world_visit_hierarchy(A3World *w, A3HierarchyVisitFn fn, void *user) {
    if (!w || !fn) return;
    visit_rec(w, w->first_root, 0, fn, user);
}

A3Entity a3_entity_duplicate(A3World *w, A3Entity e) {
    A3EntityRecord *r = a3_entity_record(w, e);
    if (!r) return A3_ENTITY_NULL;
    char name[A3_NAME_MAX];
    a3_strcpy(name, sizeof(name), r->name);
    A3Entity parent = r->parent;
    u32 flags = r->flags;
    A3Entity d = a3_entity_create(w, name);
    if (a3_entity_is_null(d)) return d;
    w->entities[d.index].flags = flags & ~(u32)A3_ENTITY_PENDING_DESTROY;
    for (u32 t = 0; t < a3_component_type_count(); ++t) {
        void *src = a3_component_get(w, e, t);
        if (!src) continue;
        A3ComponentType *ct = a3_component_type(t);
        void *dst = a3_component_add(w, d, t);
        src = a3_component_get(w, e, t); /* store may have grown */
        if (dst && src && ct) a3_memcpy(dst, src, ct->size);
    }
    if (w->entities[e.index].unknown_components)
        w->entities[d.index].unknown_components = a3_strdup(w->entities[e.index].unknown_components, A3_MEM_ECS);
    if (a3_entity_valid(w, parent)) a3_entity_set_parent(w, d, parent);
    /* children (snapshot list first: duplicating appends new children) */
    A3Entity kids[256];
    u32 nk = 0;
    for (A3Entity c = w->entities[e.index].first_child; a3_entity_valid(w, c) && nk < 256; c = w->entities[c.index].next_sibling) kids[nk++] = c;
    for (u32 i = 0; i < nk; ++i) {
        A3Entity dc = a3_entity_duplicate(w, kids[i]);
        if (a3_entity_valid(w, dc)) a3_entity_set_parent(w, dc, d);
    }
    return d;
}

/* ======================================================================== */
/* Component stores                                                         */
/* ======================================================================== */

static void store_snapshot_layout(A3ComponentStore *s, const A3ComponentType *t) {
    a3_free(s->layout);
    s->layout = 0;
    s->layout_count = 0;
    if (!(t->flags & A3_COMP_CUSTOM) || !t->field_count) return;
    s->layout = A3_NEW_ARRAY(A3FieldDesc, t->field_count, A3_MEM_ECS);
    if (s->layout) { a3_memcpy(s->layout, t->fields, sizeof(A3FieldDesc) * t->field_count); s->layout_count = t->field_count; }
}

/* Migrates instances of a redefined custom component: fields are matched by
 * name and type; new fields get defaults; removed fields are dropped. */
static void store_sync_layout(A3ComponentStore *s, const A3ComponentType *t) {
    if (s->layout_version == t->layout_version && (s->elem_size == t->size || s->capacity == 0)) return;
    if (s->capacity == 0) { s->elem_size = t->size; s->layout_version = t->layout_version; store_snapshot_layout(s, t); return; }
    u8 *nd = (u8 *)a3_malloc((usize)t->size * s->capacity, A3_MEM_ECS);
    if (!nd) { A3_ERROR("ecs", "out of memory migrating component '%s'", t->name); return; }
    for (u32 i = 0; i < s->count; ++i) {
        u8 *dst = nd + (usize)i * t->size;
        const u8 *src = s->data + (usize)i * s->elem_size;
        a3_memcpy(dst, t->defaults, t->size);
        for (u32 f = 0; f < s->layout_count; ++f) {
            const A3FieldDesc *of = &s->layout[f];
            const A3FieldDesc *nf = a3_component_find_field(t, of->name);
            if (nf && nf->type == of->type) a3_memcpy(dst + nf->offset, src + of->offset, nf->size);
        }
    }
    a3_free(s->data);
    s->data = nd;
    s->elem_size = t->size;
    s->layout_version = t->layout_version;
    store_snapshot_layout(s, t);
    A3_INFO("ecs", "migrated %u instance(s) of custom component '%s' to its new layout", s->count, t->name);
}

static b32 store_reserve(A3ComponentStore *s, const A3ComponentType *t, u32 entity_index) {
    store_sync_layout(s, t);
    if (entity_index >= s->sparse_cap) {
        u32 nc = s->sparse_cap ? s->sparse_cap : 256;
        while (nc <= entity_index) nc *= 2;
        u32 *n = (u32 *)a3_realloc(s->sparse, sizeof(u32) * nc, A3_MEM_ECS);
        if (!n) return 0;
        a3_zero(n + s->sparse_cap, sizeof(u32) * (nc - s->sparse_cap));
        s->sparse = n;
        s->sparse_cap = nc;
    }
    if (s->count >= s->capacity) {
        u32 nc = s->capacity ? s->capacity * 2 : 64;
        u8 *nd = (u8 *)a3_realloc(s->data, (usize)t->size * nc, A3_MEM_ECS);
        if (!nd) return 0;
        s->data = nd;
        A3Entity *ne = (A3Entity *)a3_realloc(s->entities, sizeof(A3Entity) * nc, A3_MEM_ECS);
        if (!ne) return 0;
        s->entities = ne;
        s->capacity = nc;
        s->elem_size = t->size;
        if (!s->layout && (t->flags & A3_COMP_CUSTOM)) { s->layout_version = t->layout_version; store_snapshot_layout(s, t); }
    }
    return 1;
}

void *a3_component_get(const A3World *w, A3Entity e, u32 type_id) {
    if (!a3_entity_valid(w, e) || type_id >= A3_MAX_COMPONENT_TYPES) return 0;
    const A3ComponentStore *s = &w->stores[type_id];
    if (e.index >= s->sparse_cap) return 0;
    u32 d = s->sparse[e.index];
    if (!d) return 0;
    const A3ComponentType *t = a3_component_type(type_id);
    if (t && (s->layout_version != t->layout_version || s->elem_size != t->size))
        store_sync_layout((A3ComponentStore *)s, t);
    return s->data + (usize)(d - 1) * s->elem_size;
}

b32 a3_component_has(const A3World *w, A3Entity e, u32 type_id) {
    if (!a3_entity_valid(w, e) || type_id >= A3_MAX_COMPONENT_TYPES) return 0;
    return (w->entities[e.index].mask[type_id / 64] >> (type_id % 64)) & 1u;
}

void *a3_component_add(A3World *w, A3Entity e, u32 type_id) {
    if (!a3_entity_valid(w, e)) { A3_ERROR("ecs", "add component to invalid entity"); return 0; }
    A3ComponentType *t = a3_component_type(type_id);
    if (!t) { A3_ERROR("ecs", "add component: unknown type id %u", type_id); return 0; }
    void *existing = a3_component_get(w, e, type_id);
    if (existing) return existing;
    /* requirements first (e.g. CharacterController needs Transform) */
    for (int i = 0; i < 4; ++i) {
        if (!t->requires[i][0]) continue;
        u32 rid = a3_component_id(t->requires[i]);
        if (rid == 0xFFFFFFFFu) { A3_WARN("ecs", "'%s' requires unknown component '%s'", t->name, t->requires[i]); continue; }
        if (rid != type_id && !a3_component_has(w, e, rid)) a3_component_add(w, e, rid);
    }
    A3ComponentStore *s = &w->stores[type_id];
    if (!store_reserve(s, t, e.index)) { A3_ERROR("ecs", "out of memory adding '%s'", t->name); return 0; }
    u32 d = s->count++;
    s->entities[d] = e;
    s->sparse[e.index] = d + 1;
    void *data = s->data + (usize)d * s->elem_size;
    a3_memcpy(data, t->defaults, t->size);
    w->entities[e.index].mask[type_id / 64] |= 1ull << (type_id % 64);
    w->structure_version++;
    if (t->on_add) {
        t->on_add(w, e, data);
        data = a3_component_get(w, e, type_id); /* hook may add components and move memory */
    }
    return data;
}

b32 a3_component_remove(A3World *w, A3Entity e, u32 type_id) {
    if (!a3_component_has(w, e, type_id)) return 0;
    A3ComponentType *t = a3_component_type(type_id);
    A3ComponentStore *s = &w->stores[type_id];
    if (t && t->on_remove) t->on_remove(w, e, a3_component_get(w, e, type_id));
    u32 d = s->sparse[e.index] - 1;
    u32 last = s->count - 1;
    if (d != last) {
        a3_memcpy(s->data + (usize)d * s->elem_size, s->data + (usize)last * s->elem_size, s->elem_size);
        A3Entity moved = s->entities[last];
        s->entities[d] = moved;
        s->sparse[moved.index] = d + 1;
    }
    s->count--;
    s->sparse[e.index] = 0;
    w->entities[e.index].mask[type_id / 64] &= ~(1ull << (type_id % 64));
    w->structure_version++;
    return 1;
}

u32 a3_component_count(const A3World *w, u32 type_id) {
    return (w && type_id < A3_MAX_COMPONENT_TYPES) ? w->stores[type_id].count : 0;
}

void *a3_component_add_by_name(A3World *w, A3Entity e, const char *type_name) {
    u32 id = a3_component_id(type_name);
    if (id == 0xFFFFFFFFu) { A3_ERROR("ecs", "unknown component type '%s'", type_name ? type_name : ""); return 0; }
    return a3_component_add(w, e, id);
}

void *a3_component_get_by_name(const A3World *w, A3Entity e, const char *type_name) {
    u32 id = a3_component_id(type_name);
    return id == 0xFFFFFFFFu ? 0 : a3_component_get(w, e, id);
}

void *a3_component_array(const A3World *w, u32 type_id, u32 *out_count, const A3Entity **out_entities) {
    if (!w || type_id >= A3_MAX_COMPONENT_TYPES) { if (out_count) *out_count = 0; return 0; }
    const A3ComponentStore *s = &w->stores[type_id];
    const A3ComponentType *t = a3_component_type(type_id);
    if (t && s->count && (s->layout_version != t->layout_version || s->elem_size != t->size)) store_sync_layout((A3ComponentStore *)s, t);
    if (out_count) *out_count = s->count;
    if (out_entities) *out_entities = s->entities;
    return s->data;
}

/* ======================================================================== */
/* Queries                                                                  */
/* ======================================================================== */

A3Query a3_query_begin(A3World *w, const u32 *types, u32 count) {
    A3Query q;
    a3_zero_struct(&q);
    q.world = w;
    if (count > A3_QUERY_MAX) { A3_ERROR("ecs", "query with too many component types (%u)", count); count = A3_QUERY_MAX; }
    q.type_count = count;
    u32 best = 0xFFFFFFFFu;
    for (u32 i = 0; i < count; ++i) {
        q.types[i] = types[i];
        u32 c = (w && types[i] < A3_MAX_COMPONENT_TYPES) ? w->stores[types[i]].count : 0;
        if (c < best) { best = c; q.driver = i; }
    }
    q.entity = A3_ENTITY_NULL;
    return q;
}

b32 a3_query_next(A3Query *q) {
    if (!q->world || q->type_count == 0) return 0;
    A3World *w = q->world;
    u32 dt = q->types[q->driver];
    if (dt >= A3_MAX_COMPONENT_TYPES) return 0;
    A3ComponentStore *ds = &w->stores[dt];
    while (q->cursor < ds->count) {
        A3Entity e = ds->entities[q->cursor++];
        const A3EntityRecord *r = &w->entities[e.index];
        b32 ok = 1;
        for (u32 i = 0; i < q->type_count && ok; ++i) {
            u32 t = q->types[i];
            ok = (r->mask[t / 64] >> (t % 64)) & 1u;
        }
        if (!ok) continue;
        if (!q->include_inactive && !a3_entity_active(w, e)) continue;
        q->entity = e;
        for (u32 i = 0; i < q->type_count; ++i) q->components[i] = a3_component_get(w, e, q->types[i]);
        return 1;
    }
    return 0;
}

/* ======================================================================== */
/* Clone                                                                    */
/* ======================================================================== */

A3World *a3_world_clone(const A3World *src, const char *name) {
    if (!src) return 0;
    A3World *w = a3_world_create(name ? name : src->name);
    if (!w) return 0;
    w->guid_seed = src->guid_seed ^ 0x5bd1e995ull;
    w->first_root = src->first_root;
    w->last_root = src->last_root;
    w->hierarchy_version = src->hierarchy_version;
    w->structure_version = src->structure_version;
    if (src->entity_cap) {
        w->entities = A3_NEW_ARRAY(A3EntityRecord, src->entity_cap, A3_MEM_ECS);
        if (!w->entities) goto fail;
        a3_memcpy(w->entities, src->entities, sizeof(A3EntityRecord) * src->entity_cap);
        w->entity_cap = src->entity_cap;
        for (u32 i = 0; i < src->high_water; ++i) {
            w->entities[i].unknown_components = 0;
            if (src->entities[i].unknown_components)
                w->entities[i].unknown_components = a3_strdup(src->entities[i].unknown_components, A3_MEM_ECS);
        }
    }
    w->entity_count = src->entity_count;
    w->high_water = src->high_water;
    if (src->free_cap) {
        w->free_list = A3_NEW_ARRAY(u32, src->free_cap, A3_MEM_ECS);
        if (!w->free_list) goto fail;
        a3_memcpy(w->free_list, src->free_list, sizeof(u32) * src->free_count);
        w->free_cap = src->free_cap;
        w->free_count = src->free_count;
    }
    for (u32 t = 0; t < A3_MAX_COMPONENT_TYPES; ++t) {
        const A3ComponentStore *s = &src->stores[t];
        A3ComponentStore *d = &w->stores[t];
        if (!s->capacity) continue;
        d->elem_size = s->elem_size;
        d->layout_version = s->layout_version;
        d->count = s->count;
        d->capacity = s->capacity;
        d->sparse_cap = s->sparse_cap;
        d->data = (u8 *)a3_malloc((usize)s->elem_size * s->capacity, A3_MEM_ECS);
        d->entities = A3_NEW_ARRAY(A3Entity, s->capacity, A3_MEM_ECS);
        d->sparse = A3_NEW_ARRAY(u32, s->sparse_cap, A3_MEM_ECS);
        if (!d->data || !d->entities || !d->sparse) goto fail;
        a3_memcpy(d->data, s->data, (usize)s->elem_size * s->count);
        a3_memcpy(d->entities, s->entities, sizeof(A3Entity) * s->count);
        a3_memcpy(d->sparse, s->sparse, sizeof(u32) * s->sparse_cap);
        if (s->layout_count) {
            d->layout = A3_NEW_ARRAY(A3FieldDesc, s->layout_count, A3_MEM_ECS);
            if (d->layout) { a3_memcpy(d->layout, s->layout, sizeof(A3FieldDesc) * s->layout_count); d->layout_count = s->layout_count; }
        }
    }
    return w;
fail:
    A3_ERROR("ecs", "out of memory cloning world '%s'", src->name);
    a3_world_destroy(w);
    return 0;
}
