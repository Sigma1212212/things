/*
 * ASM3D - a3_geom_ref.c
 * C reference versions of the geometry kernels. Every expression mirrors the
 * instruction order of a3_geom_x64.S (built with -ffp-contract=off), so the
 * results are bit-identical.
 */
#include "a3_geom_kernels.h"

static A3Vec2 cr_eval(A3Vec2 p0, A3Vec2 p1, A3Vec2 p2, A3Vec2 p3, f32 t) {
    f32 t2 = t * t, t3 = t2 * t;
    A3Vec2 r;
    f32 *rr = &r.x;
    const f32 *q0 = &p0.x, *q1 = &p1.x, *q2 = &p2.x, *q3 = &p3.x;
    for (int c = 0; c < 2; ++c) {
        f32 a = q1[c] * 2.0f;
        f32 b = q2[c] - q0[c];
        f32 cc = q0[c] * 2.0f;
        f32 tmp = q1[c] * 5.0f;
        cc = cc - tmp;
        tmp = q2[c] * 4.0f;
        cc = cc + tmp;
        cc = cc - q3[c];
        f32 d = q1[c] * 3.0f;
        d = d - q0[c];
        tmp = q2[c] * 3.0f;
        d = d - tmp;
        d = d + q3[c];
        f32 v = b * t;
        v = a + v;
        tmp = cc * t2;
        v = v + tmp;
        tmp = d * t3;
        v = v + tmp;
        rr[c] = v * 0.5f;
    }
    return r;
}

u32 a3_gk_ref_catmull_rom(const A3Vec2 *p, u32 n, u32 sub, u32 closed, A3Vec2 *out) {
    if (n < 2 || sub == 0) return 0;
    u32 segs = closed ? n : n - 1, w = 0;
    for (u32 i = 0; i < segs; ++i) {
        u32 i0, i2, i3;
        if (closed) { i0 = (i + n - 1) % n; i2 = (i + 1) % n; i3 = (i + 2) % n; }
        else { i0 = i ? i - 1 : 0; i2 = i + 1; i3 = i + 2 < n ? i + 2 : n - 1; }
        for (u32 k = 0; k < sub; ++k) {
            f32 t = (f32)k / (f32)sub;
            out[w++] = cr_eval(p[i0], p[i], p[i2], p[i3], t);
        }
    }
    if (!closed) out[w++] = p[n - 1];
    return w;
}

void a3_gk_ref_frame_points(const A3Vec2 *pts, u32 count, const f32 *f, void *dst, u32 stride) {
    u8 *d = (u8 *)dst;
    for (u32 i = 0; i < count; ++i) {
        f32 x = pts[i].x, y = pts[i].y;
        f32 *o = (f32 *)(void *)(d + (usize)i * stride);
        for (int c = 0; c < 3; ++c) {
            f32 v = f[4 + c] * x;
            v = f[c] + v;
            f32 w = f[8 + c] * y;
            o[c] = v + w;
        }
    }
}

u32 a3_gk_ref_grid_indices(u32 rows, u32 k, u32 wrap, u32 base, u32 *out) {
    u32 w = 0, kk = wrap ? k : k - 1;
    if (rows < 2 || k < 2) return 0;
    for (u32 r = 0; r + 1 < rows; ++r)
        for (u32 j = 0; j < kk; ++j) {
            u32 j1 = j + 1 == k ? 0 : j + 1;
            u32 a = base + r * k + j, b = base + r * k + j1, c = base + (r + 1) * k + j1, d = base + (r + 1) * k + j;
            out[w++] = a; out[w++] = b; out[w++] = c;
            out[w++] = a; out[w++] = c; out[w++] = d;
        }
    return w;
}

static void cross3(const f32 *a, const f32 *b, f32 *r) {
    f32 l, q;
    l = a[1] * b[2]; q = a[2] * b[1]; r[0] = l - q;
    l = a[2] * b[0]; q = a[0] * b[2]; r[1] = l - q;
    l = a[0] * b[1]; q = a[1] * b[0]; r[2] = l - q;
}

void a3_gk_ref_vertex_normals(A3Vertex *v, const u32 *idx, u32 tri_count) {
    for (u32 t = 0; t < tri_count; ++t) {
        u32 ia = idx[t * 3], ib = idx[t * 3 + 1], ic = idx[t * 3 + 2];
        const f32 *a = &v[ia].position.x, *b = &v[ib].position.x, *c = &v[ic].position.x;
        f32 e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
        f32 e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
        f32 n[3];
        cross3(e1, e2, n);
        u32 ids[3] = { ia, ib, ic };
        for (int k = 0; k < 3; ++k) {
            f32 *d = &v[ids[k]].normal.x;
            d[0] = d[0] + n[0];
            d[1] = d[1] + n[1];
            d[2] = d[2] + n[2];
        }
    }
}

void a3_gk_ref_normalize(A3Vertex *v, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        f32 *n = &v[i].normal.x;
        f32 x = n[0] * n[0], y = n[1] * n[1], z = n[2] * n[2];
        f32 l2 = x + y;
        l2 = l2 + z;
        if (l2 > 1e-12f) {
            f32 s = __builtin_sqrtf(l2);
            n[0] = n[0] / s;
            n[1] = n[1] / s;
            n[2] = n[2] / s;
        } else {
            n[0] = 0.0f; n[1] = 1.0f; n[2] = 0.0f;
        }
    }
}

f32 a3_gk_ref_signed_volume(const A3Vertex *v, const u32 *idx, u32 tri_count) {
    f32 sum = 0.0f;
    for (u32 t = 0; t < tri_count; ++t) {
        const f32 *a = &v[idx[t * 3]].position.x, *b = &v[idx[t * 3 + 1]].position.x, *c = &v[idx[t * 3 + 2]].position.x;
        f32 bc[3];
        cross3(b, c, bc);
        f32 x = a[0] * bc[0], y = a[1] * bc[1], z = a[2] * bc[2];
        f32 d = x + y;
        d = d + z;
        sum = sum + d;
    }
    return sum;
}

/* ---- walk pose ---- */
#define GK_INV_2PI 0.159154943f
#define GK_2PI 6.28318548f
#define GK_PI 3.14159274f
#define GK_HALF_PI 1.57079637f

static f32 gk_sin(f32 x) {
    f32 q = x * GK_INV_2PI;
    q = q + (q < 0.0f ? -0.5f : 0.5f);
    f32 k = (f32)(i32)q;
    f32 m = k * GK_2PI;
    f32 r = x - m;
    if (r > GK_HALF_PI) r = GK_PI - r;
    if (r < -GK_HALF_PI) r = -GK_PI - r;
    f32 s = r * r;
    f32 p = s * -2.50521084e-8f;
    p = p + 2.75573192e-6f;
    p = p * s;
    p = p + -1.98412698e-4f;
    p = p * s;
    p = p + 8.33333333e-3f;
    p = p * s;
    p = p + -1.66666667e-1f;
    p = p * s;
    p = p + 1.0f;
    return p * r;
}

void a3_gk_ref_sin4(const f32 *in4, f32 *out4) { for (int i = 0; i < 4; ++i) out4[i] = gk_sin(in4[i]); }

void a3_gk_ref_walk_pose(const f32 *phase, const f32 *amp, u32 n, f32 *out) {
    for (u32 i = 0; i < n; ++i) {
        f32 ph = phase[i], a = amp[i];
        f32 s1 = gk_sin(ph + 0.0f), s2 = gk_sin(ph + 0.6f), c = gk_sin(ph + GK_HALF_PI);
        f32 *o = out + i * 10;
        f32 t = a * 0.5f;
        o[0] = t * s1;
        o[1] = -o[0];
        f32 m2 = s2 > 0.0f ? s2 : 0.0f;
        f32 ns2 = -s2;
        f32 n2 = ns2 > 0.0f ? ns2 : 0.0f;
        t = a * 0.65f;
        f32 u = t * m2;
        u = u + 0.08f;
        o[2] = -u;
        u = t * n2;
        u = u + 0.08f;
        o[3] = -u;
        t = a * 0.42f;
        u = t * s1;
        o[4] = -u;
        o[5] = u;
        t = a * 0.15f;
        u = s1 + 1.0f;
        u = t * u;
        o[6] = u + 0.2f;
        u = 1.0f - s1;
        u = t * u;
        o[7] = u + 0.2f;
        t = a * 0.035f;
        u = __builtin_fabsf(c);
        o[8] = t * u;
        t = a * 0.08f;
        o[9] = t * s1;
    }
}

#if A3_HAS_X64_ASM
u32  a3_gk_asm_catmull_rom(const A3Vec2 *ctrl, u32 n, u32 sub, u32 closed, A3Vec2 *out);
void a3_gk_asm_frame_points(const A3Vec2 *pts, u32 count, const f32 *frame, void *dst, u32 stride);
u32  a3_gk_asm_grid_indices(u32 rows, u32 k, u32 wrap, u32 base, u32 *out);
void a3_gk_asm_vertex_normals(A3Vertex *v, const u32 *idx, u32 tri_count);
void a3_gk_asm_normalize(A3Vertex *v, u32 count);
f32  a3_gk_asm_signed_volume(const A3Vertex *v, const u32 *idx, u32 tri_count);
void a3_gk_asm_walk_pose(const f32 *phase, const f32 *amp, u32 n, f32 *out);
void a3_gk_walk_pose(const f32 *p, const f32 *a, u32 n, f32 *o) { if (n) a3_gk_asm_walk_pose(p, a, n, o); }
u32  a3_gk_catmull_rom(const A3Vec2 *c, u32 n, u32 s, u32 cl, A3Vec2 *o) { return (n < 2 || !s) ? 0 : a3_gk_asm_catmull_rom(c, n, s, cl, o); }
void a3_gk_frame_points(const A3Vec2 *p, u32 n, const f32 *f, void *d, u32 st) { if (n) a3_gk_asm_frame_points(p, n, f, d, st); }
u32  a3_gk_grid_indices(u32 r, u32 k, u32 w, u32 b, u32 *o) { return (r < 2 || k < 2) ? 0 : a3_gk_asm_grid_indices(r, k, w, b, o); }
void a3_gk_vertex_normals(A3Vertex *v, const u32 *i, u32 n) { if (n) a3_gk_asm_vertex_normals(v, i, n); }
void a3_gk_normalize(A3Vertex *v, u32 n) { if (n) a3_gk_asm_normalize(v, n); }
f32  a3_gk_signed_volume(const A3Vertex *v, const u32 *i, u32 n) { return n ? a3_gk_asm_signed_volume(v, i, n) : 0.0f; }
const char *a3_gk_backend(void) { return "x86-64 SSE assembly"; }
#else
u32  a3_gk_catmull_rom(const A3Vec2 *c, u32 n, u32 s, u32 cl, A3Vec2 *o) { return a3_gk_ref_catmull_rom(c, n, s, cl, o); }
void a3_gk_frame_points(const A3Vec2 *p, u32 n, const f32 *f, void *d, u32 st) { a3_gk_ref_frame_points(p, n, f, d, st); }
u32  a3_gk_grid_indices(u32 r, u32 k, u32 w, u32 b, u32 *o) { return a3_gk_ref_grid_indices(r, k, w, b, o); }
void a3_gk_vertex_normals(A3Vertex *v, const u32 *i, u32 n) { a3_gk_ref_vertex_normals(v, i, n); }
void a3_gk_normalize(A3Vertex *v, u32 n) { a3_gk_ref_normalize(v, n); }
f32  a3_gk_signed_volume(const A3Vertex *v, const u32 *i, u32 n) { return a3_gk_ref_signed_volume(v, i, n); }
void a3_gk_walk_pose(const f32 *p, const f32 *a, u32 n, f32 *o) { a3_gk_ref_walk_pose(p, a, n, o); }
const char *a3_gk_backend(void) { return "C reference"; }
#endif
