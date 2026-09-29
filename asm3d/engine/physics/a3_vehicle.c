/*
 * ASM3D - a3_vehicle.c
 */
#include "a3_vehicle.h"
#include "a3_physics.h"
#include "a3_physics_internal.h"
#include "../scene/a3_components.h"
#include "../core/a3_string.h"
#include "../core/a3_log.h"
#include "../core/a3_format.h"
#include "../world/a3_procmeshes.h"

u32 A3_T_VEHICLE = 0xFFFFFFFFu;

typedef b32 (*A3PhysBodyVisit)(const A3ShapeInstance *shape, const A3TriMesh *mesh, A3Entity e, b32 dynamic, void *user);
void a3__physics_visit_near(A3World *w, A3Aabb box, A3Entity ignore, A3PhysBodyVisit fn, void *user);

#define GRAVITY 20.0f     /* a bit stronger than real: arcade cars feel heavy */

void a3_vehicle_register(void) {
    if (A3_T_VEHICLE != 0xFFFFFFFFu) return;
    A3CVehicle v;
    a3_zero_struct(&v);
    v.max_speed = 52.0f; v.acceleration = 13.0f; v.braking = 28.0f; v.reverse_speed = 12.0f;
    v.steer_angle = 34.0f; v.grip = 0.85f; v.wheelbase = 2.7f; v.wheel_radius = 0.34f;
    v.body_size = a3_v3(1.8f, 1.0f, 4.4f); v.body_clearance = 0.42f;
    v.use_input = 0; v.ground_probe = 1; v.collide_world = 1; v.chase_camera = 1; v.camera_distance = 7.5f; v.camera_height = 2.6f;
    u32 t = a3_component_register("Vehicle", "Gameplay", sizeof(A3CVehicle), 16, &v, A3_COMP_BUILTIN,
        "An arcade car: throttle, brakes, steering, drifting with the handbrake, ground following and crashes. "
        "Player Controlled uses W/S (throttle/brake), A/D (steer) and Space (handbrake).");
    A3_T_VEHICLE = t;
    a3_component_type(t)->icon = "car";
    a3_component_require(t, "Transform");
    a3_field_range(A3_REFLECT_FIELD(t, A3CVehicle, max_speed, A3_FIELD_F32, "Top Speed", "Meters per second (50 m/s = 180 km/h)."), 1, 200, 0.5f);
    a3_field_range(A3_REFLECT_FIELD(t, A3CVehicle, acceleration, A3_FIELD_F32, "Acceleration", "How quickly the car picks up speed."), 0.5f, 100, 0.5f);
    a3_field_range(A3_REFLECT_FIELD(t, A3CVehicle, braking, A3_FIELD_F32, "Braking", "How quickly the brakes stop the car."), 0.5f, 200, 0.5f);
    a3_field_range(A3_REFLECT_FIELD(t, A3CVehicle, reverse_speed, A3_FIELD_F32, "Reverse Speed", "Top speed backwards."), 0, 60, 0.5f);
    a3_field_range(A3_REFLECT_FIELD(t, A3CVehicle, steer_angle, A3_FIELD_F32, "Steering", "Front wheel angle in degrees at low speed."), 1, 60, 0.5f);
    a3_field_range(A3_REFLECT_FIELD(t, A3CVehicle, grip, A3_FIELD_F32, "Grip", "1 = on rails, lower = slides in corners."), 0.05f, 1, 0.01f)->flags |= A3_FIELD_FLAG_SLIDER;
    A3_REFLECT_FIELD(t, A3CVehicle, use_input, A3_FIELD_BOOL, "Player Controlled", "Drive with W/A/S/D and Space. Off: scripts or traffic set Throttle and Steer.");
    A3_REFLECT_FIELD(t, A3CVehicle, chase_camera, A3_FIELD_BOOL, "Chase Camera", "The main camera follows behind while the player drives.");
    a3_field_range(A3_REFLECT_FIELD(t, A3CVehicle, camera_distance, A3_FIELD_F32, "Camera Distance", "Chase camera distance."), 1, 50, 0.1f);
    a3_field_range(A3_REFLECT_FIELD(t, A3CVehicle, camera_height, A3_FIELD_F32, "Camera Height", "Chase camera height."), 0, 20, 0.1f);
    A3_REFLECT_FIELD(t, A3CVehicle, ground_probe, A3_FIELD_BOOL, "Follow Ground", "Raycast the ground for height and slope. Off keeps the current height (cheap, flat roads).")->flags |= A3_FIELD_FLAG_ADVANCED;
    A3_REFLECT_FIELD(t, A3CVehicle, collide_world, A3_FIELD_BOOL, "Collisions", "Bump into walls and objects. Off for traffic cars that steer around things themselves.")->flags |= A3_FIELD_FLAG_ADVANCED;
    a3_field_range(A3_REFLECT_FIELD(t, A3CVehicle, wheelbase, A3_FIELD_F32, "Wheelbase", "Distance between the axles (m)."), 0.5f, 20, 0.05f)->flags |= A3_FIELD_FLAG_ADVANCED;
    a3_field_range(A3_REFLECT_FIELD(t, A3CVehicle, wheel_radius, A3_FIELD_F32, "Wheel Radius", "Used to spin the wheels."), 0.05f, 3, 0.01f)->flags |= A3_FIELD_FLAG_ADVANCED;
    A3_REFLECT_FIELD(t, A3CVehicle, body_size, A3_FIELD_VEC3, "Body Size", "Collision box (m).")->flags |= A3_FIELD_FLAG_ADVANCED;
    a3_field_range(A3_REFLECT_FIELD(t, A3CVehicle, body_clearance, A3_FIELD_F32, "Clearance", "Obstacles lower than this (curbs) are driven over."), 0, 3, 0.01f)->flags |= A3_FIELD_FLAG_ADVANCED;
    A3FieldDesc *f;
    f = A3_REFLECT_FIELD(t, A3CVehicle, throttle, A3_FIELD_F32, "Throttle", "-1 brake/reverse .. 1 full throttle (runtime input)."); f->flags |= A3_FIELD_FLAG_TRANSIENT; a3_field_range(f, -1, 1, 0.01f);
    f = A3_REFLECT_FIELD(t, A3CVehicle, steer, A3_FIELD_F32, "Steer", "-1 left .. 1 right (runtime input)."); f->flags |= A3_FIELD_FLAG_TRANSIENT; a3_field_range(f, -1, 1, 0.01f);
    A3_REFLECT_FIELD(t, A3CVehicle, handbrake, A3_FIELD_BOOL, "Handbrake", "Runtime input: drift.")->flags |= A3_FIELD_FLAG_TRANSIENT;
    A3_REFLECT_FIELD(t, A3CVehicle, speed, A3_FIELD_F32, "Speed", "Current speed (m/s, negative when reversing).")->flags |= A3_FIELD_FLAG_TRANSIENT | A3_FIELD_FLAG_READONLY;
    A3_REFLECT_FIELD(t, A3CVehicle, velocity, A3_FIELD_VEC3, "Velocity", "World velocity (m/s).")->flags |= A3_FIELD_FLAG_TRANSIENT | A3_FIELD_FLAG_READONLY;
    A3_REFLECT_FIELD(t, A3CVehicle, grounded, A3_FIELD_BOOL, "Grounded", "Wheels on the ground.")->flags |= A3_FIELD_FLAG_TRANSIENT | A3_FIELD_FLAG_READONLY;
    A3_REFLECT_FIELD(t, A3CVehicle, last_impact, A3_FIELD_F32, "Last Impact", "Speed of this step's collision (m/s), 0 when none.")->flags |= A3_FIELD_FLAG_TRANSIENT | A3_FIELD_FLAG_READONLY;
    A3_REFLECT_FIELD(t, A3CVehicle, damage, A3_FIELD_F32, "Damage", "Total of hard impacts; scripts may reset it.")->flags |= A3_FIELD_FLAG_TRANSIENT;
}

/* ---- collision: push the body box out of solid colliders ---- */

typedef struct CarHit {
    A3ShapeInstance box;
    A3Vec3 push;
    A3Vec3 normal;          /* strongest wall normal */
    f32 best;
    A3World *w;
    A3Vec3 velocity;
} CarHit;

static b32 car_visit(const A3ShapeInstance *shape, const A3TriMesh *mesh, A3Entity e, b32 dynamic, void *user) {
    CarHit *c = (CarHit *)user;
    A3Manifold m;
    b32 hit = mesh ? a3_collide_mesh(mesh, &c->box, &m) : a3_collide_convex(shape, &c->box, &m);
    if (!hit || !m.count) return 1;
    f32 depth = 0;
    for (u32 i = 0; i < m.count; ++i) depth = a3_maxf(depth, m.pts[i].depth);
    A3Vec3 n = m.normal;          /* from the collider towards the car */
    n.y = 0;                      /* walls push sideways; the ground rays handle height */
    f32 len = a3_v3_len(n);
    if (len < 0.3f || depth <= 0) return 1;
    n = a3_v3_scale(n, 1.0f / len);
    if (dynamic) {
        /* shove light objects out of the way instead of stopping */
        f32 into = -a3_v3_dot(c->velocity, n);
        if (into > 0) a3_physics_add_impulse(c->w, e, a3_v3_add(a3_v3_scale(n, -into * 40.0f), a3_v3(0, into * 8.0f, 0)));
        return 1;
    }
    c->push = a3_v3_add(c->push, a3_v3_scale(n, depth));
    if (depth > c->best) { c->best = depth; c->normal = n; }
    return 1;
}

static void make_box(A3ShapeInstance *s, A3Vec3 center, A3Quat rot, A3Vec3 half) {
    a3_zero_struct(s);
    s->type = A3_SHAPE_BOX;
    s->pos = center;
    s->rot = rot;
    s->rot_m = a3_mat4_from_quat(rot);
    s->half = half;
    A3Vec3 ext = a3_v3_zero();
    for (int i = 0; i < 3; ++i) {
        A3Vec3 axis = a3_v3(s->rot_m.m[i * 4 + 0], s->rot_m.m[i * 4 + 1], s->rot_m.m[i * 4 + 2]);
        f32 h = i == 0 ? half.x : i == 1 ? half.y : half.z;
        ext = a3_v3_add(ext, a3_v3(a3_absf(axis.x) * h, a3_absf(axis.y) * h, a3_absf(axis.z) * h));
    }
    s->aabb = a3_aabb(a3_v3_sub(center, ext), a3_v3_add(center, ext));
}

static f32 ground_at(A3World *w, A3Entity self, A3Vec3 p, f32 above, b32 *hit_out) {
    A3RaycastHit hit;
    A3Vec3 o = a3_v3(p.x, p.y + above, p.z);
    if (a3_physics_raycast_ignore(w, o, a3_v3(0, -1, 0), above + 3.0f, 0xFFFFFFFFu, self, &hit) && hit.normal.y > 0.5f) {
        *hit_out = 1;
        return hit.point.y;
    }
    *hit_out = 0;
    return p.y;
}

static void spin_wheels(A3World *w, A3Entity e, const A3CVehicle *v) {
    for (A3Entity c = a3_entity_first_child(w, e); a3_entity_valid(w, c); c = a3_entity_next_sibling(w, c)) {
        const char *name = a3_entity_name(w, c);
        if (!a3_str_starts_with(name, "Wheel")) continue;
        A3CTransform *t = a3_transform(w, c);
        if (!t) continue;
        b32 front = t->position.z < 0, left = t->position.x < 0;
        /* left wheels are turned around (rim face outward), so they spin the other way in local space */
        A3Quat spin = left ? a3_quat_mul(a3_quat_axis_angle(a3_v3(0, 1, 0), A3_PI), a3_quat_axis_angle(a3_v3(1, 0, 0), v->wheel_angle))
                           : a3_quat_axis_angle(a3_v3(1, 0, 0), -v->wheel_angle);
        t->rotation = front ? a3_quat_mul(a3_quat_axis_angle(a3_v3(0, 1, 0), -v->steer_current), spin) : spin;
    }
}

void a3_vehicle_step(A3World *w, A3Entity e, A3CVehicle *v, f32 dt) {
    A3CTransform *t = a3_transform(w, e);
    if (!t || dt <= 0) return;
    if (v->initialized && a3_v3_len_sq(a3_v3_sub(t->position, v->last_position)) > 9.0f) {
        v->initialized = 0;               /* teleported by a script: start fresh */
        v->vertical_speed = 0;
        v->pitch = 0;
    }
    if (!v->initialized) {
        /* heading from the placed rotation */
        A3Vec3 fwd = a3_quat_rotate(t->rotation, a3_v3(0, 0, -1));
        v->yaw = a3_atan2f(-fwd.x, -fwd.z);
        v->velocity = a3_v3_zero();
        v->speed = 0;
        v->initialized = 1;
    }
    f32 throttle = a3_clampf(v->throttle, -1, 1), steer_in = a3_clampf(v->steer, -1, 1);
    f32 vmax = a3_maxf(v->max_speed, 0.1f);
    f32 speed = v->speed;
    /* ---- longitudinal ---- */
    if (v->grounded || !v->ground_probe) {
        if (throttle > 0.01f) {
            if (speed < -0.5f) speed = a3_minf(speed + v->braking * throttle * dt, 0);
            else {
                f32 k = speed / vmax;
                speed += v->acceleration * throttle * a3_maxf(1.0f - k * k, 0.0f) * dt;
            }
        } else if (throttle < -0.01f) {
            if (speed > 0.5f) speed = a3_maxf(speed + v->braking * throttle * dt, 0);
            else speed = a3_maxf(speed + v->acceleration * 0.7f * throttle * dt, -v->reverse_speed);
        } else {
            f32 drag = (1.2f + 0.012f * speed * speed) * dt;          /* rolling + air */
            speed = a3_absf(speed) <= drag ? 0.0f : speed - a3_signf(speed) * drag;
        }
        if (v->handbrake) {
            f32 hb = v->braking * 0.45f * dt;
            speed = a3_absf(speed) <= hb ? 0.0f : speed - a3_signf(speed) * hb;
        }
    }
    /* ---- steering (narrower at speed, smoothed) ---- */
    f32 range = v->steer_angle * A3_DEG2RAD * (1.0f - 0.65f * a3_clampf(a3_absf(speed) / vmax, 0, 1));
    f32 target = steer_in * range;
    f32 rate = 3.5f * dt;
    v->steer_current += a3_clampf(target - v->steer_current, -rate, rate);
    if (v->grounded || !v->ground_probe) {
        f32 yaw_rate = speed * a3_tanf(v->steer_current) / a3_maxf(v->wheelbase, 0.5f);
        /* tires can only pull ~2 g sideways (arcade): limits the turn rate at speed */
        f32 max_rate = (14.0f + 8.0f * a3_clampf(v->grip, 0, 1)) / a3_maxf(a3_absf(speed), 1.0f);
        yaw_rate = a3_clampf(yaw_rate, -max_rate, max_rate);
        if (v->handbrake) yaw_rate *= 1.5f;
        v->yaw = a3_wrap_angle(v->yaw - yaw_rate * dt);   /* steer right = clockwise from above */
    }
    A3Vec3 fwd = a3_v3(-a3_sinf(v->yaw), 0, -a3_cosf(v->yaw));
    /* ---- grip: velocity follows the heading ---- */
    A3Vec3 vh = a3_v3(v->velocity.x, 0, v->velocity.z);
    A3Vec3 want = a3_v3_scale(fwd, speed);
    if (v->grounded || !v->ground_probe) {
        /* forward motion comes from the speed; sideways sliding dies out at the grip rate */
        f32 g = a3_clampf(v->grip, 0.02f, 1.0f) * (v->handbrake ? 0.22f : 1.0f);
        A3Vec3 lateral = a3_v3_sub(vh, a3_v3_scale(fwd, a3_v3_dot(vh, fwd)));
        vh = a3_v3_add(want, a3_v3_scale(lateral, a3_expf(-g * 14.0f * dt)));
    }
    /* ---- vertical ---- */
    A3Vec3 pos = t->position;
    pos = a3_v3_add(pos, a3_v3_scale(vh, dt));
    f32 half_wb = a3_maxf(v->wheelbase, 0.5f) * 0.5f;
    if (v->ground_probe) {
        b32 hf, hr;
        A3Vec3 pf = a3_v3_add(pos, a3_v3_scale(fwd, half_wb)), pr = a3_v3_sub(pos, a3_v3_scale(fwd, half_wb));
        f32 above = 1.2f + a3_maxf(-v->vertical_speed * dt, 0.0f);
        f32 yf = ground_at(w, e, pf, above, &hf), yr = ground_at(w, e, pr, above, &hr);
        f32 gy = hf && hr ? (yf + yr) * 0.5f : hf ? yf : hr ? yr : pos.y - 100.0f;
        v->vertical_speed -= GRAVITY * dt;
        f32 ny = pos.y + v->vertical_speed * dt;
        if ((hf || hr) && ny <= gy + 0.05f) {
            /* on the ground: snap (smoothly up curbs), follow the slope */
            pos.y = ny < gy ? a3_lerpf(a3_maxf(pos.y, ny), gy, a3_minf(1.0f, dt * 25.0f)) : gy;
            if (pos.y < gy - 0.3f) pos.y = gy - 0.3f;
            v->vertical_speed = a3_maxf(v->vertical_speed, 0.0f) * 0.0f;
            v->grounded = 1;
            f32 target_pitch = hf && hr ? a3_atan2f(yf - yr, half_wb * 2.0f) : v->pitch;
            v->pitch = a3_lerpf(v->pitch, target_pitch, a3_minf(1.0f, dt * 12.0f));
        } else {
            pos.y = ny;
            v->grounded = 0;
            v->pitch = a3_lerpf(v->pitch, a3_clampf(v->vertical_speed * 0.02f, -0.35f, 0.35f), a3_minf(1.0f, dt * 2.0f));
        }
        if (pos.y < -60.0f) { pos.y = 2.0f; v->vertical_speed = 0; vh = a3_v3_zero(); speed = 0; }  /* fell out of the world */
    } else {
        v->grounded = 1;
        v->vertical_speed = 0;
    }
    /* ---- collisions ---- */
    v->last_impact = 0;
    A3Quat rot = a3_quat_mul(a3_quat_axis_angle(a3_v3(0, 1, 0), v->yaw), a3_quat_axis_angle(a3_v3(1, 0, 0), v->pitch));
    A3Vec3 half = a3_v3(v->body_size.x * 0.5f, a3_maxf(v->body_size.y * 0.5f - v->body_clearance * 0.5f, 0.1f), v->body_size.z * 0.5f);
    for (int iter = 0; iter < 2 && v->collide_world; ++iter) {
        CarHit ch;
        a3_zero_struct(&ch);
        ch.w = w;
        ch.velocity = vh;
        A3Vec3 center = a3_v3_add(pos, a3_quat_rotate(rot, a3_v3(0, v->body_clearance + half.y, 0)));
        make_box(&ch.box, center, rot, half);
        a3__physics_visit_near(w, ch.box.aabb, e, car_visit, &ch);
        if (ch.best <= 0) break;
        A3Vec3 push = ch.push;
        f32 pl = a3_v3_len(push);
        if (pl > 1.5f) push = a3_v3_scale(push, 1.5f / pl);
        pos = a3_v3_add(pos, push);
        f32 into = -a3_v3_dot(vh, ch.normal);
        if (into > 0) {
            vh = a3_v3_add(vh, a3_v3_scale(ch.normal, into * 1.25f));     /* a little bounce */
            vh = a3_v3_scale(vh, 0.92f);
            v->last_impact = a3_maxf(v->last_impact, into);
            if (into > 4.0f) v->damage += into;
            speed = a3_v3_dot(vh, fwd);
        }
    }
    v->speed = speed;
    v->velocity = a3_v3(vh.x, v->ground_probe ? v->vertical_speed : 0.0f, vh.z);
    v->wheel_angle = a3_wrap_angle(v->wheel_angle + speed * dt / a3_maxf(v->wheel_radius, 0.05f));
    t->position = pos;
    t->rotation = rot;
    v->last_position = pos;
    A3CRigidBody *rb = (A3CRigidBody *)a3_component_get(w, e, A3_T_RIGIDBODY);
    if (rb && rb->type == A3_BODY_KINEMATIC) rb->velocity = v->velocity;
    spin_wheels(w, e, v);
}

/* 0 by day .. 1 at night, from the brightest directional light (the sun) */
static f32 night_factor(A3World *w) {
    u32 n = 0;
    const A3Entity *ents = 0;
    const A3CLight *ls = (const A3CLight *)a3_component_array(w, A3_T_LIGHT, &n, &ents);
    f32 sun = 0;
    for (u32 i = 0; i < n; ++i)
        if (ls[i].type == A3_LIGHT_DIRECTIONAL && a3_entity_active(w, ents[i]))
            sun = a3_maxf(sun, ls[i].intensity * (ls[i].color.x + ls[i].color.y + ls[i].color.z) / 3.0f);
    return 1.0f - a3_smoothstep(0.15f, 0.8f, sun);
}

static void set_headlights(A3World *w, A3Entity car, f32 night) {
    for (A3Entity c = a3_entity_first_child(w, car); a3_entity_valid(w, c); c = a3_entity_next_sibling(w, c)) {
        if (!a3_streq(a3_entity_name(w, c), "Headlights")) continue;
        A3CLight *l = (A3CLight *)a3_component_get(w, c, A3_T_LIGHT);
        if (l) l->intensity = 22.0f * night;     /* daytime running lights do not light the road */
    }
}

void a3_vehicle_update_all(A3World *w, f32 dt) {
    if (A3_T_VEHICLE == 0xFFFFFFFFu) return;
    u32 n = 0;
    const A3Entity *ents = 0;
    f32 night = night_factor(w);
    a3_component_array(w, A3_T_VEHICLE, &n, &ents);
    for (u32 i = 0; i < n; ++i) set_headlights(w, ents[i], night);
    for (u32 i = 0; i < n; ++i) {
        A3Entity e = ents[i];
        if (!a3_entity_active(w, e)) continue;
        A3CVehicle *v = (A3CVehicle *)a3_component_get(w, e, A3_T_VEHICLE);
        if (v) a3_vehicle_step(w, e, v, dt);
    }
}

A3Entity a3_vehicle_find_player(A3World *w) {
    if (A3_T_VEHICLE == 0xFFFFFFFFu) return A3_ENTITY_NULL;
    u32 n = 0;
    const A3Entity *ents = 0;
    A3CVehicle *vs = (A3CVehicle *)a3_component_array(w, A3_T_VEHICLE, &n, &ents);
    for (u32 i = 0; i < n; ++i) if (vs[i].use_input && a3_entity_active(w, ents[i])) return ents[i];
    return A3_ENTITY_NULL;
}

void a3_vehicle_update_camera(A3World *w, A3Entity vehicle, const A3CVehicle *v, A3Entity camera, f32 dt) {
    A3CTransform *ct = a3_transform(w, camera);
    A3CTransform *vt = a3_transform(w, vehicle);
    if (!ct || !vt || a3_entity_valid(w, a3_entity_parent(w, camera))) return;
    A3Vec3 fwd = a3_v3(-a3_sinf(v->yaw), 0, -a3_cosf(v->yaw));
    /* look a little into the direction of travel when sliding */
    A3Vec3 vel = a3_v3(v->velocity.x, 0, v->velocity.z);
    f32 sp = a3_v3_len(vel);
    A3Vec3 dir = sp > 3.0f ? a3_v3_norm(a3_v3_lerp(fwd, a3_v3_scale(vel, 1.0f / sp), 0.35f)) : fwd;
    if (v->speed < -2.0f) dir = fwd;
    f32 dist = v->camera_distance + a3_clampf(a3_absf(v->speed) * 0.05f, 0, 2.5f);
    A3Vec3 target = a3_v3_add(vt->position, a3_v3(0, 1.2f, 0));
    A3Vec3 want = a3_v3_add(a3_v3_sub(vt->position, a3_v3_scale(dir, dist)), a3_v3(0, v->camera_height, 0));
    /* keep the camera out of buildings */
    A3Vec3 to = a3_v3_sub(want, target);
    f32 tl = a3_v3_len(to);
    A3RaycastHit hit;
    if (tl > 0.1f && a3_physics_raycast_ignore(w, target, a3_v3_scale(to, 1.0f / tl), tl, 0xFFFFFFFFu, vehicle, &hit))
        want = a3_v3_add(target, a3_v3_scale(to, a3_maxf(hit.distance - 0.4f, 0.8f) / tl));
    f32 k = 1.0f - a3_expf(-dt * 6.0f);
    ct->position = a3_v3_lerp(ct->position, want, k);
    A3Vec3 look = a3_v3_sub(a3_v3_add(target, a3_v3_scale(fwd, 4.0f)), ct->position);
    if (a3_v3_len_sq(look) > 1e-4f) ct->rotation = a3_quat_look_rotation(a3_v3_norm(look), a3_v3(0, 1, 0));
}

/* ---- car prefab ---- */

static A3Entity part(A3World *w, A3Entity parent, const char *name, A3Vec3 pos, A3Vec3 scale, const char *mesh_path, i32 prim, const char *material, A3Vec4 color) {
    A3Entity e = a3_entity_create(w, name);
    A3CTransform *t = (A3CTransform *)a3_component_add(w, e, A3_T_TRANSFORM);
    t->position = pos;
    t->scale = scale;
    a3_entity_set_parent(w, e, parent);
    A3CMeshRenderer *mr = (A3CMeshRenderer *)a3_component_add(w, e, A3_T_MESH_RENDERER);
    mr->primitive = prim;
    mr->base_color = color;
    if (mesh_path) a3_strcpy(mr->mesh.path, sizeof(mr->mesh.path), mesh_path);
    if (material) a3_strcpy(mr->material.path, sizeof(mr->material.path), material);
    return e;
}

A3Entity a3_vehicle_spawn_car_style(A3World *w, const char *name, A3Vec3 ground_pos, f32 yaw_deg, A3Vec4 paint, const char *style) {
    a3_vehicle_register();
    char mesh[64];
    a3_snprintf(mesh, sizeof(mesh), "builtin:car_%s", style && *style ? style : "sedan");
    f32 wb, track, wr, len, width, height;
    a3_procmodels_car_info(mesh, &wb, &track, &wr, &len, &width, &height);
    A3Entity car = a3_entity_create(w, name ? name : "Car");
    A3CTransform *t = (A3CTransform *)a3_component_add(w, car, A3_T_TRANSFORM);
    t->position = ground_pos;
    t->rotation = a3_quat_axis_angle(a3_v3(0, 1, 0), yaw_deg * A3_DEG2RAD);
    A3CMeshRenderer *mr = (A3CMeshRenderer *)a3_component_add(w, car, A3_T_MESH_RENDERER);
    mr->primitive = A3_PRIM_NONE;
    mr->base_color = paint;
    a3_strcpy(mr->mesh.path, sizeof(mr->mesh.path), mesh);
    a3_strcpy(mr->material.path, sizeof(mr->material.path), "builtin:carbody");
    A3CCollider *c = (A3CCollider *)a3_component_add(w, car, A3_T_COLLIDER);
    c->shape = A3_SHAPE_BOX;
    c->size = a3_v3(width, height - 0.2f, len);
    c->center = a3_v3(0, 0.2f + (height - 0.2f) * 0.5f, 0);
    A3CRigidBody *rb = (A3CRigidBody *)a3_component_add(w, car, A3_T_RIGIDBODY);
    rb->type = A3_BODY_KINEMATIC;
    rb->mass = 1300.0f;
    rb->lock_rotation = 1;
    rb->gravity_scale = 0;
    A3CVehicle *v = (A3CVehicle *)a3_component_add(w, car, A3_T_VEHICLE);
    if (v) {
        v->wheelbase = wb;
        v->wheel_radius = wr;
        v->body_size = a3_v3(width, height - 0.3f, len);
    }
    static const char *const names[4] = { "Wheel FL", "Wheel FR", "Wheel RL", "Wheel RR" };
    for (int i = 0; i < 4; ++i) {
        f32 x = (i & 1) ? track : -track, z = i < 2 ? -wb * 0.5f : wb * 0.5f;
        A3Entity wh = part(w, car, names[i], a3_v3(x, wr, z), a3_v3s(wr), "builtin:wheel_detailed", A3_PRIM_NONE, "builtin:wheel", a3_v4(1, 1, 1, 1));
        if (x < 0) a3_transform(w, wh)->rotation = a3_quat_axis_angle(a3_v3(0, 1, 0), A3_PI);  /* rim face outward */
    }
    /* headlights: one spot light lighting the road ahead */
    A3Entity hl = a3_entity_create(w, "Headlights");
    A3CTransform *ht = (A3CTransform *)a3_component_add(w, hl, A3_T_TRANSFORM);
    ht->position = a3_v3(0, 0.75f, -len * 0.5f - 0.1f);
    ht->rotation = a3_quat_axis_angle(a3_v3(1, 0, 0), -8.0f * A3_DEG2RAD);
    a3_entity_set_parent(w, hl, car);
    A3CLight *l = (A3CLight *)a3_component_add(w, hl, A3_T_LIGHT);
    if (l) { l->type = A3_LIGHT_SPOT; l->color = a3_v4(1.0f, 0.93f, 0.82f, 1); l->intensity = 22.0f * night_factor(w); l->range = 32.0f; l->spot_angle = 64.0f; }
    return car;
}

A3Entity a3_vehicle_spawn_car(A3World *w, const char *name, A3Vec3 ground_pos, f32 yaw_deg, A3Vec4 paint) {
    return a3_vehicle_spawn_car_style(w, name, ground_pos, yaw_deg, paint, "sedan");
}
