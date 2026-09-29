/*
 * ASM3D - a3_modeling_ref.c
 * C reference versions of the modeling kernels. Every expression mirrors the
 * instruction order of a3_modeling_x64.S (built with -ffp-contract=off), so
 * the results are bit-identical.
 */
#include "a3_modeling_kernels.h"

void a3_mk_ref_transform(A3Vec4 *pos, const u8 *mask, u32 count, const f32 *m) {
    for (u32 i = 0; i < count; ++i) {
        if (mask && !mask[i]) continue;
        f32 x = pos[i].x, y = pos[i].y, z = pos[i].z;
        f32 r[3];
        for (u32 c = 0; c < 3; ++c) {
            f32 a = m[c] * x;
            f32 b = m[4 + c] * y;
            a = a + b;
            b = m[8 + c] * z;
            a = a + b;
            r[c] = a + m[12 + c];
        }
        pos[i].x = r[0];
        pos[i].y = r[1];
        pos[i].z = r[2];
    }
}

static A3Vec4 sub4(A3Vec4 a, A3Vec4 b) { A3Vec4 r = { a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w }; return r; }

static A3Vec4 cross4(A3Vec4 a, A3Vec4 b) {
    A3Vec4 r;
    f32 l, q;
    l = a.y * b.z; q = a.z * b.y; r.x = l - q;
    l = a.z * b.x; q = a.x * b.z; r.y = l - q;
    l = a.x * b.y; q = a.y * b.x; r.z = l - q;
    r.w = 0.0f;
    return r;
}

static f32 dot3(A3Vec4 a, A3Vec4 b) {
    f32 x = a.x * b.x, y = a.y * b.y, z = a.z * b.z;
    f32 s = x + y;
    return s + z;
}

void a3_mk_ref_tri_normals(const A3Vec4 *pos, const u32 *tris, u32 tri_count, A3Vec4 *out) {
    for (u32 i = 0; i < tri_count; ++i) {
        A3Vec4 a = pos[tris[i * 3]], b = pos[tris[i * 3 + 1]], c = pos[tris[i * 3 + 2]];
        out[i] = cross4(sub4(b, a), sub4(c, a));
    }
}

u32 a3_mk_ref_raycast(const A3Vec4 *pos, const u32 *tris, u32 tri_count, const f32 *ray, f32 *t_inout) {
    A3Vec4 o = { ray[0], ray[1], ray[2], ray[3] }, d = { ray[4], ray[5], ray[6], ray[7] };
    f32 best = *t_inout;
    u32 hit = 0;
    for (u32 i = 0; i < tri_count; ++i) {
        A3Vec4 a = pos[tris[i * 3]], b = pos[tris[i * 3 + 1]], c = pos[tris[i * 3 + 2]];
        A3Vec4 e1 = sub4(b, a), e2 = sub4(c, a);
        A3Vec4 p = cross4(d, e2);
        f32 det = dot3(e1, p);
        f32 adet = det < 0.0f ? -det : det;
        if (!(adet >= 1e-12f)) continue;
        f32 inv = 1.0f / det;
        A3Vec4 s = sub4(o, a);
        f32 u = dot3(s, p) * inv;
        if (!(u >= 0.0f) || u > 1.0f) continue;
        A3Vec4 q = cross4(s, e1);
        f32 v = dot3(d, q) * inv;
        if (!(v >= 0.0f)) continue;
        f32 uv = u + v;
        if (uv > 1.0f) continue;
        f32 t = dot3(e2, q) * inv;
        if (t > 1e-6f && best > t) { best = t; hit = i + 1; }
    }
    *t_inout = best;
    return hit;
}

void a3_mk_ref_bounds(const A3Vec4 *pos, u32 count, A3Vec4 *out_min, A3Vec4 *out_max) {
    A3Vec4 mn = pos[0], mx = pos[0];
    for (u32 i = 1; i < count; ++i) {
        const A3Vec4 p = pos[i];
        mn.x = mn.x < p.x ? mn.x : p.x; mx.x = mx.x > p.x ? mx.x : p.x;
        mn.y = mn.y < p.y ? mn.y : p.y; mx.y = mx.y > p.y ? mx.y : p.y;
        mn.z = mn.z < p.z ? mn.z : p.z; mx.z = mx.z > p.z ? mx.z : p.z;
        mn.w = mn.w < p.w ? mn.w : p.w; mx.w = mx.w > p.w ? mx.w : p.w;
    }
    *out_min = mn;
    *out_max = mx;
}

#if A3_HAS_X64_ASM
void a3_mk_asm_transform(A3Vec4 *pos, const u8 *mask, u32 count, const f32 *m16);
void a3_mk_asm_tri_normals(const A3Vec4 *pos, const u32 *tris, u32 tri_count, A3Vec4 *out);
u32  a3_mk_asm_raycast(const A3Vec4 *pos, const u32 *tris, u32 tri_count, const f32 *ray, f32 *t_inout);
void a3_mk_asm_bounds(const A3Vec4 *pos, u32 count, A3Vec4 *out_min, A3Vec4 *out_max);
void a3_mk_transform(A3Vec4 *p, const u8 *m, u32 n, const f32 *m16) { if (n) a3_mk_asm_transform(p, m, n, m16); }
void a3_mk_tri_normals(const A3Vec4 *p, const u32 *t, u32 n, A3Vec4 *o) { if (n) a3_mk_asm_tri_normals(p, t, n, o); }
u32  a3_mk_raycast(const A3Vec4 *p, const u32 *t, u32 n, const f32 *r, f32 *ti) { return n ? a3_mk_asm_raycast(p, t, n, r, ti) : 0; }
void a3_mk_bounds(const A3Vec4 *p, u32 n, A3Vec4 *mn, A3Vec4 *mx) { if (n) a3_mk_asm_bounds(p, n, mn, mx); }
const char *a3_mk_backend(void) { return "x86-64 SSE assembly"; }
#else
void a3_mk_transform(A3Vec4 *p, const u8 *m, u32 n, const f32 *m16) { a3_mk_ref_transform(p, m, n, m16); }
void a3_mk_tri_normals(const A3Vec4 *p, const u32 *t, u32 n, A3Vec4 *o) { a3_mk_ref_tri_normals(p, t, n, o); }
u32  a3_mk_raycast(const A3Vec4 *p, const u32 *t, u32 n, const f32 *r, f32 *ti) { return a3_mk_ref_raycast(p, t, n, r, ti); }
void a3_mk_bounds(const A3Vec4 *p, u32 n, A3Vec4 *mn, A3Vec4 *mx) { if (n) a3_mk_ref_bounds(p, n, mn, mx); }
const char *a3_mk_backend(void) { return "C reference"; }
#endif
