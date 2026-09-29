/*
 * ASM3D - rhi_null.c
 * Null RHI backend: accepts every call and draws nothing. Used by headless
 * builds (unit tests, command-line tools, dedicated servers) and as the
 * placeholder on targets whose GPU backend is not implemented yet.
 */
#include "a3_rhi.h"
#include "../core/a3_string.h"

static A3RhiInfo g_null_info; /* api[0] == 0 means "no GPU" to callers */
static A3RhiStats g_null_stats;

b32  a3_rhi_init(A3RhiGetProc get_proc) { A3_UNUSED(get_proc); return 0; }
void a3_rhi_shutdown(void) {}
const A3RhiInfo *a3_rhi_info(void) { return &g_null_info; }
void a3_rhi_begin_frame(void) {}
void a3_rhi_end_frame(void) {}
const A3RhiStats *a3_rhi_stats(void) { return &g_null_stats; }
b32  a3_rhi_depth_zero_to_one(void) { return 0; }
A3RhiBuffer a3_rhi_buffer_create(A3BufferType t, usize s, const void *d, b32 dy) { A3_UNUSED(t); A3_UNUSED(s); A3_UNUSED(d); A3_UNUSED(dy); A3RhiBuffer b = { 0 }; return b; }
void a3_rhi_buffer_update(A3RhiBuffer b, usize o, usize s, const void *d) { A3_UNUSED(b); A3_UNUSED(o); A3_UNUSED(s); A3_UNUSED(d); }
void a3_rhi_buffer_upload(A3RhiBuffer b, usize s, const void *d) { A3_UNUSED(b); A3_UNUSED(s); A3_UNUSED(d); }
void a3_rhi_buffer_destroy(A3RhiBuffer b) { A3_UNUSED(b); }
A3RhiMesh a3_rhi_mesh_create(A3RhiBuffer v, const A3VertexLayout *l, A3RhiBuffer i, b32 i32_) { A3_UNUSED(v); A3_UNUSED(l); A3_UNUSED(i); A3_UNUSED(i32_); A3RhiMesh m = { 0 }; return m; }
void a3_rhi_mesh_set_instances(A3RhiMesh m, A3RhiBuffer b, const A3VertexLayout *l, usize o) { A3_UNUSED(m); A3_UNUSED(b); A3_UNUSED(l); A3_UNUSED(o); }
void a3_rhi_mesh_destroy(A3RhiMesh m) { A3_UNUSED(m); }
A3RhiTexture a3_rhi_texture_create(const A3TextureDesc *d) { A3_UNUSED(d); A3RhiTexture t = { 0 }; return t; }
void a3_rhi_texture_update(A3RhiTexture t, i32 x, i32 y, i32 w, i32 h, const void *d) { A3_UNUSED(t); A3_UNUSED(x); A3_UNUSED(y); A3_UNUSED(w); A3_UNUSED(h); A3_UNUSED(d); }
void a3_rhi_texture_size(A3RhiTexture t, i32 *w, i32 *h) { A3_UNUSED(t); if (w) *w = 0; if (h) *h = 0; }
void a3_rhi_texture_destroy(A3RhiTexture t) { A3_UNUSED(t); }
u32  a3_rhi_texture_native(A3RhiTexture t) { A3_UNUSED(t); return 0; }
A3RhiShader a3_rhi_shader_create(const char *vs, const char *fs, const char *n, A3ShaderCompileResult *r) {
    A3_UNUSED(vs); A3_UNUSED(fs); A3_UNUSED(n);
    if (r) { a3_zero_struct(r); a3_strcpy(r->raw_log, sizeof(r->raw_log), "no GPU backend in this build"); }
    A3RhiShader s = { 0 };
    return s;
}
void a3_rhi_shader_destroy(A3RhiShader s) { A3_UNUSED(s); }
void a3_rhi_shader_bind(A3RhiShader s) { A3_UNUSED(s); }
i32  a3_rhi_uniform_location(A3RhiShader s, const char *n) { A3_UNUSED(s); A3_UNUSED(n); return -1; }
void a3_rhi_set_int(A3RhiShader s, const char *n, i32 v) { A3_UNUSED(s); A3_UNUSED(n); A3_UNUSED(v); }
void a3_rhi_set_float(A3RhiShader s, const char *n, f32 v) { A3_UNUSED(s); A3_UNUSED(n); A3_UNUSED(v); }
void a3_rhi_set_vec2(A3RhiShader s, const char *n, A3Vec2 v) { A3_UNUSED(s); A3_UNUSED(n); A3_UNUSED(v); }
void a3_rhi_set_vec3(A3RhiShader s, const char *n, A3Vec3 v) { A3_UNUSED(s); A3_UNUSED(n); A3_UNUSED(v); }
void a3_rhi_set_vec4(A3RhiShader s, const char *n, A3Vec4 v) { A3_UNUSED(s); A3_UNUSED(n); A3_UNUSED(v); }
void a3_rhi_set_mat4(A3RhiShader s, const char *n, const A3Mat4 *m) { A3_UNUSED(s); A3_UNUSED(n); A3_UNUSED(m); }
void a3_rhi_set_vec4_array(A3RhiShader s, const char *n, const A3Vec4 *v, u32 c) { A3_UNUSED(s); A3_UNUSED(n); A3_UNUSED(v); A3_UNUSED(c); }
void a3_rhi_set_mat4_array(A3RhiShader s, const char *n, const A3Mat4 *m, u32 c) { A3_UNUSED(s); A3_UNUSED(n); A3_UNUSED(m); A3_UNUSED(c); }
void a3_rhi_bind_texture(u32 slot, A3RhiTexture t) { A3_UNUSED(slot); A3_UNUSED(t); }
void a3_rhi_bind_uniform_buffer(u32 slot, A3RhiBuffer b) { A3_UNUSED(slot); A3_UNUSED(b); }
void a3_rhi_shader_bind_block(A3RhiShader s, const char *b, u32 slot) { A3_UNUSED(s); A3_UNUSED(b); A3_UNUSED(slot); }
A3RhiTarget a3_rhi_target_create(A3RhiTexture c, A3RhiTexture d) { A3_UNUSED(c); A3_UNUSED(d); A3RhiTarget t = { 0 }; return t; }
A3RhiTarget a3_rhi_target_create_mrt(const A3RhiTexture *c, u32 n, A3RhiTexture d) { A3_UNUSED(c); A3_UNUSED(n); A3_UNUSED(d); A3RhiTarget t = { 0 }; return t; }
void a3_rhi_set_draw_buffers(u32 n) { A3_UNUSED(n); }
void a3_rhi_target_destroy(A3RhiTarget t) { A3_UNUSED(t); }
void a3_rhi_target_bind(A3RhiTarget t, i32 w, i32 h) { A3_UNUSED(t); A3_UNUSED(w); A3_UNUSED(h); }
void a3_rhi_clear(b32 c, A3Vec4 rgba, b32 d, f32 dv) { A3_UNUSED(c); A3_UNUSED(rgba); A3_UNUSED(d); A3_UNUSED(dv); }
void a3_rhi_read_pixels(i32 x, i32 y, i32 w, i32 h, void *out) { A3_UNUSED(x); A3_UNUSED(y); if (out && w > 0 && h > 0) a3_memset(out, 0, (usize)w * (usize)h * 4); }
void a3_rhi_set_state(const A3RenderState *s) { A3_UNUSED(s); }
void a3_rhi_draw(A3RhiMesh m, A3PrimType p, u32 f, u32 c, u32 i) { A3_UNUSED(m); A3_UNUSED(p); A3_UNUSED(f); A3_UNUSED(c); A3_UNUSED(i); }
void a3_rhi_draw_arrays(A3RhiMesh m, A3PrimType p, u32 f, u32 c, u32 i) { A3_UNUSED(m); A3_UNUSED(p); A3_UNUSED(f); A3_UNUSED(c); A3_UNUSED(i); }
void a3_rhi_draw_fullscreen(void) {}
void a3_rhi_gpu_timer_begin(void) {}
void a3_rhi_gpu_timer_end(void) {}
