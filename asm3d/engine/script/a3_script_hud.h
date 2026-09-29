/*
 * ASM3D - a3_script_hud.h
 * Draws a world's script HUD into a screen rectangle (game window or editor viewport).
 */
#ifndef A3_SCRIPT_HUD_H
#define A3_SCRIPT_HUD_H

#include "../ui/a3_ui.h"
#include "../ecs/a3_ecs.h"

A3_EXTERN_C_BEGIN
void a3_scripts_draw_hud(A3Ui *ui, A3World *w, A3Rect area);
A3_EXTERN_C_END

#endif
