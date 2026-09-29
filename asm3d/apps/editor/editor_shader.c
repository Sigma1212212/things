/*
 * ASM3D Editor - Shader Maker (node graph -> GLSL surface shader).
 * Temporary stub while the editor shell is brought up; replaced next.
 */
#include "editor.h"

void ed_shader_init(A3Editor *ed) { A3_UNUSED(ed); }
void ed_shader_shutdown(A3Editor *ed) { A3_UNUSED(ed); }
void ed_shader_new(A3Editor *ed) { A3_UNUSED(ed); }
b32  ed_shader_open(A3Editor *ed, const char *rel) { A3_UNUSED(ed); A3_UNUSED(rel); return 0; }
void ed_shader_render_preview(A3Editor *ed) { A3_UNUSED(ed); }
void ed_shader_panel(void *user, A3Ui *ui, A3Rect r) { A3_UNUSED(user); A3_UNUSED(r); a3_ui_label(ui, "Shader Maker"); }
