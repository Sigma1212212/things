/*
 * ASM3D - a3_math.h
 * Vector / matrix / quaternion math.
 *
 * Conventions: right-handed, +Y up, -Z forward, column-major matrices,
 * column vectors (p' = M * p). Angles in radians.
 *
 * Scalar transcendental functions (a3_sinf, ...) are implemented by the engine
 * instead of libm so that native and WebAssembly builds produce bit-identical
 * results. Builds use -ffp-contract=off to prevent FMA contraction differences.
 * This is what makes seed-based procedural generation reproducible everywhere.
 */
#ifndef A3_MATH_H
#define A3_MATH_H

#include "a3_base.h"

A3_EXTERN_C_BEGIN

#define A3_PI 3.14159265358979323846f
#define A3_TAU 6.28318530717958647692f
#define A3_HALF_PI 1.57079632679489661923f
#define A3_DEG2RAD (A3_PI / 180.0f)
#define A3_RAD2DEG (180.0f / A3_PI)
#define A3_EPSILON 1e-6f
#define A3_F32_MAX 3.402823466e+38f
#define A3_INF (__builtin_inff())

/* ---- Scalars (deterministic) ---- */
f32 a3_sinf(f32 x);
f32 a3_cosf(f32 x);
void a3_sincosf(f32 x, f32 *s, f32 *c);
f32 a3_tanf(f32 x);
f32 a3_atanf(f32 x);
f32 a3_atan2f(f32 y, f32 x);
f32 a3_asinf(f32 x);
f32 a3_acosf(f32 x);
f32 a3_expf(f32 x);
f32 a3_logf(f32 x);
f32 a3_log2f(f32 x);
f32 a3_powf(f32 x, f32 y);
f32 a3_fmodf(f32 x, f32 y);

A3_INLINE f32 a3_sqrtf(f32 x) { return __builtin_sqrtf(x); }  /* IEEE exact on all targets */
A3_INLINE f32 a3_absf(f32 x) { return __builtin_fabsf(x); }
A3_INLINE f32 a3_floorf(f32 x) { return __builtin_floorf(x); }
A3_INLINE f32 a3_ceilf(f32 x) { return __builtin_ceilf(x); }
A3_INLINE f32 a3_truncf(f32 x) { return __builtin_truncf(x); }
A3_INLINE f32 a3_roundf(f32 x) { return a3_floorf(x + 0.5f); }
A3_INLINE f32 a3_minf(f32 a, f32 b) { return a < b ? a : b; }
A3_INLINE f32 a3_maxf(f32 a, f32 b) { return a > b ? a : b; }
A3_INLINE f32 a3_clampf(f32 x, f32 lo, f32 hi) { return x < lo ? lo : (x > hi ? hi : x); }
A3_INLINE f32 a3_saturate(f32 x) { return a3_clampf(x, 0.0f, 1.0f); }
A3_INLINE i32 a3_mini(i32 a, i32 b) { return a < b ? a : b; }
A3_INLINE i32 a3_maxi(i32 a, i32 b) { return a > b ? a : b; }
A3_INLINE i32 a3_clampi(i32 x, i32 lo, i32 hi) { return x < lo ? lo : (x > hi ? hi : x); }
A3_INLINE u32 a3_minu(u32 a, u32 b) { return a < b ? a : b; }
A3_INLINE u32 a3_maxu(u32 a, u32 b) { return a > b ? a : b; }
A3_INLINE f32 a3_lerpf(f32 a, f32 b, f32 t) { return a + (b - a) * t; }
A3_INLINE f32 a3_signf(f32 x) { return x > 0.0f ? 1.0f : (x < 0.0f ? -1.0f : 0.0f); }
A3_INLINE f32 a3_smoothstep(f32 e0, f32 e1, f32 x) { f32 t = a3_saturate((x - e0) / (e1 - e0)); return t * t * (3.0f - 2.0f * t); }
A3_INLINE b32 a3_nearly_equal(f32 a, f32 b, f32 eps) { return a3_absf(a - b) <= eps; }
A3_INLINE f32 a3_rsqrtf(f32 x) { return 1.0f / a3_sqrtf(x); }
/* Wraps an angle into [-PI, PI]. */
f32 a3_wrap_angle(f32 a);
/* Frame-rate independent exponential smoothing factor. */
A3_INLINE f32 a3_damp_factor(f32 sharpness, f32 dt) { return 1.0f - a3_expf(-sharpness * dt); }

/* ---- Types ---- */
typedef struct A3Vec2 { f32 x, y; } A3Vec2;
typedef struct A3Vec3 { f32 x, y, z; } A3Vec3;
typedef struct A3_ALIGNAS(16) A3Vec4 { f32 x, y, z, w; } A3Vec4;
typedef struct A3_ALIGNAS(16) A3Quat { f32 x, y, z, w; } A3Quat;
/* Column-major: m[col * 4 + row]. */
typedef struct A3_ALIGNAS(16) A3Mat4 { f32 m[16]; } A3Mat4;
typedef struct A3IVec2 { i32 x, y; } A3IVec2;
typedef struct A3IVec3 { i32 x, y, z; } A3IVec3;

typedef struct A3Transform {
    A3Vec3 position;
    A3Quat rotation;
    A3Vec3 scale;
} A3Transform;

typedef struct A3Aabb { A3Vec3 min, max; } A3Aabb;
typedef struct A3Sphere { A3Vec3 center; f32 radius; } A3Sphere;
typedef struct A3Plane { A3Vec3 n; f32 d; } A3Plane; /* dot(n, p) + d = 0 */
typedef struct A3Ray { A3Vec3 origin, dir; } A3Ray;
/* Planes stored as vec4 (nx, ny, nz, d), normals pointing inward. Order:
 * left, right, bottom, top, near, far. */
typedef struct A3_ALIGNAS(16) A3Frustum { A3Vec4 planes[6]; } A3Frustum;

/* ---- Vec2 ---- */
A3_INLINE A3Vec2 a3_v2(f32 x, f32 y) { A3Vec2 r = { x, y }; return r; }
A3_INLINE A3Vec2 a3_v2_add(A3Vec2 a, A3Vec2 b) { return a3_v2(a.x + b.x, a.y + b.y); }
A3_INLINE A3Vec2 a3_v2_sub(A3Vec2 a, A3Vec2 b) { return a3_v2(a.x - b.x, a.y - b.y); }
A3_INLINE A3Vec2 a3_v2_scale(A3Vec2 a, f32 s) { return a3_v2(a.x * s, a.y * s); }
A3_INLINE f32 a3_v2_dot(A3Vec2 a, A3Vec2 b) { return a.x * b.x + a.y * b.y; }
A3_INLINE f32 a3_v2_len(A3Vec2 a) { return a3_sqrtf(a3_v2_dot(a, a)); }
A3_INLINE A3Vec2 a3_v2_norm(A3Vec2 a) { f32 l = a3_v2_len(a); return l > A3_EPSILON ? a3_v2_scale(a, 1.0f / l) : a3_v2(0, 0); }
A3_INLINE A3Vec2 a3_v2_lerp(A3Vec2 a, A3Vec2 b, f32 t) { return a3_v2(a3_lerpf(a.x, b.x, t), a3_lerpf(a.y, b.y, t)); }

/* ---- Vec3 ---- */
A3_INLINE A3Vec3 a3_v3(f32 x, f32 y, f32 z) { A3Vec3 r = { x, y, z }; return r; }
A3_INLINE A3Vec3 a3_v3s(f32 s) { return a3_v3(s, s, s); }
A3_INLINE A3Vec3 a3_v3_zero(void) { return a3_v3(0, 0, 0); }
A3_INLINE A3Vec3 a3_v3_one(void) { return a3_v3(1, 1, 1); }
A3_INLINE A3Vec3 a3_v3_up(void) { return a3_v3(0, 1, 0); }
A3_INLINE A3Vec3 a3_v3_forward(void) { return a3_v3(0, 0, -1); }
A3_INLINE A3Vec3 a3_v3_right(void) { return a3_v3(1, 0, 0); }
A3_INLINE A3Vec3 a3_v3_add(A3Vec3 a, A3Vec3 b) { return a3_v3(a.x + b.x, a.y + b.y, a.z + b.z); }
A3_INLINE A3Vec3 a3_v3_sub(A3Vec3 a, A3Vec3 b) { return a3_v3(a.x - b.x, a.y - b.y, a.z - b.z); }
A3_INLINE A3Vec3 a3_v3_mul(A3Vec3 a, A3Vec3 b) { return a3_v3(a.x * b.x, a.y * b.y, a.z * b.z); }
A3_INLINE A3Vec3 a3_v3_div(A3Vec3 a, A3Vec3 b) { return a3_v3(a.x / b.x, a.y / b.y, a.z / b.z); }
A3_INLINE A3Vec3 a3_v3_scale(A3Vec3 a, f32 s) { return a3_v3(a.x * s, a.y * s, a.z * s); }
A3_INLINE A3Vec3 a3_v3_neg(A3Vec3 a) { return a3_v3(-a.x, -a.y, -a.z); }
A3_INLINE A3Vec3 a3_v3_madd(A3Vec3 a, A3Vec3 b, f32 s) { return a3_v3(a.x + b.x * s, a.y + b.y * s, a.z + b.z * s); }
A3_INLINE f32 a3_v3_dot(A3Vec3 a, A3Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
A3_INLINE A3Vec3 a3_v3_cross(A3Vec3 a, A3Vec3 b) { return a3_v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
A3_INLINE f32 a3_v3_len_sq(A3Vec3 a) { return a3_v3_dot(a, a); }
A3_INLINE f32 a3_v3_len(A3Vec3 a) { return a3_sqrtf(a3_v3_dot(a, a)); }
A3_INLINE f32 a3_v3_dist(A3Vec3 a, A3Vec3 b) { return a3_v3_len(a3_v3_sub(a, b)); }
A3_INLINE f32 a3_v3_dist_sq(A3Vec3 a, A3Vec3 b) { return a3_v3_len_sq(a3_v3_sub(a, b)); }
A3_INLINE A3Vec3 a3_v3_norm(A3Vec3 a) { f32 l = a3_v3_len(a); return l > A3_EPSILON ? a3_v3_scale(a, 1.0f / l) : a3_v3(0, 0, 0); }
A3_INLINE A3Vec3 a3_v3_lerp(A3Vec3 a, A3Vec3 b, f32 t) { return a3_v3(a3_lerpf(a.x, b.x, t), a3_lerpf(a.y, b.y, t), a3_lerpf(a.z, b.z, t)); }
A3_INLINE A3Vec3 a3_v3_min(A3Vec3 a, A3Vec3 b) { return a3_v3(a3_minf(a.x, b.x), a3_minf(a.y, b.y), a3_minf(a.z, b.z)); }
A3_INLINE A3Vec3 a3_v3_max(A3Vec3 a, A3Vec3 b) { return a3_v3(a3_maxf(a.x, b.x), a3_maxf(a.y, b.y), a3_maxf(a.z, b.z)); }
A3_INLINE A3Vec3 a3_v3_abs(A3Vec3 a) { return a3_v3(a3_absf(a.x), a3_absf(a.y), a3_absf(a.z)); }
A3_INLINE b32 a3_v3_nearly_equal(A3Vec3 a, A3Vec3 b, f32 eps) { return a3_nearly_equal(a.x, b.x, eps) && a3_nearly_equal(a.y, b.y, eps) && a3_nearly_equal(a.z, b.z, eps); }
A3_INLINE A3Vec3 a3_v3_reflect(A3Vec3 v, A3Vec3 n) { return a3_v3_sub(v, a3_v3_scale(n, 2.0f * a3_v3_dot(v, n))); }
A3_INLINE A3Vec3 a3_v3_project_on_plane(A3Vec3 v, A3Vec3 n) { return a3_v3_sub(v, a3_v3_scale(n, a3_v3_dot(v, n))); }
A3_INLINE f32 a3_v3_max_comp(A3Vec3 a) { return a3_maxf(a.x, a3_maxf(a.y, a.z)); }

/* ---- Vec4 ---- */
A3_INLINE A3Vec4 a3_v4(f32 x, f32 y, f32 z, f32 w) { A3Vec4 r = { x, y, z, w }; return r; }
A3_INLINE A3Vec4 a3_v4_from3(A3Vec3 v, f32 w) { return a3_v4(v.x, v.y, v.z, w); }
A3_INLINE A3Vec3 a3_v4_xyz(A3Vec4 v) { return a3_v3(v.x, v.y, v.z); }
A3_INLINE A3Vec4 a3_v4_add(A3Vec4 a, A3Vec4 b) { return a3_v4(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w); }
A3_INLINE A3Vec4 a3_v4_scale(A3Vec4 a, f32 s) { return a3_v4(a.x * s, a.y * s, a.z * s, a.w * s); }
A3_INLINE f32 a3_v4_dot(A3Vec4 a, A3Vec4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
A3_INLINE A3Vec4 a3_v4_lerp(A3Vec4 a, A3Vec4 b, f32 t) { return a3_v4(a3_lerpf(a.x, b.x, t), a3_lerpf(a.y, b.y, t), a3_lerpf(a.z, b.z, t), a3_lerpf(a.w, b.w, t)); }

/* ---- Color ----
 * Colors shown in the editor (color pickers, textures) are sRGB. Lighting
 * math happens in linear space, so colors are converted on the way in. */
f32 a3_srgb_to_linear(f32 c);
f32 a3_linear_to_srgb(f32 c);
A3_INLINE A3Vec4 a3_color_to_linear(A3Vec4 c) { return a3_v4(a3_srgb_to_linear(c.x), a3_srgb_to_linear(c.y), a3_srgb_to_linear(c.z), c.w); }
A3_INLINE A3Vec3 a3_color3_to_linear(A3Vec4 c) { return a3_v3(a3_srgb_to_linear(c.x), a3_srgb_to_linear(c.y), a3_srgb_to_linear(c.z)); }

/* ---- Quaternion ---- */
A3_INLINE A3Quat a3_quat(f32 x, f32 y, f32 z, f32 w) { A3Quat q = { x, y, z, w }; return q; }
A3_INLINE A3Quat a3_quat_identity(void) { return a3_quat(0, 0, 0, 1); }
A3Quat a3_quat_axis_angle(A3Vec3 axis, f32 angle);
A3Quat a3_quat_euler(f32 pitch, f32 yaw, f32 roll); /* radians; applied yaw(Y) * pitch(X) * roll(Z) */
A3Vec3 a3_quat_to_euler(A3Quat q);                   /* returns (pitch, yaw, roll) */
A3Quat a3_quat_mul(A3Quat a, A3Quat b);
A3Quat a3_quat_normalize(A3Quat q);
A3Quat a3_quat_conjugate(A3Quat q);
A3Quat a3_quat_inverse(A3Quat q);
A3Vec3 a3_quat_rotate(A3Quat q, A3Vec3 v);
A3Quat a3_quat_slerp(A3Quat a, A3Quat b, f32 t);
A3Quat a3_quat_nlerp(A3Quat a, A3Quat b, f32 t);
A3Quat a3_quat_look_rotation(A3Vec3 forward, A3Vec3 up);
A3Quat a3_quat_from_mat4(const A3Mat4 *m);
f32    a3_quat_dot(A3Quat a, A3Quat b);

/* ---- Mat4 ---- */
A3Mat4 a3_mat4_identity(void);
A3Mat4 a3_mat4_translation(A3Vec3 t);
A3Mat4 a3_mat4_scale(A3Vec3 s);
A3Mat4 a3_mat4_from_quat(A3Quat q);
A3Mat4 a3_mat4_trs(A3Vec3 t, A3Quat r, A3Vec3 s);
A3Mat4 a3_mat4_from_transform(const A3Transform *t);
A3Mat4 a3_mat4_mul(const A3Mat4 *a, const A3Mat4 *b); /* a * b; SIMD/asm backed */
A3Vec4 a3_mat4_mul_v4(const A3Mat4 *m, A3Vec4 v);
A3Vec3 a3_mat4_mul_point(const A3Mat4 *m, A3Vec3 p);   /* w = 1, no divide */
A3Vec3 a3_mat4_mul_dir(const A3Mat4 *m, A3Vec3 d);     /* w = 0 */
A3Vec3 a3_mat4_mul_point_project(const A3Mat4 *m, A3Vec3 p); /* with perspective divide */
A3Mat4 a3_mat4_transpose(const A3Mat4 *m);
b32    a3_mat4_inverse(const A3Mat4 *m, A3Mat4 *out);  /* false if singular */
A3Mat4 a3_mat4_inverse_affine(const A3Mat4 *m);
/* depth_zero_to_one: 1 for WebGPU/D3D/Vulkan clip space, 0 for OpenGL. */
A3Mat4 a3_mat4_perspective(f32 fovy, f32 aspect, f32 znear, f32 zfar, b32 depth_zero_to_one);
A3Mat4 a3_mat4_ortho(f32 left, f32 right, f32 bottom, f32 top, f32 znear, f32 zfar, b32 depth_zero_to_one);
A3Mat4 a3_mat4_look_at(A3Vec3 eye, A3Vec3 target, A3Vec3 up);
A3Vec3 a3_mat4_get_translation(const A3Mat4 *m);
b32    a3_mat4_nearly_equal(const A3Mat4 *a, const A3Mat4 *b, f32 eps);
void   a3_mat4_decompose(const A3Mat4 *m, A3Vec3 *t, A3Quat *r, A3Vec3 *s);

/* ---- Transform ---- */
A3_INLINE A3Transform a3_transform_identity(void) {
    A3Transform t = { { 0, 0, 0 }, { 0, 0, 0, 1 }, { 1, 1, 1 } };
    return t;
}
A3Transform a3_transform_combine(const A3Transform *parent, const A3Transform *child);
A3Vec3 a3_transform_point(const A3Transform *t, A3Vec3 p);
A3_INLINE A3Vec3 a3_transform_forward(const A3Transform *t) { return a3_quat_rotate(t->rotation, a3_v3(0, 0, -1)); }
A3_INLINE A3Vec3 a3_transform_right(const A3Transform *t) { return a3_quat_rotate(t->rotation, a3_v3(1, 0, 0)); }
A3_INLINE A3Vec3 a3_transform_up(const A3Transform *t) { return a3_quat_rotate(t->rotation, a3_v3(0, 1, 0)); }

/* ---- Geometry ---- */
A3_INLINE A3Aabb a3_aabb(A3Vec3 mn, A3Vec3 mx) { A3Aabb b = { mn, mx }; return b; }
A3_INLINE A3Aabb a3_aabb_empty(void) { return a3_aabb(a3_v3s(A3_F32_MAX), a3_v3s(-A3_F32_MAX)); }
A3_INLINE A3Vec3 a3_aabb_center(A3Aabb b) { return a3_v3_scale(a3_v3_add(b.min, b.max), 0.5f); }
A3_INLINE A3Vec3 a3_aabb_extents(A3Aabb b) { return a3_v3_scale(a3_v3_sub(b.max, b.min), 0.5f); }
A3_INLINE A3Aabb a3_aabb_expand(A3Aabb b, A3Vec3 p) { return a3_aabb(a3_v3_min(b.min, p), a3_v3_max(b.max, p)); }
A3_INLINE A3Aabb a3_aabb_union(A3Aabb a, A3Aabb b) { return a3_aabb(a3_v3_min(a.min, b.min), a3_v3_max(a.max, b.max)); }
A3_INLINE b32 a3_aabb_overlap(A3Aabb a, A3Aabb b) {
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y && a.min.z <= b.max.z && a.max.z >= b.min.z;
}
A3_INLINE b32 a3_aabb_contains(A3Aabb a, A3Vec3 p) {
    return p.x >= a.min.x && p.x <= a.max.x && p.y >= a.min.y && p.y <= a.max.y && p.z >= a.min.z && p.z <= a.max.z;
}
A3_INLINE b32 a3_aabb_valid(A3Aabb a) { return a.min.x <= a.max.x && a.min.y <= a.max.y && a.min.z <= a.max.z; }
A3Aabb a3_aabb_transform(A3Aabb b, const A3Mat4 *m);
A3Vec3 a3_aabb_closest_point(A3Aabb b, A3Vec3 p);

A3Frustum a3_frustum_from_matrix(const A3Mat4 *view_proj, b32 depth_zero_to_one);
b32 a3_frustum_test_sphere(const A3Frustum *f, A3Vec3 center, f32 radius);
b32 a3_frustum_test_aabb(const A3Frustum *f, A3Aabb b);

/* Ray tests return true on hit and the distance along the (normalized) ray. */
b32 a3_ray_aabb(A3Ray ray, A3Aabb b, f32 *t_out);
b32 a3_ray_sphere(A3Ray ray, A3Vec3 center, f32 radius, f32 *t_out);
b32 a3_ray_plane(A3Ray ray, A3Plane plane, f32 *t_out);
b32 a3_ray_triangle(A3Ray ray, A3Vec3 a, A3Vec3 b, A3Vec3 c, f32 *t_out);

/* ---- Batch (SIMD / assembly accelerated) routines ----
 * Implementations: C reference (a3_simd_ref.c), x86-64 SSE assembly
 * (a3_simd_x64.S) and wasm SIMD128 (a3_simd_wasm.c). The dispatching
 * versions below pick the fastest implementation compiled in. */
void a3_mat4_mul_batch(A3Mat4 *out, const A3Mat4 *a, const A3Mat4 *b, u32 count); /* out[i] = a[i]*b[i] */
void a3_mat4_mul_one_many(A3Mat4 *out, const A3Mat4 *parent, const A3Mat4 *locals, u32 count); /* out[i] = parent*locals[i] */
void a3_transform_points(A3Vec4 *out, const A3Mat4 *m, const A3Vec4 *in, u32 count);
/* Writes 1/0 into visible[i] for spheres (x,y,z,radius) against the frustum. Returns visible count. */
u32  a3_cull_spheres(const A3Frustum *f, const A3Vec4 *spheres, u8 *visible, u32 count);
/* pos[i] += vel[i] * dt  (xyzw), used by particles and simple integration. */
void a3_integrate_v4(A3Vec4 *pos, const A3Vec4 *vel, f32 dt, u32 count);
/* Name of the implementation compiled in, e.g. "x86-64 SSE asm". */
const char *a3_simd_backend_name(void);

/* Reference implementations (always available, used by tests). */
void a3_ref_mat4_mul(A3Mat4 *out, const A3Mat4 *a, const A3Mat4 *b);
void a3_ref_transform_points(A3Vec4 *out, const A3Mat4 *m, const A3Vec4 *in, u32 count);
u32  a3_ref_cull_spheres(const A3Frustum *f, const A3Vec4 *spheres, u8 *visible, u32 count);
void a3_ref_integrate_v4(A3Vec4 *pos, const A3Vec4 *vel, f32 dt, u32 count);

A3_EXTERN_C_END

#endif
