/*
 * ASM3D - a3_assets.c
 */
#include "a3_assets.h"
#include "../core/a3_log.h"
#include "../core/a3_memory.h"
#include "../core/a3_string.h"
#include "../core/a3_format.h"
#include "../core/a3_image.h"
#include "../platform/a3_platform.h"

#define MAX_MESHES 4096
#define MAX_TEXTURES 4096
#define MAX_MESH_GENERATORS 64

typedef struct MeshGen { char path[64]; A3MeshGenerator fn; } MeshGen;
static MeshGen g_mesh_gens[MAX_MESH_GENERATORS];
static u32 g_mesh_gen_count;

static struct {
    char root[A3_PATH_MAX];
    A3MeshAsset *meshes;      /* index 0 unused */
    u32 mesh_count;
    A3TextureAsset *textures; /* index 0 unused */
    u32 texture_count;
    u32 primitives[A3_PRIM_COUNT];
    A3RhiTexture white, checker;
    b32 gpu;
    b32 ready;
} g_assets;

static b32 rhi_available(void) { return a3_rhi_info()->api[0] != 0; }

b32 a3_assets_init(const char *root) {
    if (g_assets.ready) return 1;
    g_assets.meshes = A3_NEW_ARRAY(A3MeshAsset, MAX_MESHES, A3_MEM_RESOURCE);
    g_assets.textures = A3_NEW_ARRAY(A3TextureAsset, MAX_TEXTURES, A3_MEM_RESOURCE);
    if (!g_assets.meshes || !g_assets.textures) { A3_ERROR("assets", "out of memory"); return 0; }
    g_assets.mesh_count = 1;
    g_assets.texture_count = 1;
    a3_assets_set_root(root);
    g_assets.ready = 1;
    a3_assets_gpu_ready();
    return 1;
}

void a3_assets_set_root(const char *root) {
    a3_strcpy(g_assets.root, sizeof(g_assets.root), root ? root : "");
    a3_path_normalize(g_assets.root);
}

const char *a3_assets_root(void) { return g_assets.root; }

void a3_assets_path(const char *rel, char *out, usize cap) {
    if (!rel) rel = "";
    if (rel[0] == '/' || !g_assets.root[0]) a3_strcpy(out, cap, rel);
    else a3_path_join(out, cap, g_assets.root, rel);
}

static void upload_mesh(A3MeshAsset *m) {
    if (!g_assets.gpu || m->gpu.id || !m->cpu.vertex_count) return;
    m->vb = a3_rhi_buffer_create(A3_BUFFER_VERTEX, sizeof(A3Vertex) * m->cpu.vertex_count, m->cpu.vertices, 0);
    m->ib = a3_rhi_buffer_create(A3_BUFFER_INDEX, sizeof(u32) * m->cpu.index_count, m->cpu.indices, 0);
    A3VertexLayout l;
    a3_zero_struct(&l);
    l.stride = sizeof(A3Vertex);
    l.attribs[0] = (A3VertexAttrib){ 0, 3, A3_ATTR_FLOAT, (u32)A3_OFFSETOF(A3Vertex, position), 0 };
    l.attribs[1] = (A3VertexAttrib){ 1, 3, A3_ATTR_FLOAT, (u32)A3_OFFSETOF(A3Vertex, normal), 0 };
    l.attribs[2] = (A3VertexAttrib){ 2, 2, A3_ATTR_FLOAT, (u32)A3_OFFSETOF(A3Vertex, uv), 0 };
    l.count = 3;
    m->gpu = a3_rhi_mesh_create(m->vb, &l, m->ib, 1);
}

static void release_mesh_gpu(A3MeshAsset *m) {
    if (m->gpu.id) a3_rhi_mesh_destroy(m->gpu);
    if (m->vb.id) a3_rhi_buffer_destroy(m->vb);
    if (m->ib.id) a3_rhi_buffer_destroy(m->ib);
    m->gpu.id = m->vb.id = m->ib.id = 0;
}

static void make_default_textures(void) {
    if (!g_assets.gpu || g_assets.white.id) return;
    u8 white[4 * 4 * 4];
    a3_memset(white, 255, sizeof(white));
    A3TextureDesc d;
    a3_zero_struct(&d);
    d.width = d.height = 4;
    d.format = A3_TEX_RGBA8;
    d.data = white;
    d.debug_name = "white";
    g_assets.white = a3_rhi_texture_create(&d);
    u8 checker[64 * 64 * 4];
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            b32 c = ((x / 8) + (y / 8)) & 1;
            u8 *p = checker + (y * 64 + x) * 4;
            p[0] = c ? 255 : 30; p[1] = c ? 0 : 30; p[2] = c ? 255 : 30; p[3] = 255;
        }
    d.width = d.height = 64;
    d.data = checker;
    d.filter = A3_FILTER_NEAREST;
    d.debug_name = "missing";
    g_assets.checker = a3_rhi_texture_create(&d);
}

void a3_assets_gpu_ready(void) {
    if (!g_assets.ready || g_assets.gpu || !rhi_available()) return;
    g_assets.gpu = 1;
    make_default_textures();
    for (u32 i = 1; i < g_assets.mesh_count; ++i) upload_mesh(&g_assets.meshes[i]);
}

void a3_assets_shutdown(void) {
    if (!g_assets.ready) return;
    for (u32 i = 1; i < g_assets.mesh_count; ++i) {
        release_mesh_gpu(&g_assets.meshes[i]);
        a3_mesh_free(&g_assets.meshes[i].cpu);
    }
    for (u32 i = 1; i < g_assets.texture_count; ++i) if (g_assets.textures[i].tex.id) a3_rhi_texture_destroy(g_assets.textures[i].tex);
    if (g_assets.white.id) a3_rhi_texture_destroy(g_assets.white);
    if (g_assets.checker.id) a3_rhi_texture_destroy(g_assets.checker);
    a3_free(g_assets.meshes);
    a3_free(g_assets.textures);
    a3_zero_struct(&g_assets);
}

/* ---- meshes ---- */

static u32 find_mesh(const char *path) {
    for (u32 i = 1; i < g_assets.mesh_count; ++i) if (a3_streq(g_assets.meshes[i].path, path)) return i;
    return 0;
}

static u32 new_mesh_slot(const char *path) {
    if (g_assets.mesh_count >= MAX_MESHES) { A3_ERROR("assets", "too many meshes loaded (max %d)", MAX_MESHES); return 0; }
    u32 id = g_assets.mesh_count++;
    A3MeshAsset *m = &g_assets.meshes[id];
    a3_zero_struct(m);
    a3_strcpy(m->path, sizeof(m->path), path);
    return id;
}

static void finish_mesh(A3MeshAsset *m) {
    m->index_count = m->cpu.index_count;
    m->bounds = m->cpu.bounds;
    m->radius = m->cpu.radius;
    m->state = A3_ASSET_STATE_LOADED;
    upload_mesh(m);
}

u32 a3_assets_mesh_from_data(const char *name, A3MeshData *data) {
    u32 id = find_mesh(name);
    if (id) {
        A3MeshAsset *m = &g_assets.meshes[id];
        release_mesh_gpu(m);
        a3_mesh_free(&m->cpu);
        m->version++;
    } else {
        id = new_mesh_slot(name);
        if (!id) { a3_mesh_free(data); return 0; }
    }
    A3MeshAsset *m = &g_assets.meshes[id];
    m->cpu = *data;
    a3_zero_struct(data);
    finish_mesh(m);
    return id;
}

u32 a3_assets_mesh_primitive(A3Primitive p) {
    if (p <= A3_PRIM_NONE || p >= A3_PRIM_COUNT) p = A3_PRIM_CUBE;
    if (g_assets.primitives[p]) return g_assets.primitives[p];
    static const char *names[A3_PRIM_COUNT] = { "", "builtin:cube", "builtin:sphere", "builtin:plane", "builtin:cylinder", "builtin:capsule", "builtin:cone" };
    A3MeshData md;
    b32 ok = 0;
    switch (p) {
    case A3_PRIM_CUBE: ok = a3_mesh_cube(&md); break;
    case A3_PRIM_SPHERE: ok = a3_mesh_sphere(&md, 32, 16); break;
    case A3_PRIM_PLANE: ok = a3_mesh_plane(&md, 1, 1.0f); break;
    case A3_PRIM_CYLINDER: ok = a3_mesh_cylinder(&md, 32); break;
    case A3_PRIM_CAPSULE: ok = a3_mesh_capsule(&md, 24, 12); break;
    case A3_PRIM_CONE: ok = a3_mesh_cone(&md, 32); break;
    default: break;
    }
    if (!ok) return 0;
    g_assets.primitives[p] = a3_assets_mesh_from_data(names[p], &md);
    return g_assets.primitives[p];
}

static A3MeshGenerator find_generator(const char *path) {
    for (u32 i = 0; i < g_mesh_gen_count; ++i) if (a3_streq(g_mesh_gens[i].path, path)) return g_mesh_gens[i].fn;
    return 0;
}

b32 a3_assets_register_mesh_generator(const char *path, A3MeshGenerator fn) {
    if (!path || !fn || a3_strlen(path) >= sizeof(g_mesh_gens[0].path)) return 0;
    for (u32 i = 0; i < g_mesh_gen_count; ++i)
        if (a3_streq(g_mesh_gens[i].path, path)) { g_mesh_gens[i].fn = fn; return 1; }
    if (g_mesh_gen_count >= MAX_MESH_GENERATORS) { A3_ERROR("assets", "too many mesh generators (max %d)", MAX_MESH_GENERATORS); return 0; }
    a3_strcpy(g_mesh_gens[g_mesh_gen_count].path, sizeof(g_mesh_gens[0].path), path);
    g_mesh_gens[g_mesh_gen_count++].fn = fn;
    return 1;
}

b32 a3_assets_has_mesh_generator(const char *path) { return path && find_generator(path) != 0; }

static b32 load_mesh_file(A3MeshAsset *m) {
    char full[1024];
    a3_assets_path(m->path, full, sizeof(full));
    A3FileData fd;
    A3Result r = a3_file_read_all(full, A3_MEM_TEMP, &fd);
    if (r != A3_OK) {
        a3_snprintf(m->error, sizeof(m->error), "file not found: %s", m->path);
        return 0;
    }
    const char *ext = a3_path_extension(m->path);
    b32 ok = 0;
    if (a3_streq(ext, ".obj") || a3_streq(ext, ".OBJ")) {
        char err[128] = "";
        ok = a3_mesh_load_obj((const char *)fd.data, fd.size, &m->cpu, err, sizeof(err)) == A3_OK;
        if (!ok) a3_snprintf(m->error, sizeof(m->error), "%s", err);
    } else {
        a3_snprintf(m->error, sizeof(m->error), "unsupported model format '%s' (supported: .obj)", ext);
    }
    a3_free(fd.data);
    return ok;
}

u32 a3_assets_mesh(const char *path) {
    if (!path || !*path) return 0;
    if (a3_str_starts_with(path, "builtin:")) {
        for (int p = 1; p < A3_PRIM_COUNT; ++p) {
            u32 id = a3_assets_mesh_primitive((A3Primitive)p);
            if (id && a3_streq(g_assets.meshes[id].path, path)) return id;
        }
    }
    u32 id = find_mesh(path);
    if (id) return id;
    A3MeshGenerator gen = find_generator(path);
    if (gen) {
        A3MeshData md;
        a3_zero_struct(&md);
        if (gen(&md)) return a3_assets_mesh_from_data(path, &md);
        a3_mesh_free(&md);
    }
    id = new_mesh_slot(path);
    if (!id) return 0;
    A3MeshAsset *m = &g_assets.meshes[id];
    if (gen) {
        m->state = A3_ASSET_STATE_FAILED;
        a3_snprintf(m->error, sizeof(m->error), "procedural mesh generator failed");
        A3_ERROR("assets", "mesh '%s': procedural generator failed, showing a cube", path);
    } else if (load_mesh_file(m)) {
        finish_mesh(m);
        A3_INFO("assets", "loaded mesh '%s' (%u vertices, %u triangles)", path, m->cpu.vertex_count, m->cpu.index_count / 3);
    } else {
        m->state = A3_ASSET_STATE_FAILED;
        a3_log_hint(A3_LOG_ERROR, "assets", "A model could not be loaded, so a placeholder cube is shown. Check that the file exists and is a valid .obj.",
                    "mesh '%s': %s", path, m->error);
    }
    return id;
}

const A3MeshAsset *a3_assets_mesh_get(u32 id) {
    if (!g_assets.ready || id == 0 || id >= g_assets.mesh_count) return 0;
    A3MeshAsset *m = &g_assets.meshes[id];
    if (m->state == A3_ASSET_STATE_FAILED) {
        u32 cube = a3_assets_mesh_primitive(A3_PRIM_CUBE);
        return cube && cube != id ? &g_assets.meshes[cube] : 0;
    }
    return m;
}

u32 a3_assets_mesh_count(void) { return g_assets.mesh_count ? g_assets.mesh_count - 1 : 0; }

/* ---- textures ---- */

static u32 find_texture(const char *path) {
    for (u32 i = 1; i < g_assets.texture_count; ++i) if (a3_streq(g_assets.textures[i].path, path)) return i;
    return 0;
}

static b32 load_texture_file(A3TextureAsset *t) {
    char full[1024], err[128] = "";
    a3_assets_path(t->path, full, sizeof(full));
    A3Image img;
    if (a3_image_load(full, &img, err, sizeof(err)) != A3_OK) {
        a3_snprintf(t->error, sizeof(t->error), "%s", err);
        return 0;
    }
    t->width = img.width;
    t->height = img.height;
    if (g_assets.gpu) {
        A3TextureDesc d;
        a3_zero_struct(&d);
        d.width = img.width;
        d.height = img.height;
        d.format = A3_TEX_SRGBA8; /* color textures are authored in sRGB */
        d.filter = A3_FILTER_TRILINEAR;
        d.wrap = A3_WRAP_REPEAT;
        d.mipmaps = 1;
        d.data = img.pixels;
        d.debug_name = t->path;
        t->tex = a3_rhi_texture_create(&d);
    }
    a3_image_free(&img);
    return !g_assets.gpu || t->tex.id != 0;
}

u32 a3_assets_texture(const char *path) {
    if (!path || !*path) return 0;
    u32 id = find_texture(path);
    if (id) return id;
    if (g_assets.texture_count >= MAX_TEXTURES) { A3_ERROR("assets", "too many textures"); return 0; }
    id = g_assets.texture_count++;
    A3TextureAsset *t = &g_assets.textures[id];
    a3_zero_struct(t);
    a3_strcpy(t->path, sizeof(t->path), path);
    if (load_texture_file(t)) {
        t->state = A3_ASSET_STATE_LOADED;
        A3_INFO("assets", "loaded texture '%s' (%dx%d)", path, t->width, t->height);
    } else {
        t->state = A3_ASSET_STATE_FAILED;
        a3_log_hint(A3_LOG_ERROR, "assets", "An image could not be loaded, so a pink checkerboard is shown instead. Supported formats: PNG, TGA, BMP.",
                    "texture '%s': %s", path, t->error);
    }
    return id;
}

u32 a3_assets_texture_from_pixels(const char *name, const u8 *rgba, i32 w, i32 h, b32 mipmaps) {
    u32 id = find_texture(name);
    if (!id) {
        if (g_assets.texture_count >= MAX_TEXTURES) return 0;
        id = g_assets.texture_count++;
        a3_zero_struct(&g_assets.textures[id]);
        a3_strcpy(g_assets.textures[id].path, A3_PATH_MAX, name);
    }
    A3TextureAsset *t = &g_assets.textures[id];
    if (t->tex.id) a3_rhi_texture_destroy(t->tex);
    t->tex.id = 0;
    t->width = w;
    t->height = h;
    if (g_assets.gpu) {
        A3TextureDesc d;
        a3_zero_struct(&d);
        d.width = w; d.height = h; d.format = A3_TEX_RGBA8;
        d.filter = mipmaps ? A3_FILTER_TRILINEAR : A3_FILTER_LINEAR;
        d.mipmaps = mipmaps;
        d.data = rgba;
        d.debug_name = name;
        t->tex = a3_rhi_texture_create(&d);
    }
    t->state = A3_ASSET_STATE_LOADED;
    t->version++;
    return id;
}

const A3TextureAsset *a3_assets_texture_get(u32 id) {
    if (!g_assets.ready || id == 0 || id >= g_assets.texture_count) return 0;
    return &g_assets.textures[id];
}

A3RhiTexture a3_assets_texture_rhi(u32 id) {
    const A3TextureAsset *t = a3_assets_texture_get(id);
    if (!t) return g_assets.white;
    if (t->state == A3_ASSET_STATE_FAILED || !t->tex.id) return g_assets.checker;
    return t->tex;
}

A3RhiTexture a3_assets_white_texture(void) { return g_assets.white; }
A3RhiTexture a3_assets_checker_texture(void) { return g_assets.checker; }
u32 a3_assets_texture_count(void) { return g_assets.texture_count ? g_assets.texture_count - 1 : 0; }

/* ---- references ---- */

u32 a3_assets_resolve_mesh(A3AssetRef *ref) {
    if (!ref || !ref->path[0]) return 0;
    if (ref->handle && ref->handle < g_assets.mesh_count && a3_streq(g_assets.meshes[ref->handle].path, ref->path)) return ref->handle;
    ref->handle = a3_assets_mesh(ref->path);
    return ref->handle;
}

u32 a3_assets_resolve_texture(A3AssetRef *ref) {
    if (!ref || !ref->path[0]) return 0;
    if (ref->handle && ref->handle < g_assets.texture_count && a3_streq(g_assets.textures[ref->handle].path, ref->path)) return ref->handle;
    ref->handle = a3_assets_texture(ref->path);
    return ref->handle;
}

u32 a3_assets_mesh_for_renderer(A3CMeshRenderer *mr) {
    if (!mr) return 0;
    if (mr->mesh.path[0]) {
        u32 id = a3_assets_resolve_mesh(&mr->mesh);
        if (id) return id;
    }
    if (mr->primitive == A3_PRIM_NONE) return mr->mesh.path[0] ? a3_assets_mesh_primitive(A3_PRIM_CUBE) : 0;
    return a3_assets_mesh_primitive((A3Primitive)mr->primitive);
}

b32 a3_assets_reload(const char *path) {
    u32 id = find_mesh(path);
    if (id) {
        A3MeshAsset *m = &g_assets.meshes[id];
        A3MeshData old = m->cpu;
        a3_zero_struct(&m->cpu);
        if (load_mesh_file(m)) {
            release_mesh_gpu(m);
            a3_mesh_free(&old);
            finish_mesh(m);
            m->version++;
            A3_INFO("assets", "reloaded mesh '%s'", path);
            return 1;
        }
        m->cpu = old; /* keep the previous version on failure */
        A3_ERROR("assets", "reload of '%s' failed: %s (keeping previous version)", path, m->error);
        return 0;
    }
    id = find_texture(path);
    if (id) {
        A3TextureAsset *t = &g_assets.textures[id];
        A3RhiTexture old = t->tex;
        t->tex.id = 0;
        if (load_texture_file(t)) {
            if (old.id) a3_rhi_texture_destroy(old);
            t->state = A3_ASSET_STATE_LOADED;
            t->version++;
            A3_INFO("assets", "reloaded texture '%s'", path);
            return 1;
        }
        t->tex = old;
        A3_ERROR("assets", "reload of '%s' failed: %s (keeping previous version)", path, t->error);
        return 0;
    }
    return 0;
}

u32 a3_assets_failed_count(void) {
    u32 n = 0;
    for (u32 i = 1; i < g_assets.mesh_count; ++i) n += g_assets.meshes[i].state == A3_ASSET_STATE_FAILED;
    for (u32 i = 1; i < g_assets.texture_count; ++i) n += g_assets.textures[i].state == A3_ASSET_STATE_FAILED;
    return n;
}
