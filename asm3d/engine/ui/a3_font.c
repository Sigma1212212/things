/*
 * ASM3D - a3_font.c
 */
#include "a3_font.h"
#include "../core/a3_memory.h"
#include "../core/a3_string.h"

static i16 g_lut[A3_FONT_COUNT][256]; /* codepoint (0..255) -> glyph index */
static b32 g_lut_ready;

static void build_lut(void) {
    for (int f = 0; f < A3_FONT_COUNT; ++f) {
        for (int i = 0; i < 256; ++i) g_lut[f][i] = -1;
        const A3FontFaceData *fd = &a3_font_faces[f];
        for (u32 g = fd->first_glyph; g < fd->first_glyph + fd->glyph_count; ++g)
            if (a3_font_glyphs[g].codepoint < 256) g_lut[f][a3_font_glyphs[g].codepoint] = (i16)g;
    }
    g_lut_ready = 1;
}

u8 *a3_font_decode_atlas(void) {
    usize total = (usize)a3_font_atlas_width * (usize)a3_font_atlas_height;
    u8 *out = (u8 *)a3_calloc(total, A3_MEM_UI);
    if (!out) return 0;
    usize o = 0;
    for (u32 i = 0; i < a3_font_atlas_rle_size && o < total;) {
        u8 b = a3_font_atlas_rle[i++];
        if (b >= 128) { o += (usize)(b - 127); continue; }
        u32 n = (u32)b + 1;
        for (u32 k = 0; k < n && i < a3_font_atlas_rle_size && o < total; ++k) out[o++] = a3_font_atlas_rle[i++];
    }
    return out;
}

const A3GlyphData *a3_font_glyph(A3FontId font, u32 cp) {
    if (!g_lut_ready) build_lut();
    if ((u32)font >= A3_FONT_COUNT) font = A3_FONT_UI;
    if (cp < 256) {
        i16 g = g_lut[font][cp];
        if (g >= 0) return &a3_font_glyphs[g];
    } else {
        const A3FontFaceData *fd = &a3_font_faces[font];
        for (u32 g = fd->first_glyph; g < fd->first_glyph + fd->glyph_count; ++g)
            if (a3_font_glyphs[g].codepoint == cp) return &a3_font_glyphs[g];
    }
    i16 q = g_lut[font]['?'];
    return q >= 0 ? &a3_font_glyphs[q] : &a3_font_glyphs[0];
}

f32 a3_font_line_height(A3FontId font) {
    const A3FontFaceData *f = &a3_font_faces[(u32)font < A3_FONT_COUNT ? font : 0];
    return (f32)(f->ascent + f->descent) + 2.0f;
}

f32 a3_font_ascent(A3FontId font) { return (f32)a3_font_faces[(u32)font < A3_FONT_COUNT ? font : 0].ascent; }

u32 a3_utf8_decode(const char *s, u32 *out) {
    const u8 *p = (const u8 *)s;
    if (p[0] < 0x80) { *out = p[0]; return 1; }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) { *out = ((u32)(p[0] & 0x1F) << 6) | (p[1] & 0x3F); return 2; }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *out = ((u32)(p[0] & 0x0F) << 12) | ((u32)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        return 3;
    }
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        *out = ((u32)(p[0] & 0x07) << 18) | ((u32)(p[1] & 0x3F) << 12) | ((u32)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        return 4;
    }
    *out = '?';
    return 1;
}

u32 a3_utf8_encode(u32 cp, char out[4]) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

f32 a3_font_text_width(A3FontId font, const char *text, i32 len) {
    if (!text) return 0;
    f32 w = 0;
    const char *end = len < 0 ? text + a3_strlen(text) : text + len;
    while (text < end && *text) {
        u32 cp;
        text += a3_utf8_decode(text, &cp);
        if (cp == '\t') { w += a3_font_glyph(font, ' ')->advance * 4; continue; }
        w += a3_font_glyph(font, cp)->advance;
    }
    return w;
}
