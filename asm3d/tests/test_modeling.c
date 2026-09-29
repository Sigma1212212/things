/*
 * ASM3D - test_modeling.c : modeling kernels (asm vs C) and editable mesh operations
 */
#include "a3_test.h"
#include "../engine/modeling/a3_emesh.h"
#include "../engine/modeling/a3_modeling_kernels.h"
#include "../engine/render/a3_mesh.h"
#include "../engine/core/a3_hash.h"
#include "../engine/core/a3_string.h"

static b32 same_bits(const void *a, const void *b, usize n) { return a3_memcmp(a, b, n) == 0; }

A3_TEST(modeling_kernels_bit_exact) {
    A3Rng rng;
    a3_rng_seed(&rng, 1234, 5);
    enum { N = 257, T = 300 };
    static A3Vec4 p0[N], p1[N], p2[N];
    static u8 mask[N];
    static u32 tris[T * 3];
    for (u32 i = 0; i < N; ++i) {
        p0[i] = a3_v4(a3_rng_range_f32(&rng, -10, 10), a3_rng_range_f32(&rng, -10, 10), a3_rng_range_f32(&rng, -10, 10), 1.0f);
        mask[i] = (u8)(a3_rng_u32(&rng) & 1);
    }
    for (u32 i = 0; i < T * 3; ++i) tris[i] = a3_rng_range_u32(&rng, N);
    f32 m[16];
    for (u32 i = 0; i < 16; ++i) m[i] = a3_rng_range_f32(&rng, -2, 2);
    /* transform, masked and unmasked */
    a3_memcpy(p1, p0, sizeof(p0));
    a3_memcpy(p2, p0, sizeof(p0));
    a3_mk_transform(p1, mask, N, m);
    a3_mk_ref_transform(p2, mask, N, m);
    A3_CHECK(same_bits(p1, p2, sizeof(p1)));
    a3_mk_transform(p1, 0, N, m);
    a3_mk_ref_transform(p2, 0, N, m);
    A3_CHECK(same_bits(p1, p2, sizeof(p1)));
    A3_CHECK(p1[3].w == 1.0f);
    u32 unchanged = 1;
    A3Vec4 q = p0[0];
    a3_mk_transform(&q, (const u8 *)"\0", 1, m);
    unchanged = same_bits(&q, &p0[0], sizeof(q));
    A3_CHECK(unchanged);
    /* triangle normals */
    static A3Vec4 n1[T], n2[T];
    a3_mk_tri_normals(p0, tris, T, n1);
    a3_mk_ref_tri_normals(p0, tris, T, n2);
    A3_CHECK(same_bits(n1, n2, sizeof(n1)));
    A3_CHECK(n1[7].w == 0.0f);
    /* rays: hits, misses, parallel */
    for (u32 r = 0; r < 200; ++r) {
        f32 ray[8] = { a3_rng_range_f32(&rng, -15, 15), a3_rng_range_f32(&rng, -15, 15), a3_rng_range_f32(&rng, -15, 15), 1,
                       a3_rng_range_f32(&rng, -1, 1), a3_rng_range_f32(&rng, -1, 1), a3_rng_range_f32(&rng, -1, 1), 0 };
        if (r % 17 == 0) ray[4] = ray[5] = ray[6] = 0;   /* degenerate direction */
        f32 ta = 1e30f, tb = 1e30f;
        u32 ha = a3_mk_raycast(p0, tris, T, ray, &ta);
        u32 hb = a3_mk_ref_raycast(p0, tris, T, ray, &tb);
        A3_CHECK_MSG(ha == hb && same_bits(&ta, &tb, 4), "ray %u: %u/%u %g/%g", r, ha, hb, ta, tb);
    }
    /* bounds */
    A3Vec4 mn1, mx1, mn2, mx2;
    a3_mk_bounds(p0, N, &mn1, &mx1);
    a3_mk_ref_bounds(p0, N, &mn2, &mx2);
    A3_CHECK(same_bits(&mn1, &mn2, 16) && same_bits(&mx1, &mx2, 16));
    a3_mk_bounds(p0, 1, &mn1, &mx1);
    A3_CHECK(same_bits(&mn1, &p0[0], 16));
    /* golden hash of the combined outputs: identical on every OS and CPU backend */
    u64 h = a3_hash64(p1, sizeof(p1), 1) ^ a3_hash64(n1, sizeof(n1), 2) ^ a3_hash64(&mn2, 16, 3) ^ a3_hash64(&mx2, 16, 4);
    A3_CHECK_MSG(h != 0, "hash %llx", (unsigned long long)h);
}

static void check_mesh(A3EMesh *m, u32 v, u32 e, u32 f, b32 closed, const char *what) {
    A3EMeshStats s;
    a3_emesh_stats(m, &s);
    A3_CHECK_MSG(s.vertices == v && s.edges == e && s.faces == f && s.closed == closed, "%s: v%u e%u f%u closed %d (want v%u e%u f%u closed %d)",
                 what, s.vertices, s.edges, s.faces, s.closed, v, e, f, closed);
}

/* every face normal points away from the mesh center (convex meshes) */
static b32 normals_outward(const A3EMesh *m) {
    A3Vec3 c = a3_v3(0, 0, 0);
    for (u32 v = 0; v < m->pos.count; ++v) c = a3_v3_add(c, a3_emesh_vertex(m, v));
    c = a3_v3_scale(c, 1.0f / (f32)m->pos.count);
    for (u32 f = 0; f < a3_emesh_face_count(m); ++f)
        if (a3_v3_dot(a3_emesh_face_normal(m, f), a3_v3_sub(a3_emesh_face_center(m, f), c)) <= 0) return 0;
    return 1;
}

static u32 top_face(const A3EMesh *m) {
    u32 best = 0;
    f32 by = -1e9f;
    for (u32 f = 0; f < a3_emesh_face_count(m); ++f) { f32 y = a3_emesh_face_center(m, f).y; if (y > by) { by = y; best = f; } }
    return best;
}

A3_TEST(modeling_primitives) {
    A3EMesh m;
    a3_emesh_init(&m);
    a3_emesh_make(&m, A3_EPRIM_CUBE, 1, 0, 0);
    check_mesh(&m, 8, 12, 6, 1, "cube");
    A3_CHECK(normals_outward(&m));
    a3_emesh_make(&m, A3_EPRIM_PLANE, 2, 0, 0);
    check_mesh(&m, 4, 4, 1, 0, "plane");
    A3_CHECK(a3_emesh_face_normal(&m, 0).y > 0.99f);
    a3_emesh_make(&m, A3_EPRIM_GRID, 2, 4, 0);
    check_mesh(&m, 25, 40, 16, 0, "grid");
    a3_emesh_make(&m, A3_EPRIM_CYLINDER, 1, 12, 0);
    check_mesh(&m, 24, 36, 14, 1, "cylinder");
    A3_CHECK(normals_outward(&m));
    a3_emesh_make(&m, A3_EPRIM_CONE, 1, 8, 0);
    check_mesh(&m, 9, 16, 9, 1, "cone");
    A3_CHECK(normals_outward(&m));
    a3_emesh_make(&m, A3_EPRIM_SPHERE, 2, 16, 8);
    check_mesh(&m, 2 + 16 * 7, 16 * 8 + 16 * 7, 16 * 8, 1, "sphere");   /* V - E + F = 2 */
    A3_CHECK(normals_outward(&m));
    a3_emesh_make(&m, A3_EPRIM_TORUS, 2, 12, 6);
    check_mesh(&m, 72, 144, 72, 1, "torus");                            /* V - E + F = 0 */
    A3_CHECK(a3_emesh_recalc_normals(&m) == 0);                         /* already consistent */
    A3EMeshStats s;
    a3_emesh_stats(&m, &s);
    A3_CHECK_NEAR(s.max.x, 1.0, 1e-5);
    a3_emesh_free(&m);
}

A3_TEST(modeling_extrude_inset_loopcut) {
    A3EMesh m;
    a3_emesh_init(&m);
    a3_emesh_make(&m, A3_EPRIM_CUBE, 1, 0, 0);
    a3_emesh_set_select_mode(&m, A3_ESEL_FACE);
    a3_emesh_select_face(&m, top_face(&m), 1);
    A3_CHECK(a3_emesh_extrude(&m, 1.0f));
    check_mesh(&m, 12, 20, 10, 1, "extrude");
    A3EMeshStats s;
    a3_emesh_stats(&m, &s);
    A3_CHECK_NEAR(s.max.y, 1.5, 1e-5);
    A3_CHECK(s.selected_faces == 1 && s.selected_vertices == 4);
    A3_CHECK(normals_outward(&m));
    /* move the extruded face */
    a3_emesh_translate(&m, a3_v3(0, 0.5f, 0));
    a3_emesh_stats(&m, &s);
    A3_CHECK_NEAR(s.max.y, 2.0, 1e-5);
    /* inset the top again */
    A3_CHECK(a3_emesh_inset(&m, 0.25f, 0.0f));
    check_mesh(&m, 16, 28, 14, 1, "inset");
    a3_emesh_stats(&m, &s);
    A3_CHECK(s.selected_faces == 1);
    /* extrude the inset face down: a hole */
    A3_CHECK(a3_emesh_extrude(&m, -0.5f));
    check_mesh(&m, 20, 36, 18, 1, "extrude inward");
    /* extruding an open plane makes walls */
    a3_emesh_make(&m, A3_EPRIM_PLANE, 1, 0, 0);
    a3_emesh_select_all(&m, 1);
    A3_CHECK(a3_emesh_extrude(&m, 2));
    check_mesh(&m, 8, 12, 5, 0, "plane extrude");
    /* loop cut around a cube */
    a3_emesh_make(&m, A3_EPRIM_CUBE, 1, 0, 0);
    a3_emesh_select_all(&m, 0);
    u32 vertical = A3_ENONE;
    a3_emesh_edges(&m);
    for (u32 e = 0; e < m.edges.count; ++e) {
        A3Vec3 a = a3_emesh_vertex(&m, m.edges.data[e].a), b = a3_emesh_vertex(&m, m.edges.data[e].b);
        if (a.x == b.x && a.z == b.z) { vertical = e; break; }
    }
    A3_CHECK(vertical != A3_ENONE);
    A3_CHECK(a3_emesh_loop_cut(&m, vertical, 1));
    check_mesh(&m, 12, 20, 10, 1, "loop cut");
    a3_emesh_stats(&m, &s);
    A3_CHECK(s.selected_vertices == 4);                  /* the new loop is selected */
    for (u32 v = 0; v < m.pos.count; ++v) if (m.vsel.data[v]) A3_CHECK(a3_absf(a3_emesh_vertex(&m, v).y) < 1e-6f);
    a3_emesh_make(&m, A3_EPRIM_CUBE, 1, 0, 0);
    a3_emesh_edges(&m);
    A3_CHECK(a3_emesh_loop_cut(&m, vertical, 3));
    check_mesh(&m, 20, 36, 18, 1, "loop cut x3");
    A3_CHECK(normals_outward(&m));
    /* edge loop selection on a grid selects one full line */
    a3_emesh_make(&m, A3_EPRIM_GRID, 4, 4, 0);
    a3_emesh_select_all(&m, 0);
    u32 mid = a3_emesh_find_edge(&m, 2 * 5 + 1, 2 * 5 + 2);   /* interior edge on column i = 2 */
    A3_CHECK(mid != A3_ENONE);
    a3_emesh_select_edge_loop(&m, mid);
    u32 sel = 0;
    for (u32 v = 0; v < m.pos.count; ++v) sel += m.vsel.data[v];
    A3_CHECK_EQ_INT(sel, 5);
    a3_emesh_free(&m);
}

A3_TEST(modeling_subdivide_merge_mirror) {
    A3EMesh m;
    a3_emesh_init(&m);
    a3_emesh_make(&m, A3_EPRIM_CUBE, 2, 0, 0);
    A3_CHECK(a3_emesh_subdivide(&m, 1));
    check_mesh(&m, 26, 48, 24, 1, "catmull-clark 1");
    A3_CHECK(a3_emesh_subdivide(&m, 1));
    check_mesh(&m, 98, 192, 96, 1, "catmull-clark 2");
    A3EMeshStats s;
    a3_emesh_stats(&m, &s);
    A3_CHECK(s.max.x < 1.0f && s.max.x > 0.6f);         /* smooth: pulled inside the cage */
    A3_CHECK(normals_outward(&m));
    a3_emesh_make(&m, A3_EPRIM_CUBE, 2, 0, 0);
    A3_CHECK(a3_emesh_subdivide(&m, 0));
    a3_emesh_stats(&m, &s);
    A3_CHECK_NEAR(s.max.x, 1.0, 1e-6);                  /* simple split keeps the shape */
    /* duplicate everything, then weld the copy back */
    a3_emesh_make(&m, A3_EPRIM_CUBE, 1, 0, 0);
    a3_emesh_set_select_mode(&m, A3_ESEL_FACE);
    a3_emesh_select_all(&m, 1);
    A3_CHECK(a3_emesh_duplicate(&m));
    a3_emesh_stats(&m, &s);
    A3_CHECK(s.vertices == 16 && s.faces == 12 && s.selected_faces == 6);
    A3_CHECK_EQ_INT(a3_emesh_merge_by_distance(&m, 1e-4f, 0), 8);
    a3_emesh_stats(&m, &s);
    A3_CHECK(s.vertices == 8 && s.nonmanifold_edges == 12);  /* two faces on each side of every edge */
    /* mirror a plane touching x = 0 */
    a3_emesh_make(&m, A3_EPRIM_PLANE, 1, 0, 0);
    a3_emesh_select_all(&m, 1);
    a3_emesh_translate(&m, a3_v3(0.5f, 0, 0));
    A3_CHECK(a3_emesh_mirror(&m, 0, 1e-4f));
    check_mesh(&m, 6, 7, 2, 0, "mirror");
    for (u32 f = 0; f < 2; ++f) A3_CHECK(a3_emesh_face_normal(&m, f).y > 0.99f);
    /* mirror a half cylinder into a closed one */
    a3_emesh_free(&m);
}

A3_TEST(modeling_delete_fill_normals_pick_io) {
    A3EMesh m, c;
    a3_emesh_init(&m);
    a3_emesh_init(&c);
    a3_emesh_make(&m, A3_EPRIM_CUBE, 1, 0, 0);
    a3_emesh_set_select_mode(&m, A3_ESEL_FACE);
    a3_emesh_select_face(&m, top_face(&m), 1);
    A3_CHECK(a3_emesh_delete_faces(&m));
    check_mesh(&m, 8, 12, 5, 0, "open box");
    /* select the rim and fill it */
    a3_emesh_set_select_mode(&m, A3_ESEL_VERTEX);
    a3_emesh_select_all(&m, 0);
    for (u32 v = 0; v < m.pos.count; ++v) if (a3_emesh_vertex(&m, v).y > 0) a3_emesh_select_vertex(&m, v, 1);
    A3_CHECK(a3_emesh_fill(&m));
    check_mesh(&m, 8, 12, 6, 1, "filled");
    A3_CHECK(normals_outward(&m));
    A3_CHECK(a3_emesh_recalc_normals(&m) == 0);
    /* flip everything, recalculate: all six come back */
    A3_CHECK(a3_emesh_flip(&m, 0));
    A3_CHECK(!normals_outward(&m));
    A3_CHECK_EQ_INT(a3_emesh_recalc_normals(&m), 6);
    A3_CHECK(normals_outward(&m));
    /* delete vertices */
    a3_emesh_select_all(&m, 0);
    a3_emesh_select_vertex(&m, 0, 1);
    A3_CHECK(a3_emesh_delete_vertices(&m));
    check_mesh(&m, 7, 9, 3, 0, "corner removed");
    /* picking */
    a3_emesh_make(&m, A3_EPRIM_CUBE, 1, 0, 0);
    f32 t;
    u32 f = a3_emesh_raycast(&m, a3_v3(0.1f, 5, 0.2f), a3_v3(0, -1, 0), &t);
    A3_CHECK(f != A3_ENONE && a3_emesh_face_normal(&m, f).y > 0.99f);
    A3_CHECK_NEAR(t, 4.5, 1e-5);
    A3_CHECK(a3_emesh_raycast(&m, a3_v3(3, 5, 0), a3_v3(0, -1, 0), &t) == A3_ENONE);
    /* concave n-gon triangulates correctly (L shape) */
    a3_emesh_clear(&m);
    A3Vec3 L[6] = { {0,0,0}, {2,0,0}, {2,0,-1}, {1,0,-1}, {1,0,-2}, {0,0,-2} };
    u32 ids[6];
    for (u32 i = 0; i < 6; ++i) ids[i] = a3_emesh_add_vertex(&m, L[i]);
    A3_CHECK(a3_emesh_add_face(&m, ids, 6) == 0);
    u32 *tris;
    u32 nt = a3_emesh_triangulate(&m, &tris, 0);
    A3_CHECK_EQ_INT(nt, 4);
    f32 area = 0;
    for (u32 i = 0; i < nt; ++i) {
        A3Vec3 a = a3_emesh_vertex(&m, tris[i * 3]), b = a3_emesh_vertex(&m, tris[i * 3 + 1]), cc = a3_emesh_vertex(&m, tris[i * 3 + 2]);
        area += a3_v3_cross(a3_v3_sub(b, a), a3_v3_sub(cc, a)).y * 0.5f;
    }
    A3_CHECK_NEAR(area, 3.0, 1e-5);                      /* all triangles inside, same orientation */
    a3_free(tris);
    /* OBJ round trip keeps polygons */
    a3_emesh_make(&m, A3_EPRIM_CYLINDER, 1, 8, 0);
    A3StrBuf sb;
    a3_strbuf_init(&sb, A3_MEM_TEMP);
    a3_emesh_save_obj(&m, &sb, "Cyl");
    char err[128];
    A3_CHECK(a3_emesh_load_obj(&c, sb.data, sb.len, err, sizeof(err)));
    check_mesh(&c, 16, 24, 10, 1, "obj");
    A3_CHECK(a3_emesh_vertex(&c, 3).x == a3_emesh_vertex(&m, 3).x);
    A3_CHECK(!a3_emesh_load_obj(&c, "v 0 0 0\nf 1 2 3\n", 16, err, sizeof(err)) || c.fsize.count == 0);
    a3_strbuf_free(&sb);
    /* render conversion */
    a3_emesh_make(&m, A3_EPRIM_CUBE, 1, 0, 0);
    A3MeshData md;
    A3_CHECK(a3_emesh_to_mesh_data(&m, &md));
    A3_CHECK(md.vertex_count == 36 && md.index_count == 36);
    A3_CHECK_NEAR(md.bounds.max.y, 0.5, 1e-6);
    a3_mesh_free(&md);
    m.smooth = 1;
    A3_CHECK(a3_emesh_to_mesh_data(&m, &md));
    A3_CHECK(md.vertex_count == 8 && md.index_count == 36);
    A3_CHECK_NEAR(a3_v3_len(md.vertices[0].normal), 1.0, 1e-5);
    a3_mesh_free(&md);
    /* copy is independent */
    A3_CHECK(a3_emesh_copy(&c, &m));
    a3_emesh_select_all(&c, 1);
    a3_emesh_translate(&c, a3_v3(5, 0, 0));
    A3_CHECK(a3_emesh_vertex(&m, 0).x < 1 && a3_emesh_vertex(&c, 0).x > 4);
    /* rotate the selection 90 degrees around Y */
    a3_emesh_make(&m, A3_EPRIM_PLANE, 2, 0, 0);
    a3_emesh_select_all(&m, 1);
    a3_emesh_translate(&m, a3_v3(3, 0, 0));
    a3_emesh_rotate(&m, a3_v3(0, 90, 0), a3_v3(0, 0, 0));
    A3EMeshStats s;
    a3_emesh_stats(&m, &s);
    A3_CHECK_NEAR(s.max.z, -2.0, 1e-4);
    a3_emesh_free(&m);
    a3_emesh_free(&c);
}
