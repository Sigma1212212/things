/*
 * ASM3D - a3_simd_ref.c
 * Portable reference kernels + dispatch to the fastest compiled backend.
 * The reference kernels define the exact arithmetic order that the assembly
 * and wasm SIMD versions reproduce.
 */
#include "a3_math.h"

void a3_ref_mat4_mul(A3Mat4 *out, const A3Mat4 *a, const A3Mat4 *b) {
    A3Mat4 r;
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            f32 v = a->m[0 * 4 + row] * b->m[c * 4 + 0];
            v = v + a->m[1 * 4 + row] * b->m[c * 4 + 1];
            v = v + a->m[2 * 4 + row] * b->m[c * 4 + 2];
            v = v + a->m[3 * 4 + row] * b->m[c * 4 + 3];
            r.m[c * 4 + row] = v;
        }
    }
    *out = r;
}

void a3_ref_transform_points(A3Vec4 *out, const A3Mat4 *mat, const A3Vec4 *in, u32 count) {
    const f32 *m = mat->m;
    for (u32 i = 0; i < count; ++i) {
        A3Vec4 v = in[i];
        f32 x = m[0] * v.x; x = x + m[4] * v.y; x = x + m[8] * v.z; x = x + m[12] * v.w;
        f32 y = m[1] * v.x; y = y + m[5] * v.y; y = y + m[9] * v.z; y = y + m[13] * v.w;
        f32 z = m[2] * v.x; z = z + m[6] * v.y; z = z + m[10] * v.z; z = z + m[14] * v.w;
        f32 w = m[3] * v.x; w = w + m[7] * v.y; w = w + m[11] * v.z; w = w + m[15] * v.w;
        out[i] = a3_v4(x, y, z, w);
    }
}

u32 a3_ref_cull_spheres(const A3Frustum *f, const A3Vec4 *spheres, u8 *visible, u32 count) {
    u32 n = 0;
    for (u32 i = 0; i < count; ++i) {
        A3Vec4 s = spheres[i];
        f32 neg_r = -s.w;
        u8 vis = 1;
        for (int p = 0; p < 6; ++p) {
            A3Vec4 pl = f->planes[p];
            f32 d = pl.x * s.x;
            d = d + pl.y * s.y;
            d = d + pl.z * s.z;
            d = d + pl.w;
            if (d < neg_r) { vis = 0; break; }
        }
        visible[i] = vis;
        n += vis;
    }
    return n;
}

void a3_ref_integrate_v4(A3Vec4 *pos, const A3Vec4 *vel, f32 dt, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        pos[i].x = pos[i].x + vel[i].x * dt;
        pos[i].y = pos[i].y + vel[i].y * dt;
        pos[i].z = pos[i].z + vel[i].z * dt;
        pos[i].w = pos[i].w + vel[i].w * dt;
    }
}

/* Transposes frustum planes into 8 SoA vectors (planes 6,7 are always-pass
 * padding) for the 4-wide kernels. */
static void frustum_soa(const A3Frustum *f, f32 soa[32]) {
    for (int group = 0; group < 2; ++group) {
        for (int lane = 0; lane < 4; ++lane) {
            int p = group * 4 + lane;
            A3Vec4 pl = p < 6 ? f->planes[p] : a3_v4(0, 0, 0, 1e30f);
            soa[group * 16 + 0 + lane] = pl.x;
            soa[group * 16 + 4 + lane] = pl.y;
            soa[group * 16 + 8 + lane] = pl.z;
            soa[group * 16 + 12 + lane] = pl.w;
        }
    }
}

#if A3_HAS_X64_ASM
void a3_asm_mat4_mul_batch(A3Mat4 *out, const A3Mat4 *a, const A3Mat4 *b, u32 count);
void a3_asm_mat4_mul_one_many(A3Mat4 *out, const A3Mat4 *parent, const A3Mat4 *locals, u32 count);
void a3_asm_transform_points(A3Vec4 *out, const A3Mat4 *m, const A3Vec4 *in, u32 count);
u32  a3_asm_cull_spheres_soa(const f32 *soa, const A3Vec4 *spheres, u8 *visible, u32 count);
void a3_asm_integrate_v4(A3Vec4 *pos, const A3Vec4 *vel, f32 dt, u32 count);

const char *a3_simd_backend_name(void) { return "x86-64 SSE assembly"; }
void a3_mat4_mul_batch(A3Mat4 *out, const A3Mat4 *a, const A3Mat4 *b, u32 count) { a3_asm_mat4_mul_batch(out, a, b, count); }
void a3_mat4_mul_one_many(A3Mat4 *out, const A3Mat4 *parent, const A3Mat4 *locals, u32 count) {
    a3_asm_mat4_mul_one_many(out, parent, locals, count);
}
void a3_transform_points(A3Vec4 *out, const A3Mat4 *m, const A3Vec4 *in, u32 count) { a3_asm_transform_points(out, m, in, count); }
u32 a3_cull_spheres(const A3Frustum *f, const A3Vec4 *spheres, u8 *visible, u32 count) {
    A3_ALIGNAS(16) f32 soa[32];
    frustum_soa(f, soa);
    return a3_asm_cull_spheres_soa(soa, spheres, visible, count);
}
void a3_integrate_v4(A3Vec4 *pos, const A3Vec4 *vel, f32 dt, u32 count) { a3_asm_integrate_v4(pos, vel, dt, count); }

#elif A3_HAS_WASM_SIMD
/* Implemented in a3_simd_wasm.c */
u32 a3_wasm_cull_spheres_soa(const f32 *soa, const A3Vec4 *spheres, u8 *visible, u32 count);
u32 a3_cull_spheres(const A3Frustum *f, const A3Vec4 *spheres, u8 *visible, u32 count) {
    A3_ALIGNAS(16) f32 soa[32];
    frustum_soa(f, soa);
    return a3_wasm_cull_spheres_soa(soa, spheres, visible, count);
}

#else
const char *a3_simd_backend_name(void) { return "portable C"; }
void a3_mat4_mul_batch(A3Mat4 *out, const A3Mat4 *a, const A3Mat4 *b, u32 count) {
    for (u32 i = 0; i < count; ++i) a3_ref_mat4_mul(&out[i], &a[i], &b[i]);
}
void a3_mat4_mul_one_many(A3Mat4 *out, const A3Mat4 *parent, const A3Mat4 *locals, u32 count) {
    A3Mat4 p = *parent;
    for (u32 i = 0; i < count; ++i) a3_ref_mat4_mul(&out[i], &p, &locals[i]);
}
void a3_transform_points(A3Vec4 *out, const A3Mat4 *m, const A3Vec4 *in, u32 count) { a3_ref_transform_points(out, m, in, count); }
u32 a3_cull_spheres(const A3Frustum *f, const A3Vec4 *spheres, u8 *visible, u32 count) {
    (void)frustum_soa;
    return a3_ref_cull_spheres(f, spheres, visible, count);
}
void a3_integrate_v4(A3Vec4 *pos, const A3Vec4 *vel, f32 dt, u32 count) { a3_ref_integrate_v4(pos, vel, dt, count); }
#endif
