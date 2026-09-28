/*
 * ASM3D - test_image.c : DEFLATE + image codecs against Pillow-generated fixtures
 */
#include "a3_test.h"
#include "../engine/core/a3_image.h"
#include "../engine/core/a3_string.h"
#include "../engine/core/a3_format.h"
#include "../engine/core/a3_memory.h"
#include "../engine/platform/a3_platform.h"

#ifndef A3_TEST_DATA_DIR
#define A3_TEST_DATA_DIR "tests/data"
#endif

#if !A3_PLATFORM_WEB
static void check_image(const char *file, const char *raw_file, int tolerance) {
    char path[512], rpath[512], err[128] = "";
    a3_snprintf(path, sizeof(path), "%s/%s", A3_TEST_DATA_DIR, file);
    a3_snprintf(rpath, sizeof(rpath), "%s/%s", A3_TEST_DATA_DIR, raw_file);
    A3Image img;
    A3Result r = a3_image_load(path, &img, err, sizeof(err));
    A3_CHECK_MSG(r == A3_OK, "%s: %s", file, err);
    if (r != A3_OK) return;
    A3_CHECK_EQ_INT(img.width, 37);
    A3_CHECK_EQ_INT(img.height, 23);
    A3FileData raw;
    A3_CHECK(a3_file_read_all(rpath, A3_MEM_TEMP, &raw) == A3_OK);
    A3_CHECK_EQ_INT(raw.size, 37 * 23 * 4);
    int bad = 0;
    for (usize i = 0; i < raw.size && i < (usize)37 * 23 * 4; ++i) {
        int d = (int)img.pixels[i] - (int)raw.data[i];
        if (d < -tolerance || d > tolerance) bad++;
    }
    A3_CHECK_MSG(bad == 0, "%s: %d bytes differ from reference", file, bad);
    a3_free(raw.data);
    a3_image_free(&img);
}
#endif

A3_TEST(image_codecs) {
#if A3_PLATFORM_WEB
    return; /* fixtures are read from disk; covered by the native run */
#else
    check_image("rgba.png", "rgba.raw", 0);
    check_image("rgba_interlaced.png", "rgba.raw", 0);
    check_image("rgb.png", "rgb.raw", 0);
    check_image("pal.png", "pal.raw", 0);
    check_image("gray.png", "gray.raw", 0);
    check_image("rgba.tga", "rgba.raw", 0);
    check_image("rgba_rle.tga", "rgba.raw", 0);
    check_image("rgb.bmp", "rgb.raw", 0);
#endif
}

A3_TEST(image_png_roundtrip) {
    const i32 W = 64, H = 40;
    u8 *px = (u8 *)a3_malloc(W * H * 4, A3_MEM_TEMP);
    for (i32 i = 0; i < W * H * 4; ++i) px[i] = (u8)(i * 31 + (i >> 7));
    u8 *png = 0;
    usize size = 0;
    A3_CHECK(a3_png_encode(px, W, H, 0, &png, &size) == A3_OK);
    A3Image img;
    char err[128] = "";
    A3_CHECK_MSG(a3_image_decode(png, size, &img, err, sizeof(err)) == A3_OK, "%s", err);
    A3_CHECK(img.width == W && img.height == H);
    A3_CHECK(img.pixels && a3_memcmp(img.pixels, px, W * H * 4) == 0);
    /* corrupted data is reported, not crashed on */
    png[size / 2] ^= 0x5A;
    png[size / 2 + 1] ^= 0xA5;
    A3Image bad;
    A3Result r = a3_image_decode(png, size / 2 + 8, &bad, err, sizeof(err));
    A3_CHECK(r != A3_OK && err[0]);
    A3Image small;
    A3_CHECK(a3_image_resize(&img, 16, 10, &small) == A3_OK && small.width == 16);
    a3_image_free(&small);
    a3_image_free(&img);
    a3_free(png);
    a3_free(px);
    A3_CHECK(a3_crc32(0, (const u8 *)"123456789", 9) == 0xCBF43926u);
    A3_CHECK(a3_adler32(1, (const u8 *)"Wikipedia", 9) == 0x11E60398u);
}
