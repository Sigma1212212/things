/*
 * ASM3D - a3_emesh.h
 * Editable polygon meshes: the data model of the modeling workspace.
 *
 * Faces are polygons of any size (triangles, quads, n-gons) listed as vertex
 * indices in counter-clockwise order seen from outside. Edges are derived
 * from the faces. Selection works like other modelers: in vertex and edge
 * mode the vertex selection decides, in face mode the face selection does.
 *
 * Operations (all undoable by copying the mesh first):
 *   primitives        cube, plane, grid, cylinder, cone, UV sphere, torus
 *   transform         move / rotate / scale the selection around its center
 *   extrude           selected faces as one region, along their normals
 *   inset             selected faces, each on its own
 *   loop cut          a ring of quads through an edge, with 1..n cuts
 *   subdivide         Catmull-Clark (smooth) or simple quad split
 *   bevel             not implemented yet
 *   merge by distance, delete, fill, flip, recalculate normals, mirror
 * Hot loops (point transform, normals, ray picking, bounds) run in x86-64
 * assembly: see a3_modeling_kernels.h.
 */
#ifndef A3_EMESH_H
#define A3_EMESH_H

#include "../core/a3_base.h"
#include "../core/a3_math.h"
#include "../core/a3_memory.h"
#include "../core/a3_strbuf.h"

A3_EXTERN_C_BEGIN

typedef enum A3ESelectMode { A3_ESEL_VERTEX = 0, A3_ESEL_EDGE, A3_ESEL_FACE } A3ESelectMode;

typedef struct A3EEdge {
    u32 a, b;            /* a < b */
    u32 f0, f1;          /* first two faces using it (A3_ENONE when absent) */
    u32 face_count;
} A3EEdge;

#define A3_ENONE 0xFFFFFFFFu

typedef struct A3EMesh {
    A3_ARRAY_TYPE(A3Vec4) pos;     /* w = 1 */
    A3_ARRAY_TYPE(u8) vsel;
    A3_ARRAY_TYPE(u32) fstart;     /* first index in loops */
    A3_ARRAY_TYPE(u32) fsize;      /* vertex count */
    A3_ARRAY_TYPE(u8) fsel;
    A3_ARRAY_TYPE(u32) loops;      /* face corners (vertex indices) */
    A3_ARRAY_TYPE(A3EEdge) edges;  /* derived: a3_emesh_edges() */
    b32 edges_valid;
    i32 select_mode;               /* A3ESelectMode */
    b32 smooth;                    /* smooth shading when converted for rendering */
} A3EMesh;

typedef struct A3EMeshStats {
    u32 vertices, edges, faces, triangles;
    u32 selected_vertices, selected_edges, selected_faces;
    u32 boundary_edges, nonmanifold_edges;
    b32 closed;                    /* every edge has exactly two faces */
    A3Vec3 min, max;
} A3EMeshStats;

/* ---- lifetime ---- */
void a3_emesh_init(A3EMesh *m);
void a3_emesh_free(A3EMesh *m);
void a3_emesh_clear(A3EMesh *m);
b32  a3_emesh_copy(A3EMesh *dst, const A3EMesh *src);   /* dst must be initialized */

/* ---- building ---- */
u32  a3_emesh_add_vertex(A3EMesh *m, A3Vec3 p);
u32  a3_emesh_add_face(A3EMesh *m, const u32 *verts, u32 n);  /* A3_ENONE if invalid */
A3_INLINE u32 a3_emesh_vertex_count(const A3EMesh *m) { return m->pos.count; }
A3_INLINE u32 a3_emesh_face_count(const A3EMesh *m) { return m->fsize.count; }
A3_INLINE const u32 *a3_emesh_face(const A3EMesh *m, u32 f, u32 *n) { *n = m->fsize.data[f]; return m->loops.data + m->fstart.data[f]; }
A3_INLINE A3Vec3 a3_emesh_vertex(const A3EMesh *m, u32 v) { return a3_v4_xyz(m->pos.data[v]); }
void a3_emesh_set_vertex(A3EMesh *m, u32 v, A3Vec3 p);

/* ---- primitives (replace the mesh; centered on the origin) ---- */
typedef enum A3EPrimitive {
    A3_EPRIM_CUBE = 0, A3_EPRIM_PLANE, A3_EPRIM_GRID, A3_EPRIM_CYLINDER, A3_EPRIM_CONE, A3_EPRIM_SPHERE, A3_EPRIM_TORUS, A3_EPRIM_COUNT
} A3EPrimitive;
extern const char *const a3_eprim_names[A3_EPRIM_COUNT];
/* size = overall size (cube edge, diameter...); segments / rings where relevant (clamped). */
void a3_emesh_make(A3EMesh *m, A3EPrimitive prim, f32 size, u32 segments, u32 rings);
/* Adds a primitive to the existing mesh, selected, at `center`. */
void a3_emesh_add_primitive(A3EMesh *m, A3EPrimitive prim, f32 size, u32 segments, u32 rings, A3Vec3 center);

/* ---- edges and geometry ---- */
void   a3_emesh_edges(A3EMesh *m);                            /* rebuilds when topology changed */
u32    a3_emesh_find_edge(A3EMesh *m, u32 a, u32 b);          /* edge index or A3_ENONE */
A3Vec3 a3_emesh_face_normal(const A3EMesh *m, u32 f);         /* unit (Newell), 0 if degenerate */
A3Vec3 a3_emesh_face_center(const A3EMesh *m, u32 f);
void   a3_emesh_stats(A3EMesh *m, A3EMeshStats *out);

/* ---- selection ---- */
void a3_emesh_set_select_mode(A3EMesh *m, A3ESelectMode mode);
void a3_emesh_select_all(A3EMesh *m, b32 select);
void a3_emesh_select_invert(A3EMesh *m);
void a3_emesh_select_vertex(A3EMesh *m, u32 v, b32 select);
void a3_emesh_select_edge(A3EMesh *m, u32 e, b32 select);
void a3_emesh_select_face(A3EMesh *m, u32 f, b32 select);
void a3_emesh_select_linked(A3EMesh *m);                      /* grow to connected parts */
void a3_emesh_select_more(A3EMesh *m);                        /* one ring of neighbors */
void a3_emesh_select_edge_loop(A3EMesh *m, u32 e);            /* adds the loop through quads */
void a3_emesh_sync_selection(A3EMesh *m);                     /* derive the other selection */
u32  a3_emesh_selected_count(const A3EMesh *m);               /* vertices (or faces in face mode) */
A3Vec3 a3_emesh_selection_center(const A3EMesh *m);          /* median of selected vertices */

/* ---- transform (assembly kernel) ---- */
void a3_emesh_transform_selected(A3EMesh *m, const A3Mat4 *mat);
void a3_emesh_translate(A3EMesh *m, A3Vec3 d);
void a3_emesh_rotate(A3EMesh *m, A3Vec3 euler_degrees, A3Vec3 pivot);
void a3_emesh_scale(A3EMesh *m, A3Vec3 s, A3Vec3 pivot);
void a3_emesh_transform_all(A3EMesh *m, const A3Mat4 *mat);

/* ---- modeling operations; return false (with nothing changed) when not applicable ---- */
b32 a3_emesh_extrude(A3EMesh *m, f32 distance);                /* selected faces, as one region */
b32 a3_emesh_inset(A3EMesh *m, f32 amount, f32 depth);         /* amount 0..1 toward each face center */
b32 a3_emesh_loop_cut(A3EMesh *m, u32 edge, u32 cuts);         /* ring of quads through `edge` */
b32 a3_emesh_subdivide(A3EMesh *m, b32 smooth);                /* whole mesh, one level */
b32 a3_emesh_delete_faces(A3EMesh *m);                         /* selected faces (keeps used vertices) */
b32 a3_emesh_delete_vertices(A3EMesh *m);                      /* selected vertices and their faces */
u32 a3_emesh_merge_by_distance(A3EMesh *m, f32 distance, b32 selected_only); /* removed vertex count */
b32 a3_emesh_fill(A3EMesh *m);                                 /* face from the selected boundary loop / vertices */
b32 a3_emesh_flip(A3EMesh *m, b32 selected_only);
u32 a3_emesh_recalc_normals(A3EMesh *m);                       /* consistent, outward; flipped face count */
b32 a3_emesh_mirror(A3EMesh *m, u32 axis, f32 merge_distance); /* whole mesh across axis 0/1/2 */
b32 a3_emesh_duplicate(A3EMesh *m);                            /* selected faces; the copy becomes selected */
void a3_emesh_remove_loose(A3EMesh *m);                        /* vertices used by no face */

/* ---- picking (assembly ray kernel over a triangulation) ---- */
/* Nearest face hit by the ray; returns face index or A3_ENONE, distance in *t. */
u32 a3_emesh_raycast(A3EMesh *m, A3Vec3 origin, A3Vec3 dir, f32 *t);

/* ---- conversion ---- */
/* Triangles (index triples into the vertex array) and the face each came from. */
u32  a3_emesh_triangulate(const A3EMesh *m, u32 **out_tris, u32 **out_face_of_tri);
struct A3MeshData;
/* Render mesh: flat shading (per-face normals) or smooth (area weighted); box-projected UVs. */
b32  a3_emesh_to_mesh_data(A3EMesh *m, struct A3MeshData *out);
void a3_emesh_save_obj(const A3EMesh *m, A3StrBuf *out, const char *object_name);
/* Reads v / f lines (polygons kept, vt / vn ignored). */
b32  a3_emesh_load_obj(A3EMesh *m, const char *text, usize len, char *err, usize err_cap);

A3_EXTERN_C_END

#endif
