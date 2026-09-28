/*
 * ASM3D - a3_simd_wasm.c
 * WebAssembly SIMD128 kernels. Same arithmetic order as a3_simd_ref.c, so
 * results are bit-identical to the native assembly and reference versions.
 */
#include "a3_math.h"

#if A3_HAS_WASM_SIMD
#include <wasm_simd128.h>

const char *a3_simd_backend_name(void) { return "WebAssembly SIMD128"; }

static inline void mat4_mul_cols(f32 *out, v128_t c0, v128_t c1, v128_t c2, v128_t c3, const f32 *b) {
    for (int col = 0; col < 4; ++col) {
        const f32 *bc = b + col * 4;
        v128_t r = wasm_f32x4_mul(c0, wasm_f32x4_splat(bc[0]));
        r = wasm_f32x4_add(r, wasm_f32x4_mul(c1, wasm_f32x4_splat(bc[1])));
        r = wasm_f32x4_add(r, wasm_f32x4_mul(c2, wasm_f32x4_splat(bc[2])));
        r = wasm_f32x4_add(r, wasm_f32x4_mul(c3, wasm_f32x4_splat(bc[3])));
        wasm_v128_store(out + col * 4, r);
    }
}

void a3_mat4_mul_batch(A3Mat4 *out, const A3Mat4 *a, const A3Mat4 *b, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        v128_t c0 = wasm_v128_load(a[i].m + 0), c1 = wasm_v128_load(a[i].m + 4);
        v128_t c2 = wasm_v128_load(a[i].m + 8), c3 = wasm_v128_load(a[i].m + 12);
        A3Mat4 bi = b[i]; /* copy guards against out aliasing b */
        mat4_mul_cols(out[i].m, c0, c1, c2, c3, bi.m);
    }
}

void a3_mat4_mul_one_many(A3Mat4 *out, const A3Mat4 *parent, const A3Mat4 *locals, u32 count) {
    v128_t c0 = wasm_v128_load(parent->m + 0), c1 = wasm_v128_load(parent->m + 4);
    v128_t c2 = wasm_v128_load(parent->m + 8), c3 = wasm_v128_load(parent->m + 12);
    for (u32 i = 0; i < count; ++i) {
        A3Mat4 li = locals[i];
        mat4_mul_cols(out[i].m, c0, c1, c2, c3, li.m);
    }
}

void a3_transform_points(A3Vec4 *out, const A3Mat4 *m, const A3Vec4 *in, u32 count) {
    v128_t c0 = wasm_v128_load(m->m + 0), c1 = wasm_v128_load(m->m + 4);
    v128_t c2 = wasm_v128_load(m->m + 8), c3 = wasm_v128_load(m->m + 12);
    for (u32 i = 0; i < count; ++i) {
        A3Vec4 v = in[i];
        v128_t r = wasm_f32x4_mul(c0, wasm_f32x4_splat(v.x));
        r = wasm_f32x4_add(r, wasm_f32x4_mul(c1, wasm_f32x4_splat(v.y)));
        r = wasm_f32x4_add(r, wasm_f32x4_mul(c2, wasm_f32x4_splat(v.z)));
        r = wasm_f32x4_add(r, wasm_f32x4_mul(c3, wasm_f32x4_splat(v.w)));
        wasm_v128_store(&out[i], r);
    }
}

u32 a3_wasm_cull_spheres_soa(const f32 *soa, const A3Vec4 *spheres, u8 *visible, u32 count) {
    v128_t x0 = wasm_v128_load(soa + 0), y0 = wasm_v128_load(soa + 4), z0 = wasm_v128_load(soa + 8), w0 = wasm_v128_load(soa + 12);
    v128_t x1 = wasm_v128_load(soa + 16), y1 = wasm_v128_load(soa + 20), z1 = wasm_v128_load(soa + 24), w1 = wasm_v128_load(soa + 28);
    u32 n = 0;
    for (u32 i = 0; i < count; ++i) {
        A3Vec4 s = spheres[i];
        v128_t cx = wasm_f32x4_splat(s.x), cy = wasm_f32x4_splat(s.y), cz = wasm_f32x4_splat(s.z);
        v128_t nr = wasm_f32x4_splat(-s.w);
        v128_t d0 = wasm_f32x4_mul(x0, cx);
        d0 = wasm_f32x4_add(d0, wasm_f32x4_mul(y0, cy));
        d0 = wasm_f32x4_add(d0, wasm_f32x4_mul(z0, cz));
        d0 = wasm_f32x4_add(d0, w0);
        v128_t d1 = wasm_f32x4_mul(x1, cx);
        d1 = wasm_f32x4_add(d1, wasm_f32x4_mul(y1, cy));
        d1 = wasm_f32x4_add(d1, wasm_f32x4_mul(z1, cz));
        d1 = wasm_f32x4_add(d1, w1);
        v128_t rej = wasm_v128_or(wasm_f32x4_lt(d0, nr), wasm_f32x4_lt(d1, nr));
        u8 vis = wasm_v128_any_true(rej) ? 0 : 1;
        visible[i] = vis;
        n += vis;
    }
    return n;
}

void a3_integrate_v4(A3Vec4 *pos, const A3Vec4 *vel, f32 dt, u32 count) {
    v128_t vdt = wasm_f32x4_splat(dt);
    for (u32 i = 0; i < count; ++i) {
        v128_t p = wasm_v128_load(&pos[i]);
        v128_t v = wasm_v128_load(&vel[i]);
        wasm_v128_store(&pos[i], wasm_f32x4_add(p, wasm_f32x4_mul(v, vdt)));
    }
}
#endif
