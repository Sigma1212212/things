/*
 * ASM3D - a3_assets.h
 * Runtime asset cache. Assets are addressed by project-relative paths
 * ("Assets/Models/crate.obj"). Loading never fails hard: a missing or broken
 * asset is replaced by a visible placeholder (checker texture / cube) and a
 * plain-language error is logged once, so the game keeps running.
 */
#ifndef A3_ASSETS_H
#define A3_ASSETS_H

#include "../core/a3_base.h"
#include "../render/a3_mesh.h"
#include "../render/a3_rhi.h"
#include "../ecs/a3_reflect.h"
#include "../scene/a3_components.h"

A3_EXTERN_C_BEGIN

#define A3_ASSET_INVALID 0u

typedef enum A3AssetState { A3_ASSET_STATE_EMPTY = 0, A3_ASSET_STATE_LOADED, A3_ASSET_STATE_FAILED } A3AssetState;

typedef struct A3MeshAsset {
    char path[A3_PATH_MAX];
    A3AssetState state;
    A3MeshData cpu;          /* kept for physics colliders and picking */
    A3RhiBuffer vb, ib;
    A3RhiMesh gpu;
    u32 index_count;
    A3Aabb bounds;
    f32 radius;
    u32 version;             /* bumped on hot reload */
    char error[160];
} A3MeshAsset;

typedef struct A3TextureAsset {
    char path[A3_PATH_MAX];
    A3AssetState state;
    A3RhiTexture tex;
    i32 width, height;
    u32 version;
    char error[160];
} A3TextureAsset;

b32  a3_assets_init(const char *project_root);  /* project_root may be "" */
void a3_assets_shutdown(void);
const char *a3_assets_root(void);
void a3_assets_set_root(const char *project_root);
/* Resolves a project-relative path to a filesystem path. */
void a3_assets_path(const char *rel, char *out, usize cap);
/* Uploads pending CPU data once a GPU context exists. */
void a3_assets_gpu_ready(void);

u32 a3_assets_mesh_primitive(A3Primitive p);
u32 a3_assets_mesh(const char *path);
/* Registers a procedurally generated mesh under a unique name (takes ownership of data). */
u32 a3_assets_mesh_from_data(const char *name, A3MeshData *data);
const A3MeshAsset *a3_assets_mesh_get(u32 id);
u32 a3_assets_mesh_count(void);

u32 a3_assets_texture(const char *path);
u32 a3_assets_texture_from_pixels(const char *name, const u8 *rgba, i32 w, i32 h, b32 mipmaps);
const A3TextureAsset *a3_assets_texture_get(u32 id);
A3RhiTexture a3_assets_texture_rhi(u32 id);
A3RhiTexture a3_assets_white_texture(void);
A3RhiTexture a3_assets_checker_texture(void);
u32 a3_assets_texture_count(void);

/* Resolve component asset references (caches the handle inside the ref). */
u32 a3_assets_resolve_mesh(A3AssetRef *ref);
u32 a3_assets_resolve_texture(A3AssetRef *ref);
/* Mesh used by a MeshRenderer (asset if set, otherwise primitive). */
u32 a3_assets_mesh_for_renderer(A3CMeshRenderer *mr);

/* Reloads an asset whose file changed on disk (editor hot reload). */
b32 a3_assets_reload(const char *path);
/* Number of assets that failed to load (project health). */
u32 a3_assets_failed_count(void);

A3_EXTERN_C_END

#endif
