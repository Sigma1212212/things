/*
 * ASM3D - a3_character.h
 * Character controller movement API (used by the built-in player system,
 * scripts and AI).
 */
#ifndef A3_CHARACTER_H
#define A3_CHARACTER_H

#include "a3_physics.h"

A3_EXTERN_C_BEGIN

/* move_input: x = strafe (-1..1), y = forward (-1..1), relative to cc->yaw. */
void a3_character_move(A3World *w, A3Entity e, A3CCharacterController *cc, A3Vec2 move_input, b32 jump, b32 sprint, f32 dt);
/* Applies mouse movement to yaw/pitch and positions the camera child. */
void a3_character_look(A3World *w, A3Entity e, A3CCharacterController *cc, A3Vec2 mouse_delta);

A3_EXTERN_C_END

#endif
