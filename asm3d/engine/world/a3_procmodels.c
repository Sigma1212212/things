/*
 * ASM3D - a3_procmodels.c
 * Detailed procedural models (see a3_procmeshes.h for the list).
 *
 * Car bodies are analytic lofts: a side-view top line (nose, hood, windshield,
 * roof, rear window, trunk), a bottom line with arched wheel wells, a
 * plan-view width and a cross-section with tumblehome, sampled finely and
 * smooth shaded. Each vertex carries "feature coordinates" in its UV:
 *   u: 0 = nose, 1 = windshield base, 2 = roof front, 3 = roof back,
 *      4 = trunk / tailgate start, 5 = tail;  parts > 10 are add-ons
 *   v: 0 = underside, 1 = shoulder (belt line), 2 = roof center line
 * so builtin:carbody can shade glass, pillars, lights, grille, plates, door
 * seams and trim by position with a single mesh and a single draw call.
 *
 * Palms, wheels and people use the same idea: u selects a material region
 * (bark / leaves / dead leaves / coconut, skin / cloth / shoes / hair...).
 *
 * The number crunching (spline sampling, placing profile points in 3D,
 * triangulating grids, accumulating and normalizing vertex normals, the
 * winding test) runs in the x86-64 assembly kernels of a3_geom_kernels.h;
 * this file only decides the shapes.
 */
#include "a3_procmeshes.h"
#include "a3_geom_kernels.h"
#include "../render/a3_mesh.h"
#include "../core/a3_math.h"
#include "../core/a3_memory.h"
#include "../core/a3_hash.h"
#include "../core/a3_sort.h"
#include "../core/a3_string.h"

/* ======================================================================== */
/* Mesh builder                                                             */
/* ======================================================================== */

typedef struct MB {
    A3_ARRAY_TYPE(A3Vertex) v;
    A3_ARRAY_TYPE(u32) idx;
    u32 part_v, part_i;      /* start of the current part */
} MB;

static void mb_init(MB *m) { a3_zero_struct(m); }
static void mb_free(MB *m) { a3_array_free(m->v); a3_array_free(m->idx); }

static u32 mb_vert(MB *m, A3Vec3 p, A3Vec2 uv) {
    A3Vertex vx;
    vx.position = p;
    vx.normal = a3_v3_zero();
    vx.uv = uv;
    a3_array_push(m->v, vx, A3_MEM_RESOURCE);
    return m->v.count - 1;
}
static void mb_tri(MB *m, u32 a, u32 b, u32 c) {
    a3_array_push(m->idx, a, A3_MEM_RESOURCE);
    a3_array_push(m->idx, b, A3_MEM_RESOURCE);
    a3_array_push(m->idx, c, A3_MEM_RESOURCE);
}
static void mb_quad(MB *m, u32 a, u32 b, u32 c, u32 d) { mb_tri(m, a, b, c); mb_tri(m, a, c, d); }

static void mb_begin_part(MB *m) { m->part_v = m->v.count; m->part_i = m->idx.count; }

/* Closed parts: make the winding outward (positive volume), then smooth
 * normals (assembly kernels). */
static void mb_end_part(MB *m, b32 closed) {
    u32 tris = (m->idx.count - m->part_i) / 3;
    if (!tris) return;
    const u32 *ix = m->idx.data + m->part_i;
    if (closed && a3_gk_signed_volume(m->v.data, ix, tris) < 0.0f)
        for (u32 i = m->part_i; i + 2 < m->idx.count; i += 3) { u32 t = m->idx.data[i + 1]; m->idx.data[i + 1] = m->idx.data[i + 2]; m->idx.data[i + 2] = t; }
    for (u32 i = m->part_v; i < m->v.count; ++i) m->v.data[i].normal = a3_v3_zero();
    a3_gk_vertex_normals(m->v.data, m->idx.data + m->part_i, tris);
    a3_gk_normalize(m->v.data + m->part_v, m->v.count - m->part_v);
}

/* Open two-sided surfaces (leaves): duplicate the part with the back side. */
static void mb_end_two_sided(MB *m) {
    mb_end_part(m, 0);
    u32 v0 = m->part_v, v1 = m->v.count, i0 = m->part_i, i1 = m->idx.count;
    for (u32 i = v0; i < v1; ++i) {
        A3Vertex vx = m->v.data[i];
        vx.normal = a3_v3_neg(vx.normal);
        a3_array_push(m->v, vx, A3_MEM_RESOURCE);
    }
    for (u32 i = i0; i + 2 < i1; i += 3) mb_tri(m, m->idx.data[i] - v0 + v1, m->idx.data[i + 2] - v0 + v1, m->idx.data[i + 1] - v0 + v1);
}

static b32 mb_finish(MB *m, A3MeshData *out) {
    b32 ok = m->v.count && m->idx.count && a3_mesh_alloc(out, m->v.count, m->idx.count);
    if (ok) {
        a3_memcpy(out->vertices, m->v.data, sizeof(A3Vertex) * m->v.count);
        a3_memcpy(out->indices, m->idx.data, sizeof(u32) * m->idx.count);
        a3_mesh_compute_bounds(out);
    }
    mb_free(m);
    return ok;
}

/* Grid of `rows` rings with `k` points each: quads between rings (ring closed
 * when `wrap`). Returns the first vertex index. */
static u32 mb_grid(MB *m, const A3Vec3 *pts, const A3Vec2 *uvs, u32 rows, u32 k, b32 wrap) {
    u32 base = m->v.count;
    for (u32 i = 0; i < rows * k; ++i) mb_vert(m, pts[i], uvs ? uvs[i] : a3_v2(0, 0));
    u32 need = (rows - 1) * k * 6;
    if (!a3_array_reserve(m->idx, m->idx.count + need, A3_MEM_RESOURCE)) return base;
    m->idx.count += a3_gk_grid_indices(rows, k, wrap, base, m->idx.data + m->idx.count);
    return base;
}

/* Ring of k points on an ellipse (rx, ry) placed in a frame (assembly):
 * point = origin + axis_a * cos * rx + axis_b * sin * ry. */
static void ring_points(u32 k, f32 rx, f32 ry, A3Vec3 origin, A3Vec3 axis_a, A3Vec3 axis_b, A3Vec3 *out) {
    A3Vec2 c[64];
    if (k > 64) k = 64;
    for (u32 i = 0; i < k; ++i) {
        f32 a = (f32)i / (f32)k * A3_TAU;
        c[i] = a3_v2(a3_cosf(a) * rx, a3_sinf(a) * ry);
    }
    const f32 frame[12] = { origin.x, origin.y, origin.z, 0, axis_a.x, axis_a.y, axis_a.z, 0, axis_b.x, axis_b.y, axis_b.z, 0 };
    a3_gk_frame_points(c, k, frame, out, sizeof(A3Vec3));
}

/* Lathe around the X axis: profile (x, r) points, `seg` segments; every
 * profile segment is its own strip (hard edges along the profile). */
static void mb_lathe(MB *m, const f32 (*prof)[2], u32 n, u32 seg, f32 region) {
    for (u32 s = 0; s + 1 < n; ++s) {
        mb_begin_part(m);
        u32 base = m->v.count;
        for (u32 row = 0; row < 2; ++row) {
            f32 x = prof[s + row][0], r = prof[s + row][1];
            A3Vec3 ring[64];
            ring_points(seg, r, r, a3_v3(x, 0, 0), a3_v3(0, 1, 0), a3_v3(0, 0, 1), ring);
            for (u32 i = 0; i <= seg; ++i)
                mb_vert(m, ring[i % seg], a3_v2(region + (f32)(s + row) / (f32)n * 0.99f, (f32)i / (f32)seg));
        }
        for (u32 i = 0; i < seg; ++i) mb_quad(m, base + i, base + i + 1, base + seg + 1 + i + 1, base + seg + 1 + i);
        mb_end_part(m, 0);
        /* outward: the strip normal should point away from the axis (or along x for discs) */
        A3Vec3 p = m->v.data[base].position, nrm = m->v.data[base].normal;
        A3Vec3 radial = a3_v3(0, p.y, p.z);
        f32 dx = prof[s + 1][0] - prof[s][0], dr = prof[s + 1][1] - prof[s][1];
        A3Vec3 want = a3_v3_add(a3_v3_scale(a3_v3_norm(a3_v3_add(radial, a3_v3(0, 1e-6f, 0))), dx), a3_v3(-dr, 0, 0)); /* profile normal (dx, -dr) rotated */
        if (a3_v3_dot(nrm, want) < 0) {
            for (u32 i = m->part_i; i + 2 < m->idx.count; i += 3) { u32 t = m->idx.data[i + 1]; m->idx.data[i + 1] = m->idx.data[i + 2]; m->idx.data[i + 2] = t; }
            for (u32 i = m->part_v; i < m->v.count; ++i) m->v.data[i].normal = a3_v3_neg(m->v.data[i].normal);
        }
    }
}

/* Ellipsoid-ish closed blob (sphere scaled), for heads, coconuts, hands. */
static void mb_blob(MB *m, A3Vec3 c, A3Vec3 r, u32 seg, u32 rings, A3Vec2 uv0, f32 uv_v_scale) {
    mb_begin_part(m);
    u32 base = m->v.count;
    for (u32 j = 0; j <= rings; ++j) {
        f32 phi = (f32)j / (f32)rings * A3_PI;
        A3Vec3 ring[64];
        ring_points(seg, a3_sinf(phi) * r.x, a3_sinf(phi) * r.z, a3_v3(c.x, c.y + a3_cosf(phi) * r.y, c.z), a3_v3(1, 0, 0), a3_v3(0, 0, 1), ring);
        for (u32 i = 0; i <= seg; ++i)
            mb_vert(m, ring[i % seg], a3_v2(uv0.x + (f32)i / (f32)seg * 0.5f, uv0.y + (1.0f - (f32)j / (f32)rings) * uv_v_scale));
    }
    for (u32 j = 0; j < rings; ++j)
        for (u32 i = 0; i < seg; ++i) {
            u32 a = base + j * (seg + 1) + i;
            mb_quad(m, a, a + 1, a + seg + 2, a + seg + 1);
        }
    mb_end_part(m, 1);
}

/* A polyline sampled from a Catmull-Rom curve through (z, y) key points,
 * evaluated by z (keys must increase in z). */
typedef struct Curve { A3Vec2 pts[512]; u32 n; } Curve;

static void curve_build(Curve *c, const A3Vec2 *keys, u32 nk) {
    u32 sub = nk > 1 ? 510 / (nk - 1) : 1;
    if (sub > 24) sub = 24;
    c->n = a3_gk_catmull_rom(keys, nk, sub, 0, c->pts);
}

static f32 curve_at(const Curve *c, f32 z) {
    if (z <= c->pts[0].x) return c->pts[0].y;
    for (u32 i = 1; i < c->n; ++i) {
        if (z <= c->pts[i].x) {
            f32 dz = c->pts[i].x - c->pts[i - 1].x;
            f32 t = dz > 1e-6f ? (z - c->pts[i - 1].x) / dz : 0.0f;
            return a3_lerpf(c->pts[i - 1].y, c->pts[i].y, t);
        }
    }
    return c->pts[c->n - 1].y;
}

/* ======================================================================== */
/* Cars                                                                     */
/* ======================================================================== */

typedef struct CarStyle {
    f32 wb, fo, ro;           /* wheelbase, front / rear overhang */
    f32 width, wheel_r, clear;
    f32 nose_h, hood_h, belt_h, roof_h, trunk_h, tail_h;
    f32 ws0, r0, r1, rw1;     /* windshield base, roof start, roof end, rear window end (z, front = -z) */
    f32 tumble;               /* roof half width / body half width */
    i32 addon;                /* 0 none, 1 taxi sign, 2 police light bar */
} CarStyle;

static const CarStyle k_sedan  = { 2.80f, 0.95f, 1.00f, 1.84f, 0.33f, 0.16f, 0.64f, 0.93f, 0.97f, 1.45f, 1.03f, 0.95f, -0.95f, -0.22f, 0.88f, 1.48f, 0.74f, 0 };
static const CarStyle k_sports = { 2.56f, 0.98f, 0.88f, 1.92f, 0.34f, 0.12f, 0.52f, 0.80f, 0.86f, 1.22f, 0.96f, 0.90f, -0.52f, 0.08f, 0.52f, 1.58f, 0.70f, 0 };
static const CarStyle k_suv    = { 2.86f, 0.92f, 1.04f, 1.95f, 0.38f, 0.24f, 0.86f, 1.12f, 1.17f, 1.80f, 1.25f, 1.10f, -1.02f, -0.40f, 1.88f, 2.26f, 0.80f, 0 };
static const CarStyle k_hatch  = { 2.56f, 0.82f, 0.74f, 1.78f, 0.31f, 0.15f, 0.64f, 0.90f, 0.95f, 1.49f, 1.12f, 0.98f, -0.74f, -0.06f, 1.12f, 1.74f, 0.75f, 0 };

static f32 car_half_width(const CarStyle *s, f32 z) {
    f32 front = -s->wb * 0.5f - s->fo, rear = s->wb * 0.5f + s->ro;
    /* rounded corners in plan view: circular falloff over the last rr meters */
    f32 d = a3_minf(z - front, rear - z);
    f32 rr = z - front < rear - z ? 0.42f : 0.36f;
    f32 w = 1.0f;
    if (d < rr) { f32 q = (rr - d) / rr; w = 0.52f + 0.48f * a3_sqrtf(a3_maxf(1.0f - q * q, 0.0f)); }
    /* fender flare over the wheels */
    for (int a = -1; a <= 1; a += 2) {
        f32 dz = z - a * s->wb * 0.5f;
        w += 0.018f * a3_maxf(0.0f, 1.0f - (dz * dz) / 0.36f);
    }
    return s->width * 0.5f * a3_minf(w, 1.0f);
}

static f32 car_bottom(const CarStyle *s, f32 z) {
    f32 front = -s->wb * 0.5f - s->fo, rear = s->wb * 0.5f + s->ro;
    f32 b = s->clear;
    f32 ef = a3_saturate(1.0f - (z - front) / 0.5f), er = a3_saturate(1.0f - (rear - z) / 0.5f);
    b += ef * ef * ef * 0.2f + er * er * er * 0.18f;                /* bumpers curve up underneath */
    f32 ra = s->wheel_r + 0.075f;
    for (int a = -1; a <= 1; a += 2) {
        f32 dz = z - a * s->wb * 0.5f;
        if (a3_absf(dz) < ra) b = a3_maxf(b, s->wheel_r + a3_sqrtf(ra * ra - dz * dz) * 0.97f);
    }
    return b;
}

static f32 car_u(const CarStyle *s, f32 z) {
    f32 front = -s->wb * 0.5f - s->fo, rear = s->wb * 0.5f + s->ro;
    f32 k[6] = { front, s->ws0, s->r0, s->r1, s->rw1, rear };
    if (z <= k[0]) return 0;
    for (int i = 1; i < 6; ++i) if (z <= k[i]) return (f32)(i - 1) + (z - k[i - 1]) / a3_maxf(k[i] - k[i - 1], 1e-4f);
    return 5;
}

/* Right-side control points of the cross-section at z (P0 bottom center .. P7 top center) */
static void car_section(const CarStyle *s, const Curve *top, f32 z, A3Vec2 *p, f32 *v) {
    f32 T = curve_at(top, z);
    f32 B = car_bottom(s, z);
    f32 W = car_half_width(s, z);
    f32 front = -s->wb * 0.5f - s->fo, len = s->wb + s->fo + s->ro;
    f32 belt = s->belt_h + 0.035f * (z - front) / len;          /* slight wedge */
    f32 sh = a3_minf(belt, T - 0.035f);
    f32 g = a3_saturate((T - sh - 0.04f) / 0.22f);               /* greenhouse weight */
    if (B > sh - 0.12f) B = sh - 0.12f;                          /* keep a sill under the arches */
    p[0] = a3_v2(0, B);                        v[0] = 0.0f;
    p[1] = a3_v2(0.80f * W, B);                v[1] = 0.02f;
    p[2] = a3_v2(0.975f * W, B + 0.055f);      v[2] = 0.12f;
    p[3] = a3_v2(W, B + 0.42f * (sh - B));     v[3] = 0.55f;
    p[4] = a3_v2(0.985f * W, sh - 0.03f);      v[4] = 1.0f;
    A3Vec2 hood5 = a3_v2(0.86f * W, a3_lerpf(sh, T, 0.72f)), gh5 = a3_v2(0.93f * W, sh + 0.025f);
    A3Vec2 hood6 = a3_v2(0.48f * W, T - 0.004f), gh6 = a3_v2(s->tumble * W, T - 0.055f);
    p[5] = a3_v2(a3_lerpf(hood5.x, gh5.x, g), a3_lerpf(hood5.y, gh5.y, g)); v[5] = 1.05f;
    p[6] = a3_v2(a3_lerpf(hood6.x, gh6.x, g), a3_lerpf(hood6.y, gh6.y, g)); v[6] = 1.95f;
    p[7] = a3_v2(0, T);                        v[7] = 2.0f;
}

#define CAR_RING_CTRL 14
#define CAR_RING_SUB 4
#define CAR_RING (CAR_RING_CTRL * CAR_RING_SUB)

static void car_ring(const CarStyle *s, const Curve *top, f32 z, A3Vec3 *out, A3Vec2 *uv) {
    A3Vec2 p[8];
    f32 v[8];
    car_section(s, top, z, p, v);
    /* closed ring: P0, P1..P6 right, P7, P6..P1 left */
    A3Vec2 ring[CAR_RING_CTRL];
    f32 rv[CAR_RING_CTRL];
    u32 n = 0;
    for (int i = 0; i <= 7; ++i) { ring[n] = p[i]; rv[n++] = v[i]; }
    for (int i = 6; i >= 1; --i) { ring[n] = a3_v2(-p[i].x, p[i].y); rv[n++] = v[i]; }
    A3Vec2 smooth[CAR_RING];
    a3_gk_catmull_rom(ring, CAR_RING_CTRL, CAR_RING_SUB, 1, smooth);                 /* assembly */
    const f32 frame[12] = { 0, 0, z, 0, 1, 0, 0, 0, 0, 1, 0, 0 };                    /* (x, y) -> (x, y, z) */
    a3_gk_frame_points(smooth, CAR_RING, frame, out, sizeof(A3Vec3));                /* assembly */
    f32 u = car_u(s, z);
    for (u32 i = 0; i < CAR_RING_CTRL; ++i)
        for (u32 k = 0; k < CAR_RING_SUB; ++k)
            uv[i * CAR_RING_SUB + k] = a3_v2(u, a3_lerpf(rv[i], rv[(i + 1) % CAR_RING_CTRL], (f32)k / CAR_RING_SUB));
}

static int cmp_f32(const void *a, const void *b, void *user) {
    A3_UNUSED(user);
    f32 x = *(const f32 *)a, y = *(const f32 *)b; return x < y ? -1 : x > y; }

/* add-on box (sign, light bar, mirror): uv.x = region code */
static void mb_box(MB *m, A3Vec3 lo, A3Vec3 hi, f32 region) {
    static const int F[6][4] = { { 0, 3, 7, 4 }, { 1, 5, 6, 2 }, { 3, 2, 6, 7 }, { 0, 4, 5, 1 }, { 4, 7, 6, 5 }, { 0, 1, 2, 3 } };
    A3Vec3 c[8] = { { lo.x, lo.y, lo.z }, { hi.x, lo.y, lo.z }, { hi.x, hi.y, lo.z }, { lo.x, hi.y, lo.z },
                    { lo.x, lo.y, hi.z }, { hi.x, lo.y, hi.z }, { hi.x, hi.y, hi.z }, { lo.x, hi.y, hi.z } };
    for (int f = 0; f < 6; ++f) {
        mb_begin_part(m);
        u32 a = mb_vert(m, c[F[f][0]], a3_v2(region, 0)), b = mb_vert(m, c[F[f][1]], a3_v2(region + 0.3f, 0));
        u32 cc = mb_vert(m, c[F[f][2]], a3_v2(region + 0.3f, 1)), d = mb_vert(m, c[F[f][3]], a3_v2(region, 1));
        mb_quad(m, a, b, cc, d);
        mb_end_part(m, 0);
        /* outward normal check against the box center */
        A3Vec3 ctr = a3_v3_scale(a3_v3_add(lo, hi), 0.5f);
        A3Vec3 fc = a3_v3_scale(a3_v3_add(m->v.data[a].position, m->v.data[cc].position), 0.5f);
        if (a3_v3_dot(m->v.data[a].normal, a3_v3_sub(fc, ctr)) < 0) {
            u32 *ix = &m->idx.data[m->part_i];
            u32 t = ix[1]; ix[1] = ix[2]; ix[2] = t;
            t = ix[4]; ix[4] = ix[5]; ix[5] = t;
            for (u32 i = m->part_v; i < m->v.count; ++i) m->v.data[i].normal = a3_v3_neg(m->v.data[i].normal);
        }
    }
}

static b32 gen_car(const CarStyle *s, A3MeshData *out) {
    f32 front = -s->wb * 0.5f - s->fo, rear = s->wb * 0.5f + s->ro;
    /* side-view top line */
    A3Vec2 keys[] = {
        { front, s->nose_h - 0.06f },
        { front + 0.06f, s->nose_h + 0.05f },
        { front + 0.22f, s->nose_h + 0.55f * (s->hood_h - s->nose_h) },
        { front + 0.55f * (s->ws0 - front), s->hood_h - 0.04f },
        { s->ws0, s->hood_h },
        { a3_lerpf(s->ws0, s->r0, 0.5f), a3_lerpf(s->hood_h, s->roof_h, 0.64f) },
        { s->r0, s->roof_h - 0.015f },
        { (s->r0 + s->r1) * 0.5f, s->roof_h },
        { s->r1, s->roof_h - 0.02f },
        { a3_lerpf(s->r1, s->rw1, 0.5f), a3_lerpf(s->roof_h, s->trunk_h, 0.58f) },
        { s->rw1, s->trunk_h },
        { a3_lerpf(s->rw1, rear, 0.7f), s->trunk_h - 0.01f },
        { rear - 0.07f, a3_lerpf(s->trunk_h, s->tail_h, 0.6f) },
        { rear, s->tail_h - 0.07f },
    };
    Curve top;
    curve_build(&top, keys, A3_ARRAY_COUNT(keys));
    /* sections: uniform, denser at the ends and around the arches, plus the feature breakpoints */
    f32 zs[400];
    u32 nz = 0;
    for (f32 z = front; z <= rear && nz < 250; z += 0.07f) zs[nz++] = z;
    for (int a = -1; a <= 1; a += 2)
        for (f32 d = -(s->wheel_r + 0.12f); d <= s->wheel_r + 0.12f && nz < 340; d += 0.035f) zs[nz++] = a * s->wb * 0.5f + d;
    static const f32 end_d[] = { 0.004f, 0.012f, 0.024f, 0.04f, 0.06f, 0.085f, 0.115f, 0.15f, 0.19f, 0.24f, 0.3f, 0.37f };
    for (u32 i = 0; i < A3_ARRAY_COUNT(end_d) && nz < 380; ++i) { zs[nz++] = front + end_d[i]; zs[nz++] = rear - end_d[i]; }
    f32 bp[] = { front, s->ws0, s->r0, s->r1, s->rw1, rear };
    for (u32 i = 0; i < 6; ++i) zs[nz++] = bp[i];
    a3_sort(zs, nz, sizeof(f32), cmp_f32, 0);
    u32 w = 0;
    for (u32 i = 0; i < nz; ++i) if (zs[i] >= front && zs[i] <= rear && (!w || zs[i] - zs[w - 1] > 0.008f)) zs[w++] = zs[i];
    nz = w;
    zs[nz - 1] = rear;
    zs[0] = front;

    MB m;
    mb_init(&m);
    mb_begin_part(&m);
    A3Vec3 *pts = A3_NEW_ARRAY(A3Vec3, nz * CAR_RING, A3_MEM_TEMP);
    A3Vec2 *uvs = A3_NEW_ARRAY(A3Vec2, nz * CAR_RING, A3_MEM_TEMP);
    if (!pts || !uvs) { a3_free(pts); a3_free(uvs); return 0; }
    for (u32 i = 0; i < nz; ++i) car_ring(s, &top, zs[i], pts + i * CAR_RING, uvs + i * CAR_RING);
    mb_grid(&m, pts, uvs, nz, CAR_RING, 1);
    /* end caps (front face and tail) as horizontal bands between mirrored ring points */
    for (int end = 0; end < 2; ++end) {
        const A3Vec3 *r = pts + (end ? (nz - 1) * CAR_RING : 0);
        const A3Vec2 *ru = uvs + (end ? (nz - 1) * CAR_RING : 0);
        u32 half = CAR_RING / 2;
        for (u32 j = 0; j < half; ++j) {
            /* ring index j (right side, going up) pairs with (CAR_RING - j) % CAR_RING (left) */
            u32 rj = j, rj1 = j + 1, lj = (CAR_RING - j) % CAR_RING, lj1 = (CAR_RING - j - 1) % CAR_RING;
            u32 a = mb_vert(&m, r[rj], ru[rj]), b = mb_vert(&m, r[rj1], ru[rj1]);
            u32 c = mb_vert(&m, r[lj1], ru[lj1]), d = mb_vert(&m, r[lj], ru[lj]);
            mb_quad(&m, a, b, c, d);
        }
    }
    a3_free(pts);
    a3_free(uvs);
    mb_end_part(&m, 1);
    /* the caps must not blend normals with the sides: re-flatten their normals */
    {
        u32 capv = m.v.count - (CAR_RING / 2) * 4 * 2;
        for (u32 i = capv; i < m.v.count; ++i) {
            f32 sgn = m.v.data[i].position.z < 0 ? -1.0f : 1.0f;
            m.v.data[i].normal = a3_v3_norm(a3_v3(0, 0.08f, sgn));
        }
    }
    /* side mirrors (region 11) */
    f32 mz = s->ws0 + 0.2f, mw = car_half_width(s, mz) * 0.93f, my = s->belt_h + 0.04f;
    for (int side = -1; side <= 1; side += 2) {
        f32 x0 = side * (mw - 0.04f), x1 = side * (mw + 0.14f);
        mb_box(&m, a3_v3(a3_minf(x0, x1), my, mz - 0.04f), a3_v3(a3_maxf(x0, x1), my + 0.1f, mz + 0.05f), 11.0f);
    }
    /* add-ons */
    if (s->addon == 1) {           /* taxi roof sign */
        f32 z = (s->r0 + s->r1) * 0.5f;
        mb_box(&m, a3_v3(-0.32f, s->roof_h - 0.01f, z - 0.12f), a3_v3(0.32f, s->roof_h + 0.2f, z + 0.12f), 12.0f);
    } else if (s->addon == 2) {    /* police light bar: red half, blue half */
        f32 z = s->r0 + 0.25f * (s->r1 - s->r0);
        mb_box(&m, a3_v3(-0.62f, s->roof_h - 0.01f, z - 0.13f), a3_v3(-0.02f, s->roof_h + 0.12f, z + 0.13f), 13.0f);
        mb_box(&m, a3_v3(0.02f, s->roof_h - 0.01f, z - 0.13f), a3_v3(0.62f, s->roof_h + 0.12f, z + 0.13f), 14.0f);
    }
    return mb_finish(&m, out);
}

static b32 gen_car_sedan(A3MeshData *o) { return gen_car(&k_sedan, o); }
static b32 gen_car_sports(A3MeshData *o) { return gen_car(&k_sports, o); }
static b32 gen_car_suv(A3MeshData *o) { return gen_car(&k_suv, o); }
static b32 gen_car_hatch(A3MeshData *o) { return gen_car(&k_hatch, o); }
static b32 gen_car_taxi(A3MeshData *o) { CarStyle s = k_sedan; s.addon = 1; return gen_car(&s, o); }
static b32 gen_car_police(A3MeshData *o) { CarStyle s = k_sedan; s.addon = 2; return gen_car(&s, o); }

/* Tire + rim, axle along X, outer (rim face) side = +X. Radius 1 (scale per car). */
static b32 gen_wheel_detailed(A3MeshData *out) {
    static const f32 prof[][2] = {       /* (x, r) */
        { -0.33f, 0.00f }, { -0.33f, 0.62f }, { -0.345f, 0.70f }, { -0.335f, 0.86f }, { -0.30f, 0.96f },
        { -0.25f, 1.00f }, { 0.25f, 1.00f }, { 0.30f, 0.96f }, { 0.335f, 0.86f }, { 0.345f, 0.70f },
        { 0.33f, 0.64f }, { 0.30f, 0.60f }, { 0.20f, 0.56f }, { 0.17f, 0.25f }, { 0.22f, 0.13f }, { 0.22f, 0.0f },
    };
    MB m;
    mb_init(&m);
    mb_lathe(&m, prof, A3_ARRAY_COUNT(prof), 40, 0.0f);
    return mb_finish(&m, out);
}

/* ======================================================================== */
/* Palms (meters: about 9 m tall; scale per instance)                       */
/* ======================================================================== */

static A3Vec3 bez3(A3Vec3 a, A3Vec3 b, A3Vec3 c, A3Vec3 d, f32 t) {
    f32 u = 1 - t;
    return a3_v3_add(a3_v3_add(a3_v3_scale(a, u * u * u), a3_v3_scale(b, 3 * u * u * t)), a3_v3_add(a3_v3_scale(c, 3 * u * t * t), a3_v3_scale(d, t * t * t)));
}

static b32 gen_palm(A3MeshData *out, u64 seed, f32 height, f32 lean) {
    A3Rng rng;
    a3_rng_seed(&rng, seed, 3);
    MB m;
    mb_init(&m);
    /* ---- trunk: tapered, curved, with growth rings (region 0) ---- */
    A3Vec3 s0 = a3_v3(0, 0, 0), s1 = a3_v3(lean * 0.15f, height * 0.35f, 0), s2 = a3_v3(lean * 0.9f, height * 0.7f, lean * 0.1f), s3 = a3_v3(lean, height, lean * 0.15f);
    enum { TS = 64, TK = 14 };
    A3Vec3 pts[TS * TK];
    A3Vec2 uvs[TS * TK];
    for (u32 i = 0; i < TS; ++i) {
        f32 t = (f32)i / (TS - 1);
        A3Vec3 c = bez3(s0, s1, s2, s3, t);
        A3Vec3 tan = a3_v3_norm(a3_v3_sub(bez3(s0, s1, s2, s3, a3_minf(t + 0.01f, 1)), bez3(s0, s1, s2, s3, a3_maxf(t - 0.01f, 0))));
        A3Vec3 side = a3_v3_norm(a3_v3_cross(tan, a3_v3(0, 0, 1)));
        A3Vec3 fwd = a3_v3_cross(side, tan);
        f32 y = t * height;
        f32 r = a3_lerpf(0.21f, 0.13f, t) + 0.12f * a3_powf(1.0f - a3_saturate(y / 0.9f), 2.0f);   /* flared base */
        f32 ring = 0.5f + 0.5f * a3_cosf(y * A3_TAU / 0.22f);
        r *= 1.0f + 0.05f * ring * ring;
        ring_points(TK, r, r, c, side, fwd, pts + i * TK);
        for (u32 k = 0; k < TK; ++k) uvs[i * TK + k] = a3_v2(0.0f + (f32)k / TK * 0.99f, y);
    }
    mb_begin_part(&m);
    mb_grid(&m, pts, uvs, TS, TK, 1);
    mb_end_part(&m, 0);
    /* flip if needed: normal of the first vertex should point away from the spine */
    if (a3_v3_dot(m.v.data[m.part_v].normal, a3_v3_sub(m.v.data[m.part_v].position, s0)) < 0) {
        for (u32 i = m.part_i; i + 2 < m.idx.count; i += 3) { u32 t = m.idx.data[i + 1]; m.idx.data[i + 1] = m.idx.data[i + 2]; m.idx.data[i + 2] = t; }
        for (u32 i = m.part_v; i < m.v.count; ++i) m.v.data[i].normal = a3_v3_neg(m.v.data[i].normal);
    }
    A3Vec3 crown = s3;
    /* ---- crown shaft bulge + coconuts (region 3) ---- */
    mb_blob(&m, a3_v3(crown.x, crown.y + 0.05f, crown.z), a3_v3(0.2f, 0.32f, 0.2f), 10, 6, a3_v2(0.0f, height), 0.3f);
    for (int k = 0; k < 5; ++k) {
        f32 a = (f32)k / 5.0f * A3_TAU + a3_rng_f32(&rng);
        mb_blob(&m, a3_v3(crown.x + a3_cosf(a) * 0.2f, crown.y - 0.15f - a3_rng_f32(&rng) * 0.1f, crown.z + a3_sinf(a) * 0.2f), a3_v3s(0.11f), 8, 5, a3_v2(3.0f, 0), 1.0f);
    }
    /* ---- fronds: rachis with leaflets both sides (region 1 green, 2 dead) ---- */
    u32 fronds = 16 + a3_rng_range_u32(&rng, 5);
    for (u32 f = 0; f < fronds; ++f) {
        b32 dead = f >= fronds - 3;
        f32 yaw = (f32)f / (f32)fronds * A3_TAU * 2.618f + a3_rng_range_f32(&rng, -0.2f, 0.2f);   /* golden spiral */
        f32 len = dead ? a3_rng_range_f32(&rng, 2.4f, 3.0f) : a3_rng_range_f32(&rng, 3.4f, 4.4f);
        f32 up = dead ? -1.6f : a3_rng_range_f32(&rng, -0.25f, 0.85f);        /* initial elevation */
        A3Vec3 dir = a3_v3(a3_cosf(yaw), 0, a3_sinf(yaw));
        A3Vec3 side = a3_v3(-dir.z, 0, dir.x);
        enum { FS = 20, LEAF = 26 };
        A3Vec3 spine[FS];
        for (u32 i = 0; i < FS; ++i) {
            f32 t = (f32)i / (FS - 1);
            f32 h = up * t * len * 0.45f - (dead ? 0.2f : 0.9f) * t * t * len * 0.5f;
            spine[i] = a3_v3_add(a3_v3(crown.x, crown.y + 0.12f, crown.z), a3_v3_add(a3_v3_scale(dir, t * len * (dead ? 0.35f : 1.0f)), a3_v3(0, h, 0)));
        }
        f32 region = dead ? 2.0f : 1.0f;
        mb_begin_part(&m);
        /* rachis: thin ribbon on top of the spine */
        for (u32 i = 0; i + 1 < FS; ++i) {
            f32 wdt = 0.03f * (1.0f - (f32)i / FS) + 0.008f;
            A3Vec3 a0 = a3_v3_add(spine[i], a3_v3_scale(side, wdt)), a1 = a3_v3_sub(spine[i], a3_v3_scale(side, wdt));
            A3Vec3 b0 = a3_v3_add(spine[i + 1], a3_v3_scale(side, wdt)), b1 = a3_v3_sub(spine[i + 1], a3_v3_scale(side, wdt));
            mb_quad(&m, mb_vert(&m, a0, a3_v2(region + 0.02f, 0)), mb_vert(&m, b0, a3_v2(region + 0.02f, 0)), mb_vert(&m, b1, a3_v2(region + 0.02f, 0)), mb_vert(&m, a1, a3_v2(region + 0.02f, 0)));
        }
        /* leaflets: V-shaped, drooping, longest in the middle */
        for (u32 l = 2; l < LEAF; ++l) {
            f32 t = (f32)l / LEAF;
            f32 st = t * (FS - 1);
            u32 i0 = (u32)st;
            if (i0 >= FS - 1) i0 = FS - 2;
            A3Vec3 base = a3_v3_lerp(spine[i0], spine[i0 + 1], st - (f32)i0);
            A3Vec3 fwd = a3_v3_norm(a3_v3_sub(spine[i0 + 1], spine[i0]));
            f32 ll = (dead ? 0.35f : 0.75f) * a3_sinf(A3_PI * a3_minf(t * 1.1f, 1.0f)) + 0.08f;
            for (int sd = -1; sd <= 1; sd += 2) {
                A3Vec3 odir = a3_v3_add(a3_v3_scale(side, (f32)sd * 0.85f), a3_v3_scale(fwd, 0.45f));
                odir = a3_v3_add(odir, a3_v3(0, dead ? -1.2f : -0.35f - 0.5f * t, 0));
                odir = a3_v3_norm(odir);
                A3Vec3 tip = a3_v3_add(base, a3_v3_scale(odir, ll));
                A3Vec3 mid = a3_v3_add(a3_v3_lerp(base, tip, 0.5f), a3_v3(0, -0.06f * ll, 0));
                A3Vec3 wv = a3_v3_scale(fwd, 0.035f + 0.02f * (1.0f - t));
                f32 jitter = a3_rng_range_f32(&rng, 0.0f, 0.3f);
                u32 a = mb_vert(&m, a3_v3_sub(base, wv), a3_v2(region + 0.05f, 0));
                u32 b = mb_vert(&m, a3_v3_add(base, wv), a3_v2(region + 0.05f, 0));
                u32 c = mb_vert(&m, a3_v3_add(mid, a3_v3_scale(wv, 1.1f)), a3_v2(region + 0.5f + jitter * 0.1f, 0.5f));
                u32 d = mb_vert(&m, a3_v3_sub(mid, a3_v3_scale(wv, 1.1f)), a3_v2(region + 0.5f + jitter * 0.1f, 0.5f));
                u32 e = mb_vert(&m, tip, a3_v2(region + 0.95f, 1.0f));
                mb_quad(&m, a, b, c, d);
                mb_tri(&m, d, c, e);
            }
        }
        mb_end_two_sided(&m);
    }
    return mb_finish(&m, out);
}

static b32 gen_palm_a(A3MeshData *o) { return gen_palm(o, 11, 9.0f, 0.9f); }
static b32 gen_palm_b(A3MeshData *o) { return gen_palm(o, 23, 9.5f, -0.4f); }
static b32 gen_palm_c(A3MeshData *o) { return gen_palm(o, 37, 8.5f, 1.6f); }

/* ======================================================================== */
/* People: jointed parts (meters), pivot at the joint                       */
/* ======================================================================== */

/* Lofted limb along -Y from the pivot: elliptical sections (rx, rz, y, v). */
typedef struct LimbSec { f32 y, rx, rz, cx, cz; } LimbSec;

static void mb_limb(MB *m, const LimbSec *s, u32 n, f32 region_lo, f32 region_hi, f32 split_y) {
    enum { K = 12 };
    A3Vec3 pts[40 * K];
    A3Vec2 uvs[40 * K];
    for (u32 i = 0; i < n; ++i) {
        ring_points(K, s[i].rx, s[i].rz, a3_v3(s[i].cx, s[i].y, s[i].cz), a3_v3(1, 0, 0), a3_v3(0, 0, 1), pts + i * K);
        for (u32 k = 0; k < K; ++k) uvs[i * K + k] = a3_v2((s[i].y < split_y ? region_lo : region_hi) + 0.5f, s[i].y);
    }
    mb_begin_part(m);
    u32 base = mb_grid(m, pts, uvs, n, K, 1);
    /* caps */
    u32 ct = mb_vert(m, a3_v3(s[0].cx, s[0].y + s[0].rx * 0.3f, s[0].cz), uvs[0]);
    u32 cb = mb_vert(m, a3_v3(s[n - 1].cx, s[n - 1].y - s[n - 1].rx * 0.3f, s[n - 1].cz), uvs[(n - 1) * K]);
    for (u32 k = 0; k < K; ++k) {
        mb_tri(m, ct, base + (k + 1) % K, base + k);
        mb_tri(m, cb, base + (n - 1) * K + k, base + (n - 1) * K + (k + 1) % K);
    }
    mb_end_part(m, 1);
}

/* regions: 0 skin, 1 upper clothing (base color), 2 lower clothing (base color), 3 shoes, 4 hair */
static b32 gen_human_torso(A3MeshData *out) {
    /* pivot = hips (y 0). Up to the neck (+0.58). */
    static const LimbSec s[] = {
        { 0.62f, 0.05f, 0.05f, 0, 0.01f }, { 0.56f, 0.055f, 0.055f, 0, 0.01f }, { 0.52f, 0.13f, 0.09f, 0, 0.0f },
        { 0.49f, 0.20f, 0.11f, 0, 0.0f }, { 0.42f, 0.205f, 0.12f, 0, 0.01f }, { 0.32f, 0.18f, 0.115f, 0, 0.015f },
        { 0.20f, 0.155f, 0.10f, 0, 0.01f }, { 0.10f, 0.16f, 0.10f, 0, 0.0f }, { 0.02f, 0.175f, 0.105f, 0, -0.005f },
        { -0.06f, 0.17f, 0.10f, 0, -0.01f }, { -0.10f, 0.14f, 0.085f, 0, -0.01f },
    };
    MB m;
    mb_init(&m);
    /* the neck is skin, the rest is the shirt; the lower part (below the belt) is trousers */
    enum { K = 14 };
    u32 n = A3_ARRAY_COUNT(s);
    A3Vec3 pts[20 * K];
    A3Vec2 uvs[20 * K];
    for (u32 i = 0; i < n; ++i) {
        f32 reg = s[i].y > 0.53f ? 0.0f : s[i].y > 0.03f ? 1.0f : 2.0f;
        ring_points(K, s[i].rx, s[i].rz, a3_v3(s[i].cx, s[i].y, s[i].cz), a3_v3(1, 0, 0), a3_v3(0, 0, 1), pts + i * K);
        for (u32 k = 0; k < K; ++k) uvs[i * K + k] = a3_v2(reg + 0.5f, s[i].y);
    }
    mb_begin_part(&m);
    u32 base = mb_grid(&m, pts, uvs, n, K, 1);
    u32 ct = mb_vert(&m, a3_v3(0, 0.63f, 0.01f), a3_v2(0.5f, 0.63f)), cb = mb_vert(&m, a3_v3(0, -0.12f, 0), a3_v2(2.5f, -0.12f));
    for (u32 k = 0; k < K; ++k) {
        mb_tri(&m, ct, base + (k + 1) % K, base + k);
        mb_tri(&m, cb, base + (n - 1) * K + k, base + (n - 1) * K + (k + 1) % K);
    }
    mb_end_part(&m, 1);
    return mb_finish(&m, out);
}

static b32 gen_human_head(A3MeshData *out) {
    MB m;
    mb_init(&m);
    /* pivot at the neck base: skull, jaw, nose, ears; hair region by uv.x = 4 on the top/back */
    mb_begin_part(&m);
    enum { SEG = 18, RINGS = 14 };
    u32 base = m.v.count;
    for (u32 j = 0; j <= RINGS; ++j) {
        f32 phi = (f32)j / RINGS * A3_PI;
        for (u32 i = 0; i <= SEG; ++i) {
            f32 th = (f32)i / SEG * A3_TAU;
            A3Vec3 d = a3_v3(a3_sinf(phi) * a3_sinf(th), a3_cosf(phi), -a3_sinf(phi) * a3_cosf(th));   /* th = 0 faces -Z (front) */
            f32 rx = 0.078f, ry = 0.115f, rz = 0.098f;
            if (d.y < 0) { rx *= 0.82f + 0.18f * (1.0f + d.y); rz *= 0.9f; }                 /* narrower jaw */
            if (d.z < -0.6f && d.y > -0.35f && d.y < 0.05f && a3_absf(d.x) < 0.22f) rz *= 1.0f + 0.22f * (1.0f - a3_absf(d.x) / 0.22f) * (1.0f - a3_absf(d.y + 0.15f) / 0.2f); /* nose */
            A3Vec3 p = a3_v3(d.x * rx, 0.12f + d.y * ry, d.z * rz + 0.01f);
            mb_vert(&m, p, a3_v2(0.5f, d.y));
        }
    }
    for (u32 j = 0; j < RINGS; ++j)
        for (u32 i = 0; i < SEG; ++i) {
            u32 a = base + j * (SEG + 1) + i;
            mb_quad(&m, a, a + 1, a + SEG + 2, a + SEG + 1);
        }
    mb_end_part(&m, 1);
    /* hair: a shell over the top and back of the skull, tucked under the skin at the face */
    mb_begin_part(&m);
    base = m.v.count;
    enum { HR = 9 };
    for (u32 j = 0; j <= HR; ++j) {
        f32 phi = (f32)j / HR * (A3_PI * 0.62f);
        for (u32 i = 0; i <= SEG; ++i) {
            f32 th = (f32)i / SEG * A3_TAU;
            A3Vec3 d = a3_v3(a3_sinf(phi) * a3_sinf(th), a3_cosf(phi), -a3_sinf(phi) * a3_cosf(th));
            f32 grow = 1.07f;
            if (d.z < -0.25f && d.y < 0.55f) grow = 0.93f;                    /* hairline: hidden in front */
            A3Vec3 p = a3_v3(d.x * 0.078f * grow, 0.12f + d.y * 0.115f * grow, d.z * 0.098f * grow + 0.01f + (d.z > 0.0f ? 0.008f : 0.0f));
            mb_vert(&m, p, a3_v2(4.5f, d.y));
        }
    }
    for (u32 j = 0; j < HR; ++j)
        for (u32 i = 0; i < SEG; ++i) {
            u32 a = base + j * (SEG + 1) + i;
            mb_quad(&m, a, a + 1, a + SEG + 2, a + SEG + 1);
        }
    mb_end_part(&m, 0);
    if (a3_v3_dot(m.v.data[m.part_v + (SEG + 1) * 3].normal, a3_v3_sub(m.v.data[m.part_v + (SEG + 1) * 3].position, a3_v3(0, 0.12f, 0.01f))) < 0) {
        for (u32 i = m.part_i; i + 2 < m.idx.count; i += 3) { u32 t = m.idx.data[i + 1]; m.idx.data[i + 1] = m.idx.data[i + 2]; m.idx.data[i + 2] = t; }
        for (u32 i = m.part_v; i < m.v.count; ++i) m.v.data[i].normal = a3_v3_neg(m.v.data[i].normal);
    }
    /* ears */
    for (int sd = -1; sd <= 1; sd += 2) mb_blob(&m, a3_v3(sd * 0.078f, 0.12f, 0.012f), a3_v3(0.012f, 0.028f, 0.02f), 6, 4, a3_v2(0.5f, 0), 0.0f);
    return mb_finish(&m, out);
}

static b32 gen_human_upper_arm(A3MeshData *out) {
    static const LimbSec s[] = { { 0.03f, 0.055f, 0.055f, 0, 0 }, { -0.02f, 0.052f, 0.05f, 0, 0 }, { -0.14f, 0.045f, 0.045f, 0, 0 }, { -0.26f, 0.038f, 0.038f, 0, 0 }, { -0.29f, 0.036f, 0.036f, 0, 0 } };
    MB m;
    mb_init(&m);
    mb_limb(&m, s, A3_ARRAY_COUNT(s), 1.0f, 1.0f, -1);
    return mb_finish(&m, out);
}

static b32 gen_human_forearm(A3MeshData *out) {
    static const LimbSec s[] = { { 0.02f, 0.036f, 0.036f, 0, 0 }, { -0.06f, 0.038f, 0.036f, 0, 0 }, { -0.2f, 0.028f, 0.024f, 0, 0 }, { -0.25f, 0.026f, 0.02f, 0, 0 } };
    MB m;
    mb_init(&m);
    mb_limb(&m, s, A3_ARRAY_COUNT(s), 0.0f, 0.0f, -1);
    /* hand: flattened blob */
    mb_blob(&m, a3_v3(0, -0.31f, 0.005f), a3_v3(0.024f, 0.07f, 0.042f), 8, 6, a3_v2(0.5f, 0), 0.0f);
    return mb_finish(&m, out);
}

static b32 gen_human_thigh(A3MeshData *out) {
    static const LimbSec s[] = { { 0.04f, 0.085f, 0.085f, 0, 0 }, { -0.02f, 0.082f, 0.085f, 0, 0.005f }, { -0.2f, 0.068f, 0.07f, 0, 0.005f }, { -0.38f, 0.052f, 0.055f, 0, 0 }, { -0.44f, 0.05f, 0.052f, 0, 0 } };
    MB m;
    mb_init(&m);
    mb_limb(&m, s, A3_ARRAY_COUNT(s), 2.0f, 2.0f, -1);
    return mb_finish(&m, out);
}

static b32 gen_human_shin(A3MeshData *out) {
    static const LimbSec s[] = { { 0.03f, 0.05f, 0.052f, 0, 0 }, { -0.08f, 0.052f, 0.058f, 0, 0.01f }, { -0.25f, 0.04f, 0.042f, 0, 0 }, { -0.38f, 0.034f, 0.036f, 0, 0 }, { -0.42f, 0.036f, 0.038f, 0, 0 } };
    MB m;
    mb_init(&m);
    mb_limb(&m, s, A3_ARRAY_COUNT(s), 2.0f, 2.0f, -1);
    /* shoe: rounded box toward -Z (front) */
    static const LimbSec shoe[] = {
        { -0.40f, 0.045f, 0.06f, 0, -0.03f }, { -0.43f, 0.052f, 0.12f, 0, -0.06f }, { -0.46f, 0.052f, 0.13f, 0, -0.065f }, { -0.47f, 0.048f, 0.12f, 0, -0.06f },
    };
    mb_limb(&m, shoe, A3_ARRAY_COUNT(shoe), 3.0f, 3.0f, -1);
    return mb_finish(&m, out);
}

/* ======================================================================== */
/* Street furniture                                                         */
/* ======================================================================== */

/* Traffic signal: pole with an arm over the road and two 3-lamp heads.
 * regions: 0 metal pole, 21 housing, 22 red lamp, 23 amber lamp, 24 green lamp.
 * The arm points along -X; the lamps face +Z (toward oncoming traffic). */
static b32 gen_traffic_light(A3MeshData *out) {
    MB m;
    mb_init(&m);
    static const f32 pole[][2] = { { 0.0f, 0.0f }, { 0.0f, 0.12f }, { 0.25f, 0.1f }, { 5.6f, 0.08f }, { 5.7f, 0.0f } };
    /* the lathe turns around X: build the pole along X, then rotate to Y */
    mb_lathe(&m, pole, A3_ARRAY_COUNT(pole), 12, 0.0f);
    for (u32 i = 0; i < m.v.count; ++i) {
        A3Vec3 p = m.v.data[i].position, n = m.v.data[i].normal;
        m.v.data[i].position = a3_v3(p.y, p.x, p.z);
        m.v.data[i].normal = a3_v3(n.y, n.x, n.z);
    }
    for (u32 i = 0; i + 2 < m.idx.count; i += 3) { u32 t = m.idx.data[i + 1]; m.idx.data[i + 1] = m.idx.data[i + 2]; m.idx.data[i + 2] = t; }
    mb_box(&m, a3_v3(-4.2f, 5.3f, -0.05f), a3_v3(0.0f, 5.4f, 0.05f), 0.2f);            /* arm over the road (-X) */
    for (int h = 0; h < 2; ++h) {
        f32 x = h ? -3.8f : -1.9f;
        mb_box(&m, a3_v3(x - 0.17f, 4.25f, -0.14f), a3_v3(x + 0.17f, 5.32f, 0.12f), 21.0f);
        mb_box(&m, a3_v3(x - 0.2f, 4.2f, -0.16f), a3_v3(x + 0.2f, 5.37f, -0.14f), 21.0f);   /* back plate */
        for (int l = 0; l < 3; ++l) {
            f32 y = 5.12f - l * 0.34f;
            mb_box(&m, a3_v3(x - 0.11f, y - 0.11f, 0.12f), a3_v3(x + 0.11f, y + 0.11f, 0.16f), 22.0f + l);   /* lenses face +Z */
            mb_box(&m, a3_v3(x - 0.13f, y + 0.1f, 0.12f), a3_v3(x + 0.13f, y + 0.13f, 0.28f), 21.0f);       /* visor */
        }
    }
    return mb_finish(&m, out);
}

static b32 gen_hydrant(A3MeshData *out) {
    static const f32 p[][2] = { { 0.0f, 0.0f }, { 0.0f, 0.13f }, { 0.06f, 0.13f }, { 0.08f, 0.1f }, { 0.55f, 0.1f }, { 0.58f, 0.12f }, { 0.62f, 0.12f }, { 0.66f, 0.1f }, { 0.74f, 0.07f }, { 0.78f, 0.03f }, { 0.8f, 0.0f } };
    MB m;
    mb_init(&m);
    mb_lathe(&m, p, A3_ARRAY_COUNT(p), 16, 30.0f);
    for (u32 i = 0; i < m.v.count; ++i) {
        A3Vec3 q = m.v.data[i].position, n = m.v.data[i].normal;
        m.v.data[i].position = a3_v3(q.y, q.x, q.z);
        m.v.data[i].normal = a3_v3(n.y, n.x, n.z);
    }
    for (u32 i = 0; i + 2 < m.idx.count; i += 3) { u32 t = m.idx.data[i + 1]; m.idx.data[i + 1] = m.idx.data[i + 2]; m.idx.data[i + 2] = t; }
    mb_box(&m, a3_v3(-0.17f, 0.4f, -0.04f), a3_v3(0.17f, 0.48f, 0.04f), 30.0f);      /* side outlets */
    return mb_finish(&m, out);
}

static b32 gen_bench(A3MeshData *out) {
    MB m;
    mb_init(&m);
    for (int s = 0; s < 3; ++s) mb_box(&m, a3_v3(-0.9f, 0.42f, -0.2f + s * 0.14f), a3_v3(0.9f, 0.46f, -0.09f + s * 0.14f), 31.0f);   /* seat slats */
    for (int s = 0; s < 2; ++s) mb_box(&m, a3_v3(-0.9f, 0.55f + s * 0.16f, 0.22f), a3_v3(0.9f, 0.66f + s * 0.16f, 0.26f), 31.0f);   /* back slats */
    for (int s = -1; s <= 1; s += 2) {
        mb_box(&m, a3_v3(s * 0.8f - 0.03f, 0.0f, -0.2f), a3_v3(s * 0.8f + 0.03f, 0.42f, -0.14f), 32.0f);
        mb_box(&m, a3_v3(s * 0.8f - 0.03f, 0.0f, 0.18f), a3_v3(s * 0.8f + 0.03f, 0.85f, 0.24f), 32.0f);
        mb_box(&m, a3_v3(s * 0.8f - 0.03f, 0.4f, -0.2f), a3_v3(s * 0.8f + 0.03f, 0.44f, 0.24f), 32.0f);
    }
    return mb_finish(&m, out);
}

static b32 gen_trash_can(A3MeshData *out) {
    static const f32 p[][2] = { { 0.0f, 0.0f }, { 0.0f, 0.24f }, { 0.85f, 0.27f }, { 0.9f, 0.28f }, { 0.95f, 0.26f }, { 1.0f, 0.1f }, { 1.02f, 0.0f } };
    MB m;
    mb_init(&m);
    mb_lathe(&m, p, A3_ARRAY_COUNT(p), 20, 32.0f);
    for (u32 i = 0; i < m.v.count; ++i) {
        A3Vec3 q = m.v.data[i].position, n = m.v.data[i].normal;
        m.v.data[i].position = a3_v3(q.y, q.x, q.z);
        m.v.data[i].normal = a3_v3(n.y, n.x, n.z);
    }
    for (u32 i = 0; i + 2 < m.idx.count; i += 3) { u32 t = m.idx.data[i + 1]; m.idx.data[i + 1] = m.idx.data[i + 2]; m.idx.data[i + 2] = t; }
    return mb_finish(&m, out);
}

/* Street lamp: tapered pole, curved arm, lantern head (region 0 metal, 25 lamp glass). */
static b32 gen_street_lamp(A3MeshData *out) {
    static const f32 pole[][2] = { { 0.0f, 0.0f }, { 0.0f, 0.13f }, { 0.4f, 0.11f }, { 0.5f, 0.08f }, { 7.0f, 0.055f }, { 7.1f, 0.0f } };
    MB m;
    mb_init(&m);
    mb_lathe(&m, pole, A3_ARRAY_COUNT(pole), 12, 0.0f);
    for (u32 i = 0; i < m.v.count; ++i) {
        A3Vec3 q = m.v.data[i].position, n = m.v.data[i].normal;
        m.v.data[i].position = a3_v3(q.y, q.x, q.z);
        m.v.data[i].normal = a3_v3(n.y, n.x, n.z);
    }
    for (u32 i = 0; i + 2 < m.idx.count; i += 3) { u32 t = m.idx.data[i + 1]; m.idx.data[i + 1] = m.idx.data[i + 2]; m.idx.data[i + 2] = t; }
    /* arm toward -Z, gently curving */
    for (int k = 0; k < 6; ++k) {
        f32 t0 = (f32)k / 6.0f, t1 = (f32)(k + 1) / 6.0f;
        f32 z0 = -1.4f * t0, z1 = -1.4f * t1;
        f32 y0 = 6.9f + 0.35f * a3_sinf(t0 * A3_PI * 0.8f), y1 = 6.9f + 0.35f * a3_sinf(t1 * A3_PI * 0.8f);
        mb_box(&m, a3_v3(-0.03f, a3_minf(y0, y1) - 0.03f, z1), a3_v3(0.03f, a3_maxf(y0, y1) + 0.03f, z0), 0.3f);
    }
    mb_box(&m, a3_v3(-0.22f, 6.95f, -1.75f), a3_v3(0.22f, 7.12f, -1.2f), 0.4f);       /* housing */
    mb_box(&m, a3_v3(-0.18f, 6.9f, -1.7f), a3_v3(0.18f, 6.96f, -1.25f), 25.0f);       /* lamp glass (underside) */
    return mb_finish(&m, out);
}

/* registration table used by a3_procmeshes.c */
typedef b32 (*ModelGen)(A3MeshData *);
typedef struct ModelEntry { const char *name; ModelGen fn; } ModelEntry;
static const ModelEntry k_models[] = {
    { "builtin:car_sedan", gen_car_sedan }, { "builtin:car_sports", gen_car_sports }, { "builtin:car_suv", gen_car_suv },
    { "builtin:car_hatch", gen_car_hatch }, { "builtin:car_taxi", gen_car_taxi }, { "builtin:car_police", gen_car_police },
    { "builtin:wheel_detailed", gen_wheel_detailed },
    { "builtin:palm_a", gen_palm_a }, { "builtin:palm_b", gen_palm_b }, { "builtin:palm_c", gen_palm_c },
    { "builtin:human_torso", gen_human_torso }, { "builtin:human_head", gen_human_head },
    { "builtin:human_upper_arm", gen_human_upper_arm }, { "builtin:human_forearm", gen_human_forearm },
    { "builtin:human_thigh", gen_human_thigh }, { "builtin:human_shin", gen_human_shin },
    { "builtin:traffic_light", gen_traffic_light }, { "builtin:hydrant", gen_hydrant }, { "builtin:bench", gen_bench },
    { "builtin:trash_can", gen_trash_can }, { "builtin:street_lamp", gen_street_lamp },
};

u32 a3__procmodels_count(void) { return (u32)A3_ARRAY_COUNT(k_models); }
const char *a3__procmodels_name(u32 i) { return i < A3_ARRAY_COUNT(k_models) ? k_models[i].name : ""; }
void *a3__procmodels_fn(u32 i) { return i < A3_ARRAY_COUNT(k_models) ? (void *)k_models[i].fn : 0; }

/* car dimensions for the vehicle prefab */
b32 a3_procmodels_car_info(const char *style, f32 *wheelbase, f32 *track, f32 *wheel_r, f32 *length, f32 *width, f32 *height) {
    const CarStyle *s = &k_sedan;
    if (style && a3_str_ends_with(style, "sports")) s = &k_sports;
    else if (style && a3_str_ends_with(style, "suv")) s = &k_suv;
    else if (style && a3_str_ends_with(style, "hatch")) s = &k_hatch;
    if (wheelbase) *wheelbase = s->wb;
    if (track) *track = s->width * 0.5f - 0.12f - 0.1f;
    if (wheel_r) *wheel_r = s->wheel_r;
    if (length) *length = s->wb + s->fo + s->ro;
    if (width) *width = s->width;
    if (height) *height = s->roof_h;
    return 1;
}
