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
 * Built with the modeling library (A3EMesh), so they are ordinary meshes for
 * the renderer, physics and picking. Registration is idempotent.
 */
#ifndef A3_PROCMESHES_H
#define A3_PROCMESHES_H

#include "../core/a3_base.h"

A3_EXTERN_C_BEGIN

void a3_procmeshes_register(void);
u32  a3_procmeshes_count(void);
const char *a3_procmeshes_name(u32 i);   /* "builtin:palm_crown", ... */

A3_EXTERN_C_END

#endif
