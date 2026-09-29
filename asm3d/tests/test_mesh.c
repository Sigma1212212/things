/*
 * ASM3D - test_mesh.c : procedural primitives + OBJ importer (CPU only)
 */
#include "a3_test.h"
#include "../engine/render/a3_mesh.h"
#include "../engine/core/a3_string.h"

/* Every triangle must wind counter-clockwise when seen from outside, i.e. its
 * geometric normal must agree with the vertex normals; otherwise back-face
 * culling would make faces disappear. */
static int count_bad_winding(const A3MeshData *m) {
    int bad = 0;
    for (u32 i = 0; i + 2 < m->index_count; i += 3) {
        const A3Vertex *a = &m->vertices[m->indices[i]], *b = &m->vertices[m->indices[i + 1]], *c = &m->vertices[m->indices[i + 2]];
        A3Vec3 n = a3_v3_cross(a3_v3_sub(b->position, a->position), a3_v3_sub(c->position, a->position));
        if (a3_v3_len_sq(n) < 1e-12f) continue; /* degenerate at poles */
        A3Vec3 vn = a3_v3_add(a3_v3_add(a->normal, b->normal), c->normal);
        if (a3_v3_dot(n, vn) <= 0) bad++;
    }
    return bad;
}

A3_TEST(mesh_primitives) {
    A3MeshData m;
    A3_CHECK(a3_mesh_cube(&m));
    A3_CHECK_EQ_INT(m.index_count, 36);
    A3_CHECK_EQ_INT(count_bad_winding(&m), 0);
    A3_CHECK(a3_v3_nearly_equal(m.bounds.max, a3_v3s(0.5f), 1e-6f));
    a3_mesh_free(&m);
    A3_CHECK(a3_mesh_sphere(&m, 24, 12));
    A3_CHECK_EQ_INT(count_bad_winding(&m), 0);
    A3_CHECK_NEAR(m.radius, 0.5, 1e-5);
    a3_mesh_free(&m);
    A3_CHECK(a3_mesh_plane(&m, 4, 1));
    A3_CHECK_EQ_INT(count_bad_winding(&m), 0);
    A3_CHECK_EQ_INT(m.index_count, 4 * 4 * 6);
    a3_mesh_free(&m);
    A3_CHECK(a3_mesh_cylinder(&m, 16));
    A3_CHECK_EQ_INT(count_bad_winding(&m), 0);
    a3_mesh_free(&m);
    A3_CHECK(a3_mesh_cone(&m, 16));
    A3_CHECK_EQ_INT(count_bad_winding(&m), 0);
    a3_mesh_free(&m);
    A3_CHECK(a3_mesh_capsule(&m, 16, 8));
    A3_CHECK_EQ_INT(count_bad_winding(&m), 0);
    A3_CHECK_NEAR(m.bounds.max.y, 1.0, 1e-5);
    A3_CHECK_NEAR(m.bounds.min.y, -1.0, 1e-5);
    a3_mesh_free(&m);
    f32 h[9] = { 0, 0, 0, 0, 1, 0, 0, 0, 0 };
    A3_CHECK(a3_mesh_heightfield(&m, h, 2, 2.0f, 1.0f));
    A3_CHECK_EQ_INT(count_bad_winding(&m), 0);
    A3_CHECK_NEAR(m.bounds.max.y, 1.0, 0);
    a3_mesh_free(&m);
}

A3_TEST(mesh_obj) {
    const char *obj =
        "# quad\n"
        "v -1 0 -1\nv 1 0 -1\nv 1 0 1\nv -1 0 1\n"
        "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
        "vn 0 1 0\n"
        "f 1/1/1 4/4/1 3/3/1 2/2/1\n";
    A3MeshData m;
    char err[128] = "";
    A3_CHECK_MSG(a3_mesh_load_obj(obj, a3_strlen(obj), &m, err, sizeof(err)) == A3_OK, "%s", err);
    A3_CHECK_EQ_INT(m.vertex_count, 4);
    A3_CHECK_EQ_INT(m.index_count, 6);
    A3_CHECK_EQ_INT(count_bad_winding(&m), 0);
    a3_mesh_free(&m);
    const char *nonormals = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    A3_CHECK(a3_mesh_load_obj(nonormals, a3_strlen(nonormals), &m, err, sizeof(err)) == A3_OK);
    A3_CHECK(a3_v3_nearly_equal(m.vertices[0].normal, a3_v3(0, 0, 1), 1e-6f));
    a3_mesh_free(&m);
    const char *bad = "v 0 0 0\nv 1 0 0\nf 1 2 7\n";
    A3_CHECK(a3_mesh_load_obj(bad, a3_strlen(bad), &m, err, sizeof(err)) == A3_ERR_PARSE);
    A3_CHECK(a3_strstr(err, "line 3") != 0);
}
