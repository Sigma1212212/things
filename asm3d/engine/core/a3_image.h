/*
 * ASM3D - a3_image.h
 * Image decoding/encoding with no external libraries:
 *   PNG (8-bit gray/gray+alpha/RGB/RGBA/palette, non-interlaced and Adam7),
 *   TGA (uncompressed + RLE), BMP (24/32-bit), PPM/PGM (binary).
 * Output is always RGBA8. Also includes the DEFLATE decoder (zlib) used by
 * PNG and by compressed build packages.
 */
#ifndef A3_IMAGE_H
#define A3_IMAGE_H

#include "a3_base.h"
#include "a3_memory.h"

A3_EXTERN_C_BEGIN

typedef struct A3Image {
    i32 width, height;
    u8 *pixels;          /* RGBA8, top-down rows, a3_malloc'd */
} A3Image;

/* Decodes from memory; format detected from the header bytes.
 * On failure returns an error code and writes a message to err (optional). */
A3Result a3_image_decode(const u8 *data, usize size, A3Image *out, char *err, usize err_cap);
A3Result a3_image_load(const char *path, A3Image *out, char *err, usize err_cap);
void     a3_image_free(A3Image *img);
/* Encodes RGBA8 as PNG (zlib "stored" + filter 0: fast, lossless, larger files).
 * Result is a3_malloc'd. */
A3Result a3_png_encode(const u8 *rgba, i32 w, i32 h, b32 flip_y, u8 **out, usize *out_size);
A3Result a3_png_write_file(const char *path, const u8 *rgba, i32 w, i32 h, b32 flip_y);
/* Box-filter resize (thumbnails). */
A3Result a3_image_resize(const A3Image *src, i32 w, i32 h, A3Image *out);

/* zlib stream (2-byte header + deflate + adler32) or raw deflate. Output grows
 * as needed up to max_out. Returns A3_OK and a3_malloc'd buffer. */
A3Result a3_zlib_decompress(const u8 *in, usize in_size, b32 has_zlib_header, usize max_out, u8 **out, usize *out_size);
u32 a3_crc32(u32 crc, const u8 *data, usize len);
u32 a3_adler32(u32 adler, const u8 *data, usize len);

A3_EXTERN_C_END

#endif
