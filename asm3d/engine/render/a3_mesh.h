/*
 * ASM3D - a3_mesh.h
 * CPU-side mesh data, procedural primitives and the OBJ importer.
 */
#ifndef A3_MESH_H
#define A3_MESH_H

#include "../core/a3_base.h"
#include "../core/a3_math.h"

A3_EXTERN_C_BEGIN

typedef struct A3Vertex {
    A3Vec3 position;
    A3Vec3 normal;
    A3Vec2 uv;
} A3Vertex;

typedef struct A3MeshData {
    A3Vertex *vertices;
    u32 *indices;
    u32 vertex_count;
    u32 index_count;
    A3Aabb bounds;
    f32 radius;          /* bounding sphere radius around bounds center */
} A3MeshData;

void a3_mesh_free(A3MeshData *m);
b32  a3_mesh_alloc(A3MeshData *m, u32 vertex_count, u32 index_count);
void a3_mesh_compute_bounds(A3MeshData *m);
void a3_mesh_compute_normals(A3MeshData *m);

/* Primitives are unit-sized and centered on the origin (plane lies in XZ). */
b32 a3_mesh_cube(A3MeshData *m);
b32 a3_mesh_sphere(A3MeshData *m, u32 segments, u32 rings);
b32 a3_mesh_plane(A3MeshData *m, u32 subdivisions, f32 uv_scale);
b32 a3_mesh_cylinder(A3MeshData *m, u32 segments);
b32 a3_mesh_cone(A3MeshData *m, u32 segments);
b32 a3_mesh_capsule(A3MeshData *m, u32 segments, u32 rings); /* height 2, radius 0.5 */
/* Heightfield grid (terrain chunks): heights[(z*(res+1))+x], size in meters. */
b32 a3_mesh_heightfield(A3MeshData *m, const f32 *heights, u32 res, f32 size, f32 uv_scale);

/* Wavefront OBJ (positions, normals, uvs, polygons -> triangles).
 * err receives a readable message with the line number on failure. */
A3Result a3_mesh_load_obj(const char *text, usize len, A3MeshData *out, char *err, usize err_cap);

A3_EXTERN_C_END

#endif
