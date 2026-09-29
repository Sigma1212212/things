/*
 * ASM3D - a3_procmeshes.h
 * Procedural meshes used by the city and vehicles, registered with the asset
 * cache as "builtin:<name>" paths (see a3_assets_register_mesh_generator):
 *
 *   builtin:palm_crown   9 drooping fronds, unit size (origin = trunk top)
 *   builtin:car_body     sedan hull + roof, 1.8 x 1.45 x 4.4 m, front = -Z
 *   builtin:car_glass    the cabin (windows), same frame as car_body
 *   builtin:wheel        tire, 0.68 m diameter, axle along X
 *
 * Detailed models (a3_procmodels.c, geometry kernels in x86-64 assembly):
 *   builtin:car_sedan / car_sports / car_suv / car_hatch / car_taxi / car_police
 *                        smooth lofted bodies with wheel arches and mirrors,
 *                        shaded by builtin:carbody (glass, lights, trim...)
 *   builtin:wheel_detailed  tire + rim, radius 1 (scale by the wheel radius)
 *   builtin:palm_a / palm_b / palm_c   curved trunks, 16-20 pinnate fronds,
 *                        coconuts (builtin:palm material)
 *   builtin:human_torso / head / upper_arm / forearm / thigh / shin
 *                        jointed body parts (builtin:human material)
 *   builtin:traffic_light / hydrant / bench / trash_can / street_lamp
 *
 * All are ordinary meshes for
 * the renderer, physics and picking. Registration is idempotent.
 */
#ifndef A3_PROCMESHES_H
#define A3_PROCMESHES_H

#include "../core/a3_base.h"

A3_EXTERN_C_BEGIN

void a3_procmeshes_register(void);
/* Dimensions of a car body style ("builtin:car_sedan", "..._sports", "..._suv", "..._hatch";
 * taxi and police use the sedan). Any output pointer may be NULL. */
b32  a3_procmodels_car_info(const char *style, f32 *wheelbase, f32 *track, f32 *wheel_r, f32 *length, f32 *width, f32 *height);
u32  a3_procmeshes_count(void);
const char *a3_procmeshes_name(u32 i);   /* "builtin:palm_crown", ... */

A3_EXTERN_C_END

#endif
