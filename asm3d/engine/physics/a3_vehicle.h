/*
 * ASM3D - a3_vehicle.h
 * Arcade vehicles: a kinematic car model tuned for fun rather than realism.
 *
 *   - throttle / brake / reverse with a speed-dependent acceleration curve
 *   - bicycle-model steering (yaw rate = speed * tan(steer) / wheelbase),
 *     steering range narrows at high speed
 *   - grip: the velocity turns toward the car's heading at a rate set by
 *     Grip; the handbrake lowers it, so the car drifts
 *   - ground: two rays (front and rear axle) give height and pitch; no
 *     ground = gravity (ramps and jumps work)
 *   - collisions: the body box is pushed out of solid colliders, the speed
 *     into the wall is removed and reported as an impact; dynamic bodies are
 *     shoved; kinematic RigidBody velocity is kept in sync so dynamic objects
 *     react to the car
 *   - wheels (child objects named "Wheel*") spin and the front ones steer
 *
 * The entity origin is on the ground between the axles; the car faces -Z.
 * Moving the object more than 3 m in one step (a teleport from a script)
 * resets the car: it stops and takes the new heading.
 * Inputs (throttle, steer, handbrake) come from the move actions when
 * "Player Controlled" is on, otherwise from scripts or the traffic system.
 */
#ifndef A3_VEHICLE_H
#define A3_VEHICLE_H

#include "../ecs/a3_ecs.h"
#include "../core/a3_math.h"

A3_EXTERN_C_BEGIN

extern u32 A3_T_VEHICLE;

typedef struct A3CVehicle {
    f32 max_speed;          /* m/s */
    f32 acceleration;       /* m/s^2 at low speed */
    f32 braking;            /* m/s^2 */
    f32 reverse_speed;      /* m/s */
    f32 steer_angle;        /* degrees at low speed */
    f32 grip;               /* 0..1: how fast the velocity follows the heading */
    f32 wheelbase;          /* m */
    f32 wheel_radius;       /* m (wheel spin) */
    A3Vec3 body_size;       /* collision box (m); bottom sits body_clearance above the ground */
    f32 body_clearance;     /* curbs lower than this are driven over */
    b32 use_input;          /* Player Controlled: move_y = throttle, move_x = steer, jump = handbrake */
    b32 ground_probe;       /* raycast the ground (off: keep the current height, e.g. AI traffic on flat roads) */
    b32 collide_world;      /* push out of walls and other colliders (off: AI traffic, which avoids by itself) */
    b32 chase_camera;       /* when player controlled, the main camera follows behind */
    f32 camera_distance;
    f32 camera_height;
    /* inputs (runtime; set by scripts or AI) */
    f32 throttle;           /* -1..1 */
    f32 steer;              /* -1..1 (positive = right) */
    b32 handbrake;
    /* state (runtime) */
    f32 speed;              /* m/s along the heading (negative = reversing) */
    A3Vec3 velocity;        /* m/s world */
    f32 yaw;                /* radians */
    f32 pitch;              /* radians */
    f32 steer_current;      /* radians, smoothed */
    f32 wheel_angle;        /* radians, wheel spin */
    f32 vertical_speed;
    b32 grounded;
    f32 last_impact;        /* m/s of the latest collision (0 when none this step) */
    f32 damage;             /* sum of impacts above 4 m/s */
    b32 initialized;
    A3Vec3 last_position;   /* runtime: a jump from here means the car was teleported (reset) */
    f32 _pad[2];
} A3CVehicle;

void a3_vehicle_register(void);

/* Advances one vehicle by dt using its current inputs. */
void a3_vehicle_step(A3World *w, A3Entity e, A3CVehicle *v, f32 dt);
/* Steps every active vehicle. */
void a3_vehicle_update_all(A3World *w, f32 dt);
/* Places `camera` behind the player-controlled vehicle (smoothed). */
void a3_vehicle_update_camera(A3World *w, A3Entity vehicle, const A3CVehicle *v, A3Entity camera, f32 dt);
/* The active player-controlled vehicle, if any. */
A3Entity a3_vehicle_find_player(A3World *w);

/* Builds a complete car (body, cabin glass, wheels, head and tail lights,
 * collider, kinematic body, Vehicle) at a ground position facing yaw
 * (degrees, 0 = -Z). Returns the root entity. */
A3Entity a3_vehicle_spawn_car(A3World *w, const char *name, A3Vec3 ground_pos, f32 yaw_deg, A3Vec4 paint);

A3_EXTERN_C_END

#endif
