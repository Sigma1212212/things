/*
 * ASM3D - a3_renderer.c
 */
#include "a3_renderer.h"
#include "../world/a3_weather.h"
#include "a3_shaders.h"
#include "a3_mesh.h"
#include "../core/a3_log.h"
#include "../core/a3_memory.h"
#include "../core/a3_string.h"
#include "../core/a3_format.h"
#include "../core/a3_sort.h"
#include "../core/a3_strbuf.h"
#include "../core/a3_hash.h"
#include "../core/a3_json.h"
#include "../resource/a3_assets.h"
#include "../particles/a3_particles.h"
#include "../scene/a3_components.h"
#include "../platform/a3_platform.h"

#define MAX_MATERIALS 256
#define MAX_OBJECT_LIGHTS 8      /* lights per object (instance data) */
#define LIGHT_CAP 1024           /* lights per frame (light texture: 256 per row, 3 texels each) */
#define BLOOM_MIPS 5

typedef struct InstanceData {
    A3Mat4 model;
    A3Vec4 color;
    A3Vec4 params;  /* metallic, roughness, emissive, receive_shadows */
    A3Vec4 lights0, lights1; /* indices into the light texture, -1 = none */
} InstanceData;

typedef struct DebugVertex { A3Vec3 pos; u32 color; } DebugVertex;

typedef struct Material {
    b32 used;
    A3RhiShader shader;
    A3Vec4 params[4];
    u32 textures[2];
    char name[64];
} Material;

typedef struct TargetSet {
    i32 w, h;
    u64 last_used;
    A3RhiTexture hdr_color, hdr_depth, ldr_color, normal, post, ao, ao_blur;
    A3RhiTarget hdr_target, ldr_target, post_target, ao_target, ao_blur_target;
    A3RhiTexture bloom[BLOOM_MIPS];
    A3RhiTarget bloom_target[BLOOM_MIPS];
    i32 bloom_w[BLOOM_MIPS], bloom_h[BLOOM_MIPS];
} TargetSet;

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
    A3RhiShader lit, shadow, sky, tonemap, fxaa, lines, grid, particle;
    A3RhiShader ssao, blur4, composite, bloom_down, bloom_up, lum, avg4, adapt;
    /* auto exposure: 64 -> 16 -> 4 -> 1 log-luminance chain, 2 adaptation texels */
    A3RhiTexture lum_tex[4], adapt_tex[2];
    A3RhiTarget lum_target[4], adapt_target[2];
    u32 adapt_cur;
    b32 adapt_valid;
    u64 adapt_clock;
    /* lights of the frame (texture) */
    A3RhiTexture light_tex;
    A3Vec4 light_texels[LIGHT_CAP * 3];
    /* particles */
    A3RhiBuffer particle_quad_vb, particle_quad_ib, particle_buf;
    usize particle_buf_size;
    A3RhiMesh particle_mesh;
    A3VertexLayout particle_layout;
    A3RhiBuffer instance_buf;
    A3VertexLayout instance_layout;
    /* HDR chain */
    i32 rt_w, rt_h;
    A3RhiTexture hdr_color, hdr_depth, ldr_color;
    A3RhiTarget hdr_target, ldr_target;   /* current set (one of target_sets) */
    TargetSet *cur;
    TargetSet target_sets[3];
    u64 target_clock;
    /* material assets by path (.a3shader files) */
    A3HashMap path_materials;             /* hash(path) -> material id, 0xFFFFFFFF = failed */
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
        a3_rhi_set_int(s, "u_light_tex", 4);
    }
    return s;
}

A3Renderer *a3_renderer_create(void) {
    A3Renderer *r = A3_NEW(A3Renderer, A3_MEM_RENDER);
    if (!r) return 0;
    r->settings.shadows = 1;
    r->settings.shadow_map_size = 4096;
    r->settings.shadow_distance = 160.0f;
    r->settings.fxaa = 1;
    r->settings.vignette = 0.25f;
    r->settings.frustum_culling = 1;
    r->settings.max_lights = MAX_OBJECT_LIGHTS;
    r->settings.ssao = 1;
    r->settings.ssr = 1;
    r->settings.bloom = 1;
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
    r->particle = make_shader(A3_SHADER_PARTICLE_VS, A3_SHADER_PARTICLE_FS, "particle");
    r->ssao = make_shader(A3_SHADER_FULLSCREEN_VS, A3_SHADER_SSAO_FS, "ssao");
    r->blur4 = make_shader(A3_SHADER_FULLSCREEN_VS, A3_SHADER_BLUR4_FS, "blur4");
    r->composite = make_shader(A3_SHADER_FULLSCREEN_VS, A3_SHADER_COMPOSITE_FS, "composite");
    r->lum = make_shader(A3_SHADER_FULLSCREEN_VS, A3_SHADER_LUM_FS, "luminance");
    r->avg4 = make_shader(A3_SHADER_FULLSCREEN_VS, A3_SHADER_AVG4_FS, "average4");
    r->adapt = make_shader(A3_SHADER_FULLSCREEN_VS, A3_SHADER_ADAPT_FS, "adapt");
    r->bloom_down = make_shader(A3_SHADER_FULLSCREEN_VS, A3_SHADER_BLOOM_DOWN_FS, "bloom_down");
    r->bloom_up = make_shader(A3_SHADER_FULLSCREEN_VS, A3_SHADER_BLOOM_UP_FS, "bloom_up");
    {
        A3TextureDesc ld;
        a3_zero_struct(&ld);
        ld.width = 256 * 3;
        ld.height = LIGHT_CAP / 256;
        ld.format = A3_TEX_RGBA32F;
        ld.filter = A3_FILTER_NEAREST;
        ld.wrap = A3_WRAP_CLAMP;
        ld.debug_name = "lights";
        r->light_tex = a3_rhi_texture_create(&ld);
    }
    r->instance_buf = a3_rhi_buffer_create(A3_BUFFER_VERTEX, sizeof(InstanceData) * 1024, 0, 1);
    A3VertexLayout *il = &r->instance_layout;
    il->stride = sizeof(InstanceData);
    for (u32 i = 0; i < 4; ++i) il->attribs[i] = (A3VertexAttrib){ 4 + i, 4, A3_ATTR_FLOAT, i * 16, 1 };
    il->attribs[4] = (A3VertexAttrib){ 8, 4, A3_ATTR_FLOAT, (u32)A3_OFFSETOF(InstanceData, color), 1 };
    il->attribs[5] = (A3VertexAttrib){ 9, 4, A3_ATTR_FLOAT, (u32)A3_OFFSETOF(InstanceData, params), 1 };
    il->attribs[6] = (A3VertexAttrib){ 10, 4, A3_ATTR_FLOAT, (u32)A3_OFFSETOF(InstanceData, lights0), 1 };
    il->attribs[7] = (A3VertexAttrib){ 11, 4, A3_ATTR_FLOAT, (u32)A3_OFFSETOF(InstanceData, lights1), 1 };
    il->count = 8;
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
    /* particle billboard quad: position (loc 0) + uv (loc 2); instances: pos+size (loc 4), color (loc 8) */
    f32 qv[] = { -0.5f, -0.5f, 0, 0, 0, 0.5f, -0.5f, 0, 1, 0, 0.5f, 0.5f, 0, 1, 1, -0.5f, 0.5f, 0, 0, 1 };
    u32 qi[] = { 0, 1, 2, 0, 2, 3 };
    r->particle_quad_vb = a3_rhi_buffer_create(A3_BUFFER_VERTEX, sizeof(qv), qv, 0);
    r->particle_quad_ib = a3_rhi_buffer_create(A3_BUFFER_INDEX, sizeof(qi), qi, 0);
    A3VertexLayout ql;
    a3_zero_struct(&ql);
    ql.stride = 20;
    ql.attribs[0] = (A3VertexAttrib){ 0, 3, A3_ATTR_FLOAT, 0, 0 };
    ql.attribs[1] = (A3VertexAttrib){ 2, 2, A3_ATTR_FLOAT, 12, 0 };
    ql.count = 2;
    r->particle_mesh = a3_rhi_mesh_create(r->particle_quad_vb, &ql, r->particle_quad_ib, 1);
    a3_zero_struct(&r->particle_layout);
    r->particle_layout.stride = sizeof(A3ParticleInstance);
    r->particle_layout.attribs[0] = (A3VertexAttrib){ 4, 4, A3_ATTR_FLOAT, 0, 1 };
    r->particle_layout.attribs[1] = (A3VertexAttrib){ 8, 4, A3_ATTR_FLOAT, 16, 1 };
    r->particle_layout.count = 2;
    a3_hashmap_init(&r->entity_materials, 64, A3_MEM_RENDER);
    a3_hashmap_init(&r->path_materials, 32, A3_MEM_RENDER);
    a3_rhi_set_int(r->tonemap, "u_hdr", 0);
    a3_rhi_set_int(r->fxaa, "u_ldr", 0);
    return r;
}

static u32 material_from_path(A3Renderer *r, const char *path);

static void destroy_target_set(TargetSet *t) {
    A3RhiTarget targets[] = { t->hdr_target, t->ldr_target, t->post_target, t->ao_target, t->ao_blur_target };
    for (u32 i = 0; i < A3_ARRAY_COUNT(targets); ++i) if (targets[i].id) a3_rhi_target_destroy(targets[i]);
    A3RhiTexture texs[] = { t->hdr_color, t->hdr_depth, t->ldr_color, t->normal, t->post, t->ao, t->ao_blur };
    for (u32 i = 0; i < A3_ARRAY_COUNT(texs); ++i) if (texs[i].id) a3_rhi_texture_destroy(texs[i]);
    for (u32 i = 0; i < BLOOM_MIPS; ++i) {
        if (t->bloom_target[i].id) a3_rhi_target_destroy(t->bloom_target[i]);
        if (t->bloom[i].id) a3_rhi_texture_destroy(t->bloom[i]);
    }
    a3_zero_struct(t);
}

static void destroy_targets(A3Renderer *r) {
    for (u32 i = 0; i < A3_ARRAY_COUNT(r->target_sets); ++i) destroy_target_set(&r->target_sets[i]);
    r->hdr_target.id = r->ldr_target.id = r->hdr_color.id = r->hdr_depth.id = r->ldr_color.id = 0;
    r->rt_w = r->rt_h = 0;
}

void a3_renderer_destroy(A3Renderer *r) {
    if (!r) return;
    destroy_targets(r);
    if (r->shadow_target.id) a3_rhi_target_destroy(r->shadow_target);
    if (r->shadow_tex.id) a3_rhi_texture_destroy(r->shadow_tex);
    A3RhiShader shaders[] = { r->lit, r->shadow, r->sky, r->tonemap, r->fxaa, r->lines, r->grid, r->particle,
                              r->ssao, r->blur4, r->composite, r->bloom_down, r->bloom_up, r->lum, r->avg4, r->adapt };
    for (u32 i = 0; i < 4; ++i) { if (r->lum_target[i].id) a3_rhi_target_destroy(r->lum_target[i]); if (r->lum_tex[i].id) a3_rhi_texture_destroy(r->lum_tex[i]); }
    for (u32 i = 0; i < 2; ++i) { if (r->adapt_target[i].id) a3_rhi_target_destroy(r->adapt_target[i]); if (r->adapt_tex[i].id) a3_rhi_texture_destroy(r->adapt_tex[i]); }
    if (r->light_tex.id) a3_rhi_texture_destroy(r->light_tex);
    for (u32 i = 0; i < A3_ARRAY_COUNT(shaders); ++i) a3_rhi_shader_destroy(shaders[i]);
    for (u32 i = 0; i < MAX_MATERIALS; ++i) if (r->materials[i].used) a3_rhi_shader_destroy(r->materials[i].shader);
    a3_rhi_mesh_destroy(r->debug_mesh);
    a3_rhi_mesh_destroy(r->grid_mesh);
    a3_rhi_mesh_destroy(r->particle_mesh);
    a3_rhi_buffer_destroy(r->particle_quad_vb);
    a3_rhi_buffer_destroy(r->particle_quad_ib);
    if (r->particle_buf.id) a3_rhi_buffer_destroy(r->particle_buf);
    a3_rhi_buffer_destroy(r->debug_buf);
    a3_rhi_buffer_destroy(r->grid_vb);
    a3_rhi_buffer_destroy(r->grid_ib);
    a3_rhi_buffer_destroy(r->instance_buf);
    a3_array_free(r->debug_verts);
    a3_hashmap_free(&r->entity_materials);
    a3_hashmap_free(&r->path_materials);
    a3_free(r);
}

A3RenderSettings *a3_renderer_settings(A3Renderer *r) { return r ? &r->settings : 0; }
const A3RenderFrameInfo *a3_renderer_frame_info(A3Renderer *r) { return r ? &r->info : 0; }

/* A few HDR target sets are cached by size so views of different sizes
 * (editor viewport, Shader Maker preview, thumbnails) do not reallocate
 * render targets every frame. */
static b32 ensure_targets(A3Renderer *r, i32 w, i32 h) {
    if (w <= 0 || h <= 0) return 0;
    TargetSet *use = 0;
    for (u32 i = 0; i < A3_ARRAY_COUNT(r->target_sets); ++i)
        if (r->target_sets[i].w == w && r->target_sets[i].h == h && r->target_sets[i].hdr_target.id) { use = &r->target_sets[i]; break; }
    if (!use) {
        use = &r->target_sets[0];
        for (u32 i = 1; i < A3_ARRAY_COUNT(r->target_sets); ++i)
            if (r->target_sets[i].last_used < use->last_used) use = &r->target_sets[i];
        destroy_target_set(use);
        A3TextureDesc d;
        a3_zero_struct(&d);
        d.width = w; d.height = h; d.wrap = A3_WRAP_CLAMP; d.filter = A3_FILTER_LINEAR;
        d.format = A3_TEX_RGBA16F; d.debug_name = "hdr_color";
        use->hdr_color = a3_rhi_texture_create(&d);
        d.format = A3_TEX_DEPTH24; d.debug_name = "hdr_depth";
        use->hdr_depth = a3_rhi_texture_create(&d);
        d.format = A3_TEX_RGBA8; d.debug_name = "ldr_color";
        use->ldr_color = a3_rhi_texture_create(&d);
        d.format = A3_TEX_RGBA16F; d.debug_name = "normal_roughness";
        use->normal = a3_rhi_texture_create(&d);
        d.debug_name = "post";
        use->post = a3_rhi_texture_create(&d);
        d.format = A3_TEX_R8; d.debug_name = "ao";
        use->ao = a3_rhi_texture_create(&d);
        d.debug_name = "ao_blur";
        use->ao_blur = a3_rhi_texture_create(&d);
        A3RhiTexture mrt[2] = { use->hdr_color, use->normal };
        use->hdr_target = a3_rhi_target_create_mrt(mrt, 2, use->hdr_depth);
        use->ldr_target = a3_rhi_target_create(use->ldr_color, (A3RhiTexture){ 0 });
        use->post_target = a3_rhi_target_create(use->post, (A3RhiTexture){ 0 });
        use->ao_target = a3_rhi_target_create(use->ao, (A3RhiTexture){ 0 });
        use->ao_blur_target = a3_rhi_target_create(use->ao_blur, (A3RhiTexture){ 0 });
        i32 bw = w, bh = h;
        for (u32 i = 0; i < BLOOM_MIPS; ++i) {
            bw = a3_maxi(bw / 2, 1);
            bh = a3_maxi(bh / 2, 1);
            A3TextureDesc bd = d;
            bd.width = bw; bd.height = bh; bd.format = A3_TEX_RGBA16F; bd.debug_name = "bloom";
            use->bloom[i] = a3_rhi_texture_create(&bd);
            use->bloom_target[i] = a3_rhi_target_create(use->bloom[i], (A3RhiTexture){ 0 });
            use->bloom_w[i] = bw;
            use->bloom_h[i] = bh;
        }
        if (!use->hdr_target.id || !use->ldr_target.id || !use->post_target.id) { destroy_target_set(use); return 0; }
        use->w = w;
        use->h = h;
    }
    use->last_used = ++r->target_clock;
    r->hdr_color = use->hdr_color;
    r->hdr_depth = use->hdr_depth;
    r->ldr_color = use->ldr_color;
    r->hdr_target = use->hdr_target;
    r->ldr_target = use->ldr_target;
    r->cur = use;
    r->rt_w = w;
    r->rt_h = h;
    return 1;
}

static b32 ensure_exposure(A3Renderer *r) {
    if (r->adapt_target[1].id) return 1;
    if (!r->lum.id || !r->avg4.id || !r->adapt.id) return 0;
    static const i32 sizes[4] = { 64, 16, 4, 1 };
    A3TextureDesc d;
    a3_zero_struct(&d);
    d.format = A3_TEX_R16F;
    d.filter = A3_FILTER_NEAREST;
    d.wrap = A3_WRAP_CLAMP;
    d.debug_name = "luminance";
    for (u32 i = 0; i < 4; ++i) {
        d.width = d.height = sizes[i];
        r->lum_tex[i] = a3_rhi_texture_create(&d);
        r->lum_target[i] = a3_rhi_target_create(r->lum_tex[i], (A3RhiTexture){ 0 });
    }
    d.width = d.height = 1;
    d.debug_name = "adaptation";
    for (u32 i = 0; i < 2; ++i) {
        r->adapt_tex[i] = a3_rhi_texture_create(&d);
        r->adapt_target[i] = a3_rhi_target_create(r->adapt_tex[i], (A3RhiTexture){ 0 });
    }
    r->adapt_valid = 0;
    return r->adapt_target[1].id != 0;
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
    u32 count;                 /* local lights in the light texture */
    A3Vec4 *pos;               /* xyz, range (points into scratch) */
    f32 *strength;             /* brightness for choosing the most important lights per object */
} LightSet;

/* Collects the sun and up to LIGHT_CAP local lights (nearest to the camera first) into the light texture. */
static void gather_lights(A3Renderer *r, A3World *w, A3Vec3 cam, LightSet *ls, A3Arena *scratch) {
    a3_zero_struct(ls);
    u32 total = a3_component_count(w, A3_T_LIGHT);
    typedef struct Cand { A3Vec4 pos, color, dir; f32 dist, strength; } Cand;
    Cand *c = total ? A3_ARENA_PUSH_ARRAY(scratch, Cand, total) : 0;
    u32 n = 0;
    u32 types[2] = { A3_T_LIGHT, A3_T_TRANSFORM };
    A3Query q = a3_query_begin(w, types, 2);
    while (a3_query_next(&q)) {
        const A3CLight *l = (const A3CLight *)q.components[0];
        const A3CTransform *t = (const A3CTransform *)q.components[1];
        if (!a3_entity_active(w, q.entity)) continue;
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
        if (!c || l->intensity <= 0) continue;
        Cand *k = &c[n++];
        k->pos = a3_v4(pos.x, pos.y, pos.z, a3_maxf(l->range, 0.01f));
        k->color = a3_v4(color.x * 10.0f, color.y * 10.0f, color.z * 10.0f, l->type == A3_LIGHT_SPOT ? 2.0f : 1.0f);
        k->dir = a3_v4(fwd.x, fwd.y, fwd.z, a3_cosf(a3_clampf(l->spot_angle, 1, 179) * 0.5f * A3_DEG2RAD));
        k->dist = a3_maxf(a3_v3_dist(pos, cam) - l->range, 0.0f);
        k->strength = (color.x + color.y + color.z) * l->range;
    }
    if (!ls->has_sun) {
        ls->sun_dir = a3_v3_norm(a3_v3(-0.4f, -1.0f, -0.3f));
        ls->sun_color = a3_v3_zero();
    }
    /* keep the nearest LIGHT_CAP */
    if (n > LIGHT_CAP) {
        A3SortPair *sp = A3_ARENA_PUSH_ARRAY(scratch, A3SortPair, n * 2);
        for (u32 i = 0; i < n; ++i) { sp[i].key = (u64)(c[i].dist * 16.0f); sp[i].value = i; }
        a3_radix_sort_pairs(sp, sp + n, n);
        Cand *sorted = A3_ARENA_PUSH_ARRAY(scratch, Cand, LIGHT_CAP);
        for (u32 i = 0; i < LIGHT_CAP; ++i) sorted[i] = c[sp[i].value];
        c = sorted;
        n = LIGHT_CAP;
    }
    ls->count = n;
    ls->pos = n ? A3_ARENA_PUSH_ARRAY(scratch, A3Vec4, n) : 0;
    ls->strength = n ? A3_ARENA_PUSH_ARRAY(scratch, f32, n) : 0;
    for (u32 i = 0; i < n; ++i) {
        u32 x = (i % 256) * 3, y = i / 256;
        r->light_texels[(y * 256 * 3) + x] = c[i].pos;
        r->light_texels[(y * 256 * 3) + x + 1] = c[i].color;
        r->light_texels[(y * 256 * 3) + x + 2] = c[i].dir;
        ls->pos[i] = c[i].pos;
        ls->strength[i] = c[i].strength;
    }
    if (n) a3_rhi_texture_update(r->light_tex, 0, 0, 256 * 3, (i32)((n + 255) / 256), r->light_texels);
}

/* Up to `max` lights whose range reaches the sphere, most important first. */
static void pick_lights(const LightSet *ls, A3Vec4 sphere, u32 max, A3Vec4 *out0, A3Vec4 *out1) {
    f32 idx[MAX_OBJECT_LIGHTS], score[MAX_OBJECT_LIGHTS];
    u32 k = 0;
    for (u32 i = 0; i < ls->count; ++i) {
        A3Vec4 p = ls->pos[i];
        f32 dx = p.x - sphere.x, dy = p.y - sphere.y, dz = p.z - sphere.z;
        f32 reach = p.w + sphere.w;
        f32 d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > reach * reach) continue;
        f32 d = a3_maxf(a3_sqrtf(d2) - sphere.w, 0.0f);
        f32 sc = ls->strength[i] / (d * d + 1.0f);
        if (k < max) { idx[k] = (f32)i; score[k] = sc; k++; }
        else {
            u32 worst = 0;
            for (u32 j = 1; j < k; ++j) if (score[j] < score[worst]) worst = j;
            if (sc <= score[worst]) continue;
            idx[worst] = (f32)i;
            score[worst] = sc;
        }
    }
    f32 o[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
    for (u32 i = 0; i < k; ++i) o[i] = idx[i];
    *out0 = a3_v4(o[0], o[1], o[2], o[3]);
    *out1 = a3_v4(o[4], o[5], o[6], o[7]);
}

/* Cascaded shadow maps: the view frustum is cut into 3 slices (near, middle,
 * far); each slice gets its own sun projection fitted to the slice's bounding
 * sphere (stable size, so edges do not swim when the camera turns) and
 * rendered into one tile of a 2x2 atlas. Near objects get sharp shadows,
 * distant ones still get shadows. */
#define A3_CASCADES 3
typedef struct Cascade { A3Mat4 vp; f32 split; f32 texel; f32 inv_depth; } Cascade;

static void cascade_fit(A3Renderer *r, const A3RenderView *v, A3Vec3 sun_dir, f32 n, f32 f, i32 tile, Cascade *out) {
    A3Mat4 inv_view = a3_mat4_inverse_affine(&v->view);
    A3Vec3 fwd = a3_v3_norm(a3_mat4_mul_dir(&inv_view, a3_v3(0, 0, -1)));
    f32 tx = v->proj.m[0] != 0.0f ? 1.0f / a3_absf(v->proj.m[0]) : 1.0f;
    f32 ty = v->proj.m[5] != 0.0f ? 1.0f / a3_absf(v->proj.m[5]) : 1.0f;
    f32 k2 = tx * tx + ty * ty;
    /* bounding sphere of the slice: center on the view axis at c, radius to the far corners */
    f32 c = a3_minf(f, 0.5f * (n + f) * (1.0f + k2));
    f32 dfar = f - c, dnear = c - n;
    f32 rfar = a3_sqrtf(dfar * dfar + f * f * k2), rnear = a3_sqrtf(dnear * dnear + n * n * k2);
    f32 radius = a3_maxf(rfar, rnear);
    radius = a3_ceilf(radius * 4.0f) / 4.0f;               /* quantized: size does not jitter */
    A3Vec3 center = a3_v3_add(v->camera_pos, a3_v3_scale(fwd, c));
    A3Vec3 up = a3_absf(sun_dir.y) > 0.99f ? a3_v3(0, 0, 1) : a3_v3(0, 1, 0);
    f32 back = a3_maxf(radius * 2.0f, 450.0f);              /* tall towers behind the slice still cast */
    A3Mat4 light_view = a3_mat4_look_at(a3_v3_sub(center, a3_v3_scale(sun_dir, back)), center, up);
    /* snap to texels to stop shadow edges shimmering when the camera moves */
    f32 texel = (2.0f * radius) / (f32)tile;
    A3Vec3 lc = a3_mat4_mul_point(&light_view, center);
    A3Vec3 snapped = a3_v3(a3_floorf(lc.x / texel) * texel, a3_floorf(lc.y / texel) * texel, lc.z);
    A3Mat4 fix = a3_mat4_translation(a3_v3(snapped.x - lc.x, snapped.y - lc.y, 0));
    light_view = a3_mat4_mul(&fix, &light_view);
    f32 range = back + radius * 2.0f;
    A3Mat4 proj = a3_mat4_ortho(-radius, radius, -radius, radius, 0.1f, range, a3_rhi_depth_zero_to_one());
    out->vp = a3_mat4_mul(&proj, &light_view);
    out->split = f;
    out->texel = texel;
    out->inv_depth = 1.0f / range;
    A3_UNUSED(r);
}

static void shadow_cascades(A3Renderer *r, const A3RenderView *v, A3Vec3 sun_dir, Cascade *cs) {
    f32 dist = a3_maxf(r->settings.shadow_distance, 5.0f);
    f32 nearp = a3_maxf(v->near_plane, 0.05f);
    f32 splits[A3_CASCADES + 1] = { nearp, a3_maxf(dist * 0.08f, nearp + 1.0f), a3_maxf(dist * 0.3f, nearp + 2.0f), dist };
    i32 tile = r->shadow_size / 2;
    for (u32 i = 0; i < A3_CASCADES; ++i) cascade_fit(r, v, sun_dir, splits[i], splits[i + 1], tile, &cs[i]);
}

static int batch_key_bits(u32 v, u32 bits) { return (int)(v & ((1u << bits) - 1)); }

static u64 make_key(const Renderable *rd) {
    /* transparent last; then material, mesh, texture */
    return ((u64)(rd->transparent ? 1 : 0) << 63) | ((u64)batch_key_bits(rd->material, 8) << 48) |
           ((u64)batch_key_bits(rd->mesh, 16) << 24) | (u64)batch_key_bits(rd->texture, 24);
}

static f32 g_flash;   /* lightning of the frame */

static void set_frame_uniforms(A3RhiShader s, const A3RenderView *v, const LightSet *ls, const A3CWorldSettings *ws,
                               const A3Mat4 *view_proj, const Cascade *cs, b32 shadows, i32 shadow_size) {
    a3_rhi_set_mat4(s, "u_view_proj", view_proj);
    a3_rhi_set_mat4(s, "u_light_view_proj", &cs[0].vp);
    a3_rhi_set_mat4(s, "u_cascade_vp0", &cs[0].vp);
    a3_rhi_set_mat4(s, "u_cascade_vp1", &cs[1].vp);
    a3_rhi_set_mat4(s, "u_cascade_vp2", &cs[2].vp);
    a3_rhi_set_vec3(s, "u_cascade_split", a3_v3(cs[0].split, cs[1].split, cs[2].split));
    a3_rhi_set_vec3(s, "u_cascade_texel", a3_v3(cs[0].texel, cs[1].texel, cs[2].texel));
    a3_rhi_set_vec3(s, "u_cascade_depth", a3_v3(cs[0].inv_depth, cs[1].inv_depth, cs[2].inv_depth));
    a3_rhi_set_vec3(s, "u_camera_pos", v->camera_pos);
    a3_rhi_set_float(s, "u_time", v->time);
    a3_rhi_set_vec3(s, "u_sun_dir", ls->sun_dir);
    a3_rhi_set_vec3(s, "u_sun_color", ls->sun_color);
    a3_rhi_set_vec3(s, "u_sky_color", a3_v4_xyz(ws->sky_top));
    a3_rhi_set_vec3(s, "u_sky_horizon", a3_v4_xyz(ws->sky_horizon));
    a3_rhi_set_vec3(s, "u_ground_color", a3_v4_xyz(ws->ground_color));
    a3_rhi_set_float(s, "u_ambient", ws->ambient_intensity);
    a3_rhi_set_vec3(s, "u_fog_color", a3_v4_xyz(ws->fog_color));
    a3_rhi_set_float(s, "u_fog_density", ws->fog_density);
    a3_rhi_set_int(s, "u_shadows_enabled", shadows ? 1 : 0);
    a3_rhi_set_vec2(s, "u_shadow_texel", a3_v2(1.0f / (f32)shadow_size, 1.0f / (f32)shadow_size));
    a3_rhi_set_int(s, "u_light_tex", 4);
    a3_rhi_set_int(s, "u_fog_in_post", 1);
    a3_rhi_set_float(s, "u_wetness", a3_clampf(ws->wetness, 0.0f, 1.0f));
    a3_rhi_set_float(s, "u_rain", a3_clampf(ws->rain, 0.0f, 1.0f));
    a3_rhi_set_float(s, "u_flash", g_flash);
    A3_UNUSED(ls);
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
    gather_lights(r, w, v->camera_pos, &ls, scratch);
    r->info.lights = ls.count + (ls.has_sun ? 1 : 0);
    /* weather: rain clouds dim the sun, thicken and grey the fog; lightning */
    f32 rain = a3_clampf(ws->rain, 0.0f, 1.0f);
    g_flash = a3_weather_lightning(v->time, rain);
    if (rain > 0.0f) {
        ls.sun_color = a3_v3_scale(ls.sun_color, a3_weather_sun_factor(rain));
        ws_copy.fog_density *= 1.0f + 2.0f * rain;
        ws_copy.ambient_intensity *= 1.0f + 0.9f * rain;                  /* the grey sky lights everything evenly */
        f32 fl = (ws_copy.fog_color.x + ws_copy.fog_color.y + ws_copy.fog_color.z) / 3.0f;
        ws_copy.fog_color = a3_v4_lerp(ws_copy.fog_color, a3_v4(fl * 0.8f, fl * 0.82f, fl * 0.88f, 1), rain * 0.6f);
        f32 tl = (ws_copy.sky_top.x + ws_copy.sky_top.y + ws_copy.sky_top.z) / 3.0f;
        ws_copy.sky_top = a3_v4_lerp(ws_copy.sky_top, a3_v4(tl, tl, tl * 1.05f, 1), rain * 0.7f);
    }

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
        if (!rd->material && mr->material.path[0]) rd->material = material_from_path(r, mr->material.path);
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

    /* ---- lights per visible object (most important MAX_OBJECT_LIGHTS) ---- */
    u32 per_object = (u32)A3_CLAMP(r->settings.max_lights, 0, MAX_OBJECT_LIGHTS);
    for (u32 i = 0; i < n; ++i) {
        if (visible[i] && ls.count && per_object) pick_lights(&ls, items[i].sphere, per_object, &items[i].inst.lights0, &items[i].inst.lights1);
        else items[i].inst.lights0 = items[i].inst.lights1 = a3_v4(-1, -1, -1, -1);
    }

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
    Cascade cascades[A3_CASCADES];
    for (u32 c = 0; c < A3_CASCADES; ++c) { cascades[c].vp = a3_mat4_identity(); cascades[c].split = 1e9f; cascades[c].texel = 0.05f; cascades[c].inv_depth = 0.001f; }
    if (shadows) {
        shadow_cascades(r, v, ls.sun_dir, cascades);
        i32 tile = r->shadow_size / 2;
        a3_rhi_target_bind(r->shadow_target, r->shadow_size, r->shadow_size);
        a3_rhi_clear(0, a3_v4(0, 0, 0, 0), 1, 1.0f);
        A3RenderState st = { A3_BLEND_OPAQUE, A3_CULL_NONE, A3_DEPTH_LESS_WRITE, 0, 0, { 0 }, 2.0f };
        a3_rhi_set_state(&st);
        a3_rhi_shader_bind(r->shadow);
        for (u32 c = 0; c < A3_CASCADES; ++c) {
            /* atlas tiles: cascade 0 bottom-left, 1 bottom-right, 2 top-left */
            a3_rhi_viewport(c == 1 ? tile : 0, c == 2 ? tile : 0, tile, tile);
            a3_rhi_set_mat4(r->shadow, "u_view_proj", &cascades[c].vp);
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
    }

    /* ---- main pass (HDR) ---- */
    a3_rhi_target_bind(r->hdr_target, r->rt_w, r->rt_h);
    a3_rhi_gpu_timer_begin();
    A3Vec4 clear = a3_color_to_linear(v->clear_color);
    a3_rhi_clear(1, a3_v4(clear.x, clear.y, clear.z, 1), 1, 1.0f);
    if (v->draw_sky && r->sky.id) {
        A3RenderState st = { A3_BLEND_OPAQUE, A3_CULL_NONE, A3_DEPTH_OFF, 0, 0, { 0 }, 0 };
        a3_rhi_set_state(&st);
        a3_rhi_set_draw_buffers(1);
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
        a3_rhi_set_float(r->sky, "u_time", v->time);
        a3_rhi_set_float(r->sky, "u_clouds", ws_copy.cloud_cover);
        a3_rhi_set_float(r->sky, "u_rain", rain);
        a3_rhi_set_float(r->sky, "u_flash", g_flash);
        a3_rhi_draw_fullscreen();
    }
    a3_rhi_set_draw_buffers(2);
    a3_rhi_bind_texture(1, r->shadow_tex);
    a3_rhi_bind_texture(4, r->light_tex);
    u32 bound_material = 0xFFFFFFFFu;
    b32 color_only = 0;
    A3RhiShader shader = r->lit;
    for (u32 b = 0; b < nb; ++b) {
        Batch *bt = &batches[b];
        if (bt->material != bound_material) {
            bound_material = bt->material;
            Material *mat = bt->material ? &r->materials[bt->material] : 0;
            shader = (mat && mat->shader.id) ? mat->shader : r->lit;
            a3_rhi_shader_bind(shader);
            set_frame_uniforms(shader, v, &ls, &ws_copy, &view_proj, cascades, shadows, r->shadow_size);
            if (mat) {
                a3_rhi_set_vec4_array(shader, "u_material", mat->params, 4);
                a3_rhi_bind_texture(2, mat->textures[0] ? a3_assets_texture_rhi(mat->textures[0]) : a3_assets_white_texture());
                a3_rhi_bind_texture(3, mat->textures[1] ? a3_assets_texture_rhi(mat->textures[1]) : a3_assets_white_texture());
            }
        }
        if (bt->transparent && !color_only) { a3_rhi_set_draw_buffers(1); color_only = 1; }   /* keep the surface normals behind glass */
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

    /* ---- particles (after opaque geometry; depth tested, no depth writes) ---- */
    a3_rhi_set_draw_buffers(1);
    {
        A3ParticleBatch pb[128];
        const A3ParticleInstance *pinst = 0;
        u32 pcount = 0;
        u32 npb = a3_particles_collect(w, v->camera_pos, pb, A3_ARRAY_COUNT(pb), &pinst, &pcount);
        if (npb && pcount && r->particle.id) {
            usize bytes = sizeof(A3ParticleInstance) * pcount;
            if (bytes > r->particle_buf_size) {
                if (r->particle_buf.id) a3_rhi_buffer_destroy(r->particle_buf);
                r->particle_buf_size = bytes * 2;
                r->particle_buf = a3_rhi_buffer_create(A3_BUFFER_VERTEX, r->particle_buf_size, 0, 1);
            }
            a3_rhi_buffer_update(r->particle_buf, 0, bytes, pinst);
            a3_rhi_shader_bind(r->particle);
            a3_rhi_set_mat4(r->particle, "u_view_proj", &view_proj);
            /* camera axes are the rows of the view matrix */
            a3_rhi_set_vec3(r->particle, "u_cam_right", a3_v3(v->view.m[0], v->view.m[4], v->view.m[8]));
            a3_rhi_set_vec3(r->particle, "u_cam_up", a3_v3(v->view.m[1], v->view.m[5], v->view.m[9]));
            a3_rhi_set_int(r->particle, "u_tex", 0);
            for (u32 i = 0; i < npb; ++i) {
                A3RenderState st = { pb[i].blend == A3_PARTICLE_ALPHA ? A3_BLEND_ALPHA : A3_BLEND_ADDITIVE, A3_CULL_NONE, A3_DEPTH_LESS_NOWRITE, 0, 0, { 0 }, 0 };
                a3_rhi_set_state(&st);
                a3_rhi_set_int(r->particle, "u_has_texture", pb[i].texture ? 1 : 0);
                a3_rhi_bind_texture(0, pb[i].texture ? a3_assets_texture_rhi(pb[i].texture) : a3_assets_white_texture());
                a3_rhi_mesh_set_instances(r->particle_mesh, r->particle_buf, &r->particle_layout, sizeof(A3ParticleInstance) * pb[i].first);
                a3_rhi_draw(r->particle_mesh, A3_PRIM_TRIANGLES, 0, 6, pb[i].count);
            }
            r->info.particles = pcount;
        } else r->info.particles = 0;
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

    /* ---- post: SSAO -> composite (AO, reflections, height fog) -> bloom -> tonemap (+FXAA) ---- */
    A3RenderState post = { A3_BLEND_OPAQUE, A3_CULL_NONE, A3_DEPTH_OFF, 0, 0, { 0 }, 0 };
    TargetSet *ts = r->cur;
    b32 zo = a3_rhi_depth_zero_to_one();
    A3Mat4 inv_proj, inv_view;
    if (!a3_mat4_inverse(&v->proj, &inv_proj)) inv_proj = a3_mat4_identity();
    inv_view = a3_mat4_inverse_affine(&v->view);
    b32 do_ssao = r->settings.ssao && ws->ao_strength > 0 && r->ssao.id && ts->ao_target.id;
    if (do_ssao) {
        a3_rhi_target_bind(ts->ao_target, r->rt_w, r->rt_h);
        a3_rhi_set_state(&post);
        a3_rhi_shader_bind(r->ssao);
        a3_rhi_set_int(r->ssao, "u_depth", 0);
        a3_rhi_set_int(r->ssao, "u_normal", 1);
        a3_rhi_set_mat4(r->ssao, "u_proj", &v->proj);
        a3_rhi_set_mat4(r->ssao, "u_inv_proj", &inv_proj);
        a3_rhi_set_mat4(r->ssao, "u_view", &v->view);
        a3_rhi_set_int(r->ssao, "u_zo", zo ? 1 : 0);
        a3_rhi_set_float(r->ssao, "u_radius", 0.6f);
        a3_rhi_bind_texture(0, r->hdr_depth);
        a3_rhi_bind_texture(1, ts->normal);
        a3_rhi_draw_fullscreen();
        a3_rhi_target_bind(ts->ao_blur_target, r->rt_w, r->rt_h);
        a3_rhi_shader_bind(r->blur4);
        a3_rhi_set_int(r->blur4, "u_src", 0);
        a3_rhi_set_vec2(r->blur4, "u_texel", a3_v2(1.0f / r->rt_w, 1.0f / r->rt_h));
        a3_rhi_bind_texture(0, ts->ao);
        a3_rhi_draw_fullscreen();
    }
    if (r->composite.id) {
        a3_rhi_target_bind(ts->post_target, r->rt_w, r->rt_h);
        a3_rhi_set_state(&post);
        a3_rhi_shader_bind(r->composite);
        a3_rhi_set_int(r->composite, "u_hdr", 0);
        a3_rhi_set_int(r->composite, "u_depth", 1);
        a3_rhi_set_int(r->composite, "u_normal", 2);
        a3_rhi_set_int(r->composite, "u_ao", 3);
        a3_rhi_set_mat4(r->composite, "u_proj", &v->proj);
        a3_rhi_set_mat4(r->composite, "u_inv_proj", &inv_proj);
        a3_rhi_set_mat4(r->composite, "u_view", &v->view);
        a3_rhi_set_mat4(r->composite, "u_inv_view", &inv_view);
        a3_rhi_set_int(r->composite, "u_zo", zo ? 1 : 0);
        a3_rhi_set_vec3(r->composite, "u_camera_pos", v->camera_pos);
        a3_rhi_set_float(r->composite, "u_ao_strength", do_ssao ? ws->ao_strength : 0.0f);
        a3_rhi_set_float(r->composite, "u_ssr_strength", r->settings.ssr ? ws->reflection_strength : 0.0f);
        a3_rhi_set_vec3(r->composite, "u_fog_color", a3_v4_xyz(ws_copy.fog_color));
        a3_rhi_set_float(r->composite, "u_fog_density", ws_copy.fog_density);
        a3_rhi_set_float(r->composite, "u_rain", rain);
        a3_rhi_set_float(r->composite, "u_time", v->time);
        a3_rhi_set_float(r->composite, "u_aspect", (f32)r->rt_w / (f32)a3_maxi(r->rt_h, 1));
        {
            A3Vec3 cf = a3_v3_norm(a3_mat4_mul_dir(&inv_view, a3_v3(0, 0, -1)));
            f32 fov_y = v->proj.m[5] != 0.0f ? 2.0f * a3_atanf(1.0f / a3_absf(v->proj.m[5])) : 1.0f;
            a3_rhi_set_vec2(r->composite, "u_view_angles", a3_v2(a3_atan2f(cf.x, -cf.z) / fov_y, a3_asinf(a3_clampf(cf.y, -1, 1)) / fov_y));
        }
        a3_rhi_set_float(r->composite, "u_fog_falloff", ws->fog_height_falloff);
        a3_rhi_set_vec3(r->composite, "u_sun_dir", ls.sun_dir);
        a3_rhi_set_vec3(r->composite, "u_sun_color", ls.has_sun ? ls.sun_color : a3_v3_zero());
        {
            /* where the sun is on screen, and how much its shafts should show */
            A3Vec3 to_sun = a3_v3_neg(ls.sun_dir);
            A3Vec3 far_p = a3_v3_add(v->camera_pos, a3_v3_scale(to_sun, 1000.0f));
            A3Vec4 clip = a3_mat4_mul_v4(&view_proj, a3_v4(far_p.x, far_p.y, far_p.z, 1.0f));
            A3Vec3 fwd = a3_v3_norm(a3_mat4_mul_dir(&inv_view, a3_v3(0, 0, -1)));
            f32 facing = a3_smoothstep(0.0f, 0.35f, a3_v3_dot(fwd, to_sun));
            f32 elev = a3_smoothstep(-0.03f, 0.06f, to_sun.y);
            f32 vis = clip.w > 0.01f && ls.has_sun ? facing * elev : 0.0f;
            A3Vec2 suv = clip.w > 0.01f ? a3_v2(clip.x / clip.w * 0.5f + 0.5f, clip.y / clip.w * 0.5f + 0.5f) : a3_v2(0.5f, 0.5f);
            a3_rhi_set_vec3(r->composite, "u_sun_screen", a3_v3(suv.x, suv.y, vis));
            a3_rhi_set_float(r->composite, "u_shafts", ws->light_shafts);
        }
        a3_rhi_bind_texture(0, r->hdr_color);
        a3_rhi_bind_texture(1, r->hdr_depth);
        a3_rhi_bind_texture(2, ts->normal);
        a3_rhi_bind_texture(3, do_ssao ? ts->ao_blur : a3_assets_white_texture());
        a3_rhi_draw_fullscreen();
    }
    A3RhiTexture scene_tex = r->composite.id ? ts->post : r->hdr_color;
    b32 do_bloom = r->settings.bloom && ws->bloom_intensity > 0 && r->bloom_down.id && ts->bloom_target[0].id;
    if (do_bloom) {
        a3_rhi_set_state(&post);
        a3_rhi_shader_bind(r->bloom_down);
        a3_rhi_set_int(r->bloom_down, "u_src", 0);
        a3_rhi_set_float(r->bloom_down, "u_threshold", ws->bloom_threshold);
        A3RhiTexture src = scene_tex;
        i32 sw = r->rt_w, sh = r->rt_h;
        for (u32 i = 0; i < BLOOM_MIPS; ++i) {
            a3_rhi_target_bind(ts->bloom_target[i], ts->bloom_w[i], ts->bloom_h[i]);
            a3_rhi_set_int(r->bloom_down, "u_prefilter", i == 0 ? 1 : 0);
            a3_rhi_set_vec2(r->bloom_down, "u_texel", a3_v2(1.0f / sw, 1.0f / sh));
            a3_rhi_bind_texture(0, src);
            a3_rhi_draw_fullscreen();
            src = ts->bloom[i];
            sw = ts->bloom_w[i];
            sh = ts->bloom_h[i];
        }
        A3RenderState add = { A3_BLEND_ADDITIVE, A3_CULL_NONE, A3_DEPTH_OFF, 0, 0, { 0 }, 0 };
        a3_rhi_set_state(&add);
        a3_rhi_shader_bind(r->bloom_up);
        a3_rhi_set_int(r->bloom_up, "u_src", 0);
        for (i32 i = BLOOM_MIPS - 1; i > 0; --i) {
            a3_rhi_target_bind(ts->bloom_target[i - 1], ts->bloom_w[i - 1], ts->bloom_h[i - 1]);
            a3_rhi_set_vec2(r->bloom_up, "u_texel", a3_v2(1.0f / ts->bloom_w[i], 1.0f / ts->bloom_h[i]));
            a3_rhi_bind_texture(0, ts->bloom[i]);
            a3_rhi_draw_fullscreen();
        }
    }
    /* auto exposure: average log luminance, then adapt over time (real time, so it also works in the editor) */
    b32 do_auto = ws->auto_exposure > 0 && ensure_exposure(r);
    if (do_auto) {
        static const i32 sizes[4] = { 64, 16, 4, 1 };
        a3_rhi_set_state(&post);
        a3_rhi_target_bind(r->lum_target[0], 64, 64);
        a3_rhi_shader_bind(r->lum);
        a3_rhi_set_int(r->lum, "u_src", 0);
        a3_rhi_set_vec2(r->lum, "u_texel", a3_v2(1.0f / 64.0f, 1.0f / 64.0f));
        a3_rhi_bind_texture(0, scene_tex);
        a3_rhi_draw_fullscreen();
        a3_rhi_shader_bind(r->avg4);
        a3_rhi_set_int(r->avg4, "u_src", 0);
        for (u32 i = 1; i < 4; ++i) {
            a3_rhi_target_bind(r->lum_target[i], sizes[i], sizes[i]);
            a3_rhi_set_vec2(r->avg4, "u_src_texel", a3_v2(1.0f / (f32)sizes[i - 1], 1.0f / (f32)sizes[i - 1]));
            a3_rhi_bind_texture(0, r->lum_tex[i - 1]);
            a3_rhi_draw_fullscreen();
        }
        u64 now = a3_time_ns();
        f32 dt = r->adapt_clock ? a3_clampf((f32)((f64)(now - r->adapt_clock) / 1e9), 0.0f, 0.25f) : 0.0f;
        r->adapt_clock = now;
        u32 next = r->adapt_cur ^ 1u;
        a3_rhi_target_bind(r->adapt_target[next], 1, 1);
        a3_rhi_shader_bind(r->adapt);
        a3_rhi_set_int(r->adapt, "u_prev", 0);
        a3_rhi_set_int(r->adapt, "u_avg", 1);
        a3_rhi_set_float(r->adapt, "u_k_up", r->adapt_valid ? 1.0f - a3_expf(-dt * 2.5f) : 1.0f);    /* to brighter: fast */
        a3_rhi_set_float(r->adapt, "u_k_down", r->adapt_valid ? 1.0f - a3_expf(-dt * 1.0f) : 1.0f);  /* to darker: slower */
        a3_rhi_bind_texture(0, r->adapt_tex[r->adapt_cur]);
        a3_rhi_bind_texture(1, r->lum_tex[3]);
        a3_rhi_draw_fullscreen();
        r->adapt_cur = next;
        r->adapt_valid = 1;
    }
    f32 exposure = (v->exposure > 0 ? v->exposure : 1.0f) * (ws->exposure > 0 ? ws->exposure : 1.0f);
    A3Vec4 tint = a3_color_to_linear(ws->tint);
    if (r->settings.fxaa && r->fxaa.id) a3_rhi_target_bind(r->ldr_target, r->rt_w, r->rt_h);
    else a3_rhi_target_bind(v->target, v->width, v->height);
    a3_rhi_set_state(&post);
    a3_rhi_shader_bind(r->tonemap);
    a3_rhi_set_int(r->tonemap, "u_hdr", 0);
    a3_rhi_set_int(r->tonemap, "u_bloom", 1);
    a3_rhi_set_float(r->tonemap, "u_exposure", exposure);
    a3_rhi_set_float(r->tonemap, "u_vignette", r->settings.vignette);
    a3_rhi_set_float(r->tonemap, "u_bloom_intensity", do_bloom ? ws->bloom_intensity : 0.0f);
    a3_rhi_set_float(r->tonemap, "u_saturation", ws->saturation > 0 ? ws->saturation : 1.0f);
    a3_rhi_set_float(r->tonemap, "u_contrast", ws->contrast > 0 ? ws->contrast : 1.0f);
    a3_rhi_set_vec3(r->tonemap, "u_tint", (tint.x + tint.y + tint.z) > 0 ? a3_v4_xyz(tint) : a3_v3_one());
    a3_rhi_set_int(r->tonemap, "u_adapt", 2);
    a3_rhi_set_float(r->tonemap, "u_auto_exposure", do_auto ? ws->auto_exposure : 0.0f);
    a3_rhi_bind_texture(0, scene_tex);
    a3_rhi_bind_texture(1, do_bloom ? ts->bloom[0] : scene_tex);
    a3_rhi_bind_texture(2, do_auto ? r->adapt_tex[r->adapt_cur] : a3_assets_white_texture());
    a3_rhi_draw_fullscreen();
    if (r->settings.fxaa && r->fxaa.id) {
        a3_rhi_target_bind(v->target, v->width, v->height);
        a3_rhi_set_state(&post);
        a3_rhi_shader_bind(r->fxaa);
        a3_rhi_set_vec2(r->fxaa, "u_texel", a3_v2(1.0f / r->rt_w, 1.0f / r->rt_h));
        a3_rhi_bind_texture(0, r->ldr_color);
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

/* .a3shader assets (written by the Shader Maker) carry the generated surface
 * code, so games load materials without the graph compiler. A file that fails
 * is remembered and logged once; the object falls back to its simple material. */
static u32 material_from_path(A3Renderer *r, const char *path) {
    u64 key = a3_hash_str(path), val = 0;
    if (a3_hashmap_get(&r->path_materials, key, &val)) return val == 0xFFFFFFFFu ? 0 : (u32)val;
    u32 id = 0;
    if (a3_str_starts_with(path, "builtin:")) {
        const char *code = a3_builtin_material_code(path);
        A3ShaderCompileResult res;
        if (code) id = a3_renderer_material_create(r, code, path, &res);
        if (!code) a3_log_hint(A3_LOG_ERROR, "render", "Built-in materials: building, artdeco, tower, road, sidewalk, sand, water, glass, neon, carpaint, palm_trunk, foliage, metal.", "unknown built-in material '%s'", path);
        else if (!id) A3_ERROR("render", "built-in material %s failed to compile:\n%s", path, res.raw_log);
        a3_hashmap_put(&r->path_materials, key, id ? id : 0xFFFFFFFFu);
        return id;
    }
    char abs[A3_PATH_MAX * 2];
    a3_assets_path(path, abs, sizeof(abs));
    A3FileData fd;
    if (a3_file_read_all(abs, A3_MEM_TEMP, &fd) != A3_OK) {
        a3_log_hint(A3_LOG_ERROR, "render", "Check the Material field of the Mesh Renderer.", "material file not found: %s", path);
    } else {
        A3Arena ar;
        a3_arena_init(&ar, A3_MEM_TEMP, A3_KB(32));
        A3JsonError err;
        A3Json *root = a3_json_parse((const char *)fd.data, fd.size, &ar, &err);
        const char *code = root ? a3_json_get_string(root, "surface", 0) : 0;
        if (!code) {
            A3_ERROR("render", "material %s is damaged or has no compiled surface (line %d)", path, root ? 0 : err.line);
        } else {
            A3ShaderCompileResult res;
            id = a3_renderer_material_create(r, code, a3_json_get_string(root, "name", path), &res);
            if (!id) {
                A3_ERROR("render", "material %s failed to compile: %s", path, res.error_count ? res.errors[0].message : res.raw_log);
            } else {
                A3Vec4 params[4];
                for (int i = 0; i < 4; ++i) params[i] = a3_v4(1, 1, 1, 1);
                A3Json *pa = a3_json_get(root, "params");
                for (u32 i = 0; i < 4 && i < a3_json_count(pa); ++i) a3_json_get_floats(a3_json_at(pa, i), &params[i].x, 4);
                a3_renderer_material_set_params(r, id, params);
                A3Json *tx = a3_json_get(root, "textures");
                for (u32 i = 0; i < 2 && i < a3_json_count(tx); ++i) {
                    const char *tp = a3_json_string(a3_json_at(tx, i), "");
                    if (tp[0]) a3_renderer_material_set_texture(r, id, i, a3_assets_texture(tp));
                }
            }
        }
        a3_arena_release(&ar);
        a3_free(fd.data);
    }
    a3_hashmap_put(&r->path_materials, key, id ? id : 0xFFFFFFFFu);
    return id;
}

void a3_renderer_material_invalidate(A3Renderer *r, const char *path) {
    if (!r || !path) return;
    u64 key = a3_hash_str(path), val = 0;
    if (!a3_hashmap_get(&r->path_materials, key, &val)) return;
    if (val != 0xFFFFFFFFu) a3_renderer_material_destroy(r, (u32)val);
    a3_hashmap_remove(&r->path_materials, key);
}

void a3_renderer_set_entity_material(A3Renderer *r, A3World *w, A3Entity e, u32 material) {
    u64 guid = a3_entity_guid(w, e);
    if (!r || !guid) return;
    if (material) a3_hashmap_put(&r->entity_materials, guid, material);
    else a3_hashmap_remove(&r->entity_materials, guid);
}
