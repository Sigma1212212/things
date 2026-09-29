/*
 * ASM3D - a3_dock.c
 */
#include "a3_dock.h"
#include "a3_ui_internal.h"
#include "../core/a3_string.h"
#include "../core/a3_format.h"
#include "../core/a3_json.h"
#include "../core/a3_log.h"

#define TAB_H 30.0f
#define SPLIT_W 5.0f

void a3_dock_init(A3Dock *d) {
    a3_zero_struct(d);
    d->drag_panel = -1;
    d->focused_panel = -1;
    a3_dock_reset(d);
}

static i32 new_node(A3Dock *d) {
    for (i32 i = 0; i < A3_DOCK_MAX_NODES; ++i) {
        if (!d->nodes[i].used) {
            a3_zero_struct(&d->nodes[i]);
            d->nodes[i].used = 1;
            d->nodes[i].parent = -1;
            d->nodes[i].child[0] = d->nodes[i].child[1] = -1;
            d->nodes[i].ratio = 0.5f;
            return i;
        }
    }
    return -1;
}

void a3_dock_reset(A3Dock *d) {
    for (i32 i = 0; i < A3_DOCK_MAX_NODES; ++i) d->nodes[i].used = 0;
    for (u32 i = 0; i < d->panel_count; ++i) d->panels[i].open = 0;
    d->root = new_node(d);
}

i32 a3_dock_root(A3Dock *d) { return d->root; }

i32 a3_dock_add_panel(A3Dock *d, const char *name, u32 icon, A3DockDrawFn draw, void *user, u32 flags) {
    i32 existing = a3_dock_find_panel(d, name);
    if (existing >= 0) return existing;
    if (d->panel_count >= A3_DOCK_MAX_PANELS) return -1;
    A3DockPanel *p = &d->panels[d->panel_count];
    a3_zero_struct(p);
    a3_strcpy(p->name, sizeof(p->name), name);
    p->icon = icon;
    p->draw = draw;
    p->user = user;
    p->flags = flags;
    return (i32)d->panel_count++;
}

i32 a3_dock_find_panel(A3Dock *d, const char *name) {
    for (u32 i = 0; i < d->panel_count; ++i) if (a3_streq(d->panels[i].name, name)) return (i32)i;
    return -1;
}

static b32 is_leaf(const A3DockNode *n) { return n->child[0] < 0; }

i32 a3_dock_split(A3Dock *d, i32 node, b32 vertical, f32 ratio, b32 second) {
    if (node < 0 || !d->nodes[node].used || !is_leaf(&d->nodes[node])) return -1;
    i32 a = new_node(d), b = new_node(d);
    if (a < 0 || b < 0) return -1;
    A3DockNode *n = &d->nodes[node];
    /* existing tabs move to the side that is kept */
    A3DockNode *keep = &d->nodes[second ? a : b];
    for (u32 i = 0; i < n->tab_count; ++i) keep->tabs[i] = n->tabs[i];
    keep->tab_count = n->tab_count;
    keep->active = n->active;
    n->tab_count = 0;
    n->child[0] = a;
    n->child[1] = b;
    n->vertical = vertical;
    n->ratio = a3_clampf(ratio, 0.05f, 0.95f);
    d->nodes[a].parent = node;
    d->nodes[b].parent = node;
    return second ? b : a;
}

void a3_dock_add_tab(A3Dock *d, i32 node, i32 panel) {
    if (node < 0 || panel < 0 || !d->nodes[node].used || !is_leaf(&d->nodes[node])) return;
    A3DockNode *n = &d->nodes[node];
    for (u32 i = 0; i < n->tab_count; ++i) if (n->tabs[i] == panel) { n->active = (i32)i; return; }
    if (n->tab_count >= A3_DOCK_MAX_TABS) return;
    n->tabs[n->tab_count++] = panel;
    n->active = (i32)n->tab_count - 1;
    d->panels[panel].open = 1;
}

/* Removes a leaf that became empty by collapsing its parent split. */
static void collapse_empty(A3Dock *d, i32 node) {
    A3DockNode *n = &d->nodes[node];
    if (!is_leaf(n) || n->tab_count || n->parent < 0) return;
    i32 p = n->parent;
    A3DockNode *pn = &d->nodes[p];
    i32 sibling = pn->child[0] == node ? pn->child[1] : pn->child[0];
    A3DockNode s = d->nodes[sibling];
    /* pull sibling into parent */
    pn->child[0] = s.child[0];
    pn->child[1] = s.child[1];
    pn->vertical = s.vertical;
    pn->ratio = s.ratio;
    pn->tab_count = s.tab_count;
    for (u32 i = 0; i < s.tab_count; ++i) pn->tabs[i] = s.tabs[i];
    pn->active = s.active;
    if (pn->child[0] >= 0) { d->nodes[pn->child[0]].parent = p; d->nodes[pn->child[1]].parent = p; }
    d->nodes[node].used = 0;
    d->nodes[sibling].used = 0;
}

static i32 node_of_panel(A3Dock *d, i32 panel, u32 *tab_index) {
    for (i32 i = 0; i < A3_DOCK_MAX_NODES; ++i) {
        A3DockNode *n = &d->nodes[i];
        if (!n->used || !is_leaf(n)) continue;
        for (u32 t = 0; t < n->tab_count; ++t) if (n->tabs[t] == panel) { if (tab_index) *tab_index = t; return i; }
    }
    return -1;
}

static void remove_tab(A3Dock *d, i32 panel) {
    u32 t;
    i32 ni = node_of_panel(d, panel, &t);
    if (ni < 0) return;
    A3DockNode *n = &d->nodes[ni];
    for (u32 k = t; k + 1 < n->tab_count; ++k) n->tabs[k] = n->tabs[k + 1];
    n->tab_count--;
    if (n->active >= (i32)n->tab_count) n->active = (i32)n->tab_count - 1;
    if (n->active < 0) n->active = 0;
    collapse_empty(d, ni);
}

void a3_dock_close_panel(A3Dock *d, i32 panel) {
    if (panel < 0 || (u32)panel >= d->panel_count) return;
    remove_tab(d, panel);
    d->panels[panel].open = 0;
}

static i32 largest_leaf(A3Dock *d) {
    i32 best = d->root;
    f32 area = -1;
    for (i32 i = 0; i < A3_DOCK_MAX_NODES; ++i) {
        A3DockNode *n = &d->nodes[i];
        if (!n->used || !is_leaf(n)) continue;
        f32 a = n->rect.w * n->rect.h;
        if (a > area) { area = a; best = i; }
    }
    return best;
}

void a3_dock_show(A3Dock *d, const char *name) {
    i32 p = a3_dock_find_panel(d, name);
    if (p < 0) return;
    u32 t;
    i32 ni = node_of_panel(d, p, &t);
    if (ni >= 0) { d->nodes[ni].active = (i32)t; d->focused_panel = p; return; }
    a3_dock_add_tab(d, largest_leaf(d), p);
    d->focused_panel = p;
}

b32 a3_dock_is_visible(A3Dock *d, const char *name) {
    i32 p = a3_dock_find_panel(d, name);
    u32 t;
    i32 ni = p >= 0 ? node_of_panel(d, p, &t) : -1;
    return ni >= 0 && d->nodes[ni].active == (i32)t;
}

/* ---- drawing ---- */

typedef struct DropTarget { i32 node; i32 zone; A3Rect preview; } DropTarget; /* zone: 0 center, 1 left, 2 right, 3 top, 4 bottom */

static void layout(A3Dock *d, i32 ni, A3Rect r) {
    A3DockNode *n = &d->nodes[ni];
    n->rect = r;
    if (is_leaf(n)) return;
    if (n->vertical) {
        f32 w0 = a3_floorf((r.w - SPLIT_W) * n->ratio);
        layout(d, n->child[0], a3_rect(r.x, r.y, w0, r.h));
        layout(d, n->child[1], a3_rect(r.x + w0 + SPLIT_W, r.y, r.w - w0 - SPLIT_W, r.h));
    } else {
        f32 h0 = a3_floorf((r.h - SPLIT_W) * n->ratio);
        layout(d, n->child[0], a3_rect(r.x, r.y, r.w, h0));
        layout(d, n->child[1], a3_rect(r.x, r.y + h0 + SPLIT_W, r.w, r.h - h0 - SPLIT_W));
    }
}

static void draw_splitters(A3Dock *d, A3Ui *ui, i32 ni) {
    A3DockNode *n = &d->nodes[ni];
    if (is_leaf(n)) return;
    A3Rect a = d->nodes[n->child[0]].rect;
    A3Rect sr = n->vertical ? a3_rect(a.x + a.w, n->rect.y, SPLIT_W, n->rect.h) : a3_rect(n->rect.x, a.y + a.h, n->rect.w, SPLIT_W);
    char id[32];
    a3_snprintf(id, sizeof(id), "dock_split_%d", ni);
    f32 v = 0;
    if (a3_ui_splitter(ui, id, sr, n->vertical, &v) && v != 0) {
        f32 total = n->vertical ? n->rect.w - SPLIT_W : n->rect.h - SPLIT_W;
        if (total > 1) n->ratio = a3_clampf(n->ratio + v / total, 0.08f, 0.92f);
    }
    draw_splitters(d, ui, n->child[0]);
    draw_splitters(d, ui, n->child[1]);
}

static void draw_leaf(A3Dock *d, A3Ui *ui, i32 ni, DropTarget *drop) {
    A3DockNode *n = &d->nodes[ni];
    A3UiTheme *th = a3_ui_theme(ui);
    A3Rect r = n->rect;
    if (r.w < 20 || r.h < 20) return;
    /* tab bar */
    A3Rect bar = a3_rect(r.x, r.y, r.w, TAB_H);
    a3_ui_rect_corners(ui, bar, th->colors[A3_UIC_HEADER], th->panel_rounding, 1 | 2);
    a3_ui_push_clip(ui, bar);
    f32 x = r.x + 4;
    char pid[64];
    for (u32 t = 0; t < n->tab_count; ++t) {
        A3DockPanel *p = &d->panels[n->tabs[t]];
        f32 tw = a3_font_text_width(A3_FONT_UI, p->name, -1) + (p->icon ? 40 : 22) + 18;
        A3Rect tr = a3_rect(x, r.y + 4, tw, TAB_H - 4);
        a3_snprintf(pid, sizeof(pid), "dock_tab_%s", p->name);
        A3UiId id = a3_ui_id(ui, pid);
        b32 active = (i32)t == n->active;
        b32 hovered = a3_rect_contains(tr, ui->in.mouse_pos) && a3_rect_contains(a3_ui_clip(ui), ui->in.mouse_pos);
        ui->last_id = id;
        ui->last_rect = tr;
        ui->last_hovered = hovered;
        if (hovered) { ui->hot_next = id; ui->any_hovered = 1; }
        if (hovered && ui->in.mouse_pressed[A3_MOUSE_LEFT] && !ui->popup_count) {
            n->active = (i32)t;
            ui->active = id;
            ui->drag_start_mouse = ui->in.mouse_pos;
            d->focused_panel = n->tabs[t];
        }
        if (hovered && ui->in.mouse_pressed[A3_MOUSE_MIDDLE]) { a3_dock_close_panel(d, n->tabs[t]); a3_ui_pop_clip(ui); return; }
        if (a3_ui_drag_source(ui, "dock_panel", &n->tabs[t], sizeof(i32), p->name)) d->drag_panel = n->tabs[t];
        u32 bg = active ? th->colors[A3_UIC_PANEL] : (hovered ? th->colors[A3_UIC_WIDGET_HOVER] : 0);
        if (bg) a3_ui_rect_corners(ui, tr, bg, 5, 1 | 2);
        if (active) a3_ui_rect(ui, a3_rect(tr.x + 6, tr.y, tr.w - 12, 2), d->focused_panel == n->tabs[t] ? th->colors[A3_UIC_ACCENT] : th->colors[A3_UIC_BORDER], 1);
        f32 tx = tr.x + 10;
        if (p->icon) { a3_ui_icon(ui, p->icon, a3_v2(tx + 7, tr.y + tr.h * 0.5f), active ? th->colors[A3_UIC_ACCENT_HOVER] : th->colors[A3_UIC_TEXT_DIM], A3_FONT_UI); tx += 20; }
        a3_ui_text_in_rect(ui, A3_FONT_UI, a3_rect(tx, tr.y, tr.w - (tx - tr.x) - 20, tr.h), A3_ALIGN_LEFT, active ? th->colors[A3_UIC_TEXT] : th->colors[A3_UIC_TEXT_DIM], p->name);
        /* close button */
        A3Rect cr = a3_rect(tr.x + tr.w - 20, tr.y + (tr.h - 14) * 0.5f, 14, 14);
        if (hovered || active) {
            b32 ch = a3_rect_contains(cr, ui->in.mouse_pos);
            if (ch) a3_ui_rect(ui, cr, th->colors[A3_UIC_WIDGET_ACTIVE], 3);
            a3_ui_icon(ui, A3_ICON_CROSS, a3_v2(cr.x + 7, cr.y + 7), th->colors[A3_UIC_TEXT_DIM], A3_FONT_UI);
            if (ch && ui->in.mouse_pressed[A3_MOUSE_LEFT]) { ui->active = 0; a3_dock_close_panel(d, n->tabs[t]); a3_ui_pop_clip(ui); return; }
        }
        x += tw + 2;
    }
    a3_ui_pop_clip(ui);
    /* content */
    A3Rect content = a3_rect(r.x, r.y + TAB_H, r.w, r.h - TAB_H);
    a3_ui_rect_corners(ui, content, th->colors[A3_UIC_PANEL], th->panel_rounding, 4 | 8);
    if (n->tab_count && n->active >= 0 && n->active < (i32)n->tab_count) {
        i32 pi = n->tabs[n->active];
        A3DockPanel *p = &d->panels[pi];
        if (a3_rect_contains(content, ui->in.mouse_pos) && (ui->in.mouse_pressed[A3_MOUSE_LEFT] || ui->in.mouse_pressed[A3_MOUSE_RIGHT]) && !ui->popup_count)
            d->focused_panel = pi;
        if (p->draw) {
            a3_ui_push_id(ui, p->name);
            if (a3_ui_begin_panel(ui, p->name, content, p->flags | A3_PANEL_NO_BACKGROUND)) p->draw(p->user, ui, a3_ui_panel_rect(ui));
            a3_ui_end_panel(ui);
            a3_ui_pop_id(ui);
        }
    } else {
        a3_ui_text_in_rect(ui, A3_FONT_UI, content, A3_ALIGN_CENTER, th->colors[A3_UIC_TEXT_DISABLED], "Drop a panel here");
    }
    /* drop zones while dragging a tab */
    if (d->drag_panel >= 0 && a3_ui_dragging(ui, "dock_panel") && a3_rect_contains(r, ui->in.mouse_pos)) {
        A3Vec2 m = ui->in.mouse_pos;
        f32 fx = (m.x - r.x) / r.w, fy = (m.y - content.y) / a3_maxf(content.h, 1);
        i32 zone = 0;
        f32 edge = 0.25f;
        if (m.y < r.y + TAB_H) zone = 0;
        else if (fx < edge && fx < fy && fx < 1 - fy) zone = 1;
        else if (fx > 1 - edge && 1 - fx < fy && 1 - fx < 1 - fy) zone = 2;
        else if (fy < edge) zone = 3;
        else if (fy > 1 - edge) zone = 4;
        A3Rect pv = content;
        if (zone == 1) pv.w *= 0.5f;
        else if (zone == 2) { pv.x += pv.w * 0.5f; pv.w *= 0.5f; }
        else if (zone == 3) pv.h *= 0.5f;
        else if (zone == 4) { pv.y += pv.h * 0.5f; pv.h *= 0.5f; }
        drop->node = ni;
        drop->zone = zone;
        drop->preview = pv;
    }
}

static void draw_node(A3Dock *d, A3Ui *ui, i32 ni, DropTarget *drop) {
    A3DockNode *n = &d->nodes[ni];
    if (is_leaf(n)) { draw_leaf(d, ui, ni, drop); return; }
    draw_node(d, ui, n->child[0], drop);
    draw_node(d, ui, n->child[1], drop);
}

void a3_dock_draw(A3Dock *d, A3Ui *ui, A3Rect area) {
    if (d->root < 0 || !d->nodes[d->root].used) a3_dock_reset(d);
    layout(d, d->root, area);
    DropTarget drop = { -1, 0, { 0, 0, 0, 0 } };
    draw_node(d, ui, d->root, &drop);
    draw_splitters(d, ui, d->root);
    if (drop.node >= 0) {
        a3_ui_set_layer(ui, A3_LAYER_OVERLAY);
        A3UiTheme *th = a3_ui_theme(ui);
        a3_ui_rect(ui, drop.preview, a3_color_alpha(th->colors[A3_UIC_ACCENT], 0.25f), 6);
        a3_ui_rect_outline(ui, drop.preview, th->colors[A3_UIC_ACCENT], 6, 2);
        a3_ui_set_layer(ui, A3_LAYER_MAIN);
    }
    /* complete the drop */
    if (d->drag_panel >= 0 && !ui->in.mouse[A3_MOUSE_LEFT]) {
        i32 panel = d->drag_panel;
        d->drag_panel = -1;
        if (drop.node >= 0) {
            u32 t;
            i32 from = node_of_panel(d, panel, &t);
            b32 same_leaf_center = from == drop.node && drop.zone == 0;
            b32 alone_same = from == drop.node && d->nodes[from].tab_count == 1;
            if (!same_leaf_center && !alone_same) {
                remove_tab(d, panel);
                i32 target = drop.node;
                if (!d->nodes[target].used || !is_leaf(&d->nodes[target])) target = largest_leaf(d);
                if (drop.zone == 0) a3_dock_add_tab(d, target, panel);
                else {
                    b32 vertical = drop.zone == 1 || drop.zone == 2;
                    b32 second = drop.zone == 2 || drop.zone == 4;
                    i32 leaf = a3_dock_split(d, target, vertical, second ? 0.65f : 0.35f, second);
                    if (leaf >= 0) a3_dock_add_tab(d, leaf, panel);
                }
                d->focused_panel = panel;
            }
        }
    }
}

/* ---- serialization ---- */

static void save_node(A3Dock *d, i32 ni, A3JsonWriter *jw) {
    A3DockNode *n = &d->nodes[ni];
    a3_jw_begin_object(jw);
    if (is_leaf(n)) {
        a3_jw_key(jw, "tabs");
        a3_jw_begin_array(jw);
        for (u32 t = 0; t < n->tab_count; ++t) a3_jw_string(jw, d->panels[n->tabs[t]].name);
        a3_jw_end_array(jw);
        a3_jw_kv_int(jw, "active", n->active);
    } else {
        a3_jw_kv_bool(jw, "vertical", n->vertical);
        a3_jw_kv_number(jw, "ratio", n->ratio);
        a3_jw_key(jw, "a");
        save_node(d, n->child[0], jw);
        a3_jw_key(jw, "b");
        save_node(d, n->child[1], jw);
    }
    a3_jw_end_object(jw);
}

void a3_dock_save(A3Dock *d, A3StrBuf *out) {
    A3JsonWriter jw;
    a3_jw_init(&jw, out, 0);
    a3_jw_begin_object(&jw);
    a3_jw_kv_string(&jw, "format", "asm3d.layout");
    a3_jw_key(&jw, "root");
    save_node(d, d->root, &jw);
    a3_jw_end_object(&jw);
}

static void load_node(A3Dock *d, i32 ni, const A3Json *j) {
    const A3Json *tabs = a3_json_get(j, "tabs");
    if (tabs) {
        A3_JSON_FOREACH(t, tabs) {
            i32 p = a3_dock_find_panel(d, a3_json_string(t, ""));
            if (p >= 0 && !d->panels[p].open) a3_dock_add_tab(d, ni, p);
        }
        d->nodes[ni].active = (i32)a3_json_get_number(j, "active", 0);
        if (d->nodes[ni].active >= (i32)d->nodes[ni].tab_count) d->nodes[ni].active = 0;
        return;
    }
    b32 vertical = a3_json_get_bool(j, "vertical", 1);
    f32 ratio = (f32)a3_json_get_number(j, "ratio", 0.5);
    i32 a = a3_dock_split(d, ni, vertical, ratio, 0);
    if (a < 0) return;
    i32 b = d->nodes[ni].child[1];
    load_node(d, a, a3_json_get(j, "a"));
    load_node(d, b, a3_json_get(j, "b"));
}

b32 a3_dock_load(A3Dock *d, const char *json, usize len) {
    A3Arena arena;
    a3_arena_init(&arena, A3_MEM_TEMP, A3_KB(16));
    A3Json *root = a3_json_parse(json, len, &arena, 0);
    b32 ok = root && a3_streq(a3_json_get_string(root, "format", ""), "asm3d.layout");
    if (ok) {
        a3_dock_reset(d);
        load_node(d, d->root, a3_json_get(root, "root"));
    }
    a3_arena_release(&arena);
    return ok;
}
