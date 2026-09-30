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
#include "a3_vehicle_kernels.h"
#include "../audio/a3_audio.h"

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
    v.physics_model = A3_VEHICLE_REALISTIC; v.drive = A3_DRIVE_REAR; v.mass = 1400.0f; v.engine_torque = 340.0f; v.redline = 6800.0f;
    v.suspension_travel = 0.28f; v.suspension_hz = 1.6f; v.traction_control = 1;
    u32 t = a3_component_register("Vehicle", "Gameplay", sizeof(A3CVehicle), 16, &v, A3_COMP_BUILTIN,
        "A car. Realistic: rigid body on raycast suspension, tire slip and grip limits, engine, gearbox and weight transfer. "
        "Arcade: simple and forgiving. Player Controlled uses W/S (throttle/brake), A/D (steer) and Space (handbrake).");
    A3_T_VEHICLE = t;
    a3_component_type(t)->icon = "car";
    a3_component_require(t, "Transform");
    a3_field_range(A3_REFLECT_FIELD(t, A3CVehicle, max_speed, A3_FIELD_F32, "Top Speed", "Meters per second (50 m/s = 180 km/h)."), 1, 200, 0.5f);
    a3_field_range(A3_REFLECT_FIELD(t, A3CVehicle, acceleration, A3_FIELD_F32, "Acceleration", "How quickly the car picks up speed."), 0.5f, 100, 0.5f);
    a3_field_range(A3_REFLECT_FIELD(t, A3CVehicle, braking, A3_FIELD_F32, "Braking", "How quickly the brakes stop the car."), 0.5f, 200, 0.5f);
    a3_field_range(A3_REFLECT_FIELD(t, A3CVehicle, reverse_speed, A3_FIELD_F32, "Reverse Speed", "Top speed backwards."), 0, 60, 0.5f);
    a3_field_range(A3_REFLECT_FIELD(t, A3CVehicle, steer_angle, A3_FIELD_F32, "Steering", "Front wheel angle in degrees at low speed."), 1, 60, 0.5f);
    a3_field_range(A3_REFLECT_FIELD(t, A3CVehicle, grip, A3_FIELD_F32, "Grip", "Tire grip. Realistic: friction 0.6 + 0.6 x Grip (0.85 = sports tires). Arcade: 1 = on rails."), 0.05f, 1, 0.01f)->flags |= A3_FIELD_FLAG_SLIDER;
    {
        static const char *const models[2] = { "Realistic", "Arcade" };
        static const char *const drives[3] = { "Rear Wheels", "Front Wheels", "All Wheels" };
        A3FieldDesc *fd = A3_REFLECT_FIELD(t, A3CVehicle, physics_model, A3_FIELD_ENUM, "Physics", "Realistic: suspension, tire slip, engine and gearbox. Arcade: simple and forgiving (AI traffic).");
        if (fd) { fd->enum_names = models; fd->enum_count = 2; }
        fd = A3_REFLECT_FIELD(t, A3CVehicle, drive, A3_FIELD_ENUM, "Drive", "Driven wheels (Realistic). Rear wheel drive can slide the tail with the throttle.");
        if (fd) { fd->enum_names = drives; fd->enum_count = 3; }
    }
    a3_field_range(A3_REFLECT_FIELD(t, A3CVehicle, mass, A3_FIELD_F32, "Mass", "Kilograms (Realistic)."), 100, 40000, 10);
    a3_field_range(A3_REFLECT_FIELD(t, A3CVehicle, engine_torque, A3_FIELD_F32, "Engine Torque", "Newton meters at the torque peak (Realistic)."), 20, 3000, 5);
    a3_field_range(A3_REFLECT_FIELD(t, A3CVehicle, redline, A3_FIELD_F32, "Redline", "Maximum engine rpm (Realistic)."), 2000, 12000, 100)->flags |= A3_FIELD_FLAG_ADVANCED;
    a3_field_range(A3_REFLECT_FIELD(t, A3CVehicle, suspension_travel, A3_FIELD_F32, "Suspension Travel", "Meters (Realistic)."), 0.05f, 1.0f, 0.01f)->flags |= A3_FIELD_FLAG_ADVANCED;
    a3_field_range(A3_REFLECT_FIELD(t, A3CVehicle, suspension_hz, A3_FIELD_F32, "Suspension Stiffness", "Natural frequency in Hz: 1.2 soft (SUV) .. 2.5 stiff (race car)."), 0.5f, 5.0f, 0.05f)->flags |= A3_FIELD_FLAG_ADVANCED;
    A3_REFLECT_FIELD(t, A3CVehicle, traction_control, A3_FIELD_BOOL, "Stability Control", "Stability and traction control: cuts power when the driven wheels spin and catches the tail when the car starts to spin (Realistic). Off with the handbrake.")->flags |= A3_FIELD_FLAG_ADVANCED;
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
    A3_REFLECT_FIELD(t, A3CVehicle, rpm, A3_FIELD_F32, "Rpm", "Engine speed (Realistic).")->flags |= A3_FIELD_FLAG_TRANSIENT | A3_FIELD_FLAG_READONLY;
    A3_REFLECT_FIELD(t, A3CVehicle, gear, A3_FIELD_I32, "Gear", "-1 reverse, 1..6 (Realistic).")->flags |= A3_FIELD_FLAG_TRANSIENT | A3_FIELD_FLAG_READONLY;
    A3_REFLECT_FIELD(t, A3CVehicle, wheel_slip, A3_FIELD_F32, "Wheel Slip", "0 gripping .. 1 sliding (Realistic).")->flags |= A3_FIELD_FLAG_TRANSIENT | A3_FIELD_FLAG_READONLY;
    A3_REFLECT_FIELD(t, A3CVehicle, roll, A3_FIELD_F32, "Body Roll", "Radians (Realistic).")->flags |= A3_FIELD_FLAG_TRANSIENT | A3_FIELD_FLAG_READONLY;
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

/* Pushes the body box out of solid colliders (sideways only; the ground is
 * handled by the wheels). Removes the velocity into walls (with a little
 * bounce), records the impact. Returns true when something was hit. */
static b32 collide_body(A3World *w, A3Entity e, A3CVehicle *v, A3Vec3 *pos, A3Quat rot, A3Vec3 *vel) {
    b32 any = 0;
    A3Vec3 half = a3_v3(v->body_size.x * 0.5f, a3_maxf(v->body_size.y * 0.5f - v->body_clearance * 0.5f, 0.1f), v->body_size.z * 0.5f);
    for (int iter = 0; iter < 2 && v->collide_world; ++iter) {
        CarHit ch;
        a3_zero_struct(&ch);
        ch.w = w;
        ch.velocity = *vel;
        A3Vec3 center = a3_v3_add(*pos, a3_quat_rotate(rot, a3_v3(0, v->body_clearance + half.y, 0)));
        make_box(&ch.box, center, rot, half);
        a3__physics_visit_near(w, ch.box.aabb, e, car_visit, &ch);
        if (ch.best <= 0) break;
        A3Vec3 push = ch.push;
        f32 pl = a3_v3_len(push);
        if (pl > 1.5f) push = a3_v3_scale(push, 1.5f / pl);
        *pos = a3_v3_add(*pos, push);
        f32 into = -a3_v3_dot(*vel, ch.normal);
        if (into > 0) {
            *vel = a3_v3_add(*vel, a3_v3_scale(ch.normal, into * 1.25f));     /* a little bounce */
            *vel = a3_v3_scale(*vel, 0.92f);
            v->last_impact = a3_maxf(v->last_impact, into);
            if (into > 4.0f) v->damage += into;
            any = 1;
        }
    }
    return any;
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

static void sim_step_cars(A3World *w, const A3Entity *ents, u32 count, f32 dt);

void a3_vehicle_step(A3World *w, A3Entity e, A3CVehicle *v, f32 dt) {
    A3CTransform *t = a3_transform(w, e);
    if (!t || dt <= 0) return;
    if (v->physics_model == A3_VEHICLE_REALISTIC) { sim_step_cars(w, &e, 1, dt); return; }
    v->sim_ready = 0;
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
    if (collide_body(w, e, v, &pos, rot, &vh)) speed = a3_v3_dot(vh, fwd);
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

/* ======================================================================== */
/* Realistic model: rigid body on raycast wheels                            */
/* ======================================================================== */

#define SIM_SUBSTEPS 4
#define SIM_G 9.81f
#define IDLE_RPM 850.0f
#define CG_HEIGHT 0.55f           /* center of mass above the ground (m) */
#define ROLL_CENTER 0.35f         /* tire forces act this fraction of the way up to the CG */
static const f32 k_gears[7] = { 3.3f, 3.6f, 2.19f, 1.51f, 1.13f, 0.91f, 0.76f };   /* [0] = reverse */
#define TOP_GEAR 6

/* engine torque as a fraction of the peak: flat-ish middle, softer at idle and near the redline */
static f32 torque_curve(f32 rpm, f32 redline) {
    f32 d = rpm / redline - 0.62f;
    return a3_clampf(1.0f - d * d * 1.6f, 0.35f, 1.0f);
}

static f32 gear_ratio(i32 gear) { return gear < 0 ? k_gears[0] : k_gears[A3_CLAMP(gear, 1, TOP_GEAR)]; }

static b32 is_driven(const A3CVehicle *v, u32 wheel) {
    return v->drive == A3_DRIVE_ALL || (v->drive == A3_DRIVE_FRONT ? wheel < 2 : wheel >= 2);
}

static f32 sim_mass(const A3CVehicle *v) { return a3_maxf(v->mass, 50.0f); }
static f32 sim_stiffness(const A3CVehicle *v) { f32 wn = A3_TAU * a3_clampf(v->suspension_hz, 0.3f, 6.0f); return sim_mass(v) * 0.25f * wn * wn; }
static f32 sim_travel(const A3CVehicle *v) { return a3_clampf(v->suspension_travel, 0.05f, 1.0f); }

static void sim_init(A3World *w, A3Entity e, A3CVehicle *v, const A3CTransform *t) {
    v->orientation = a3_quat_normalize(t->rotation);
    v->velocity = a3_v3_zero();
    v->angular_velocity = a3_v3_zero();
    v->gear = 1;
    v->rpm = IDLE_RPM;
    v->shift_timer = 0;
    v->speed = 0;
    f32 wr = a3_maxf(v->wheel_radius, 0.1f);
    f32 hx = a3_maxf(v->body_size.x * 0.5f - 0.25f, 0.3f), hz = a3_maxf(v->wheelbase, 0.5f) * 0.5f;
    for (u32 i = 0; i < 4; ++i) v->hardpoint[i] = a3_v3((i & 1) ? hx : -hx, wr, i < 2 ? -hz : hz);
    for (A3Entity c = a3_entity_first_child(w, e); a3_entity_valid(w, c); c = a3_entity_next_sibling(w, c)) {
        if (!a3_str_starts_with(a3_entity_name(w, c), "Wheel")) continue;
        const A3CTransform *ct = a3_transform(w, c);
        if (!ct) continue;
        u32 idx = (ct->position.z < 0 ? 0u : 2u) + (ct->position.x < 0 ? 0u : 1u);
        v->hardpoint[idx] = a3_v3(ct->position.x, wr, ct->position.z);
    }
    f32 m = sim_mass(v), k = sim_stiffness(v), L = sim_travel(v);
    f32 sag = a3_minf(m * SIM_G * 0.25f / k, L * 0.8f);        /* static compression */
    v->top_y = wr + L - sag;                                     /* at rest the origin is on the ground */
    for (u32 i = 0; i < 4; ++i) v->comp[i] = sag;
    v->sim_ready = 1;
}

/* gearing and drag follow Top Speed, Engine Torque and Mass (scripts may
 * change them at any time) */
static void sim_derive(A3CVehicle *v) {
    f32 wr = a3_maxf(v->wheel_radius, 0.1f), m = sim_mass(v);
    f32 vmax = a3_maxf(v->max_speed, 5.0f), redline = a3_maxf(v->redline, 2000.0f);
    f32 wmax = redline * A3_TAU / 60.0f;
    /* top gear at the redline is a little above Top Speed */
    v->final_drive = wmax * wr / (vmax * 1.06f * k_gears[TOP_GEAR]);
    /* drag area: the engine's power (minus rolling resistance) is used up at Top Speed */
    f32 power = v->engine_torque * torque_curve(redline * 0.9f, redline) * wmax * 0.9f * 0.85f;
    f32 avail = a3_maxf(power - 0.012f * m * SIM_G * vmax, power * 0.2f);
    v->drag_area = avail / (0.6f * vmax * vmax * vmax);
}

typedef struct SimCar {
    A3Entity e;
    A3CVehicle *v;
    A3CTransform *t;
    A3Vec3 pos;                 /* origin (on the ground between the wheels) */
    A3Vec3 contact[4], normal[4], fwd_g[4], lat_g[4];
    b32 hit[4];
    f32 raw_comp[4];
    f32 drive;                  /* engine force at the driven wheels (N, signed) */
    f32 brake;                  /* 0..1 */
    b32 lock_rear;
    f32 k, c, L, wr, m, mu;
    f32 load[4];                /* last substep's wheel loads (ABS, load sensitivity) */
    f32 cg_fwd;                 /* m */
    f32 front_share;            /* static weight on the front axle (0..1) */
    f32 fy[4];                  /* last substep's lateral tire forces (traction control) */
    f32 esc;                    /* 0..1 stability control intervention (cuts the throttle) */
} SimCar;

/* per frame, before the substeps: steering, gearbox, engine */
static void sim_frame_inputs(SimCar *sc, f32 dt) {
    A3CVehicle *v = sc->v;
    f32 throttle = a3_clampf(v->throttle, -1, 1), steer_in = a3_clampf(v->steer, -1, 1);
    f32 vmax = a3_maxf(v->max_speed, 0.1f), vf = v->speed;
    /* speed-sensitive steering: at speed full lock asks for about the angle
       of a 1 g turn plus the tires' peak slip angle, never past it */
    f32 v2 = a3_maxf(vf * vf, 1.0f);
    f32 range = a3_minf(v->steer_angle * A3_DEG2RAD, a3_atanf(a3_maxf(v->wheelbase, 0.5f) * SIM_G * sc->mu / v2) + 0.12f);
    f32 rate = 3.0f * dt;
    A3_UNUSED(vmax);
    v->steer_current += a3_clampf(steer_in * range - v->steer_current, -rate, rate);
    /* direction: holding brake at a stop selects reverse, throttle selects first */
    if (v->gear > 0 && throttle < -0.05f && vf < 0.8f) v->gear = -1;
    else if (v->gear < 0 && throttle > 0.05f && vf > -0.8f) v->gear = 1;
    b32 rev = v->gear < 0;
    f32 accel = rev ? a3_maxf(-throttle, 0) : a3_maxf(throttle, 0);
    f32 brake = rev ? a3_maxf(throttle, 0) : a3_maxf(-throttle, 0);
    f32 redline = a3_maxf(v->redline, 2000.0f);
    f32 ratio = gear_ratio(v->gear) * v->final_drive;
    f32 wheel_rpm = a3_absf(vf) / sc->wr * ratio * (60.0f / A3_TAU);
    /* clutch: at pull-away the engine revs up with the throttle */
    f32 target = (v->gear == 1 || rev) && accel > 0.05f ? a3_maxf(wheel_rpm, IDLE_RPM + accel * 0.45f * redline) : a3_maxf(wheel_rpm, IDLE_RPM);
    v->rpm += (a3_minf(target, redline * 1.02f) - v->rpm) * a3_minf(1.0f, dt * 10.0f);
    /* automatic gearbox */
    v->shift_timer -= dt;
    if (!rev && v->shift_timer <= 0) {
        if (wheel_rpm > 0.93f * redline && v->gear < TOP_GEAR && accel > 0.1f) { v->gear++; v->shift_timer = 0.22f; }
        else if (v->gear > 1 && wheel_rpm * gear_ratio(v->gear - 1) / gear_ratio(v->gear) < (accel > 0.6f ? 0.8f : 0.55f) * redline &&
                 wheel_rpm < 0.42f * redline) { v->gear--; v->shift_timer = 0.18f; }
    }
    f32 torque = v->engine_torque * torque_curve(v->rpm, redline) * accel;
    if (v->rpm >= redline * 0.995f || v->shift_timer > 0) torque = 0;         /* limiter, shifting */
    f32 drive = torque * ratio * 0.85f / sc->wr * (rev ? -1.0f : 1.0f);
    if (accel < 0.05f && !rev && v->gear > 0) {                                  /* engine braking */
        f32 eb = 0.14f * v->engine_torque * ratio / sc->wr * a3_clampf((v->rpm - IDLE_RPM) / redline, 0, 1);
        drive -= a3_signf(vf) * a3_minf(eb, a3_absf(vf) * 400.0f);
    }
    f32 slip = 0;
    for (u32 i = 0; i < 4; ++i) if (is_driven(v, i)) slip = a3_maxf(slip, v->slip_w[i]);
    if (v->traction_control && slip > 0.3f && accel > 0) drive *= 0.5f;
    if (accel < 0.05f && brake < 0.05f && a3_absf(vf) < 0.4f) brake = 0.3f;    /* parked: hold */
    sc->drive = drive;
    sc->brake = brake;
    sc->lock_rear = v->handbrake && a3_absf(vf) > 1.5f;
    if (v->handbrake && !sc->lock_rear) sc->brake = a3_maxf(sc->brake, 0.5f);
}

static A3Vec3 rot_v(A3Quat q, f32 x, f32 y, f32 z) { return a3_quat_rotate(q, a3_v3(x, y, z)); }

/* per substep: rays and wheel inputs for the kernel */
static void sim_prepare(A3World *w, SimCar *sc, A3WheelQuad *q, f32 dts) {
    A3CVehicle *v = sc->v;
    A3Quat r = v->orientation;
    A3Vec3 up = rot_v(r, 0, 1, 0), fwd = rot_v(r, 0, 0, -1), right = rot_v(r, 1, 0, 0);
    A3Vec3 cg = a3_v3_add(sc->pos, rot_v(r, 0, CG_HEIGHT, -sc->cg_fwd));
    f32 per_wheel = sc->m * 0.25f;
    u32 nd = v->drive == A3_DRIVE_ALL ? 4u : 2u;
    f32 brake_total = sc->m * SIM_G * 1.25f * sc->brake;
    for (u32 i = 0; i < 4; ++i) {
        A3Vec3 hp = v->hardpoint[i];
        A3Vec3 top = a3_v3_add(sc->pos, rot_v(r, hp.x, v->top_y, hp.z));
        f32 len = sc->L + sc->wr;
        A3RaycastHit hit;
        b32 ok = v->ground_probe && a3_physics_raycast_ignore(w, top, a3_v3_neg(up), len + 0.05f, 0xFFFFFFFFu, sc->e, &hit) && hit.normal.y > 0.35f;
        if (!v->ground_probe) {
            /* flat ground at the origin's height */
            f32 d = up.y > 0.2f ? (top.y - sc->pos.y) / up.y : len + 1.0f;
            ok = d <= len + 0.05f;
            hit.distance = d;
            hit.point = a3_v3_sub(top, a3_v3_scale(up, d));
            hit.normal = a3_v3(0, 1, 0);
        }
        f32 comp = ok ? len - hit.distance : -1.0f;
        sc->hit[i] = ok;
        sc->raw_comp[i] = comp;
        sc->contact[i] = ok ? hit.point : a3_v3_sub(top, a3_v3_scale(up, len));
        sc->normal[i] = ok ? hit.normal : up;
        f32 c_in = a3_minf(comp, sc->L);
        f32 prev = v->comp[i];
        q->comp[i] = c_in;
        q->comp_vel[i] = ok && prev > 0 ? (c_in - prev) / dts : 0.0f;
        v->comp[i] = ok ? c_in : 0.0f;
        /* wheel axes on the ground plane */
        f32 st = i < 2 ? v->steer_current : 0.0f;
        A3Vec3 wf = a3_v3_add(a3_v3_scale(fwd, a3_cosf(st)), a3_v3_scale(right, a3_sinf(st)));
        A3Vec3 n = sc->normal[i];
        A3Vec3 fg = a3_v3_sub(wf, a3_v3_scale(n, a3_v3_dot(wf, n)));
        f32 fl = a3_v3_len(fg);
        fg = fl > 1e-4f ? a3_v3_scale(fg, 1.0f / fl) : fwd;
        A3Vec3 lg = a3_v3_cross(fg, n);
        sc->fwd_g[i] = fg;
        sc->lat_g[i] = lg;
        A3Vec3 vc = a3_v3_add(v->velocity, a3_v3_cross(v->angular_velocity, a3_v3_sub(sc->contact[i], cg)));
        f32 vl = a3_v3_dot(vc, fg);
        q->v_long[i] = vl;
        q->v_lat[i] = a3_v3_dot(vc, lg);
        /* springs sized to each axle's share of the weight, so the car sits level */
        f32 share = (i < 2 ? sc->front_share : 1.0f - sc->front_share) * 2.0f;
        q->stiffness[i] = sc->k * share;
        q->damping[i] = sc->c * share;
        /* load sensitivity: a more heavily loaded tire grips less per newton */
        f32 stat = sc->m * SIM_G * 0.5f * (i < 2 ? sc->front_share : 1.0f - sc->front_share);
        q->mu[i] = sc->mu * a3_clampf(1.0f - 0.12f * (sc->load[i] / stat - 1.0f), 0.75f, 1.1f);
        /* longitudinal request: engine + brakes + rolling resistance, never more than stops the wheel this substep */
        f32 stop = a3_absf(vl) * per_wheel / dts;
        f32 fb = brake_total * (i < 2 ? 0.64f : 0.36f) * 0.5f;
        fb = a3_minf(fb, 0.92f * sc->mu * sc->load[i]);            /* ABS: stay just under the grip limit */
        f32 fr = 0.012f * per_wheel * SIM_G;
        f32 drive = is_driven(v, i) ? sc->drive / (f32)nd * (1.0f - 0.7f * sc->esc) : 0.0f;
        if (v->traction_control && drive != 0.0f) {
            /* traction control: cornering grip first, the engine gets what is left */
            f32 lim = q->mu[i] * sc->load[i];
            f32 left = a3_sqrtf(a3_maxf(lim * lim - sc->fy[i] * sc->fy[i], 0.0f)) * 0.9f;
            drive = a3_clampf(drive, -left, left);
        }
        q->drive[i] = drive - a3_signf(vl) * a3_minf(fb + fr, stop);
        q->lock[i] = (i >= 2 && sc->lock_rear) ? 1.0f : 0.0f;
    }
}

/* per substep: apply the kernel's forces and integrate the rigid body */
static void sim_integrate(SimCar *sc, const A3WheelQuad *q, f32 dts) {
    A3CVehicle *v = sc->v;
    A3Quat r = v->orientation;
    A3Vec3 up = rot_v(r, 0, 1, 0), right = rot_v(r, 1, 0, 0), fwd = rot_v(r, 0, 0, -1);
    A3Vec3 cg = a3_v3_add(sc->pos, rot_v(r, 0, CG_HEIGHT, -sc->cg_fwd));
    A3Vec3 F = a3_v3(0, -SIM_G * sc->m, 0), T = a3_v3_zero();
    f32 excess = 0;
    A3Vec3 excess_n = up;
    for (u32 i = 0; i < 4; ++i) {
        v->slip_w[i] = q->slip[i];
        sc->load[i] = q->load[i];
        sc->fy[i] = q->fy[i];
        if (!sc->hit[i]) continue;
        f32 load = q->load[i];
        /* anti-roll bars: the more compressed side of each axle is pushed up */
        u32 o = i ^ 1u;
        f32 karb = sc->k * (i < 2 ? 0.55f : 0.2f);                 /* stiffer at the front: stable, mild understeer */
        if (sc->hit[o]) load += karb * (q->comp[i] - q->comp[o]);
        A3Vec3 fs = a3_v3_scale(up, a3_maxf(load, 0.0f));
        A3Vec3 ft = a3_v3_add(a3_v3_scale(sc->fwd_g[i], q->fx[i]), a3_v3_scale(sc->lat_g[i], q->fy[i]));
        A3Vec3 pc = sc->contact[i];
        A3Vec3 pt = a3_v3_add(pc, a3_v3_scale(up, CG_HEIGHT * ROLL_CENTER));
        F = a3_v3_add(F, a3_v3_add(fs, ft));
        T = a3_v3_add(T, a3_v3_add(a3_v3_cross(a3_v3_sub(pc, cg), fs), a3_v3_cross(a3_v3_sub(pt, cg), ft)));
        if (sc->raw_comp[i] > sc->L && sc->raw_comp[i] - sc->L > excess) { excess = sc->raw_comp[i] - sc->L; excess_n = sc->normal[i]; }
    }
    /* stability control: when the car rotates faster than the steering asks
       for (the tail is stepping out), a yaw moment like braking one outer
       wheel pulls it back. Off with the handbrake, so drifts still work. */
    f32 vfwd = a3_v3_dot(v->velocity, fwd);
    if (v->traction_control && !v->handbrake && a3_absf(vfwd) > 4.0f && (sc->hit[0] || sc->hit[1] || sc->hit[2] || sc->hit[3])) {
        f32 wy = a3_v3_dot(v->angular_velocity, up);
        f32 want = -vfwd * a3_tanf(v->steer_current) / a3_maxf(v->wheelbase, 0.5f);   /* steer right = negative yaw rate */
        f32 cap = sc->mu * SIM_G / a3_absf(vfwd);
        want = a3_clampf(want, -cap, cap);
        f32 excess_rate = wy - want;
        f32 over = a3_absf(wy) > a3_absf(want) ? excess_rate : 0.0f;
        sc->esc = a3_clampf((a3_absf(over) - 0.05f) * 4.0f, 0.0f, 1.0f);
        if (a3_absf(over) > 0.05f) {
            f32 tmax = 0.5f * sc->m * SIM_G * a3_maxf(v->body_size.x, 1.0f) * 0.5f;
            T = a3_v3_sub(T, a3_v3_scale(up, a3_clampf(over * sc->m * 12.0f, -tmax, tmax)));
        }
    } else {
        sc->esc = 0;
    }
    /* aerodynamic drag */
    f32 sp = a3_v3_len(v->velocity);
    F = a3_v3_sub(F, a3_v3_scale(v->velocity, 0.6f * v->drag_area * sp));
    /* integrate (semi-implicit Euler) */
    v->velocity = a3_v3_add(v->velocity, a3_v3_scale(F, dts / sc->m));
    A3Vec3 bs = v->body_size;
    f32 m = sc->m, lx = bs.x, ly = a3_maxf(bs.y, 0.8f), lz = bs.z;
    A3Vec3 inv_i = a3_v3(12.0f / (m * (ly * ly + lz * lz)), 12.0f / (m * (lx * lx + lz * lz)), 12.0f / (m * (lx * lx + ly * ly)));
    A3Quat ri = a3_quat_conjugate(r);
    A3Vec3 tl = a3_quat_rotate(ri, T);
    A3Vec3 al = a3_v3(tl.x * inv_i.x, tl.y * inv_i.y, tl.z * inv_i.z);
    v->angular_velocity = a3_v3_add(v->angular_velocity, a3_v3_scale(a3_quat_rotate(r, al), dts));
    v->angular_velocity = a3_v3_scale(v->angular_velocity, 1.0f - 0.4f * dts);
    /* hitting the bump stops (or landing hard): push out, stop the motion into the ground */
    if (excess > 0) {
        cg = a3_v3_add(cg, a3_v3_scale(excess_n, excess * 0.8f));
        f32 vn = a3_v3_dot(v->velocity, excess_n);
        if (vn < 0) {
            v->velocity = a3_v3_sub(v->velocity, a3_v3_scale(excess_n, vn));
            if (-vn > 4.0f) v->last_impact = a3_maxf(v->last_impact, -vn);
        }
    }
    cg = a3_v3_add(cg, a3_v3_scale(v->velocity, dts));
    A3Vec3 wv = v->angular_velocity;
    A3Quat dq = a3_quat(wv.x, wv.y, wv.z, 0);
    A3Quat qd = a3_quat_mul(dq, r);
    r = a3_quat_normalize(a3_quat(r.x + qd.x * 0.5f * dts, r.y + qd.y * 0.5f * dts, r.z + qd.z * 0.5f * dts, r.w + qd.w * 0.5f * dts));
    v->orientation = r;
    sc->pos = a3_v3_sub(cg, rot_v(r, 0, CG_HEIGHT, -sc->cg_fwd));
    A3_UNUSED(right);
}

static void sim_place_wheels(A3World *w, A3Entity e, const A3CVehicle *v) {
    for (A3Entity c = a3_entity_first_child(w, e); a3_entity_valid(w, c); c = a3_entity_next_sibling(w, c)) {
        if (!a3_str_starts_with(a3_entity_name(w, c), "Wheel")) continue;
        A3CTransform *ct = a3_transform(w, c);
        if (!ct) continue;
        u32 idx = (ct->position.z < 0 ? 0u : 2u) + (ct->position.x < 0 ? 0u : 1u);
        ct->position.y = v->top_y - sim_travel(v) + a3_maxf(v->comp[idx], 0.0f);
    }
}

/* Steps the realistic cars together: the wheel kernel runs once per
 * substep for all of them. */
static void sim_step_cars(A3World *w, const A3Entity *ents, u32 count, f32 dt) {
    if (!count || dt <= 0) return;
    dt = a3_minf(dt, 0.05f);
    SimCar *cars = A3_NEW_ARRAY(SimCar, count, A3_MEM_TEMP);
    A3WheelQuad *quads = A3_NEW_ARRAY(A3WheelQuad, count, A3_MEM_TEMP);
    if (!cars || !quads) { a3_free(cars); a3_free(quads); return; }
    u32 n = 0;
    for (u32 i = 0; i < count; ++i) {
        A3CVehicle *v = (A3CVehicle *)a3_component_get(w, ents[i], A3_T_VEHICLE);
        A3CTransform *t = a3_transform(w, ents[i]);
        if (!v || !t) continue;
        if (v->sim_ready && a3_v3_len_sq(a3_v3_sub(t->position, v->last_position)) > 9.0f) v->sim_ready = 0;   /* teleported */
        if (!v->sim_ready) sim_init(w, ents[i], v, t);
        sim_derive(v);
        SimCar *sc = &cars[n++];
        sc->e = ents[i];
        sc->v = v;
        sc->t = t;
        sc->pos = t->position;
        sc->m = sim_mass(v);
        sc->k = sim_stiffness(v);
        sc->c = 2.0f * 0.38f * a3_sqrtf(sc->k * sc->m * 0.25f);
        sc->L = sim_travel(v);
        sc->wr = a3_maxf(v->wheel_radius, 0.1f);
        sc->mu = 0.6f + 0.6f * a3_clampf(v->grip, 0, 1);
        /* weight distribution by layout: front-drive cars are nose heavy, rear-drive close to 50/50 */
        sc->front_share = v->drive == A3_DRIVE_FRONT ? 0.62f : v->drive == A3_DRIVE_ALL ? 0.56f : 0.52f;
        sc->cg_fwd = (sc->front_share - 0.5f) * a3_maxf(v->wheelbase, 0.5f);
        for (u32 k = 0; k < 4; ++k) sc->load[k] = sc->m * SIM_G * 0.5f * (k < 2 ? sc->front_share : 1.0f - sc->front_share);
        v->last_impact = 0;
        sim_frame_inputs(sc, dt);
    }
    f32 dts = dt / (f32)SIM_SUBSTEPS;
    for (u32 s = 0; s < SIM_SUBSTEPS; ++s) {
        for (u32 i = 0; i < n; ++i) sim_prepare(w, &cars[i], &quads[i], dts);
        a3_vk_wheels(quads, n);                                   /* assembly: all wheels of all cars */
        for (u32 i = 0; i < n; ++i) sim_integrate(&cars[i], &quads[i], dts);
    }
    for (u32 i = 0; i < n; ++i) {
        SimCar *sc = &cars[i];
        A3CVehicle *v = sc->v;
        A3Quat r = v->orientation;
        A3Vec3 pos = sc->pos;
        if (collide_body(w, sc->e, v, &pos, r, &v->velocity)) {
            /* walls also stop the spin they would cause */
            v->angular_velocity = a3_v3_scale(v->angular_velocity, 0.6f);
        }
        if (pos.y < -60.0f) { pos.y = 2.0f; v->velocity = a3_v3_zero(); v->angular_velocity = a3_v3_zero(); v->orientation = a3_quat_axis_angle(a3_v3(0, 1, 0), v->yaw); }
        A3Vec3 fwd = rot_v(v->orientation, 0, 0, -1), right = rot_v(v->orientation, 1, 0, 0), up = rot_v(v->orientation, 0, 1, 0);
        v->speed = a3_v3_dot(v->velocity, fwd);
        v->yaw = a3_atan2f(-fwd.x, -fwd.z);
        v->pitch = a3_asinf(a3_clampf(fwd.y, -1, 1));
        v->roll = a3_atan2f(right.y, up.y);
        v->grounded = sc->hit[0] || sc->hit[1] || sc->hit[2] || sc->hit[3];
        v->vertical_speed = v->velocity.y;
        f32 slip = 0;
        for (u32 k = 0; k < 4; ++k) slip = a3_maxf(slip, v->slip_w[k]);
        v->wheel_slip = slip;
        v->wheel_angle = a3_wrap_angle(v->wheel_angle + (v->speed + (sc->drive > 0 ? slip * 6.0f : 0.0f)) * dt / sc->wr);
        sc->t->position = pos;
        sc->t->rotation = v->orientation;
        v->last_position = pos;
        A3CRigidBody *rb = (A3CRigidBody *)a3_component_get(w, sc->e, A3_T_RIGIDBODY);
        if (rb && rb->type == A3_BODY_KINEMATIC) rb->velocity = v->velocity;
        sim_place_wheels(w, sc->e, v);
        spin_wheels(w, sc->e, v);
    }
    a3_free(quads);
    a3_free(cars);
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

/* Engine and tire sounds (child objects "Engine Sound" / "Tire Sound"):
 * the engine loop is recorded at 2000 rpm, so pitch = rpm / 2000. */
static void update_sounds(A3World *w, A3Entity car, const A3CVehicle *v) {
    if (A3_T_AUDIO_SOURCE == 0xFFFFFFFFu) return;
    f32 rpm = v->rpm, slip = v->wheel_slip;
    if (v->physics_model != A3_VEHICLE_REALISTIC) {
        rpm = 900.0f + a3_absf(v->speed) / a3_maxf(v->max_speed, 1.0f) * 4800.0f;
        A3Vec3 vel = a3_v3(v->velocity.x, 0, v->velocity.z);
        f32 sp = a3_v3_len(vel);
        f32 lat = sp > 0.5f ? a3_absf(a3_v3_dot(vel, a3_v3(-a3_cosf(v->yaw), 0, a3_sinf(v->yaw)))) : 0.0f;
        slip = a3_clampf(lat / 6.0f, 0, 1);
    }
    f32 thr = a3_absf(v->throttle);
    f32 speed = a3_v3_len(v->velocity);
    for (A3Entity c = a3_entity_first_child(w, car); a3_entity_valid(w, c); c = a3_entity_next_sibling(w, c)) {
        const char *name = a3_entity_name(w, c);
        b32 engine = a3_streq(name, "Engine Sound"), tire = a3_streq(name, "Tire Sound");
        if (!engine && !tire) continue;
        A3CAudioSource *src = (A3CAudioSource *)a3_component_get(w, c, A3_T_AUDIO_SOURCE);
        if (!src) continue;
        if (engine) {
            src->pitch = a3_clampf(rpm / 2000.0f, 0.3f, 4.0f);
            src->volume = 0.22f + 0.5f * thr * (v->shift_timer > 0 ? 0.4f : 1.0f);
        } else {
            f32 k = a3_smoothstep(0.35f, 0.9f, slip) * a3_clampf(speed / 5.0f, 0, 1) * (v->grounded ? 1.0f : 0.0f);
            src->volume = 0.75f * k;
            src->pitch = 0.9f + 0.2f * slip;
        }
    }
}

void a3_vehicle_update_all(A3World *w, f32 dt) {
    if (A3_T_VEHICLE == 0xFFFFFFFFu) return;
    u32 n = 0;
    const A3Entity *ents = 0;
    f32 night = night_factor(w);
    a3_component_array(w, A3_T_VEHICLE, &n, &ents);
    for (u32 i = 0; i < n; ++i) set_headlights(w, ents[i], night);
    A3Entity *real = n ? A3_NEW_ARRAY(A3Entity, n, A3_MEM_TEMP) : 0;
    u32 nreal = 0;
    for (u32 i = 0; i < n; ++i) {
        A3Entity e = ents[i];
        if (!a3_entity_active(w, e)) continue;
        A3CVehicle *v = (A3CVehicle *)a3_component_get(w, e, A3_T_VEHICLE);
        if (!v) continue;
        if (v->physics_model == A3_VEHICLE_REALISTIC && real) real[nreal++] = e;
        else a3_vehicle_step(w, e, v, dt);
    }
    sim_step_cars(w, real, nreal, dt);      /* all realistic cars share the wheel kernel calls */
    a3_free(real);
    for (u32 i = 0; i < n; ++i) {
        const A3CVehicle *v = (const A3CVehicle *)a3_component_get(w, ents[i], A3_T_VEHICLE);
        if (v && a3_entity_active(w, ents[i])) update_sounds(w, ents[i], v);
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
        /* drivetrain and chassis per body style (Realistic model) */
        const char *st = style && *style ? style : "sedan";
        typedef struct Tune { const char *name; f32 mass, torque, top, hz, travel, grip; i32 drive; } Tune;
        static const Tune tunes[] = {
            { "sedan",  1450, 360, 56, 1.6f, 0.28f, 0.85f, A3_DRIVE_REAR },
            { "sports", 1350, 540, 80, 2.2f, 0.22f, 0.95f, A3_DRIVE_REAR },
            { "suv",    2050, 540, 54, 1.3f, 0.34f, 0.8f,  A3_DRIVE_ALL },
            { "hatch",  1150, 250, 50, 1.8f, 0.26f, 0.85f, A3_DRIVE_FRONT },
            { "taxi",   1500, 330, 50, 1.5f, 0.28f, 0.8f,  A3_DRIVE_FRONT },
            { "police", 1650, 500, 70, 1.8f, 0.26f, 0.9f,  A3_DRIVE_REAR },
        };
        for (u32 i = 0; i < A3_ARRAY_COUNT(tunes); ++i) {
            if (!a3_streq(tunes[i].name, st)) continue;
            v->mass = tunes[i].mass; v->engine_torque = tunes[i].torque; v->max_speed = tunes[i].top;
            v->suspension_hz = tunes[i].hz; v->suspension_travel = tunes[i].travel; v->grip = tunes[i].grip; v->drive = tunes[i].drive;
            rb->mass = tunes[i].mass;
        }
    }
    static const char *const names[4] = { "Wheel FL", "Wheel FR", "Wheel RL", "Wheel RR" };
    for (int i = 0; i < 4; ++i) {
        f32 x = (i & 1) ? track : -track, z = i < 2 ? -wb * 0.5f : wb * 0.5f;
        A3Entity wh = part(w, car, names[i], a3_v3(x, wr, z), a3_v3s(wr), "builtin:wheel_detailed", A3_PRIM_NONE, "builtin:wheel", a3_v4(1, 1, 1, 1));
        if (x < 0) a3_transform(w, wh)->rotation = a3_quat_axis_angle(a3_v3(0, 1, 0), A3_PI);  /* rim face outward */
    }
    /* engine and tire sounds (3D loops; volume and pitch follow the car) */
    if (A3_T_AUDIO_SOURCE != 0xFFFFFFFFu) {
        for (int k = 0; k < 2; ++k) {
            A3Entity se = a3_entity_create(w, k == 0 ? "Engine Sound" : "Tire Sound");
            A3CTransform *st = (A3CTransform *)a3_component_add(w, se, A3_T_TRANSFORM);
            st->position = a3_v3(0, 0.5f, k == 0 ? -len * 0.3f : 0.0f);
            a3_entity_set_parent(w, se, car);
            A3CAudioSource *src = (A3CAudioSource *)a3_component_add(w, se, A3_T_AUDIO_SOURCE);
            if (!src) continue;
            src->builtin = k == 0 ? A3_SOUND_ENGINE : A3_SOUND_SKID;
            src->clip.path[0] = 0;
            src->loop = 1;
            src->play_on_start = 1;
            src->spatial = 1;
            src->volume = 0;
            src->pitch = 1;
            src->min_distance = 4.0f;
            src->max_distance = k == 0 ? 90.0f : 70.0f;
        }
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
