/*
 * ASM3D - a3_collide.c
 * Narrowphase contact generation and the static-mesh BVH.
 * Algorithms follow standard references (Ericson, "Real-Time Collision
 * Detection"; SAT with reference-face clipping for boxes).
 */
#include "a3_physics_internal.h"
#include "../core/a3_memory.h"
#include "../core/a3_string.h"
#include "../core/a3_log.h"

/* ======================================================================== */
/* Geometry helpers                                                         */
/* ======================================================================== */

A3Vec3 a3_closest_point_triangle(A3Vec3 p, A3Vec3 a, A3Vec3 b, A3Vec3 c) {
    A3Vec3 ab = a3_v3_sub(b, a), ac = a3_v3_sub(c, a), ap = a3_v3_sub(p, a);
    f32 d1 = a3_v3_dot(ab, ap), d2 = a3_v3_dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return a;
    A3Vec3 bp = a3_v3_sub(p, b);
    f32 d3 = a3_v3_dot(ab, bp), d4 = a3_v3_dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return b;
    f32 vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return a3_v3_add(a, a3_v3_scale(ab, d1 / (d1 - d3)));
    A3Vec3 cp = a3_v3_sub(p, c);
    f32 d5 = a3_v3_dot(ab, cp), d6 = a3_v3_dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return c;
    f32 vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return a3_v3_add(a, a3_v3_scale(ac, d2 / (d2 - d6)));
    f32 va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0)
        return a3_v3_add(b, a3_v3_scale(a3_v3_sub(c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6))));
    f32 denom = 1.0f / (va + vb + vc);
    f32 v = vb * denom, w = vc * denom;
    return a3_v3_add(a, a3_v3_add(a3_v3_scale(ab, v), a3_v3_scale(ac, w)));
}

f32 a3_closest_segment_segment(A3Vec3 p1, A3Vec3 q1, A3Vec3 p2, A3Vec3 q2, f32 *s, f32 *t, A3Vec3 *c1, A3Vec3 *c2) {
    A3Vec3 d1 = a3_v3_sub(q1, p1), d2 = a3_v3_sub(q2, p2), r = a3_v3_sub(p1, p2);
    f32 a = a3_v3_dot(d1, d1), e = a3_v3_dot(d2, d2), f = a3_v3_dot(d2, r);
    f32 ss, tt;
    if (a <= 1e-9f && e <= 1e-9f) { ss = tt = 0; }
    else if (a <= 1e-9f) { ss = 0; tt = a3_clampf(f / e, 0, 1); }
    else {
        f32 c = a3_v3_dot(d1, r);
        if (e <= 1e-9f) { tt = 0; ss = a3_clampf(-c / a, 0, 1); }
        else {
            f32 b = a3_v3_dot(d1, d2), denom = a * e - b * b;
            ss = denom != 0 ? a3_clampf((b * f - c * e) / denom, 0, 1) : 0;
            tt = (b * ss + f) / e;
            if (tt < 0) { tt = 0; ss = a3_clampf(-c / a, 0, 1); }
            else if (tt > 1) { tt = 1; ss = a3_clampf((b - c) / a, 0, 1); }
        }
    }
    A3Vec3 x1 = a3_v3_madd(p1, d1, ss), x2 = a3_v3_madd(p2, d2, tt);
    if (s) *s = ss;
    if (t) *t = tt;
    if (c1) *c1 = x1;
    if (c2) *c2 = x2;
    return a3_v3_dist_sq(x1, x2);
}

f32 a3_closest_segment_triangle(A3Vec3 p, A3Vec3 q, A3Vec3 a, A3Vec3 b, A3Vec3 c, A3Vec3 *on_seg, A3Vec3 *on_tri) {
    /* segment crossing the triangle? */
    A3Vec3 n = a3_v3_cross(a3_v3_sub(b, a), a3_v3_sub(c, a));
    f32 nl = a3_v3_len(n);
    if (nl > 1e-12f) {
        n = a3_v3_scale(n, 1.0f / nl);
        f32 dp = a3_v3_dot(a3_v3_sub(p, a), n), dq = a3_v3_dot(a3_v3_sub(q, a), n);
        if ((dp <= 0 && dq >= 0) || (dp >= 0 && dq <= 0)) {
            f32 denom = dp - dq;
            f32 t = a3_absf(denom) > 1e-12f ? dp / denom : 0;
            A3Vec3 x = a3_v3_lerp(p, q, t);
            A3Vec3 ct = a3_closest_point_triangle(x, a, b, c);
            if (a3_v3_dist_sq(ct, x) < 1e-10f) { *on_seg = x; *on_tri = x; return 0; }
        }
    }
    f32 best = A3_F32_MAX;
    A3Vec3 bs = p, bt = a;
    A3Vec3 cp = a3_closest_point_triangle(p, a, b, c);
    f32 d = a3_v3_dist_sq(p, cp);
    if (d < best) { best = d; bs = p; bt = cp; }
    cp = a3_closest_point_triangle(q, a, b, c);
    d = a3_v3_dist_sq(q, cp);
    if (d < best) { best = d; bs = q; bt = cp; }
    const A3Vec3 e[3][2] = { { a, b }, { b, c }, { c, a } };
    for (int i = 0; i < 3; ++i) {
        A3Vec3 x1, x2;
        d = a3_closest_segment_segment(p, q, e[i][0], e[i][1], 0, 0, &x1, &x2);
        if (d < best) { best = d; bs = x1; bt = x2; }
    }
    *on_seg = bs;
    *on_tri = bt;
    return best;
}

void a3_capsule_segment(const A3ShapeInstance *s, A3Vec3 *p0, A3Vec3 *p1) {
    A3Vec3 axis = a3_v3(s->rot_m.m[4], s->rot_m.m[5], s->rot_m.m[6]); /* local +Y */
    *p0 = a3_v3_madd(s->pos, axis, -s->half_height);
    *p1 = a3_v3_madd(s->pos, axis, s->half_height);
}

static A3Vec3 box_axis(const A3ShapeInstance *s, int i) { return a3_v3(s->rot_m.m[i * 4], s->rot_m.m[i * 4 + 1], s->rot_m.m[i * 4 + 2]); }
static f32 v3c(A3Vec3 v, int i) { return i == 0 ? v.x : (i == 1 ? v.y : v.z); }

static void manifold_add(A3Manifold *m, A3Vec3 p, f32 depth) {
    if (m->count < A3_MAX_MANIFOLD) { m->pts[m->count].pos = p; m->pts[m->count].depth = depth; m->count++; return; }
    /* replace the shallowest point if deeper */
    u32 s = 0;
    for (u32 i = 1; i < m->count; ++i) if (m->pts[i].depth < m->pts[s].depth) s = i;
    if (depth > m->pts[s].depth) { m->pts[s].pos = p; m->pts[s].depth = depth; }
}

/* Reduces an arbitrary point set to <= 4 points spanning the largest area. */
static void reduce_points(const A3ContactPoint *in, u32 n, A3Vec3 normal, A3Manifold *m) {
    m->count = 0;
    if (n <= 4) { for (u32 i = 0; i < n; ++i) m->pts[m->count++] = in[i]; return; }
    u32 i0 = 0;
    for (u32 i = 1; i < n; ++i) if (in[i].depth > in[i0].depth) i0 = i;
    u32 i1 = i0;
    f32 best = -1;
    for (u32 i = 0; i < n; ++i) { f32 d = a3_v3_dist_sq(in[i].pos, in[i0].pos); if (d > best) { best = d; i1 = i; } }
    u32 i2 = i0;
    best = -1;
    for (u32 i = 0; i < n; ++i) {
        f32 a = a3_v3_dot(a3_v3_cross(a3_v3_sub(in[i1].pos, in[i0].pos), a3_v3_sub(in[i].pos, in[i0].pos)), normal);
        if (a3_absf(a) > best) { best = a3_absf(a); i2 = i; }
    }
    u32 i3 = i0;
    best = -1;
    A3Vec3 cen = a3_v3_scale(a3_v3_add(a3_v3_add(in[i0].pos, in[i1].pos), in[i2].pos), 1.0f / 3.0f);
    for (u32 i = 0; i < n; ++i) {
        if (i == i0 || i == i1 || i == i2) continue;
        f32 d = a3_v3_dist_sq(in[i].pos, cen);
        if (d > best) { best = d; i3 = i; }
    }
    u32 ids[4] = { i0, i1, i2, i3 };
    for (u32 k = 0; k < 4; ++k) {
        b32 dup = 0;
        for (u32 j = 0; j < k; ++j) if (ids[j] == ids[k]) dup = 1;
        if (!dup) m->pts[m->count++] = in[ids[k]];
    }
}

/* ======================================================================== */
/* Pairs                                                                    */
/* ======================================================================== */

static b32 sphere_sphere(A3Vec3 ca, f32 ra, A3Vec3 cb, f32 rb, A3Manifold *m) {
    A3Vec3 d = a3_v3_sub(cb, ca);
    f32 dist2 = a3_v3_len_sq(d), r = ra + rb;
    if (dist2 >= r * r) return 0;
    f32 dist = a3_sqrtf(dist2);
    A3Vec3 n = dist > 1e-6f ? a3_v3_scale(d, 1.0f / dist) : a3_v3(0, 1, 0);
    m->normal = n;
    m->count = 1;
    m->pts[0].pos = a3_v3_madd(cb, n, -rb);
    m->pts[0].depth = r - dist;
    return 1;
}

/* Sphere (center c, radius r) against box b. Normal from box to sphere. */
static b32 box_sphere(const A3ShapeInstance *b, A3Vec3 c, f32 r, A3Manifold *m) {
    A3Vec3 d = a3_v3_sub(c, b->pos);
    A3Vec3 local = a3_v3(a3_v3_dot(d, box_axis(b, 0)), a3_v3_dot(d, box_axis(b, 1)), a3_v3_dot(d, box_axis(b, 2)));
    A3Vec3 cl = a3_v3(a3_clampf(local.x, -b->half.x, b->half.x), a3_clampf(local.y, -b->half.y, b->half.y), a3_clampf(local.z, -b->half.z, b->half.z));
    A3Vec3 diff = a3_v3_sub(local, cl);
    f32 dist2 = a3_v3_len_sq(diff);
    A3Vec3 n_local;
    f32 depth;
    if (dist2 > 1e-12f) {
        if (dist2 >= r * r) return 0;
        f32 dist = a3_sqrtf(dist2);
        n_local = a3_v3_scale(diff, 1.0f / dist);
        depth = r - dist;
    } else {
        /* center inside: push out through the nearest face */
        f32 dx = b->half.x - a3_absf(local.x), dy = b->half.y - a3_absf(local.y), dz = b->half.z - a3_absf(local.z);
        if (dx <= dy && dx <= dz) { n_local = a3_v3(local.x >= 0 ? 1.0f : -1.0f, 0, 0); depth = dx + r; cl.x = n_local.x * b->half.x; }
        else if (dy <= dz) { n_local = a3_v3(0, local.y >= 0 ? 1.0f : -1.0f, 0); depth = dy + r; cl.y = n_local.y * b->half.y; }
        else { n_local = a3_v3(0, 0, local.z >= 0 ? 1.0f : -1.0f); depth = dz + r; cl.z = n_local.z * b->half.z; }
    }
    A3Vec3 n = a3_mat4_mul_dir(&b->rot_m, n_local);
    m->normal = n;
    m->count = 1;
    m->pts[0].pos = a3_v3_madd(c, n, -r);
    m->pts[0].depth = depth;
    return 1;
}

static f32 dist2_point_box_local(A3Vec3 p, A3Vec3 h) {
    f32 dx = a3_maxf(a3_absf(p.x) - h.x, 0), dy = a3_maxf(a3_absf(p.y) - h.y, 0), dz = a3_maxf(a3_absf(p.z) - h.z, 0);
    return dx * dx + dy * dy + dz * dz;
}

/* Capsule vs box: endpoint spheres + closest-point sphere, merged. Normal from box to capsule. */
static b32 box_capsule(const A3ShapeInstance *b, const A3ShapeInstance *cap, A3Manifold *m) {
    A3Vec3 p0, p1;
    a3_capsule_segment(cap, &p0, &p1);
    /* ternary search for the segment point closest to the box (convex distance) */
    A3Vec3 lp0 = a3_v3_sub(p0, b->pos), lp1 = a3_v3_sub(p1, b->pos);
    lp0 = a3_v3(a3_v3_dot(lp0, box_axis(b, 0)), a3_v3_dot(lp0, box_axis(b, 1)), a3_v3_dot(lp0, box_axis(b, 2)));
    lp1 = a3_v3(a3_v3_dot(lp1, box_axis(b, 0)), a3_v3_dot(lp1, box_axis(b, 1)), a3_v3_dot(lp1, box_axis(b, 2)));
    f32 lo = 0, hi = 1;
    for (int it = 0; it < 24; ++it) {
        f32 m1 = lo + (hi - lo) / 3.0f, m2 = hi - (hi - lo) / 3.0f;
        if (dist2_point_box_local(a3_v3_lerp(lp0, lp1, m1), b->half) <= dist2_point_box_local(a3_v3_lerp(lp0, lp1, m2), b->half)) hi = m2;
        else lo = m1;
    }
    f32 tbest = (lo + hi) * 0.5f;
    if (dist2_point_box_local(lp0, b->half) < 1e-12f || dist2_point_box_local(lp1, b->half) < 1e-12f) {
        /* segment penetrates: use the point closest to the box center */
        A3Vec3 d = a3_v3_sub(lp1, lp0);
        f32 dd = a3_v3_len_sq(d);
        tbest = dd > 1e-12f ? a3_clampf(-a3_v3_dot(lp0, d) / dd, 0, 1) : 0;
    }
    A3Vec3 cands[3] = { p0, p1, a3_v3_lerp(p0, p1, tbest) };
    A3Manifold best;
    a3_zero_struct(&best);
    b32 any = 0;
    A3ContactPoint pts[3];
    A3Vec3 normals[3];
    u32 np = 0;
    for (int i = 0; i < 3; ++i) {
        A3Manifold t;
        if (box_sphere(b, cands[i], cap->radius, &t)) {
            pts[np] = t.pts[0];
            normals[np] = t.normal;
            np++;
            if (!any || t.pts[0].depth > best.pts[0].depth) best = t;
            any = 1;
        }
    }
    if (!any) return 0;
    m->normal = best.normal;
    m->count = 0;
    for (u32 i = 0; i < np; ++i) {
        if (a3_v3_dot(normals[i], best.normal) < 0.95f) continue;
        b32 dup = 0;
        for (u32 k = 0; k < m->count; ++k) if (a3_v3_dist_sq(m->pts[k].pos, pts[i].pos) < 1e-6f) dup = 1;
        if (!dup) manifold_add(m, pts[i].pos, pts[i].depth);
    }
    if (!m->count) { m->count = 1; m->pts[0] = best.pts[0]; }
    return 1;
}

static b32 capsule_capsule(const A3ShapeInstance *a, const A3ShapeInstance *b, A3Manifold *m) {
    A3Vec3 a0, a1, b0, b1, ca, cb;
    a3_capsule_segment(a, &a0, &a1);
    a3_capsule_segment(b, &b0, &b1);
    a3_closest_segment_segment(a0, a1, b0, b1, 0, 0, &ca, &cb);
    if (!sphere_sphere(ca, a->radius, cb, b->radius, m)) return 0;
    /* parallel capsules lying side by side: add a second point for stability */
    A3Vec3 da = a3_v3_norm(a3_v3_sub(a1, a0)), db = a3_v3_norm(a3_v3_sub(b1, b0));
    if (a3_absf(a3_v3_dot(da, db)) > 0.98f) {
        A3Manifold t;
        A3Vec3 x1, x2;
        A3Vec3 ends[2] = { b0, b1 };
        for (int i = 0; i < 2; ++i) {
            a3_closest_segment_segment(a0, a1, ends[i], ends[i], 0, 0, &x1, &x2);
            if (sphere_sphere(x1, a->radius, x2, b->radius, &t) && a3_v3_dist_sq(t.pts[0].pos, m->pts[0].pos) > 1e-4f)
                manifold_add(m, t.pts[0].pos, t.pts[0].depth);
        }
    }
    return 1;
}

/* Box vs box: separating axis test + reference face clipping. */
static b32 box_box(const A3ShapeInstance *a, const A3ShapeInstance *b, A3Manifold *m) {
    A3Vec3 A[3] = { box_axis(a, 0), box_axis(a, 1), box_axis(a, 2) };
    A3Vec3 B[3] = { box_axis(b, 0), box_axis(b, 1), box_axis(b, 2) };
    f32 ea[3] = { a->half.x, a->half.y, a->half.z }, eb[3] = { b->half.x, b->half.y, b->half.z };
    A3Vec3 T = a3_v3_sub(b->pos, a->pos);
    f32 best_pen = A3_F32_MAX;
    int best_axis = -1;
    A3Vec3 best_n = a3_v3(0, 1, 0);
    for (int k = 0; k < 15; ++k) {
        A3Vec3 L;
        if (k < 3) L = A[k];
        else if (k < 6) L = B[k - 3];
        else L = a3_v3_cross(A[(k - 6) / 3], B[(k - 6) % 3]);
        f32 ll = a3_v3_len_sq(L);
        if (ll < 1e-8f) continue;
        L = a3_v3_scale(L, 1.0f / a3_sqrtf(ll));
        f32 ra = ea[0] * a3_absf(a3_v3_dot(A[0], L)) + ea[1] * a3_absf(a3_v3_dot(A[1], L)) + ea[2] * a3_absf(a3_v3_dot(A[2], L));
        f32 rb = eb[0] * a3_absf(a3_v3_dot(B[0], L)) + eb[1] * a3_absf(a3_v3_dot(B[1], L)) + eb[2] * a3_absf(a3_v3_dot(B[2], L));
        f32 dist = a3_v3_dot(T, L);
        f32 pen = ra + rb - a3_absf(dist);
        if (pen < 0) return 0;
        f32 score = k < 6 ? pen : pen * 1.05f + 0.005f; /* prefer face contacts (stable stacking) */
        f32 best_score = best_axis < 6 ? best_pen : best_pen * 1.05f + 0.005f;
        if (best_axis < 0 || score < best_score) {
            best_pen = pen;
            best_axis = k;
            best_n = dist < 0 ? a3_v3_neg(L) : L;
        }
    }
    m->normal = best_n;
    m->count = 0;
    if (best_axis >= 6) {
        /* edge-edge: one contact at the closest points of the two edges */
        int i = (best_axis - 6) / 3, j = (best_axis - 6) % 3;
        A3Vec3 pa = a->pos, pb = b->pos;
        for (int k = 0; k < 3; ++k) {
            if (k != i) pa = a3_v3_madd(pa, A[k], (a3_v3_dot(A[k], best_n) > 0 ? 1.0f : -1.0f) * ea[k]);
            if (k != j) pb = a3_v3_madd(pb, B[k], (a3_v3_dot(B[k], best_n) > 0 ? -1.0f : 1.0f) * eb[k]);
        }
        A3Vec3 c1, c2;
        a3_closest_segment_segment(a3_v3_madd(pa, A[i], -ea[i]), a3_v3_madd(pa, A[i], ea[i]),
                                   a3_v3_madd(pb, B[j], -eb[j]), a3_v3_madd(pb, B[j], eb[j]), 0, 0, &c1, &c2);
        m->count = 1;
        m->pts[0].pos = a3_v3_scale(a3_v3_add(c1, c2), 0.5f);
        m->pts[0].depth = best_pen;
        return 1;
    }
    /* face contact: reference face on the box owning the axis */
    b32 ref_is_a = best_axis < 3;
    const A3Vec3 *R = ref_is_a ? A : B, *I = ref_is_a ? B : A;
    const f32 *er = ref_is_a ? ea : eb, *ei = ref_is_a ? eb : ea;
    A3Vec3 rc = ref_is_a ? a->pos : b->pos, ic = ref_is_a ? b->pos : a->pos;
    A3Vec3 n = ref_is_a ? best_n : a3_v3_neg(best_n); /* points from reference to incident */
    int ra = ref_is_a ? best_axis : best_axis - 3;
    /* incident face: most anti-parallel to n */
    int ia = 0;
    f32 most = 0;
    for (int k = 0; k < 3; ++k) { f32 d = a3_absf(a3_v3_dot(I[k], n)); if (d > most) { most = d; ia = k; } }
    f32 sign = a3_v3_dot(I[ia], n) > 0 ? -1.0f : 1.0f;
    A3Vec3 fc = a3_v3_madd(ic, I[ia], sign * ei[ia]);
    int u = (ia + 1) % 3, v = (ia + 2) % 3;
    A3Vec3 poly[16], tmp[16];
    int pn = 4;
    poly[0] = a3_v3_madd(a3_v3_madd(fc, I[u], ei[u]), I[v], ei[v]);
    poly[1] = a3_v3_madd(a3_v3_madd(fc, I[u], -ei[u]), I[v], ei[v]);
    poly[2] = a3_v3_madd(a3_v3_madd(fc, I[u], -ei[u]), I[v], -ei[v]);
    poly[3] = a3_v3_madd(a3_v3_madd(fc, I[u], ei[u]), I[v], -ei[v]);
    /* clip against the 4 side planes of the reference face */
    for (int side = 0; side < 4 && pn > 0; ++side) {
        int ax = (ra + 1 + side / 2) % 3;
        f32 s = (side & 1) ? -1.0f : 1.0f;
        A3Vec3 pn_ = a3_v3_scale(R[ax], s);
        f32 off = a3_v3_dot(pn_, rc) + er[ax];
        int tn = 0;
        for (int k = 0; k < pn; ++k) {
            A3Vec3 p = poly[k], q = poly[(k + 1) % pn];
            f32 dp = a3_v3_dot(pn_, p) - off, dq = a3_v3_dot(pn_, q) - off;
            if (dp <= 0) tmp[tn++] = p;
            if ((dp <= 0) != (dq <= 0) && tn < 16) tmp[tn++] = a3_v3_lerp(p, q, dp / (dp - dq));
        }
        pn = tn;
        for (int k = 0; k < pn; ++k) poly[k] = tmp[k];
    }
    A3Vec3 ref_center = a3_v3_madd(rc, n, er[ra]);
    A3ContactPoint pts[16];
    u32 np = 0;
    for (int k = 0; k < pn; ++k) {
        f32 sep = a3_v3_dot(a3_v3_sub(poly[k], ref_center), n);
        if (sep <= 0.0f && np < 16) {
            /* contact point on B's surface */
            A3Vec3 on_ref = a3_v3_madd(poly[k], n, -sep);
            pts[np].pos = ref_is_a ? poly[k] : on_ref;
            pts[np].depth = -sep;
            np++;
        }
    }
    if (np == 0) {
        m->count = 1;
        m->pts[0].pos = a3_v3_scale(a3_v3_add(a->pos, b->pos), 0.5f);
        m->pts[0].depth = best_pen;
        return 1;
    }
    reduce_points(pts, np, best_n, m);
    return 1;
}

b32 a3_collide_convex(const A3ShapeInstance *a, const A3ShapeInstance *b, A3Manifold *m) {
    a3_zero_struct(m);
    A3ShapeType ta = a->type, tb = b->type;
    /* canonical order: box < sphere < capsule; flip normal when swapped */
    b32 swapped = 0;
    if ((ta == A3_SHAPE_SPHERE && tb == A3_SHAPE_BOX) || (ta == A3_SHAPE_CAPSULE && tb == A3_SHAPE_BOX) || (ta == A3_SHAPE_CAPSULE && tb == A3_SHAPE_SPHERE)) {
        const A3ShapeInstance *t = a; a = b; b = t;
        A3ShapeType tt = ta; ta = tb; tb = tt;
        swapped = 1;
    }
    b32 hit = 0;
    if (ta == A3_SHAPE_BOX && tb == A3_SHAPE_BOX) hit = box_box(a, b, m);
    else if (ta == A3_SHAPE_BOX && tb == A3_SHAPE_SPHERE) hit = box_sphere(a, b->pos, b->radius, m);
    else if (ta == A3_SHAPE_BOX && tb == A3_SHAPE_CAPSULE) hit = box_capsule(a, b, m);
    else if (ta == A3_SHAPE_SPHERE && tb == A3_SHAPE_SPHERE) hit = sphere_sphere(a->pos, a->radius, b->pos, b->radius, m);
    else if (ta == A3_SHAPE_SPHERE && tb == A3_SHAPE_CAPSULE) {
        A3Vec3 p0, p1, cp;
        a3_capsule_segment(b, &p0, &p1);
        a3_closest_segment_segment(a->pos, a->pos, p0, p1, 0, 0, 0, &cp);
        hit = sphere_sphere(a->pos, a->radius, cp, b->radius, m);
    } else if (ta == A3_SHAPE_CAPSULE && tb == A3_SHAPE_CAPSULE) hit = capsule_capsule(a, b, m);
    if (hit && swapped) {
        /* points were on (new) B = original A surface; move them onto original B */
        for (u32 i = 0; i < m->count; ++i) m->pts[i].pos = a3_v3_madd(m->pts[i].pos, m->normal, m->pts[i].depth);
        m->normal = a3_v3_neg(m->normal);
    }
    return hit;
}

/* ---- triangles ---- */

b32 a3_collide_triangle(const A3Vec3 tri[3], const A3ShapeInstance *b, A3Manifold *m) {
    a3_zero_struct(m);
    A3Vec3 tn = a3_v3_cross(a3_v3_sub(tri[1], tri[0]), a3_v3_sub(tri[2], tri[0]));
    f32 tl = a3_v3_len(tn);
    if (tl < 1e-12f) return 0;
    tn = a3_v3_scale(tn, 1.0f / tl);
    if (b->type == A3_SHAPE_SPHERE) {
        A3Vec3 cp = a3_closest_point_triangle(b->pos, tri[0], tri[1], tri[2]);
        A3Vec3 d = a3_v3_sub(b->pos, cp);
        f32 dist2 = a3_v3_len_sq(d);
        if (dist2 >= b->radius * b->radius) return 0;
        f32 dist = a3_sqrtf(dist2);
        A3Vec3 n = dist > 1e-6f ? a3_v3_scale(d, 1.0f / dist) : tn;
        m->normal = n;
        m->count = 1;
        m->pts[0].pos = a3_v3_madd(b->pos, n, -b->radius);
        m->pts[0].depth = b->radius - dist;
        return 1;
    }
    if (b->type == A3_SHAPE_CAPSULE) {
        A3Vec3 p0, p1, s, t;
        a3_capsule_segment(b, &p0, &p1);
        f32 d2 = a3_closest_segment_triangle(p0, p1, tri[0], tri[1], tri[2], &s, &t);
        if (d2 >= b->radius * b->radius) return 0;
        f32 dist = a3_sqrtf(d2);
        A3Vec3 n = dist > 1e-6f ? a3_v3_scale(a3_v3_sub(s, t), 1.0f / dist) : (a3_v3_dot(a3_v3_sub(b->pos, tri[0]), tn) >= 0 ? tn : a3_v3_neg(tn));
        m->normal = n;
        m->count = 0;
        manifold_add(m, a3_v3_madd(s, n, -b->radius), b->radius - dist);
        /* endpoints for a stable resting contact */
        A3Vec3 ends[2] = { p0, p1 };
        for (int i = 0; i < 2; ++i) {
            A3Vec3 cp = a3_closest_point_triangle(ends[i], tri[0], tri[1], tri[2]);
            A3Vec3 d = a3_v3_sub(ends[i], cp);
            f32 dd = a3_v3_len(d);
            if (dd < b->radius && dd > 1e-6f && a3_v3_dot(a3_v3_scale(d, 1.0f / dd), n) > 0.95f && a3_v3_dist_sq(cp, t) > 1e-4f)
                manifold_add(m, a3_v3_madd(ends[i], n, -b->radius), b->radius - dd);
        }
        return 1;
    }
    if (b->type == A3_SHAPE_BOX) {
        A3Vec3 Ax[3] = { box_axis(b, 0), box_axis(b, 1), box_axis(b, 2) };
        f32 e[3] = { b->half.x, b->half.y, b->half.z };
        A3Vec3 edges[3] = { a3_v3_sub(tri[1], tri[0]), a3_v3_sub(tri[2], tri[1]), a3_v3_sub(tri[0], tri[2]) };
        f32 best = A3_F32_MAX;
        A3Vec3 best_n = tn;
        for (int k = 0; k < 13; ++k) {
            A3Vec3 L = k == 0 ? tn : (k < 4 ? Ax[k - 1] : a3_v3_cross(Ax[(k - 4) / 3], edges[(k - 4) % 3]));
            f32 ll = a3_v3_len_sq(L);
            if (ll < 1e-8f) continue;
            L = a3_v3_scale(L, 1.0f / a3_sqrtf(ll));
            f32 r = e[0] * a3_absf(a3_v3_dot(Ax[0], L)) + e[1] * a3_absf(a3_v3_dot(Ax[1], L)) + e[2] * a3_absf(a3_v3_dot(Ax[2], L));
            f32 c = a3_v3_dot(b->pos, L);
            f32 t0 = a3_v3_dot(tri[0], L), t1 = a3_v3_dot(tri[1], L), t2 = a3_v3_dot(tri[2], L);
            f32 tmin = a3_minf(t0, a3_minf(t1, t2)), tmax = a3_maxf(t0, a3_maxf(t1, t2));
            f32 pen_pos = tmax - (c - r); /* box pushed along +L */
            f32 pen_neg = (c + r) - tmin; /* box pushed along -L */
            if (pen_pos < 0 || pen_neg < 0) return 0;
            if (k == 0) {
                /* triangle face: push towards the side the box center is on */
                b32 front = a3_v3_dot(a3_v3_sub(b->pos, tri[0]), tn) >= 0;
                best_n = front ? tn : a3_v3_neg(tn);
                best = front ? pen_pos : pen_neg;
                continue;
            }
            f32 pen = pen_pos < pen_neg ? pen_pos : pen_neg;
            A3Vec3 n = pen_pos < pen_neg ? L : a3_v3_neg(L);
            if (pen < best - 0.01f) { best = pen; best_n = n; } /* favour the face normal (fewer edge snags) */
        }
        m->normal = best_n;
        m->count = 0;
        /* contacts: box corners behind the triangle plane that project inside it */
        f32 plane_d = a3_v3_dot(tri[0], best_n);
        for (int i = 0; i < 8; ++i) {
            A3Vec3 p = b->pos;
            for (int k = 0; k < 3; ++k) p = a3_v3_madd(p, Ax[k], ((i >> k) & 1) ? e[k] : -e[k]);
            f32 d = a3_v3_dot(p, best_n) - plane_d;
            if (d >= 0) continue;
            A3Vec3 cp = a3_closest_point_triangle(p, tri[0], tri[1], tri[2]);
            A3Vec3 proj = a3_v3_madd(p, best_n, -d);
            if (a3_v3_dist_sq(cp, proj) < 1e-4f) manifold_add(m, p, a3_minf(-d, best + 0.05f));
        }
        if (!m->count) {
            /* edge/vertex contact: deepest box vertex along -n */
            A3Vec3 p = b->pos;
            for (int k = 0; k < 3; ++k) p = a3_v3_madd(p, Ax[k], a3_v3_dot(Ax[k], best_n) > 0 ? -e[k] : e[k]);
            manifold_add(m, p, best);
        }
        return 1;
    }
    return 0;
}

typedef struct MeshCollideCtx { const A3ShapeInstance *b; A3ContactPoint pts[32]; u32 np; A3Vec3 normal_sum; f32 best_depth; A3Vec3 best_normal; } MeshCollideCtx;

static b32 mesh_visit(const A3Vec3 *tri, u32 idx, void *user) {
    A3_UNUSED(idx);
    MeshCollideCtx *c = (MeshCollideCtx *)user;
    A3Manifold tm;
    if (!a3_collide_triangle(tri, c->b, &tm)) return 1;
    for (u32 i = 0; i < tm.count; ++i) {
        if (c->np < 32) c->pts[c->np++] = tm.pts[i];
        c->normal_sum = a3_v3_madd(c->normal_sum, tm.normal, tm.pts[i].depth);
        if (tm.pts[i].depth > c->best_depth) { c->best_depth = tm.pts[i].depth; c->best_normal = tm.normal; }
    }
    return 1;
}

b32 a3_collide_mesh(const A3TriMesh *mesh, const A3ShapeInstance *b, A3Manifold *m) {
    a3_zero_struct(m);
    MeshCollideCtx c;
    a3_zero_struct(&c);
    c.b = b;
    a3_trimesh_query_aabb(mesh, b->aabb, mesh_visit, &c);
    if (!c.np) return 0;
    A3Vec3 n = a3_v3_norm(c.normal_sum);
    if (a3_v3_len_sq(n) < 0.5f) n = c.best_normal;
    /* keep points consistent with the averaged normal (smooths internal edges) */
    for (u32 i = 0; i < c.np; ++i) {
        c.pts[i].depth = a3_maxf(c.pts[i].depth * a3_maxf(a3_v3_dot(n, c.best_normal), 0.2f), 0.0f);
    }
    reduce_points(c.pts, c.np, n, m);
    m->normal = n;
    return m->count > 0;
}

/* ======================================================================== */
/* Raycasts                                                                 */
/* ======================================================================== */

b32 a3_raycast_shape(const A3ShapeInstance *s, A3Vec3 o, A3Vec3 d, f32 max_t, f32 *t_out, A3Vec3 *n_out) {
    f32 t;
    if (s->type == A3_SHAPE_SPHERE) {
        A3Ray r = { o, d };
        if (!a3_ray_sphere(r, s->pos, s->radius, &t) || t > max_t) return 0;
        A3Vec3 p = a3_v3_madd(o, d, t);
        *t_out = t;
        *n_out = a3_v3_norm(a3_v3_sub(p, s->pos));
        return 1;
    }
    if (s->type == A3_SHAPE_BOX) {
        A3Vec3 lo_ = a3_v3_sub(o, s->pos);
        A3Vec3 lo = a3_v3(a3_v3_dot(lo_, box_axis(s, 0)), a3_v3_dot(lo_, box_axis(s, 1)), a3_v3_dot(lo_, box_axis(s, 2)));
        A3Vec3 ld = a3_v3(a3_v3_dot(d, box_axis(s, 0)), a3_v3_dot(d, box_axis(s, 1)), a3_v3_dot(d, box_axis(s, 2)));
        A3Ray r = { lo, ld };
        if (!a3_ray_aabb(r, a3_aabb(a3_v3_neg(s->half), s->half), &t) || t > max_t) return 0;
        A3Vec3 p = a3_v3_madd(lo, ld, t);
        /* normal: face with largest normalized coordinate */
        A3Vec3 q = a3_v3(p.x / s->half.x, p.y / s->half.y, p.z / s->half.z);
        A3Vec3 nl;
        if (a3_absf(q.x) >= a3_absf(q.y) && a3_absf(q.x) >= a3_absf(q.z)) nl = a3_v3(a3_signf(q.x), 0, 0);
        else if (a3_absf(q.y) >= a3_absf(q.z)) nl = a3_v3(0, a3_signf(q.y), 0);
        else nl = a3_v3(0, 0, a3_signf(q.z));
        *t_out = t;
        *n_out = a3_mat4_mul_dir(&s->rot_m, nl);
        return 1;
    }
    if (s->type == A3_SHAPE_CAPSULE) {
        /* conservative: march by closest distance (sphere tracing) */
        A3Vec3 p0, p1;
        a3_capsule_segment(s, &p0, &p1);
        f32 tt = 0;
        for (int i = 0; i < 64 && tt <= max_t; ++i) {
            A3Vec3 p = a3_v3_madd(o, d, tt), c;
            a3_closest_segment_segment(p, p, p0, p1, 0, 0, 0, &c);
            f32 dist = a3_v3_dist(p, c) - s->radius;
            if (dist < 1e-4f) {
                *t_out = tt;
                *n_out = a3_v3_norm(a3_v3_sub(p, c));
                return 1;
            }
            tt += dist;
        }
        return 0;
    }
    return 0;
}

/* ======================================================================== */
/* Triangle BVH                                                             */
/* ======================================================================== */

static A3Aabb tri_box(const A3Vec3 *v) { return a3_aabb_expand(a3_aabb_expand(a3_aabb(v[0], v[0]), v[1]), v[2]); }

static u32 bvh_build_rec(A3TriMesh *tm, u32 first, u32 count, u32 depth) {
    u32 idx = tm->node_count++;
    A3BvhNode *node = &tm->nodes[idx];
    A3Aabb box = a3_aabb_empty(), cbox = a3_aabb_empty();
    for (u32 i = 0; i < count; ++i) {
        const A3Vec3 *v = &tm->verts[tm->tri_order[first + i] * 3];
        box = a3_aabb_union(box, tri_box(v));
        cbox = a3_aabb_expand(cbox, a3_v3_scale(a3_v3_add(a3_v3_add(v[0], v[1]), v[2]), 1.0f / 3.0f));
    }
    node->box = box;
    if (count <= 4 || depth > 40) { node->first = first; node->count = count; return idx; }
    A3Vec3 ext = a3_v3_sub(cbox.max, cbox.min);
    int axis = ext.x > ext.y ? (ext.x > ext.z ? 0 : 2) : (ext.y > ext.z ? 1 : 2);
    f32 mid = v3c(a3_aabb_center(cbox), axis);
    u32 i = first, j = first + count;
    while (i < j) {
        const A3Vec3 *v = &tm->verts[tm->tri_order[i] * 3];
        f32 c = (v3c(v[0], axis) + v3c(v[1], axis) + v3c(v[2], axis)) / 3.0f;
        if (c < mid) ++i;
        else { u32 t = tm->tri_order[i]; tm->tri_order[i] = tm->tri_order[--j]; tm->tri_order[j] = t; }
    }
    u32 left = i - first;
    if (left == 0 || left == count) left = count / 2;
    node->count = 0;
    bvh_build_rec(tm, first, left, depth + 1);            /* left child = idx + 1 */
    u32 right = bvh_build_rec(tm, first + left, count - left, depth + 1);
    tm->nodes[idx].first = right;
    return idx;
}

b32 a3_trimesh_build(A3TriMesh *tm, const A3Vec3 *pos, const u32 *indices, u32 index_count, const A3Mat4 *world) {
    a3_zero_struct(tm);
    u32 tris = index_count / 3;
    if (!tris) return 0;
    tm->verts = A3_NEW_ARRAY(A3Vec3, tris * 3, A3_MEM_PHYSICS);
    tm->tri_order = A3_NEW_ARRAY(u32, tris, A3_MEM_PHYSICS);
    tm->nodes = A3_NEW_ARRAY(A3BvhNode, tris * 2 + 1, A3_MEM_PHYSICS);
    if (!tm->verts || !tm->tri_order || !tm->nodes) { a3_trimesh_free(tm); return 0; }
    for (u32 i = 0; i < tris * 3; ++i) tm->verts[i] = world ? a3_mat4_mul_point(world, pos[indices[i]]) : pos[indices[i]];
    for (u32 i = 0; i < tris; ++i) tm->tri_order[i] = i;
    tm->tri_count = tris;
    bvh_build_rec(tm, 0, tris, 0);
    tm->bounds = tm->nodes[0].box;
    return 1;
}

void a3_trimesh_free(A3TriMesh *tm) {
    a3_free(tm->verts);
    a3_free(tm->tri_order);
    a3_free(tm->nodes);
    a3_zero_struct(tm);
}

void a3_trimesh_query_aabb(const A3TriMesh *tm, A3Aabb box, A3TriVisitFn fn, void *user) {
    if (!tm->node_count) return;
    u32 stack[128];
    u32 sp = 0;
    stack[sp++] = 0;
    while (sp) {
        const A3BvhNode *n = &tm->nodes[stack[--sp]];
        if (!a3_aabb_overlap(n->box, box)) continue;
        if (n->count) {
            for (u32 i = 0; i < n->count; ++i) {
                u32 t = tm->tri_order[n->first + i];
                const A3Vec3 *v = &tm->verts[t * 3];
                if (a3_aabb_overlap(tri_box(v), box) && !fn(v, t, user)) return;
            }
        } else if (sp + 2 <= 128) {
            u32 self = (u32)(n - tm->nodes);
            stack[sp++] = n->first;
            stack[sp++] = self + 1;
        }
    }
}

b32 a3_trimesh_raycast(const A3TriMesh *tm, A3Vec3 o, A3Vec3 d, f32 max_t, f32 *t_out, A3Vec3 *n_out) {
    if (!tm->node_count) return 0;
    A3Ray ray = { o, d };
    f32 best = max_t;
    b32 hit = 0;
    u32 stack[128];
    u32 sp = 0;
    stack[sp++] = 0;
    while (sp) {
        const A3BvhNode *n = &tm->nodes[stack[--sp]];
        f32 tb;
        if (!a3_ray_aabb(ray, n->box, &tb) || tb > best) continue;
        if (n->count) {
            for (u32 i = 0; i < n->count; ++i) {
                u32 t = tm->tri_order[n->first + i];
                const A3Vec3 *v = &tm->verts[t * 3];
                f32 tt;
                if (a3_ray_triangle(ray, v[0], v[1], v[2], &tt) && tt < best) {
                    best = tt;
                    hit = 1;
                    A3Vec3 nn = a3_v3_norm(a3_v3_cross(a3_v3_sub(v[1], v[0]), a3_v3_sub(v[2], v[0])));
                    *n_out = a3_v3_dot(nn, d) > 0 ? a3_v3_neg(nn) : nn;
                }
            }
        } else if (sp + 2 <= 128) {
            u32 self = (u32)(n - tm->nodes);
            stack[sp++] = n->first;
            stack[sp++] = self + 1;
        }
    }
    if (hit) *t_out = best;
    return hit;
}
