/*
 * ASM3D - a3_physics_ref.c
 * C reference kernels. The operation order deliberately mirrors the SSE
 * assembly (e.g. horizontal sums are ((x + z) + (y + w))) so both produce
 * bit-identical results. Do not "simplify" the arithmetic here without
 * updating a3_physics_x64.S.
 */
#include "a3_physics_kernels.h"

/* Horizontal sum in the same order as the assembly's movhlps/shuffle pattern. */
static inline f32 hsum4(f32 x, f32 y, f32 z, f32 w) { return (x + z) + (y + w); }
static inline f32 dot4(A3Vec4 a, A3Vec4 b) { return hsum4(a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w); }
/* SSE min/max semantics: second operand is returned when either is NaN. */
static inline f32 sse_min(f32 a, f32 b) { return a < b ? a : b; }
static inline f32 sse_max(f32 a, f32 b) { return a > b ? a : b; }

void a3_phys_ref_integrate_velocities(A3Vec4 *v, A3Vec4 *w, const A3Vec4 *a, const A3Vec4 *d, f32 dt, u32 n) {
    for (u32 i = 0; i < n; ++i) {
        f32 lf = d[i].x, af = d[i].y;
        v[i].x = (v[i].x + a[i].x * dt) * lf;
        v[i].y = (v[i].y + a[i].y * dt) * lf;
        v[i].z = (v[i].z + a[i].z * dt) * lf;
        v[i].w = (v[i].w + a[i].w * dt) * lf;
        w[i].x = w[i].x * af;
        w[i].y = w[i].y * af;
        w[i].z = w[i].z * af;
        w[i].w = w[i].w * af;
    }
}

void a3_phys_ref_integrate_transforms(A3Vec4 *p, A3Quat *q, const A3Vec4 *v, const A3Vec4 *w, f32 dt, u32 n) {
    f32 hdt = 0.5f * dt;
    for (u32 i = 0; i < n; ++i) {
        p[i].x = p[i].x + v[i].x * dt;
        p[i].y = p[i].y + v[i].y * dt;
        p[i].z = p[i].z + v[i].z * dt;
        p[i].w = p[i].w + v[i].w * dt;
        f32 wx = w[i].x, wy = w[i].y, wz = w[i].z;
        f32 qx = q[i].x, qy = q[i].y, qz = q[i].z, qw = q[i].w;
        /* A = (wx*qw, wy*qw, wz*qw, wx*qx); B = (wy*qz, wz*qx, wx*qy, wy*qy); C = (wz*qy, wx*qz, wy*qx, wz*qz) */
        f32 dx = (wx * qw + wy * qz) - wz * qy;
        f32 dy = (wy * qw + wz * qx) - wx * qz;
        f32 dz = (wz * qw + wx * qy) - wy * qx;
        f32 dw = -((wx * qx + wy * qy) + wz * qz);
        qx = qx + dx * hdt;
        qy = qy + dy * hdt;
        qz = qz + dz * hdt;
        qw = qw + dw * hdt;
        f32 len2 = hsum4(qx * qx, qy * qy, qz * qz, qw * qw);
        if (len2 < 1e-12f) { q[i] = a3_quat(0, 0, 0, 1); continue; }
        f32 len = __builtin_sqrtf(len2);
        q[i] = a3_quat(qx / len, qy / len, qz / len, qw / len);
    }
}

void a3_phys_ref_solve_rows(A3SolverRow *rows, u32 n, A3Vec4 *lv, A3Vec4 *av) {
    for (u32 i = 0; i < n; ++i) {
        A3SolverRow *r = &rows[i];
        A3Vec4 *va = &lv[r->body_a], *vb = &lv[r->body_b];
        A3Vec4 *wa = &av[r->body_a], *wb = &av[r->body_b];
        f32 lo = r->lo, hi = r->hi;
        if (r->friction_of >= 0) {
            hi = r->mu * rows[r->friction_of].acc;
            lo = -hi;
        }
        f32 vrel = ((dot4(r->lin, *vb) - dot4(r->lin, *va)) + dot4(r->ang_b, *wb)) - dot4(r->ang_a, *wa);
        f32 lambda = -(vrel + r->bias) * r->eff_mass;
        f32 old = r->acc;
        f32 acc = old + lambda;
        acc = sse_max(acc, lo);
        acc = sse_min(acc, hi);
        lambda = acc - old;
        r->acc = acc;
        f32 la = lambda * r->inv_mass_a, lb = lambda * r->inv_mass_b;
        va->x = va->x - r->lin.x * la; va->y = va->y - r->lin.y * la; va->z = va->z - r->lin.z * la; va->w = va->w - r->lin.w * la;
        vb->x = vb->x + r->lin.x * lb; vb->y = vb->y + r->lin.y * lb; vb->z = vb->z + r->lin.z * lb; vb->w = vb->w + r->lin.w * lb;
        wa->x = wa->x - r->ia.x * lambda; wa->y = wa->y - r->ia.y * lambda; wa->z = wa->z - r->ia.z * lambda; wa->w = wa->w - r->ia.w * lambda;
        wb->x = wb->x + r->ib.x * lambda; wb->y = wb->y + r->ib.y * lambda; wb->z = wb->z + r->ib.z * lambda; wb->w = wb->w + r->ib.w * lambda;
    }
}

static inline f32 fabs_bits(f32 x) { return __builtin_fabsf(x); }

void a3_phys_ref_compute_aabbs(const A3Vec4 *c, const A3Vec4 *cols, const A3Vec4 *h, A3Vec4 *mn, A3Vec4 *mx, u32 n) {
    for (u32 i = 0; i < n; ++i) {
        const A3Vec4 *c0 = &cols[i * 3], *c1 = &cols[i * 3 + 1], *c2 = &cols[i * 3 + 2];
        f32 e[4];
        const f32 *a0 = &c0->x, *a1 = &c1->x, *a2 = &c2->x;
        for (int k = 0; k < 4; ++k) e[k] = (fabs_bits(a0[k]) * h[i].x + fabs_bits(a1[k]) * h[i].y) + fabs_bits(a2[k]) * h[i].z;
        mn[i] = a3_v4(c[i].x - e[0], c[i].y - e[1], c[i].z - e[2], c[i].w - e[3]);
        mx[i] = a3_v4(c[i].x + e[0], c[i].y + e[1], c[i].z + e[2], c[i].w + e[3]);
    }
}

u32 a3_phys_ref_sap_pairs(const A3Vec4 *mn, const A3Vec4 *mx, u32 n, u32 *out, u32 max_pairs) {
    u32 found = 0;
    for (u32 i = 0; i < n; ++i) {
        for (u32 j = i + 1; j < n; ++j) {
            if (mn[j].x > mx[i].x) break;
            if (mn[i].x <= mx[j].x && mn[j].x <= mx[i].x &&
                mn[i].y <= mx[j].y && mn[j].y <= mx[i].y &&
                mn[i].z <= mx[j].z && mn[j].z <= mx[i].z) {
                if (found >= max_pairs) return found;
                out[found * 2] = i;
                out[found * 2 + 1] = j;
                found++;
            }
        }
    }
    return found;
}

u32 a3_phys_ref_ray_aabbs(const A3Vec4 *op, const A3Vec4 *invp, f32 t_max, const A3Vec4 *mn, const A3Vec4 *mx, u32 n, f32 *out_t) {
    const A3Vec4 o = *op, inv = *invp;
    u32 hits = 0;
    for (u32 i = 0; i < n; ++i) {
        f32 t1x = (mn[i].x - o.x) * inv.x, t2x = (mx[i].x - o.x) * inv.x;
        f32 t1y = (mn[i].y - o.y) * inv.y, t2y = (mx[i].y - o.y) * inv.y;
        f32 t1z = (mn[i].z - o.z) * inv.z, t2z = (mx[i].z - o.z) * inv.z;
        /* per-lane near/far with SSE semantics; w lane: near = 0, far = t_max */
        f32 nx = sse_min(t1x, t2x), ny = sse_min(t1y, t2y), nz = sse_min(t1z, t2z), nw = 0.0f;
        f32 fx = sse_max(t1x, t2x), fy = sse_max(t1y, t2y), fz = sse_max(t1z, t2z), fw = t_max;
        f32 tn = sse_max(sse_max(nx, nz), sse_max(ny, nw));
        f32 tf = sse_min(sse_min(fx, fz), sse_min(fy, fw));
        if (tn <= tf) { out_t[i] = tn; hits++; }
        else out_t[i] = -1.0f;
    }
    return hits;
}

#if !A3_HAS_X64_ASM
const char *a3_phys_kernel_backend(void) { return "portable C"; }
void a3_phys_integrate_velocities(A3Vec4 *v, A3Vec4 *w, const A3Vec4 *a, const A3Vec4 *d, f32 dt, u32 n) { a3_phys_ref_integrate_velocities(v, w, a, d, dt, n); }
void a3_phys_integrate_transforms(A3Vec4 *p, A3Quat *q, const A3Vec4 *v, const A3Vec4 *w, f32 dt, u32 n) { a3_phys_ref_integrate_transforms(p, q, v, w, dt, n); }
void a3_phys_solve_rows(A3SolverRow *r, u32 n, A3Vec4 *lv, A3Vec4 *av) { a3_phys_ref_solve_rows(r, n, lv, av); }
void a3_phys_compute_aabbs(const A3Vec4 *c, const A3Vec4 *cols, const A3Vec4 *h, A3Vec4 *mn, A3Vec4 *mx, u32 n) { a3_phys_ref_compute_aabbs(c, cols, h, mn, mx, n); }
u32 a3_phys_sap_pairs(const A3Vec4 *mn, const A3Vec4 *mx, u32 n, u32 *out, u32 max_pairs) { return a3_phys_ref_sap_pairs(mn, mx, n, out, max_pairs); }
u32 a3_phys_ray_aabbs(const A3Vec4 *o, const A3Vec4 *inv, f32 t_max, const A3Vec4 *mn, const A3Vec4 *mx, u32 n, f32 *out_t) { return a3_phys_ref_ray_aabbs(o, inv, t_max, mn, mx, n, out_t); }
#else
const char *a3_phys_kernel_backend(void) { return "x86-64 SSE assembly"; }
#endif
