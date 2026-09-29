/*
 * ASM3D - a3_font.h
 * Embedded bitmap fonts (generated atlas) and text measurement.
 */
#ifndef A3_FONT_H
#define A3_FONT_H

#include "../core/a3_base.h"

A3_EXTERN_C_BEGIN

typedef enum A3FontId {
    A3_FONT_UI = 0,
    A3_FONT_UI_LARGE,
    A3_FONT_TITLE,
    A3_FONT_UI_BOLD,
    A3_FONT_HEADING,
    A3_FONT_MONO,
    A3_FONT_MONO_LARGE,
    A3_FONT_COUNT
} A3FontId;

typedef struct A3FontFaceData { i32 size, ascent, descent; u32 first_glyph, glyph_count; } A3FontFaceData;
typedef struct A3GlyphData { u32 codepoint; u16 x, y, w, h; i16 xoff, yoff; f32 advance; } A3GlyphData;

extern const i32 a3_font_atlas_width, a3_font_atlas_height;
extern const A3FontFaceData a3_font_faces[A3_FONT_COUNT];
extern const u32 a3_font_glyph_count;
extern const A3GlyphData a3_font_glyphs[];
extern const u32 a3_font_atlas_rle_size;
extern const u8 a3_font_atlas_rle[];

/* Decodes the atlas into an 8-bit alpha buffer (a3_malloc'd, w*h bytes). */
u8 *a3_font_decode_atlas(void);
/* Glyph lookup (falls back to '?' for missing codepoints). */
const A3GlyphData *a3_font_glyph(A3FontId font, u32 codepoint);
f32  a3_font_line_height(A3FontId font);
f32  a3_font_ascent(A3FontId font);
/* Width of UTF-8 text (up to len bytes; len < 0 means NUL terminated). */
f32  a3_font_text_width(A3FontId font, const char *text, i32 len);
/* Decodes one UTF-8 codepoint, returns bytes consumed (>= 1). */
u32  a3_utf8_decode(const char *s, u32 *out_cp);
u32  a3_utf8_encode(u32 cp, char out[4]);

/* Icon codepoints available in every font. */
#define A3_ICON_PLAY 0x25B6u
#define A3_ICON_STOP 0x25A0u
#define A3_ICON_DOT 0x25CFu
#define A3_ICON_CIRCLE 0x25CBu
#define A3_ICON_CHECK 0x2713u
#define A3_ICON_CROSS 0x2715u
#define A3_ICON_GEAR 0x2699u
#define A3_ICON_SUN 0x2600u
#define A3_ICON_STAR 0x2605u
#define A3_ICON_DOWN 0x25BCu
#define A3_ICON_RIGHT 0x25BAu
#define A3_ICON_LEFT 0x25C0u
#define A3_ICON_UP 0x25B2u
#define A3_ICON_TRI_RIGHT 0x25B8u
#define A3_ICON_TRI_DOWN 0x25BEu
#define A3_ICON_HOME 0x2302u
#define A3_ICON_PENCIL 0x270Eu
#define A3_ICON_MENU 0x2630u
#define A3_ICON_PLUS_CIRCLE 0x2295u
#define A3_ICON_PLUS_BOX 0x229Eu
#define A3_ICON_REFRESH 0x21BBu
#define A3_ICON_UNDO 0x21B6u
#define A3_ICON_REDO 0x21B7u
#define A3_ICON_PLUS 0x271Au
#define A3_ICON_WARNING 0x26A0u
#define A3_ICON_BOX 0x25A3u
#define A3_ICON_DIAMOND 0x25C6u
#define A3_ICON_DIAMOND_O 0x25C7u
#define A3_ICON_MUSIC 0x266Au
#define A3_ICON_CLOUD 0x2601u
#define A3_ICON_BOLT 0x26A1u
#define A3_ICON_PAUSE 0x2016u
#define A3_ICON_ARROW_RIGHT 0x2192u
#define A3_ICON_ARROW_LEFT 0x2190u
#define A3_ICON_ARROW_UP 0x2191u
#define A3_ICON_ARROW_DOWN 0x2193u
#define A3_ICON_MOVE_H 0x2194u
#define A3_ICON_MOVE_V 0x2195u
#define A3_ICON_SWAP 0x21C4u
#define A3_ICON_BULLET 0x2022u
#define A3_ICON_ELLIPSIS 0x2026u
#define A3_ICON_HAMBURGER 0x2261u
#define A3_ICON_COMMAND 0x2318u
#define A3_ICON_RING 0x25EFu
#define A3_ICON_HEART 0x2764u
#define A3_ICON_LIGHT 0x263Cu
#define A3_ICON_TARGET 0x2609u
#define A3_ICON_HEX 0x2B21u
#define A3_ICON_SQUARE_O 0x25A1u
#define A3_ICON_SMALL_SQUARE 0x25ABu
#define A3_ICON_BIG_DOT 0x2B24u
#define A3_ICON_CUBE 0x2750u
#define A3_ICON_DASHED_BOX 0x2B1Au

A3_EXTERN_C_END

#endif
