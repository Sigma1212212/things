/*
 * ASM3D - a3_script_hud.c
 * Draws the HUD commands queued by scripts (hud_text / hud_rect / hud_bar).
 */
#include "a3_script_engine.h"
#include "a3_script_hud.h"
#include "../ui/a3_font.h"

void a3_scripts_draw_hud(A3Ui *ui, A3World *w, A3Rect area) {
    const A3HudCmd *cmds = 0;
    u32 n = a3_scripts_hud(w, &cmds);
    if (!n || area.w <= 0 || area.h <= 0) return;
    f32 sx = area.w / A3_HUD_WIDTH, sy = area.h / A3_HUD_HEIGHT;
    a3_ui_push_clip(ui, area);
    for (u32 i = 0; i < n; ++i) {
        const A3HudCmd *c = &cmds[i];
        u32 col = a3_color_from_vec4(c->color);
        if (c->kind == A3_HUD_RECT) {
            a3_ui_rect(ui, a3_rect(area.x + c->x * sx, area.y + c->y * sy, c->w * sx, c->h * sy), col, 0);
            continue;
        }
        /* text: size 1 is about 24 HUD units tall */
        const A3FontId font = A3_FONT_TITLE;
        f32 scale = (c->w > 0 ? c->w : 1.0f) * 24.0f * sy / a3_font_line_height(font);
        f32 tw = a3_font_text_width(font, c->text, -1) * scale;
        f32 x = area.x + c->x * sx, y = area.y + c->y * sy;
        if (c->h == 1) x -= tw * 0.5f;
        else if (c->h == 2) x -= tw;
        f32 sh = a3_maxf(1.0f, 2.0f * sy * (c->w > 0 ? c->w : 1.0f));
        a3_ui_text_scaled(ui, font, a3_v2(x + sh, y + sh), a3_rgba(0, 0, 0, (u8)(160.0f * a3_saturate(c->color.w))), c->text, scale);
        a3_ui_text_scaled(ui, font, a3_v2(x, y), col, c->text, scale);
    }
    a3_ui_pop_clip(ui);
}
