/*
 * ASM3D - a3_vehicle_ref.c
 * C reference of the wheel kernel. Every expression mirrors the instruction
 * order of a3_vehicle_x64.S (built with -ffp-contract=off), so the results
 * are bit-identical.
 */
#include "a3_vehicle_kernels.h"

#define VK_INV_2PI 0.159154943f
#define VK_2PI 6.28318548f
#define VK_PI 3.14159274f
#define VK_HALF_PI 1.57079637f

static u32 vk_bits(f32 f) { union { f32 f; u32 u; } c; c.f = f; return c.u; }
static f32 vk_float(u32 u) { union { f32 f; u32 u; } c; c.u = u; return c.f; }
static f32 vk_max(f32 a, f32 b) { return a > b ? a : b; }   /* maxps */
static f32 vk_min(f32 a, f32 b) { return a < b ? a : b; }   /* minps */
static f32 vk_abs(f32 a) { return vk_float(vk_bits(a) & 0x7fffffffu); }
static f32 vk_neg(f32 a) { return vk_float(vk_bits(a) ^ 0x80000000u); }

static f32 vk_atan(f32 x) {
    u32 sign = vk_bits(x) & 0x80000000u;
    f32 a = vk_abs(x);
    f32 r = 1.0f / a;
    b32 big = 1.0f < a;
    f32 t = big ? r : a;
    f32 t2 = t * t;
    f32 p = t2 * -0.01172120f;
    p = p + 0.05265332f;
    p = p * t2;
    p = p + -0.11643287f;
    p = p * t2;
    p = p + 0.19354346f;
    p = p * t2;
    p = p + -0.33262347f;
    p = p * t2;
    p = p + 0.99997726f;
    p = p * t;
    f32 h = VK_HALF_PI - p;
    f32 res = big ? h : p;
    return vk_float(vk_bits(res) ^ sign);
}

static f32 vk_sin(f32 x) {
    f32 q = x * VK_INV_2PI;
    q = q + (q < 0.0f ? -0.5f : 0.5f);
    f32 k = (f32)(i32)q;
    f32 m = k * VK_2PI;
    f32 r = x - m;
    if (r > VK_HALF_PI) r = VK_PI - r;
    if (r < -VK_HALF_PI) r = -VK_PI - r;
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

void a3_vk_ref_atan4(const f32 *in4, f32 *out4) { for (int i = 0; i < 4; ++i) out4[i] = vk_atan(in4[i]); }
void a3_vk_ref_sin4(const f32 *in4, f32 *out4) { for (int i = 0; i < 4; ++i) out4[i] = vk_sin(in4[i]); }

void a3_vk_ref_wheels(A3WheelQuad *quads, u32 count) {
    for (u32 n = 0; n < count; ++n) {
        A3WheelQuad *q = &quads[n];
        for (int i = 0; i < 4; ++i) {
            f32 cp = q->comp[i];
            f32 ld = q->stiffness[i] * cp;
            f32 dp = q->damping[i] * q->comp_vel[i];
            ld = ld + dp;
            ld = vk_max(ld, 0.0f);
            b32 ground = 0.0f < cp;
            ld = ground ? ld : 0.0f;
            q->load[i] = ld;
            f32 limit = ld * q->mu[i];
            f32 vl = q->v_long[i];
            f32 den = vk_max(vk_abs(vl), 3.0f);
            f32 ratio = q->v_lat[i] / den;
            f32 alpha = vk_atan(ratio);
            f32 x = alpha * 16.0f;
            f32 ax = vk_atan(x);
            f32 arg = ax * 1.35f;
            f32 sy = vk_sin(arg);
            f32 fy0 = vk_neg(sy * limit);
            f32 lk = limit * 0.9f;
            f32 lkfx = vk_float((vk_bits(lk) | (vk_bits(vl) & 0x80000000u)) ^ 0x80000000u);
            b32 locked = 0.5f < q->lock[i];
            f32 req = locked ? lkfx : q->drive[i];
            f32 nl = vk_neg(limit);
            f32 fx = vk_max(req, nl);
            fx = vk_min(fx, limit);
            q->fx[i] = fx;
            f32 c2 = limit * limit;
            f32 f2 = fx * fx;
            c2 = c2 - f2;
            c2 = vk_max(c2, 0.0f);
            f32 cap = __builtin_sqrtf(c2);
            f32 t = fy0 * 0.4f;
            f32 fyl = locked ? t : fy0;
            f32 ncap = vk_neg(cap);
            f32 fy = vk_max(fyl, ncap);
            fy = vk_min(fy, cap);
            q->fy[i] = fy;
            f32 s1 = vk_abs(req) - limit;
            f32 lm = vk_max(limit, 1.0f);
            s1 = s1 / lm;
            f32 s2 = vk_abs(alpha) - 0.12f;
            s2 = s2 * 5.0f;
            f32 s = vk_max(s1, s2);
            s = vk_max(s, q->lock[i]);
            s = vk_max(s, 0.0f);
            s = vk_min(s, 1.0f);
            q->slip[i] = ground ? s : 0.0f;
        }
    }
}

#if A3_HAS_X64_ASM
void a3_vk_asm_wheels(A3WheelQuad *quads, u32 count);
void a3_vk_wheels(A3WheelQuad *quads, u32 count) { if (count) a3_vk_asm_wheels(quads, count); }
const char *a3_vk_backend(void) { return "x86-64 SSE assembly"; }
#else
void a3_vk_wheels(A3WheelQuad *quads, u32 count) { a3_vk_ref_wheels(quads, count); }
const char *a3_vk_backend(void) { return "C reference"; }
#endif
