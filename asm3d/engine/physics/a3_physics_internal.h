/*
 * ASM3D - a3_physics_internal.h
 * Narrowphase + triangle BVH (internal to the physics module).
 */
#ifndef A3_PHYSICS_INTERNAL_H
#define A3_PHYSICS_INTERNAL_H

#include "a3_physics.h"

A3_EXTERN_C_BEGIN

#define A3_MAX_MANIFOLD 4

typedef struct A3ContactPoint {
    A3Vec3 pos;        /* world space, on B's surface */
    f32 depth;         /* penetration (> 0) */
} A3ContactPoint;

typedef struct A3Manifold {
    A3Vec3 normal;     /* from A towards B */
    u32 count;
    A3ContactPoint pts[A3_MAX_MANIFOLD];
} A3Manifold;

/* ---- triangle BVH for static mesh colliders (world space) ---- */
typedef struct A3BvhNode {
    A3Aabb box;
    u32 first;         /* leaf: first triangle index; inner: left child */
    u32 count;         /* leaf: triangle count; inner: 0 */
} A3BvhNode;

typedef struct A3TriMesh {
    A3Vec3 *verts;     /* 3 per triangle, world space */
    u32 tri_count;
    u32 *tri_order;
    A3BvhNode *nodes;
    u32 node_count;
    A3Aabb bounds;
    u64 source_hash;   /* transform + mesh version, to detect changes */
} A3TriMesh;

b32  a3_trimesh_build(A3TriMesh *tm, const A3Vec3 *positions, const u32 *indices, u32 index_count, const A3Mat4 *world);
void a3_trimesh_free(A3TriMesh *tm);
typedef b32 (*A3TriVisitFn)(const A3Vec3 *tri, u32 tri_index, void *user); /* return false to stop */
void a3_trimesh_query_aabb(const A3TriMesh *tm, A3Aabb box, A3TriVisitFn fn, void *user);
b32  a3_trimesh_raycast(const A3TriMesh *tm, A3Vec3 origin, A3Vec3 dir, f32 max_t, f32 *t_out, A3Vec3 *normal_out);

/* ---- geometry helpers ---- */
A3Vec3 a3_closest_point_triangle(A3Vec3 p, A3Vec3 a, A3Vec3 b, A3Vec3 c);
f32    a3_closest_segment_segment(A3Vec3 p1, A3Vec3 q1, A3Vec3 p2, A3Vec3 q2, f32 *s, f32 *t, A3Vec3 *c1, A3Vec3 *c2);
/* Closest points between segment pq and triangle abc (squared distance returned). */
f32    a3_closest_segment_triangle(A3Vec3 p, A3Vec3 q, A3Vec3 a, A3Vec3 b, A3Vec3 c, A3Vec3 *on_seg, A3Vec3 *on_tri);
void   a3_capsule_segment(const A3ShapeInstance *s, A3Vec3 *p0, A3Vec3 *p1);

/* ---- narrowphase ----
 * Returns true and fills m when shapes touch. Mesh shapes must be `a` or `b`
 * with the other being convex; `mesh` supplies the triangle data. */
b32 a3_collide_convex(const A3ShapeInstance *a, const A3ShapeInstance *b, A3Manifold *m);
b32 a3_collide_mesh(const A3TriMesh *mesh, const A3ShapeInstance *b, A3Manifold *m);
/* Shape vs single triangle (normal from triangle towards the shape). */
b32 a3_collide_triangle(const A3Vec3 tri[3], const A3ShapeInstance *b, A3Manifold *m);
/* Ray vs convex shape; returns distance along normalized dir. */
b32 a3_raycast_shape(const A3ShapeInstance *s, A3Vec3 origin, A3Vec3 dir, f32 max_t, f32 *t, A3Vec3 *normal);

A3_EXTERN_C_END

#endif
