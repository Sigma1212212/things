/*
 * ASM3D - a3_vehicle.h
 * Vehicles with two physics models (Physics field):
 *
 * Realistic (default): a 6-degree-of-freedom rigid body on four raycast
 * wheels.
 *   - suspension: one ray per wheel, spring + damper + anti-roll bars; the
 *     body pitches under braking, squats when accelerating and rolls in
 *     corners because the forces act at the contact patches, below the
 *     center of mass (weight transfer is not faked, it happens)
 *   - tires: slip angle -> Pacejka-style lateral force (peak near 8 degrees),
 *     friction circle shared between cornering and traction/braking, so
 *     too much throttle or brake in a corner makes the car slide; locked
 *     rear wheels with the handbrake
 *   - drivetrain: engine torque curve, rev limiter, 6-speed automatic
 *     gearbox (gears scaled so the car reaches Top Speed), rear / front /
 *     all-wheel drive, engine braking, brake bias, traction control
 *   - aerodynamic drag and rolling resistance; the drag area is chosen so
 *     the engine's power tops out at Top Speed
 *   - the per-wheel math (suspension load, slip angle, tire curve, friction
 *     circle) runs for all cars at once in the x86-64 assembly kernel
 *     a3_vk_wheels (a3_vehicle_x64.S), 4 substeps per frame
 *   - wheels move up and down with the suspension; Rpm, Gear and Wheel Slip
 *     drive the engine and tire sounds
 *
 * Arcade: the kinematic model tuned for fun (used by AI traffic):
 *   - throttle / brake / reverse with a speed-dependent acceleration curve
 *   - bicycle-model steering, grip pulls the velocity toward the heading,
 *     the handbrake lowers it, so the car drifts
 *   - two ground rays (front and rear axle) give height and pitch
 *
 * Both: collisions push the body box out of solid colliders, the speed into
 * the wall is removed and reported as an impact; dynamic bodies are shoved;
 * kinematic RigidBody velocity is kept in sync; wheels (child objects named
 * "Wheel*") spin and the front ones steer.
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

enum { A3_VEHICLE_REALISTIC = 0, A3_VEHICLE_ARCADE = 1 };
enum { A3_DRIVE_REAR = 0, A3_DRIVE_FRONT = 1, A3_DRIVE_ALL = 2 };

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
    /* realistic model */
    i32 physics_model;      /* A3_VEHICLE_REALISTIC / A3_VEHICLE_ARCADE */
    i32 drive;              /* A3_DRIVE_REAR / FRONT / ALL */
    f32 mass;               /* kg */
    f32 engine_torque;      /* N m at the torque peak */
    f32 redline;            /* rpm */
    f32 suspension_travel;  /* m */
    f32 suspension_hz;      /* natural frequency (stiffness) */
    b32 traction_control;
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
    /* realistic model state (runtime) */
    f32 rpm;
    i32 gear;               /* -1 reverse, 1..6 */
    f32 wheel_slip;         /* 0..1, the most sliding tire */
    f32 roll;               /* radians, body roll (read only) */
    f32 shift_timer;
    b32 sim_ready;
    A3Quat orientation;
    A3Vec3 angular_velocity;
    f32 comp[4];            /* suspension compression per wheel (FL, FR, RL, RR) */
    A3Vec3 hardpoint[4];    /* wheel rest positions (local) */
    f32 top_y;              /* local height of the suspension mounts */
    f32 drag_area;          /* derived: Cd * A */
    f32 final_drive;        /* derived */
    f32 slip_w[4];
    A3Vec3 skid_last[4];    /* last skid mark point per wheel */
    b32 skid_on[4];
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

/* Builds a complete car (smooth body with glass and lights, four detailed
 * wheels, a headlight spot light, collider, kinematic body, Vehicle) at a
 * ground position facing yaw (degrees, 0 = -Z). Returns the root entity. */
A3Entity a3_vehicle_spawn_car(A3World *w, const char *name, A3Vec3 ground_pos, f32 yaw_deg, A3Vec4 paint);
/* style: "sedan", "sports", "suv", "hatch", "taxi" or "police" (builtin:car_<style>). */
A3Entity a3_vehicle_spawn_car_style(A3World *w, const char *name, A3Vec3 ground_pos, f32 yaw_deg, A3Vec4 paint, const char *style);

A3_EXTERN_C_END

#endif
