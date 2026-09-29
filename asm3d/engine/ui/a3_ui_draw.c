/*
 * ASM3D - a3_ui_draw.c
 * Anti-aliased 2D drawing: convex fills with 1px feathered edges, strokes,
 * text from the embedded atlas, images; batched into one draw call per
 * texture/clip change and rendered through the RHI.
 */
#include "a3_ui_internal.h"
#include "../core/a3_log.h"
#include "../core/a3_string.h"
#include "../render/a3_shaders.h"

A3Rect a3_rect_intersect(A3Rect a, A3Rect b) {
    f32 x0 = a3_maxf(a.x, b.x), y0 = a3_maxf(a.y, b.y);
    f32 x1 = a3_minf(a.x + a.w, b.x + b.w), y1 = a3_minf(a.y + a.h, b.y + b.h);
    return a3_rect(x0, y0, a3_maxf(x1 - x0, 0), a3_maxf(y1 - y0, 0));
}

u32 a3_color_from_vec4(A3Vec4 c) {
    return a3_rgba((u8)(a3_saturate(c.x) * 255.0f + 0.5f), (u8)(a3_saturate(c.y) * 255.0f + 0.5f),
                   (u8)(a3_saturate(c.z) * 255.0f + 0.5f), (u8)(a3_saturate(c.w) * 255.0f + 0.5f));
}

A3Vec4 a3_color_to_vec4(u32 c) {
    return a3_v4((f32)(c & 0xFF) / 255.0f, (f32)((c >> 8) & 0xFF) / 255.0f, (f32)((c >> 16) & 0xFF) / 255.0f, (f32)(c >> 24) / 255.0f);
}

u32 a3_color_alpha(u32 c, f32 m) {
    u32 a = (u32)((f32)(c >> 24) * a3_saturate(m) + 0.5f);
    return (c & 0x00FFFFFFu) | (a << 24);
}

u32 a3_color_lerp(u32 a, u32 b, f32 t) {
    A3Vec4 x = a3_color_to_vec4(a), y = a3_color_to_vec4(b);
    return a3_color_from_vec4(a3_v4_lerp(x, y, t));
}

UiDrawList *ui_list(A3Ui *ui) { return &ui->lists[ui->layer]; }

void a3_ui_set_layer(A3Ui *ui, A3UiLayer layer) { ui->layer = layer < A3_LAYER_COUNT ? layer : A3_LAYER_MAIN; }
A3UiLayer a3_ui_layer(A3Ui *ui) { return ui->layer; }

static A3Rect full_rect(A3Ui *ui) { return a3_rect(0, 0, (f32)ui->width, (f32)ui->height); }

A3Rect a3_ui_clip(A3Ui *ui) {
    UiDrawList *l = ui_list(ui);
    return l->clip_depth ? l->clip_stack[l->clip_depth - 1] : full_rect(ui);
}

/* Starts a new command when texture/mode/clip change. */
void ui_prim_reserve_cmd(A3Ui *ui, u32 tex, u32 mode) {
    UiDrawList *l = ui_list(ui);
    A3Rect clip = a3_ui_clip(ui);
    if (l->cmds.count) {
        UiCmd *c = &a3_array_last(l->cmds);
        if (c->tex == tex && c->mode == mode && c->clip.x == clip.x && c->clip.y == clip.y && c->clip.w == clip.w && c->clip.h == clip.h) return;
        if (c->count == 0) { c->tex = tex; c->mode = mode; c->clip = clip; c->first = l->i.count; return; }
    }
    UiCmd nc = { l->i.count, 0, clip, tex, mode };
    a3_array_push(l->cmds, nc, A3_MEM_UI);
}

void a3_ui_push_clip(A3Ui *ui, A3Rect r) {
    UiDrawList *l = ui_list(ui);
    A3Rect cur = a3_ui_clip(ui);
    if (l->clip_depth < 64) l->clip_stack[l->clip_depth++] = a3_rect_intersect(cur, r);
}

void a3_ui_pop_clip(A3Ui *ui) {
    UiDrawList *l = ui_list(ui);
    if (l->clip_depth) l->clip_depth--;
}

static b32 clipped_out(A3Ui *ui, A3Rect r) {
    A3Rect c = a3_ui_clip(ui);
    return r.x > c.x + c.w || r.y > c.y + c.h || r.x + r.w < c.x || r.y + r.h < c.y || c.w <= 0 || c.h <= 0;
}

static u32 push_vtx(UiDrawList *l, f32 x, f32 y, f32 u, f32 v, u32 col) {
    UiVertex vx = { x, y, u, v, col };
    a3_array_push(l->v, vx, A3_MEM_UI);
    return l->v.count - 1;
}

static void push_tri(UiDrawList *l, u32 a, u32 b, u32 c) {
    if (!a3_array_reserve(l->i, l->i.count + 3, A3_MEM_UI)) return;
    l->i.data[l->i.count++] = a;
    l->i.data[l->i.count++] = b;
    l->i.data[l->i.count++] = c;
    a3_array_last(l->cmds).count += 3;
}

/* Convex polygon fill with a 1px anti-aliased fringe. */
static void fill_convex(A3Ui *ui, const A3Vec2 *p, u32 n, u32 col) {
    if (n < 3 || !(col >> 24)) return;
    ui_prim_reserve_cmd(ui, 0, 1);
    UiDrawList *l = ui_list(ui);
    f32 u = ui->white_u, v = ui->white_v;
    u32 col_t = col & 0x00FFFFFFu;
    A3Vec2 normals[128];
    if (n > 128) n = 128;
    for (u32 i = 0; i < n; ++i) {
        A3Vec2 a = p[i], b = p[(i + 1) % n];
        A3Vec2 d = a3_v2_norm(a3_v2_sub(b, a));
        normals[i] = a3_v2(d.y, -d.x);
    }
    u32 base = l->v.count;
    for (u32 i = 0; i < n; ++i) {
        A3Vec2 n0 = normals[(i + n - 1) % n], n1 = normals[i];
        A3Vec2 dm = a3_v2_scale(a3_v2_add(n0, n1), 0.5f);
        f32 d2 = a3_v2_dot(dm, dm);
        if (d2 > 1e-6f) dm = a3_v2_scale(dm, 1.0f / (d2 < 0.5f ? 0.5f : d2));
        dm = a3_v2_scale(dm, 0.5f);
        push_vtx(l, p[i].x - dm.x, p[i].y - dm.y, u, v, col);    /* inner */
        push_vtx(l, p[i].x + dm.x, p[i].y + dm.y, u, v, col_t);  /* outer (transparent) */
    }
    for (u32 i = 2; i < n; ++i) push_tri(l, base, base + (i - 1) * 2, base + i * 2);
    for (u32 i = 0; i < n; ++i) {
        u32 i0 = i, i1 = (i + 1) % n;
        push_tri(l, base + i0 * 2, base + i1 * 2, base + i1 * 2 + 1);
        push_tri(l, base + i0 * 2, base + i1 * 2 + 1, base + i0 * 2 + 1);
    }
}

static u32 rounded_path(A3Vec2 *out, A3Rect r, f32 rad, u32 corners) {
    rad = a3_minf(rad, a3_minf(r.w, r.h) * 0.5f);
    if (rad < 0.5f) corners = 0;
    u32 n = 0;
    const int SEG = rad > 6 ? 6 : 4;
    struct { f32 cx, cy, a0; u32 bit; } cs[4] = {
        { r.x + r.w - rad, r.y + rad, -A3_HALF_PI, 2 },       /* top right */
        { r.x + r.w - rad, r.y + r.h - rad, 0, 4 },           /* bottom right */
        { r.x + rad, r.y + r.h - rad, A3_HALF_PI, 8 },        /* bottom left */
        { r.x + rad, r.y + rad, A3_PI, 1 },                   /* top left */
    };
    A3Vec2 sq[4] = { { r.x + r.w, r.y }, { r.x + r.w, r.y + r.h }, { r.x, r.y + r.h }, { r.x, r.y } };
    for (int c = 0; c < 4; ++c) {
        if (corners & cs[c].bit) {
            for (int s = 0; s <= SEG; ++s) {
                f32 a = cs[c].a0 + A3_HALF_PI * (f32)s / (f32)SEG, sn, cn;
                a3_sincosf(a, &sn, &cn);
                out[n++] = a3_v2(cs[c].cx + cn * rad, cs[c].cy + sn * rad);
            }
        } else {
            out[n++] = sq[c];
        }
    }
    return n;
}

void a3_ui_rect_corners(A3Ui *ui, A3Rect r, u32 col, f32 rounding, u32 corners) {
    if (r.w <= 0 || r.h <= 0 || clipped_out(ui, r)) return;
    A3Vec2 pts[40];
    u32 n = rounded_path(pts, r, rounding, corners);
    fill_convex(ui, pts, n, col);
}

void a3_ui_rect(A3Ui *ui, A3Rect r, u32 col, f32 rounding) { a3_ui_rect_corners(ui, r, col, rounding, 15); }

static void stroke_segment(A3Ui *ui, A3Vec2 a, A3Vec2 b, u32 col, f32 t) {
    UiDrawList *l = ui_list(ui);
    A3Vec2 d = a3_v2_sub(b, a);
    f32 len = a3_v2_len(d);
    if (len < 1e-4f) return;
    d = a3_v2_scale(d, 1.0f / len);
    A3Vec2 n = a3_v2(-d.y, d.x);
    f32 core = a3_maxf(t - 1.0f, 0.0f) * 0.5f, edge = core + 1.0f;
    u32 ct = col & 0x00FFFFFFu;
    f32 u = ui->white_u, v = ui->white_v;
    /* extend ends slightly so consecutive segments overlap */
    A3Vec2 a2 = a3_v2_sub(a, a3_v2_scale(d, 0.5f)), b2 = a3_v2_add(b, a3_v2_scale(d, 0.5f));
    u32 base = l->v.count;
    A3Vec2 pts[2] = { a2, b2 };
    for (int k = 0; k < 2; ++k) {
        A3Vec2 p = pts[k];
        push_vtx(l, p.x + n.x * edge, p.y + n.y * edge, u, v, ct);
        push_vtx(l, p.x + n.x * core, p.y + n.y * core, u, v, col);
        push_vtx(l, p.x - n.x * core, p.y - n.y * core, u, v, col);
        push_vtx(l, p.x - n.x * edge, p.y - n.y * edge, u, v, ct);
    }
    for (u32 s = 0; s < 3; ++s) {
        push_tri(l, base + s, base + 4 + s, base + 4 + s + 1);
        push_tri(l, base + s, base + 4 + s + 1, base + s + 1);
    }
}

void a3_ui_line(A3Ui *ui, A3Vec2 a, A3Vec2 b, u32 col, f32 t) {
    if (!(col >> 24)) return;
    A3Rect bb = a3_rect(a3_minf(a.x, b.x) - t, a3_minf(a.y, b.y) - t, a3_absf(a.x - b.x) + 2 * t, a3_absf(a.y - b.y) + 2 * t);
    if (clipped_out(ui, bb)) return;
    ui_prim_reserve_cmd(ui, 0, 1);
    stroke_segment(ui, a, b, col, t);
}

void a3_ui_polyline(A3Ui *ui, const A3Vec2 *p, u32 n, u32 col, f32 t, b32 closed) {
    if (n < 2) return;
    ui_prim_reserve_cmd(ui, 0, 1);
    for (u32 i = 0; i + 1 < n; ++i) stroke_segment(ui, p[i], p[i + 1], col, t);
    if (closed) stroke_segment(ui, p[n - 1], p[0], col, t);
}

void a3_ui_rect_outline(A3Ui *ui, A3Rect r, u32 col, f32 rounding, f32 t) {
    if (r.w <= 0 || r.h <= 0 || clipped_out(ui, a3_rect(r.x - 2, r.y - 2, r.w + 4, r.h + 4))) return;
    A3Vec2 pts[40];
    A3Rect ri = a3_rect(r.x + t * 0.5f, r.y + t * 0.5f, r.w - t, r.h - t);
    u32 n = rounded_path(pts, ri, rounding, 15);
    a3_ui_polyline(ui, pts, n, col, t, 1);
}

void a3_ui_rect_gradient(A3Ui *ui, A3Rect r, u32 top, u32 bottom) {
    if (r.w <= 0 || r.h <= 0 || clipped_out(ui, r)) return;
    ui_prim_reserve_cmd(ui, 0, 1);
    UiDrawList *l = ui_list(ui);
    f32 u = ui->white_u, v = ui->white_v;
    u32 b = l->v.count;
    push_vtx(l, r.x, r.y, u, v, top);
    push_vtx(l, r.x + r.w, r.y, u, v, top);
    push_vtx(l, r.x + r.w, r.y + r.h, u, v, bottom);
    push_vtx(l, r.x, r.y + r.h, u, v, bottom);
    push_tri(l, b, b + 1, b + 2);
    push_tri(l, b, b + 2, b + 3);
}

void a3_ui_rect_gradient_h(A3Ui *ui, A3Rect r, u32 left, u32 right) {
    if (r.w <= 0 || r.h <= 0 || clipped_out(ui, r)) return;
    ui_prim_reserve_cmd(ui, 0, 1);
    UiDrawList *l = ui_list(ui);
    f32 u = ui->white_u, v = ui->white_v;
    u32 b = l->v.count;
    push_vtx(l, r.x, r.y, u, v, left);
    push_vtx(l, r.x + r.w, r.y, u, v, right);
    push_vtx(l, r.x + r.w, r.y + r.h, u, v, right);
    push_vtx(l, r.x, r.y + r.h, u, v, left);
    push_tri(l, b, b + 1, b + 2);
    push_tri(l, b, b + 2, b + 3);
}

void a3_ui_shadow(A3Ui *ui, A3Rect r, f32 size, f32 rounding, u32 col) {
    /* layered translucent rects approximate a soft shadow */
    const int steps = 5;
    for (int i = steps; i >= 1; --i) {
        f32 k = (f32)i / (f32)steps;
        f32 s = size * k;
        a3_ui_rect(ui, a3_rect(r.x - s * 0.5f, r.y - s * 0.3f + size * 0.3f, r.w + s, r.h + s), a3_color_alpha(col, (1.0f - k) * 0.35f + 0.05f), rounding + s);
    }
}

void a3_ui_triangle(A3Ui *ui, A3Vec2 a, A3Vec2 b, A3Vec2 c, u32 col) {
    A3Vec2 p[3] = { a, b, c };
    /* ensure clockwise order in screen space for the fringe normals */
    f32 cross = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (cross < 0) { p[1] = c; p[2] = b; }
    fill_convex(ui, p, 3, col);
}

void a3_ui_circle(A3Ui *ui, A3Vec2 c, f32 r, u32 col) {
    if (clipped_out(ui, a3_rect(c.x - r, c.y - r, 2 * r, 2 * r))) return;
    A3Vec2 p[48];
    u32 n = r < 5 ? 12 : (r < 16 ? 24 : 40);
    for (u32 i = 0; i < n; ++i) {
        f32 s, co;
        a3_sincosf(A3_TAU * (f32)i / (f32)n, &s, &co);
        p[i] = a3_v2(c.x + co * r, c.y + s * r);
    }
    fill_convex(ui, p, n, col);
}

void a3_ui_circle_outline(A3Ui *ui, A3Vec2 c, f32 r, u32 col, f32 t) {
    A3Vec2 p[48];
    u32 n = r < 16 ? 24 : 40;
    for (u32 i = 0; i < n; ++i) {
        f32 s, co;
        a3_sincosf(A3_TAU * (f32)i / (f32)n, &s, &co);
        p[i] = a3_v2(c.x + co * r, c.y + s * r);
    }
    a3_ui_polyline(ui, p, n, col, t, 1);
}

void a3_ui_bezier(A3Ui *ui, A3Vec2 p0, A3Vec2 p1, A3Vec2 p2, A3Vec2 p3, u32 col, f32 t) {
    A3Vec2 pts[33];
    for (int i = 0; i <= 32; ++i) {
        f32 s = (f32)i / 32.0f, is = 1.0f - s;
        f32 a = is * is * is, b = 3 * is * is * s, c = 3 * is * s * s, d = s * s * s;
        pts[i] = a3_v2(a * p0.x + b * p1.x + c * p2.x + d * p3.x, a * p0.y + b * p1.y + c * p2.y + d * p3.y);
    }
    a3_ui_polyline(ui, pts, 33, col, t, 0);
}

static f32 text_impl(A3Ui *ui, A3FontId font, A3Vec2 pos, u32 col, const char *text, i32 len, f32 scale) {
    if (!text) return 0;
    f32 x = scale == 1.0f ? a3_floorf(pos.x + 0.5f) : pos.x, y = scale == 1.0f ? a3_floorf(pos.y + 0.5f) : pos.y;
    f32 start = x;
    f32 lh = a3_font_line_height(font) * scale;
    A3Rect clip = a3_ui_clip(ui);
    if (y > clip.y + clip.h || y + lh < clip.y) return a3_font_text_width(font, text, len) * scale;
    ui_prim_reserve_cmd(ui, 0, 1);
    UiDrawList *l = ui_list(ui);
    f32 iw = 1.0f / (f32)a3_font_atlas_width, ih = 1.0f / (f32)a3_font_atlas_height;
    const char *end = len < 0 ? text + a3_strlen(text) : text + len;
    while (text < end && *text) {
        u32 cp;
        text += a3_utf8_decode(text, &cp);
        if (cp == '\t') { x += a3_font_glyph(font, ' ')->advance * 4 * scale; continue; }
        if (cp == '\n') continue;
        const A3GlyphData *g = a3_font_glyph(font, cp);
        if (g->w && x + g->xoff * scale < clip.x + clip.w && x + (g->xoff + g->w) * scale > clip.x) {
            f32 x0 = x + g->xoff * scale, y0 = y + g->yoff * scale, x1 = x0 + g->w * scale, y1 = y0 + g->h * scale;
            f32 u0 = g->x * iw, v0 = g->y * ih, u1 = (g->x + g->w) * iw, v1 = (g->y + g->h) * ih;
            u32 b = l->v.count;
            push_vtx(l, x0, y0, u0, v0, col);
            push_vtx(l, x1, y0, u1, v0, col);
            push_vtx(l, x1, y1, u1, v1, col);
            push_vtx(l, x0, y1, u0, v1, col);
            push_tri(l, b, b + 1, b + 2);
            push_tri(l, b, b + 2, b + 3);
        }
        x += g->advance * scale;
    }
    return x - start;
}

f32 a3_ui_text_n(A3Ui *ui, A3FontId font, A3Vec2 pos, u32 col, const char *text, i32 len) { return text_impl(ui, font, pos, col, text, len, 1.0f); }
f32 a3_ui_text_scaled(A3Ui *ui, A3FontId font, A3Vec2 pos, u32 col, const char *text, f32 scale) { return text_impl(ui, font, pos, col, text, -1, scale > 0 ? scale : 1.0f); }

f32 a3_ui_text(A3Ui *ui, A3FontId font, A3Vec2 pos, u32 col, const char *text) { return a3_ui_text_n(ui, font, pos, col, text, -1); }

void a3_ui_text_in_rect(A3Ui *ui, A3FontId font, A3Rect r, A3Align align, u32 col, const char *text) {
    f32 w = a3_font_text_width(font, text, -1);
    f32 lh = a3_font_line_height(font);
    f32 x = r.x;
    if (align == A3_ALIGN_CENTER) x = r.x + (r.w - w) * 0.5f;
    else if (align == A3_ALIGN_RIGHT) x = r.x + r.w - w;
    f32 y = r.y + (r.h - lh) * 0.5f + 1.0f;
    b32 overflow = w > r.w + 0.5f;
    if (overflow) {
        a3_ui_push_clip(ui, r);
        x = r.x;
    }
    a3_ui_text(ui, font, a3_v2(x, y), col, text);
    if (overflow) a3_ui_pop_clip(ui);
}

/* Word wrap: breaks at spaces; also honours '\n'. */
static f32 wrap_text(A3Ui *ui, A3FontId font, A3Rect r, u32 col, const char *text, b32 draw) {
    f32 lh = a3_font_line_height(font);
    f32 y = r.y;
    const char *p = text;
    while (p && *p) {
        const char *line = p, *last_break = 0;
        f32 w = 0;
        while (*p && *p != '\n') {
            u32 cp;
            u32 n = a3_utf8_decode(p, &cp);
            f32 adv = a3_font_glyph(font, cp)->advance;
            if (w + adv > r.w && last_break) { p = last_break; break; }
            if (cp == ' ') last_break = p;
            w += adv;
            p += n;
        }
        const char *line_end = p;
        if (draw) a3_ui_text_n(ui, font, a3_v2(r.x, y), col, line, (i32)(line_end - line));
        y += lh;
        if (*p == '\n' || *p == ' ') ++p;
    }
    return y - r.y;
}

f32 a3_ui_text_wrapped(A3Ui *ui, A3FontId font, A3Rect r, u32 col, const char *text) { return wrap_text(ui, font, r, col, text, 1); }
f32 a3_ui_text_wrapped_height(A3FontId font, f32 width, const char *text) { return wrap_text(0, font, a3_rect(0, 0, width, 0), 0, text, 0); }

void a3_ui_icon(A3Ui *ui, u32 icon, A3Vec2 c, u32 col, A3FontId font) {
    const A3GlyphData *g = a3_font_glyph(font, icon);
    f32 asc = a3_font_ascent(font);
    A3Vec2 p = a3_v2(c.x - g->advance * 0.5f, c.y - (g->yoff + g->h * 0.5f));
    A3_UNUSED(asc);
    char buf[4];
    u32 n = a3_utf8_encode(icon, buf);
    a3_ui_text_n(ui, font, p, col, buf, (i32)n);
}

void a3_ui_image(A3Ui *ui, A3RhiTexture tex, A3Rect r, A3Vec2 uv0, A3Vec2 uv1, u32 tint) {
    if (!tex.id || clipped_out(ui, r)) return;
    ui_prim_reserve_cmd(ui, tex.id, 0);
    UiDrawList *l = ui_list(ui);
    u32 b = l->v.count;
    push_vtx(l, r.x, r.y, uv0.x, uv0.y, tint);
    push_vtx(l, r.x + r.w, r.y, uv1.x, uv0.y, tint);
    push_vtx(l, r.x + r.w, r.y + r.h, uv1.x, uv1.y, tint);
    push_vtx(l, r.x, r.y + r.h, uv0.x, uv1.y, tint);
    push_tri(l, b, b + 1, b + 2);
    push_tri(l, b, b + 2, b + 3);
}

/* ---- procedural icons ---- */
void a3_ui_draw_icon_search(A3Ui *ui, A3Vec2 c, f32 s, u32 col) {
    a3_ui_circle_outline(ui, a3_v2(c.x - s * 0.1f, c.y - s * 0.1f), s * 0.3f, col, 1.5f);
    a3_ui_line(ui, a3_v2(c.x + s * 0.12f, c.y + s * 0.12f), a3_v2(c.x + s * 0.38f, c.y + s * 0.38f), col, 2.0f);
}

void a3_ui_draw_icon_pause(A3Ui *ui, A3Vec2 c, f32 s, u32 col) {
    a3_ui_rect(ui, a3_rect(c.x - s * 0.3f, c.y - s * 0.35f, s * 0.2f, s * 0.7f), col, 1);
    a3_ui_rect(ui, a3_rect(c.x + s * 0.1f, c.y - s * 0.35f, s * 0.2f, s * 0.7f), col, 1);
}

void a3_ui_draw_icon_step(A3Ui *ui, A3Vec2 c, f32 s, u32 col) {
    a3_ui_triangle(ui, a3_v2(c.x - s * 0.3f, c.y - s * 0.35f), a3_v2(c.x + s * 0.15f, c.y), a3_v2(c.x - s * 0.3f, c.y + s * 0.35f), col);
    a3_ui_rect(ui, a3_rect(c.x + s * 0.18f, c.y - s * 0.35f, s * 0.14f, s * 0.7f), col, 1);
}

void a3_ui_draw_icon_folder(A3Ui *ui, A3Vec2 c, f32 s, u32 col) {
    a3_ui_rect(ui, a3_rect(c.x - s * 0.45f, c.y - s * 0.32f, s * 0.4f, s * 0.2f), col, 2);
    a3_ui_rect(ui, a3_rect(c.x - s * 0.45f, c.y - s * 0.22f, s * 0.9f, s * 0.6f), col, 3);
}

void a3_ui_draw_icon_file(A3Ui *ui, A3Vec2 c, f32 s, u32 col) {
    A3Rect r = a3_rect(c.x - s * 0.32f, c.y - s * 0.42f, s * 0.64f, s * 0.84f);
    a3_ui_rect(ui, r, col, 2);
    a3_ui_triangle(ui, a3_v2(r.x + r.w - s * 0.22f, r.y), a3_v2(r.x + r.w, r.y), a3_v2(r.x + r.w, r.y + s * 0.22f), a3_color_alpha(0xFF000000u, 0.35f));
}

/* ======================================================================== */
/* GPU                                                                      */
/* ======================================================================== */

void ui_draw_init(A3Ui *ui) {
    ui->white_u = 2.0f / (f32)a3_font_atlas_width;
    ui->white_v = 2.0f / (f32)a3_font_atlas_height;
    if (!a3_rhi_info()->api[0]) return; /* headless */
    u8 *alpha = a3_font_decode_atlas();
    if (!alpha) return;
    A3TextureDesc d;
    a3_zero_struct(&d);
    d.width = a3_font_atlas_width;
    d.height = a3_font_atlas_height;
    d.format = A3_TEX_R8;
    d.filter = A3_FILTER_LINEAR;
    d.wrap = A3_WRAP_CLAMP;
    d.data = alpha;
    d.debug_name = "ui_font_atlas";
    ui->atlas = a3_rhi_texture_create(&d);
    a3_free(alpha);
    A3ShaderCompileResult res;
    ui->shader = a3_rhi_shader_create(A3_SHADER_UI_VS, A3_SHADER_UI_FS, "ui", &res);
    ui->vb = a3_rhi_buffer_create(A3_BUFFER_VERTEX, sizeof(UiVertex) * 4096, 0, 1);
    ui->ib = a3_rhi_buffer_create(A3_BUFFER_INDEX, sizeof(u32) * 8192, 0, 1);
    A3VertexLayout l;
    a3_zero_struct(&l);
    l.stride = sizeof(UiVertex);
    l.attribs[0] = (A3VertexAttrib){ 0, 2, A3_ATTR_FLOAT, 0, 0 };
    l.attribs[1] = (A3VertexAttrib){ 2, 2, A3_ATTR_FLOAT, 8, 0 };
    l.attribs[2] = (A3VertexAttrib){ 3, 4, A3_ATTR_UBYTE_NORM, 16, 0 };
    l.count = 3;
    ui->mesh = a3_rhi_mesh_create(ui->vb, &l, ui->ib, 1);
    ui->gpu_ok = ui->atlas.id && ui->shader.id && ui->mesh.id;
    if (!ui->gpu_ok) A3_ERROR("ui", "UI renderer failed to initialize");
}

void ui_draw_shutdown(A3Ui *ui) {
    for (int i = 0; i < A3_LAYER_COUNT; ++i) {
        a3_array_free(ui->lists[i].v);
        a3_array_free(ui->lists[i].i);
        a3_array_free(ui->lists[i].cmds);
    }
    if (ui->mesh.id) a3_rhi_mesh_destroy(ui->mesh);
    if (ui->vb.id) a3_rhi_buffer_destroy(ui->vb);
    if (ui->ib.id) a3_rhi_buffer_destroy(ui->ib);
    if (ui->atlas.id) a3_rhi_texture_destroy(ui->atlas);
    if (ui->shader.id) a3_rhi_shader_destroy(ui->shader);
}

void ui_draw_reset(A3Ui *ui) {
    for (int i = 0; i < A3_LAYER_COUNT; ++i) {
        UiDrawList *l = &ui->lists[i];
        a3_array_clear(l->v);
        a3_array_clear(l->i);
        a3_array_clear(l->cmds);
        l->clip_depth = 0;
    }
}

u32 a3_ui_vertex_count(A3Ui *ui) {
    u32 n = 0;
    for (int i = 0; i < A3_LAYER_COUNT; ++i) n += ui->lists[i].v.count;
    return n;
}

void a3_ui_render(A3Ui *ui) {
    if (!ui || !ui->gpu_ok) return;
    /* merge layers into one vertex/index stream */
    u32 total_v = 0, total_i = 0;
    for (int i = 0; i < A3_LAYER_COUNT; ++i) { total_v += ui->lists[i].v.count; total_i += ui->lists[i].i.count; }
    if (!total_i) return;
    A3ArenaMark mark = a3_scratch_begin();
    UiVertex *vs = A3_ARENA_PUSH_ARRAY(a3_scratch(), UiVertex, total_v);
    u32 *is = A3_ARENA_PUSH_ARRAY(a3_scratch(), u32, total_i);
    if (!vs || !is) { a3_scratch_end(mark); return; }
    u32 vo = 0, io = 0;
    u32 layer_first_index[A3_LAYER_COUNT];
    for (int L = 0; L < A3_LAYER_COUNT; ++L) {
        UiDrawList *l = &ui->lists[L];
        layer_first_index[L] = io;
        a3_memcpy(vs + vo, l->v.data, sizeof(UiVertex) * l->v.count);
        for (u32 k = 0; k < l->i.count; ++k) is[io + k] = l->i.data[k] + vo;
        vo += l->v.count;
        io += l->i.count;
    }
    a3_rhi_buffer_upload(ui->vb, sizeof(UiVertex) * total_v, vs);
    a3_rhi_buffer_upload(ui->ib, sizeof(u32) * total_i, is);
    a3_scratch_end(mark);
    a3_rhi_shader_bind(ui->shader);
    a3_rhi_set_vec2(ui->shader, "u_screen", a3_v2((f32)ui->width, (f32)ui->height));
    a3_rhi_set_int(ui->shader, "u_tex", 0);
    for (int L = 0; L < A3_LAYER_COUNT; ++L) {
        UiDrawList *l = &ui->lists[L];
        for (u32 c = 0; c < l->cmds.count; ++c) {
            UiCmd *cmd = &l->cmds.data[c];
            if (!cmd->count || cmd->clip.w <= 0 || cmd->clip.h <= 0) continue;
            A3RenderState st;
            a3_zero_struct(&st);
            st.blend = A3_BLEND_ALPHA;
            st.cull = A3_CULL_NONE;
            st.depth = A3_DEPTH_OFF;
            st.scissor = 1;
            st.scissor_rect[0] = (i32)cmd->clip.x;
            st.scissor_rect[1] = ui->height - (i32)(cmd->clip.y + cmd->clip.h);
            st.scissor_rect[2] = (i32)(cmd->clip.w + 0.999f);
            st.scissor_rect[3] = (i32)(cmd->clip.h + 0.999f);
            a3_rhi_set_state(&st);
            a3_rhi_set_int(ui->shader, "u_mode", (i32)cmd->mode);
            A3RhiTexture t = cmd->tex ? (A3RhiTexture){ cmd->tex } : ui->atlas;
            a3_rhi_bind_texture(0, t);
            a3_rhi_draw(ui->mesh, A3_PRIM_TRIANGLES, layer_first_index[L] + cmd->first, cmd->count, 1);
        }
    }
    A3RenderState reset;
    a3_zero_struct(&reset);
    reset.cull = A3_CULL_NONE;
    reset.depth = A3_DEPTH_OFF;
    a3_rhi_set_state(&reset);
}
