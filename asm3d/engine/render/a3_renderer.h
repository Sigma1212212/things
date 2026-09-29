/*
 * ASM3D - a3_renderer.h
 * Forward+ style PBR renderer (desktop): shadow-mapped sun, up to 16 local
 * lights, procedural sky, HDR with ACES tonemapping, FXAA, instanced
 * batching and SIMD/assembly frustum culling. Custom surface shaders from the
 * Shader Maker plug into the same pipeline.
 */
#ifndef A3_RENDERER_H
#define A3_RENDERER_H

#include "a3_rhi.h"
#include "../ecs/a3_ecs.h"

A3_EXTERN_C_BEGIN

typedef struct A3Renderer A3Renderer;

typedef struct A3RenderSettings {
    b32 shadows;
    i32 shadow_map_size;     /* 512..4096 */
    f32 shadow_distance;     /* meters covered by the sun shadow */
    b32 fxaa;
    f32 vignette;
    b32 frustum_culling;
    b32 wireframe;
    i32 max_lights;          /* <= 16 */
} A3RenderSettings;

typedef struct A3RenderView {
    A3Mat4 view, proj;
    A3Vec3 camera_pos;
    f32 near_plane, far_plane;
    i32 width, height;
    A3RhiTarget target;      /* {0} = window */
    b32 draw_sky;
    A3Vec4 clear_color;
    f32 exposure;
    f32 time;
    b32 draw_grid;           /* editor ground grid */
    b32 draw_debug;          /* debug lines / overlays */
} A3RenderView;

typedef struct A3RenderFrameInfo {
    u32 renderables;
    u32 visible;
    u32 culled;
    u32 batches;
    u32 shadow_casters;
    u32 lights;
    u32 particles;
    f64 cpu_ms;
} A3RenderFrameInfo;

A3Renderer *a3_renderer_create(void);
void a3_renderer_destroy(A3Renderer *r);
A3RenderSettings *a3_renderer_settings(A3Renderer *r);
const A3RenderFrameInfo *a3_renderer_frame_info(A3Renderer *r);

/* Builds a view from a Camera entity. Returns false if the entity has no camera. */
b32 a3_render_view_from_camera(A3World *w, A3Entity camera, i32 width, i32 height, A3RenderView *out);
A3RenderView a3_render_view_look_at(A3Vec3 eye, A3Vec3 target, f32 fov_deg, i32 width, i32 height);

void a3_renderer_draw_world(A3Renderer *r, A3World *w, const A3RenderView *view);

/* ---- Debug drawing (cleared after each draw_world) ---- */
void a3_debug_line(A3Renderer *r, A3Vec3 a, A3Vec3 b, A3Vec4 color);
void a3_debug_aabb(A3Renderer *r, A3Aabb box, A3Vec4 color);
void a3_debug_obb(A3Renderer *r, const A3Mat4 *m, A3Aabb local, A3Vec4 color);
void a3_debug_sphere(A3Renderer *r, A3Vec3 c, f32 radius, A3Vec4 color);
void a3_debug_circle(A3Renderer *r, A3Vec3 c, A3Vec3 normal, f32 radius, A3Vec4 color);
void a3_debug_arrow(A3Renderer *r, A3Vec3 from, A3Vec3 to, A3Vec4 color);
void a3_debug_set_depth_test(A3Renderer *r, b32 on);

/* ---- Custom surface materials (Shader Maker) ----
 * surface_code must define `void a3_surface(inout A3Surface s)`.
 * Error line numbers in `result` refer to surface_code. Returns a material
 * id (>0) or 0 on failure. */
u32  a3_renderer_material_create(A3Renderer *r, const char *surface_code, const char *name, A3ShaderCompileResult *result);
b32  a3_renderer_material_update(A3Renderer *r, u32 material, const char *surface_code, A3ShaderCompileResult *result);
void a3_renderer_material_destroy(A3Renderer *r, u32 material);
void a3_renderer_material_set_params(A3Renderer *r, u32 material, const A3Vec4 params[4]);
void a3_renderer_material_set_texture(A3Renderer *r, u32 material, u32 slot, u32 texture_asset);
/* MeshRenderer.material may point to a .a3shader file; it is loaded on first
 * use and cached. Call after the file changes so the next frame reloads it. */
void a3_renderer_material_invalidate(A3Renderer *r, const char *path);
/* Maps an entity to a material without an asset file (Shader Maker preview). */
void a3_renderer_set_entity_material(A3Renderer *r, A3World *w, A3Entity e, u32 material);

/* Reads back the last rendered view (RGBA8, bottom-up). Used for screenshots,
 * thumbnails and automated render tests. */
b32 a3_renderer_read_view(A3Renderer *r, i32 width, i32 height, u8 *out_rgba);

A3_EXTERN_C_END

#endif
