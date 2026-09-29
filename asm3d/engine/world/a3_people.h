/*
 * ASM3D - a3_people.h
 * People: the Person component gives an object a jointed human body built
 * from the builtin:human_* meshes (hips/torso, head, upper arms, forearms
 * with hands, thighs, shins with shoes) and animates a walk / run cycle from
 * how fast the object actually moves, so it works for traffic pedestrians,
 * a CharacterController player or anything moved by a script.
 *
 * Looks: shirt color, skin tone (0 light .. 1 dark), style (hair color,
 * trousers, shoes) and height. The body is built the first time the system
 * sees the object (also in the editor) as child objects named Hips, Head,
 * Arm L/R, Forearm L/R, Leg L/R, Shin L/R; the feet are at the origin and
 * the person faces -Z.
 */
#ifndef A3_PEOPLE_H
#define A3_PEOPLE_H

#include "../ecs/a3_ecs.h"
#include "../core/a3_math.h"

A3_EXTERN_C_BEGIN

extern u32 A3_T_PERSON;

typedef struct A3CPerson {
    A3Vec4 shirt;           /* shirt color */
    f32 skin;               /* 0 light .. 1 dark */
    f32 style;              /* 0..1: hair, trousers and shoes */
    f32 height;             /* meters (1.75 = default body) */
    b32 animate;            /* walk cycle from the measured speed */
    /* runtime */
    f32 phase;
    f32 speed;              /* smoothed m/s */
    A3Vec3 last_position;
    b32 has_last;
    f32 _pad[2];
} A3CPerson;

void a3_people_register(void);
/* Builds missing bodies and animates every Person. */
void a3_people_update(A3World *w, f32 dt);
/* Creates a person object (Transform + Person) with random looks from seed. */
A3Entity a3_person_spawn(A3World *w, const char *name, A3Vec3 feet, f32 yaw_deg, u64 seed);

A3_EXTERN_C_END

#endif
