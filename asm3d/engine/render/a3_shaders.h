/*
 * ASM3D - a3_shaders.h
 * Built-in GLSL shader sources.
 *
 * Materials are written as a *surface function* that the engine inserts
 * into the lit template:
 *
 *     void a3_surface(inout A3Surface s) {
 *         s.albedo = texture(u_albedo_tex, s.uv).rgb * s.color.rgb;
 *     }
 *
 * The Shader Maker generates exactly such a function from its node graph,
 * so custom materials automatically get lighting, shadows, fog, instancing
 * and every render feature added later.
 */
#ifndef A3_SHADERS_H
#define A3_SHADERS_H

#include "../core/a3_base.h"

A3_EXTERN_C_BEGIN

extern const char *A3_SHADER_LIT_VS;
/* Fragment template: A3_SHADER_LIT_FS_HEAD + <surface function> + A3_SHADER_LIT_FS_TAIL */
extern const char *A3_SHADER_LIT_FS_HEAD;
extern const char *A3_SHADER_LIT_FS_TAIL;
extern const char *A3_SHADER_DEFAULT_SURFACE;

extern const char *A3_SHADER_SHADOW_VS;
extern const char *A3_SHADER_SHADOW_FS;
extern const char *A3_SHADER_SKY_VS;
extern const char *A3_SHADER_SKY_FS;
extern const char *A3_SHADER_FULLSCREEN_VS;
extern const char *A3_SHADER_TONEMAP_FS;
extern const char *A3_SHADER_SSAO_FS;
extern const char *A3_SHADER_BLUR4_FS;
extern const char *A3_SHADER_COMPOSITE_FS;
extern const char *A3_SHADER_BLOOM_DOWN_FS;
extern const char *A3_SHADER_BLOOM_UP_FS;
extern const char *A3_SHADER_FXAA_FS;
extern const char *A3_SHADER_LINES_VS;
extern const char *A3_SHADER_LINES_FS;
extern const char *A3_SHADER_UI_VS;
extern const char *A3_SHADER_UI_FS;
extern const char *A3_SHADER_GRID_VS;
extern const char *A3_SHADER_GRID_FS;
extern const char *A3_SHADER_PARTICLE_VS;
extern const char *A3_SHADER_PARTICLE_FS;

/* Number of source lines before the surface function in the assembled lit
 * fragment shader; used to map compile errors back to the user's code. */
int a3_shader_surface_line_offset(void);

A3_EXTERN_C_END

#endif
