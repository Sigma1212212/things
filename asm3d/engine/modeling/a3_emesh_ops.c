/*
 * ASM3D - a3_emesh_ops.c
 * Text form of the modeling operations ("extrude:1", "select-normal:0,1,0"),
 * shared by asm3d_cli, the editor's Modeling panel and tests.
 */
#include "a3_emesh_ops.h"
#include "../core/a3_string.h"
#include "../core/a3_format.h"
#include "../script/a3_script.h"

static f64 a3_atof(const char *s) { f64 d = 0; a3_parse_f64(s, a3_strlen(s), &d); return d; }
static i64 a3_atoi(const char *s) { i64 v = 0; b32 neg = *s == '-'; if (neg) ++s; while (a3_is_digit(*s)) v = v * 10 + (*s++ - '0'); return neg ? -v : v; }

static b32 op_fail(A3EMeshOpError *e, const char *hint, const char *fmt, ...) A3_PRINTF_LIKE(3, 4);
static b32 op_fail(A3EMeshOpError *e, const char *hint, const char *fmt, ...) {
    if (!e) return 0;
    va_list ap;
    va_start(ap, fmt);
    a3_vsnprintf(e->message, sizeof(e->message), fmt, ap);
    va_end(ap);
    a3_strcpy(e->hint, sizeof(e->hint), hint ? hint : "");
    return 0;
}

/* numbers after "name:" separated by commas */
static u32 op_numbers(const char *argstr, f32 *out, u32 cap) {
    u32 n = 0;
    A3Str rest = a3_str(argstr ? argstr : "");
    while (rest.len && n < cap) {
        A3Str t = a3_str_trim(a3_str_split_next(&rest, ','));
        f64 d;
        if (t.len && a3_parse_f64(t.ptr, t.len, &d)) out[n++] = (f32)d;
    }
    return n;
}

static const A3EMeshOpInfo g_ops[] = {
    { "mode", "vertex|edge|face", "Selection mode (face mode: operations use the selected faces)." },
    { "select-all", "", "Select everything." },
    { "select-none", "", "Clear the selection." },
    { "select-invert", "", "Invert the selection." },
    { "select-normal", "x,y,z[,min_dot]", "Select faces whose normal points along x,y,z (default min_dot 0.9), e.g. select-normal:0,1,0 for the top." },
    { "select-box", "minx,miny,minz,maxx,maxy,maxz", "Select vertices inside a box." },
    { "select-linked", "", "Grow the selection to whole connected parts." },
    { "select-more", "", "Grow the selection by one ring." },
    { "translate", "x,y,z", "Move the selection." },
    { "rotate", "x,y,z", "Rotate the selection (degrees) around its center." },
    { "scale", "x,y,z | s", "Scale the selection around its center." },
    { "extrude", "distance", "Extrude the selected faces as one region along their normals." },
    { "inset", "amount[,depth]", "Inset each selected face (amount 0..1 toward its center)." },
    { "loopcut", "x,y,z[,cuts]", "Loop cut through the quad ring of the edge nearest to point x,y,z." },
    { "subdivide", "[smooth|simple][,levels]", "Catmull-Clark (smooth, default) or simple subdivision of the whole mesh." },
    { "delete-faces", "", "Delete the selected faces." },
    { "delete-verts", "", "Delete the selected vertices and their faces." },
    { "merge", "distance", "Merge vertices closer than distance (selected only when something is selected)." },
    { "fill", "", "Make a face from the selected boundary loop (or selected vertices)." },
    { "duplicate", "", "Duplicate the selected faces (the copy becomes selected)." },
    { "flip", "", "Reverse the selected faces (all when nothing is selected)." },
    { "recalc-normals", "", "Make all faces point outward consistently." },
    { "mirror", "x|y|z[,merge_distance]", "Mirror the whole mesh across a plane through the origin." },
    { "add", "primitive[,size,x,y,z]", "Add a primitive (cube, plane, grid, cylinder, cone, sphere, torus) at x,y,z." },
    { "shading", "smooth|flat", "Shading used when the model is rendered." },
};

b32 a3_emesh_run_op(A3EMesh *m, const char *spec, A3EMeshOpError *e) {
    if (e) a3_zero_struct(e);
    char name[48];
    const char *colon = a3_strchr(spec, ':');
    usize nl = colon ? (usize)(colon - spec) : a3_strlen(spec);
    if (nl >= sizeof(name)) nl = sizeof(name) - 1;
    a3_memcpy(name, spec, nl);
    name[nl] = 0;
    const char *args = colon ? colon + 1 : "";
    f32 v[8] = { 0 };
    u32 nv = op_numbers(args, v, 8);
    A3Vec3 pivot = a3_emesh_selection_center(m);
    if (a3_streq(name, "mode")) {
        if (a3_streq(args, "vertex")) a3_emesh_set_select_mode(m, A3_ESEL_VERTEX);
        else if (a3_streq(args, "edge")) a3_emesh_set_select_mode(m, A3_ESEL_EDGE);
        else if (a3_streq(args, "face")) a3_emesh_set_select_mode(m, A3_ESEL_FACE);
        else return op_fail(e, "Use mode:vertex, mode:edge or mode:face.", "unknown mode '%s'", args);
    } else if (a3_streq(name, "select-all")) a3_emesh_select_all(m, 1);
    else if (a3_streq(name, "select-none")) a3_emesh_select_all(m, 0);
    else if (a3_streq(name, "select-invert")) a3_emesh_select_invert(m);
    else if (a3_streq(name, "select-linked")) a3_emesh_select_linked(m);
    else if (a3_streq(name, "select-more")) a3_emesh_select_more(m);
    else if (a3_streq(name, "select-normal")) {
        if (nv < 3) return op_fail(e, "Example: select-normal:0,1,0", "select-normal needs a direction");
        A3Vec3 d = a3_v3_norm(a3_v3(v[0], v[1], v[2]));
        f32 th = nv > 3 ? v[3] : 0.9f;
        a3_emesh_set_select_mode(m, A3_ESEL_FACE);
        for (u32 f = 0; f < a3_emesh_face_count(m); ++f) if (a3_v3_dot(a3_emesh_face_normal(m, f), d) >= th) m->fsel.data[f] = 1;
        a3_emesh_sync_selection(m);
    } else if (a3_streq(name, "select-box")) {
        if (nv < 6) return op_fail(e, "Example: select-box:-1,0.4,-1,1,1,1", "select-box needs 6 numbers");
        if (m->select_mode == A3_ESEL_FACE) a3_emesh_set_select_mode(m, A3_ESEL_VERTEX);
        for (u32 i = 0; i < m->pos.count; ++i) {
            A3Vec3 p = a3_emesh_vertex(m, i);
            if (p.x >= v[0] && p.y >= v[1] && p.z >= v[2] && p.x <= v[3] && p.y <= v[4] && p.z <= v[5]) m->vsel.data[i] = 1;
        }
        a3_emesh_sync_selection(m);
    } else if (a3_streq(name, "translate")) {
        if (nv < 3) return op_fail(e, "Example: translate:0,1,0", "translate needs x,y,z");
        a3_emesh_translate(m, a3_v3(v[0], v[1], v[2]));
    } else if (a3_streq(name, "rotate")) {
        if (nv < 3) return op_fail(e, "Example: rotate:0,45,0", "rotate needs x,y,z degrees");
        a3_emesh_rotate(m, a3_v3(v[0], v[1], v[2]), pivot);
    } else if (a3_streq(name, "scale")) {
        if (nv == 1) v[1] = v[2] = v[0];
        else if (nv < 3) return op_fail(e, "Example: scale:2 or scale:1,2,1", "scale needs a factor");
        a3_emesh_scale(m, a3_v3(v[0], v[1], v[2]), pivot);
    } else if (a3_streq(name, "extrude")) {
        if (!a3_emesh_extrude(m, nv ? v[0] : 1.0f)) return op_fail(e, "Select faces first, e.g. --op select-normal:0,1,0", "extrude: no faces selected");
    } else if (a3_streq(name, "inset")) {
        if (!a3_emesh_inset(m, nv ? v[0] : 0.2f, nv > 1 ? v[1] : 0.0f)) return op_fail(e, "Select faces first.", "inset: no faces selected");
    } else if (a3_streq(name, "loopcut")) {
        if (nv < 3) return op_fail(e, "Example: loopcut:0.5,0,0.5,2 (point near an edge, number of cuts)", "loopcut needs a point");
        a3_emesh_edges(m);
        A3Vec3 p = a3_v3(v[0], v[1], v[2]);
        u32 best = A3_ENONE;
        f32 bd = 1e30f;
        for (u32 ei = 0; ei < m->edges.count; ++ei) {
            A3Vec3 mid = a3_v3_scale(a3_v3_add(a3_emesh_vertex(m, m->edges.data[ei].a), a3_emesh_vertex(m, m->edges.data[ei].b)), 0.5f);
            f32 d = a3_v3_len_sq(a3_v3_sub(mid, p));
            if (d < bd) { bd = d; best = ei; }
        }
        if (!a3_emesh_loop_cut(m, best, nv > 3 ? (u32)v[3] : 1)) return op_fail(e, "Loop cuts need quads around the edge.", "loopcut: no quad ring at that edge");
    } else if (a3_streq(name, "subdivide")) {
        b32 smooth = !a3_str_starts_with(args, "simple");
        u32 levels = 1;
        const char *comma = a3_strchr(args, ',');
        if (comma) levels = (u32)a3_clampi((i32)a3_atoi(comma + 1), 1, 4);
        else if (a3_is_digit(args[0])) levels = (u32)a3_clampi((i32)a3_atoi(args), 1, 4);
        for (u32 i = 0; i < levels; ++i) a3_emesh_subdivide(m, smooth);
    } else if (a3_streq(name, "delete-faces")) {
        if (!a3_emesh_delete_faces(m)) return op_fail(e, "Select faces first.", "delete-faces: nothing selected");
    } else if (a3_streq(name, "delete-verts")) {
        if (!a3_emesh_delete_vertices(m)) return op_fail(e, "Select vertices first.", "delete-verts: nothing selected");
    } else if (a3_streq(name, "merge")) {
        u32 merged = a3_emesh_merge_by_distance(m, nv ? v[0] : 1e-4f, a3_emesh_selected_count(m) > 0);
        if (e) e->merged = merged;
    } else if (a3_streq(name, "fill")) {
        if (!a3_emesh_fill(m)) return op_fail(e, "Select an open boundary loop (or 3+ vertices).", "fill: nothing to fill");
    } else if (a3_streq(name, "duplicate")) {
        if (!a3_emesh_duplicate(m)) return op_fail(e, "Select faces first.", "duplicate: no faces selected");
    } else if (a3_streq(name, "flip")) a3_emesh_flip(m, a3_emesh_selected_count(m) > 0);
    else if (a3_streq(name, "recalc-normals")) a3_emesh_recalc_normals(m);
    else if (a3_streq(name, "mirror")) {
        u32 axis = args[0] == 'y' ? 1 : args[0] == 'z' ? 2 : 0;
        f32 md = 1e-4f;
        const char *comma = a3_strchr(args, ',');
        if (comma) md = (f32)a3_atof(comma + 1);
        a3_emesh_mirror(m, axis, md);
    } else if (a3_streq(name, "add")) {
        char pn[32];
        const char *comma = a3_strchr(args, ',');
        usize pl = comma ? (usize)(comma - args) : a3_strlen(args);
        if (pl >= sizeof(pn)) pl = sizeof(pn) - 1;
        a3_memcpy(pn, args, pl);
        pn[pl] = 0;
        i32 pi = a3_emesh_primitive_from_name(pn);
        if (pi < 0) return op_fail(e, "Primitives: cube, plane, grid, cylinder, cone, sphere, torus.", "unknown primitive '%s'", pn);
        f32 nums[4] = { 1, 0, 0, 0 };
        u32 k = comma ? op_numbers(comma + 1, nums, 4) : 0;
        a3_emesh_add_primitive(m, (A3EPrimitive)pi, k ? nums[0] : 1, 0, 0, a3_v3(nums[1], nums[2], nums[3]));
    } else if (a3_streq(name, "shading")) m->smooth = a3_streq(args, "smooth");
    else {
        const char *cands[64];
        u32 n = 0;
        for (u32 i = 0; i < A3_ARRAY_COUNT(g_ops); ++i) cands[n++] = g_ops[i].name;
        char hint[160];
        a3s_suggest(name, cands, n, hint, sizeof(hint));
        return op_fail(e, hint[0] ? hint : "Run 'mesh ops' for the list.", "unknown operation '%s'", name);
    }
    return 1;
}


u32 a3_emesh_op_count(void) { return A3_ARRAY_COUNT(g_ops); }
const A3EMeshOpInfo *a3_emesh_op_info(u32 i) { return i < A3_ARRAY_COUNT(g_ops) ? &g_ops[i] : 0; }
