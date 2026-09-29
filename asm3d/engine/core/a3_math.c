/*
 * ASM3D - a3_math.c
 * Deterministic scalar functions and non-inline math routines.
 * All transcendental functions evaluate in IEEE double using only
 * + - * / sqrt, which are correctly rounded on every supported target.
 */
#include "a3_math.h"
#include "a3_string.h"

/* ======================================================================== */
/* Scalars                                                                  */
/* ======================================================================== */

static const f64 PIO2_HI = 1.57079632673412561417e+00; /* first 33 bits of pi/2 */
static const f64 PIO2_LO = 6.07710050650619224932e-11; /* pi/2 - PIO2_HI */
static const f64 PI_D = 3.14159265358979323846;

static f64 d_floor(f64 x) { return __builtin_floor(x); }
static f64 d_sqrt(f64 x) { return __builtin_sqrt(x); }

/* sin on [-pi/4, pi/4] */
static f64 k_sin(f64 r) {
    f64 r2 = r * r;
    return r * (1.0 + r2 * (-1.0 / 6.0 + r2 * (1.0 / 120.0 + r2 * (-1.0 / 5040.0 + r2 * (1.0 / 362880.0 + r2 * (-1.0 / 39916800.0))))));
}
/* cos on [-pi/4, pi/4] */
static f64 k_cos(f64 r) {
    f64 r2 = r * r;
    return 1.0 + r2 * (-0.5 + r2 * (1.0 / 24.0 + r2 * (-1.0 / 720.0 + r2 * (1.0 / 40320.0 + r2 * (-1.0 / 3628800.0 + r2 * (1.0 / 479001600.0))))));
}

static void d_sincos(f64 x, f64 *s, f64 *c) {
    f64 k = d_floor(x * (2.0 / PI_D) + 0.5);
    f64 r = (x - k * PIO2_HI) - k * PIO2_LO;
    i64 q = (i64)k & 3;
    f64 sr = k_sin(r), cr = k_cos(r);
    switch (q) {
    case 0: *s = sr; *c = cr; break;
    case 1: *s = cr; *c = -sr; break;
    case 2: *s = -sr; *c = -cr; break;
    default: *s = -cr; *c = sr; break;
    }
}

f32 a3_sinf(f32 x) { f64 s, c; d_sincos(x, &s, &c); return (f32)s; }
f32 a3_cosf(f32 x) { f64 s, c; d_sincos(x, &s, &c); return (f32)c; }
void a3_sincosf(f32 x, f32 *s, f32 *c) { f64 sd, cd; d_sincos(x, &sd, &cd); *s = (f32)sd; *c = (f32)cd; }
f32 a3_tanf(f32 x) { f64 s, c; d_sincos(x, &s, &c); return (f32)(s / c); }

static f64 d_atan(f64 x) {
    b32 neg = x < 0;
    if (neg) x = -x;
    b32 inv = x > 1.0;
    if (inv) x = 1.0 / x;
    /* two half-angle reductions: atan(x) = 2 atan(x / (1 + sqrt(1 + x^2))) */
    x = x / (1.0 + d_sqrt(1.0 + x * x));
    x = x / (1.0 + d_sqrt(1.0 + x * x));
    f64 x2 = x * x, term = x, sum = x;
    for (int n = 3; n <= 21; n += 2) { term *= -x2; sum += term / n; }
    sum *= 4.0;
    if (inv) sum = PI_D * 0.5 - sum;
    return neg ? -sum : sum;
}

static f64 d_atan2(f64 y, f64 x) {
    if (x > 0) return d_atan(y / x);
    if (x < 0) return y >= 0 ? d_atan(y / x) + PI_D : d_atan(y / x) - PI_D;
    if (y > 0) return PI_D * 0.5;
    if (y < 0) return -PI_D * 0.5;
    return 0.0;
}

f32 a3_atanf(f32 x) { return (f32)d_atan(x); }
f32 a3_atan2f(f32 y, f32 x) { return (f32)d_atan2(y, x); }

f32 a3_asinf(f32 x) {
    f64 d = x;
    if (d > 1.0) d = 1.0;
    if (d < -1.0) d = -1.0;
    return (f32)d_atan2(d, d_sqrt(1.0 - d * d));
}

f32 a3_acosf(f32 x) {
    f64 d = x;
    if (d > 1.0) d = 1.0;
    if (d < -1.0) d = -1.0;
    return (f32)d_atan2(d_sqrt(1.0 - d * d), d);
}

static f64 d_pow2i(i32 n) {
    /* builds 2^n directly in the exponent bits */
    if (n < -1022) return 0.0;
    if (n > 1023) return __builtin_inf();
    u64 bits = (u64)(n + 1023) << 52;
    f64 r;
    a3_memcpy(&r, &bits, 8);
    return r;
}

static f64 d_exp(f64 x) {
    if (x > 709.0) return __builtin_inf();
    if (x < -745.0) return 0.0;
    const f64 LOG2E = 1.44269504088896340736;
    const f64 LN2_HI = 6.93147180369123816490e-01, LN2_LO = 1.90821492927058770002e-10;
    f64 k = d_floor(x * LOG2E + 0.5);
    f64 r = (x - k * LN2_HI) - k * LN2_LO;
    f64 term = 1.0, sum = 1.0;
    for (int n = 1; n <= 13; ++n) { term *= r / n; sum += term; }
    return sum * d_pow2i((i32)k);
}

static f64 d_log(f64 x) {
    if (x < 0 || x != x) return __builtin_nan("");
    if (x == 0) return -__builtin_inf();
    if (x > 1.7976931348623157e308) return x;
    u64 bits;
    a3_memcpy(&bits, &x, 8);
    i32 e = (i32)((bits >> 52) & 0x7FF);
    if (e == 0) { /* subnormal */
        x *= 18014398509481984.0; /* 2^54 */
        a3_memcpy(&bits, &x, 8);
        e = (i32)((bits >> 52) & 0x7FF) - 54;
    }
    e -= 1023;
    bits = (bits & 0x000FFFFFFFFFFFFFull) | 0x3FF0000000000000ull;
    f64 m;
    a3_memcpy(&m, &bits, 8);
    if (m > 1.41421356237309504880) { m *= 0.5; ++e; }
    f64 s = (m - 1.0) / (m + 1.0), s2 = s * s, term = s, sum = s;
    for (int n = 3; n <= 19; n += 2) { term *= s2; sum += term / n; }
    return 2.0 * sum + e * 0.69314718055994530942;
}

f32 a3_expf(f32 x) { return (f32)d_exp(x); }
f32 a3_logf(f32 x) { return (f32)d_log(x); }
f32 a3_log2f(f32 x) { return (f32)(d_log(x) * 1.44269504088896340736); }

f32 a3_powf(f32 x, f32 y) {
    if (y == 0.0f) return 1.0f;
    if (x == 0.0f) return y > 0 ? 0.0f : A3_INF;
    if (x < 0.0f) {
        f32 yi = a3_truncf(y);
        if (yi != y) return __builtin_nanf("");
        f64 r = d_exp(y * d_log(-(f64)x));
        i64 iy = (i64)yi;
        return (f32)((iy & 1) ? -r : r);
    }
    return (f32)d_exp((f64)y * d_log(x));
}

f32 a3_fmodf(f32 x, f32 y) {
    if (y == 0.0f) return __builtin_nanf("");
    f64 q = (f64)x / (f64)y;
    q = q < 0 ? -d_floor(-q) : d_floor(q);
    return (f32)((f64)x - q * (f64)y);
}

f32 a3_wrap_angle(f32 a) {
    f64 d = a;
    d = d - (f64)A3_TAU * d_floor((d + PI_D) / (2.0 * PI_D));
    return (f32)d;
}

/* ======================================================================== */
/* Quaternion                                                               */
/* ======================================================================== */

A3Quat a3_quat_axis_angle(A3Vec3 axis, f32 angle) {
    A3Vec3 n = a3_v3_norm(axis);
    f32 s, c;
    a3_sincosf(angle * 0.5f, &s, &c);
    return a3_quat(n.x * s, n.y * s, n.z * s, c);
}

A3Quat a3_quat_mul(A3Quat a, A3Quat b) {
    return a3_quat(
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);
}

A3Quat a3_quat_euler(f32 pitch, f32 yaw, f32 roll) {
    A3Quat qy = a3_quat_axis_angle(a3_v3(0, 1, 0), yaw);
    A3Quat qx = a3_quat_axis_angle(a3_v3(1, 0, 0), pitch);
    A3Quat qz = a3_quat_axis_angle(a3_v3(0, 0, 1), roll);
    return a3_quat_normalize(a3_quat_mul(a3_quat_mul(qy, qx), qz));
}

A3Vec3 a3_quat_to_euler(A3Quat q) {
    A3Mat4 m = a3_mat4_from_quat(q);
#define M(r, c) m.m[(c) * 4 + (r)]
    f32 m12 = a3_clampf(M(1, 2), -1.0f, 1.0f);
    f32 pitch = a3_asinf(-m12), yaw, roll;
    if (a3_absf(m12) < 0.99999f) {
        yaw = a3_atan2f(M(0, 2), M(2, 2));
        roll = a3_atan2f(M(1, 0), M(1, 1));
    } else {
        yaw = a3_atan2f(-M(2, 0), M(0, 0));
        roll = 0.0f;
    }
#undef M
    return a3_v3(pitch, yaw, roll);
}

f32 a3_quat_dot(A3Quat a, A3Quat b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

A3Quat a3_quat_normalize(A3Quat q) {
    f32 l = a3_sqrtf(a3_quat_dot(q, q));
    if (l < A3_EPSILON) return a3_quat_identity();
    f32 inv = 1.0f / l;
    return a3_quat(q.x * inv, q.y * inv, q.z * inv, q.w * inv);
}

A3Quat a3_quat_conjugate(A3Quat q) { return a3_quat(-q.x, -q.y, -q.z, q.w); }

A3Quat a3_quat_inverse(A3Quat q) {
    f32 d = a3_quat_dot(q, q);
    if (d < A3_EPSILON) return a3_quat_identity();
    f32 inv = 1.0f / d;
    return a3_quat(-q.x * inv, -q.y * inv, -q.z * inv, q.w * inv);
}

A3Vec3 a3_quat_rotate(A3Quat q, A3Vec3 v) {
    A3Vec3 u = a3_v3(q.x, q.y, q.z);
    A3Vec3 t = a3_v3_scale(a3_v3_cross(u, v), 2.0f);
    return a3_v3_add(a3_v3_add(v, a3_v3_scale(t, q.w)), a3_v3_cross(u, t));
}

A3Quat a3_quat_nlerp(A3Quat a, A3Quat b, f32 t) {
    if (a3_quat_dot(a, b) < 0) b = a3_quat(-b.x, -b.y, -b.z, -b.w);
    return a3_quat_normalize(a3_quat(a3_lerpf(a.x, b.x, t), a3_lerpf(a.y, b.y, t), a3_lerpf(a.z, b.z, t), a3_lerpf(a.w, b.w, t)));
}

A3Quat a3_quat_slerp(A3Quat a, A3Quat b, f32 t) {
    f32 d = a3_quat_dot(a, b);
    if (d < 0) { b = a3_quat(-b.x, -b.y, -b.z, -b.w); d = -d; }
    if (d > 0.9995f) return a3_quat_nlerp(a, b, t);
    f32 theta = a3_acosf(d);
    f32 st = a3_sinf(theta);
    f32 wa = a3_sinf((1 - t) * theta) / st, wb = a3_sinf(t * theta) / st;
    return a3_quat(a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb);
}

A3Quat a3_quat_from_mat4(const A3Mat4 *mat) {
    const f32 *m = mat->m;
#define M(r, c) m[(c) * 4 + (r)]
    f32 tr = M(0, 0) + M(1, 1) + M(2, 2);
    A3Quat q;
    if (tr > 0) {
        f32 s = a3_sqrtf(tr + 1.0f) * 2.0f;
        q = a3_quat((M(2, 1) - M(1, 2)) / s, (M(0, 2) - M(2, 0)) / s, (M(1, 0) - M(0, 1)) / s, 0.25f * s);
    } else if (M(0, 0) > M(1, 1) && M(0, 0) > M(2, 2)) {
        f32 s = a3_sqrtf(1.0f + M(0, 0) - M(1, 1) - M(2, 2)) * 2.0f;
        q = a3_quat(0.25f * s, (M(0, 1) + M(1, 0)) / s, (M(0, 2) + M(2, 0)) / s, (M(2, 1) - M(1, 2)) / s);
    } else if (M(1, 1) > M(2, 2)) {
        f32 s = a3_sqrtf(1.0f + M(1, 1) - M(0, 0) - M(2, 2)) * 2.0f;
        q = a3_quat((M(0, 1) + M(1, 0)) / s, 0.25f * s, (M(1, 2) + M(2, 1)) / s, (M(0, 2) - M(2, 0)) / s);
    } else {
        f32 s = a3_sqrtf(1.0f + M(2, 2) - M(0, 0) - M(1, 1)) * 2.0f;
        q = a3_quat((M(0, 2) + M(2, 0)) / s, (M(1, 2) + M(2, 1)) / s, 0.25f * s, (M(1, 0) - M(0, 1)) / s);
    }
#undef M
    return a3_quat_normalize(q);
}

A3Quat a3_quat_look_rotation(A3Vec3 forward, A3Vec3 up) {
    A3Vec3 f = a3_v3_norm(forward);
    if (a3_v3_len_sq(f) < A3_EPSILON) return a3_quat_identity();
    A3Vec3 r = a3_v3_cross(f, up);
    if (a3_v3_len_sq(r) < A3_EPSILON) r = a3_v3_cross(f, a3_v3(0, 0, 1));
    r = a3_v3_norm(r);
    A3Vec3 u = a3_v3_cross(r, f);
    A3Mat4 m = a3_mat4_identity();
    m.m[0] = r.x; m.m[1] = r.y; m.m[2] = r.z;
    m.m[4] = u.x; m.m[5] = u.y; m.m[6] = u.z;
    m.m[8] = -f.x; m.m[9] = -f.y; m.m[10] = -f.z;
    return a3_quat_from_mat4(&m);
}

/* ======================================================================== */
/* Mat4                                                                     */
/* ======================================================================== */

A3Mat4 a3_mat4_identity(void) {
    A3Mat4 r = { { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } };
    return r;
}

A3Mat4 a3_mat4_translation(A3Vec3 t) {
    A3Mat4 r = a3_mat4_identity();
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
    return r;
}

A3Mat4 a3_mat4_scale(A3Vec3 s) {
    A3Mat4 r = a3_mat4_identity();
    r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z;
    return r;
}

A3Mat4 a3_mat4_from_quat(A3Quat q) {
    f32 xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    f32 xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    f32 wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    A3Mat4 r = a3_mat4_identity();
    r.m[0] = 1 - 2 * (yy + zz); r.m[4] = 2 * (xy - wz);     r.m[8] = 2 * (xz + wy);
    r.m[1] = 2 * (xy + wz);     r.m[5] = 1 - 2 * (xx + zz); r.m[9] = 2 * (yz - wx);
    r.m[2] = 2 * (xz - wy);     r.m[6] = 2 * (yz + wx);     r.m[10] = 1 - 2 * (xx + yy);
    return r;
}

A3Mat4 a3_mat4_trs(A3Vec3 t, A3Quat q, A3Vec3 s) {
    A3Mat4 r = a3_mat4_from_quat(q);
    r.m[0] *= s.x; r.m[1] *= s.x; r.m[2] *= s.x;
    r.m[4] *= s.y; r.m[5] *= s.y; r.m[6] *= s.y;
    r.m[8] *= s.z; r.m[9] *= s.z; r.m[10] *= s.z;
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
    return r;
}

A3Mat4 a3_mat4_from_transform(const A3Transform *t) { return a3_mat4_trs(t->position, t->rotation, t->scale); }

A3Mat4 a3_mat4_mul(const A3Mat4 *a, const A3Mat4 *b) {
    A3Mat4 r;
    a3_mat4_mul_batch(&r, a, b, 1);
    return r;
}

A3Vec4 a3_mat4_mul_v4(const A3Mat4 *mat, A3Vec4 v) {
    const f32 *m = mat->m;
    return a3_v4(m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12] * v.w,
                 m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13] * v.w,
                 m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14] * v.w,
                 m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15] * v.w);
}

A3Vec3 a3_mat4_mul_point(const A3Mat4 *mat, A3Vec3 p) {
    const f32 *m = mat->m;
    return a3_v3(m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
                 m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
                 m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]);
}

A3Vec3 a3_mat4_mul_dir(const A3Mat4 *mat, A3Vec3 d) {
    const f32 *m = mat->m;
    return a3_v3(m[0] * d.x + m[4] * d.y + m[8] * d.z,
                 m[1] * d.x + m[5] * d.y + m[9] * d.z,
                 m[2] * d.x + m[6] * d.y + m[10] * d.z);
}

A3Vec3 a3_mat4_mul_point_project(const A3Mat4 *m, A3Vec3 p) {
    A3Vec4 r = a3_mat4_mul_v4(m, a3_v4_from3(p, 1.0f));
    f32 w = a3_absf(r.w) > A3_EPSILON ? r.w : A3_EPSILON;
    return a3_v3(r.x / w, r.y / w, r.z / w);
}

A3Mat4 a3_mat4_transpose(const A3Mat4 *m) {
    A3Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr) r.m[c * 4 + rr] = m->m[rr * 4 + c];
    return r;
}

b32 a3_mat4_inverse(const A3Mat4 *mat, A3Mat4 *out) {
    const f32 *m = mat->m;
    f32 inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    f32 det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (a3_absf(det) < 1e-12f) return 0;
    f32 id = 1.0f / det;
    for (int i = 0; i < 16; ++i) out->m[i] = inv[i] * id;
    return 1;
}

A3Mat4 a3_mat4_inverse_affine(const A3Mat4 *m) {
    A3Mat4 r;
    if (!a3_mat4_inverse(m, &r)) return a3_mat4_identity();
    return r;
}

A3Mat4 a3_mat4_perspective(f32 fovy, f32 aspect, f32 zn, f32 zf, b32 zo) {
    A3Mat4 r;
    a3_zero(&r, sizeof(r));
    if (aspect <= 0.0f) aspect = 1.0f;
    f32 f = 1.0f / a3_tanf(fovy * 0.5f);
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[11] = -1.0f;
    if (zo) { r.m[10] = zf / (zn - zf); r.m[14] = zf * zn / (zn - zf); }
    else { r.m[10] = (zf + zn) / (zn - zf); r.m[14] = 2.0f * zf * zn / (zn - zf); }
    return r;
}

A3Mat4 a3_mat4_ortho(f32 l, f32 rt, f32 b, f32 t, f32 zn, f32 zf, b32 zo) {
    A3Mat4 r = a3_mat4_identity();
    r.m[0] = 2.0f / (rt - l);
    r.m[5] = 2.0f / (t - b);
    r.m[12] = -(rt + l) / (rt - l);
    r.m[13] = -(t + b) / (t - b);
    if (zo) { r.m[10] = -1.0f / (zf - zn); r.m[14] = -zn / (zf - zn); }
    else { r.m[10] = -2.0f / (zf - zn); r.m[14] = -(zf + zn) / (zf - zn); }
    return r;
}

A3Mat4 a3_mat4_look_at(A3Vec3 eye, A3Vec3 target, A3Vec3 up) {
    A3Vec3 f = a3_v3_norm(a3_v3_sub(target, eye));
    A3Vec3 s = a3_v3_cross(f, up);
    if (a3_v3_len_sq(s) < A3_EPSILON) s = a3_v3_cross(f, a3_v3(0, 0, 1));
    s = a3_v3_norm(s);
    A3Vec3 u = a3_v3_cross(s, f);
    A3Mat4 r = a3_mat4_identity();
    r.m[0] = s.x; r.m[4] = s.y; r.m[8] = s.z;
    r.m[1] = u.x; r.m[5] = u.y; r.m[9] = u.z;
    r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
    r.m[12] = -a3_v3_dot(s, eye);
    r.m[13] = -a3_v3_dot(u, eye);
    r.m[14] = a3_v3_dot(f, eye);
    return r;
}

A3Vec3 a3_mat4_get_translation(const A3Mat4 *m) { return a3_v3(m->m[12], m->m[13], m->m[14]); }

b32 a3_mat4_nearly_equal(const A3Mat4 *a, const A3Mat4 *b, f32 eps) {
    for (int i = 0; i < 16; ++i) if (!a3_nearly_equal(a->m[i], b->m[i], eps)) return 0;
    return 1;
}

void a3_mat4_decompose(const A3Mat4 *m, A3Vec3 *t, A3Quat *r, A3Vec3 *s) {
    A3Vec3 c0 = a3_v3(m->m[0], m->m[1], m->m[2]);
    A3Vec3 c1 = a3_v3(m->m[4], m->m[5], m->m[6]);
    A3Vec3 c2 = a3_v3(m->m[8], m->m[9], m->m[10]);
    A3Vec3 sc = a3_v3(a3_v3_len(c0), a3_v3_len(c1), a3_v3_len(c2));
    if (a3_v3_dot(a3_v3_cross(c0, c1), c2) < 0) sc.x = -sc.x;
    if (t) *t = a3_mat4_get_translation(m);
    if (s) *s = sc;
    if (r) {
        A3Mat4 rm = a3_mat4_identity();
        if (sc.x != 0) { rm.m[0] = c0.x / sc.x; rm.m[1] = c0.y / sc.x; rm.m[2] = c0.z / sc.x; }
        if (sc.y != 0) { rm.m[4] = c1.x / sc.y; rm.m[5] = c1.y / sc.y; rm.m[6] = c1.z / sc.y; }
        if (sc.z != 0) { rm.m[8] = c2.x / sc.z; rm.m[9] = c2.y / sc.z; rm.m[10] = c2.z / sc.z; }
        *r = a3_quat_from_mat4(&rm);
    }
}

/* ======================================================================== */
/* Transform                                                                */
/* ======================================================================== */

A3Transform a3_transform_combine(const A3Transform *p, const A3Transform *c) {
    A3Transform r;
    r.scale = a3_v3_mul(p->scale, c->scale);
    r.rotation = a3_quat_normalize(a3_quat_mul(p->rotation, c->rotation));
    r.position = a3_v3_add(p->position, a3_quat_rotate(p->rotation, a3_v3_mul(p->scale, c->position)));
    return r;
}

A3Vec3 a3_transform_point(const A3Transform *t, A3Vec3 p) {
    return a3_v3_add(t->position, a3_quat_rotate(t->rotation, a3_v3_mul(t->scale, p)));
}

/* ======================================================================== */
/* Geometry                                                                 */
/* ======================================================================== */

A3Aabb a3_aabb_transform(A3Aabb b, const A3Mat4 *m) {
    /* Arvo's method: transform center + absolute-rotated extents. */
    A3Vec3 c = a3_aabb_center(b), e = a3_aabb_extents(b);
    A3Vec3 nc = a3_mat4_mul_point(m, c);
    A3Vec3 ne = a3_v3(
        a3_absf(m->m[0]) * e.x + a3_absf(m->m[4]) * e.y + a3_absf(m->m[8]) * e.z,
        a3_absf(m->m[1]) * e.x + a3_absf(m->m[5]) * e.y + a3_absf(m->m[9]) * e.z,
        a3_absf(m->m[2]) * e.x + a3_absf(m->m[6]) * e.y + a3_absf(m->m[10]) * e.z);
    return a3_aabb(a3_v3_sub(nc, ne), a3_v3_add(nc, ne));
}

A3Vec3 a3_aabb_closest_point(A3Aabb b, A3Vec3 p) {
    return a3_v3(a3_clampf(p.x, b.min.x, b.max.x), a3_clampf(p.y, b.min.y, b.max.y), a3_clampf(p.z, b.min.z, b.max.z));
}

A3Frustum a3_frustum_from_matrix(const A3Mat4 *vp, b32 zo) {
    const f32 *m = vp->m;
    A3Vec4 r0 = a3_v4(m[0], m[4], m[8], m[12]);
    A3Vec4 r1 = a3_v4(m[1], m[5], m[9], m[13]);
    A3Vec4 r2 = a3_v4(m[2], m[6], m[10], m[14]);
    A3Vec4 r3 = a3_v4(m[3], m[7], m[11], m[15]);
    A3Frustum f;
    f.planes[0] = a3_v4_add(r3, r0);
    f.planes[1] = a3_v4_add(r3, a3_v4_scale(r0, -1));
    f.planes[2] = a3_v4_add(r3, r1);
    f.planes[3] = a3_v4_add(r3, a3_v4_scale(r1, -1));
    f.planes[4] = zo ? r2 : a3_v4_add(r3, r2);
    f.planes[5] = a3_v4_add(r3, a3_v4_scale(r2, -1));
    for (int i = 0; i < 6; ++i) {
        A3Vec4 p = f.planes[i];
        f32 l = a3_sqrtf(p.x * p.x + p.y * p.y + p.z * p.z);
        if (l > A3_EPSILON) f.planes[i] = a3_v4_scale(p, 1.0f / l);
    }
    return f;
}

b32 a3_frustum_test_sphere(const A3Frustum *f, A3Vec3 c, f32 r) {
    for (int i = 0; i < 6; ++i) {
        const A3Vec4 p = f->planes[i];
        if (p.x * c.x + p.y * c.y + p.z * c.z + p.w < -r) return 0;
    }
    return 1;
}

b32 a3_frustum_test_aabb(const A3Frustum *f, A3Aabb b) {
    for (int i = 0; i < 6; ++i) {
        const A3Vec4 p = f->planes[i];
        A3Vec3 v = a3_v3(p.x >= 0 ? b.max.x : b.min.x, p.y >= 0 ? b.max.y : b.min.y, p.z >= 0 ? b.max.z : b.min.z);
        if (p.x * v.x + p.y * v.y + p.z * v.z + p.w < 0) return 0;
    }
    return 1;
}

b32 a3_ray_aabb(A3Ray ray, A3Aabb b, f32 *t_out) {
    f32 tmin = 0.0f, tmax = A3_F32_MAX;
    const f32 *o = &ray.origin.x, *d = &ray.dir.x, *mn = &b.min.x, *mx = &b.max.x;
    for (int i = 0; i < 3; ++i) {
        if (a3_absf(d[i]) < 1e-9f) {
            if (o[i] < mn[i] || o[i] > mx[i]) return 0;
        } else {
            f32 inv = 1.0f / d[i];
            f32 t1 = (mn[i] - o[i]) * inv, t2 = (mx[i] - o[i]) * inv;
            if (t1 > t2) { f32 tmp = t1; t1 = t2; t2 = tmp; }
            tmin = a3_maxf(tmin, t1);
            tmax = a3_minf(tmax, t2);
            if (tmin > tmax) return 0;
        }
    }
    if (t_out) *t_out = tmin;
    return 1;
}

b32 a3_ray_sphere(A3Ray ray, A3Vec3 c, f32 r, f32 *t_out) {
    A3Vec3 m = a3_v3_sub(ray.origin, c);
    f32 b = a3_v3_dot(m, ray.dir);
    f32 cc = a3_v3_dot(m, m) - r * r;
    if (cc > 0 && b > 0) return 0;
    f32 disc = b * b - cc;
    if (disc < 0) return 0;
    f32 t = -b - a3_sqrtf(disc);
    if (t < 0) t = 0;
    if (t_out) *t_out = t;
    return 1;
}

b32 a3_ray_plane(A3Ray ray, A3Plane pl, f32 *t_out) {
    f32 denom = a3_v3_dot(pl.n, ray.dir);
    if (a3_absf(denom) < 1e-9f) return 0;
    f32 t = -(a3_v3_dot(pl.n, ray.origin) + pl.d) / denom;
    if (t < 0) return 0;
    if (t_out) *t_out = t;
    return 1;
}

b32 a3_ray_triangle(A3Ray ray, A3Vec3 a, A3Vec3 b, A3Vec3 c, f32 *t_out) {
    A3Vec3 e1 = a3_v3_sub(b, a), e2 = a3_v3_sub(c, a);
    A3Vec3 p = a3_v3_cross(ray.dir, e2);
    f32 det = a3_v3_dot(e1, p);
    if (a3_absf(det) < 1e-9f) return 0;
    f32 inv = 1.0f / det;
    A3Vec3 s = a3_v3_sub(ray.origin, a);
    f32 u = a3_v3_dot(s, p) * inv;
    if (u < 0 || u > 1) return 0;
    A3Vec3 q = a3_v3_cross(s, e1);
    f32 v = a3_v3_dot(ray.dir, q) * inv;
    if (v < 0 || u + v > 1) return 0;
    f32 t = a3_v3_dot(e2, q) * inv;
    if (t < 0) return 0;
    if (t_out) *t_out = t;
    return 1;
}
