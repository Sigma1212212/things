/*
 * ASM3D - a3_emesh.c
 * Editable polygon meshes (see a3_emesh.h).
 */
#include "a3_emesh.h"
#include "a3_modeling_kernels.h"
#include "../core/a3_string.h"
#include "../core/a3_format.h"
#include "../core/a3_hash.h"
#include "../core/a3_log.h"
#include "../render/a3_mesh.h"

#define TAG A3_MEM_WORLD

const char *const a3_eprim_names[A3_EPRIM_COUNT] = { "cube", "plane", "grid", "cylinder", "cone", "sphere", "torus" };

i32 a3_emesh_primitive_from_name(const char *name) {
    for (u32 i = 0; i < A3_EPRIM_COUNT; ++i) if (name && a3_streq(a3_eprim_names[i], name)) return (i32)i;
    return -1;
}

/* ======================================================================== */
/* Lifetime and building                                                    */
/* ======================================================================== */

void a3_emesh_init(A3EMesh *m) { a3_zero_struct(m); }

void a3_emesh_free(A3EMesh *m) {
    a3_array_free(m->pos);
    a3_array_free(m->vsel);
    a3_array_free(m->fstart);
    a3_array_free(m->fsize);
    a3_array_free(m->fsel);
    a3_array_free(m->loops);
    a3_array_free(m->edges);
    a3_zero_struct(m);
}

void a3_emesh_clear(A3EMesh *m) {
    a3_array_clear(m->pos);
    a3_array_clear(m->vsel);
    a3_array_clear(m->fstart);
    a3_array_clear(m->fsize);
    a3_array_clear(m->fsel);
    a3_array_clear(m->loops);
    a3_array_clear(m->edges);
    m->edges_valid = 0;
}

#define COPY_ARRAY(dst, src) do { a3_array_clear(dst); if ((src).count) { if (!a3_array_reserve(dst, (src).count, TAG)) return 0; a3_memcpy((dst).data, (src).data, sizeof(*(src).data) * (src).count); } (dst).count = (src).count; } while (0)

b32 a3_emesh_copy(A3EMesh *dst, const A3EMesh *src) {
    if (dst == src) return 1;
    COPY_ARRAY(dst->pos, src->pos);
    COPY_ARRAY(dst->vsel, src->vsel);
    COPY_ARRAY(dst->fstart, src->fstart);
    COPY_ARRAY(dst->fsize, src->fsize);
    COPY_ARRAY(dst->fsel, src->fsel);
    COPY_ARRAY(dst->loops, src->loops);
    a3_array_clear(dst->edges);
    dst->edges_valid = 0;
    dst->select_mode = src->select_mode;
    dst->smooth = src->smooth;
    return 1;
}

u32 a3_emesh_add_vertex(A3EMesh *m, A3Vec3 p) {
    if (!a3_array_push(m->pos, a3_v4_from3(p, 1.0f), TAG)) return A3_ENONE;
    if (!a3_array_push(m->vsel, (u8)0, TAG)) { m->pos.count--; return A3_ENONE; }
    return m->pos.count - 1;
}

void a3_emesh_set_vertex(A3EMesh *m, u32 v, A3Vec3 p) { if (v < m->pos.count) m->pos.data[v] = a3_v4_from3(p, 1.0f); }

u32 a3_emesh_add_face(A3EMesh *m, const u32 *verts, u32 n) {
    if (n < 3) return A3_ENONE;
    for (u32 i = 0; i < n; ++i) {
        if (verts[i] >= m->pos.count) return A3_ENONE;
        if (verts[i] == verts[(i + 1) % n]) return A3_ENONE;
    }
    u32 start = m->loops.count;
    for (u32 i = 0; i < n; ++i) if (!a3_array_push(m->loops, verts[i], TAG)) { m->loops.count = start; return A3_ENONE; }
    a3_array_push(m->fstart, start, TAG);
    a3_array_push(m->fsize, n, TAG);
    a3_array_push(m->fsel, (u8)0, TAG);
    m->edges_valid = 0;
    return m->fsize.count - 1;
}

/* ---- rebuilding face lists (used by every topology operation) ---- */

typedef struct FaceBuild {
    A3_ARRAY_TYPE(u32) fstart, fsize, loops;
    A3_ARRAY_TYPE(u8) fsel;
} FaceBuild;

static void fb_add(FaceBuild *b, const u32 *v, u32 n, u8 sel) {
    if (n < 3) return;
    a3_array_push(b->fstart, b->loops.count, TAG);
    a3_array_push(b->fsize, n, TAG);
    a3_array_push(b->fsel, sel, TAG);
    for (u32 i = 0; i < n; ++i) a3_array_push(b->loops, v[i], TAG);
}

static void fb_commit(A3EMesh *m, FaceBuild *b) {
    a3_array_free(m->fstart);
    a3_array_free(m->fsize);
    a3_array_free(m->loops);
    a3_array_free(m->fsel);
    m->fstart.data = b->fstart.data; m->fstart.count = b->fstart.count; m->fstart.cap = b->fstart.cap;
    m->fsize.data = b->fsize.data; m->fsize.count = b->fsize.count; m->fsize.cap = b->fsize.cap;
    m->loops.data = b->loops.data; m->loops.count = b->loops.count; m->loops.cap = b->loops.cap;
    m->fsel.data = b->fsel.data; m->fsel.count = b->fsel.count; m->fsel.cap = b->fsel.cap;
    a3_zero_struct(b);
    m->edges_valid = 0;
}

static void fb_free(FaceBuild *b) {
    a3_array_free(b->fstart);
    a3_array_free(b->fsize);
    a3_array_free(b->loops);
    a3_array_free(b->fsel);
}

/* ======================================================================== */
/* Geometry                                                                 */
/* ======================================================================== */

static A3Vec3 P(const A3EMesh *m, u32 v) { return a3_v4_xyz(m->pos.data[v]); }

A3Vec3 a3_emesh_face_normal(const A3EMesh *m, u32 f) {
    u32 n;
    const u32 *v = a3_emesh_face(m, f, &n);
    A3Vec3 r = a3_v3(0, 0, 0);
    for (u32 i = 0; i < n; ++i) {
        A3Vec3 a = P(m, v[i]), b = P(m, v[(i + 1) % n]);
        r.x += (a.y - b.y) * (a.z + b.z);
        r.y += (a.z - b.z) * (a.x + b.x);
        r.z += (a.x - b.x) * (a.y + b.y);
    }
    return a3_v3_norm(r);
}

A3Vec3 a3_emesh_face_center(const A3EMesh *m, u32 f) {
    u32 n;
    const u32 *v = a3_emesh_face(m, f, &n);
    A3Vec3 c = a3_v3(0, 0, 0);
    for (u32 i = 0; i < n; ++i) c = a3_v3_add(c, P(m, v[i]));
    return a3_v3_scale(c, 1.0f / (f32)n);
}

static u64 ekey(u32 a, u32 b) { return a < b ? ((u64)a << 32) | b : ((u64)b << 32) | a; }

void a3_emesh_edges(A3EMesh *m) {
    if (m->edges_valid) return;
    a3_array_clear(m->edges);
    A3HashMap map;
    a3_hashmap_init(&map, m->loops.count + 16, A3_MEM_TEMP);
    for (u32 f = 0; f < m->fsize.count; ++f) {
        u32 n;
        const u32 *v = a3_emesh_face(m, f, &n);
        for (u32 i = 0; i < n; ++i) {
            u32 a = v[i], b = v[(i + 1) % n];
            u64 key = ekey(a, b), idx;
            if (a3_hashmap_get(&map, key, &idx)) {
                A3EEdge *e = &m->edges.data[idx - 1];
                if (e->face_count == 1) e->f1 = f;
                e->face_count++;
            } else {
                A3EEdge e = { a < b ? a : b, a < b ? b : a, f, A3_ENONE, 1 };
                a3_array_push(m->edges, e, TAG);
                a3_hashmap_put(&map, key, m->edges.count);
            }
        }
    }
    a3_hashmap_free(&map);
    m->edges_valid = 1;
}

u32 a3_emesh_find_edge(A3EMesh *m, u32 a, u32 b) {
    a3_emesh_edges(m);
    u32 lo = a < b ? a : b, hi = a < b ? b : a;
    for (u32 i = 0; i < m->edges.count; ++i) if (m->edges.data[i].a == lo && m->edges.data[i].b == hi) return i;
    return A3_ENONE;
}

/* edge lookup map for the current edge table (caller frees) */
static void edge_map(A3EMesh *m, A3HashMap *map) {
    a3_emesh_edges(m);
    a3_hashmap_init(map, m->edges.count * 2 + 16, A3_MEM_TEMP);
    for (u32 i = 0; i < m->edges.count; ++i) a3_hashmap_put(map, ekey(m->edges.data[i].a, m->edges.data[i].b), i + 1);
}

static u32 edge_of(const A3HashMap *map, u32 a, u32 b) {
    u64 v;
    return a3_hashmap_get(map, ekey(a, b), &v) ? (u32)(v - 1) : A3_ENONE;
}

void a3_emesh_stats(A3EMesh *m, A3EMeshStats *s) {
    a3_zero_struct(s);
    a3_emesh_edges(m);
    a3_emesh_sync_selection(m);
    s->vertices = m->pos.count;
    s->edges = m->edges.count;
    s->faces = m->fsize.count;
    for (u32 f = 0; f < m->fsize.count; ++f) { s->triangles += m->fsize.data[f] - 2; if (m->fsel.data[f]) s->selected_faces++; }
    for (u32 v = 0; v < m->pos.count; ++v) if (m->vsel.data[v]) s->selected_vertices++;
    for (u32 e = 0; e < m->edges.count; ++e) {
        const A3EEdge *ed = &m->edges.data[e];
        if (ed->face_count == 1) s->boundary_edges++;
        if (ed->face_count > 2) s->nonmanifold_edges++;
        if (m->vsel.data[ed->a] && m->vsel.data[ed->b]) s->selected_edges++;
    }
    s->closed = m->edges.count > 0 && s->boundary_edges == 0 && s->nonmanifold_edges == 0;
    if (m->pos.count) {
        A3Vec4 mn, mx;
        a3_mk_bounds(m->pos.data, m->pos.count, &mn, &mx);
        s->min = a3_v4_xyz(mn);
        s->max = a3_v4_xyz(mx);
    }
}

/* ======================================================================== */
/* Selection                                                                */
/* ======================================================================== */

static void faces_from_vertices(A3EMesh *m) {
    for (u32 f = 0; f < m->fsize.count; ++f) {
        u32 n;
        const u32 *v = a3_emesh_face(m, f, &n);
        u8 all = 1;
        for (u32 i = 0; i < n && all; ++i) if (!m->vsel.data[v[i]]) all = 0;
        m->fsel.data[f] = all;
    }
}

static void vertices_from_faces(A3EMesh *m) {
    a3_memset(m->vsel.data, 0, m->vsel.count);
    for (u32 f = 0; f < m->fsize.count; ++f) {
        if (!m->fsel.data[f]) continue;
        u32 n;
        const u32 *v = a3_emesh_face(m, f, &n);
        for (u32 i = 0; i < n; ++i) m->vsel.data[v[i]] = 1;
    }
}

void a3_emesh_sync_selection(A3EMesh *m) {
    if (m->select_mode == A3_ESEL_FACE) vertices_from_faces(m);
    else faces_from_vertices(m);
}

void a3_emesh_set_select_mode(A3EMesh *m, A3ESelectMode mode) {
    if (mode == A3_ESEL_FACE && m->select_mode != A3_ESEL_FACE) faces_from_vertices(m);
    m->select_mode = mode;
    a3_emesh_sync_selection(m);
}

void a3_emesh_select_all(A3EMesh *m, b32 select) {
    a3_memset(m->vsel.data, select ? 1 : 0, m->vsel.count);
    a3_memset(m->fsel.data, select ? 1 : 0, m->fsel.count);
}

void a3_emesh_select_invert(A3EMesh *m) {
    if (m->select_mode == A3_ESEL_FACE) { for (u32 f = 0; f < m->fsel.count; ++f) m->fsel.data[f] = !m->fsel.data[f]; }
    else for (u32 v = 0; v < m->vsel.count; ++v) m->vsel.data[v] = !m->vsel.data[v];
    a3_emesh_sync_selection(m);
}

void a3_emesh_select_vertex(A3EMesh *m, u32 v, b32 select) {
    if (v >= m->pos.count) return;
    m->vsel.data[v] = select ? 1 : 0;
    faces_from_vertices(m);
}

void a3_emesh_select_edge(A3EMesh *m, u32 e, b32 select) {
    a3_emesh_edges(m);
    if (e >= m->edges.count) return;
    m->vsel.data[m->edges.data[e].a] = select ? 1 : 0;
    m->vsel.data[m->edges.data[e].b] = select ? 1 : 0;
    faces_from_vertices(m);
}

void a3_emesh_select_face(A3EMesh *m, u32 f, b32 select) {
    if (f >= m->fsize.count) return;
    m->fsel.data[f] = select ? 1 : 0;
    if (m->select_mode == A3_ESEL_FACE) { vertices_from_faces(m); return; }
    u32 n;
    const u32 *v = a3_emesh_face(m, f, &n);
    for (u32 i = 0; i < n; ++i) m->vsel.data[v[i]] = select ? 1 : 0;
    faces_from_vertices(m);
}

void a3_emesh_select_linked(A3EMesh *m) {
    a3_emesh_sync_selection(m);
    a3_emesh_edges(m);
    b32 changed = 1;
    while (changed) {
        changed = 0;
        for (u32 e = 0; e < m->edges.count; ++e) {
            u8 *a = &m->vsel.data[m->edges.data[e].a], *b = &m->vsel.data[m->edges.data[e].b];
            if (*a != *b) { *a = *b = 1; changed = 1; }
        }
    }
    faces_from_vertices(m);
}

void a3_emesh_select_more(A3EMesh *m) {
    a3_emesh_sync_selection(m);
    if (m->select_mode == A3_ESEL_FACE) {
        /* faces sharing a vertex with the selection */
        u8 *add = (u8 *)a3_calloc(m->fsize.count + 1, A3_MEM_TEMP);
        for (u32 f = 0; f < m->fsize.count; ++f) {
            u32 n;
            const u32 *v = a3_emesh_face(m, f, &n);
            for (u32 i = 0; i < n; ++i) if (m->vsel.data[v[i]]) { add[f] = 1; break; }
        }
        for (u32 f = 0; f < m->fsize.count; ++f) if (add[f]) m->fsel.data[f] = 1;
        a3_free(add);
        vertices_from_faces(m);
        return;
    }
    a3_emesh_edges(m);
    u8 *orig = (u8 *)a3_malloc(m->vsel.count + 1, A3_MEM_TEMP);
    a3_memcpy(orig, m->vsel.data, m->vsel.count);
    for (u32 e = 0; e < m->edges.count; ++e) {
        u32 a = m->edges.data[e].a, b = m->edges.data[e].b;
        if (orig[a] || orig[b]) m->vsel.data[a] = m->vsel.data[b] = 1;
    }
    a3_free(orig);
    faces_from_vertices(m);
}

/* vertex -> incident edges (CSR) */
typedef struct VertEdges { u32 *start, *list; } VertEdges;

static void vert_edges(A3EMesh *m, VertEdges *ve) {
    a3_emesh_edges(m);
    u32 nv = m->pos.count, ne = m->edges.count;
    ve->start = (u32 *)a3_calloc(sizeof(u32) * (nv + 1), A3_MEM_TEMP);
    ve->list = (u32 *)a3_malloc(sizeof(u32) * (ne * 2 + 1), A3_MEM_TEMP);
    for (u32 e = 0; e < ne; ++e) { ve->start[m->edges.data[e].a + 1]++; ve->start[m->edges.data[e].b + 1]++; }
    for (u32 v = 0; v < nv; ++v) ve->start[v + 1] += ve->start[v];
    u32 *fill = (u32 *)a3_malloc(sizeof(u32) * (nv + 1), A3_MEM_TEMP);
    a3_memcpy(fill, ve->start, sizeof(u32) * (nv + 1));
    for (u32 e = 0; e < ne; ++e) { ve->list[fill[m->edges.data[e].a]++] = e; ve->list[fill[m->edges.data[e].b]++] = e; }
    a3_free(fill);
}

static void vert_edges_free(VertEdges *ve) { a3_free(ve->start); a3_free(ve->list); }

static b32 edge_has_face(const A3EEdge *e, u32 f) { return f != A3_ENONE && (e->f0 == f || e->f1 == f); }

/* next edge of an edge loop through vertex v (quad topology: valence 4, not sharing a face) */
static u32 loop_next(A3EMesh *m, const VertEdges *ve, u32 e, u32 v) {
    u32 valence = ve->start[v + 1] - ve->start[v];
    if (valence != 4) return A3_ENONE;
    const A3EEdge *cur = &m->edges.data[e];
    for (u32 k = ve->start[v]; k < ve->start[v + 1]; ++k) {
        u32 o = ve->list[k];
        if (o == e) continue;
        const A3EEdge *oe = &m->edges.data[o];
        if (edge_has_face(oe, cur->f0) || edge_has_face(oe, cur->f1) || edge_has_face(cur, oe->f0) || edge_has_face(cur, oe->f1)) continue;
        return o;
    }
    return A3_ENONE;
}

void a3_emesh_select_edge_loop(A3EMesh *m, u32 e0) {
    a3_emesh_edges(m);
    if (e0 >= m->edges.count) return;
    VertEdges ve;
    vert_edges(m, &ve);
    for (u32 dir = 0; dir < 2; ++dir) {
        u32 e = e0, v = dir ? m->edges.data[e0].a : m->edges.data[e0].b;
        for (u32 guard = 0; guard < m->edges.count + 1; ++guard) {
            m->vsel.data[m->edges.data[e].a] = m->vsel.data[m->edges.data[e].b] = 1;
            u32 n = loop_next(m, &ve, e, v);
            if (n == A3_ENONE || n == e0) break;
            e = n;
            v = m->edges.data[e].a == v ? m->edges.data[e].b : m->edges.data[e].a;
        }
    }
    vert_edges_free(&ve);
    faces_from_vertices(m);
}

u32 a3_emesh_selected_count(const A3EMesh *m) {
    u32 c = 0;
    if (m->select_mode == A3_ESEL_FACE) { for (u32 f = 0; f < m->fsel.count; ++f) c += m->fsel.data[f] != 0; }
    else for (u32 v = 0; v < m->vsel.count; ++v) c += m->vsel.data[v] != 0;
    return c;
}

A3Vec3 a3_emesh_selection_center(const A3EMesh *m) {
    A3Vec3 c = a3_v3(0, 0, 0);
    u32 n = 0;
    for (u32 v = 0; v < m->pos.count; ++v) if (m->vsel.data[v]) { c = a3_v3_add(c, P(m, v)); n++; }
    return n ? a3_v3_scale(c, 1.0f / (f32)n) : c;
}

/* ======================================================================== */
/* Transform                                                                */
/* ======================================================================== */

void a3_emesh_transform_selected(A3EMesh *m, const A3Mat4 *mat) {
    a3_emesh_sync_selection(m);
    a3_mk_transform(m->pos.data, m->vsel.data, m->pos.count, mat->m);
}

void a3_emesh_transform_all(A3EMesh *m, const A3Mat4 *mat) { a3_mk_transform(m->pos.data, 0, m->pos.count, mat->m); }

void a3_emesh_translate(A3EMesh *m, A3Vec3 d) {
    A3Mat4 t = a3_mat4_translation(d);
    a3_emesh_transform_selected(m, &t);
}

static A3Mat4 about_pivot(const A3Mat4 *core, A3Vec3 pivot) {
    A3Mat4 to = a3_mat4_translation(a3_v3_neg(pivot)), back = a3_mat4_translation(pivot);
    A3Mat4 a = a3_mat4_mul(core, &to);
    return a3_mat4_mul(&back, &a);
}

void a3_emesh_rotate(A3EMesh *m, A3Vec3 e, A3Vec3 pivot) {
    A3Mat4 r = a3_mat4_from_quat(a3_quat_euler(e.x * A3_DEG2RAD, e.y * A3_DEG2RAD, e.z * A3_DEG2RAD));
    A3Mat4 t = about_pivot(&r, pivot);
    a3_emesh_transform_selected(m, &t);
}

void a3_emesh_scale(A3EMesh *m, A3Vec3 s, A3Vec3 pivot) {
    A3Mat4 sc = a3_mat4_scale(s);
    A3Mat4 t = about_pivot(&sc, pivot);
    a3_emesh_transform_selected(m, &t);
}

/* ======================================================================== */
/* Cleanup helpers                                                          */
/* ======================================================================== */

/* Applies remap (old vertex -> new vertex or A3_ENONE) to faces, dropping
 * repeated corners and faces that become degenerate, then compacts vertices. */
static void remap_vertices(A3EMesh *m, const u32 *remap, u32 new_count, const A3Vec4 *new_pos, const u8 *new_sel) {
    FaceBuild b;
    a3_zero_struct(&b);
    u32 tmp[1024];
    for (u32 f = 0; f < m->fsize.count; ++f) {
        u32 n;
        const u32 *v = a3_emesh_face(m, f, &n);
        u32 k = 0;
        b32 dropped = 0;
        for (u32 i = 0; i < n && k < 1024; ++i) {
            u32 nv = remap[v[i]];
            if (nv == A3_ENONE) { dropped = 1; break; }
            if (k && tmp[k - 1] == nv) continue;
            tmp[k++] = nv;
        }
        while (k > 1 && tmp[k - 1] == tmp[0]) k--;
        if (!dropped && k >= 3) fb_add(&b, tmp, k, m->fsel.data[f]);
    }
    fb_commit(m, &b);
    a3_array_clear(m->pos);
    a3_array_clear(m->vsel);
    a3_array_reserve(m->pos, new_count, TAG);
    a3_array_reserve(m->vsel, new_count, TAG);
    for (u32 i = 0; i < new_count; ++i) { a3_array_push(m->pos, new_pos[i], TAG); a3_array_push(m->vsel, new_sel[i], TAG); }
}

void a3_emesh_remove_loose(A3EMesh *m) {
    u32 nv = m->pos.count;
    u8 *used = (u8 *)a3_calloc(nv + 1, A3_MEM_TEMP);
    for (u32 i = 0; i < m->loops.count; ++i) used[m->loops.data[i]] = 1;
    u32 *remap = (u32 *)a3_malloc(sizeof(u32) * (nv + 1), A3_MEM_TEMP);
    A3Vec4 *np = (A3Vec4 *)a3_malloc(sizeof(A3Vec4) * (nv + 1), A3_MEM_TEMP);
    u8 *ns = (u8 *)a3_malloc(nv + 1, A3_MEM_TEMP);
    u32 k = 0;
    for (u32 v = 0; v < nv; ++v) {
        if (used[v]) { remap[v] = k; np[k] = m->pos.data[v]; ns[k] = m->vsel.data[v]; k++; }
        else remap[v] = A3_ENONE;
    }
    if (k != nv) remap_vertices(m, remap, k, np, ns);
    a3_free(used); a3_free(remap); a3_free(np); a3_free(ns);
}

/* ======================================================================== */
/* Normals                                                                  */
/* ======================================================================== */

static void reverse_face(A3EMesh *m, u32 f) {
    u32 *v = m->loops.data + m->fstart.data[f];
    u32 n = m->fsize.data[f];
    for (u32 i = 0; i < n / 2; ++i) { u32 t = v[i]; v[i] = v[n - 1 - i]; v[n - 1 - i] = t; }
}

/* does face f traverse a -> b ? */
static b32 face_has_directed(const A3EMesh *m, u32 f, u32 a, u32 b) {
    u32 n;
    const u32 *v = a3_emesh_face(m, f, &n);
    for (u32 i = 0; i < n; ++i) if (v[i] == a && v[(i + 1) % n] == b) return 1;
    return 0;
}

b32 a3_emesh_flip(A3EMesh *m, b32 selected_only) {
    a3_emesh_sync_selection(m);
    u32 c = 0;
    for (u32 f = 0; f < m->fsize.count; ++f) if (!selected_only || m->fsel.data[f]) { reverse_face(m, f); c++; }
    m->edges_valid = 0;
    return c > 0;
}

u32 a3_emesh_recalc_normals(A3EMesh *m) {
    u32 nf = m->fsize.count;
    if (!nf) return 0;
    a3_emesh_edges(m);
    A3HashMap map;
    edge_map(m, &map);
    u32 *comp = (u32 *)a3_malloc(sizeof(u32) * nf, A3_MEM_TEMP);
    u32 *queue = (u32 *)a3_malloc(sizeof(u32) * nf, A3_MEM_TEMP);
    for (u32 f = 0; f < nf; ++f) comp[f] = A3_ENONE;
    u32 flips = 0, ncomp = 0;
    for (u32 seed = 0; seed < nf; ++seed) {
        if (comp[seed] != A3_ENONE) continue;
        u32 qh = 0, qt = 0;
        queue[qt++] = seed;
        comp[seed] = ncomp;
        while (qh < qt) {
            u32 f = queue[qh++];
            u32 n;
            const u32 *v = a3_emesh_face(m, f, &n);
            for (u32 i = 0; i < n; ++i) {
                u32 a = v[i], b = v[(i + 1) % n];
                u32 e = edge_of(&map, a, b);
                if (e == A3_ENONE) continue;
                const A3EEdge *ed = &m->edges.data[e];
                if (ed->face_count != 2) continue;
                u32 g = ed->f0 == f ? ed->f1 : ed->f0;
                if (g == A3_ENONE || comp[g] != A3_ENONE) continue;
                if (face_has_directed(m, g, a, b)) { reverse_face(m, g); flips++; } /* neighbor must run b -> a */
                comp[g] = ncomp;
                queue[qt++] = g;
            }
        }
        ncomp++;
    }
    /* outward: positive signed volume per closed component */
    for (u32 c = 0; c < ncomp; ++c) {
        f64 vol = 0;
        for (u32 f = 0; f < nf; ++f) {
            if (comp[f] != c) continue;
            u32 n;
            const u32 *v = a3_emesh_face(m, f, &n);
            A3Vec3 p0 = P(m, v[0]);
            for (u32 i = 1; i + 1 < n; ++i) vol += (f64)a3_v3_dot(p0, a3_v3_cross(P(m, v[i]), P(m, v[i + 1])));
        }
        if (vol < -1e-9) for (u32 f = 0; f < nf; ++f) if (comp[f] == c) { reverse_face(m, f); flips++; }
    }
    a3_hashmap_free(&map);
    a3_free(comp);
    a3_free(queue);
    m->edges_valid = 0;
    return flips;
}

/* ======================================================================== */
/* Primitives                                                               */
/* ======================================================================== */

static void quad(A3EMesh *m, u32 a, u32 b, u32 c, u32 d) { u32 v[4] = { a, b, c, d }; a3_emesh_add_face(m, v, 4); }
static void tri(A3EMesh *m, u32 a, u32 b, u32 c) { u32 v[3] = { a, b, c }; a3_emesh_add_face(m, v, 3); }

void a3_emesh_make(A3EMesh *m, A3EPrimitive prim, f32 size, u32 segments, u32 rings) {
    a3_emesh_clear(m);
    if (size <= 0) size = 1;
    f32 h = size * 0.5f;
    b32 closed = 1;
    switch (prim) {
    default:
    case A3_EPRIM_CUBE: {
        for (u32 i = 0; i < 8; ++i) a3_emesh_add_vertex(m, a3_v3((i & 1) ? h : -h, (i & 2) ? h : -h, (i & 4) ? h : -h));
        quad(m, 0, 4, 6, 2); quad(m, 1, 3, 7, 5); quad(m, 0, 1, 5, 4);
        quad(m, 2, 6, 7, 3); quad(m, 0, 2, 3, 1); quad(m, 4, 5, 7, 6);
    } break;
    case A3_EPRIM_PLANE: case A3_EPRIM_GRID: {
        u32 n = prim == A3_EPRIM_PLANE ? 1 : a3_clampi((i32)(segments ? segments : 10), 1, 256);
        f32 step = size / (f32)n;
        for (u32 i = 0; i <= n; ++i) for (u32 j = 0; j <= n; ++j) a3_emesh_add_vertex(m, a3_v3(-h + i * step, 0, -h + j * step));
        for (u32 i = 0; i < n; ++i) for (u32 j = 0; j < n; ++j) {
            u32 a = i * (n + 1) + j;
            quad(m, a, a + 1, a + n + 2, a + n + 1);   /* (i,j) (i,j+1) (i+1,j+1) (i+1,j): +Y normal */
        }
        closed = 0;
    } break;
    case A3_EPRIM_CYLINDER: case A3_EPRIM_CONE: {
        u32 s = (u32)a3_clampi((i32)(segments ? segments : 16), 3, 512);
        for (u32 i = 0; i < s; ++i) {
            f32 a = (f32)i / (f32)s * A3_TAU;
            a3_emesh_add_vertex(m, a3_v3(a3_cosf(a) * h, -h, a3_sinf(a) * h));
        }
        if (prim == A3_EPRIM_CYLINDER) {
            for (u32 i = 0; i < s; ++i) {
                f32 a = (f32)i / (f32)s * A3_TAU;
                a3_emesh_add_vertex(m, a3_v3(a3_cosf(a) * h, h, a3_sinf(a) * h));
            }
            for (u32 i = 0; i < s; ++i) quad(m, i, (i + 1) % s, s + (i + 1) % s, s + i);
            u32 cap[512];
            for (u32 i = 0; i < s; ++i) cap[i] = s + i;
            a3_emesh_add_face(m, cap, s);
        } else {
            u32 apex = a3_emesh_add_vertex(m, a3_v3(0, h, 0));
            for (u32 i = 0; i < s; ++i) tri(m, i, (i + 1) % s, apex);
        }
        u32 cap[512];
        for (u32 i = 0; i < s; ++i) cap[i] = i;
        a3_emesh_add_face(m, cap, s);
    } break;
    case A3_EPRIM_SPHERE: {
        u32 s = (u32)a3_clampi((i32)(segments ? segments : 24), 3, 512);
        u32 r = (u32)a3_clampi((i32)(rings ? rings : 12), 2, 256);
        u32 top = a3_emesh_add_vertex(m, a3_v3(0, h, 0));
        for (u32 j = 1; j < r; ++j) {
            f32 phi = (f32)j / (f32)r * A3_PI;
            f32 y = a3_cosf(phi) * h, rr = a3_sinf(phi) * h;
            for (u32 i = 0; i < s; ++i) {
                f32 a = (f32)i / (f32)s * A3_TAU;
                a3_emesh_add_vertex(m, a3_v3(a3_cosf(a) * rr, y, a3_sinf(a) * rr));
            }
        }
        u32 bottom = a3_emesh_add_vertex(m, a3_v3(0, -h, 0));
        for (u32 i = 0; i < s; ++i) tri(m, top, 1 + (i + 1) % s, 1 + i);
        for (u32 j = 0; j + 2 < r; ++j) for (u32 i = 0; i < s; ++i) {
            u32 a = 1 + j * s + i, b = 1 + j * s + (i + 1) % s;
            quad(m, a, b, b + s, a + s);
        }
        u32 last = 1 + (r - 2) * s;
        for (u32 i = 0; i < s; ++i) tri(m, bottom, last + i, last + (i + 1) % s);
    } break;
    case A3_EPRIM_TORUS: {
        u32 s = (u32)a3_clampi((i32)(segments ? segments : 32), 3, 512);
        u32 r = (u32)a3_clampi((i32)(rings ? rings : 12), 3, 256);
        f32 tube = size * 0.125f, major = h - tube;
        for (u32 i = 0; i < s; ++i) {
            f32 th = (f32)i / (f32)s * A3_TAU;
            for (u32 j = 0; j < r; ++j) {
                f32 ph = (f32)j / (f32)r * A3_TAU;
                f32 d = major + tube * a3_cosf(ph);
                a3_emesh_add_vertex(m, a3_v3(d * a3_cosf(th), tube * a3_sinf(ph), d * a3_sinf(th)));
            }
        }
        for (u32 i = 0; i < s; ++i) for (u32 j = 0; j < r; ++j) {
            u32 i2 = (i + 1) % s, j2 = (j + 1) % r;
            quad(m, i * r + j, i2 * r + j, i2 * r + j2, i * r + j2);
        }
    } break;
    }
    if (closed) a3_emesh_recalc_normals(m);
    m->edges_valid = 0;
}

void a3_emesh_add_primitive(A3EMesh *m, A3EPrimitive prim, f32 size, u32 segments, u32 rings, A3Vec3 center) {
    A3EMesh t;
    a3_emesh_init(&t);
    a3_emesh_make(&t, prim, size, segments, rings);
    a3_emesh_select_all(m, 0);
    u32 base = m->pos.count;
    for (u32 v = 0; v < t.pos.count; ++v) {
        u32 nv = a3_emesh_add_vertex(m, a3_v3_add(P(&t, v), center));
        if (nv != A3_ENONE) m->vsel.data[nv] = 1;
    }
    for (u32 f = 0; f < t.fsize.count; ++f) {
        u32 n, tmp[512];
        const u32 *v = a3_emesh_face(&t, f, &n);
        for (u32 i = 0; i < n; ++i) tmp[i] = v[i] + base;
        u32 nf = a3_emesh_add_face(m, tmp, n);
        if (nf != A3_ENONE) m->fsel.data[nf] = 1;
    }
    a3_emesh_free(&t);
}

/* ======================================================================== */
/* Extrude, inset, duplicate                                                */
/* ======================================================================== */

b32 a3_emesh_extrude(A3EMesh *m, f32 distance) {
    a3_emesh_sync_selection(m);
    a3_emesh_edges(m);
    u32 nf = m->fsize.count, nv = m->pos.count;
    u32 nsel = 0;
    for (u32 f = 0; f < nf; ++f) nsel += m->fsel.data[f] != 0;
    if (!nsel) return 0;
    A3HashMap map;
    edge_map(m, &map);
    /* region normal: mean of selected face normals; per-vertex normals when the region bends a lot */
    A3Vec3 mean = a3_v3(0, 0, 0);
    for (u32 f = 0; f < nf; ++f) if (m->fsel.data[f]) mean = a3_v3_add(mean, a3_emesh_face_normal(m, f));
    mean = a3_v3_norm(mean);
    b32 flat = a3_v3_len(mean) > 0.5f;
    for (u32 f = 0; f < nf && flat; ++f) if (m->fsel.data[f] && a3_v3_dot(a3_emesh_face_normal(m, f), mean) < 0.7f) flat = 0;
    u32 *dup = (u32 *)a3_malloc(sizeof(u32) * (nv + 1), A3_MEM_TEMP);
    A3Vec3 *vn = (A3Vec3 *)a3_calloc(sizeof(A3Vec3) * (nv + 1), A3_MEM_TEMP);
    for (u32 v = 0; v < nv; ++v) dup[v] = A3_ENONE;
    for (u32 f = 0; f < nf; ++f) {
        if (!m->fsel.data[f]) continue;
        A3Vec3 fnorm = a3_emesh_face_normal(m, f);
        u32 n;
        const u32 *v = a3_emesh_face(m, f, &n);
        for (u32 i = 0; i < n; ++i) vn[v[i]] = a3_v3_add(vn[v[i]], fnorm);
    }
    for (u32 v = 0; v < nv; ++v) {
        if (a3_v3_len_sq(vn[v]) == 0) continue;
        A3Vec3 d = flat ? mean : a3_v3_norm(vn[v]);
        dup[v] = a3_emesh_add_vertex(m, a3_v3_add(P(m, v), a3_v3_scale(d, distance)));
    }
    FaceBuild b;
    a3_zero_struct(&b);
    u32 tmp[1024];
    for (u32 f = 0; f < nf; ++f) {
        u32 n;
        const u32 *v = a3_emesh_face(m, f, &n);
        if (!m->fsel.data[f]) { fb_add(&b, v, n, 0); continue; }
        for (u32 i = 0; i < n && i < 1024; ++i) tmp[i] = dup[v[i]];
        fb_add(&b, tmp, n, 1);
        /* side walls on the region boundary */
        for (u32 i = 0; i < n; ++i) {
            u32 a = v[i], c = v[(i + 1) % n];
            u32 e = edge_of(&map, a, c);
            const A3EEdge *ed = e != A3_ENONE ? &m->edges.data[e] : 0;
            b32 boundary = !ed || ed->face_count != 2;
            if (ed && ed->face_count == 2) {
                u32 g = ed->f0 == f ? ed->f1 : ed->f0;
                boundary = !m->fsel.data[g];
            }
            if (!boundary) continue;
            u32 q[4] = { a, c, dup[c], dup[a] };
            fb_add(&b, q, 4, 0);
        }
    }
    fb_commit(m, &b);
    a3_hashmap_free(&map);
    /* selection moves to the new faces */
    a3_memset(m->vsel.data, 0, m->vsel.count);
    for (u32 v = 0; v < nv; ++v) if (dup[v] != A3_ENONE) m->vsel.data[dup[v]] = 1;
    a3_free(dup);
    a3_free(vn);
    a3_emesh_remove_loose(m);
    if (m->select_mode != A3_ESEL_FACE) faces_from_vertices(m);
    return 1;
}

b32 a3_emesh_inset(A3EMesh *m, f32 amount, f32 depth) {
    a3_emesh_sync_selection(m);
    u32 nf = m->fsize.count;
    u32 nsel = 0;
    for (u32 f = 0; f < nf; ++f) nsel += m->fsel.data[f] != 0;
    if (!nsel) return 0;
    amount = a3_clampf(amount, 0.0f, 0.999f);
    FaceBuild b;
    a3_zero_struct(&b);
    u32 inner[1024];
    a3_memset(m->vsel.data, 0, m->vsel.count);
    for (u32 f = 0; f < nf; ++f) {
        u32 n;
        const u32 *v0 = a3_emesh_face(m, f, &n);
        if (!m->fsel.data[f]) { fb_add(&b, v0, n, 0); continue; }
        A3Vec3 c = a3_emesh_face_center(m, f), nrm = a3_emesh_face_normal(m, f);
        u32 vv[1024];
        for (u32 i = 0; i < n && i < 1024; ++i) vv[i] = v0[i];   /* the face array may move while adding vertices */
        for (u32 i = 0; i < n && i < 1024; ++i) {
            A3Vec3 p = P(m, vv[i]);
            A3Vec3 q = a3_v3_add(a3_v3_add(p, a3_v3_scale(a3_v3_sub(c, p), amount)), a3_v3_scale(nrm, depth));
            inner[i] = a3_emesh_add_vertex(m, q);
            m->vsel.data[inner[i]] = 1;
        }
        for (u32 i = 0; i < n; ++i) {
            u32 q[4] = { vv[i], vv[(i + 1) % n], inner[(i + 1) % n], inner[i] };
            fb_add(&b, q, 4, 0);
        }
        fb_add(&b, inner, n, 1);
    }
    fb_commit(m, &b);
    if (m->select_mode != A3_ESEL_FACE) faces_from_vertices(m);
    return 1;
}

b32 a3_emesh_duplicate(A3EMesh *m) {
    a3_emesh_sync_selection(m);
    u32 nf = m->fsize.count, nv = m->pos.count;
    u32 *dup = (u32 *)a3_malloc(sizeof(u32) * (nv + 1), A3_MEM_TEMP);
    for (u32 v = 0; v < nv; ++v) dup[v] = A3_ENONE;
    u32 made = 0;
    for (u32 f = 0; f < nf; ++f) {
        if (!m->fsel.data[f]) continue;
        u32 n, tmp[1024];
        const u32 *v = a3_emesh_face(m, f, &n);
        for (u32 i = 0; i < n && i < 1024; ++i) {
            u32 src = m->loops.data[m->fstart.data[f] + i];
            if (dup[src] == A3_ENONE) dup[src] = a3_emesh_add_vertex(m, P(m, src));
            tmp[i] = dup[src];
        }
        A3_UNUSED(v);
        a3_emesh_add_face(m, tmp, n);
        made++;
    }
    if (!made) { a3_free(dup); return 0; }
    for (u32 f = 0; f < m->fsel.count; ++f) m->fsel.data[f] = f >= nf;
    vertices_from_faces(m);
    a3_free(dup);
    return 1;
}

/* ======================================================================== */
/* Loop cut                                                                 */
/* ======================================================================== */

/* j-th cut vertex (1..cuts) on edge u-w, counted from u */
static u32 cut_vertex(const A3EMesh *m, const A3HashMap *map, const u32 *base, u32 cuts, u32 u, u32 w, u32 j) {
    u32 e = edge_of(map, u, w);
    return u == m->edges.data[e].a ? base[e] + j - 1 : base[e] + cuts - j;
}

b32 a3_emesh_loop_cut(A3EMesh *m, u32 edge, u32 cuts) {
    a3_emesh_edges(m);
    if (edge >= m->edges.count || cuts < 1) return 0;
    if (cuts > 64) cuts = 64;
    u32 nf = m->fsize.count;
    A3HashMap map;
    edge_map(m, &map);
    i32 *entry = (i32 *)a3_malloc(sizeof(i32) * (nf + 1), A3_MEM_TEMP);   /* ring faces: entry corner, else -1 */
    for (u32 f = 0; f < nf; ++f) entry[f] = -1;
    u8 *cut = (u8 *)a3_calloc(m->edges.count + 1, A3_MEM_TEMP);
    cut[edge] = 1;
    u32 ring = 0;
    const A3EEdge *e0 = &m->edges.data[edge];
    u32 starts[2] = { e0->f0, e0->f1 };
    for (u32 side = 0; side < 2; ++side) {
        u32 f = starts[side], e = edge;
        while (f != A3_ENONE && entry[f] < 0 && m->fsize.data[f] == 4) {
            const u32 *q = m->loops.data + m->fstart.data[f];
            i32 k = -1;
            for (i32 i = 0; i < 4; ++i) if (edge_of(&map, q[i], q[(i + 1) & 3]) == e) k = i;
            if (k < 0) break;
            entry[f] = k;
            ring++;
            u32 opp = edge_of(&map, q[(k + 2) & 3], q[(k + 3) & 3]);
            if (opp == A3_ENONE) break;
            cut[opp] = 1;
            const A3EEdge *oe = &m->edges.data[opp];
            if (oe->face_count != 2) break;
            f = oe->f0 == f ? oe->f1 : oe->f0;
            e = opp;
        }
    }
    if (!ring) { a3_hashmap_free(&map); a3_free(entry); a3_free(cut); return 0; }
    /* cut vertices, ordered from edge.a to edge.b */
    u32 *base = (u32 *)a3_malloc(sizeof(u32) * (m->edges.count + 1), A3_MEM_TEMP);
    u32 ne = m->edges.count;
    a3_memset(m->vsel.data, 0, m->vsel.count);
    for (u32 ei = 0; ei < ne; ++ei) {
        base[ei] = A3_ENONE;
        if (!cut[ei]) continue;
        A3Vec3 a = P(m, m->edges.data[ei].a), b = P(m, m->edges.data[ei].b);
        base[ei] = m->pos.count;
        for (u32 j = 1; j <= cuts; ++j) {
            u32 nv = a3_emesh_add_vertex(m, a3_v3_lerp(a, b, (f32)j / (f32)(cuts + 1)));
            m->vsel.data[nv] = 1;
        }
    }
    FaceBuild fb;
    a3_zero_struct(&fb);
#define CUTV(u, w, j) cut_vertex(m, &map, base, cuts, (u), (w), (j))
    u32 tmp[2048];
    for (u32 f = 0; f < nf; ++f) {
        u32 n;
        const u32 *v = a3_emesh_face(m, f, &n);
        if (entry[f] >= 0) {
            i32 k = entry[f];
            u32 q0 = v[k], q1 = v[(k + 1) & 3], q2 = v[(k + 2) & 3], q3 = v[(k + 3) & 3];
            for (u32 j = 0; j <= cuts; ++j) {
                u32 s1a = j == 0 ? q0 : CUTV(q0, q1, j);
                u32 s1b = j + 1 == cuts + 1 ? q1 : CUTV(q0, q1, j + 1);
                u32 s2a = j == 0 ? q3 : CUTV(q3, q2, j);
                u32 s2b = j + 1 == cuts + 1 ? q2 : CUTV(q3, q2, j + 1);
                u32 qd[4] = { s1a, s1b, s2b, s2a };
                fb_add(&fb, qd, 4, m->fsel.data[f]);
            }
            continue;
        }
        u32 k = 0;
        for (u32 i = 0; i < n && k < 2000; ++i) {
            u32 a = v[i], b = v[(i + 1) % n];
            tmp[k++] = a;
            u32 e = edge_of(&map, a, b);
            if (e != A3_ENONE && cut[e]) for (u32 j = 1; j <= cuts && k < 2000; ++j) tmp[k++] = CUTV(a, b, j);
        }
        fb_add(&fb, tmp, k, m->fsel.data[f]);
    }
#undef CUTV
    fb_commit(m, &fb);
    a3_hashmap_free(&map);
    a3_free(entry);
    a3_free(cut);
    a3_free(base);
    if (m->select_mode == A3_ESEL_FACE) m->select_mode = A3_ESEL_EDGE;
    faces_from_vertices(m);
    return 1;
}

/* ======================================================================== */
/* Subdivision (Catmull-Clark or simple)                                    */
/* ======================================================================== */

b32 a3_emesh_subdivide(A3EMesh *m, b32 smooth) {
    u32 nf = m->fsize.count, nv = m->pos.count;
    if (!nf) return 0;
    a3_emesh_edges(m);
    u32 ne = m->edges.count;
    A3HashMap map;
    edge_map(m, &map);
    A3Vec3 *fp = (A3Vec3 *)a3_malloc(sizeof(A3Vec3) * nf, A3_MEM_TEMP);
    for (u32 f = 0; f < nf; ++f) fp[f] = a3_emesh_face_center(m, f);
    A3Vec3 *ep = (A3Vec3 *)a3_malloc(sizeof(A3Vec3) * (ne + 1), A3_MEM_TEMP);
    for (u32 e = 0; e < ne; ++e) {
        const A3EEdge *ed = &m->edges.data[e];
        A3Vec3 mid = a3_v3_scale(a3_v3_add(P(m, ed->a), P(m, ed->b)), 0.5f);
        if (smooth && ed->face_count == 2)
            ep[e] = a3_v3_scale(a3_v3_add(a3_v3_add(P(m, ed->a), P(m, ed->b)), a3_v3_add(fp[ed->f0], fp[ed->f1])), 0.25f);
        else ep[e] = mid;
    }
    A3Vec3 *vp = (A3Vec3 *)a3_malloc(sizeof(A3Vec3) * (nv + 1), A3_MEM_TEMP);
    for (u32 v = 0; v < nv; ++v) vp[v] = P(m, v);
    if (smooth) {
        A3Vec3 *fsum = (A3Vec3 *)a3_calloc(sizeof(A3Vec3) * (nv + 1), A3_MEM_TEMP);
        A3Vec3 *esum = (A3Vec3 *)a3_calloc(sizeof(A3Vec3) * (nv + 1), A3_MEM_TEMP);
        A3Vec3 *bsum = (A3Vec3 *)a3_calloc(sizeof(A3Vec3) * (nv + 1), A3_MEM_TEMP);
        u32 *fc = (u32 *)a3_calloc(sizeof(u32) * (nv + 1), A3_MEM_TEMP);
        u32 *ec = (u32 *)a3_calloc(sizeof(u32) * (nv + 1), A3_MEM_TEMP);
        u32 *bc = (u32 *)a3_calloc(sizeof(u32) * (nv + 1), A3_MEM_TEMP);
        for (u32 f = 0; f < nf; ++f) {
            u32 n;
            const u32 *v = a3_emesh_face(m, f, &n);
            for (u32 i = 0; i < n; ++i) { fsum[v[i]] = a3_v3_add(fsum[v[i]], fp[f]); fc[v[i]]++; }
        }
        for (u32 e = 0; e < ne; ++e) {
            const A3EEdge *ed = &m->edges.data[e];
            A3Vec3 mid = a3_v3_scale(a3_v3_add(P(m, ed->a), P(m, ed->b)), 0.5f);
            esum[ed->a] = a3_v3_add(esum[ed->a], mid); ec[ed->a]++;
            esum[ed->b] = a3_v3_add(esum[ed->b], mid); ec[ed->b]++;
            if (ed->face_count == 1) {
                bsum[ed->a] = a3_v3_add(bsum[ed->a], P(m, ed->b)); bc[ed->a]++;
                bsum[ed->b] = a3_v3_add(bsum[ed->b], P(m, ed->a)); bc[ed->b]++;
            }
        }
        for (u32 v = 0; v < nv; ++v) {
            if (!ec[v]) continue;
            A3Vec3 p = P(m, v);
            if (bc[v] == 2) vp[v] = a3_v3_add(a3_v3_scale(p, 0.75f), a3_v3_scale(bsum[v], 0.125f));
            else if (bc[v]) vp[v] = p;   /* corners and non-manifold boundaries stay */
            else {
                f32 n = (f32)ec[v];
                A3Vec3 q = a3_v3_scale(fsum[v], 1.0f / (f32)fc[v]);
                A3Vec3 r = a3_v3_scale(esum[v], 1.0f / (f32)ec[v]);
                vp[v] = a3_v3_scale(a3_v3_add(a3_v3_add(q, a3_v3_scale(r, 2.0f)), a3_v3_scale(p, n - 3.0f)), 1.0f / n);
            }
        }
        a3_free(fsum); a3_free(esum); a3_free(bsum); a3_free(fc); a3_free(ec); a3_free(bc);
    }
    /* new vertex layout: old | edge points | face points */
    for (u32 v = 0; v < nv; ++v) m->pos.data[v] = a3_v4_from3(vp[v], 1);
    u32 ebase = m->pos.count;
    for (u32 e = 0; e < ne; ++e) a3_emesh_add_vertex(m, ep[e]);
    u32 fbase = m->pos.count;
    for (u32 f = 0; f < nf; ++f) a3_emesh_add_vertex(m, fp[f]);
    FaceBuild b;
    a3_zero_struct(&b);
    for (u32 f = 0; f < nf; ++f) {
        u32 n;
        const u32 *v = a3_emesh_face(m, f, &n);
        for (u32 i = 0; i < n; ++i) {
            u32 prev = v[(i + n - 1) % n], cur = v[i], next = v[(i + 1) % n];
            u32 q[4] = { cur, ebase + edge_of(&map, cur, next), fbase + f, ebase + edge_of(&map, prev, cur) };
            fb_add(&b, q, 4, m->fsel.data[f]);
        }
    }
    fb_commit(m, &b);
    a3_hashmap_free(&map);
    a3_free(fp); a3_free(ep); a3_free(vp);
    a3_emesh_sync_selection(m);
    if (m->select_mode != A3_ESEL_FACE) { vertices_from_faces(m); faces_from_vertices(m); }
    return 1;
}

/* ======================================================================== */
/* Delete, merge, fill, mirror                                              */
/* ======================================================================== */

b32 a3_emesh_delete_faces(A3EMesh *m) {
    a3_emesh_sync_selection(m);
    FaceBuild b;
    a3_zero_struct(&b);
    u32 removed = 0;
    for (u32 f = 0; f < m->fsize.count; ++f) {
        u32 n;
        const u32 *v = a3_emesh_face(m, f, &n);
        if (m->fsel.data[f]) { removed++; continue; }
        fb_add(&b, v, n, 0);
    }
    if (!removed) { fb_free(&b); return 0; }
    fb_commit(m, &b);
    a3_memset(m->vsel.data, 0, m->vsel.count);
    a3_emesh_remove_loose(m);
    return 1;
}

b32 a3_emesh_delete_vertices(A3EMesh *m) {
    a3_emesh_sync_selection(m);
    u32 nv = m->pos.count, k = 0;
    u32 *remap = (u32 *)a3_malloc(sizeof(u32) * (nv + 1), A3_MEM_TEMP);
    A3Vec4 *np = (A3Vec4 *)a3_malloc(sizeof(A3Vec4) * (nv + 1), A3_MEM_TEMP);
    u8 *ns = (u8 *)a3_calloc(nv + 1, A3_MEM_TEMP);
    for (u32 v = 0; v < nv; ++v) {
        if (m->vsel.data[v]) remap[v] = A3_ENONE;
        else { remap[v] = k; np[k] = m->pos.data[v]; k++; }
    }
    b32 any = k != nv;
    if (any) remap_vertices(m, remap, k, np, ns);
    a3_free(remap); a3_free(np); a3_free(ns);
    if (any) a3_emesh_remove_loose(m);
    return any;
}

static i64 cell_of(f32 x, f32 inv) { return (i64)a3_floorf(x * inv); }
static u64 cell_key(i64 x, i64 y, i64 z) { return a3_hash_combine(a3_hash_combine((u64)x * 0x9E3779B97F4A7C15ull, (u64)y), (u64)z * 0xC2B2AE3D27D4EB4Full) | 1; }

u32 a3_emesh_merge_by_distance(A3EMesh *m, f32 distance, b32 selected_only) {
    a3_emesh_sync_selection(m);
    u32 nv = m->pos.count;
    if (!nv) return 0;
    if (distance <= 0) distance = 1e-4f;
    f32 inv = 1.0f / distance, d2 = distance * distance;
    A3HashMap head;
    a3_hashmap_init(&head, nv * 2 + 16, A3_MEM_TEMP);
    u32 *next = (u32 *)a3_malloc(sizeof(u32) * (nv + 1), A3_MEM_TEMP);
    u32 *rep = (u32 *)a3_malloc(sizeof(u32) * (nv + 1), A3_MEM_TEMP);
    u32 merged = 0;
    for (u32 v = 0; v < nv; ++v) {
        rep[v] = v;
        next[v] = A3_ENONE;
        if (selected_only && !m->vsel.data[v]) continue;
        A3Vec3 p = P(m, v);
        i64 cx = cell_of(p.x, inv), cy = cell_of(p.y, inv), cz = cell_of(p.z, inv);
        u32 found = A3_ENONE;
        for (i64 dx = -1; dx <= 1 && found == A3_ENONE; ++dx)
            for (i64 dy = -1; dy <= 1 && found == A3_ENONE; ++dy)
                for (i64 dz = -1; dz <= 1 && found == A3_ENONE; ++dz) {
                    u64 h;
                    if (!a3_hashmap_get(&head, cell_key(cx + dx, cy + dy, cz + dz), &h)) continue;
                    for (u32 c = (u32)(h - 1); c != A3_ENONE; c = next[c])
                        if (a3_v3_len_sq(a3_v3_sub(P(m, c), p)) <= d2) { found = c; break; }
                }
        if (found != A3_ENONE) { rep[v] = found; merged++; continue; }
        u64 key = cell_key(cx, cy, cz), h;
        next[v] = a3_hashmap_get(&head, key, &h) ? (u32)(h - 1) : A3_ENONE;
        a3_hashmap_put(&head, key, (u64)v + 1);
    }
    if (merged) {
        u32 *remap = (u32 *)a3_malloc(sizeof(u32) * (nv + 1), A3_MEM_TEMP);
        A3Vec4 *np = (A3Vec4 *)a3_malloc(sizeof(A3Vec4) * (nv + 1), A3_MEM_TEMP);
        u8 *ns = (u8 *)a3_malloc(nv + 1, A3_MEM_TEMP);
        u32 k = 0;
        for (u32 v = 0; v < nv; ++v) if (rep[v] == v) { remap[v] = k; np[k] = m->pos.data[v]; ns[k] = m->vsel.data[v]; k++; }
        for (u32 v = 0; v < nv; ++v) if (rep[v] != v) remap[v] = remap[rep[v]];
        remap_vertices(m, remap, k, np, ns);
        a3_free(remap); a3_free(np); a3_free(ns);
        a3_emesh_remove_loose(m);
        a3_emesh_sync_selection(m);
    }
    a3_hashmap_free(&head);
    a3_free(next);
    a3_free(rep);
    return merged;
}

static f32 angle2(f32 y, f32 x) { return a3_atan2f(y, x); }

b32 a3_emesh_fill(A3EMesh *m) {
    a3_emesh_sync_selection(m);
    a3_emesh_edges(m);
    /* 1. a closed loop of selected boundary edges */
    u32 ne = m->edges.count;
    u32 *bedges = (u32 *)a3_malloc(sizeof(u32) * (ne + 1), A3_MEM_TEMP);
    u32 nb = 0;
    for (u32 e = 0; e < ne; ++e) {
        const A3EEdge *ed = &m->edges.data[e];
        if (ed->face_count == 1 && m->vsel.data[ed->a] && m->vsel.data[ed->b]) bedges[nb++] = e;
    }
    u32 order[1024];
    u32 count = 0;
    b32 ok = 0;
    if (nb >= 3 && nb < 1024) {
        /* start on an edge; the new face runs opposite to the existing face */
        const A3EEdge *e0 = &m->edges.data[bedges[0]];
        u32 a = e0->a, b = e0->b;
        if (!face_has_directed(m, e0->f0, a, b)) { u32 t = a; a = b; b = t; }  /* now the old face runs a -> b */
        order[count++] = b;
        order[count++] = a;
        u32 prev_e = bedges[0], cur = a;
        for (u32 guard = 0; guard < nb; ++guard) {
            u32 nxt = A3_ENONE, nxt_e = A3_ENONE;
            for (u32 i = 0; i < nb; ++i) {
                const A3EEdge *ed = &m->edges.data[bedges[i]];
                if (bedges[i] == prev_e) continue;
                if (ed->a == cur) { nxt = ed->b; nxt_e = bedges[i]; break; }
                if (ed->b == cur) { nxt = ed->a; nxt_e = bedges[i]; break; }
            }
            if (nxt == A3_ENONE) break;
            if (nxt == order[0]) { ok = count == nb; break; }
            if (count >= 1024) break;
            order[count++] = nxt;
            prev_e = nxt_e;
            cur = nxt;
        }
    }
    a3_free(bedges);
    if (!ok) {
        /* 2. selected vertices ordered by angle around their center */
        count = 0;
        for (u32 v = 0; v < m->pos.count && count < 1024; ++v) if (m->vsel.data[v]) order[count++] = v;
        if (count < 3 || count > 256) return 0;
        A3Vec3 c = a3_v3(0, 0, 0);
        for (u32 i = 0; i < count; ++i) c = a3_v3_add(c, P(m, order[i]));
        c = a3_v3_scale(c, 1.0f / (f32)count);
        A3Vec3 nrm = a3_v3(0, 0, 0);
        f32 best = 0;
        for (u32 i = 0; i < count; ++i) for (u32 j = i + 1; j < count; ++j) {
            A3Vec3 x = a3_v3_cross(a3_v3_sub(P(m, order[i]), c), a3_v3_sub(P(m, order[j]), c));
            f32 l = a3_v3_len(x);
            if (l > best) { best = l; nrm = a3_v3_scale(x, 1.0f / l); }
        }
        if (best <= 1e-12f) return 0;
        A3Vec3 ax = a3_v3_norm(a3_v3_sub(P(m, order[0]), c));
        if (a3_v3_len_sq(ax) == 0) return 0;
        A3Vec3 ay = a3_v3_cross(nrm, ax);
        f32 ang[1024];
        for (u32 i = 0; i < count; ++i) { A3Vec3 d = a3_v3_sub(P(m, order[i]), c); ang[i] = angle2(a3_v3_dot(d, ay), a3_v3_dot(d, ax)); }
        for (u32 i = 1; i < count; ++i) for (u32 j = i; j > 0 && ang[j - 1] > ang[j]; --j) {
            f32 ta = ang[j]; ang[j] = ang[j - 1]; ang[j - 1] = ta;
            u32 tv = order[j]; order[j] = order[j - 1]; order[j - 1] = tv;
        }
    }
    u32 f = a3_emesh_add_face(m, order, count);
    if (f == A3_ENONE) return 0;
    /* agree with neighbors: shared edges must run in opposite directions */
    for (u32 i = 0; i < count; ++i) {
        u32 a = order[i], b = order[(i + 1) % count];
        for (u32 g = 0; g < f; ++g) if (face_has_directed(m, g, a, b)) { reverse_face(m, f); i = count; break; }
    }
    a3_memset(m->fsel.data, 0, m->fsel.count);
    m->fsel.data[f] = 1;
    a3_emesh_sync_selection(m);
    m->edges_valid = 0;
    return 1;
}

b32 a3_emesh_mirror(A3EMesh *m, u32 axis, f32 merge_distance) {
    if (axis > 2 || !m->pos.count) return 0;
    u32 nv = m->pos.count, nf = m->fsize.count;
    u32 *mir = (u32 *)a3_malloc(sizeof(u32) * (nv + 1), A3_MEM_TEMP);
    for (u32 v = 0; v < nv; ++v) {
        A3Vec3 p = P(m, v);
        f32 *c = axis == 0 ? &p.x : axis == 1 ? &p.y : &p.z;
        if (a3_absf(*c) <= merge_distance) {
            *c = 0;
            a3_emesh_set_vertex(m, v, p);   /* snap to the mirror plane and share it */
            mir[v] = v;
        } else {
            *c = -*c;
            mir[v] = a3_emesh_add_vertex(m, p);
        }
    }
    for (u32 f = 0; f < nf; ++f) {
        u32 n, tmp[1024];
        const u32 *v = a3_emesh_face(m, f, &n);
        for (u32 i = 0; i < n && i < 1024; ++i) tmp[n - 1 - i] = mir[m->loops.data[m->fstart.data[f] + i]];
        A3_UNUSED(v);
        a3_emesh_add_face(m, tmp, n);
    }
    a3_free(mir);
    return 1;
}

/* ======================================================================== */
/* Triangulation, picking, conversion                                       */
/* ======================================================================== */

/* ear clipping in the plane of the face (handles concave polygons) */
static u32 triangulate_face(const A3EMesh *m, u32 f, u32 *out) {
    u32 n;
    const u32 *v = a3_emesh_face(m, f, &n);
    if (n == 3) { out[0] = v[0]; out[1] = v[1]; out[2] = v[2]; return 1; }
    A3Vec3 nrm = a3_emesh_face_normal(m, f);
    u32 ax = 0;
    if (a3_absf(nrm.y) > a3_absf(nrm.x) && a3_absf(nrm.y) >= a3_absf(nrm.z)) ax = 1;
    else if (a3_absf(nrm.z) > a3_absf(nrm.x) && a3_absf(nrm.z) > a3_absf(nrm.y)) ax = 2;
    f32 sign = (ax == 0 ? nrm.x : ax == 1 ? nrm.y : nrm.z) < 0 ? -1.0f : 1.0f;
    u32 idx[1024];
    f32 px[1024], py[1024];
    u32 cnt = n < 1024 ? n : 1024;
    for (u32 i = 0; i < cnt; ++i) {
        A3Vec3 p = P(m, v[i]);
        idx[i] = i;
        /* project dropping the dominant axis, keeping counter-clockwise orientation */
        if (ax == 0) { px[i] = p.y; py[i] = p.z; }
        else if (ax == 1) { px[i] = p.z; py[i] = p.x; }
        else { px[i] = p.x; py[i] = p.y; }
        if (sign < 0) py[i] = -py[i];
    }
    u32 t = 0, remaining = cnt, guard = 0;
    while (remaining > 3 && guard++ < cnt * cnt) {
        b32 clipped = 0;
        for (u32 i = 0; i < remaining; ++i) {
            u32 ia = idx[(i + remaining - 1) % remaining], ib = idx[i], ic = idx[(i + 1) % remaining];
            f32 cross = (px[ib] - px[ia]) * (py[ic] - py[ia]) - (py[ib] - py[ia]) * (px[ic] - px[ia]);
            if (cross <= 0) continue;
            b32 inside = 0;
            for (u32 k = 0; k < remaining && !inside; ++k) {
                u32 ip = idx[k];
                if (ip == ia || ip == ib || ip == ic) continue;
                f32 d1 = (px[ib] - px[ia]) * (py[ip] - py[ia]) - (py[ib] - py[ia]) * (px[ip] - px[ia]);
                f32 d2 = (px[ic] - px[ib]) * (py[ip] - py[ib]) - (py[ic] - py[ib]) * (px[ip] - px[ib]);
                f32 d3 = (px[ia] - px[ic]) * (py[ip] - py[ic]) - (py[ia] - py[ic]) * (px[ip] - px[ic]);
                if (d1 >= 0 && d2 >= 0 && d3 >= 0) inside = 1;
            }
            if (inside) continue;
            out[t * 3] = v[ia]; out[t * 3 + 1] = v[ib]; out[t * 3 + 2] = v[ic];
            t++;
            for (u32 k = i; k + 1 < remaining; ++k) idx[k] = idx[k + 1];
            remaining--;
            clipped = 1;
            break;
        }
        if (!clipped) break;
    }
    if (remaining > 3) {
        /* degenerate / self-intersecting: fan the rest */
        for (u32 i = 1; i + 1 < remaining; ++i) { out[t * 3] = v[idx[0]]; out[t * 3 + 1] = v[idx[i]]; out[t * 3 + 2] = v[idx[i + 1]]; t++; }
    } else if (remaining == 3) {
        out[t * 3] = v[idx[0]]; out[t * 3 + 1] = v[idx[1]]; out[t * 3 + 2] = v[idx[2]]; t++;
    }
    return t;
}

u32 a3_emesh_triangulate(const A3EMesh *m, u32 **out_tris, u32 **out_face) {
    u32 total = 0;
    for (u32 f = 0; f < m->fsize.count; ++f) total += m->fsize.data[f] - 2;
    u32 *tris = (u32 *)a3_malloc(sizeof(u32) * 3 * (total + 1), TAG);
    u32 *face = out_face ? (u32 *)a3_malloc(sizeof(u32) * (total + 1), TAG) : 0;
    u32 t = 0;
    for (u32 f = 0; f < m->fsize.count; ++f) {
        u32 k = triangulate_face(m, f, tris + t * 3);
        if (face) for (u32 i = 0; i < k; ++i) face[t + i] = f;
        t += k;
    }
    *out_tris = tris;
    if (out_face) *out_face = face;
    return t;
}

u32 a3_emesh_raycast(A3EMesh *m, A3Vec3 origin, A3Vec3 dir, f32 *t) {
    u32 *tris, *face;
    u32 nt = a3_emesh_triangulate(m, &tris, &face);
    f32 ray[8] = { origin.x, origin.y, origin.z, 1.0f, dir.x, dir.y, dir.z, 0.0f };
    f32 best = 1e30f;
    u32 hit = a3_mk_raycast(m->pos.data, tris, nt, ray, &best);
    u32 f = hit ? face[hit - 1] : A3_ENONE;
    if (t) *t = best;
    a3_free(tris);
    a3_free(face);
    return f;
}

static A3Vec2 box_uv(A3Vec3 p, A3Vec3 n) {
    f32 ax = a3_absf(n.x), ay = a3_absf(n.y), az = a3_absf(n.z);
    if (ax >= ay && ax >= az) return a3_v2(n.x > 0 ? -p.z : p.z, p.y);
    if (ay >= az) return a3_v2(p.x, n.y > 0 ? -p.z : p.z);
    return a3_v2(n.z > 0 ? p.x : -p.x, p.y);
}

b32 a3_emesh_to_mesh_data(A3EMesh *m, A3MeshData *out) {
    a3_zero_struct(out);
    u32 *tris, *face;
    u32 nt = a3_emesh_triangulate(m, &tris, &face);
    if (!nt) { a3_free(tris); a3_free(face); return 0; }
    b32 ok;
    if (m->smooth) {
        u32 nv = m->pos.count;
        A3Vec4 *tn = (A3Vec4 *)a3_malloc(sizeof(A3Vec4) * nt, A3_MEM_TEMP);
        a3_mk_tri_normals(m->pos.data, tris, nt, tn);        /* assembly */
        A3Vec3 *vn = (A3Vec3 *)a3_calloc(sizeof(A3Vec3) * (nv + 1), A3_MEM_TEMP);
        for (u32 t = 0; t < nt; ++t) for (u32 k = 0; k < 3; ++k) vn[tris[t * 3 + k]] = a3_v3_add(vn[tris[t * 3 + k]], a3_v4_xyz(tn[t]));
        ok = a3_mesh_alloc(out, nv, nt * 3);
        if (ok) {
            for (u32 v = 0; v < nv; ++v) {
                A3Vec3 n = a3_v3_norm(vn[v]);
                out->vertices[v].position = P(m, v);
                out->vertices[v].normal = n;
                out->vertices[v].uv = box_uv(P(m, v), n);
            }
            a3_memcpy(out->indices, tris, sizeof(u32) * nt * 3);
        }
        a3_free(tn);
        a3_free(vn);
    } else {
        ok = a3_mesh_alloc(out, nt * 3, nt * 3);
        if (ok) {
            for (u32 t = 0; t < nt; ++t) {
                A3Vec3 n = a3_emesh_face_normal(m, face[t]);
                for (u32 k = 0; k < 3; ++k) {
                    A3Vertex *vx = &out->vertices[t * 3 + k];
                    vx->position = P(m, tris[t * 3 + k]);
                    vx->normal = n;
                    vx->uv = box_uv(vx->position, n);
                    out->indices[t * 3 + k] = t * 3 + k;
                }
            }
        }
    }
    if (ok) a3_mesh_compute_bounds(out);
    a3_free(tris);
    a3_free(face);
    return ok;
}

void a3_emesh_save_obj(const A3EMesh *m, A3StrBuf *out, const char *name) {
    a3_strbuf_appendf(out, "# ASM3D model: %u vertices, %u faces\n", m->pos.count, m->fsize.count);
    a3_strbuf_appendf(out, "o %s\n", name && name[0] ? name : "Model");
    for (u32 v = 0; v < m->pos.count; ++v) a3_strbuf_appendf(out, "v %.9g %.9g %.9g\n", (f64)m->pos.data[v].x, (f64)m->pos.data[v].y, (f64)m->pos.data[v].z);
    if (m->smooth) {
        a3_strbuf_append(out, "s 1\n");
        for (u32 f = 0; f < m->fsize.count; ++f) {
            u32 n;
            const u32 *v = a3_emesh_face(m, f, &n);
            a3_strbuf_append(out, "f");
            for (u32 i = 0; i < n; ++i) a3_strbuf_appendf(out, " %u", v[i] + 1);
            a3_strbuf_append_char(out, '\n');
        }
        return;
    }
    /* flat shading: one normal per face keeps hard edges in any OBJ reader */
    a3_strbuf_append(out, "s off\n");
    for (u32 f = 0; f < m->fsize.count; ++f) {
        A3Vec3 n = a3_emesh_face_normal(m, f);
        a3_strbuf_appendf(out, "vn %.6g %.6g %.6g\n", (f64)n.x, (f64)n.y, (f64)n.z);
    }
    for (u32 f = 0; f < m->fsize.count; ++f) {
        u32 n;
        const u32 *v = a3_emesh_face(m, f, &n);
        a3_strbuf_append(out, "f");
        for (u32 i = 0; i < n; ++i) a3_strbuf_appendf(out, " %u//%u", v[i] + 1, f + 1);
        a3_strbuf_append_char(out, '\n');
    }
}

b32 a3_emesh_load_obj(A3EMesh *m, const char *text, usize len, char *err, usize err_cap) {
    a3_emesh_clear(m);
    if (err && err_cap) err[0] = 0;
    A3Str rest = a3_str_n(text, len);
    u32 line_no = 0;
    while (rest.len) {
        A3Str line = a3_str_trim(a3_str_split_next(&rest, '\n'));
        line_no++;
        if (line.len < 2 || line.ptr[0] == '#') continue;
        if (line.ptr[0] == 'v' && line.ptr[1] == ' ') {
            f32 c[3] = { 0, 0, 0 };
            A3Str r = a3_str_sub(line, 2, line.len - 2);
            for (u32 i = 0; i < 3; ++i) {
                r = a3_str_trim(r);
                isize sp = a3_str_find(r, ' ');
                A3Str tok = sp < 0 ? r : a3_str_sub(r, 0, (usize)sp);
                f64 d;
                if (!a3_parse_f64(tok.ptr, tok.len, &d)) { if (err) a3_snprintf(err, err_cap, "line %u: bad vertex", line_no); return 0; }
                c[i] = (f32)d;
                r = sp < 0 ? a3_str_sub(r, r.len, 0) : a3_str_sub(r, (usize)sp + 1, r.len - (usize)sp - 1);
            }
            a3_emesh_add_vertex(m, a3_v3(c[0], c[1], c[2]));
        } else if (line.ptr[0] == 's' && line.ptr[1] == ' ') {
            m->smooth = !(line.len >= 5 && line.ptr[2] == 'o' && line.ptr[3] == 'f' && line.ptr[4] == 'f') && line.ptr[2] != '0';
        } else if (line.ptr[0] == 'f' && line.ptr[1] == ' ') {
            u32 idx[1024], n = 0;
            A3Str r = a3_str_sub(line, 2, line.len - 2);
            while (r.len && n < 1024) {
                r = a3_str_trim(r);
                if (!r.len) break;
                isize sp = a3_str_find(r, ' ');
                A3Str tok = sp < 0 ? r : a3_str_sub(r, 0, (usize)sp);
                i64 vi = 0;
                b32 neg = 0;
                usize k = 0;
                if (k < tok.len && tok.ptr[k] == '-') { neg = 1; k++; }
                for (; k < tok.len && a3_is_digit(tok.ptr[k]); ++k) vi = vi * 10 + (tok.ptr[k] - '0');
                if (neg) vi = (i64)m->pos.count - vi; else vi -= 1;
                if (vi < 0 || vi >= (i64)m->pos.count) { if (err) a3_snprintf(err, err_cap, "line %u: face uses vertex %lld, but only %u exist", line_no, (long long)vi + 1, m->pos.count); return 0; }
                idx[n++] = (u32)vi;
                r = sp < 0 ? a3_str_sub(r, r.len, 0) : a3_str_sub(r, (usize)sp + 1, r.len - (usize)sp - 1);
            }
            if (a3_emesh_add_face(m, idx, n) == A3_ENONE && err && !err[0]) a3_snprintf(err, err_cap, "line %u: skipped a degenerate face", line_no);
        }
    }
    if (err) err[0] = 0;
    return m->pos.count > 0;
}
