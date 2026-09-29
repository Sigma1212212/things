/*
 * ASM3D - a3_renderer.c
 */
#include "a3_renderer.h"
#include "a3_shaders.h"
#include "a3_mesh.h"
#include "../core/a3_log.h"
#include "../core/a3_memory.h"
#include "../core/a3_string.h"
#include "../core/a3_format.h"
#include "../core/a3_sort.h"
#include "../core/a3_strbuf.h"
#include "../core/a3_hash.h"
#include "../resource/a3_assets.h"
#include "../scene/a3_components.h"
#include "../platform/a3_platform.h"

#define MAX_MATERIALS 256
#define MAX_LIGHTS 16

typedef struct InstanceData {
    A3Mat4 model;
    A3Vec4 color;
    A3Vec4 params;  /* metallic, roughness, emissive, receive_shadows */
} InstanceData;

typedef struct DebugVertex { A3Vec3 pos; u32 color; } DebugVertex;

typedef struct Material {
    b32 used;
    A3RhiShader shader;
    A3Vec4 params[4];
    u32 textures[2];
    char name[64];
} Material;

typedef struct Batch {
    u32 mesh;
    u32 texture;
    u32 material;
    u32 first;       /* first instance */
    u32 count;
    b32 transparent;
} Batch;

typedef struct Renderable {
    u32 mesh, texture, material;
    b32 cast_shadows, transparent;
    InstanceData inst;
    A3Vec4 sphere;
} Renderable;

struct A3Renderer {
    A3RenderSettings settings;
    A3RenderFrameInfo info;
    A3RhiShader lit, shadow, sky, tonemap, fxaa, lines, grid;
    A3RhiBuffer instance_buf;
    A3VertexLayout instance_layout;
    /* HDR chain */
    i32 rt_w, rt_h;
    A3RhiTexture hdr_color, hdr_depth, ldr_color;
    A3RhiTarget hdr_target, ldr_target;
    /* shadow */
    i32 shadow_size;
    A3RhiTexture shadow_tex;
    A3RhiTarget shadow_target;
    /* debug lines */
    A3_ARRAY_TYPE(DebugVertex) debug_verts;
    A3RhiBuffer debug_buf;
    A3RhiMesh debug_mesh;
    b32 debug_depth;
    /* editor grid */
    A3RhiBuffer grid_vb, grid_ib;
    A3RhiMesh grid_mesh;
    Material materials[MAX_MATERIALS];
    A3HashMap entity_materials; /* guid -> material id */
    /* last view for readback */
    A3RhiTarget last_target;
};

/* ======================================================================== */
/* Setup                                                                    */
/* ======================================================================== */

static A3RhiShader make_shader(const char *vs, const char *fs, const char *name) {
    A3ShaderCompileResult res;
    A3RhiShader s = a3_rhi_shader_create(vs, fs, name, &res);
    if (!s.id) A3_ERROR("render", "built-in shader '%s' failed; rendering will be incomplete", name);
    return s;
}

static A3RhiShader make_lit_shader(const char *surface, const char *name, A3ShaderCompileResult *res) {
    A3StrBuf fs;
    a3_strbuf_init(&fs, A3_MEM_RENDER);
    a3_strbuf_append(&fs, A3_SHADER_LIT_FS_HEAD);
    a3_strbuf_append(&fs, "#line 1\n");
    a3_strbuf_append(&fs, surface);
    a3_strbuf_append(&fs, "\n#line 10000\n");
    a3_strbuf_append(&fs, A3_SHADER_LIT_FS_TAIL);
    A3RhiShader s = a3_rhi_shader_create(A3_SHADER_LIT_VS, fs.data, name, res);
    a3_strbuf_free(&fs);
    if (s.id) {
        a3_rhi_set_int(s, "u_albedo_tex", 0);
        a3_rhi_set_int(s, "u_shadow_map", 1);
        a3_rhi_set_int(s, "u_tex1", 2);
        a3_rhi_set_int(s, "u_tex2", 3);
    }
    return s;
}

A3Renderer *a3_renderer_create(void) {
    A3Renderer *r = A3_NEW(A3Renderer, A3_MEM_RENDER);
    if (!r) return 0;
    r->settings.shadows = 1;
    r->settings.shadow_map_size = 2048;
    r->settings.shadow_distance = 60.0f;
    r->settings.fxaa = 1;
    r->settings.vignette = 0.25f;
    r->settings.frustum_culling = 1;
    r->settings.max_lights = MAX_LIGHTS;
    r->debug_depth = 1;
    A3ShaderCompileResult res;
    r->lit = make_lit_shader(A3_SHADER_DEFAULT_SURFACE, "lit", &res);
    if (!r->lit.id) A3_ERROR("render", "default lit shader failed:\n%s", res.raw_log);
    r->shadow = make_shader(A3_SHADER_SHADOW_VS, A3_SHADER_SHADOW_FS, "shadow");
    r->sky = make_shader(A3_SHADER_SKY_VS, A3_SHADER_SKY_FS, "sky");
    r->tonemap = make_shader(A3_SHADER_FULLSCREEN_VS, A3_SHADER_TONEMAP_FS, "tonemap");
    r->fxaa = make_shader(A3_SHADER_FULLSCREEN_VS, A3_SHADER_FXAA_FS, "fxaa");
    r->lines = make_shader(A3_SHADER_LINES_VS, A3_SHADER_LINES_FS, "lines");
    r->grid = make_shader(A3_SHADER_GRID_VS, A3_SHADER_GRID_FS, "grid");
    r->instance_buf = a3_rhi_buffer_create(A3_BUFFER_VERTEX, sizeof(InstanceData) * 1024, 0, 1);
    A3VertexLayout *il = &r->instance_layout;
    il->stride = sizeof(InstanceData);
    for (u32 i = 0; i < 4; ++i) il->attribs[i] = (A3VertexAttrib){ 4 + i, 4, A3_ATTR_FLOAT, i * 16, 1 };
    il->attribs[4] = (A3VertexAttrib){ 8, 4, A3_ATTR_FLOAT, (u32)A3_OFFSETOF(InstanceData, color), 1 };
    il->attribs[5] = (A3VertexAttrib){ 9, 4, A3_ATTR_FLOAT, (u32)A3_OFFSETOF(InstanceData, params), 1 };
    il->count = 6;
    /* debug lines */
    r->debug_buf = a3_rhi_buffer_create(A3_BUFFER_VERTEX, sizeof(DebugVertex) * 4096, 0, 1);
    A3VertexLayout dl;
    a3_zero_struct(&dl);
    dl.stride = sizeof(DebugVertex);
    dl.attribs[0] = (A3VertexAttrib){ 0, 3, A3_ATTR_FLOAT, 0, 0 };
    dl.attribs[1] = (A3VertexAttrib){ 3, 4, A3_ATTR_UBYTE_NORM, 12, 0 };
    dl.count = 2;
    r->debug_mesh = a3_rhi_mesh_create(r->debug_buf, &dl, (A3RhiBuffer){ 0 }, 0);
    /* grid quad */
    f32 gv[] = { -1, 0, -1, 1, 0, -1, 1, 0, 1, -1, 0, 1 };
    u32 gi[] = { 0, 2, 1, 0, 3, 2 };
    r->grid_vb = a3_rhi_buffer_create(A3_BUFFER_VERTEX, sizeof(gv), gv, 0);
    r->grid_ib = a3_rhi_buffer_create(A3_BUFFER_INDEX, sizeof(gi), gi, 0);
    A3VertexLayout gl;
    a3_zero_struct(&gl);
    gl.stride = 12;
    gl.attribs[0] = (A3VertexAttrib){ 0, 3, A3_ATTR_FLOAT, 0, 0 };
    gl.count = 1;
    r->grid_mesh = a3_rhi_mesh_create(r->grid_vb, &gl, r->grid_ib, 1);
    a3_hashmap_init(&r->entity_materials, 64, A3_MEM_RENDER);
    a3_rhi_set_int(r->tonemap, "u_hdr", 0);
    a3_rhi_set_int(r->fxaa, "u_ldr", 0);
    return r;
}

static void destroy_targets(A3Renderer *r) {
    if (r->hdr_target.id) a3_rhi_target_destroy(r->hdr_target);
    if (r->ldr_target.id) a3_rhi_target_destroy(r->ldr_target);
    if (r->hdr_color.id) a3_rhi_texture_destroy(r->hdr_color);
    if (r->hdr_depth.id) a3_rhi_texture_destroy(r->hdr_depth);
    if (r->ldr_color.id) a3_rhi_texture_destroy(r->ldr_color);
    r->hdr_target.id = r->ldr_target.id = r->hdr_color.id = r->hdr_depth.id = r->ldr_color.id = 0;
    r->rt_w = r->rt_h = 0;
}

void a3_renderer_destroy(A3Renderer *r) {
    if (!r) return;
    destroy_targets(r);
    if (r->shadow_target.id) a3_rhi_target_destroy(r->shadow_target);
    if (r->shadow_tex.id) a3_rhi_texture_destroy(r->shadow_tex);
    A3RhiShader shaders[] = { r->lit, r->shadow, r->sky, r->tonemap, r->fxaa, r->lines, r->grid };
    for (u32 i = 0; i < A3_ARRAY_COUNT(shaders); ++i) a3_rhi_shader_destroy(shaders[i]);
    for (u32 i = 0; i < MAX_MATERIALS; ++i) if (r->materials[i].used) a3_rhi_shader_destroy(r->materials[i].shader);
    a3_rhi_mesh_destroy(r->debug_mesh);
    a3_rhi_mesh_destroy(r->grid_mesh);
    a3_rhi_buffer_destroy(r->debug_buf);
    a3_rhi_buffer_destroy(r->grid_vb);
    a3_rhi_buffer_destroy(r->grid_ib);
    a3_rhi_buffer_destroy(r->instance_buf);
    a3_array_free(r->debug_verts);
    a3_hashmap_free(&r->entity_materials);
    a3_free(r);
}

A3RenderSettings *a3_renderer_settings(A3Renderer *r) { return r ? &r->settings : 0; }
const A3RenderFrameInfo *a3_renderer_frame_info(A3Renderer *r) { return r ? &r->info : 0; }

static b32 ensure_targets(A3Renderer *r, i32 w, i32 h) {
    if (w <= 0 || h <= 0) return 0;
    if (r->rt_w == w && r->rt_h == h && r->hdr_target.id) return 1;
    destroy_targets(r);
    A3TextureDesc d;
    a3_zero_struct(&d);
    d.width = w; d.height = h; d.wrap = A3_WRAP_CLAMP; d.filter = A3_FILTER_LINEAR;
    d.format = A3_TEX_RGBA16F; d.debug_name = "hdr_color";
    r->hdr_color = a3_rhi_texture_create(&d);
    d.format = A3_TEX_DEPTH24; d.debug_name = "hdr_depth";
    r->hdr_depth = a3_rhi_texture_create(&d);
    d.format = A3_TEX_RGBA8; d.debug_name = "ldr_color";
    r->ldr_color = a3_rhi_texture_create(&d);
    r->hdr_target = a3_rhi_target_create(r->hdr_color, r->hdr_depth);
    r->ldr_target = a3_rhi_target_create(r->ldr_color, (A3RhiTexture){ 0 });
    if (!r->hdr_target.id || !r->ldr_target.id) { destroy_targets(r); return 0; }
    r->rt_w = w;
    r->rt_h = h;
    return 1;
}

static b32 ensure_shadow(A3Renderer *r) {
    i32 size = A3_CLAMP(r->settings.shadow_map_size, 256, 4096);
    if (r->shadow_target.id && r->shadow_size == size) return 1;
    if (r->shadow_target.id) a3_rhi_target_destroy(r->shadow_target);
    if (r->shadow_tex.id) a3_rhi_texture_destroy(r->shadow_tex);
    A3TextureDesc d;
    a3_zero_struct(&d);
    d.width = d.height = size;
    d.format = A3_TEX_DEPTH24;
    d.shadow_compare = 1;
    d.debug_name = "shadow_map";
    r->shadow_tex = a3_rhi_texture_create(&d);
    r->shadow_target = a3_rhi_target_create((A3RhiTexture){ 0 }, r->shadow_tex);
    r->shadow_size = size;
    return r->shadow_target.id != 0;
}

/* ======================================================================== */
/* Views                                                                    */
/* ======================================================================== */

b32 a3_render_view_from_camera(A3World *w, A3Entity cam, i32 width, i32 height, A3RenderView *out) {
    A3CCamera *c = (A3CCamera *)a3_component_get(w, cam, A3_T_CAMERA);
    if (!c || width <= 0 || height <= 0) return 0;
    a3_zero_struct(out);
    A3Mat4 world = a3_transform_compute_world(w, cam);
    A3Vec3 pos, scale;
    A3Quat rot;
    a3_mat4_decompose(&world, &pos, &rot, &scale);
    A3Mat4 cam_m = a3_mat4_trs(pos, rot, a3_v3_one());
    out->view = a3_mat4_inverse_affine(&cam_m);
    f32 aspect = (f32)width / (f32)height;
    b32 zo = a3_rhi_depth_zero_to_one();
    if (c->projection == A3_PROJ_ORTHOGRAPHIC)
        out->proj = a3_mat4_ortho(-c->ortho_size * aspect, c->ortho_size * aspect, -c->ortho_size, c->ortho_size, c->near_plane, c->far_plane, zo);
    else
        out->proj = a3_mat4_perspective(c->fov * A3_DEG2RAD, aspect, c->near_plane, c->far_plane, zo);
    out->camera_pos = pos;
    out->near_plane = c->near_plane;
    out->far_plane = c->far_plane;
    out->width = width;
    out->height = height;
    out->draw_sky = c->use_sky;
    out->clear_color = c->clear_color;
    out->exposure = c->exposure > 0 ? c->exposure : 1.0f;
    return 1;
}

A3RenderView a3_render_view_look_at(A3Vec3 eye, A3Vec3 target, f32 fov_deg, i32 width, i32 height) {
    A3RenderView v;
    a3_zero_struct(&v);
    v.view = a3_mat4_look_at(eye, target, a3_v3(0, 1, 0));
    v.proj = a3_mat4_perspective(fov_deg * A3_DEG2RAD, (f32)width / (f32)(height ? height : 1), 0.1f, 1000.0f, a3_rhi_depth_zero_to_one());
    v.camera_pos = eye;
    v.near_plane = 0.1f;
    v.far_plane = 1000.0f;
    v.width = width;
    v.height = height;
    v.draw_sky = 1;
    v.clear_color = a3_v4(0.1f, 0.1f, 0.12f, 1);
    v.exposure = 1.0f;
    return v;
}

/* ======================================================================== */
/* Frame                                                                    */
/* ======================================================================== */

typedef struct LightSet {
    b32 has_sun;
    A3Vec3 sun_dir, sun_color;
    b32 sun_shadows;
    u32 count;
    A3Vec4 pos[MAX_LIGHTS], color[MAX_LIGHTS], dir[MAX_LIGHTS];
    f32 dist[MAX_LIGHTS];
} LightSet;

static void gather_lights(A3Renderer *r, A3World *w, A3Vec3 cam, LightSet *ls) {
    a3_zero_struct(ls);
    u32 types[2] = { A3_T_LIGHT, A3_T_TRANSFORM };
    A3Query q = a3_query_begin(w, types, 2);
    u32 max_lights = (u32)A3_CLAMP(r->settings.max_lights, 0, MAX_LIGHTS);
    while (a3_query_next(&q)) {
        const A3CLight *l = (const A3CLight *)q.components[0];
        const A3CTransform *t = (const A3CTransform *)q.components[1];
        A3Vec3 color = a3_v3_scale(a3_color3_to_linear(l->color), l->intensity);
        A3Vec3 pos = a3_mat4_get_translation(&t->world);
        A3Vec3 fwd = a3_v3_norm(a3_mat4_mul_dir(&t->world, a3_v3(0, 0, -1)));
        if (l->type == A3_LIGHT_DIRECTIONAL) {
            if (!ls->has_sun) {
                ls->has_sun = 1;
                ls->sun_dir = fwd;
                ls->sun_color = a3_v3_scale(color, 3.6f); /* physical-ish sun strength vs ambient */
                ls->sun_shadows = l->cast_shadows;
            }
            continue;
        }
        f32 d = a3_v3_dist(pos, cam) - l->range;
        u32 slot = ls->count;
        if (ls->count >= max_lights) {
            /* replace the farthest light if this one is closer */
            u32 far_i = 0;
            for (u32 i = 1; i < ls->count; ++i) if (ls->dist[i] > ls->dist[far_i]) far_i = i;
            if (max_lights == 0 || d >= ls->dist[far_i]) continue;
            slot = far_i;
        } else {
            ls->count++;
        }
        ls->dist[slot] = d;
        ls->pos[slot] = a3_v4(pos.x, pos.y, pos.z, a3_maxf(l->range, 0.01f));
        ls->color[slot] = a3_v4(color.x * 10.0f, color.y * 10.0f, color.z * 10.0f, l->type == A3_LIGHT_SPOT ? 2.0f : 1.0f);
        ls->dir[slot] = a3_v4(fwd.x, fwd.y, fwd.z, a3_cosf(a3_clampf(l->spot_angle, 1, 179) * 0.5f * A3_DEG2RAD));
    }
    if (!ls->has_sun) {
        ls->sun_dir = a3_v3_norm(a3_v3(-0.4f, -1.0f, -0.3f));
        ls->sun_color = a3_v3_zero();
    }
}

static A3Mat4 shadow_matrix(A3Renderer *r, const A3RenderView *v, A3Vec3 sun_dir) {
    f32 dist = a3_maxf(r->settings.shadow_distance, 5.0f);
    A3Mat4 inv_view = a3_mat4_inverse_affine(&v->view);
    A3Vec3 fwd = a3_v3_norm(a3_mat4_mul_dir(&inv_view, a3_v3(0, 0, -1)));
    A3Vec3 center = a3_v3_add(v->camera_pos, a3_v3_scale(fwd, dist * 0.4f));
    f32 radius = dist * 0.6f;
    A3Vec3 up = a3_absf(sun_dir.y) > 0.99f ? a3_v3(0, 0, 1) : a3_v3(0, 1, 0);
    A3Mat4 light_view = a3_mat4_look_at(a3_v3_sub(center, a3_v3_scale(sun_dir, radius * 2.0f)), center, up);
    /* snap to texels to stop shadow edges shimmering when the camera moves */
    f32 texel = (2.0f * radius) / (f32)r->shadow_size;
    A3Vec3 lc = a3_mat4_mul_point(&light_view, center);
    A3Vec3 snapped = a3_v3(a3_floorf(lc.x / texel) * texel, a3_floorf(lc.y / texel) * texel, lc.z);
    A3Mat4 fix = a3_mat4_translation(a3_v3(snapped.x - lc.x, snapped.y - lc.y, 0));
    light_view = a3_mat4_mul(&fix, &light_view);
    A3Mat4 proj = a3_mat4_ortho(-radius, radius, -radius, radius, 0.1f, radius * 4.0f, a3_rhi_depth_zero_to_one());
    return a3_mat4_mul(&proj, &light_view);
}

static int batch_key_bits(u32 v, u32 bits) { return (int)(v & ((1u << bits) - 1)); }

static u64 make_key(const Renderable *rd) {
    /* transparent last; then material, mesh, texture */
    return ((u64)(rd->transparent ? 1 : 0) << 63) | ((u64)batch_key_bits(rd->material, 8) << 48) |
           ((u64)batch_key_bits(rd->mesh, 16) << 24) | (u64)batch_key_bits(rd->texture, 24);
}

static void set_frame_uniforms(A3RhiShader s, const A3RenderView *v, const LightSet *ls, const A3CWorldSettings *ws,
                               const A3Mat4 *view_proj, const A3Mat4 *light_vp, b32 shadows, i32 shadow_size) {
    a3_rhi_set_mat4(s, "u_view_proj", view_proj);
    a3_rhi_set_mat4(s, "u_light_view_proj", light_vp);
    a3_rhi_set_vec3(s, "u_camera_pos", v->camera_pos);
    a3_rhi_set_float(s, "u_time", v->time);
    a3_rhi_set_vec3(s, "u_sun_dir", ls->sun_dir);
    a3_rhi_set_vec3(s, "u_sun_color", ls->sun_color);
    a3_rhi_set_vec3(s, "u_sky_color", a3_v4_xyz(ws->sky_top));
    a3_rhi_set_vec3(s, "u_ground_color", a3_v4_xyz(ws->ground_color));
    a3_rhi_set_float(s, "u_ambient", ws->ambient_intensity);
    a3_rhi_set_vec3(s, "u_fog_color", a3_v4_xyz(ws->fog_color));
    a3_rhi_set_float(s, "u_fog_density", ws->fog_density);
    a3_rhi_set_int(s, "u_shadows_enabled", shadows ? 1 : 0);
    a3_rhi_set_vec2(s, "u_shadow_texel", a3_v2(1.0f / (f32)shadow_size, 1.0f / (f32)shadow_size));
    a3_rhi_set_int(s, "u_light_count", (i32)ls->count);
    if (ls->count) {
        a3_rhi_set_vec4_array(s, "u_light_pos", ls->pos, ls->count);
        a3_rhi_set_vec4_array(s, "u_light_color", ls->color, ls->count);
        a3_rhi_set_vec4_array(s, "u_light_dir", ls->dir, ls->count);
    }
}

void a3_renderer_draw_world(A3Renderer *r, A3World *w, const A3RenderView *v) {
    if (!r || !w || !v) return;
    u64 t0 = a3_time_ns();
    a3_zero_struct(&r->info);
    if (!ensure_targets(r, v->width, v->height)) return;
    A3ArenaMark mark = a3_scratch_begin();
    A3Arena *scratch = a3_scratch();

    a3_transform_system_update(w);
    A3CWorldSettings *ws = a3_world_settings(w);
    A3CWorldSettings ws_copy = *ws;
    ws_copy.sky_top = a3_color_to_linear(ws->sky_top);
    ws_copy.sky_horizon = a3_color_to_linear(ws->sky_horizon);
    ws_copy.ground_color = a3_color_to_linear(ws->ground_color);
    ws_copy.fog_color = a3_color_to_linear(ws->fog_color);
    ws_copy.ambient = a3_color_to_linear(ws->ambient);
    LightSet ls;
    gather_lights(r, w, v->camera_pos, &ls);
    r->info.lights = ls.count + (ls.has_sun ? 1 : 0);

    /* ---- gather renderables ---- */
    u32 n_mr = a3_component_count(w, A3_T_MESH_RENDERER);
    Renderable *items = n_mr ? A3_ARENA_PUSH_ARRAY(scratch, Renderable, n_mr) : 0;
    u32 n = 0;
    u32 types[2] = { A3_T_MESH_RENDERER, A3_T_TRANSFORM };
    A3Query q = a3_query_begin(w, types, 2);
    while (a3_query_next(&q) && items) {
        A3CMeshRenderer *mr = (A3CMeshRenderer *)q.components[0];
        A3CTransform *t = (A3CTransform *)q.components[1];
        if (!mr->visible) continue;
        u32 mesh_id = a3_assets_mesh_for_renderer(mr);
        const A3MeshAsset *mesh = a3_assets_mesh_get(mesh_id);
        if (!mesh || !mesh->gpu.id) continue;
        Renderable *rd = &items[n++];
        rd->mesh = mesh_id; /* a failed asset resolves to the placeholder cube in a3_assets_mesh_get */
        rd->texture = mr->texture.path[0] ? a3_assets_resolve_texture(&mr->texture) : 0;
        u64 mat = 0;
        rd->material = a3_hashmap_get(&r->entity_materials, a3_entity_guid(w, q.entity), &mat) ? (u32)mat : 0;
        if (rd->material >= MAX_MATERIALS || !r->materials[rd->material].used) rd->material = 0;
        rd->cast_shadows = mr->cast_shadows;
        rd->transparent = mr->base_color.w < 0.999f;
        rd->inst.model = t->world;
        rd->inst.color = a3_color_to_linear(mr->base_color);
        rd->inst.params = a3_v4(mr->metallic, mr->roughness, mr->emissive, mr->receive_shadows ? 1.0f : 0.0f);
        A3Vec3 c = a3_mat4_mul_point(&t->world, a3_aabb_center(mesh->bounds));
        f32 sx = a3_v3_len(a3_v3(t->world.m[0], t->world.m[1], t->world.m[2]));
        f32 sy = a3_v3_len(a3_v3(t->world.m[4], t->world.m[5], t->world.m[6]));
        f32 sz = a3_v3_len(a3_v3(t->world.m[8], t->world.m[9], t->world.m[10]));
        rd->sphere = a3_v4(c.x, c.y, c.z, mesh->radius * a3_maxf(sx, a3_maxf(sy, sz)));
    }
    r->info.renderables = n;

    /* ---- frustum culling (SIMD / assembly kernel) ---- */
    A3Mat4 view_proj = a3_mat4_mul(&v->proj, &v->view);
    u8 *visible = n ? (u8 *)a3_arena_push(scratch, n, 16) : 0;
    A3Vec4 *spheres = n ? A3_ARENA_PUSH_ARRAY(scratch, A3Vec4, n) : 0;
    for (u32 i = 0; i < n; ++i) spheres[i] = items[i].sphere;
    u32 n_visible = n;
    if (n && r->settings.frustum_culling) {
        A3Frustum fr = a3_frustum_from_matrix(&view_proj, a3_rhi_depth_zero_to_one());
        n_visible = a3_cull_spheres(&fr, spheres, visible, n);
    } else if (n) {
        a3_memset(visible, 1, n);
    }
    r->info.visible = n_visible;
    r->info.culled = n - n_visible;

    /* ---- sort + batch visible items; also collect shadow casters ---- */
    A3SortPair *pairs = n ? A3_ARENA_PUSH_ARRAY(scratch, A3SortPair, n * 2) : 0;
    u32 np = 0;
    for (u32 i = 0; i < n; ++i) {
        if (!visible[i]) continue;
        u64 key = make_key(&items[i]);
        if (items[i].transparent) {
            /* back to front: larger distance first */
            f32 d = a3_v3_dist(a3_v3(items[i].sphere.x, items[i].sphere.y, items[i].sphere.z), v->camera_pos);
            u32 di = 0xFFFFFFu - (u32)a3_clampf(d * 64.0f, 0, 16777215.0f);
            key = (1ull << 63) | (u64)di << 24 | (u64)batch_key_bits(items[i].mesh, 24);
        }
        pairs[np].key = key;
        pairs[np].value = i;
        np++;
    }
    a3_radix_sort_pairs(pairs, pairs + n, np);

    b32 shadows = r->settings.shadows && ls.has_sun && ls.sun_shadows && ensure_shadow(r);
    A3Vec3 shadow_center = v->camera_pos;
    f32 shadow_radius = a3_maxf(r->settings.shadow_distance, 5.0f) * 1.2f;
    u32 n_casters = 0;
    u32 *casters = 0;
    if (shadows && n) {
        casters = A3_ARENA_PUSH_ARRAY(scratch, u32, n);
        A3SortPair *cp = A3_ARENA_PUSH_ARRAY(scratch, A3SortPair, n * 2);
        for (u32 i = 0; i < n; ++i) {
            if (!items[i].cast_shadows) continue;
            f32 d = a3_v3_dist(a3_v3(items[i].sphere.x, items[i].sphere.y, items[i].sphere.z), shadow_center);
            if (d - items[i].sphere.w > shadow_radius) continue;
            cp[n_casters].key = items[i].mesh;
            cp[n_casters].value = i;
            n_casters++;
        }
        a3_radix_sort_pairs(cp, cp + n, n_casters);
        for (u32 i = 0; i < n_casters; ++i) casters[i] = cp[i].value;
    }
    r->info.shadow_casters = n_casters;

    /* instance buffer: [visible sorted][shadow casters] */
    u32 total_inst = np + n_casters;
    InstanceData *inst = total_inst ? A3_ARENA_PUSH_ARRAY(scratch, InstanceData, total_inst) : 0;
    Batch *batches = np ? A3_ARENA_PUSH_ARRAY(scratch, Batch, np) : 0;
    u32 nb = 0;
    for (u32 i = 0; i < np; ++i) {
        const Renderable *rd = &items[pairs[i].value];
        inst[i] = rd->inst;
        b32 same = nb > 0 && !rd->transparent && !batches[nb - 1].transparent && batches[nb - 1].mesh == rd->mesh &&
                   batches[nb - 1].texture == rd->texture && batches[nb - 1].material == rd->material;
        if (same) batches[nb - 1].count++;
        else batches[nb++] = (Batch){ rd->mesh, rd->texture, rd->material, i, 1, rd->transparent };
    }
    for (u32 i = 0; i < n_casters; ++i) inst[np + i] = items[casters[i]].inst;
    if (total_inst) a3_rhi_buffer_upload(r->instance_buf, sizeof(InstanceData) * total_inst, inst);
    r->info.batches = nb;

    /* ---- shadow pass ---- */
    A3Mat4 light_vp = a3_mat4_identity();
    if (shadows) {
        light_vp = shadow_matrix(r, v, ls.sun_dir);
        a3_rhi_target_bind(r->shadow_target, r->shadow_size, r->shadow_size);
        a3_rhi_clear(0, a3_v4(0, 0, 0, 0), 1, 1.0f);
        A3RenderState st = { A3_BLEND_OPAQUE, A3_CULL_NONE, A3_DEPTH_LESS_WRITE, 0, 0, { 0 }, 2.0f };
        a3_rhi_set_state(&st);
        a3_rhi_shader_bind(r->shadow);
        a3_rhi_set_mat4(r->shadow, "u_view_proj", &light_vp);
        u32 i = 0;
        while (i < n_casters) {
            u32 mesh = items[casters[i]].mesh, j = i + 1;
            while (j < n_casters && items[casters[j]].mesh == mesh) ++j;
            const A3MeshAsset *m = a3_assets_mesh_get(mesh);
            if (m && m->gpu.id) {
                a3_rhi_mesh_set_instances(m->gpu, r->instance_buf, &r->instance_layout, sizeof(InstanceData) * (np + i));
                a3_rhi_draw(m->gpu, A3_PRIM_TRIANGLES, 0, m->index_count, j - i);
            }
            i = j;
        }
    }

    /* ---- main pass (HDR) ---- */
    a3_rhi_target_bind(r->hdr_target, r->rt_w, r->rt_h);
    a3_rhi_gpu_timer_begin();
    A3Vec4 clear = a3_color_to_linear(v->clear_color);
    a3_rhi_clear(1, a3_v4(clear.x, clear.y, clear.z, 1), 1, 1.0f);
    if (v->draw_sky && r->sky.id) {
        A3RenderState st = { A3_BLEND_OPAQUE, A3_CULL_NONE, A3_DEPTH_OFF, 0, 0, { 0 }, 0 };
        a3_rhi_set_state(&st);
        a3_rhi_shader_bind(r->sky);
        A3Mat4 inv_vp;
        if (!a3_mat4_inverse(&view_proj, &inv_vp)) inv_vp = a3_mat4_identity();
        a3_rhi_set_mat4(r->sky, "u_inv_view_proj", &inv_vp);
        a3_rhi_set_vec3(r->sky, "u_camera_pos", v->camera_pos);
        a3_rhi_set_vec3(r->sky, "u_sky_top", a3_v4_xyz(ws_copy.sky_top));
        a3_rhi_set_vec3(r->sky, "u_sky_horizon", a3_v4_xyz(ws_copy.sky_horizon));
        a3_rhi_set_vec3(r->sky, "u_ground_color", a3_v4_xyz(ws_copy.ground_color));
        a3_rhi_set_vec3(r->sky, "u_sun_dir", ls.sun_dir);
        a3_rhi_set_vec3(r->sky, "u_sun_color", ls.has_sun ? ls.sun_color : a3_v3_zero());
        a3_rhi_draw_fullscreen();
    }
    a3_rhi_bind_texture(1, r->shadow_tex);
    u32 bound_material = 0xFFFFFFFFu;
    A3RhiShader shader = r->lit;
    for (u32 b = 0; b < nb; ++b) {
        Batch *bt = &batches[b];
        if (bt->material != bound_material) {
            bound_material = bt->material;
            Material *mat = bt->material ? &r->materials[bt->material] : 0;
            shader = (mat && mat->shader.id) ? mat->shader : r->lit;
            a3_rhi_shader_bind(shader);
            set_frame_uniforms(shader, v, &ls, &ws_copy, &view_proj, &light_vp, shadows, r->shadow_size);
            if (mat) {
                a3_rhi_set_vec4_array(shader, "u_material", mat->params, 4);
                a3_rhi_bind_texture(2, mat->textures[0] ? a3_assets_texture_rhi(mat->textures[0]) : a3_assets_white_texture());
                a3_rhi_bind_texture(3, mat->textures[1] ? a3_assets_texture_rhi(mat->textures[1]) : a3_assets_white_texture());
            }
        }
        A3RenderState st = { bt->transparent ? A3_BLEND_ALPHA : A3_BLEND_OPAQUE, bt->transparent ? A3_CULL_NONE : A3_CULL_BACK,
                             bt->transparent ? A3_DEPTH_LESS_NOWRITE : A3_DEPTH_LESS_WRITE, r->settings.wireframe, 0, { 0 }, 0 };
        a3_rhi_set_state(&st);
        a3_rhi_set_int(shader, "u_has_texture", bt->texture ? 1 : 0);
        a3_rhi_bind_texture(0, bt->texture ? a3_assets_texture_rhi(bt->texture) : a3_assets_white_texture());
        const A3MeshAsset *m = a3_assets_mesh_get(bt->mesh);
        if (!m || !m->gpu.id) continue;
        a3_rhi_mesh_set_instances(m->gpu, r->instance_buf, &r->instance_layout, sizeof(InstanceData) * bt->first);
        a3_rhi_draw(m->gpu, A3_PRIM_TRIANGLES, 0, m->index_count, bt->count);
    }

    /* ---- editor grid + debug lines ---- */
    if (v->draw_grid && r->grid.id) {
        A3RenderState st = { A3_BLEND_ALPHA, A3_CULL_NONE, A3_DEPTH_LESS_NOWRITE, 0, 0, { 0 }, 0 };
        a3_rhi_set_state(&st);
        a3_rhi_shader_bind(r->grid);
        a3_rhi_set_mat4(r->grid, "u_view_proj", &view_proj);
        a3_rhi_set_vec3(r->grid, "u_camera_pos", v->camera_pos);
        a3_rhi_draw(r->grid_mesh, A3_PRIM_TRIANGLES, 0, 6, 1);
    }
    if (r->debug_verts.count) {
        if (v->draw_debug) {
            A3RenderState st = { A3_BLEND_ALPHA, A3_CULL_NONE, r->debug_depth ? A3_DEPTH_LEQUAL_NOWRITE : A3_DEPTH_OFF, 0, 0, { 0 }, 0 };
            a3_rhi_set_state(&st);
            a3_rhi_shader_bind(r->lines);
            a3_rhi_set_mat4(r->lines, "u_view_proj", &view_proj);
            a3_rhi_buffer_upload(r->debug_buf, sizeof(DebugVertex) * r->debug_verts.count, r->debug_verts.data);
            a3_rhi_draw_arrays(r->debug_mesh, A3_PRIM_LINES, 0, r->debug_verts.count, 1);
        }
        a3_array_clear(r->debug_verts);
    }
    a3_rhi_gpu_timer_end();

    /* ---- post: tonemap (+FXAA) ---- */
    A3RenderState post = { A3_BLEND_OPAQUE, A3_CULL_NONE, A3_DEPTH_OFF, 0, 0, { 0 }, 0 };
    if (r->settings.fxaa && r->fxaa.id) {
        a3_rhi_target_bind(r->ldr_target, r->rt_w, r->rt_h);
        a3_rhi_set_state(&post);
        a3_rhi_shader_bind(r->tonemap);
        a3_rhi_set_float(r->tonemap, "u_exposure", v->exposure > 0 ? v->exposure : 1.0f);
        a3_rhi_set_float(r->tonemap, "u_vignette", r->settings.vignette);
        a3_rhi_bind_texture(0, r->hdr_color);
        a3_rhi_draw_fullscreen();
        a3_rhi_target_bind(v->target, v->width, v->height);
        a3_rhi_set_state(&post);
        a3_rhi_shader_bind(r->fxaa);
        a3_rhi_set_vec2(r->fxaa, "u_texel", a3_v2(1.0f / r->rt_w, 1.0f / r->rt_h));
        a3_rhi_bind_texture(0, r->ldr_color);
        a3_rhi_draw_fullscreen();
    } else {
        a3_rhi_target_bind(v->target, v->width, v->height);
        a3_rhi_set_state(&post);
        a3_rhi_shader_bind(r->tonemap);
        a3_rhi_set_float(r->tonemap, "u_exposure", v->exposure > 0 ? v->exposure : 1.0f);
        a3_rhi_set_float(r->tonemap, "u_vignette", r->settings.vignette);
        a3_rhi_bind_texture(0, r->hdr_color);
        a3_rhi_draw_fullscreen();
    }
    r->last_target = v->target;
    a3_scratch_end(mark);
    r->info.cpu_ms = (f64)(a3_time_ns() - t0) / 1e6;
}

b32 a3_renderer_read_view(A3Renderer *r, i32 width, i32 height, u8 *out) {
    if (!r || !out) return 0;
    a3_rhi_target_bind(r->last_target, width, height);
    a3_rhi_read_pixels(0, 0, width, height, out);
    return 1;
}

/* ======================================================================== */
/* Debug drawing                                                            */
/* ======================================================================== */

static u32 pack_color(A3Vec4 c) {
    u32 r = (u32)(a3_saturate(c.x) * 255.0f + 0.5f), g = (u32)(a3_saturate(c.y) * 255.0f + 0.5f);
    u32 b = (u32)(a3_saturate(c.z) * 255.0f + 0.5f), a = (u32)(a3_saturate(c.w) * 255.0f + 0.5f);
    return r | (g << 8) | (b << 16) | (a << 24);
}

void a3_debug_line(A3Renderer *r, A3Vec3 a, A3Vec3 b, A3Vec4 color) {
    if (!r || r->debug_verts.count > 1000000) return;
    u32 c = pack_color(color);
    DebugVertex va = { a, c }, vb = { b, c };
    a3_array_push(r->debug_verts, va, A3_MEM_RENDER);
    a3_array_push(r->debug_verts, vb, A3_MEM_RENDER);
}

void a3_debug_set_depth_test(A3Renderer *r, b32 on) { if (r) r->debug_depth = on; }

void a3_debug_obb(A3Renderer *r, const A3Mat4 *m, A3Aabb b, A3Vec4 color) {
    A3Vec3 c[8];
    for (int i = 0; i < 8; ++i) {
        A3Vec3 p = a3_v3((i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y, (i & 4) ? b.max.z : b.min.z);
        c[i] = m ? a3_mat4_mul_point(m, p) : p;
    }
    static const int E[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
    for (int e = 0; e < 12; ++e) a3_debug_line(r, c[E[e][0]], c[E[e][1]], color);
}

void a3_debug_aabb(A3Renderer *r, A3Aabb box, A3Vec4 color) { a3_debug_obb(r, 0, box, color); }

void a3_debug_circle(A3Renderer *r, A3Vec3 c, A3Vec3 n, f32 radius, A3Vec4 color) {
    n = a3_v3_norm(n);
    A3Vec3 t = a3_absf(n.y) < 0.99f ? a3_v3_norm(a3_v3_cross(n, a3_v3(0, 1, 0))) : a3_v3(1, 0, 0);
    A3Vec3 bt = a3_v3_cross(n, t);
    const int SEG = 32;
    A3Vec3 prev = a3_v3_add(c, a3_v3_scale(t, radius));
    for (int i = 1; i <= SEG; ++i) {
        f32 s, co;
        a3_sincosf(A3_TAU * (f32)i / SEG, &s, &co);
        A3Vec3 p = a3_v3_add(c, a3_v3_add(a3_v3_scale(t, co * radius), a3_v3_scale(bt, s * radius)));
        a3_debug_line(r, prev, p, color);
        prev = p;
    }
}

void a3_debug_sphere(A3Renderer *r, A3Vec3 c, f32 radius, A3Vec4 color) {
    a3_debug_circle(r, c, a3_v3(1, 0, 0), radius, color);
    a3_debug_circle(r, c, a3_v3(0, 1, 0), radius, color);
    a3_debug_circle(r, c, a3_v3(0, 0, 1), radius, color);
}

void a3_debug_arrow(A3Renderer *r, A3Vec3 from, A3Vec3 to, A3Vec4 color) {
    a3_debug_line(r, from, to, color);
    A3Vec3 d = a3_v3_sub(to, from);
    f32 len = a3_v3_len(d);
    if (len < 1e-4f) return;
    d = a3_v3_scale(d, 1.0f / len);
    A3Vec3 side = a3_absf(d.y) < 0.99f ? a3_v3_norm(a3_v3_cross(d, a3_v3(0, 1, 0))) : a3_v3(1, 0, 0);
    A3Vec3 back = a3_v3_sub(to, a3_v3_scale(d, len * 0.2f));
    a3_debug_line(r, to, a3_v3_add(back, a3_v3_scale(side, len * 0.08f)), color);
    a3_debug_line(r, to, a3_v3_sub(back, a3_v3_scale(side, len * 0.08f)), color);
}

/* ======================================================================== */
/* Materials                                                                */
/* ======================================================================== */

u32 a3_renderer_material_create(A3Renderer *r, const char *code, const char *name, A3ShaderCompileResult *res) {
    if (!r || !code) return 0;
    u32 slot = 0;
    for (u32 i = 1; i < MAX_MATERIALS; ++i) if (!r->materials[i].used) { slot = i; break; }
    if (!slot) { A3_ERROR("render", "too many custom materials (max %d)", MAX_MATERIALS - 1); return 0; }
    A3RhiShader s = make_lit_shader(code, name ? name : "material", res);
    if (!s.id) return 0;
    Material *m = &r->materials[slot];
    a3_zero_struct(m);
    m->used = 1;
    m->shader = s;
    a3_strcpy(m->name, sizeof(m->name), name ? name : "material");
    for (int i = 0; i < 4; ++i) m->params[i] = a3_v4(1, 1, 1, 1);
    return slot;
}

b32 a3_renderer_material_update(A3Renderer *r, u32 id, const char *code, A3ShaderCompileResult *res) {
    if (!r || id == 0 || id >= MAX_MATERIALS || !r->materials[id].used) return 0;
    A3RhiShader s = make_lit_shader(code, r->materials[id].name, res);
    if (!s.id) return 0; /* keep the previous working shader */
    a3_rhi_shader_destroy(r->materials[id].shader);
    r->materials[id].shader = s;
    return 1;
}

void a3_renderer_material_destroy(A3Renderer *r, u32 id) {
    if (!r || id == 0 || id >= MAX_MATERIALS || !r->materials[id].used) return;
    a3_rhi_shader_destroy(r->materials[id].shader);
    a3_zero_struct(&r->materials[id]);
}

void a3_renderer_material_set_params(A3Renderer *r, u32 id, const A3Vec4 params[4]) {
    if (!r || id == 0 || id >= MAX_MATERIALS || !r->materials[id].used || !params) return;
    for (int i = 0; i < 4; ++i) r->materials[id].params[i] = params[i];
}

void a3_renderer_material_set_texture(A3Renderer *r, u32 id, u32 slot, u32 tex) {
    if (!r || id == 0 || id >= MAX_MATERIALS || !r->materials[id].used || slot > 1) return;
    r->materials[id].textures[slot] = tex;
}

void a3_renderer_set_entity_material(A3Renderer *r, A3World *w, A3Entity e, u32 material) {
    u64 guid = a3_entity_guid(w, e);
    if (!r || !guid) return;
    if (material) a3_hashmap_put(&r->entity_materials, guid, material);
    else a3_hashmap_remove(&r->entity_materials, guid);
}
