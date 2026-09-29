/*
 * ASM3D Editor - 3D viewport: camera, picking, gizmos, overlays, drag & drop.
 */
#include "editor.h"
#include "../../engine/physics/a3_physics.h"
#include "../../engine/resource/a3_assets.h"
#include "../../engine/render/a3_mesh.h"
#include "../../engine/core/a3_log.h"
#include "../../engine/core/a3_string.h"
#include "../../engine/core/a3_format.h"

#define GIZMO_PX 90.0f

/* ======================================================================== */
/* Camera                                                                   */
/* ======================================================================== */

static A3Quat cam_rot(A3Editor *ed) { return a3_quat_euler(ed->cam_pitch, ed->cam_yaw, 0); }

A3Mat4 ed_camera_view(A3Editor *ed) {
    A3Mat4 m = a3_mat4_trs(ed->cam_pos, cam_rot(ed), a3_v3_one());
    return a3_mat4_inverse_affine(&m);
}

static A3RenderView make_view(A3Editor *ed, b32 *is_game) {
    A3RenderView v;
    A3World *w = ed_active_world(ed);
    *is_game = 0;
    if (ed->mode != ED_EDIT && ed->game_view && w) {
        a3_transform_system_update(w);
        A3Entity cam = a3_find_primary_camera(w);
        if (a3_render_view_from_camera(w, cam, ed->vp_w, ed->vp_h, &v)) { *is_game = 1; v.target = ed->vp_target; return v; }
    }
    a3_zero_struct(&v);
    v.view = ed_camera_view(ed);
    v.proj = a3_mat4_perspective(60 * A3_DEG2RAD, (f32)ed->vp_w / (f32)(ed->vp_h ? ed->vp_h : 1), 0.05f, 2000.0f, a3_rhi_depth_zero_to_one());
    v.camera_pos = ed->cam_pos;
    v.near_plane = 0.05f;
    v.far_plane = 2000.0f;
    v.width = ed->vp_w;
    v.height = ed->vp_h;
    v.target = ed->vp_target;
    v.draw_sky = 1;
    v.clear_color = a3_v4(0.12f, 0.13f, 0.15f, 1);
    v.exposure = 1.0f;
    v.draw_grid = ed->show_grid;
    v.draw_debug = 1;
    return v;
}

static A3Mat4 current_view_proj(A3Editor *ed) {
    b32 g;
    A3RenderView v = make_view(ed, &g);
    return a3_mat4_mul(&v.proj, &v.view);
}

static b32 world_to_screen(A3Editor *ed, const A3Mat4 *vp, A3Vec3 p, A3Vec2 *out) {
    A3Vec4 c = a3_mat4_mul_v4(vp, a3_v4_from3(p, 1));
    if (c.w <= 1e-4f) return 0;
    f32 x = c.x / c.w, y = c.y / c.w;
    *out = a3_v2(ed->vp_rect.x + (x * 0.5f + 0.5f) * ed->vp_rect.w, ed->vp_rect.y + (0.5f - y * 0.5f) * ed->vp_rect.h);
    return 1;
}

static A3Ray screen_ray(A3Editor *ed, const A3Mat4 *vp, A3Vec2 m) {
    A3Mat4 inv;
    A3Ray r = { ed->cam_pos, a3_v3(0, 0, -1) };
    if (!a3_mat4_inverse(vp, &inv)) return r;
    f32 x = (m.x - ed->vp_rect.x) / ed->vp_rect.w * 2 - 1, y = 1 - (m.y - ed->vp_rect.y) / ed->vp_rect.h * 2;
    A3Vec3 n = a3_mat4_mul_point_project(&inv, a3_v3(x, y, a3_rhi_depth_zero_to_one() ? 0.0f : -1.0f));
    A3Vec3 f = a3_mat4_mul_point_project(&inv, a3_v3(x, y, 1.0f));
    r.origin = n;
    r.dir = a3_v3_norm(a3_v3_sub(f, n));
    return r;
}

static void camera_controls(A3Editor *ed, A3Ui *ui, f32 dt) {
    const A3InputState *in = a3_ui_input(ui);
    b32 over = ed->vp_hovered;
    if (in->mouse_pressed[A3_MOUSE_RIGHT] && over) ed->cam_flying = 1;
    if (!in->mouse[A3_MOUSE_RIGHT]) ed->cam_flying = 0;
    A3Quat rot = cam_rot(ed);
    A3Vec3 fwd = a3_quat_rotate(rot, a3_v3(0, 0, -1)), right = a3_quat_rotate(rot, a3_v3(1, 0, 0)), up = a3_v3(0, 1, 0);
    if (ed->cam_flying) {
        ed->cam_yaw -= in->mouse_delta.x * 0.0035f;
        ed->cam_pitch = a3_clampf(ed->cam_pitch - in->mouse_delta.y * 0.0035f, -1.55f, 1.55f);
        f32 speed = ed->cam_speed * ((in->mods & A3_MOD_SHIFT) ? 3.0f : 1.0f);
        A3Vec3 mv = a3_v3_zero();
        if (in->keys[A3_KEY_W]) mv = a3_v3_add(mv, fwd);
        if (in->keys[A3_KEY_S]) mv = a3_v3_sub(mv, fwd);
        if (in->keys[A3_KEY_D]) mv = a3_v3_add(mv, right);
        if (in->keys[A3_KEY_A]) mv = a3_v3_sub(mv, right);
        if (in->keys[A3_KEY_E]) mv = a3_v3_add(mv, up);
        if (in->keys[A3_KEY_Q]) mv = a3_v3_sub(mv, up);
        ed->cam_pos = a3_v3_madd(ed->cam_pos, mv, speed * dt);
        if (in->scroll.y != 0) ed->cam_speed = a3_clampf(ed->cam_speed * (in->scroll.y > 0 ? 1.25f : 0.8f), 0.5f, 200.0f);
    } else if (over) {
        if (in->scroll.y != 0) ed->cam_pos = a3_v3_madd(ed->cam_pos, fwd, in->scroll.y * ed->cam_speed * 0.25f);
        if (in->mouse[A3_MOUSE_MIDDLE]) {
            f32 k = ed->cam_speed * 0.004f;
            ed->cam_pos = a3_v3_madd(ed->cam_pos, right, -in->mouse_delta.x * k);
            ed->cam_pos = a3_v3_madd(ed->cam_pos, a3_quat_rotate(rot, a3_v3(0, 1, 0)), in->mouse_delta.y * k);
        }
        if ((in->mods & A3_MOD_ALT) && in->mouse[A3_MOUSE_LEFT]) {
            /* orbit around the selection (or a point ahead) */
            A3World *w = ed_active_world(ed);
            A3Entity sel = ed_selected(ed);
            A3Vec3 pivot = a3_entity_valid(w, sel) ? a3_transform_world_position(w, sel) : a3_v3_madd(ed->cam_pos, fwd, 10);
            f32 dist = a3_v3_dist(pivot, ed->cam_pos);
            ed->cam_yaw -= in->mouse_delta.x * 0.005f;
            ed->cam_pitch = a3_clampf(ed->cam_pitch - in->mouse_delta.y * 0.005f, -1.55f, 1.55f);
            A3Vec3 nf = a3_quat_rotate(cam_rot(ed), a3_v3(0, 0, -1));
            ed->cam_pos = a3_v3_madd(pivot, nf, -dist);
        }
    }
}

void ed_focus_selected(A3Editor *ed) {
    A3World *w = ed_active_world(ed);
    A3Entity e = ed_selected(ed);
    if (!a3_entity_valid(w, e)) return;
    A3Vec3 p = a3_transform_world_position(w, e);
    f32 radius = 1.5f;
    A3CMeshRenderer *mr = (A3CMeshRenderer *)a3_component_get(w, e, A3_T_MESH_RENDERER);
    if (mr) {
        const A3MeshAsset *m = a3_assets_mesh_get(a3_assets_mesh_for_renderer(mr));
        A3CTransform *t = a3_transform(w, e);
        if (m && t) radius = a3_maxf(m->radius * a3_v3_max_comp(a3_v3_abs(t->scale)), 0.5f);
    }
    A3Vec3 fwd = a3_quat_rotate(cam_rot(ed), a3_v3(0, 0, -1));
    ed->cam_pos = a3_v3_madd(p, fwd, -radius * 4.5f);
}

/* ======================================================================== */
/* Picking                                                                  */
/* ======================================================================== */

static A3Entity pick(A3Editor *ed, A3Ray ray, f32 *out_t) {
    A3World *w = ed_active_world(ed);
    A3Entity best = A3_ENTITY_NULL;
    f32 best_t = A3_F32_MAX;
    u32 types[2] = { A3_T_MESH_RENDERER, A3_T_TRANSFORM };
    A3Query q = a3_query_begin(w, types, 2);
    while (a3_query_next(&q)) {
        A3CMeshRenderer *mr = (A3CMeshRenderer *)q.components[0];
        A3CTransform *t = (A3CTransform *)q.components[1];
        if (!mr->visible) continue;
        const A3MeshAsset *m = a3_assets_mesh_get(a3_assets_mesh_for_renderer(mr));
        if (!m) continue;
        A3Mat4 inv;
        if (!a3_mat4_inverse(&t->world, &inv)) continue;
        A3Ray lr = { a3_mat4_mul_point(&inv, ray.origin), a3_mat4_mul_dir(&inv, ray.dir) };
        f32 scale = a3_v3_len(lr.dir);
        if (scale < 1e-8f) continue;
        lr.dir = a3_v3_scale(lr.dir, 1.0f / scale);
        f32 tb;
        if (!a3_ray_aabb(lr, m->bounds, &tb)) continue;
        f32 tl = A3_F32_MAX;
        const A3MeshData *md = &m->cpu;
        for (u32 i = 0; i + 2 < md->index_count; i += 3) {
            f32 tt;
            if (a3_ray_triangle(lr, md->vertices[md->indices[i]].position, md->vertices[md->indices[i + 1]].position, md->vertices[md->indices[i + 2]].position, &tt) && tt < tl) tl = tt;
        }
        if (tl == A3_F32_MAX) continue;
        f32 world_t = tl / scale;
        if (world_t < best_t) { best_t = world_t; best = q.entity; }
    }
    if (out_t) *out_t = best_t;
    return best;
}

/* ======================================================================== */
/* Gizmos                                                                   */
/* ======================================================================== */

static f32 dist_point_segment(A3Vec2 p, A3Vec2 a, A3Vec2 b) {
    A3Vec2 ab = a3_v2_sub(b, a), ap = a3_v2_sub(p, a);
    f32 t = a3_clampf(a3_v2_dot(ap, ab) / a3_maxf(a3_v2_dot(ab, ab), 1e-6f), 0, 1);
    return a3_v2_len(a3_v2_sub(p, a3_v2_add(a, a3_v2_scale(ab, t))));
}

/* closest point parameter on line (p, d) to ray (o, r) */
static f32 line_ray_param(A3Vec3 p, A3Vec3 d, A3Vec3 o, A3Vec3 r) {
    A3Vec3 w0 = a3_v3_sub(p, o);
    f32 a = a3_v3_dot(d, d), b = a3_v3_dot(d, r), c = a3_v3_dot(r, r), dd = a3_v3_dot(d, w0), e = a3_v3_dot(r, w0);
    f32 den = a * c - b * b;
    if (a3_absf(den) < 1e-8f) return 0;
    return (b * e - c * dd) / den;
}

static b32 ray_plane_hit(A3Ray r, A3Vec3 p, A3Vec3 n, A3Vec3 *hit) {
    f32 den = a3_v3_dot(n, r.dir);
    if (a3_absf(den) < 1e-6f) return 0;
    f32 t = a3_v3_dot(a3_v3_sub(p, r.origin), n) / den;
    *hit = a3_v3_madd(r.origin, r.dir, t);
    return 1;
}

static void gizmo(A3Editor *ed, A3Ui *ui) {
    A3World *w = ed->world;
    if (ed->mode != ED_EDIT) return;
    A3Entity e = ed_selected(ed);
    A3CTransform *t = a3_transform(w, e);
    if (!t) return;
    const A3InputState *in = a3_ui_input(ui);
    A3UiTheme *th = a3_ui_theme(ui);
    A3Mat4 vp = current_view_proj(ed);
    A3Vec3 center = a3_transform_world_position(w, e);
    A3Vec2 c2;
    if (!world_to_screen(ed, &vp, center, &c2)) return;
    f32 dist = a3_v3_dist(center, ed->cam_pos);
    f32 world_len = dist * 0.16f; /* ~constant screen size */
    A3Quat wrot = a3_transform_world_rotation(w, e);
    A3Vec3 axes[3] = { a3_v3(1, 0, 0), a3_v3(0, 1, 0), a3_v3(0, 0, 1) };
    if (ed->gizmo_local || ed->gizmo == ED_GIZMO_SCALE) for (int i = 0; i < 3; ++i) axes[i] = a3_quat_rotate(wrot, axes[i]);
    u32 cols[3] = { th->colors[A3_UIC_AXIS_X], th->colors[A3_UIC_AXIS_Y], th->colors[A3_UIC_AXIS_Z] };
    A3Vec2 ends[3];
    for (int i = 0; i < 3; ++i) if (!world_to_screen(ed, &vp, a3_v3_madd(center, axes[i], world_len), &ends[i])) ends[i] = c2;
    A3Vec2 m = in->mouse_pos;
    A3Ray ray = screen_ray(ed, &vp, m);
    /* hover */
    i32 hover = -1;
    if (!ed->gizmo_dragging && ed->vp_hovered && !ed->cam_flying) {
        f32 best = 9.0f;
        if (ed->gizmo == ED_GIZMO_ROTATE) {
            for (int a = 0; a < 3; ++a) {
                A3Vec3 u = a3_absf(axes[a].y) < 0.99f ? a3_v3_norm(a3_v3_cross(axes[a], a3_v3(0, 1, 0))) : a3_v3(1, 0, 0);
                A3Vec3 v = a3_v3_cross(axes[a], u);
                A3Vec2 prev = c2;
                for (int s = 0; s <= 48; ++s) {
                    f32 sn, cs;
                    a3_sincosf(A3_TAU * (f32)s / 48.0f, &sn, &cs);
                    A3Vec2 p2;
                    if (!world_to_screen(ed, &vp, a3_v3_add(center, a3_v3_add(a3_v3_scale(u, cs * world_len), a3_v3_scale(v, sn * world_len))), &p2)) continue;
                    if (s > 0) { f32 d = dist_point_segment(m, prev, p2); if (d < best) { best = d; hover = a; } }
                    prev = p2;
                }
            }
        } else {
            for (int a = 0; a < 3; ++a) { f32 d = dist_point_segment(m, c2, ends[a]); if (d < best) { best = d; hover = a; } }
            if (ed->gizmo == ED_GIZMO_MOVE) {
                /* plane handles */
                for (int a = 0; a < 3; ++a) {
                    int i1 = (a + 1) % 3, i2 = (a + 2) % 3;
                    A3Vec3 q = a3_v3_add(center, a3_v3_scale(a3_v3_add(axes[i1], axes[i2]), world_len * 0.3f));
                    A3Vec2 q2;
                    if (world_to_screen(ed, &vp, q, &q2) && a3_v2_len(a3_v2_sub(m, q2)) < 10) hover = 3 + a;
                }
            }
            if (ed->gizmo == ED_GIZMO_SCALE && a3_v2_len(a3_v2_sub(m, c2)) < 10) hover = 6;
        }
    }
    /* start drag */
    if (hover >= 0 && in->mouse_pressed[A3_MOUSE_LEFT]) {
        ed->gizmo_dragging = 1;
        ed->gizmo_axis = hover;
        ed->gizmo_start_pos = center;
        ed->gizmo_start_rot = t->rotation;
        ed->gizmo_start_scale = t->scale;
        if (hover < 3 && ed->gizmo == ED_GIZMO_MOVE) {
            f32 s = line_ray_param(center, axes[hover], ray.origin, ray.dir);
            ed->gizmo_grab_offset = a3_v3(s, 0, 0);
        } else if (hover >= 3 && hover < 6) {
            A3Vec3 hit = center;
            ray_plane_hit(ray, center, axes[hover - 3], &hit);
            ed->gizmo_grab_offset = a3_v3_sub(hit, center);
        }
        ed->gizmo_start_angle = a3_atan2f(m.y - c2.y, m.x - c2.x);
        ed->vp_drag_start = m;
    }
    /* drag */
    if (ed->gizmo_dragging) {
        if (!in->mouse[A3_MOUSE_LEFT]) { ed->gizmo_dragging = 0; ed->gizmo_axis = -1; }
        else {
            i32 a = ed->gizmo_axis;
            b32 snap = ed->snap || (in->mods & A3_MOD_CTRL);
            if (ed->gizmo == ED_GIZMO_MOVE) {
                A3Vec3 np = ed->gizmo_start_pos;
                if (a < 3) {
                    f32 s = line_ray_param(ed->gizmo_start_pos, axes[a], ray.origin, ray.dir) - ed->gizmo_grab_offset.x;
                    if (snap) s = a3_roundf(s);
                    np = a3_v3_madd(ed->gizmo_start_pos, axes[a], s);
                } else {
                    A3Vec3 hit;
                    if (ray_plane_hit(ray, ed->gizmo_start_pos, axes[a - 3], &hit)) {
                        np = a3_v3_sub(hit, ed->gizmo_grab_offset);
                        if (snap) np = a3_v3(a3_roundf(np.x), a3_roundf(np.y), a3_roundf(np.z));
                    }
                }
                a3_transform_set_world_position(w, e, np);
                ed_undo_mark_changed(ed, "Move");
            } else if (ed->gizmo == ED_GIZMO_ROTATE) {
                f32 ang = a3_atan2f(m.y - c2.y, m.x - c2.x) - ed->gizmo_start_angle;
                /* screen rotation direction depends on whether the axis faces the camera */
                A3Vec3 to_cam = a3_v3_sub(ed->cam_pos, center);
                if (a3_v3_dot(axes[a], to_cam) > 0) ang = -ang;
                if (snap) ang = a3_roundf(ang / (15 * A3_DEG2RAD)) * 15 * A3_DEG2RAD;
                A3Quat dq = a3_quat_axis_angle(axes[a], ang);
                /* apply in world space, convert back to local */
                A3Entity parent = a3_entity_parent(w, e);
                A3Quat pr = a3_entity_valid(w, parent) ? a3_transform_world_rotation(w, parent) : a3_quat_identity();
                A3Quat start_world = a3_quat_mul(pr, ed->gizmo_start_rot);
                t->rotation = a3_quat_normalize(a3_quat_mul(a3_quat_conjugate(pr), a3_quat_mul(dq, start_world)));
                ed_undo_mark_changed(ed, "Rotate");
            }
        }
    }
    /* scale drag (separate to keep the flow readable) */
    if (ed->gizmo_dragging && ed->gizmo == ED_GIZMO_SCALE) {
        i32 a = ed->gizmo_axis;
        f32 dx = in->mouse_pos.x - ed->vp_drag_start.x, dy = in->mouse_pos.y - ed->vp_drag_start.y;
        f32 amount;
        if (a == 6) amount = (dx - dy) / 100.0f;
        else {
            A3Vec2 dir = a3_v2_norm(a3_v2_sub(ends[a], c2));
            amount = (dx * dir.x + dy * dir.y) / 100.0f;
        }
        f32 k = a3_maxf(1.0f + amount, 0.01f);
        b32 snap = ed->snap || (in->mods & A3_MOD_CTRL);
        A3Vec3 s = ed->gizmo_start_scale;
        if (a == 6) s = a3_v3_scale(s, k);
        else if (a == 0) s.x *= k;
        else if (a == 1) s.y *= k;
        else if (a == 2) s.z *= k;
        if (snap) s = a3_v3(a3_roundf(s.x * 4) / 4, a3_roundf(s.y * 4) / 4, a3_roundf(s.z * 4) / 4);
        t->scale = s;
        ed_undo_mark_changed(ed, "Scale");
    }
    /* draw */
    a3_ui_push_clip(ui, ed->vp_rect);
    i32 hl = ed->gizmo_dragging ? ed->gizmo_axis : hover;
    if (ed->gizmo == ED_GIZMO_ROTATE) {
        for (int a = 0; a < 3; ++a) {
            A3Vec3 u = a3_absf(axes[a].y) < 0.99f ? a3_v3_norm(a3_v3_cross(axes[a], a3_v3(0, 1, 0))) : a3_v3(1, 0, 0);
            A3Vec3 v = a3_v3_cross(axes[a], u);
            A3Vec2 pts[49];
            u32 n = 0;
            for (int s = 0; s <= 48; ++s) {
                f32 sn, cs;
                a3_sincosf(A3_TAU * (f32)s / 48.0f, &sn, &cs);
                A3Vec2 p2;
                if (world_to_screen(ed, &vp, a3_v3_add(center, a3_v3_add(a3_v3_scale(u, cs * world_len), a3_v3_scale(v, sn * world_len))), &p2)) pts[n++] = p2;
            }
            a3_ui_polyline(ui, pts, n, hl == a ? 0xFFFFFFFFu : cols[a], hl == a ? 3.0f : 2.0f, 0);
        }
    } else {
        for (int a = 0; a < 3; ++a) {
            u32 col = hl == a ? a3_rgb(255, 230, 90) : cols[a];
            a3_ui_line(ui, c2, ends[a], col, hl == a ? 3.5f : 2.5f);
            A3Vec2 dir = a3_v2_norm(a3_v2_sub(ends[a], c2));
            A3Vec2 nrm = a3_v2(-dir.y, dir.x);
            if (ed->gizmo == ED_GIZMO_MOVE) {
                A3Vec2 tip = a3_v2_add(ends[a], a3_v2_scale(dir, 12));
                a3_ui_triangle(ui, tip, a3_v2_add(ends[a], a3_v2_scale(nrm, 6)), a3_v2_sub(ends[a], a3_v2_scale(nrm, 6)), col);
            } else {
                a3_ui_rect(ui, a3_rect(ends[a].x - 5, ends[a].y - 5, 10, 10), col, 2);
            }
        }
        if (ed->gizmo == ED_GIZMO_MOVE) {
            for (int a = 0; a < 3; ++a) {
                int i1 = (a + 1) % 3, i2 = (a + 2) % 3;
                A3Vec3 q = a3_v3_add(center, a3_v3_scale(a3_v3_add(axes[i1], axes[i2]), world_len * 0.3f));
                A3Vec2 q2;
                if (world_to_screen(ed, &vp, q, &q2)) a3_ui_rect(ui, a3_rect(q2.x - 6, q2.y - 6, 12, 12), a3_color_alpha(hl == 3 + a ? a3_rgb(255, 230, 90) : cols[a], 0.75f), 2);
            }
        } else {
            a3_ui_rect(ui, a3_rect(c2.x - 6, c2.y - 6, 12, 12), hl == 6 ? a3_rgb(255, 230, 90) : a3_rgb(230, 230, 230), 2);
        }
    }
    a3_ui_pop_clip(ui);
    if (hover >= 0 || ed->gizmo_dragging) ed->gizmo_hot = 1;
}

/* ======================================================================== */
/* Overlays                                                                 */
/* ======================================================================== */

static void draw_overlays(A3Editor *ed) {
    A3Renderer *r = a3_engine_renderer(ed->engine);
    A3World *w = ed_active_world(ed);
    A3Entity sel = ed_selected(ed);
    const A3Vec4 sel_col = a3_v4(1.0f, 0.62f, 0.15f, 1);
    if (a3_entity_valid(w, sel)) {
        A3CTransform *t = a3_transform(w, sel);
        A3CMeshRenderer *mr = (A3CMeshRenderer *)a3_component_get(w, sel, A3_T_MESH_RENDERER);
        if (t && mr) {
            const A3MeshAsset *m = a3_assets_mesh_get(a3_assets_mesh_for_renderer(mr));
            if (m) a3_debug_obb(r, &t->world, m->bounds, sel_col);
        }
        A3CLight *l = (A3CLight *)a3_component_get(w, sel, A3_T_LIGHT);
        if (t && l) {
            A3Vec3 p = a3_mat4_get_translation(&t->world);
            A3Vec3 f = a3_v3_norm(a3_mat4_mul_dir(&t->world, a3_v3(0, 0, -1)));
            if (l->type == A3_LIGHT_POINT) a3_debug_sphere(r, p, l->range, a3_v4(1, 0.9f, 0.4f, 0.6f));
            else a3_debug_arrow(r, p, a3_v3_madd(p, f, l->type == A3_LIGHT_SPOT ? l->range : 3.0f), a3_v4(1, 0.9f, 0.4f, 1));
        }
        A3CCamera *cam = (A3CCamera *)a3_component_get(w, sel, A3_T_CAMERA);
        if (t && cam) {
            A3Mat4 proj = a3_mat4_perspective(cam->fov * A3_DEG2RAD, 16.0f / 9.0f, cam->near_plane, a3_minf(cam->far_plane, 12.0f), 0);
            A3Mat4 world = a3_transform_compute_world(w, sel);
            A3Vec3 pos, sc;
            A3Quat rot;
            a3_mat4_decompose(&world, &pos, &rot, &sc);
            A3Mat4 cm = a3_mat4_trs(pos, rot, a3_v3_one());
            A3Mat4 view = a3_mat4_inverse_affine(&cm);
            A3Mat4 vp = a3_mat4_mul(&proj, &view), inv;
            if (a3_mat4_inverse(&vp, &inv)) {
                A3Vec3 c[8];
                for (int i = 0; i < 8; ++i) c[i] = a3_mat4_mul_point_project(&inv, a3_v3((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f));
                static const int E[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
                for (int k = 0; k < 12; ++k) a3_debug_line(r, c[E[k][0]], c[E[k][1]], a3_v4(0.8f, 0.8f, 0.9f, 0.9f));
            }
        }
    }
    if (ed->show_colliders) {
        u32 n = 0;
        const A3Entity *ents = 0;
        a3_component_array(w, A3_T_COLLIDER, &n, &ents);
        for (u32 i = 0; i < n; ++i) {
            A3ShapeInstance s;
            if (!a3_physics_collider_shape(w, ents[i], &s)) continue;
            A3Vec4 col = a3_v4(0.3f, 1.0f, 0.4f, 0.8f);
            A3CCollider *c = (A3CCollider *)a3_component_get(w, ents[i], A3_T_COLLIDER);
            if (c && c->is_trigger) col = a3_v4(0.3f, 0.7f, 1.0f, 0.8f);
            if (s.type == A3_SHAPE_BOX) {
                A3Mat4 m = a3_mat4_trs(s.pos, s.rot, a3_v3_one());
                a3_debug_obb(r, &m, a3_aabb(a3_v3_neg(s.half), s.half), col);
            } else if (s.type == A3_SHAPE_SPHERE) {
                a3_debug_sphere(r, s.pos, s.radius, col);
            } else if (s.type == A3_SHAPE_CAPSULE) {
                A3Vec3 ax = a3_quat_rotate(s.rot, a3_v3(0, 1, 0));
                A3Vec3 p0 = a3_v3_madd(s.pos, ax, -s.half_height), p1 = a3_v3_madd(s.pos, ax, s.half_height);
                a3_debug_sphere(r, p0, s.radius, col);
                a3_debug_sphere(r, p1, s.radius, col);
                A3Vec3 side = a3_quat_rotate(s.rot, a3_v3(s.radius, 0, 0)), side2 = a3_quat_rotate(s.rot, a3_v3(0, 0, s.radius));
                a3_debug_line(r, a3_v3_add(p0, side), a3_v3_add(p1, side), col);
                a3_debug_line(r, a3_v3_sub(p0, side), a3_v3_sub(p1, side), col);
                a3_debug_line(r, a3_v3_add(p0, side2), a3_v3_add(p1, side2), col);
                a3_debug_line(r, a3_v3_sub(p0, side2), a3_v3_sub(p1, side2), col);
            } else {
                a3_debug_aabb(r, s.aabb, col);
            }
        }
        /* character capsules */
        a3_component_array(w, A3_T_CHARACTER, &n, &ents);
        for (u32 i = 0; i < n; ++i) {
            A3CCharacterController *cc = (A3CCharacterController *)a3_component_get(w, ents[i], A3_T_CHARACTER);
            A3Vec3 feet = a3_transform_world_position(w, ents[i]);
            A3Vec3 p0 = a3_v3_add(feet, a3_v3(0, cc->radius, 0)), p1 = a3_v3_add(feet, a3_v3(0, cc->height - cc->radius, 0));
            a3_debug_sphere(r, p0, cc->radius, a3_v4(1, 0.5f, 1, 0.8f));
            a3_debug_sphere(r, p1, cc->radius, a3_v4(1, 0.5f, 1, 0.8f));
        }
    }
    if (ed->show_bounds) {
        u32 types[2] = { A3_T_MESH_RENDERER, A3_T_TRANSFORM };
        A3Query q = a3_query_begin(w, types, 2);
        while (a3_query_next(&q)) {
            const A3MeshAsset *m = a3_assets_mesh_get(a3_assets_mesh_for_renderer((A3CMeshRenderer *)q.components[0]));
            if (m) a3_debug_aabb(r, a3_aabb_transform(m->bounds, &((A3CTransform *)q.components[1])->world), a3_v4(0.5f, 0.5f, 0.6f, 0.5f));
        }
    }
}

void ed_viewport_render(A3Editor *ed) {
    A3World *w = ed_active_world(ed);
    if (!w || ed->vp_w <= 8 || ed->vp_h <= 8) return;
    i32 tw = 0, th = 0;
    a3_rhi_texture_size(ed->vp_color, &tw, &th);
    if (tw != ed->vp_w || th != ed->vp_h || !ed->vp_target.id) {
        ed_viewport_shutdown(ed);
        A3TextureDesc d;
        a3_zero_struct(&d);
        d.width = ed->vp_w;
        d.height = ed->vp_h;
        d.format = A3_TEX_RGBA8;
        d.wrap = A3_WRAP_CLAMP;
        d.debug_name = "editor_viewport";
        ed->vp_color = a3_rhi_texture_create(&d);
        ed->vp_target = a3_rhi_target_create(ed->vp_color, (A3RhiTexture){ 0 });
    }
    if (!ed->vp_target.id) return;
    b32 is_game;
    A3RenderView v = make_view(ed, &is_game);
    if (!is_game) draw_overlays(ed);
    a3_renderer_settings(a3_engine_renderer(ed->engine))->wireframe = ed->show_wireframe;
    v.time = (f32)a3_engine_play_time(ed->engine);
    a3_renderer_draw_world(a3_engine_renderer(ed->engine), w, &v);
}

void ed_viewport_shutdown(A3Editor *ed) {
    if (ed->vp_target.id) a3_rhi_target_destroy(ed->vp_target);
    if (ed->vp_color.id) a3_rhi_texture_destroy(ed->vp_color);
    ed->vp_target.id = ed->vp_color.id = 0;
}

/* ======================================================================== */
/* Panel                                                                    */
/* ======================================================================== */

static void icon_overlays(A3Editor *ed, A3Ui *ui, const A3Mat4 *vp) {
    if (!ed->show_icons) return;
    A3World *w = ed_active_world(ed);
    A3UiTheme *th = a3_ui_theme(ui);
    a3_ui_push_clip(ui, ed->vp_rect);
    u32 types[2] = { A3_T_LIGHT, A3_T_TRANSFORM };
    A3Query q = a3_query_begin(w, types, 2);
    while (a3_query_next(&q)) {
        A3CLight *l = (A3CLight *)q.components[0];
        A3Vec2 s;
        if (!world_to_screen(ed, vp, a3_mat4_get_translation(&((A3CTransform *)q.components[1])->world), &s)) continue;
        u32 col = a3_color_from_vec4(a3_v4(l->color.x, l->color.y, l->color.z, 1));
        a3_ui_circle(ui, s, 11, a3_rgba(20, 22, 28, 190));
        a3_ui_icon(ui, l->type == A3_LIGHT_DIRECTIONAL ? A3_ICON_SUN : A3_ICON_LIGHT, s, col, A3_FONT_UI);
        if (ed->mode == ED_EDIT && a3_v2_len(a3_v2_sub(a3_ui_mouse(ui), s)) < 11 && a3_ui_input(ui)->mouse_pressed[A3_MOUSE_LEFT] && !ed->gizmo_hot) {
            ed_select(ed, q.entity);
            ed->icon_clicked = 1;
        }
    }
    u32 ctypes[2] = { A3_T_CAMERA, A3_T_TRANSFORM };
    q = a3_query_begin(w, ctypes, 2);
    while (a3_query_next(&q)) {
        if (a3_entity_valid(w, a3_entity_parent(w, q.entity)) && a3_component_has(w, a3_entity_parent(w, q.entity), A3_T_CHARACTER)) continue;
        A3Vec2 s;
        if (!world_to_screen(ed, vp, a3_transform_world_position(w, q.entity), &s)) continue;
        a3_ui_circle(ui, s, 11, a3_rgba(20, 22, 28, 190));
        a3_ui_icon(ui, A3_ICON_TARGET, s, th->colors[A3_UIC_TEXT], A3_FONT_UI);
        if (ed->mode == ED_EDIT && a3_v2_len(a3_v2_sub(a3_ui_mouse(ui), s)) < 11 && a3_ui_input(ui)->mouse_pressed[A3_MOUSE_LEFT] && !ed->gizmo_hot) {
            ed_select(ed, q.entity);
            ed->icon_clicked = 1;
        }
    }
    a3_ui_pop_clip(ui);
}

static void viewport_toolbar(A3Editor *ed, A3Ui *ui, A3Rect r) {
    A3UiTheme *th = a3_ui_theme(ui);
    A3Rect bar = a3_rect(r.x + 8, r.y + 8, 330, 32);
    a3_ui_rect(ui, bar, a3_color_alpha(th->colors[A3_UIC_HEADER], 0.92f), 8);
    A3Vec2 saved = a3_ui_cursor_pos(ui);
    a3_ui_set_cursor_pos(ui, a3_v2(bar.x + 4, bar.y + 4));
    if (a3_ui_icon_button(ui, A3_ICON_MOVE_H, "Move (W)", ed->gizmo == ED_GIZMO_MOVE ? A3_BUTTON_TOGGLED : 0)) ed->gizmo = ED_GIZMO_MOVE;
    a3_ui_same_line(ui);
    if (a3_ui_icon_button(ui, A3_ICON_REFRESH, "Rotate (E)", ed->gizmo == ED_GIZMO_ROTATE ? A3_BUTTON_TOGGLED : 0)) ed->gizmo = ED_GIZMO_ROTATE;
    a3_ui_same_line(ui);
    if (a3_ui_icon_button(ui, A3_ICON_BOX, "Scale (R)", ed->gizmo == ED_GIZMO_SCALE ? A3_BUTTON_TOGGLED : 0)) ed->gizmo = ED_GIZMO_SCALE;
    a3_ui_same_line(ui);
    if (a3_ui_icon_button(ui, A3_ICON_HOME, ed->gizmo_local ? "Local space (click for world)" : "World space (click for local)", ed->gizmo_local ? A3_BUTTON_TOGGLED : 0)) ed->gizmo_local = !ed->gizmo_local;
    a3_ui_same_line(ui);
    if (a3_ui_icon_button(ui, A3_ICON_HAMBURGER, "Snap to grid (hold Ctrl)", ed->snap ? A3_BUTTON_TOGGLED : 0)) ed->snap = !ed->snap;
    a3_ui_same_line(ui);
    if (a3_ui_icon_button(ui, A3_ICON_DASHED_BOX, "Show colliders", ed->show_colliders ? A3_BUTTON_TOGGLED : 0)) ed->show_colliders = !ed->show_colliders;
    a3_ui_same_line(ui);
    if (a3_ui_icon_button(ui, A3_ICON_HEX, "Wireframe", ed->show_wireframe ? A3_BUTTON_TOGGLED : 0)) ed->show_wireframe = !ed->show_wireframe;
    a3_ui_same_line(ui);
    if (a3_ui_icon_button(ui, A3_ICON_SQUARE_O, "Grid", ed->show_grid ? A3_BUTTON_TOGGLED : 0)) ed->show_grid = !ed->show_grid;
    a3_ui_same_line(ui);
    if (a3_ui_icon_button(ui, A3_ICON_TARGET, ed->game_view ? "Game camera (click for editor camera)" : "Editor camera (click for game camera while playing)", ed->game_view ? A3_BUTTON_TOGGLED : 0)) ed->game_view = !ed->game_view;
    a3_ui_set_cursor_pos(ui, saved);
    /* stats overlay */
    const A3RenderFrameInfo *fi = a3_renderer_frame_info(a3_engine_renderer(ed->engine));
    char buf[160];
    a3_snprintf(buf, sizeof(buf), "%.0f FPS  %u draws  %u/%u visible", (f64)ed->fps, a3_rhi_stats()->draw_calls, fi->visible, fi->renderables);
    f32 tw = a3_font_text_width(A3_FONT_UI, buf, -1) + 20;
    A3Rect sr = a3_rect(r.x + r.w - tw - 8, r.y + 8, tw, 26);
    a3_ui_rect(ui, sr, a3_color_alpha(th->colors[A3_UIC_HEADER], 0.85f), 8);
    a3_ui_text_in_rect(ui, A3_FONT_UI, sr, A3_ALIGN_CENTER, th->colors[A3_UIC_TEXT_DIM], buf);
    if (ed->mode != ED_EDIT) {
        const char *lbl = ed->mode == ED_PAUSED ? "PAUSED" : "PLAYING";
        u32 col = ed->mode == ED_PAUSED ? th->colors[A3_UIC_WARNING] : th->colors[A3_UIC_SUCCESS];
        A3Rect pr = a3_rect(r.x + r.w * 0.5f - 60, r.y + 8, 120, 26);
        a3_ui_rect(ui, pr, a3_color_alpha(col, 0.9f), 13);
        a3_ui_text_in_rect(ui, A3_FONT_UI_BOLD, pr, A3_ALIGN_CENTER, a3_rgb(20, 20, 20), lbl);
    }
}

void ed_viewport_panel(void *user, A3Ui *ui, A3Rect r) {
    A3Editor *ed = (A3Editor *)user;
    A3Rect full = a3_ui_panel_rect(ui);
    full = a3_rect(full.x - 8, full.y - 8, full.w + 16, full.h + 16); /* use the whole tab area */
    ed->vp_rect = full;
    ed->vp_w = (i32)full.w;
    ed->vp_h = (i32)full.h;
    A3_UNUSED(r);
    const A3InputState *in = a3_ui_input(ui);
    ed->vp_hovered = a3_rect_contains(full, in->mouse_pos) && !a3_ui_popup_open(ui, "__none__");
    /* image (GL textures are bottom-up) */
    if (ed->vp_color.id) a3_ui_image(ui, ed->vp_color, full, a3_v2(0, 1), a3_v2(1, 0), 0xFFFFFFFFu);
    /* the viewport area is interactive: claim it so clicks don't fall through */
    A3Rect hit = full;
    b32 clicked_empty = 0;
    ed->gizmo_hot = 0;
    ed->icon_clicked = 0;
    A3Mat4 vp = current_view_proj(ed);
    viewport_toolbar(ed, ui, full);
    b32 over_toolbar = a3_rect_contains(a3_rect(full.x + 8, full.y + 8, 330, 32), in->mouse_pos);
    if (!over_toolbar) {
        gizmo(ed, ui);
        icon_overlays(ed, ui, &vp);
    }
    a3_ui_invisible_button(ui, "viewport_area", hit);
    b32 ui_blocked = !a3_ui_item_hovered(ui);
    /* while playing, the game receives input when the pointer is over the viewport */
    ed->game_focused = ed->mode == ED_PLAY && !ui_blocked && !over_toolbar;
    if (!ui_blocked) {
        camera_controls(ed, ui, a3_maxf(1.0f / 240.0f, ed->fps > 0 ? 1.0f / ed->fps : 1.0f / 60.0f));
        if (in->mouse_pressed[A3_MOUSE_LEFT] && !ed->gizmo_hot && !ed->icon_clicked && !(in->mods & A3_MOD_ALT) && !over_toolbar && ed->mode == ED_EDIT) {
            f32 t;
            A3Entity e = pick(ed, screen_ray(ed, &vp, in->mouse_pos), &t);
            if (a3_entity_valid(ed->world, e)) {
                /* clicking a child mesh of a character selects the character */
                A3Entity p = a3_entity_parent(ed->world, e);
                if (a3_entity_valid(ed->world, p) && a3_component_has(ed->world, p, A3_T_CHARACTER)) e = p;
                ed_select(ed, e);
            } else clicked_empty = 1;
        }
        if (clicked_empty) ed->selected = 0;
    }
    if (a3_ui_item_hovered(ui) && !ed->cam_flying && !ed->gizmo_hot) a3_ui_tooltip(ui, "Right mouse + WASD: fly    Alt + left drag: orbit    Middle drag: pan    F: focus");
    /* drag & drop assets into the scene */
    u32 size;
    const char *path = (const char *)a3_ui_drop_target_rect(ui, full, "asset", &size);
    if (path && ed->mode == ED_EDIT) {
        A3Ray ray = screen_ray(ed, &vp, in->mouse_pos);
        f32 t;
        A3Entity under = pick(ed, ray, &t);
        A3Vec3 drop_pos = a3_v3_madd(ray.origin, ray.dir, 8);
        if (a3_entity_valid(ed->world, under)) drop_pos = a3_v3_madd(ray.origin, ray.dir, t);
        else if (ray.dir.y < -1e-3f) drop_pos = a3_v3_madd(ray.origin, ray.dir, -ray.origin.y / ray.dir.y);
        const char *ext = a3_path_extension(path);
        if (a3_streq(ext, ".obj")) {
            char name[64];
            a3_path_stem(path, name, sizeof(name));
            A3Entity e = ed_create_entity(ed, name, A3_PRIM_NONE, 0);
            A3CMeshRenderer *mr = (A3CMeshRenderer *)a3_component_add(ed->world, e, A3_T_MESH_RENDERER);
            mr->primitive = A3_PRIM_NONE;
            a3_strcpy(mr->mesh.path, sizeof(mr->mesh.path), path);
            a3_transform(ed->world, e)->position = drop_pos;
            A3_INFO("editor", "placed model '%s'", path);
        } else if (a3_streq(ext, ".png") || a3_streq(ext, ".tga") || a3_streq(ext, ".bmp")) {
            if (a3_entity_valid(ed->world, under)) {
                ed_select(ed, under);
                ed_undo_begin_frame(ed);
                A3CMeshRenderer *mr = (A3CMeshRenderer *)a3_component_get(ed->world, under, A3_T_MESH_RENDERER);
                if (mr) {
                    a3_strcpy(mr->texture.path, sizeof(mr->texture.path), path);
                    mr->texture.handle = 0;
                    ed_undo_mark_changed(ed, "Apply Texture");
                    a3_ui_notify(ui, 0, "Applied %s to %s", a3_path_filename(path), a3_entity_name(ed->world, under));
                }
            } else a3_ui_notify(ui, a3_ui_theme(ui)->colors[A3_UIC_WARNING], "Drop the image onto an object to apply it");
        } else if (a3_streq(ext, ".a3scene")) {
            ed_scene_open(ed, path);
        }
    }
    /* keyboard shortcuts while the viewport has focus */
    if (ed->vp_hovered && !a3_ui_wants_keyboard(ui) && !ed->cam_flying) {
        if (in->keys_pressed[A3_KEY_W]) ed->gizmo = ED_GIZMO_MOVE;
        if (in->keys_pressed[A3_KEY_E]) ed->gizmo = ED_GIZMO_ROTATE;
        if (in->keys_pressed[A3_KEY_R]) ed->gizmo = ED_GIZMO_SCALE;
        if (in->keys_pressed[A3_KEY_F]) ed_focus_selected(ed);
    }
}
