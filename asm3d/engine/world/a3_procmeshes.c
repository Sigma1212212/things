/*
 * ASM3D - a3_procmeshes.c
 */
#include "a3_procmeshes.h"
#include "../modeling/a3_emesh.h"
#include "../render/a3_mesh.h"
#include "../resource/a3_assets.h"
#include "../core/a3_math.h"

/* Adds a closed loft: `n` rings of `k` points each (ring points in a
 * consistent order), side quads between rings and n-gon caps at both ends.
 * Face orientation is fixed afterwards by a3_emesh_recalc_normals. */
static void loft(A3EMesh *m, const A3Vec3 *pts, u32 n, u32 k) {
    u32 base = a3_emesh_vertex_count(m);
    for (u32 i = 0; i < n * k; ++i) a3_emesh_add_vertex(m, pts[i]);
    for (u32 i = 0; i + 1 < n; ++i) {
        for (u32 j = 0; j < k; ++j) {
            u32 j1 = (j + 1) % k;
            u32 q[4] = { base + i * k + j, base + i * k + j1, base + (i + 1) * k + j1, base + (i + 1) * k + j };
            a3_emesh_add_face(m, q, 4);
        }
    }
    u32 cap[32];
    if (k > 32) return;
    for (u32 j = 0; j < k; ++j) cap[j] = base + (k - 1 - j);
    a3_emesh_add_face(m, cap, k);
    for (u32 j = 0; j < k; ++j) cap[j] = base + (n - 1) * k + j;
    a3_emesh_add_face(m, cap, k);
}

static void box(A3EMesh *m, A3Vec3 lo, A3Vec3 hi) {
    A3Vec3 p[8] = {
        { lo.x, lo.y, lo.z }, { hi.x, lo.y, lo.z }, { hi.x, hi.y, lo.z }, { lo.x, hi.y, lo.z },
        { lo.x, lo.y, hi.z }, { hi.x, lo.y, hi.z }, { hi.x, hi.y, hi.z }, { lo.x, hi.y, hi.z },
    };
    loft(m, p, 2, 4);
}

static b32 finish(A3EMesh *m, A3MeshData *out) {
    a3_emesh_recalc_normals(m);
    b32 ok = a3_emesh_to_mesh_data(m, out);
    a3_emesh_free(m);
    return ok;
}

static b32 gen_palm_crown(A3MeshData *out) {
    A3EMesh m;
    a3_emesh_init(&m);
    m.smooth = 0;
    enum { FRONDS = 9, SECTIONS = 8 };
    static const f32 tilt[3] = { 0.42f, 0.05f, -0.12f };
    A3Vec3 up = a3_v3(0, 1, 0);
    for (u32 f = 0; f < FRONDS; ++f) {
        f32 yaw = (f32)f / FRONDS * A3_TAU + (f & 1) * 0.21f;
        A3Vec3 d = a3_v3(a3_cosf(yaw), 0, a3_sinf(yaw));
        A3Vec3 s = a3_v3(-d.z, 0, d.x);
        f32 len = 0.85f + 0.12f * (f32)((f * 7) % 3) / 2.0f;
        f32 lift = tilt[f % 3];
        A3Vec3 pts[SECTIONS * 4];
        for (u32 i = 0; i < SECTIONS; ++i) {
            f32 t = (f32)i / (SECTIONS - 1);
            A3Vec3 p = a3_v3_add(a3_v3_scale(d, len * t), a3_v3_scale(up, (0.2f + lift) * t - 0.62f * t * t));
            f32 w = 0.17f * a3_sinf(A3_PI * a3_minf(t * 1.15f + 0.08f, 1.0f)) + 0.012f;
            f32 sag = -0.4f * w;
            pts[i * 4 + 0] = a3_v3_add(a3_v3_add(p, a3_v3_scale(s, w)), a3_v3(0, sag, 0));
            pts[i * 4 + 1] = a3_v3_add(p, a3_v3(0, 0.022f, 0));
            pts[i * 4 + 2] = a3_v3_add(a3_v3_add(p, a3_v3_scale(s, -w)), a3_v3(0, sag, 0));
            pts[i * 4 + 3] = a3_v3_add(p, a3_v3(0, -0.018f, 0));
        }
        loft(&m, pts, SECTIONS, 4);
    }
    /* heart of the crown and a few coconuts */
    a3_emesh_add_primitive(&m, A3_EPRIM_SPHERE, 0.2f, 8, 5, a3_v3(0, 0.02f, 0));
    for (u32 c = 0; c < 3; ++c) {
        f32 a = (f32)c / 3.0f * A3_TAU + 0.5f;
        a3_emesh_add_primitive(&m, A3_EPRIM_SPHERE, 0.1f, 6, 4, a3_v3(a3_cosf(a) * 0.08f, -0.08f, a3_sinf(a) * 0.08f));
    }
    return finish(&m, out);
}

/* One side profile section of the car hull. */
typedef struct HullSec { f32 z, bottom, top, half_width; } HullSec;

static b32 gen_car_body(A3MeshData *out) {
    A3EMesh m;
    a3_emesh_init(&m);
    static const HullSec secs[] = {
        { -2.20f, 0.34f, 0.74f, 0.80f },
        { -2.05f, 0.28f, 0.84f, 0.88f },
        { -1.20f, 0.25f, 0.95f, 0.90f },
        {  1.30f, 0.25f, 0.99f, 0.90f },
        {  2.05f, 0.28f, 0.94f, 0.88f },
        {  2.20f, 0.34f, 0.84f, 0.82f },
    };
    enum { N = sizeof(secs) / sizeof(secs[0]), K = 6 };
    A3Vec3 pts[N * K];
    for (u32 i = 0; i < N; ++i) {
        const HullSec *h = &secs[i];
        f32 mid = h->bottom + 0.6f * (h->top - h->bottom);
        pts[i * K + 0] = a3_v3(h->half_width, h->bottom, h->z);
        pts[i * K + 1] = a3_v3(h->half_width, mid, h->z);
        pts[i * K + 2] = a3_v3(h->half_width * 0.93f, h->top, h->z);
        pts[i * K + 3] = a3_v3(-h->half_width * 0.93f, h->top, h->z);
        pts[i * K + 4] = a3_v3(-h->half_width, mid, h->z);
        pts[i * K + 5] = a3_v3(-h->half_width, h->bottom, h->z);
    }
    loft(&m, pts, N, K);
    box(&m, a3_v3(-0.66f, 1.40f, -0.12f), a3_v3(0.66f, 1.46f, 0.92f));    /* roof */
    box(&m, a3_v3(-0.72f, 0.96f, 2.08f), a3_v3(0.72f, 1.02f, 2.22f));     /* rear lip */
    return finish(&m, out);
}

static b32 gen_car_glass(A3MeshData *out) {
    A3EMesh m;
    a3_emesh_init(&m);
    static const f32 secs[][3] = {   /* z, top, half width at the top */
        { -1.05f, 0.93f, 0.80f },
        { -0.15f, 1.42f, 0.68f },
        {  0.95f, 1.42f, 0.68f },
        {  1.55f, 0.97f, 0.80f },
    };
    A3Vec3 pts[4 * 4];
    for (u32 i = 0; i < 4; ++i) {
        f32 z = secs[i][0], top = secs[i][1], hw = secs[i][2];
        pts[i * 4 + 0] = a3_v3(0.84f, 0.88f, z);
        pts[i * 4 + 1] = a3_v3(hw, top, z);
        pts[i * 4 + 2] = a3_v3(-hw, top, z);
        pts[i * 4 + 3] = a3_v3(-0.84f, 0.88f, z);
    }
    loft(&m, pts, 4, 4);
    return finish(&m, out);
}

static b32 gen_wheel(A3MeshData *out) {
    A3EMesh m;
    a3_emesh_init(&m);
    enum { SEG = 18 };
    A3Vec3 pts[4 * SEG];
    static const f32 ring[4][2] = { { -0.12f, 0.30f }, { -0.10f, 0.34f }, { 0.10f, 0.34f }, { 0.12f, 0.30f } }; /* x, radius */
    for (u32 r = 0; r < 4; ++r)
        for (u32 i = 0; i < SEG; ++i) {
            f32 a = (f32)i / SEG * A3_TAU;
            pts[r * SEG + i] = a3_v3(ring[r][0], a3_cosf(a) * ring[r][1], a3_sinf(a) * ring[r][1]);
        }
    loft(&m, pts, 4, SEG);
    return finish(&m, out);
}

typedef struct ProcMesh { const char *name; A3MeshGenerator fn; } ProcMesh;
static const ProcMesh k_meshes[] = {
    { "builtin:palm_crown", gen_palm_crown },
    { "builtin:car_body", gen_car_body },
    { "builtin:car_glass", gen_car_glass },
    { "builtin:wheel", gen_wheel },
};

/* detailed models live in a3_procmodels.c */
u32 a3__procmodels_count(void);
const char *a3__procmodels_name(u32 i);
void *a3__procmodels_fn(u32 i);

void a3_procmeshes_register(void) {
    for (u32 i = 0; i < A3_ARRAY_COUNT(k_meshes); ++i) a3_assets_register_mesh_generator(k_meshes[i].name, k_meshes[i].fn);
    for (u32 i = 0; i < a3__procmodels_count(); ++i) a3_assets_register_mesh_generator(a3__procmodels_name(i), (A3MeshGenerator)a3__procmodels_fn(i));
}

u32 a3_procmeshes_count(void) { return (u32)A3_ARRAY_COUNT(k_meshes) + a3__procmodels_count(); }
const char *a3_procmeshes_name(u32 i) { return i < A3_ARRAY_COUNT(k_meshes) ? k_meshes[i].name : a3__procmodels_name(i - (u32)A3_ARRAY_COUNT(k_meshes)); }
