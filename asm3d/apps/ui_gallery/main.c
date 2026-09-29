/*
 * ASM3D - UI gallery: renders every widget for visual checks.
 *   asm3d_ui_gallery [--frames N --screenshot out.png]
 */
#include "../../engine/runtime/a3_engine.h"
#include "../../engine/ui/a3_ui.h"
#include "../../engine/core/a3_string.h"
#include "../../engine/core/a3_image.h"
#include "../../engine/core/a3_memory.h"
#include "../../engine/platform/a3_platform.h"
#include <stdlib.h>

static void clip_set(void *u, const char *t) { a3_window_set_clipboard((A3Window *)u, t); }
static const char *clip_get(void *u) { return a3_window_get_clipboard((A3Window *)u); }

int main(int argc, char **argv) {
    int frames = -1;
    const char *shot = 0;
    for (int i = 1; i < argc; ++i) {
        if (!a3_strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!a3_strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
    }
    A3EngineDesc d = { "ASM3D UI Gallery", 1100, 720, 1, 0, 1, "", 60 };
    A3Engine *eng = a3_engine_create(&d);
    if (!eng) return 1;
    A3Ui *ui = a3_ui_create();
    a3_ui_set_clipboard_fns(ui, clip_set, clip_get, a3_engine_window(eng));
    b32 check = 1, toggle = 1;
    f32 slider = 0.35f, drag = 4.5f, vec[3] = { 1, 2.5f, -3 };
    i32 combo = 1;
    A3Vec4 color = a3_v4(0.25f, 0.55f, 0.95f, 1);
    char text[128] = "Player";
    char search[64] = "";
    const char *items[] = { "Perspective", "Orthographic" };
    f32 plot[64];
    for (int i = 0; i < 64; ++i) plot[i] = 8 + 4 * a3_sinf(i * 0.3f) + (i % 7);
    int frame = 0;
    f32 dt;
    while (a3_engine_begin_frame(eng, &dt)) {
        i32 w, h;
        a3_window_size(a3_engine_window(eng), &w, &h);
        a3_ui_begin_frame(ui, a3_engine_input(eng), w, h, frames >= 0 ? 1.0f / 60 : dt);
        A3UiTheme *th = a3_ui_theme(ui);
        a3_ui_rect(ui, a3_rect(0, 0, (f32)w, (f32)h), th->colors[A3_UIC_BG], 0);
        if (a3_ui_begin_menubar(ui, a3_rect(0, 0, (f32)w, 28))) {
            if (a3_ui_menubar_menu(ui, "File")) { a3_ui_menu_item(ui, "New Project", "Ctrl+N", 1); a3_ui_menu_item(ui, "Save Scene", "Ctrl+S", 1); a3_ui_end_popup(ui); }
            if (a3_ui_menubar_menu(ui, "Edit")) { a3_ui_menu_item(ui, "Undo", "Ctrl+Z", 1); a3_ui_end_popup(ui); }
            a3_ui_menubar_menu(ui, "Create");
            a3_ui_menubar_menu(ui, "Build");
            a3_ui_menubar_menu(ui, "Help");
            a3_ui_end_menubar(ui);
        }
        if (a3_ui_begin_panel(ui, "Hierarchy", a3_rect(8, 36, 260, (f32)h - 44), A3_PANEL_BORDER)) {
            a3_ui_heading(ui, "Hierarchy");
            a3_ui_input_text(ui, "search", search, sizeof(search), A3_INPUT_SEARCH, "Search entities...");
            b32 clicked;
            if (a3_ui_tree_node(ui, "world", "Level 1", A3_ICON_HOME, A3_TREE_DEFAULT_OPEN, &clicked)) {
                if (a3_ui_tree_node(ui, "player", "Player", A3_ICON_CUBE, A3_TREE_SELECTED | A3_TREE_DEFAULT_OPEN, &clicked)) {
                    a3_ui_tree_node(ui, "cam", "Player Camera", A3_ICON_TARGET, A3_TREE_LEAF, &clicked);
                    a3_ui_tree_pop(ui);
                }
                a3_ui_tree_node(ui, "sun", "Sun", A3_ICON_SUN, A3_TREE_LEAF, &clicked);
                a3_ui_tree_node(ui, "ground", "Ground", A3_ICON_BOX, A3_TREE_LEAF, &clicked);
                a3_ui_tree_node(ui, "hidden", "Hidden Crate", A3_ICON_BOX, A3_TREE_LEAF | A3_TREE_DIM, &clicked);
                a3_ui_tree_pop(ui);
            }
        }
        a3_ui_end_panel(ui);
        if (a3_ui_begin_panel(ui, "Inspector", a3_rect(276, 36, 380, (f32)h - 44), A3_PANEL_BORDER)) {
            a3_ui_heading(ui, "Inspector");
            a3_ui_property(ui, "Name", "Entity name"); a3_ui_input_text(ui, "name", text, sizeof(text), 0, 0);
            if (a3_ui_collapsing_header(ui, "tr", "Transform", A3_ICON_MOVE_H, 1)) {
                a3_ui_property(ui, "Position", "Location in meters"); a3_ui_drag_float_n(ui, "pos", vec, 3, 0.05f, 0, 0, "%.2f");
                a3_ui_property(ui, "Speed", 0); a3_ui_drag_float(ui, "spd", &drag, 0.05f, 0, 100, "%.2f");
            }
            if (a3_ui_collapsing_header(ui, "mr", "Mesh Renderer", A3_ICON_CUBE, 1)) {
                a3_ui_property(ui, "Color", 0); a3_ui_color_edit(ui, "col", &color, 1);
                a3_ui_property(ui, "Roughness", 0); a3_ui_slider_float(ui, "rough", &slider, 0, 1, "%.2f");
                a3_ui_property(ui, "Projection", 0); a3_ui_combo(ui, "proj", &combo, items, 2);
                a3_ui_property(ui, "Cast Shadows", 0); a3_ui_toggle(ui, "shadows", &toggle);
            }
            a3_ui_checkbox(ui, "Visible", &check);
            a3_ui_spacing(ui, 6);
            a3_ui_button_ex(ui, "Add Component", 0, A3_BUTTON_PRIMARY);
            a3_ui_same_line(ui);
            a3_ui_button(ui, "Reset");
            a3_ui_same_line(ui);
            a3_ui_button_ex(ui, "Delete", 0, A3_BUTTON_DANGER);
            a3_ui_separator(ui);
            a3_ui_label_wrapped(ui, "Tip: drag a model from the Assets panel into the viewport to place it. Hold right mouse and use WASD to fly around.");
            a3_ui_progress_bar(ui, 0.62f, "Building... 62%");
            a3_ui_plot_lines(ui, "Frame time (ms)", plot, 64, 0, 0, 20, 60, th->colors[A3_UIC_SUCCESS]);
        }
        a3_ui_end_panel(ui);
        if (a3_ui_begin_panel(ui, "Palette", a3_rect(664, 36, (f32)w - 672, (f32)h - 44), A3_PANEL_BORDER)) {
            a3_ui_heading(ui, "Icons & Text");
            a3_ui_label_font(ui, A3_FONT_TITLE, th->colors[A3_UIC_TEXT], "ASM3D Editor");
            a3_ui_icon_button(ui, A3_ICON_PLAY, "Play", A3_BUTTON_TOGGLED); a3_ui_same_line(ui);
            a3_ui_icon_button(ui, 0x1, "Pause", 0); a3_ui_same_line(ui);
            a3_ui_icon_button(ui, 0x2, "Step", 0); a3_ui_same_line(ui);
            a3_ui_icon_button(ui, A3_ICON_STOP, "Stop", 0); a3_ui_same_line(ui);
            a3_ui_icon_button(ui, A3_ICON_GEAR, "Settings", 0); a3_ui_same_line(ui);
            a3_ui_icon_button(ui, 0x4, "Folder", 0);
            a3_ui_label_colored(ui, th->colors[A3_UIC_SUCCESS], "%s Build succeeded (2.1 s)", "\xE2\x9C\x93");
            a3_ui_label_colored(ui, th->colors[A3_UIC_WARNING], "%s 2 textures are larger than 4096 px", "\xE2\x9A\xA0");
            a3_ui_label_colored(ui, th->colors[A3_UIC_ERROR], "%s Material 'Rock' is missing a texture", "\xE2\x9C\x95");
            a3_ui_label_font(ui, A3_FONT_MONO, th->colors[A3_UIC_TEXT_DIM], "fn on_update(dt: float) { player.jump(); }");
            a3_ui_selectable_ex(ui, "s1", "Assets/Models/crate.obj", 0, A3_ICON_CUBE, 0);
            a3_ui_selectable_ex(ui, "s2", "Assets/Textures/grass.png", 1, A3_ICON_SQUARE_O, 0);
            A3Rect r = a3_ui_next_rect(ui, 0, 90);
            a3_ui_bezier(ui, a3_v2(r.x + 10, r.y + 20), a3_v2(r.x + 110, r.y + 20), a3_v2(r.x + r.w - 110, r.y + 70), a3_v2(r.x + r.w - 10, r.y + 70), th->colors[A3_UIC_ACCENT], 2.5f);
            a3_ui_circle(ui, a3_v2(r.x + 10, r.y + 20), 6, th->colors[A3_UIC_SUCCESS]);
            a3_ui_circle(ui, a3_v2(r.x + r.w - 10, r.y + 70), 6, th->colors[A3_UIC_WARNING]);
        }
        a3_ui_end_panel(ui);
        if (frame == 0) a3_ui_notify(ui, 0, "Project saved");
        a3_ui_end_frame(ui);
        a3_rhi_target_bind((A3RhiTarget){ 0 }, w, h);
        a3_rhi_clear(1, a3_v4(0, 0, 0, 1), 1, 1);
        a3_ui_render(ui);
        a3_window_set_cursor(a3_engine_window(eng), a3_ui_cursor(ui));
        if (frames >= 0 && ++frame >= frames) {
            if (shot) a3_engine_screenshot(eng, shot);
            a3_engine_end_frame(eng);
            break;
        }
        a3_engine_end_frame(eng);
    }
    a3_ui_destroy(ui);
    a3_engine_destroy(eng);
    return 0;
}
