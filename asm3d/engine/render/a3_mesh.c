/*
 * ASM3D - a3_mesh.c
 */
#include "a3_mesh.h"
#include "../core/a3_memory.h"
#include "../core/a3_string.h"
#include "../core/a3_format.h"
#include "../core/a3_log.h"
#include "../core/a3_hash.h"

void a3_mesh_free(A3MeshData *m) {
    if (!m) return;
    a3_free(m->vertices);
    a3_free(m->indices);
    a3_zero_struct(m);
}

b32 a3_mesh_alloc(A3MeshData *m, u32 vc, u32 ic) {
    a3_zero_struct(m);
    m->vertices = A3_NEW_ARRAY(A3Vertex, vc ? vc : 1, A3_MEM_RESOURCE);
    m->indices = A3_NEW_ARRAY(u32, ic ? ic : 1, A3_MEM_RESOURCE);
    if (!m->vertices || !m->indices) { a3_mesh_free(m); return 0; }
    m->vertex_count = vc;
    m->index_count = ic;
    return 1;
}

void a3_mesh_compute_bounds(A3MeshData *m) {
    A3Aabb b = a3_aabb_empty();
    for (u32 i = 0; i < m->vertex_count; ++i) b = a3_aabb_expand(b, m->vertices[i].position);
    if (!m->vertex_count) b = a3_aabb(a3_v3_zero(), a3_v3_zero());
    m->bounds = b;
    A3Vec3 c = a3_aabb_center(b);
    f32 r2 = 0;
    for (u32 i = 0; i < m->vertex_count; ++i) r2 = a3_maxf(r2, a3_v3_dist_sq(c, m->vertices[i].position));
    m->radius = a3_sqrtf(r2);
}

void a3_mesh_compute_normals(A3MeshData *m) {
    for (u32 i = 0; i < m->vertex_count; ++i) m->vertices[i].normal = a3_v3_zero();
    for (u32 i = 0; i + 2 < m->index_count; i += 3) {
        u32 a = m->indices[i], b = m->indices[i + 1], c = m->indices[i + 2];
        if (a >= m->vertex_count || b >= m->vertex_count || c >= m->vertex_count) continue;
        A3Vec3 n = a3_v3_cross(a3_v3_sub(m->vertices[b].position, m->vertices[a].position),
                               a3_v3_sub(m->vertices[c].position, m->vertices[a].position));
        m->vertices[a].normal = a3_v3_add(m->vertices[a].normal, n);
        m->vertices[b].normal = a3_v3_add(m->vertices[b].normal, n);
        m->vertices[c].normal = a3_v3_add(m->vertices[c].normal, n);
    }
    for (u32 i = 0; i < m->vertex_count; ++i) m->vertices[i].normal = a3_v3_norm(m->vertices[i].normal);
}

static A3Vertex vtx(A3Vec3 p, A3Vec3 n, A3Vec2 uv) { A3Vertex v = { p, n, uv }; return v; }

b32 a3_mesh_cube(A3MeshData *m) {
    if (!a3_mesh_alloc(m, 24, 36)) return 0;
    static const f32 N[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    static const f32 U[6][3] = { { 0, 0, -1 }, { 0, 0, 1 }, { 1, 0, 0 }, { 1, 0, 0 }, { 1, 0, 0 }, { -1, 0, 0 } };
    for (u32 f = 0; f < 6; ++f) {
        A3Vec3 n = a3_v3(N[f][0], N[f][1], N[f][2]);
        A3Vec3 u = a3_v3(U[f][0], U[f][1], U[f][2]);
        A3Vec3 v = a3_v3_cross(n, u);
        for (u32 k = 0; k < 4; ++k) {
            f32 su = (k == 1 || k == 2) ? 0.5f : -0.5f;
            f32 sv = (k >= 2) ? 0.5f : -0.5f;
            A3Vec3 p = a3_v3_add(a3_v3_scale(n, 0.5f), a3_v3_add(a3_v3_scale(u, su), a3_v3_scale(v, sv)));
            m->vertices[f * 4 + k] = vtx(p, n, a3_v2(su + 0.5f, 0.5f - sv));
        }
        u32 b = f * 4;
        u32 *ix = m->indices + f * 6;
        ix[0] = b; ix[1] = b + 1; ix[2] = b + 2; ix[3] = b; ix[4] = b + 2; ix[5] = b + 3;
    }
    a3_mesh_compute_bounds(m);
    return 1;
}

b32 a3_mesh_sphere(A3MeshData *m, u32 seg, u32 rings) {
    if (seg < 3) seg = 3;
    if (rings < 2) rings = 2;
    if (!a3_mesh_alloc(m, (seg + 1) * (rings + 1), seg * rings * 6)) return 0;
    u32 vi = 0, ii = 0;
    for (u32 r = 0; r <= rings; ++r) {
        f32 phi = A3_PI * (f32)r / (f32)rings;
        f32 sp, cp;
        a3_sincosf(phi, &sp, &cp);
        for (u32 s = 0; s <= seg; ++s) {
            f32 th = A3_TAU * (f32)s / (f32)seg;
            f32 st, ct;
            a3_sincosf(th, &st, &ct);
            A3Vec3 n = a3_v3(sp * ct, cp, -sp * st);
            m->vertices[vi++] = vtx(a3_v3_scale(n, 0.5f), n, a3_v2((f32)s / seg, (f32)r / rings));
        }
    }
    for (u32 r = 0; r < rings; ++r)
        for (u32 s = 0; s < seg; ++s) {
            u32 a = r * (seg + 1) + s, b = a + seg + 1;
            m->indices[ii++] = a; m->indices[ii++] = b; m->indices[ii++] = a + 1;
            m->indices[ii++] = a + 1; m->indices[ii++] = b; m->indices[ii++] = b + 1;
        }
    a3_mesh_compute_bounds(m);
    return 1;
}

b32 a3_mesh_plane(A3MeshData *m, u32 sub, f32 uv_scale) {
    if (sub < 1) sub = 1;
    if (uv_scale <= 0) uv_scale = 1;
    if (!a3_mesh_alloc(m, (sub + 1) * (sub + 1), sub * sub * 6)) return 0;
    u32 vi = 0, ii = 0;
    for (u32 z = 0; z <= sub; ++z)
        for (u32 x = 0; x <= sub; ++x) {
            f32 fx = (f32)x / sub, fz = (f32)z / sub;
            m->vertices[vi++] = vtx(a3_v3(fx - 0.5f, 0, fz - 0.5f), a3_v3(0, 1, 0), a3_v2(fx * uv_scale, fz * uv_scale));
        }
    for (u32 z = 0; z < sub; ++z)
        for (u32 x = 0; x < sub; ++x) {
            u32 a = z * (sub + 1) + x, b = a + sub + 1;
            m->indices[ii++] = a; m->indices[ii++] = b; m->indices[ii++] = a + 1;
            m->indices[ii++] = a + 1; m->indices[ii++] = b; m->indices[ii++] = b + 1;
        }
    a3_mesh_compute_bounds(m);
    return 1;
}

/* Cylinder/cone share a builder: radius_top 0 gives a cone. Height 1. */
static b32 build_round(A3MeshData *m, u32 seg, f32 r_top, f32 r_bot) {
    if (seg < 3) seg = 3;
    u32 vc = (seg + 1) * 2 + (seg + 2) * 2;
    u32 ic = seg * 6 + seg * 3 * 2;
    if (!a3_mesh_alloc(m, vc, ic)) return 0;
    u32 vi = 0, ii = 0;
    f32 slope = (r_bot - r_top);
    for (u32 s = 0; s <= seg; ++s) {
        f32 th = A3_TAU * (f32)s / seg, st, ct;
        a3_sincosf(th, &st, &ct);
        A3Vec3 n = a3_v3_norm(a3_v3(ct, slope, -st));
        m->vertices[vi++] = vtx(a3_v3(ct * r_bot, -0.5f, -st * r_bot), n, a3_v2((f32)s / seg, 1));
        m->vertices[vi++] = vtx(a3_v3(ct * r_top, 0.5f, -st * r_top), n, a3_v2((f32)s / seg, 0));
    }
    for (u32 s = 0; s < seg; ++s) {
        u32 a = s * 2;
        m->indices[ii++] = a; m->indices[ii++] = a + 2; m->indices[ii++] = a + 1;
        m->indices[ii++] = a + 1; m->indices[ii++] = a + 2; m->indices[ii++] = a + 3;
    }
    for (int cap = 0; cap < 2; ++cap) {
        f32 y = cap ? 0.5f : -0.5f, r = cap ? r_top : r_bot;
        A3Vec3 n = a3_v3(0, cap ? 1.0f : -1.0f, 0);
        u32 center = vi;
        m->vertices[vi++] = vtx(a3_v3(0, y, 0), n, a3_v2(0.5f, 0.5f));
        for (u32 s = 0; s <= seg; ++s) {
            f32 th = A3_TAU * (f32)s / seg, st, ct;
            a3_sincosf(th, &st, &ct);
            m->vertices[vi++] = vtx(a3_v3(ct * r, y, -st * r), n, a3_v2(0.5f + ct * 0.5f, 0.5f + st * 0.5f));
        }
        for (u32 s = 0; s < seg; ++s) {
            if (cap) { m->indices[ii++] = center; m->indices[ii++] = center + 1 + s; m->indices[ii++] = center + 2 + s; }
            else { m->indices[ii++] = center; m->indices[ii++] = center + 2 + s; m->indices[ii++] = center + 1 + s; }
        }
    }
    m->vertex_count = vi;
    m->index_count = ii;
    a3_mesh_compute_bounds(m);
    return 1;
}

b32 a3_mesh_cylinder(A3MeshData *m, u32 seg) { return build_round(m, seg, 0.5f, 0.5f); }
b32 a3_mesh_cone(A3MeshData *m, u32 seg) { return build_round(m, seg, 0.0f, 0.5f); }

b32 a3_mesh_capsule(A3MeshData *m, u32 seg, u32 rings) {
    if (seg < 3) seg = 3;
    if (rings < 2) rings = 2;
    if (rings & 1) rings++;
    u32 rows = rings + 1; /* hemisphere rows incl. equator duplicated */
    if (!a3_mesh_alloc(m, (seg + 1) * (rows + 1), seg * rows * 6)) return 0;
    u32 vi = 0, ii = 0;
    const f32 radius = 0.5f, half = 0.5f; /* cylinder half-height; total height 2 */
    for (u32 r = 0; r <= rows; ++r) {
        u32 rr = r <= rings / 2 ? r : r - 1;
        f32 phi = A3_PI * (f32)rr / (f32)rings;
        f32 sp, cp;
        a3_sincosf(phi, &sp, &cp);
        f32 yoff = r <= rings / 2 ? half : -half;
        for (u32 s = 0; s <= seg; ++s) {
            f32 th = A3_TAU * (f32)s / seg, st, ct;
            a3_sincosf(th, &st, &ct);
            A3Vec3 n = a3_v3(sp * ct, cp, -sp * st);
            A3Vec3 p = a3_v3(n.x * radius, n.y * radius + yoff, n.z * radius);
            m->vertices[vi++] = vtx(p, n, a3_v2((f32)s / seg, (1.0f - (p.y + 1.0f) * 0.5f)));
        }
    }
    for (u32 r = 0; r < rows; ++r)
        for (u32 s = 0; s < seg; ++s) {
            u32 a = r * (seg + 1) + s, b = a + seg + 1;
            m->indices[ii++] = a; m->indices[ii++] = b; m->indices[ii++] = a + 1;
            m->indices[ii++] = a + 1; m->indices[ii++] = b; m->indices[ii++] = b + 1;
        }
    a3_mesh_compute_bounds(m);
    return 1;
}

b32 a3_mesh_heightfield(A3MeshData *m, const f32 *h, u32 res, f32 size, f32 uv_scale) {
    if (!h || res < 1) return 0;
    if (!a3_mesh_alloc(m, (res + 1) * (res + 1), res * res * 6)) return 0;
    f32 cell = size / (f32)res;
    u32 vi = 0, ii = 0;
    for (u32 z = 0; z <= res; ++z)
        for (u32 x = 0; x <= res; ++x) {
            f32 y = h[z * (res + 1) + x];
            f32 hl = h[z * (res + 1) + (x > 0 ? x - 1 : x)], hr = h[z * (res + 1) + (x < res ? x + 1 : x)];
            f32 hd = h[(z > 0 ? z - 1 : z) * (res + 1) + x], hu = h[(z < res ? z + 1 : z) * (res + 1) + x];
            f32 dx = (x > 0 && x < res) ? 2.0f * cell : cell, dz = (z > 0 && z < res) ? 2.0f * cell : cell;
            A3Vec3 n = a3_v3_norm(a3_v3(-(hr - hl) / dx, 1.0f, -(hu - hd) / dz));
            m->vertices[vi++] = vtx(a3_v3(x * cell, y, z * cell), n, a3_v2((f32)x / res * uv_scale, (f32)z / res * uv_scale));
        }
    for (u32 z = 0; z < res; ++z)
        for (u32 x = 0; x < res; ++x) {
            u32 a = z * (res + 1) + x, b = a + res + 1;
            m->indices[ii++] = a; m->indices[ii++] = b; m->indices[ii++] = a + 1;
            m->indices[ii++] = a + 1; m->indices[ii++] = b; m->indices[ii++] = b + 1;
        }
    a3_mesh_compute_bounds(m);
    return 1;
}

/* ---- OBJ ---- */

typedef A3_ARRAY_TYPE(A3Vec3) Vec3Arr;
typedef A3_ARRAY_TYPE(A3Vec2) Vec2Arr;
typedef A3_ARRAY_TYPE(A3Vertex) VertArr;
typedef A3_ARRAY_TYPE(u32) U32Arr;

static const char *skip_ws(const char *p, const char *e) { while (p < e && (*p == ' ' || *p == '\t')) ++p; return p; }

static const char *parse_float(const char *p, const char *e, f32 *out) {
    p = skip_ws(p, e);
    const char *s = p;
    while (p < e && (a3_is_digit(*p) || *p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E')) ++p;
    f64 v = 0;
    if (p == s || !a3_parse_f64(s, (usize)(p - s), &v)) return 0;
    *out = (f32)v;
    return p;
}

static const char *parse_int(const char *p, const char *e, i32 *out, b32 *present) {
    const char *s = p;
    if (p < e && *p == '-') ++p;
    while (p < e && a3_is_digit(*p)) ++p;
    *present = p > s && !(p == s + 1 && *s == '-');
    i64 v = 0;
    if (*present && !a3_parse_i64(s, (usize)(p - s), &v)) *present = 0;
    *out = (i32)v;
    return p;
}

A3Result a3_mesh_load_obj(const char *text, usize len, A3MeshData *out, char *err, usize ecap) {
    a3_zero_struct(out);
    Vec3Arr pos = { 0 }, nrm = { 0 };
    Vec2Arr uvs = { 0 };
    VertArr verts = { 0 };
    U32Arr idx = { 0 };
    A3HashMap dedup;
    a3_hashmap_init(&dedup, 1024, A3_MEM_TEMP);
    b32 any_normals = 1;
    const char *p = text, *end = text + len;
    int line_no = 0;
    A3Result result = A3_OK;
    while (p < end) {
        const char *eol = p;
        while (eol < end && *eol != '\n') ++eol;
        ++line_no;
        const char *q = skip_ws(p, eol);
        if (q + 1 < eol && q[0] == 'v' && (q[1] == ' ' || q[1] == '\t')) {
            A3Vec3 v;
            if (!(q = parse_float(q + 1, eol, &v.x)) || !(q = parse_float(q, eol, &v.y)) || !(q = parse_float(q, eol, &v.z))) {
                a3_snprintf(err, ecap, "line %d: malformed vertex position", line_no); result = A3_ERR_PARSE; break;
            }
            a3_array_push(pos, v, A3_MEM_TEMP);
        } else if (q + 2 < eol && q[0] == 'v' && q[1] == 'n') {
            A3Vec3 v;
            if (!(q = parse_float(q + 2, eol, &v.x)) || !(q = parse_float(q, eol, &v.y)) || !(q = parse_float(q, eol, &v.z))) {
                a3_snprintf(err, ecap, "line %d: malformed normal", line_no); result = A3_ERR_PARSE; break;
            }
            a3_array_push(nrm, a3_v3_norm(v), A3_MEM_TEMP);
        } else if (q + 2 < eol && q[0] == 'v' && q[1] == 't') {
            A3Vec2 v = { 0, 0 };
            if (!(q = parse_float(q + 2, eol, &v.x))) { a3_snprintf(err, ecap, "line %d: malformed uv", line_no); result = A3_ERR_PARSE; break; }
            parse_float(q, eol, &v.y);
            v.y = 1.0f - v.y; /* OBJ uv origin is bottom-left */
            a3_array_push(uvs, v, A3_MEM_TEMP);
        } else if (q + 1 < eol && q[0] == 'f' && (q[1] == ' ' || q[1] == '\t')) {
            u32 face[64];
            u32 fc = 0;
            q += 1;
            for (;;) {
                q = skip_ws(q, eol);
                if (q >= eol || *q == '\r' || *q == '#') break;
                i32 vi = 0, ti = 0, ni = 0;
                b32 hv = 0, ht = 0, hn = 0;
                q = parse_int(q, eol, &vi, &hv);
                if (q < eol && *q == '/') { ++q; q = parse_int(q, eol, &ti, &ht); if (q < eol && *q == '/') { ++q; q = parse_int(q, eol, &ni, &hn); } }
                if (!hv) { a3_snprintf(err, ecap, "line %d: malformed face", line_no); result = A3_ERR_PARSE; break; }
                if (vi < 0) vi = (i32)pos.count + vi + 1;
                if (ti < 0) ti = (i32)uvs.count + ti + 1;
                if (ni < 0) ni = (i32)nrm.count + ni + 1;
                if (vi < 1 || (u32)vi > pos.count || (ht && (ti < 1 || (u32)ti > uvs.count)) || (hn && (ni < 1 || (u32)ni > nrm.count))) {
                    a3_snprintf(err, ecap, "line %d: face refers to a vertex that does not exist", line_no); result = A3_ERR_PARSE; break;
                }
                if (!hn) any_normals = 0;
                u64 key = a3_hash_mix64(((u64)(u32)vi) | ((u64)(u32)ti << 21) | ((u64)(u32)ni << 42)) | 1;
                u64 found;
                u32 index;
                if (a3_hashmap_get(&dedup, key, &found)) index = (u32)found;
                else {
                    A3Vertex v;
                    v.position = pos.data[vi - 1];
                    v.uv = ht ? uvs.data[ti - 1] : a3_v2(0, 0);
                    v.normal = hn ? nrm.data[ni - 1] : a3_v3(0, 1, 0);
                    index = verts.count;
                    if (!a3_array_push(verts, v, A3_MEM_TEMP)) { result = A3_ERR_OUT_OF_MEMORY; break; }
                    a3_hashmap_put(&dedup, key, index);
                }
                if (fc < 64) face[fc++] = index;
            }
            if (result != A3_OK) break;
            for (u32 k = 1; k + 1 < fc; ++k) { /* fan triangulation */
                a3_array_push(idx, face[0], A3_MEM_TEMP);
                a3_array_push(idx, face[k], A3_MEM_TEMP);
                a3_array_push(idx, face[k + 1], A3_MEM_TEMP);
            }
        }
        p = eol + 1;
    }
    if (result == A3_OK && (verts.count == 0 || idx.count == 0)) {
        a3_snprintf(err, ecap, "the OBJ file contains no faces");
        result = A3_ERR_PARSE;
    }
    if (result == A3_OK && a3_mesh_alloc(out, verts.count, idx.count)) {
        a3_memcpy(out->vertices, verts.data, sizeof(A3Vertex) * verts.count);
        a3_memcpy(out->indices, idx.data, sizeof(u32) * idx.count);
        if (!any_normals) a3_mesh_compute_normals(out);
        a3_mesh_compute_bounds(out);
    } else if (result == A3_OK) {
        result = A3_ERR_OUT_OF_MEMORY;
    }
    a3_array_free(pos); a3_array_free(nrm); a3_array_free(uvs); a3_array_free(verts); a3_array_free(idx);
    a3_hashmap_free(&dedup);
    return result;
}
