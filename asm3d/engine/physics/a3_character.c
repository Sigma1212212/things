/*
 * ASM3D - a3_character.c
 * Character controller: kinematic capsule with collide-and-slide, stair
 * stepping, slope limits, ground snapping, jumping, pushing dynamic bodies,
 * mouse look and first/third person cameras.
 *
 * The entity origin is at the character's feet.
 */
#include "a3_physics.h"
#include "a3_physics_internal.h"
#include "a3_character.h"
#include "../scene/a3_components.h"
#include "../core/a3_log.h"
#include "../core/a3_string.h"

typedef b32 (*A3PhysBodyVisit)(const A3ShapeInstance *shape, const A3TriMesh *mesh, A3Entity e, b32 dynamic, void *user);
void a3__physics_visit_near(A3World *w, A3Aabb box, A3Entity ignore, A3PhysBodyVisit fn, void *user);

typedef struct ResolveCtx {
    A3World *world;
    A3ShapeInstance capsule;
    A3Vec3 push;           /* accumulated correction this pass */
    A3Vec3 ground_normal;
    b32 grounded;
    b32 hit_ceiling;
    b32 blocked;           /* hit a non-walkable surface */
    A3Vec3 wall_normal;
    f32 walk_cos;
    A3Vec3 push_dir;       /* movement direction for pushing dynamic bodies */
    f32 push_strength;
    /* probe mode: find low obstacles the character can step onto */
    b32 probe;
    f32 base_y;            /* feet height at the start of the move */
    f32 step_height;
    f32 step_top;          /* highest steppable obstacle top found */
    b32 tall;              /* an obstacle too high to step onto */
    /* classify mode: what would the capsule touch at a position */
    b32 classify;
    b32 c_walk, c_support, c_steep;
} ResolveCtx;

static void make_capsule(A3ShapeInstance *s, A3Vec3 feet, f32 height, f32 radius) {
    a3_zero_struct(s);
    s->type = A3_SHAPE_CAPSULE;
    s->radius = radius;
    s->half_height = a3_maxf(height * 0.5f - radius, 0.0f);
    s->pos = a3_v3_add(feet, a3_v3(0, height * 0.5f, 0));
    s->rot = a3_quat_identity();
    s->rot_m = a3_mat4_identity();
    A3Vec3 ext = a3_v3(radius, height * 0.5f, radius);
    s->aabb = a3_aabb(a3_v3_sub(s->pos, ext), a3_v3_add(s->pos, ext));
}

static b32 resolve_visit(const A3ShapeInstance *shape, const A3TriMesh *mesh, A3Entity e, b32 dynamic, void *user) {
    ResolveCtx *c = (ResolveCtx *)user;
    A3Manifold m;
    b32 hit = mesh ? a3_collide_mesh(mesh, &c->capsule, &m) : a3_collide_convex(shape, &c->capsule, &m);
    if (!hit || !m.count) return 1;
    f32 depth = 0;
    for (u32 i = 0; i < m.count; ++i) depth = a3_maxf(depth, m.pts[i].depth);
    A3Vec3 n = m.normal;
    if (c->classify) {
        if (n.y >= c->walk_cos) c->c_walk = 1;
        else if (n.y > -0.5f) {
            f32 top = -A3_F32_MAX;
            if (mesh) { for (u32 i = 0; i < m.count; ++i) top = a3_maxf(top, m.pts[i].pos.y); }
            else top = shape->aabb.max.y;
            if (!dynamic && top - c->base_y <= c->step_height + 0.001f) c->c_support = 1; /* edge of a step */
            else c->c_steep = 1;
        }
        return 1;
    }
    if (c->probe) {
        if (n.y < c->walk_cos && n.y > -0.5f && !dynamic) {
            /* obstacle top: bounding box top for convex shapes, contact height for meshes */
            f32 top = -A3_F32_MAX;
            if (mesh) { for (u32 i = 0; i < m.count; ++i) top = a3_maxf(top, m.pts[i].pos.y); }
            else top = shape->aabb.max.y;
            if (top - c->base_y <= c->step_height + 0.001f) c->step_top = a3_maxf(c->step_top, top);
            else c->tall = 1;
        }
        return 1;
    }
    if (dynamic) {
        /* push the object instead of being blocked hard by it */
        if (a3_v3_dot(c->push_dir, n) < 0) a3_physics_add_impulse(c->world, e, a3_v3_scale(a3_v3_neg(n), c->push_strength));
        depth *= 0.5f;
    }
    if (n.y >= c->walk_cos) {
        c->grounded = 1;
        c->ground_normal = n;
        /* stand on slopes without sliding: resolve vertically */
        c->push = a3_v3_add(c->push, a3_v3(0, depth / a3_maxf(n.y, 0.2f), 0));
    } else {
        if (n.y < -0.5f) c->hit_ceiling = 1;
        else { c->blocked = 1; c->wall_normal = n; }
        c->push = a3_v3_madd(c->push, n, depth);
    }
    return 1;
}

/* Moves the capsule out of any geometry it overlaps. */
static void resolve(ResolveCtx *c, A3Entity self, A3Vec3 *feet, f32 height, f32 radius) {
    c->grounded = c->hit_ceiling = c->blocked = 0;
    for (int pass = 0; pass < 4; ++pass) {
        make_capsule(&c->capsule, *feet, height, radius);
        c->push = a3_v3_zero();
        A3Aabb q = c->capsule.aabb;
        q.min = a3_v3_sub(q.min, a3_v3s(0.05f));
        q.max = a3_v3_add(q.max, a3_v3s(0.05f));
        a3__physics_visit_near(c->world, q, self, resolve_visit, c);
        if (a3_v3_len_sq(c->push) < 1e-10f) break;
        *feet = a3_v3_add(*feet, c->push);
    }
}

/* Moves down by up to `dist` in small increments, stopping at the first
 * support: walkable ground (resolved exactly) or the edge of a step (the
 * capsule rides up over edges smoothly). Steep surfaces that are not step
 * edges are resolved normally, so the character slides off them.
 * Returns true when the character ends up supported. */
static b32 sweep_down(ResolveCtx *c, A3Entity self, A3Vec3 *feet, f32 dist, f32 height, f32 radius, f32 step_height) {
    const f32 inc = 0.02f;
    f32 moved = 0;
    while (moved < dist) {
        f32 st = a3_minf(inc, dist - moved);
        A3Vec3 test = *feet;
        test.y -= st;
        ResolveCtx k = *c;
        k.classify = 1;
        k.c_walk = k.c_support = k.c_steep = 0;
        k.base_y = test.y;
        k.step_height = step_height;
        make_capsule(&k.capsule, test, height, radius);
        a3__physics_visit_near(c->world, k.capsule.aabb, self, resolve_visit, &k);
        if (k.c_walk) {
            *feet = test;
            resolve(c, self, feet, height, radius);
            return 1;
        }
        if (k.c_support && !k.c_steep) return 1; /* resting on a step edge: stay above it */
        *feet = test;
        moved += st;
        if (k.c_steep) {
            resolve(c, self, feet, height, radius); /* slide along the steep surface */
            if (c->grounded) return 1;
        }
    }
    return 0;
}

static A3Vec3 move_towards(A3Vec3 v, A3Vec3 target, f32 max_delta) {
    A3Vec3 d = a3_v3_sub(target, v);
    f32 l = a3_v3_len(d);
    if (l <= max_delta || l < 1e-6f) return target;
    return a3_v3_madd(v, d, max_delta / l);
}

void a3_character_move(A3World *w, A3Entity e, A3CCharacterController *cc, A3Vec2 move_input, b32 jump, b32 sprint, f32 dt) {
    A3CTransform *t = (A3CTransform *)a3_component_get(w, e, A3_T_TRANSFORM);
    if (!t || dt <= 0) return;
    A3Vec3 gravity = a3_v3_scale(a3_world_settings(w)->gravity, cc->gravity_scale);
    f32 g = a3_v3_len(gravity);
    A3Vec3 feet = a3_transform_world_position(w, e);
    /* desired horizontal velocity relative to the view yaw */
    f32 sy, cy;
    a3_sincosf(cc->yaw, &sy, &cy);
    A3Vec3 fwd = a3_v3(-sy, 0, -cy), right = a3_v3(cy, 0, -sy);
    A3Vec3 wish = a3_v3_add(a3_v3_scale(fwd, move_input.y), a3_v3_scale(right, move_input.x));
    if (a3_v3_len_sq(wish) > 1.0f) wish = a3_v3_norm(wish);
    wish = a3_v3_scale(wish, sprint ? cc->sprint_speed : cc->walk_speed);
    A3Vec3 vh = a3_v3(cc->velocity.x, 0, cc->velocity.z);
    f32 accel = cc->acceleration * (cc->grounded ? 1.0f : a3_maxf(cc->air_control, 0.0f));
    vh = move_towards(vh, wish, accel * dt);
    f32 vy = cc->velocity.y;
    if (cc->grounded && vy <= 0) vy = 0;
    if (jump && cc->grounded) {
        vy = a3_sqrtf(2.0f * g * a3_maxf(cc->jump_height, 0.0f));
        cc->grounded = 0;
    }
    vy -= g * dt;

    ResolveCtx c;
    a3_zero_struct(&c);
    c.world = w;
    c.walk_cos = a3_cosf(a3_clampf(cc->slope_limit, 0, 89) * A3_DEG2RAD);
    c.push_dir = a3_v3_norm(vh);
    c.push_strength = 0.6f * dt * 60.0f;
    f32 h = cc->height, r = cc->radius;
    b32 was_grounded = cc->grounded;

    /* 1. horizontal move, sub-stepped so thin walls are never skipped.
     *    Low obstacles (stairs, curbs) found by a probe lift the character
     *    onto them instead of blocking it. */
    f32 base_y = feet.y;
    A3Vec3 d = a3_v3_scale(vh, dt);
    f32 len = a3_v3_len(d);
    int sub = (int)(len / (r * 0.5f)) + 1;
    if (sub > 16) sub = 16;
    A3Vec3 ds = a3_v3_scale(d, 1.0f / (f32)sub);
    for (int i = 0; i < sub; ++i) {
        feet = a3_v3_add(feet, ds);
        if (was_grounded && cc->step_height > 0) {
            ResolveCtx pc = c;
            pc.probe = 1;
            pc.base_y = base_y;
            pc.step_height = cc->step_height;
            pc.step_top = -A3_F32_MAX;
            make_capsule(&pc.capsule, feet, h, r);
            a3__physics_visit_near(w, pc.capsule.aabb, e, resolve_visit, &pc);
            if (!pc.tall && pc.step_top > feet.y + 0.001f) {
                /* only step if there is head room above the obstacle */
                A3Vec3 up = feet;
                up.y = pc.step_top + 0.005f;
                ResolveCtx hc = c;
                resolve(&hc, e, &up, h, r);
                if (!hc.hit_ceiling && !hc.blocked) feet = up;
            }
        }
        resolve(&c, e, &feet, h, r);
        if (c.blocked) {
            /* slide along the wall: remove the velocity going into it */
            A3Vec3 wn = a3_v3_norm(a3_v3(c.wall_normal.x, 0, c.wall_normal.z));
            f32 into = a3_v3_dot(vh, wn);
            if (into < 0) vh = a3_v3_madd(vh, wn, -into);
            f32 into_s = a3_v3_dot(ds, wn);
            if (into_s < 0) ds = a3_v3_madd(ds, wn, -into_s);
        }
    }
    /* 2. vertical move: rising (jump) resolves against ceilings; falling
     *    sweeps down, and while walking also snaps down slopes and stairs */
    b32 grounded = 0;
    if (vy > 0) {
        feet.y += vy * dt;
        resolve(&c, e, &feet, h, r);
        if (c.hit_ceiling) vy = 0;
        grounded = 0;
    } else {
        f32 fall = -vy * dt;
        f32 snap = (was_grounded && !jump) ? cc->step_height : 0.0f;
        A3Vec3 before = feet;
        grounded = sweep_down(&c, e, &feet, fall + snap, h, r, cc->step_height);
        if (!grounded && snap > 0) {
            /* nothing below within step height: walked off a ledge, fall normally */
            feet = before;
            feet.y -= fall;
            resolve(&c, e, &feet, h, r);
            grounded = c.grounded;
        }
    }
    if (grounded && vy < 0) vy = 0;
    cc->grounded = grounded;
    cc->velocity = a3_v3(vh.x, vy, vh.z);
    a3_transform_set_world_position(w, e, feet);
    A3Entity parent = a3_entity_parent(w, e);
    A3Quat yaw_q = a3_quat_axis_angle(a3_v3(0, 1, 0), cc->yaw);
    if (a3_entity_valid(w, parent)) yaw_q = a3_quat_mul(a3_quat_conjugate(a3_transform_world_rotation(w, parent)), yaw_q);
    t->rotation = yaw_q;
}

void a3_character_look(A3World *w, A3Entity e, A3CCharacterController *cc, A3Vec2 mouse_delta) {
    f32 s = cc->mouse_sensitivity * A3_DEG2RAD;
    cc->yaw = a3_wrap_angle(cc->yaw - mouse_delta.x * s);
    cc->pitch = a3_clampf(cc->pitch - mouse_delta.y * s, -89.0f * A3_DEG2RAD, 89.0f * A3_DEG2RAD);
    A3CTransform *t = (A3CTransform *)a3_component_get(w, e, A3_T_TRANSFORM);
    if (t) t->rotation = a3_quat_axis_angle(a3_v3(0, 1, 0), cc->yaw);
    /* camera child */
    for (A3Entity c = a3_entity_first_child(w, e); a3_entity_valid(w, c); c = a3_entity_next_sibling(w, c)) {
        if (!a3_component_has(w, c, A3_T_CAMERA)) continue;
        A3CTransform *ct = (A3CTransform *)a3_component_get(w, c, A3_T_TRANSFORM);
        if (!ct) continue;
        A3Quat pitch_q = a3_quat_axis_angle(a3_v3(1, 0, 0), cc->pitch);
        if (cc->camera_mode == A3_CAM_THIRD_PERSON) {
            A3Vec3 head = a3_v3(0, cc->eye_height, 0);
            A3Vec3 back = a3_quat_rotate(pitch_q, a3_v3(0, 0, cc->camera_distance));
            /* keep the camera out of walls */
            A3Vec3 head_w = a3_v3_add(a3_transform_world_position(w, e), head);
            A3Vec3 back_w = a3_quat_rotate(a3_quat_axis_angle(a3_v3(0, 1, 0), cc->yaw), back);
            A3RaycastHit hit;
            f32 dist = cc->camera_distance;
            if (a3_physics_raycast_ignore(w, head_w, back_w, dist, 0xFFFFFFFFu, e, &hit)) dist = a3_maxf(hit.distance - 0.2f, 0.3f);
            ct->position = a3_v3_add(head, a3_v3_scale(a3_v3_norm(back), dist));
        } else {
            ct->position = a3_v3(0, cc->eye_height, 0);
        }
        ct->rotation = pitch_q;
        break;
    }
}
