/*
 * ASM3D - test_physics_kernels.c
 * The assembly physics kernels must match the C reference bit for bit.
 */
#include "a3_test.h"
#include "../engine/physics/a3_physics_kernels.h"
#include "../engine/core/a3_hash.h"
#include "../engine/core/a3_memory.h"
#include "../engine/core/a3_string.h"

static A3Vec4 rnd4(A3Rng *r, f32 s) { return a3_v4(a3_rng_range_f32(r, -s, s), a3_rng_range_f32(r, -s, s), a3_rng_range_f32(r, -s, s), 0); }

A3_TEST(physkernel_integrate) {
#if A3_HAS_X64_ASM
    A3_CHECK_STR(a3_phys_kernel_backend(), "x86-64 SSE assembly"); /* the native build really runs the asm */
#endif
    enum { N = 301 };
    A3Rng r;
    a3_rng_seed(&r, 11, 3);
    A3Vec4 *v1 = A3_NEW_ARRAY(A3Vec4, N, A3_MEM_TEMP), *v2 = A3_NEW_ARRAY(A3Vec4, N, A3_MEM_TEMP);
    A3Vec4 *w1 = A3_NEW_ARRAY(A3Vec4, N, A3_MEM_TEMP), *w2 = A3_NEW_ARRAY(A3Vec4, N, A3_MEM_TEMP);
    A3Vec4 *acc = A3_NEW_ARRAY(A3Vec4, N, A3_MEM_TEMP), *damp = A3_NEW_ARRAY(A3Vec4, N, A3_MEM_TEMP);
    A3Vec4 *p1 = A3_NEW_ARRAY(A3Vec4, N, A3_MEM_TEMP), *p2 = A3_NEW_ARRAY(A3Vec4, N, A3_MEM_TEMP);
    A3Quat *q1 = A3_NEW_ARRAY(A3Quat, N, A3_MEM_TEMP), *q2 = A3_NEW_ARRAY(A3Quat, N, A3_MEM_TEMP);
    for (u32 i = 0; i < N; ++i) {
        v1[i] = v2[i] = rnd4(&r, 20);
        w1[i] = w2[i] = rnd4(&r, 8);
        acc[i] = a3_v4(0, -9.81f * a3_rng_f32(&r), 0, 0);
        damp[i] = a3_v4(1.0f / (1.0f + a3_rng_f32(&r) * 0.05f), 1.0f / (1.0f + a3_rng_f32(&r) * 0.1f), 0, 0);
        p1[i] = p2[i] = rnd4(&r, 100);
        q1[i] = q2[i] = a3_quat_normalize(a3_quat(a3_rng_f32(&r) - 0.5f, a3_rng_f32(&r) - 0.5f, a3_rng_f32(&r) - 0.5f, a3_rng_f32(&r) - 0.5f));
    }
    q1[7] = q2[7] = a3_quat(0, 0, 0, 0); /* degenerate: must reset to identity in both */
    w1[7] = w2[7] = a3_v4(0, 0, 0, 0);
    for (int step = 0; step < 120; ++step) {
        a3_phys_integrate_velocities(v1, w1, acc, damp, 1.0f / 60.0f, N);
        a3_phys_ref_integrate_velocities(v2, w2, acc, damp, 1.0f / 60.0f, N);
        a3_phys_integrate_transforms(p1, q1, v1, w1, 1.0f / 60.0f, N);
        a3_phys_ref_integrate_transforms(p2, q2, v2, w2, 1.0f / 60.0f, N);
    }
    A3_CHECK(a3_memcmp(v1, v2, sizeof(A3Vec4) * N) == 0);
    A3_CHECK(a3_memcmp(w1, w2, sizeof(A3Vec4) * N) == 0);
    A3_CHECK(a3_memcmp(p1, p2, sizeof(A3Vec4) * N) == 0);
    A3_CHECK(a3_memcmp(q1, q2, sizeof(A3Quat) * N) == 0);
    A3_CHECK_NEAR(a3_quat_dot(q1[3], q1[3]), 1.0, 1e-5);
    A3_CHECK(q1[7].w == 1.0f);
    a3_free(v1); a3_free(v2); a3_free(w1); a3_free(w2); a3_free(acc); a3_free(damp);
    a3_free(p1); a3_free(p2); a3_free(q1); a3_free(q2);
}

A3_TEST(physkernel_solver) {
    enum { BODIES = 40, ROWS = 300 };
    A3Rng r;
    a3_rng_seed(&r, 5, 9);
    A3SolverRow *r1 = (A3SolverRow *)a3_calloc(sizeof(A3SolverRow) * ROWS, A3_MEM_TEMP);
    A3SolverRow *r2 = (A3SolverRow *)a3_calloc(sizeof(A3SolverRow) * ROWS, A3_MEM_TEMP);
    A3Vec4 *lv1 = A3_NEW_ARRAY(A3Vec4, BODIES, A3_MEM_TEMP), *lv2 = A3_NEW_ARRAY(A3Vec4, BODIES, A3_MEM_TEMP);
    A3Vec4 *av1 = A3_NEW_ARRAY(A3Vec4, BODIES, A3_MEM_TEMP), *av2 = A3_NEW_ARRAY(A3Vec4, BODIES, A3_MEM_TEMP);
    for (u32 b = 1; b < BODIES; ++b) { lv1[b] = lv2[b] = rnd4(&r, 5); av1[b] = av2[b] = rnd4(&r, 3); }
    for (u32 i = 0; i < ROWS; ++i) {
        A3SolverRow *row = &r1[i];
        b32 friction = (i % 3) != 0;
        row->lin = a3_v4_from3(a3_v3_norm(a3_v4_xyz(rnd4(&r, 1))), 0);
        row->ang_a = rnd4(&r, 1); row->ang_b = rnd4(&r, 1);
        row->ia = rnd4(&r, 0.5f); row->ib = rnd4(&r, 0.5f);
        row->body_a = (i % 5 == 0) ? 0 : (i32)a3_rng_range_u32(&r, BODIES - 1) + 1; /* 0 = static */
        row->body_b = (i32)a3_rng_range_u32(&r, BODIES - 1) + 1;
        row->inv_mass_a = row->body_a ? a3_rng_range_f32(&r, 0.1f, 2) : 0;
        if (!row->body_a) row->ia = a3_v4(0, 0, 0, 0);
        row->inv_mass_b = a3_rng_range_f32(&r, 0.1f, 2);
        row->eff_mass = a3_rng_range_f32(&r, 0.1f, 1);
        row->bias = a3_rng_range_f32(&r, -0.2f, 0.2f);
        row->lo = 0; row->hi = 1e30f; row->mu = 0.6f;
        row->friction_of = friction ? (i32)(i - (i % 3)) : -1;
    }
    a3_memcpy(r2, r1, sizeof(A3SolverRow) * ROWS);
    for (int it = 0; it < 10; ++it) {
        a3_phys_solve_rows(r1, ROWS, lv1, av1);
        a3_phys_ref_solve_rows(r2, ROWS, lv2, av2);
    }
    A3_CHECK(a3_memcmp(r1, r2, sizeof(A3SolverRow) * ROWS) == 0);
    A3_CHECK(a3_memcmp(lv1, lv2, sizeof(A3Vec4) * BODIES) == 0);
    A3_CHECK(a3_memcmp(av1, av2, sizeof(A3Vec4) * BODIES) == 0);
    A3_CHECK(lv1[0].x == 0 && lv1[0].y == 0 && lv1[0].z == 0); /* static slot untouched */
    for (u32 i = 0; i < ROWS; ++i) if (r1[i].friction_of < 0) A3_CHECK(r1[i].acc >= 0); /* normal impulses push only */
    a3_free(r1); a3_free(r2); a3_free(lv1); a3_free(lv2); a3_free(av1); a3_free(av2);
}

A3_TEST(physkernel_aabb_sap_ray) {
    enum { N = 500 };
    A3Rng r;
    a3_rng_seed(&r, 3, 3);
    A3Vec4 *c = A3_NEW_ARRAY(A3Vec4, N, A3_MEM_TEMP), *h = A3_NEW_ARRAY(A3Vec4, N, A3_MEM_TEMP), *cols = A3_NEW_ARRAY(A3Vec4, N * 3, A3_MEM_TEMP);
    A3Vec4 *mn1 = A3_NEW_ARRAY(A3Vec4, N, A3_MEM_TEMP), *mx1 = A3_NEW_ARRAY(A3Vec4, N, A3_MEM_TEMP);
    A3Vec4 *mn2 = A3_NEW_ARRAY(A3Vec4, N, A3_MEM_TEMP), *mx2 = A3_NEW_ARRAY(A3Vec4, N, A3_MEM_TEMP);
    for (u32 i = 0; i < N; ++i) {
        c[i] = rnd4(&r, 50);
        h[i] = a3_v4(a3_rng_range_f32(&r, 0.2f, 3), a3_rng_range_f32(&r, 0.2f, 3), a3_rng_range_f32(&r, 0.2f, 3), 0);
        A3Mat4 m = a3_mat4_from_quat(a3_quat_normalize(a3_quat(a3_rng_f32(&r), a3_rng_f32(&r), a3_rng_f32(&r), a3_rng_f32(&r))));
        cols[i * 3] = a3_v4(m.m[0], m.m[1], m.m[2], 0);
        cols[i * 3 + 1] = a3_v4(m.m[4], m.m[5], m.m[6], 0);
        cols[i * 3 + 2] = a3_v4(m.m[8], m.m[9], m.m[10], 0);
    }
    a3_phys_compute_aabbs(c, cols, h, mn1, mx1, N);
    a3_phys_ref_compute_aabbs(c, cols, h, mn2, mx2, N);
    A3_CHECK(a3_memcmp(mn1, mn2, sizeof(A3Vec4) * N) == 0 && a3_memcmp(mx1, mx2, sizeof(A3Vec4) * N) == 0);
    /* sort by min.x (simple insertion over a copy) */
    for (u32 i = 1; i < N; ++i) {
        A3Vec4 a = mn1[i], b = mx1[i];
        u32 j = i;
        while (j > 0 && mn1[j - 1].x > a.x) { mn1[j] = mn1[j - 1]; mx1[j] = mx1[j - 1]; --j; }
        mn1[j] = a; mx1[j] = b;
    }
    u32 *pa = A3_NEW_ARRAY(u32, 20000, A3_MEM_TEMP), *pb = A3_NEW_ARRAY(u32, 20000, A3_MEM_TEMP);
    u32 n1 = a3_phys_sap_pairs(mn1, mx1, N, pa, 10000);
    u32 n2 = a3_phys_ref_sap_pairs(mn1, mx1, N, pb, 10000);
    A3_CHECK_EQ_INT(n1, n2);
    A3_CHECK(n1 > 0 && a3_memcmp(pa, pb, sizeof(u32) * 2 * n1) == 0);
    /* brute force agreement */
    u32 brute = 0;
    for (u32 i = 0; i < N; ++i) for (u32 j = i + 1; j < N; ++j)
        if (mn1[i].x <= mx1[j].x && mn1[j].x <= mx1[i].x && mn1[i].y <= mx1[j].y && mn1[j].y <= mx1[i].y && mn1[i].z <= mx1[j].z && mn1[j].z <= mx1[i].z) brute++;
    A3_CHECK_EQ_INT(n1, brute);
    A3_CHECK_EQ_INT(a3_phys_sap_pairs(mn1, mx1, N, pa, 3), 3); /* capacity respected */
    /* rays */
    f32 *t1 = A3_NEW_ARRAY(f32, N, A3_MEM_TEMP), *t2 = A3_NEW_ARRAY(f32, N, A3_MEM_TEMP);
    for (int k = 0; k < 20; ++k) {
        A3Vec4 o = rnd4(&r, 60);
        A3Vec3 d = a3_v3_norm(a3_v4_xyz(rnd4(&r, 1)));
        if (k == 0) d = a3_v3(1, 0, 0); /* axis-aligned: infinite inverse components */
        A3Vec4 inv = a3_v4(1.0f / d.x, 1.0f / d.y, 1.0f / d.z, 0);
        u32 h1 = a3_phys_ray_aabbs(&o, &inv, 200.0f, mn1, mx1, N, t1);
        u32 h2 = a3_phys_ref_ray_aabbs(&o, &inv, 200.0f, mn1, mx1, N, t2);
        A3_CHECK_EQ_INT(h1, h2);
        A3_CHECK(a3_memcmp(t1, t2, sizeof(f32) * N) == 0);
    }
    A3Vec4 o = a3_v4(-10, 0.5f, 0.5f, 0), inv = a3_v4(1.0f, 1.0f / 0.0f, 1.0f / 0.0f, 0);
    A3Vec4 bmin = a3_v4(0, 0, 0, 0), bmax = a3_v4(1, 1, 1, 0);
    f32 t;
    A3_CHECK(a3_phys_ray_aabbs(&o, &inv, 100.0f, &bmin, &bmax, 1, &t) == 1 && t == 10.0f);
    A3_CHECK(a3_phys_ray_aabbs(&o, &inv, 5.0f, &bmin, &bmax, 1, &t) == 0 && t == -1.0f); /* beyond max distance */
    a3_free(c); a3_free(h); a3_free(cols); a3_free(mn1); a3_free(mx1); a3_free(mn2); a3_free(mx2);
    a3_free(pa); a3_free(pb); a3_free(t1); a3_free(t2);
}
