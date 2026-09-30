/*
 * ASM3D - a3_rhi.h
 * Render Hardware Interface: the only layer that talks to the GPU API.
 * Current backend: OpenGL 3.3 core (rhi_gl.c). The interface uses opaque
 * integer handles and explicit state so Vulkan / D3D12 / Metal / WebGPU
 * backends can be added without changing renderer or game code.
 */
#ifndef A3_RHI_H
#define A3_RHI_H

#include "../core/a3_base.h"
#include "../core/a3_math.h"

A3_EXTERN_C_BEGIN

typedef struct A3RhiBuffer { u32 id; } A3RhiBuffer;
typedef struct A3RhiTexture { u32 id; } A3RhiTexture;
typedef struct A3RhiShader { u32 id; } A3RhiShader;
typedef struct A3RhiMesh { u32 id; } A3RhiMesh;       /* vertex layout + buffers (VAO) */
typedef struct A3RhiTarget { u32 id; } A3RhiTarget;   /* framebuffer */

typedef enum A3BufferType { A3_BUFFER_VERTEX = 0, A3_BUFFER_INDEX, A3_BUFFER_UNIFORM } A3BufferType;

typedef enum A3TexFormat {
    A3_TEX_RGBA8 = 0, A3_TEX_R8, A3_TEX_RGBA16F, A3_TEX_R16F, A3_TEX_RG16F, A3_TEX_RGBA32F,
    A3_TEX_DEPTH24, A3_TEX_DEPTH32F, A3_TEX_SRGBA8, A3_TEX_FORMAT_COUNT
} A3TexFormat;

typedef enum A3TexFilter { A3_FILTER_LINEAR = 0, A3_FILTER_NEAREST, A3_FILTER_TRILINEAR } A3TexFilter;
typedef enum A3TexWrap { A3_WRAP_REPEAT = 0, A3_WRAP_CLAMP } A3TexWrap;

typedef struct A3TextureDesc {
    i32 width, height;
    A3TexFormat format;
    A3TexFilter filter;
    A3TexWrap wrap;
    b32 mipmaps;
    b32 shadow_compare;  /* depth textures sampled with sampler2DShadow */
    const void *data;    /* optional initial pixels, tightly packed */
    const char *debug_name;
} A3TextureDesc;

typedef enum A3AttribType { A3_ATTR_FLOAT = 0, A3_ATTR_UBYTE_NORM, A3_ATTR_UINT } A3AttribType;

typedef struct A3VertexAttrib {
    u32 location;
    u32 components;      /* 1..4 */
    A3AttribType type;
    u32 offset;
    b32 per_instance;
} A3VertexAttrib;

#define A3_MAX_ATTRIBS 12
typedef struct A3VertexLayout {
    A3VertexAttrib attribs[A3_MAX_ATTRIBS];
    u32 count;
    u32 stride;
} A3VertexLayout;

typedef enum A3BlendMode { A3_BLEND_OPAQUE = 0, A3_BLEND_ALPHA, A3_BLEND_ADDITIVE, A3_BLEND_PREMULTIPLIED } A3BlendMode;
typedef enum A3CullMode { A3_CULL_BACK = 0, A3_CULL_FRONT, A3_CULL_NONE } A3CullMode;
typedef enum A3DepthMode { A3_DEPTH_LESS_WRITE = 0, A3_DEPTH_LESS_NOWRITE, A3_DEPTH_OFF, A3_DEPTH_LEQUAL_NOWRITE, A3_DEPTH_LEQUAL_WRITE } A3DepthMode;
typedef enum A3Primitive2 { A3_PRIM_TRIANGLES = 0, A3_PRIM_LINES, A3_PRIM_POINTS } A3PrimType;

typedef struct A3RenderState {
    A3BlendMode blend;
    A3CullMode cull;
    A3DepthMode depth;
    b32 wireframe;
    b32 scissor;
    i32 scissor_rect[4];     /* x, y (bottom-left origin), w, h */
    f32 depth_bias;          /* polygon offset factor/units (shadow maps) */
} A3RenderState;

/* A shader compile/link error mapped to source lines (for the Shader Maker
 * and the error assistant). */
#define A3_SHADER_MAX_ERRORS 16
typedef struct A3ShaderError {
    i32 stage;               /* 0 = vertex, 1 = fragment, 2 = link */
    i32 line;                /* line in the *user* source (prelude removed), 0 if unknown */
    char message[200];
} A3ShaderError;

typedef struct A3ShaderCompileResult {
    b32 ok;
    u32 error_count;
    A3ShaderError errors[A3_SHADER_MAX_ERRORS];
    char raw_log[2048];
} A3ShaderCompileResult;

typedef struct A3RhiInfo {
    char api[32];
    char vendor[128];
    char renderer[128];
    char version[128];
    i32 max_texture_size;
    b32 timer_queries;
} A3RhiInfo;

typedef struct A3RhiStats {
    u32 draw_calls;
    u32 triangles;
    u32 instances;
    u32 state_changes;
    u64 buffer_bytes;
    u64 texture_bytes;
    f64 gpu_frame_ms;        /* from timer queries, 0 if unsupported */
} A3RhiStats;

/* ---- Lifetime ---- */
typedef void *(*A3RhiGetProc)(const char *name);
b32  a3_rhi_init(A3RhiGetProc get_proc);
void a3_rhi_shutdown(void);
const A3RhiInfo *a3_rhi_info(void);
void a3_rhi_begin_frame(void);
void a3_rhi_end_frame(void);
const A3RhiStats *a3_rhi_stats(void);   /* stats of the last completed frame */
/* Depth range convention of the active backend (0 = GL -1..1, 1 = 0..1). */
b32  a3_rhi_depth_zero_to_one(void);

/* ---- Buffers ---- */
A3RhiBuffer a3_rhi_buffer_create(A3BufferType type, usize size, const void *data, b32 dynamic);
void a3_rhi_buffer_update(A3RhiBuffer b, usize offset, usize size, const void *data);
/* Replaces the whole contents, growing the buffer if needed (streaming data). */
void a3_rhi_buffer_upload(A3RhiBuffer b, usize size, const void *data);
void a3_rhi_buffer_destroy(A3RhiBuffer b);

/* ---- Meshes (vertex array objects) ---- */
A3RhiMesh a3_rhi_mesh_create(A3RhiBuffer vertices, const A3VertexLayout *layout, A3RhiBuffer indices, b32 index32);
/* Points the per-instance attributes at `instances` starting at byte_offset
 * (one shared instance buffer serves all batches in a frame). */
void a3_rhi_mesh_set_instances(A3RhiMesh m, A3RhiBuffer instances, const A3VertexLayout *layout, usize byte_offset);
void a3_rhi_mesh_destroy(A3RhiMesh m);

/* ---- Textures ---- */
A3RhiTexture a3_rhi_texture_create(const A3TextureDesc *desc);
void a3_rhi_texture_update(A3RhiTexture t, i32 x, i32 y, i32 w, i32 h, const void *data);
void a3_rhi_texture_size(A3RhiTexture t, i32 *w, i32 *h);
void a3_rhi_texture_destroy(A3RhiTexture t);
u32  a3_rhi_texture_native(A3RhiTexture t); /* backend object (GL name) for editor viewports */

/* ---- Shaders ----
 * Sources are GLSL 330-style bodies without a #version line; the backend
 * adds its prelude. Line numbers in errors refer to the given sources. */
A3RhiShader a3_rhi_shader_create(const char *vertex_src, const char *fragment_src, const char *debug_name, A3ShaderCompileResult *result);
void a3_rhi_shader_destroy(A3RhiShader s);
void a3_rhi_shader_bind(A3RhiShader s);
i32  a3_rhi_uniform_location(A3RhiShader s, const char *name);
void a3_rhi_set_int(A3RhiShader s, const char *name, i32 v);
void a3_rhi_set_float(A3RhiShader s, const char *name, f32 v);
void a3_rhi_set_vec2(A3RhiShader s, const char *name, A3Vec2 v);
void a3_rhi_set_vec3(A3RhiShader s, const char *name, A3Vec3 v);
void a3_rhi_set_vec4(A3RhiShader s, const char *name, A3Vec4 v);
void a3_rhi_set_mat4(A3RhiShader s, const char *name, const A3Mat4 *m);
void a3_rhi_set_vec4_array(A3RhiShader s, const char *name, const A3Vec4 *v, u32 count);
void a3_rhi_set_mat4_array(A3RhiShader s, const char *name, const A3Mat4 *m, u32 count);
void a3_rhi_bind_texture(u32 slot, A3RhiTexture t);
void a3_rhi_bind_uniform_buffer(u32 slot, A3RhiBuffer b);
void a3_rhi_shader_bind_block(A3RhiShader s, const char *block_name, u32 slot);

/* ---- Render targets ---- */
A3RhiTarget a3_rhi_target_create(A3RhiTexture color, A3RhiTexture depth);
A3RhiTarget a3_rhi_target_create_mrt(const A3RhiTexture *colors, u32 count, A3RhiTexture depth); /* up to 4 color attachments */
void a3_rhi_set_draw_buffers(u32 count);   /* write only the first `count` attachments of the bound target */
void a3_rhi_target_destroy(A3RhiTarget t);
/* Binds a target (id 0 = window backbuffer) and sets the viewport. */
void a3_rhi_target_bind(A3RhiTarget t, i32 width, i32 height);
/* Sets the viewport inside the bound target (bottom-left origin), e.g. a shadow atlas tile. */
void a3_rhi_viewport(i32 x, i32 y, i32 width, i32 height);
void a3_rhi_clear(b32 color, A3Vec4 rgba, b32 depth, f32 depth_value);
/* Reads RGBA8 pixels of the bound target (bottom-up rows). */
void a3_rhi_read_pixels(i32 x, i32 y, i32 w, i32 h, void *out_rgba);

/* ---- Drawing ---- */
void a3_rhi_set_state(const A3RenderState *s);
void a3_rhi_draw(A3RhiMesh m, A3PrimType prim, u32 first_index, u32 index_count, u32 instance_count);
void a3_rhi_draw_arrays(A3RhiMesh m, A3PrimType prim, u32 first, u32 count, u32 instance_count);
/* Draws a full-screen triangle (post-processing); vertex shader uses gl_VertexID. */
void a3_rhi_draw_fullscreen(void);

/* ---- GPU timing ---- */
void a3_rhi_gpu_timer_begin(void);
void a3_rhi_gpu_timer_end(void);

A3_EXTERN_C_END

#endif
