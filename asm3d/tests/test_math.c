/*
 * ASM3D - test_math.c : math library and SIMD/assembly kernels
 */
#include "a3_test.h"
#include "../engine/core/a3_string.h"
#include "../engine/core/a3_hash.h"
#include "../engine/core/a3_memory.h"

/* Golden value recorded from the native build; the wasm build must match. */
#ifndef A3_DETERMINISM_GOLDEN
#define A3_DETERMINISM_GOLDEN 0x283e8953ff45402eull
#endif

A3_TEST(math_scalar) {
    for (f32 x = -20.0f; x <= 20.0f; x += 0.37f) {
        f32 s = a3_sinf(x), c = a3_cosf(x);
        A3_CHECK_NEAR(s * s + c * c, 1.0f, 1e-6);
    }
    A3_CHECK_NEAR(a3_sinf(A3_HALF_PI), 1.0f, 1e-7);
    A3_CHECK_NEAR(a3_cosf(A3_PI), -1.0f, 1e-7);
    A3_CHECK_NEAR(a3_sinf(1.0f), 0.8414709848f, 1e-7);
    A3_CHECK_NEAR(a3_tanf(0.5f), 0.5463024898f, 1e-6);
    A3_CHECK_NEAR(a3_atanf(1.0f), A3_PI / 4, 1e-7);
    A3_CHECK_NEAR(a3_atan2f(1.0f, -1.0f), 3 * A3_PI / 4, 1e-6);
    A3_CHECK_NEAR(a3_atan2f(-1.0f, -1.0f), -3 * A3_PI / 4, 1e-6);
    A3_CHECK_NEAR(a3_asinf(0.5f), A3_PI / 6, 1e-6);
    A3_CHECK_NEAR(a3_acosf(0.0f), A3_HALF_PI, 1e-6);
    A3_CHECK_NEAR(a3_expf(1.0f), 2.718281828f, 1e-6);
    A3_CHECK_NEAR(a3_expf(-10.0f), 4.539993e-5f, 1e-10);
    A3_CHECK_NEAR(a3_logf(10.0f), 2.302585093f, 1e-6);
    A3_CHECK_NEAR(a3_log2f(1024.0f), 10.0f, 1e-5);
    A3_CHECK_NEAR(a3_powf(2.0f, 10.0f), 1024.0f, 1e-3);
    A3_CHECK_NEAR(a3_powf(-2.0f, 3.0f), -8.0f, 1e-5);
    A3_CHECK_NEAR(a3_powf(9.0f, 0.5f), 3.0f, 1e-6);
    A3_CHECK_NEAR(a3_fmodf(7.5f, 2.0f), 1.5f, 1e-6);
    A3_CHECK_NEAR(a3_fmodf(-7.5f, 2.0f), -1.5f, 1e-6);
    A3_CHECK_NEAR(a3_absf(a3_wrap_angle(3 * A3_PI)), A3_PI, 1e-5); /* +PI and -PI are the same angle */
    A3_CHECK_NEAR(a3_wrap_angle(A3_TAU + 0.5f), 0.5f, 1e-5);
    A3_CHECK_NEAR(a3_wrap_angle(-A3_PI * 0.5f), -A3_PI * 0.5f, 1e-6);
    A3_CHECK(a3_floorf(-1.5f) == -2.0f && a3_ceilf(1.2f) == 2.0f);
}

/* Hash of bit patterns produced by the deterministic scalar functions.
 * The same constant must come out on native (x86-64) and WebAssembly:
 * that is the guarantee behind reproducible procedural generation. */
A3_TEST(math_determinism_golden) {
    u64 h = 0;
    for (int i = -500; i <= 500; ++i) {
        f32 x = (f32)i * 0.0731f;
        f32 v[9] = { a3_sinf(x), a3_cosf(x), a3_tanf(x * 0.2f), a3_atan2f(x, 1.7f), a3_expf(x * 0.1f),
                     a3_logf(a3_absf(x) + 0.5f), a3_powf(a3_absf(x) + 1.0f, 1.3f), a3_sqrtf(a3_absf(x)),
                     a3_fbm2(x, x * 0.5f, 1234u, 4, 2.0f, 0.5f) };
        h = a3_hash_combine(h, a3_hash64(v, sizeof(v), 0));
    }
    A3Rng rng;
    a3_rng_seed(&rng, 2024, 1);
    for (int i = 0; i < 64; ++i) h = a3_hash_combine(h, a3_rng_u32(&rng));
    char hex[17];
    a3_hash_to_hex(h, hex);
    A3_CHECK_MSG(h == A3_DETERMINISM_GOLDEN, "determinism hash %s differs from golden", hex);
}

A3_TEST(math_vec_quat) {
    A3Vec3 a = a3_v3(1, 2, 3), b = a3_v3(4, 5, 6);
    A3_CHECK_NEAR(a3_v3_dot(a, b), 32.0f, 1e-6);
    A3Vec3 c = a3_v3_cross(a3_v3(1, 0, 0), a3_v3(0, 1, 0));
    A3_CHECK(a3_v3_nearly_equal(c, a3_v3(0, 0, 1), 1e-6f));
    A3_CHECK_NEAR(a3_v3_len(a3_v3_norm(b)), 1.0f, 1e-6);
    A3Quat q = a3_quat_axis_angle(a3_v3(0, 1, 0), A3_HALF_PI);
    A3Vec3 r = a3_quat_rotate(q, a3_v3(1, 0, 0));
    A3_CHECK(a3_v3_nearly_equal(r, a3_v3(0, 0, -1), 1e-6f));
    A3Quat e = a3_quat_euler(0.3f, -1.1f, 0.2f);
    A3Vec3 eu = a3_quat_to_euler(e);
    A3_CHECK_NEAR(eu.x, 0.3f, 1e-5);
    A3_CHECK_NEAR(eu.y, -1.1f, 1e-5);
    A3_CHECK_NEAR(eu.z, 0.2f, 1e-5);
    A3Quat inv = a3_quat_inverse(e);
    A3Quat id = a3_quat_mul(e, inv);
    A3_CHECK_NEAR(id.w, 1.0f, 1e-6);
    A3Quat s0 = a3_quat_identity(), s1 = a3_quat_axis_angle(a3_v3(0, 0, 1), 1.0f);
    A3Quat mid = a3_quat_slerp(s0, s1, 0.5f);
    A3Quat expect = a3_quat_axis_angle(a3_v3(0, 0, 1), 0.5f);
    A3_CHECK_NEAR(a3_quat_dot(mid, expect), 1.0f, 1e-6);
    A3Quat look = a3_quat_look_rotation(a3_v3(1, 0, 0), a3_v3(0, 1, 0));
    A3_CHECK(a3_v3_nearly_equal(a3_quat_rotate(look, a3_v3(0, 0, -1)), a3_v3(1, 0, 0), 1e-5f));
}

A3_TEST(math_mat4) {
    A3Transform t = { a3_v3(1, 2, 3), a3_quat_euler(0.4f, 0.8f, -0.3f), a3_v3(2, 2, 2) };
    A3Mat4 m = a3_mat4_from_transform(&t);
    A3Vec3 p = a3_v3(0.5f, -1.0f, 2.0f);
    A3_CHECK(a3_v3_nearly_equal(a3_mat4_mul_point(&m, p), a3_transform_point(&t, p), 1e-5f));
    A3Mat4 inv;
    A3_CHECK(a3_mat4_inverse(&m, &inv));
    A3Mat4 ident = a3_mat4_mul(&m, &inv);
    A3Mat4 I = a3_mat4_identity();
    A3_CHECK(a3_mat4_nearly_equal(&ident, &I, 1e-5f));
    A3Vec3 dt; A3Quat dr; A3Vec3 ds;
    a3_mat4_decompose(&m, &dt, &dr, &ds);
    A3_CHECK(a3_v3_nearly_equal(dt, t.position, 1e-5f));
    A3_CHECK(a3_v3_nearly_equal(ds, t.scale, 1e-5f));
    A3_CHECK_NEAR(a3_absf(a3_quat_dot(dr, t.rotation)), 1.0f, 1e-5);
    A3Mat4 zero = { { 0 } };
    A3_CHECK(!a3_mat4_inverse(&zero, &inv)); /* singular reported, not crash */
    /* perspective: point on near plane maps to -1 (GL) and 0 (zero-to-one) */
    A3Mat4 pgl = a3_mat4_perspective(1.0f, 1.5f, 0.1f, 100.0f, 0);
    A3Mat4 pzo = a3_mat4_perspective(1.0f, 1.5f, 0.1f, 100.0f, 1);
    A3_CHECK_NEAR(a3_mat4_mul_point_project(&pgl, a3_v3(0, 0, -0.1f)).z, -1.0f, 1e-4);
    A3_CHECK_NEAR(a3_mat4_mul_point_project(&pzo, a3_v3(0, 0, -0.1f)).z, 0.0f, 1e-4);
    A3_CHECK_NEAR(a3_mat4_mul_point_project(&pzo, a3_v3(0, 0, -100.0f)).z, 1.0f, 1e-4);
    A3Mat4 view = a3_mat4_look_at(a3_v3(0, 0, 5), a3_v3(0, 0, 0), a3_v3(0, 1, 0));
    A3_CHECK(a3_v3_nearly_equal(a3_mat4_mul_point(&view, a3_v3(0, 0, 0)), a3_v3(0, 0, -5), 1e-5f));
}

A3_TEST(math_geometry) {
    A3Mat4 proj = a3_mat4_perspective(A3_HALF_PI, 1.0f, 0.1f, 100.0f, 0);
    A3Mat4 view = a3_mat4_look_at(a3_v3(0, 0, 0), a3_v3(0, 0, -1), a3_v3(0, 1, 0));
    A3Mat4 vp = a3_mat4_mul(&proj, &view);
    A3Frustum f = a3_frustum_from_matrix(&vp, 0);
    A3_CHECK(a3_frustum_test_sphere(&f, a3_v3(0, 0, -10), 1));
    A3_CHECK(!a3_frustum_test_sphere(&f, a3_v3(0, 0, 10), 1));      /* behind */
    A3_CHECK(!a3_frustum_test_sphere(&f, a3_v3(0, 0, -200), 1));    /* beyond far */
    A3_CHECK(!a3_frustum_test_sphere(&f, a3_v3(50, 0, -10), 1));    /* outside right */
    A3_CHECK(a3_frustum_test_sphere(&f, a3_v3(10.5f, 0, -10), 1));  /* straddles edge */
    A3_CHECK(a3_frustum_test_aabb(&f, a3_aabb(a3_v3(-1, -1, -11), a3_v3(1, 1, -9))));
    A3_CHECK(!a3_frustum_test_aabb(&f, a3_aabb(a3_v3(-1, -1, 9), a3_v3(1, 1, 11))));
    A3Ray ray = { a3_v3(0, 0, 10), a3_v3(0, 0, -1) };
    f32 t;
    A3_CHECK(a3_ray_aabb(ray, a3_aabb(a3_v3(-1, -1, -1), a3_v3(1, 1, 1)), &t) && a3_nearly_equal(t, 9, 1e-5f));
    A3_CHECK(a3_ray_sphere(ray, a3_v3(0, 0, 0), 2, &t) && a3_nearly_equal(t, 8, 1e-5f));
    A3Plane ground = { a3_v3(0, 1, 0), 0 };
    A3Ray down = { a3_v3(3, 5, 1), a3_v3(0, -1, 0) };
    A3_CHECK(a3_ray_plane(down, ground, &t) && a3_nearly_equal(t, 5, 1e-5f));
    A3_CHECK(a3_ray_triangle(ray, a3_v3(-1, -1, 0), a3_v3(1, -1, 0), a3_v3(0, 1, 0), &t) && a3_nearly_equal(t, 10, 1e-5f));
    A3Ray miss = { a3_v3(5, 5, 10), a3_v3(0, 0, -1) };
    A3_CHECK(!a3_ray_triangle(miss, a3_v3(-1, -1, 0), a3_v3(1, -1, 0), a3_v3(0, 1, 0), &t));
    A3Mat4 rot = a3_mat4_from_quat(a3_quat_axis_angle(a3_v3(0, 1, 0), A3_PI / 4));
    A3Aabb tb = a3_aabb_transform(a3_aabb(a3_v3(-1, -1, -1), a3_v3(1, 1, 1)), &rot);
    A3_CHECK_NEAR(tb.max.x, 1.41421356f, 1e-5);
}

/* ---- SIMD / assembly kernels vs the C reference: exact equality ---- */

static void fill_mats(A3Mat4 *m, u32 n, u32 seed) {
    A3Rng rng;
    a3_rng_seed(&rng, seed, 3);
    for (u32 i = 0; i < n; ++i)
        for (int k = 0; k < 16; ++k) m[i].m[k] = a3_rng_range_f32(&rng, -10.0f, 10.0f);
}

A3_TEST(simd_mat4_matches_reference) {
    enum { N = 257 };
    A3Mat4 *a = A3_NEW_ARRAY(A3Mat4, N, A3_MEM_TEMP), *b = A3_NEW_ARRAY(A3Mat4, N, A3_MEM_TEMP);
    A3Mat4 *out = A3_NEW_ARRAY(A3Mat4, N, A3_MEM_TEMP), *ref = A3_NEW_ARRAY(A3Mat4, N, A3_MEM_TEMP);
    fill_mats(a, N, 1);
    fill_mats(b, N, 2);
    a3_mat4_mul_batch(out, a, b, N);
    for (u32 i = 0; i < N; ++i) a3_ref_mat4_mul(&ref[i], &a[i], &b[i]);
    A3_CHECK(a3_memcmp(out, ref, sizeof(A3Mat4) * N) == 0);
    a3_mat4_mul_one_many(out, &a[0], b, N);
    for (u32 i = 0; i < N; ++i) a3_ref_mat4_mul(&ref[i], &a[0], &b[i]);
    A3_CHECK(a3_memcmp(out, ref, sizeof(A3Mat4) * N) == 0);
    /* aliasing: out == b */
    A3Mat4 x = a[5], y = b[5], r;
    a3_ref_mat4_mul(&r, &x, &y);
    a3_mat4_mul_batch(&y, &x, &y, 1);
    A3_CHECK(a3_memcmp(&y, &r, sizeof(r)) == 0);
    a3_mat4_mul_batch(out, a, b, 0); /* zero count is a no-op */
    a3_free(a); a3_free(b); a3_free(out); a3_free(ref);
}

A3_TEST(simd_transform_matches_reference) {
    enum { N = 1001 };
    A3Vec4 *in = A3_NEW_ARRAY(A3Vec4, N, A3_MEM_TEMP), *o1 = A3_NEW_ARRAY(A3Vec4, N, A3_MEM_TEMP), *o2 = A3_NEW_ARRAY(A3Vec4, N, A3_MEM_TEMP);
    A3Rng rng;
    a3_rng_seed(&rng, 9, 9);
    for (u32 i = 0; i < N; ++i) in[i] = a3_v4(a3_rng_range_f32(&rng, -100, 100), a3_rng_range_f32(&rng, -100, 100), a3_rng_range_f32(&rng, -100, 100), 1.0f);
    A3Mat4 m;
    fill_mats(&m, 1, 77);
    a3_transform_points(o1, &m, in, N);
    a3_ref_transform_points(o2, &m, in, N);
    A3_CHECK(a3_memcmp(o1, o2, sizeof(A3Vec4) * N) == 0);
    a3_free(in); a3_free(o1); a3_free(o2);
}

A3_TEST(simd_cull_matches_reference) {
    enum { N = 4099 };
    A3Vec4 *s = A3_NEW_ARRAY(A3Vec4, N, A3_MEM_TEMP);
    u8 *v1 = (u8 *)a3_calloc(N, A3_MEM_TEMP), *v2 = (u8 *)a3_calloc(N, A3_MEM_TEMP);
    A3Rng rng;
    a3_rng_seed(&rng, 5, 5);
    for (u32 i = 0; i < N; ++i) s[i] = a3_v4(a3_rng_range_f32(&rng, -150, 150), a3_rng_range_f32(&rng, -50, 50), a3_rng_range_f32(&rng, -150, 150), a3_rng_range_f32(&rng, 0.1f, 5));
    A3Mat4 proj = a3_mat4_perspective(1.2f, 16.0f / 9.0f, 0.1f, 120.0f, 0);
    A3Mat4 view = a3_mat4_look_at(a3_v3(3, 2, 8), a3_v3(0, 0, -20), a3_v3(0, 1, 0));
    A3Mat4 vp = a3_mat4_mul(&proj, &view);
    A3Frustum f = a3_frustum_from_matrix(&vp, 0);
    u32 n1 = a3_cull_spheres(&f, s, v1, N);
    u32 n2 = a3_ref_cull_spheres(&f, s, v2, N);
    A3_CHECK_EQ_INT(n1, n2);
    A3_CHECK(a3_memcmp(v1, v2, N) == 0);
    A3_CHECK(n1 > 0 && n1 < N); /* scene is partially visible */
    a3_free(s); a3_free(v1); a3_free(v2);
}

A3_TEST(simd_integrate_matches_reference) {
    enum { N = 333 };
    A3Vec4 *p1 = A3_NEW_ARRAY(A3Vec4, N, A3_MEM_TEMP), *p2 = A3_NEW_ARRAY(A3Vec4, N, A3_MEM_TEMP), *v = A3_NEW_ARRAY(A3Vec4, N, A3_MEM_TEMP);
    A3Rng rng;
    a3_rng_seed(&rng, 1, 1);
    for (u32 i = 0; i < N; ++i) {
        p1[i] = p2[i] = a3_v4(a3_rng_f32(&rng), a3_rng_f32(&rng), a3_rng_f32(&rng), 0);
        v[i] = a3_v4(a3_rng_range_f32(&rng, -5, 5), a3_rng_range_f32(&rng, -5, 5), a3_rng_range_f32(&rng, -5, 5), 1);
    }
    for (int step = 0; step < 60; ++step) {
        a3_integrate_v4(p1, v, 1.0f / 60.0f, N);
        a3_ref_integrate_v4(p2, v, 1.0f / 60.0f, N);
    }
    A3_CHECK(a3_memcmp(p1, p2, sizeof(A3Vec4) * N) == 0);
    a3_free(p1); a3_free(p2); a3_free(v);
}
