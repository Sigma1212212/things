/*
 * ASM3D - rhi_gl.c
 * OpenGL 3.3 core backend for a3_rhi.h.
 */
#include "a3_rhi.h"
#include "a3_gl.h"
#include "../core/a3_log.h"
#include "../core/a3_memory.h"
#include "../core/a3_string.h"
#include "../core/a3_format.h"
#include "../core/a3_hash.h"

#define MAX_BUFFERS 8192
#define MAX_TEXTURES 4096
#define MAX_SHADERS 1024
#define MAX_MESHES 8192
#define MAX_TARGETS 256
#define UNIFORM_CACHE 64
#define TIMER_QUERIES 4

typedef struct GlBuffer { GLuint name; GLenum target; usize size; b32 dynamic; b32 used; } GlBuffer;
typedef struct GlTexture { GLuint name; i32 w, h; A3TexFormat format; b32 mipmaps; b32 used; usize bytes; } GlTexture;
typedef struct UniformSlot { u64 hash; GLint loc; } UniformSlot;
typedef struct GlShader { GLuint program; b32 used; UniformSlot cache[UNIFORM_CACHE]; char name[48]; } GlShader;
typedef struct GlMesh { GLuint vao; b32 index32; b32 has_indices; b32 used; } GlMesh;
typedef struct GlTarget { GLuint fbo; b32 used; u32 color_count; } GlTarget;

static struct {
    b32 ready;
    A3RhiInfo info;
    A3RhiStats stats, last_stats;
    GlBuffer buffers[MAX_BUFFERS];
    GlTexture textures[MAX_TEXTURES];
    GlShader shaders[MAX_SHADERS];
    GlMesh meshes[MAX_MESHES];
    GlTarget targets[MAX_TARGETS];
    GLuint empty_vao;
    u32 bound_program;
    A3RenderState state;
    b32 state_valid;
    GLuint timer_queries[TIMER_QUERIES];
    b32 timer_pending[TIMER_QUERIES];
    u32 timer_index;
    b32 timer_active;
} g_gl;

/* ---- helpers ---- */
#define HANDLE_OK(tbl, h, max) ((h).id > 0 && (h).id <= (max) && g_gl.tbl[(h).id - 1].used)

#define FIND_FREE(tbl, max, out) do { out = 0; for (u32 _i = 0; _i < (max); ++_i) if (!g_gl.tbl[_i].used) { out = _i + 1; break; } } while (0)

static void gl_check(const char *where) {
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) A3_WARN("gl", "OpenGL error 0x%x after %s", e, where);
}

b32 a3_rhi_init(A3RhiGetProc get_proc) {
    if (g_gl.ready) return 1;
    int missing = a3_gl_load((A3GLGetProcFn)get_proc);
    if (missing) {
        a3_log_hint(A3_LOG_ERROR, "render", "Your graphics driver is missing features ASM3D needs. Update your graphics drivers.",
                    "%d OpenGL 3.3 functions unavailable", missing);
        return 0;
    }
    a3_strcpy(g_gl.info.api, sizeof(g_gl.info.api), "OpenGL 3.3 core");
    a3_strcpy(g_gl.info.vendor, sizeof(g_gl.info.vendor), (const char *)glGetString(GL_VENDOR));
    a3_strcpy(g_gl.info.renderer, sizeof(g_gl.info.renderer), (const char *)glGetString(GL_RENDERER));
    a3_strcpy(g_gl.info.version, sizeof(g_gl.info.version), (const char *)glGetString(GL_VERSION));
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &g_gl.info.max_texture_size);
    glGenVertexArrays(1, &g_gl.empty_vao);
    glGenQueries(TIMER_QUERIES, g_gl.timer_queries);
    g_gl.info.timer_queries = glGetError() == GL_NO_ERROR;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    g_gl.ready = 1;
    A3_INFO("render", "RHI: %s | %s | %s", g_gl.info.version, g_gl.info.renderer, g_gl.info.vendor);
    return 1;
}

void a3_rhi_shutdown(void) {
    if (!g_gl.ready) return;
    for (u32 i = 0; i < MAX_MESHES; ++i) if (g_gl.meshes[i].used) glDeleteVertexArrays(1, &g_gl.meshes[i].vao);
    for (u32 i = 0; i < MAX_BUFFERS; ++i) if (g_gl.buffers[i].used) glDeleteBuffers(1, &g_gl.buffers[i].name);
    for (u32 i = 0; i < MAX_TEXTURES; ++i) if (g_gl.textures[i].used) glDeleteTextures(1, &g_gl.textures[i].name);
    for (u32 i = 0; i < MAX_SHADERS; ++i) if (g_gl.shaders[i].used) glDeleteProgram(g_gl.shaders[i].program);
    for (u32 i = 0; i < MAX_TARGETS; ++i) if (g_gl.targets[i].used) glDeleteFramebuffers(1, &g_gl.targets[i].fbo);
    glDeleteVertexArrays(1, &g_gl.empty_vao);
    glDeleteQueries(TIMER_QUERIES, g_gl.timer_queries);
    a3_zero_struct(&g_gl);
}

const A3RhiInfo *a3_rhi_info(void) { return &g_gl.info; }
const A3RhiStats *a3_rhi_stats(void) { return &g_gl.last_stats; }
b32 a3_rhi_depth_zero_to_one(void) { return 0; }

void a3_rhi_begin_frame(void) {
    u64 bb = g_gl.stats.buffer_bytes, tb = g_gl.stats.texture_bytes;
    a3_zero_struct(&g_gl.stats);
    g_gl.stats.buffer_bytes = bb;
    g_gl.stats.texture_bytes = tb;
    g_gl.state_valid = 0;
}

void a3_rhi_end_frame(void) {
    f64 gpu = g_gl.last_stats.gpu_frame_ms;
    g_gl.last_stats = g_gl.stats;
    g_gl.last_stats.gpu_frame_ms = gpu;
    /* collect finished timer queries */
    if (g_gl.info.timer_queries) {
        for (u32 i = 0; i < TIMER_QUERIES; ++i) {
            if (!g_gl.timer_pending[i]) continue;
            GLint avail = 0;
            glGetQueryObjectiv(g_gl.timer_queries[i], GL_QUERY_RESULT_AVAILABLE, &avail);
            if (avail) {
                GLuint64 ns = 0;
                glGetQueryObjectui64v(g_gl.timer_queries[i], GL_QUERY_RESULT, &ns);
                g_gl.last_stats.gpu_frame_ms = (f64)ns / 1e6;
                g_gl.timer_pending[i] = 0;
            }
        }
    }
}

void a3_rhi_gpu_timer_begin(void) {
    if (!g_gl.info.timer_queries || g_gl.timer_active) return;
    u32 i = g_gl.timer_index;
    if (g_gl.timer_pending[i]) return; /* still in flight; skip this frame */
    glBeginQuery(GL_TIME_ELAPSED, g_gl.timer_queries[i]);
    g_gl.timer_active = 1;
}

void a3_rhi_gpu_timer_end(void) {
    if (!g_gl.timer_active) return;
    glEndQuery(GL_TIME_ELAPSED);
    g_gl.timer_pending[g_gl.timer_index] = 1;
    g_gl.timer_index = (g_gl.timer_index + 1) % TIMER_QUERIES;
    g_gl.timer_active = 0;
}

/* ======================================================================== */
/* Buffers                                                                  */
/* ======================================================================== */

static GLenum buffer_target(A3BufferType t) {
    return t == A3_BUFFER_INDEX ? GL_ELEMENT_ARRAY_BUFFER : t == A3_BUFFER_UNIFORM ? GL_UNIFORM_BUFFER : GL_ARRAY_BUFFER;
}

A3RhiBuffer a3_rhi_buffer_create(A3BufferType type, usize size, const void *data, b32 dynamic) {
    A3RhiBuffer h = { 0 };
    u32 slot;
    FIND_FREE(buffers, MAX_BUFFERS, slot);
    if (!slot) { A3_ERROR("render", "too many GPU buffers"); return h; }
    GlBuffer *b = &g_gl.buffers[slot - 1];
    glGenBuffers(1, &b->name);
    b->target = buffer_target(type);
    b->size = size;
    b->dynamic = dynamic;
    b->used = 1;
    if (b->target == GL_ELEMENT_ARRAY_BUFFER) glBindVertexArray(0); /* don't clobber a VAO's index binding */
    glBindBuffer(b->target, b->name);
    glBufferData(b->target, (GLsizeiptr)(size ? size : 16), data, dynamic ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
    g_gl.stats.buffer_bytes += size;
    h.id = slot;
    return h;
}

void a3_rhi_buffer_update(A3RhiBuffer h, usize offset, usize size, const void *data) {
    if (!HANDLE_OK(buffers, h, MAX_BUFFERS) || !data || !size) return;
    GlBuffer *b = &g_gl.buffers[h.id - 1];
    if (offset + size > b->size) { A3_ERROR("render", "buffer update out of range (%zu+%zu > %zu)", offset, size, b->size); return; }
    if (b->target == GL_ELEMENT_ARRAY_BUFFER) glBindVertexArray(0);
    glBindBuffer(b->target, b->name);
    glBufferSubData(b->target, (GLintptr)offset, (GLsizeiptr)size, data);
}

void a3_rhi_buffer_upload(A3RhiBuffer h, usize size, const void *data) {
    if (!HANDLE_OK(buffers, h, MAX_BUFFERS)) return;
    GlBuffer *b = &g_gl.buffers[h.id - 1];
    if (b->target == GL_ELEMENT_ARRAY_BUFFER) glBindVertexArray(0);
    glBindBuffer(b->target, b->name);
    if (size > b->size) {
        usize ns = b->size ? b->size : 1024;
        while (ns < size) ns *= 2;
        g_gl.stats.buffer_bytes += ns - b->size;
        glBufferData(b->target, (GLsizeiptr)ns, 0, GL_STREAM_DRAW);
        b->size = ns;
    } else {
        glBufferData(b->target, (GLsizeiptr)b->size, 0, GL_STREAM_DRAW); /* orphan to avoid stalls */
    }
    if (size && data) glBufferSubData(b->target, 0, (GLsizeiptr)size, data);
}

void a3_rhi_buffer_destroy(A3RhiBuffer h) {
    if (!HANDLE_OK(buffers, h, MAX_BUFFERS)) return;
    GlBuffer *b = &g_gl.buffers[h.id - 1];
    glDeleteBuffers(1, &b->name);
    g_gl.stats.buffer_bytes -= b->size;
    a3_zero_struct(b);
}

/* ======================================================================== */
/* Meshes                                                                   */
/* ======================================================================== */

static void apply_layout(const A3VertexLayout *l, b32 instances, usize base) {
    for (u32 i = 0; i < l->count; ++i) {
        const A3VertexAttrib *a = &l->attribs[i];
        glEnableVertexAttribArray(a->location);
        if (a->type == A3_ATTR_UINT)
            glVertexAttribIPointer(a->location, (GLint)a->components, GL_UNSIGNED_INT, (GLsizei)l->stride, (const void *)(uptr)(base + a->offset));
        else
            glVertexAttribPointer(a->location, (GLint)a->components, a->type == A3_ATTR_FLOAT ? GL_FLOAT : GL_UNSIGNED_BYTE,
                                  a->type == A3_ATTR_UBYTE_NORM, (GLsizei)l->stride, (const void *)(uptr)(base + a->offset));
        glVertexAttribDivisor(a->location, (instances || a->per_instance) ? 1 : 0);
    }
}

A3RhiMesh a3_rhi_mesh_create(A3RhiBuffer vertices, const A3VertexLayout *layout, A3RhiBuffer indices, b32 index32) {
    A3RhiMesh h = { 0 };
    if (!HANDLE_OK(buffers, vertices, MAX_BUFFERS) || !layout) { A3_ERROR("render", "mesh_create: invalid vertex buffer"); return h; }
    u32 slot;
    FIND_FREE(meshes, MAX_MESHES, slot);
    if (!slot) { A3_ERROR("render", "too many GPU meshes"); return h; }
    GlMesh *m = &g_gl.meshes[slot - 1];
    glGenVertexArrays(1, &m->vao);
    glBindVertexArray(m->vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_gl.buffers[vertices.id - 1].name);
    apply_layout(layout, 0, 0);
    if (HANDLE_OK(buffers, indices, MAX_BUFFERS)) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_gl.buffers[indices.id - 1].name);
        m->has_indices = 1;
    }
    glBindVertexArray(0);
    m->index32 = index32;
    m->used = 1;
    h.id = slot;
    return h;
}

void a3_rhi_mesh_set_instances(A3RhiMesh h, A3RhiBuffer instances, const A3VertexLayout *layout, usize byte_offset) {
    if (!HANDLE_OK(meshes, h, MAX_MESHES) || !HANDLE_OK(buffers, instances, MAX_BUFFERS) || !layout) return;
    glBindVertexArray(g_gl.meshes[h.id - 1].vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_gl.buffers[instances.id - 1].name);
    apply_layout(layout, 1, byte_offset);
    glBindVertexArray(0);
}

void a3_rhi_mesh_destroy(A3RhiMesh h) {
    if (!HANDLE_OK(meshes, h, MAX_MESHES)) return;
    glDeleteVertexArrays(1, &g_gl.meshes[h.id - 1].vao);
    a3_zero_struct(&g_gl.meshes[h.id - 1]);
}

/* ======================================================================== */
/* Textures                                                                 */
/* ======================================================================== */

static void tex_format(A3TexFormat f, GLint *internal, GLenum *format, GLenum *type, u32 *bpp) {
    switch (f) {
    case A3_TEX_R8: *internal = GL_R8; *format = GL_RED; *type = GL_UNSIGNED_BYTE; *bpp = 1; break;
    case A3_TEX_RGBA16F: *internal = GL_RGBA16F; *format = GL_RGBA; *type = GL_FLOAT; *bpp = 8; break;
    case A3_TEX_R16F: *internal = GL_R16F; *format = GL_RED; *type = GL_FLOAT; *bpp = 2; break;
    case A3_TEX_RG16F: *internal = GL_RG16F; *format = 0x8227 /* GL_RG */; *type = GL_FLOAT; *bpp = 4; break;
    case A3_TEX_RGBA32F: *internal = GL_RGBA32F; *format = GL_RGBA; *type = GL_FLOAT; *bpp = 16; break;
    case A3_TEX_DEPTH24: *internal = GL_DEPTH_COMPONENT24; *format = GL_DEPTH_COMPONENT; *type = GL_UNSIGNED_INT; *bpp = 4; break;
    case A3_TEX_DEPTH32F: *internal = 0x8CAC /* GL_DEPTH_COMPONENT32F */; *format = GL_DEPTH_COMPONENT; *type = GL_FLOAT; *bpp = 4; break;
    case A3_TEX_SRGBA8: *internal = 0x8C43 /* GL_SRGB8_ALPHA8 */; *format = GL_RGBA; *type = GL_UNSIGNED_BYTE; *bpp = 4; break;
    default: *internal = GL_RGBA8; *format = GL_RGBA; *type = GL_UNSIGNED_BYTE; *bpp = 4; break;
    }
}

A3RhiTexture a3_rhi_texture_create(const A3TextureDesc *d) {
    A3RhiTexture h = { 0 };
    if (!A3_VERIFY(d) || d->width <= 0 || d->height <= 0) { A3_ERROR("render", "texture_create: invalid size"); return h; }
    if (d->width > g_gl.info.max_texture_size || d->height > g_gl.info.max_texture_size) {
        a3_log_hint(A3_LOG_ERROR, "render", "The image is larger than your GPU supports. Reduce its size in the import settings.",
                    "texture %dx%d exceeds GPU limit %d", d->width, d->height, g_gl.info.max_texture_size);
        return h;
    }
    u32 slot;
    FIND_FREE(textures, MAX_TEXTURES, slot);
    if (!slot) { A3_ERROR("render", "too many textures"); return h; }
    GlTexture *t = &g_gl.textures[slot - 1];
    GLint internal; GLenum format, type; u32 bpp;
    tex_format(d->format, &internal, &format, &type, &bpp);
    glGenTextures(1, &t->name);
    glBindTexture(GL_TEXTURE_2D, t->name);
    GLenum data_type = type;
    if ((d->format == A3_TEX_RGBA16F || d->format == A3_TEX_R16F || d->format == A3_TEX_RG16F) && d->data) data_type = GL_FLOAT;
    glTexImage2D(GL_TEXTURE_2D, 0, internal, d->width, d->height, 0, format, data_type, d->data);
    b32 depth = d->format == A3_TEX_DEPTH24 || d->format == A3_TEX_DEPTH32F;
    GLint minf, magf;
    if (d->filter == A3_FILTER_NEAREST) { minf = magf = GL_NEAREST; }
    else { magf = GL_LINEAR; minf = (d->mipmaps && !depth) ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR; }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minf);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magf);
    GLint wrap = d->wrap == A3_WRAP_CLAMP ? GL_CLAMP_TO_EDGE : GL_REPEAT;
    if (depth && d->shadow_compare) {
        wrap = GL_CLAMP_TO_BORDER;
        static const f32 border[4] = { 1, 1, 1, 1 };
        glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    if (d->mipmaps && !depth) glGenerateMipmap(GL_TEXTURE_2D);
    t->w = d->width;
    t->h = d->height;
    t->format = d->format;
    t->mipmaps = d->mipmaps && !depth;
    t->bytes = (usize)d->width * (usize)d->height * bpp * (t->mipmaps ? 4 : 3) / 3;
    t->used = 1;
    g_gl.stats.texture_bytes += t->bytes;
    gl_check(d->debug_name ? d->debug_name : "texture_create");
    h.id = slot;
    return h;
}

void a3_rhi_texture_update(A3RhiTexture h, i32 x, i32 y, i32 w, i32 hh, const void *data) {
    if (!HANDLE_OK(textures, h, MAX_TEXTURES) || !data) return;
    GlTexture *t = &g_gl.textures[h.id - 1];
    if (x < 0 || y < 0 || x + w > t->w || y + hh > t->h) { A3_ERROR("render", "texture update out of bounds"); return; }
    GLint internal; GLenum format, type; u32 bpp;
    tex_format(t->format, &internal, &format, &type, &bpp);
    glBindTexture(GL_TEXTURE_2D, t->name);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, hh, format, type, data);
    if (t->mipmaps) glGenerateMipmap(GL_TEXTURE_2D);
}

void a3_rhi_texture_size(A3RhiTexture h, i32 *w, i32 *hh) {
    if (!HANDLE_OK(textures, h, MAX_TEXTURES)) { if (w) *w = 0; if (hh) *hh = 0; return; }
    if (w) *w = g_gl.textures[h.id - 1].w;
    if (hh) *hh = g_gl.textures[h.id - 1].h;
}

void a3_rhi_texture_destroy(A3RhiTexture h) {
    if (!HANDLE_OK(textures, h, MAX_TEXTURES)) return;
    GlTexture *t = &g_gl.textures[h.id - 1];
    glDeleteTextures(1, &t->name);
    g_gl.stats.texture_bytes -= t->bytes;
    a3_zero_struct(t);
}

u32 a3_rhi_texture_native(A3RhiTexture h) { return HANDLE_OK(textures, h, MAX_TEXTURES) ? g_gl.textures[h.id - 1].name : 0; }

/* ======================================================================== */
/* Shaders                                                                  */
/* ======================================================================== */

#if A3_PLATFORM_WEB
/* WebGL2: GLSL ES 3.00. The engine's shaders are written to compile as both
 * GLSL 3.30 core and GLSL ES 3.00 (explicit float literals, no implicit
 * int-to-float conversions). */
#define A3_ES_PRECISION "precision highp float;\nprecision highp int;\nprecision highp sampler2D;\nprecision highp sampler2DShadow;\n"
static const char *PRELUDE_VS =
    "#version 300 es\n"
    A3_ES_PRECISION
    "#define A3_GL 1\n"
    "#define A3_GLES 1\n"
    "#define A3_VERTEX 1\n"
    "#line 1\n";
static const char *PRELUDE_FS =
    "#version 300 es\n"
    A3_ES_PRECISION
    "#define A3_GL 1\n"
    "#define A3_GLES 1\n"
    "#define A3_FRAGMENT 1\n"
    "#line 1\n";
#else
static const char *PRELUDE_VS =
    "#version 330 core\n"
    "#define A3_GL 1\n"
    "#define A3_VERTEX 1\n"
    "#line 1\n";
static const char *PRELUDE_FS =
    "#version 330 core\n"
    "#define A3_GL 1\n"
    "#define A3_FRAGMENT 1\n"
    "precision highp float;\n"
    "#line 1\n";
#endif

/* Parses driver logs into structured errors. Handles the common formats:
 *   Mesa:    0:12(5): error: ...
 *   NVIDIA:  0(12) : error C0000: ...
 *   AMD/ARM: ERROR: 0:12: ...                                              */
static void parse_log(const char *log, i32 stage, A3ShaderCompileResult *r) {
    const char *p = log;
    while (*p && r->error_count < A3_SHADER_MAX_ERRORS) {
        const char *eol = a3_strchr(p, '\n');
        usize len = eol ? (usize)(eol - p) : a3_strlen(p);
        if (len > 2) {
            A3ShaderError *e = &r->errors[r->error_count];
            a3_zero_struct(e);
            e->stage = stage;
            const char *q = p;
            if (a3_strncmp(q, "ERROR: ", 7) == 0) q += 7;
            else if (a3_strncmp(q, "WARNING: ", 9) == 0) q += 9;
            /* skip file index */
            while (q < p + len && a3_is_digit(*q)) ++q;
            if (q < p + len && (*q == ':' || *q == '(')) {
                ++q;
                i32 line = 0;
                while (q < p + len && a3_is_digit(*q)) line = line * 10 + (*q++ - '0');
                e->line = line;
            }
            /* message: after "error:" or the last ": " */
            const char *msg = a3_strstr(p, "error");
            if (!msg || msg > p + len) msg = p;
            usize ml = (usize)(p + len - msg);
            if (ml >= sizeof(e->message)) ml = sizeof(e->message) - 1;
            a3_memcpy(e->message, msg, ml);
            e->message[ml] = 0;
            if (a3_stristr(e->message, "error") || a3_stristr(p, "error")) r->error_count++;
        }
        if (!eol) break;
        p = eol + 1;
    }
}

static GLuint compile_stage(GLenum type, const char *prelude, const char *src, i32 stage, A3ShaderCompileResult *r) {
    GLuint s = glCreateShader(type);
    const char *srcs[2] = { prelude, src ? src : "" };
    glShaderSource(s, 2, srcs, 0);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        GLsizei n = 0;
        glGetShaderInfoLog(s, sizeof(log) - 1, &n, log);
        log[n] = 0;
        usize used = a3_strlen(r->raw_log);
        a3_snprintf(r->raw_log + used, sizeof(r->raw_log) - used, "[%s]\n%s", stage == 0 ? "vertex" : "fragment", log);
        parse_log(log, stage, r);
        if (r->error_count == 0 && r->error_count < A3_SHADER_MAX_ERRORS) {
            r->errors[0].stage = stage;
            a3_strcpy(r->errors[0].message, sizeof(r->errors[0].message), log);
            r->error_count = 1;
        }
        glDeleteShader(s);
        return 0;
    }
    return s;
}

A3RhiShader a3_rhi_shader_create(const char *vs, const char *fs, const char *debug_name, A3ShaderCompileResult *result) {
    A3RhiShader h = { 0 };
    A3ShaderCompileResult local;
    A3ShaderCompileResult *r = result ? result : &local;
    a3_zero_struct(r);
    u32 slot;
    FIND_FREE(shaders, MAX_SHADERS, slot);
    if (!slot) { a3_strcpy(r->raw_log, sizeof(r->raw_log), "too many shaders"); return h; }
    GLuint v = compile_stage(GL_VERTEX_SHADER, PRELUDE_VS, vs, 0, r);
    GLuint f = compile_stage(GL_FRAGMENT_SHADER, PRELUDE_FS, fs, 1, r);
    if (!v || !f) {
        if (v) glDeleteShader(v);
        if (f) glDeleteShader(f);
        A3_ERROR("render", "shader '%s' failed to compile:\n%s", debug_name ? debug_name : "?", r->raw_log);
        return h;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, v);
    glAttachShader(prog, f);
    /* fixed attribute locations shared by all engine shaders */
    glBindAttribLocation(prog, 0, "a_position");
    glBindAttribLocation(prog, 1, "a_normal");
    glBindAttribLocation(prog, 2, "a_uv");
    glBindAttribLocation(prog, 3, "a_color");
    glLinkProgram(prog);
    glDetachShader(prog, v);
    glDetachShader(prog, f);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        GLsizei n = 0;
        glGetProgramInfoLog(prog, sizeof(log) - 1, &n, log);
        log[n] = 0;
        a3_snprintf(r->raw_log, sizeof(r->raw_log), "[link]\n%s", log);
        r->error_count = 1;
        r->errors[0].stage = 2;
        a3_strcpy(r->errors[0].message, sizeof(r->errors[0].message), log);
        glDeleteProgram(prog);
        A3_ERROR("render", "shader '%s' failed to link:\n%s", debug_name ? debug_name : "?", log);
        return h;
    }
    GlShader *s = &g_gl.shaders[slot - 1];
    a3_zero_struct(s);
    s->program = prog;
    s->used = 1;
    a3_strcpy(s->name, sizeof(s->name), debug_name ? debug_name : "shader");
    r->ok = 1;
    h.id = slot;
    return h;
}

void a3_rhi_shader_destroy(A3RhiShader h) {
    if (!HANDLE_OK(shaders, h, MAX_SHADERS)) return;
    GlShader *s = &g_gl.shaders[h.id - 1];
    if (g_gl.bound_program == s->program) { glUseProgram(0); g_gl.bound_program = 0; }
    glDeleteProgram(s->program);
    a3_zero_struct(s);
}

void a3_rhi_shader_bind(A3RhiShader h) {
    GLuint p = HANDLE_OK(shaders, h, MAX_SHADERS) ? g_gl.shaders[h.id - 1].program : 0;
    if (p != g_gl.bound_program) { glUseProgram(p); g_gl.bound_program = p; g_gl.stats.state_changes++; }
}

i32 a3_rhi_uniform_location(A3RhiShader h, const char *name) {
    if (!HANDLE_OK(shaders, h, MAX_SHADERS)) return -1;
    GlShader *s = &g_gl.shaders[h.id - 1];
    u64 hash = a3_hash_str(name);
    u32 idx = (u32)hash % UNIFORM_CACHE;
    for (u32 probe = 0; probe < UNIFORM_CACHE; ++probe) {
        UniformSlot *u = &s->cache[(idx + probe) % UNIFORM_CACHE];
        if (u->hash == hash) return u->loc;
        if (u->hash == 0) {
            u->hash = hash;
            u->loc = glGetUniformLocation(s->program, name);
            return u->loc;
        }
    }
    return glGetUniformLocation(s->program, name);
}

#define WITH_LOC(h, name) a3_rhi_shader_bind(h); GLint loc = a3_rhi_uniform_location(h, name); if (loc < 0) return;
void a3_rhi_set_int(A3RhiShader s, const char *n, i32 v) { WITH_LOC(s, n) glUniform1i(loc, v); }
void a3_rhi_set_float(A3RhiShader s, const char *n, f32 v) { WITH_LOC(s, n) glUniform1f(loc, v); }
void a3_rhi_set_vec2(A3RhiShader s, const char *n, A3Vec2 v) { WITH_LOC(s, n) glUniform2f(loc, v.x, v.y); }
void a3_rhi_set_vec3(A3RhiShader s, const char *n, A3Vec3 v) { WITH_LOC(s, n) glUniform3f(loc, v.x, v.y, v.z); }
void a3_rhi_set_vec4(A3RhiShader s, const char *n, A3Vec4 v) { WITH_LOC(s, n) glUniform4f(loc, v.x, v.y, v.z, v.w); }
void a3_rhi_set_mat4(A3RhiShader s, const char *n, const A3Mat4 *m) { WITH_LOC(s, n) glUniformMatrix4fv(loc, 1, GL_FALSE, m->m); }
void a3_rhi_set_vec4_array(A3RhiShader s, const char *n, const A3Vec4 *v, u32 c) { WITH_LOC(s, n) glUniform4fv(loc, (GLsizei)c, &v->x); }
void a3_rhi_set_mat4_array(A3RhiShader s, const char *n, const A3Mat4 *m, u32 c) { WITH_LOC(s, n) glUniformMatrix4fv(loc, (GLsizei)c, GL_FALSE, m->m); }

void a3_rhi_bind_texture(u32 slot, A3RhiTexture h) {
    glActiveTexture(GL_TEXTURE0 + slot);
    glBindTexture(GL_TEXTURE_2D, HANDLE_OK(textures, h, MAX_TEXTURES) ? g_gl.textures[h.id - 1].name : 0);
}

void a3_rhi_bind_uniform_buffer(u32 slot, A3RhiBuffer h) {
    if (!HANDLE_OK(buffers, h, MAX_BUFFERS)) return;
    glBindBufferBase(GL_UNIFORM_BUFFER, slot, g_gl.buffers[h.id - 1].name);
}

void a3_rhi_shader_bind_block(A3RhiShader h, const char *block, u32 slot) {
    if (!HANDLE_OK(shaders, h, MAX_SHADERS)) return;
    GLuint p = g_gl.shaders[h.id - 1].program;
    GLuint idx = glGetUniformBlockIndex(p, block);
    if (idx != 0xFFFFFFFFu) glUniformBlockBinding(p, idx, slot);
}

/* ======================================================================== */
/* Targets                                                                  */
/* ======================================================================== */

A3RhiTarget a3_rhi_target_create(A3RhiTexture color, A3RhiTexture depth) {
    A3RhiTarget h = { 0 };
    u32 slot;
    FIND_FREE(targets, MAX_TARGETS, slot);
    if (!slot) { A3_ERROR("render", "too many render targets"); return h; }
    GlTarget *t = &g_gl.targets[slot - 1];
    glGenFramebuffers(1, &t->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    if (HANDLE_OK(textures, color, MAX_TEXTURES)) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_gl.textures[color.id - 1].name, 0);
        GLenum db = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &db);
    } else {
        GLenum none = GL_NONE;
        glDrawBuffers(1, &none);
        glReadBuffer(GL_NONE);
    }
    if (HANDLE_OK(textures, depth, MAX_TEXTURES))
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, g_gl.textures[depth.id - 1].name, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        A3_ERROR("render", "framebuffer incomplete (0x%x)", st);
        glDeleteFramebuffers(1, &t->fbo);
        return h;
    }
    t->used = 1;
    h.id = slot;
    return h;
}

A3RhiTarget a3_rhi_target_create_mrt(const A3RhiTexture *colors, u32 count, A3RhiTexture depth) {
    A3RhiTarget h = { 0 };
    if (count > 4) count = 4;
    u32 slot;
    FIND_FREE(targets, MAX_TARGETS, slot);
    if (!slot) { A3_ERROR("render", "too many render targets"); return h; }
    GlTarget *t = &g_gl.targets[slot - 1];
    glGenFramebuffers(1, &t->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    GLenum dbs[4];
    for (u32 i = 0; i < count; ++i) {
        if (HANDLE_OK(textures, colors[i], MAX_TEXTURES))
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, g_gl.textures[colors[i].id - 1].name, 0);
        dbs[i] = GL_COLOR_ATTACHMENT0 + i;
    }
    glDrawBuffers((GLsizei)count, dbs);
    if (HANDLE_OK(textures, depth, MAX_TEXTURES))
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, g_gl.textures[depth.id - 1].name, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        A3_ERROR("render", "framebuffer incomplete (0x%x)", st);
        glDeleteFramebuffers(1, &t->fbo);
        return h;
    }
    t->used = 1;
    t->color_count = count;
    h.id = slot;
    return h;
}

/* Limits drawing to the first `count` color attachments of the bound target
 * (transparent objects, particles and lines only write the color buffer). */
void a3_rhi_set_draw_buffers(u32 count) {
    GLint fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    if (!fbo) return;
    GLenum dbs[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT0 + 1, GL_COLOR_ATTACHMENT0 + 2, GL_COLOR_ATTACHMENT0 + 3 };
    glDrawBuffers((GLsizei)(count > 4 ? 4 : count), dbs);
}

void a3_rhi_target_destroy(A3RhiTarget h) {
    if (!HANDLE_OK(targets, h, MAX_TARGETS)) return;
    glDeleteFramebuffers(1, &g_gl.targets[h.id - 1].fbo);
    a3_zero_struct(&g_gl.targets[h.id - 1]);
}

void a3_rhi_target_bind(A3RhiTarget h, i32 w, i32 hh) {
    GLuint fbo = HANDLE_OK(targets, h, MAX_TARGETS) ? g_gl.targets[h.id - 1].fbo : 0;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, w, hh);
}

void a3_rhi_viewport(i32 x, i32 y, i32 w, i32 hh) { glViewport(x, y, w, hh); }

void a3_rhi_clear(b32 color, A3Vec4 c, b32 depth, f32 dv) {
    GLbitfield mask = 0;
    if (color) { glClearColor(c.x, c.y, c.z, c.w); mask |= GL_COLOR_BUFFER_BIT; glColorMask(1, 1, 1, 1); }
    if (depth) { glClearDepth(dv); glDepthMask(GL_TRUE); mask |= GL_DEPTH_BUFFER_BIT; }
    glDisable(GL_SCISSOR_TEST);
    if (mask) glClear(mask);
    g_gl.state_valid = 0;
}

void a3_rhi_read_pixels(i32 x, i32 y, i32 w, i32 h, void *out) {
    glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out);
}

/* ======================================================================== */
/* State + draw                                                             */
/* ======================================================================== */

void a3_rhi_set_state(const A3RenderState *s) {
    A3RenderState *c = &g_gl.state;
    b32 force = !g_gl.state_valid;
    if (force || s->blend != c->blend) {
        switch (s->blend) {
        case A3_BLEND_OPAQUE: glDisable(GL_BLEND); break;
        case A3_BLEND_ALPHA: glEnable(GL_BLEND); glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
        case A3_BLEND_ADDITIVE: glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE); break;
        case A3_BLEND_PREMULTIPLIED: glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
        }
    }
    if (force || s->cull != c->cull) {
        if (s->cull == A3_CULL_NONE) glDisable(GL_CULL_FACE);
        else { glEnable(GL_CULL_FACE); glCullFace(s->cull == A3_CULL_BACK ? GL_BACK : GL_FRONT); glFrontFace(GL_CCW); }
    }
    if (force || s->depth != c->depth) {
        switch (s->depth) {
        case A3_DEPTH_LESS_WRITE: glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS); glDepthMask(GL_TRUE); break;
        case A3_DEPTH_LESS_NOWRITE: glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS); glDepthMask(GL_FALSE); break;
        case A3_DEPTH_LEQUAL_NOWRITE: glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); glDepthMask(GL_FALSE); break;
        case A3_DEPTH_LEQUAL_WRITE: glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); glDepthMask(GL_TRUE); break;
        case A3_DEPTH_OFF: glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE); break;
        }
    }
    if (force || s->wireframe != c->wireframe) glPolygonMode(GL_FRONT_AND_BACK, s->wireframe ? GL_LINE : GL_FILL);
    if (s->scissor) { glEnable(GL_SCISSOR_TEST); glScissor(s->scissor_rect[0], s->scissor_rect[1], s->scissor_rect[2], s->scissor_rect[3]); }
    else if (force || c->scissor) glDisable(GL_SCISSOR_TEST);
    if (force || s->depth_bias != c->depth_bias) {
        if (s->depth_bias != 0.0f) { glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(s->depth_bias, s->depth_bias); }
        else glDisable(GL_POLYGON_OFFSET_FILL);
    }
    *c = *s;
    g_gl.state_valid = 1;
    g_gl.stats.state_changes++;
}

static GLenum prim_mode(A3PrimType p) { return p == A3_PRIM_LINES ? GL_LINES : p == A3_PRIM_POINTS ? GL_POINTS : GL_TRIANGLES; }

void a3_rhi_draw(A3RhiMesh h, A3PrimType prim, u32 first, u32 count, u32 instances) {
    if (!HANDLE_OK(meshes, h, MAX_MESHES) || !count) return;
    GlMesh *m = &g_gl.meshes[h.id - 1];
    glBindVertexArray(m->vao);
    if (m->has_indices) {
        GLenum it = m->index32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
        const void *off = (const void *)(uptr)(first * (m->index32 ? 4u : 2u));
        if (instances > 1 || instances == 0) glDrawElementsInstanced(prim_mode(prim), (GLsizei)count, it, off, (GLsizei)(instances ? instances : 1));
        else glDrawElements(prim_mode(prim), (GLsizei)count, it, off);
    } else {
        if (instances > 1) glDrawArraysInstanced(prim_mode(prim), (GLint)first, (GLsizei)count, (GLsizei)instances);
        else glDrawArrays(prim_mode(prim), (GLint)first, (GLsizei)count);
    }
    u32 inst = instances ? instances : 1;
    g_gl.stats.draw_calls++;
    g_gl.stats.instances += inst;
    if (prim == A3_PRIM_TRIANGLES) g_gl.stats.triangles += count / 3 * inst;
}

void a3_rhi_draw_arrays(A3RhiMesh h, A3PrimType prim, u32 first, u32 count, u32 instances) {
    if (!HANDLE_OK(meshes, h, MAX_MESHES) || !count) return;
    glBindVertexArray(g_gl.meshes[h.id - 1].vao);
    if (instances > 1) glDrawArraysInstanced(prim_mode(prim), (GLint)first, (GLsizei)count, (GLsizei)instances);
    else glDrawArrays(prim_mode(prim), (GLint)first, (GLsizei)count);
    u32 inst = instances ? instances : 1;
    g_gl.stats.draw_calls++;
    g_gl.stats.instances += inst;
    if (prim == A3_PRIM_TRIANGLES) g_gl.stats.triangles += count / 3 * inst;
}

void a3_rhi_draw_fullscreen(void) {
    glBindVertexArray(g_gl.empty_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    g_gl.stats.draw_calls++;
    g_gl.stats.triangles += 1;
}
