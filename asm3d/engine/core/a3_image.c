/*
 * ASM3D - a3_image.c
 */
#include "a3_image.h"
#include "a3_string.h"
#include "a3_format.h"
#include "a3_log.h"
#include "../platform/a3_platform.h"

/* ======================================================================== */
/* Checksums                                                                */
/* ======================================================================== */

static u32 g_crc_table[256];
static b32 g_crc_ready;

u32 a3_crc32(u32 crc, const u8 *data, usize len) {
    if (!g_crc_ready) {
        for (u32 n = 0; n < 256; ++n) {
            u32 c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            g_crc_table[n] = c;
        }
        g_crc_ready = 1;
    }
    crc = ~crc;
    for (usize i = 0; i < len; ++i) crc = g_crc_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

u32 a3_adler32(u32 adler, const u8 *data, usize len) {
    u32 a = adler & 0xFFFF, b = adler >> 16;
    while (len) {
        usize n = len < 5552 ? len : 5552;
        len -= n;
        while (n--) { a += *data++; b += a; }
        a %= 65521; b %= 65521;
    }
    return (b << 16) | a;
}

/* ======================================================================== */
/* DEFLATE decoder                                                          */
/* ======================================================================== */

typedef struct Huffman {
    u16 counts[16];
    u16 symbols[288];
} Huffman;

typedef struct Inflate {
    const u8 *in;
    usize in_size, in_pos;
    u32 bitbuf;
    u32 bitcnt;
    u8 *out;
    usize out_size, out_cap, max_out;
    b32 error;
} Inflate;

static u32 inf_bits(Inflate *s, u32 n) {
    while (s->bitcnt < n) {
        if (s->in_pos >= s->in_size) { s->error = 1; return 0; }
        s->bitbuf |= (u32)s->in[s->in_pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    u32 v = s->bitbuf & ((1u << n) - 1u);
    s->bitbuf >>= n;
    s->bitcnt -= n;
    return v;
}

static b32 inf_out(Inflate *s, u8 b) {
    if (s->out_size >= s->out_cap) {
        usize nc = s->out_cap ? s->out_cap * 2 : 65536;
        if (nc > s->max_out) nc = s->max_out;
        if (nc <= s->out_size) { s->error = 1; return 0; }
        u8 *n = (u8 *)a3_realloc(s->out, nc, A3_MEM_TEMP);
        if (!n) { s->error = 1; return 0; }
        s->out = n;
        s->out_cap = nc;
    }
    s->out[s->out_size++] = b;
    return 1;
}

static b32 huff_build(Huffman *h, const u8 *lengths, u32 n) {
    u16 offs[16];
    a3_zero(h->counts, sizeof(h->counts));
    for (u32 i = 0; i < n; ++i) h->counts[lengths[i]]++;
    h->counts[0] = 0;
    i32 left = 1;
    for (int len = 1; len < 16; ++len) { left <<= 1; left -= h->counts[len]; if (left < 0) return 0; }
    offs[1] = 0;
    for (int len = 1; len < 15; ++len) offs[len + 1] = (u16)(offs[len] + h->counts[len]);
    for (u32 i = 0; i < n; ++i) if (lengths[i]) h->symbols[offs[lengths[i]]++] = (u16)i;
    return 1;
}

static i32 huff_decode(Inflate *s, const Huffman *h) {
    i32 code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
        code |= (i32)inf_bits(s, 1);
        if (s->error) return -1;
        i32 count = h->counts[len];
        if (code - count < first) return h->symbols[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->error = 1;
    return -1;
}

static const u16 LEN_BASE[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const u16 LEN_EXTRA[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const u16 DIST_BASE[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const u16 DIST_EXTRA[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static b32 inf_codes(Inflate *s, const Huffman *lit, const Huffman *dist) {
    for (;;) {
        i32 sym = huff_decode(s, lit);
        if (sym < 0) return 0;
        if (sym < 256) { if (!inf_out(s, (u8)sym)) return 0; continue; }
        if (sym == 256) return 1;
        sym -= 257;
        if (sym >= 29) { s->error = 1; return 0; }
        u32 len = LEN_BASE[sym] + inf_bits(s, LEN_EXTRA[sym]);
        i32 ds = huff_decode(s, dist);
        if (ds < 0 || ds >= 30) { s->error = 1; return 0; }
        usize d = DIST_BASE[ds] + inf_bits(s, DIST_EXTRA[ds]);
        if (s->error || d > s->out_size) { s->error = 1; return 0; }
        for (u32 i = 0; i < len; ++i) if (!inf_out(s, s->out[s->out_size - d])) return 0;
    }
}

static b32 inf_fixed(Inflate *s) {
    static Huffman lit, dist;
    static b32 built;
    if (!built) {
        u8 l[288];
        for (int i = 0; i < 144; ++i) l[i] = 8;
        for (int i = 144; i < 256; ++i) l[i] = 9;
        for (int i = 256; i < 280; ++i) l[i] = 7;
        for (int i = 280; i < 288; ++i) l[i] = 8;
        huff_build(&lit, l, 288);
        for (int i = 0; i < 30; ++i) l[i] = 5;
        huff_build(&dist, l, 30);
        built = 1;
    }
    return inf_codes(s, &lit, &dist);
}

static b32 inf_dynamic(Inflate *s) {
    static const u8 ORDER[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
    u32 nlen = inf_bits(s, 5) + 257, ndist = inf_bits(s, 5) + 1, ncode = inf_bits(s, 4) + 4;
    if (s->error || nlen > 286 || ndist > 30) return 0;
    u8 lengths[320];
    a3_zero(lengths, sizeof(lengths));
    for (u32 i = 0; i < ncode; ++i) lengths[ORDER[i]] = (u8)inf_bits(s, 3);
    Huffman lencode, lit, dist;
    if (!huff_build(&lencode, lengths, 19)) return 0;
    u32 index = 0;
    while (index < nlen + ndist) {
        i32 sym = huff_decode(s, &lencode);
        if (sym < 0) return 0;
        if (sym < 16) { lengths[index++] = (u8)sym; continue; }
        u8 len = 0;
        u32 rep;
        if (sym == 16) { if (index == 0) return 0; len = lengths[index - 1]; rep = 3 + inf_bits(s, 2); }
        else if (sym == 17) rep = 3 + inf_bits(s, 3);
        else rep = 11 + inf_bits(s, 7);
        if (index + rep > nlen + ndist) return 0;
        while (rep--) lengths[index++] = len;
    }
    if (!huff_build(&lit, lengths, nlen)) return 0;
    if (!huff_build(&dist, lengths + nlen, ndist)) return 0;
    return inf_codes(s, &lit, &dist);
}

A3Result a3_zlib_decompress(const u8 *in, usize in_size, b32 zlib_header, usize max_out, u8 **out, usize *out_size) {
    *out = 0;
    *out_size = 0;
    Inflate s;
    a3_zero_struct(&s);
    s.in = in;
    s.in_size = in_size;
    s.max_out = max_out ? max_out : A3_MB(512);
    if (zlib_header) {
        if (in_size < 2 || (in[0] & 0x0F) != 8 || ((in[0] << 8) | in[1]) % 31 != 0) return A3_ERR_PARSE;
        s.in_pos = 2;
    }
    u32 last;
    do {
        last = inf_bits(&s, 1);
        u32 type = inf_bits(&s, 2);
        if (s.error) break;
        if (type == 0) {
            s.bitbuf = 0; s.bitcnt = 0;
            if (s.in_pos + 4 > s.in_size) { s.error = 1; break; }
            u32 len = s.in[s.in_pos] | (s.in[s.in_pos + 1] << 8);
            u32 nlen = s.in[s.in_pos + 2] | (s.in[s.in_pos + 3] << 8);
            s.in_pos += 4;
            if ((len ^ 0xFFFF) != nlen || s.in_pos + len > s.in_size) { s.error = 1; break; }
            for (u32 i = 0; i < len; ++i) if (!inf_out(&s, s.in[s.in_pos + i])) break;
            s.in_pos += len;
        } else if (type == 1) {
            if (!inf_fixed(&s)) s.error = 1;
        } else if (type == 2) {
            if (!inf_dynamic(&s)) s.error = 1;
        } else {
            s.error = 1;
        }
    } while (!last && !s.error);
    if (s.error) { a3_free(s.out); return A3_ERR_PARSE; }
    *out = s.out;
    *out_size = s.out_size;
    return A3_OK;
}

/* ======================================================================== */
/* PNG                                                                      */
/* ======================================================================== */

static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

static void seterr(char *err, usize cap, const char *msg) { if (err && cap) a3_strcpy(err, cap, msg); }

static u8 paeth(i32 a, i32 b, i32 c) {
    i32 p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return (u8)a;
    return pb <= pc ? (u8)b : (u8)c;
}

/* Unfilters one (sub)image in place. Returns false on bad filter type. */
static b32 png_unfilter(u8 *data, u32 w, u32 h, u32 bpp_bytes, u32 stride, u8 *out) {
    u8 *prev = 0;
    for (u32 y = 0; y < h; ++y) {
        u8 f = data[y * (stride + 1)];
        const u8 *src = data + y * (stride + 1) + 1;
        u8 *dst = out + y * stride;
        for (u32 x = 0; x < stride; ++x) {
            i32 a = x >= bpp_bytes ? dst[x - bpp_bytes] : 0;
            i32 b = prev ? prev[x] : 0;
            i32 c = (prev && x >= bpp_bytes) ? prev[x - bpp_bytes] : 0;
            u8 v = src[x];
            switch (f) {
            case 0: break;
            case 1: v = (u8)(v + a); break;
            case 2: v = (u8)(v + b); break;
            case 3: v = (u8)(v + ((a + b) >> 1)); break;
            case 4: v = (u8)(v + paeth(a, b, c)); break;
            default: return 0;
            }
            dst[x] = v;
        }
        prev = dst;
    }
    A3_UNUSED(w);
    return 1;
}

static void png_expand(const u8 *row_data, u32 w, u32 color, u32 depth, const u8 *pal, u32 pal_n, const u8 *trns, u32 trns_n,
                       u8 *rgba_row, u32 x0, u32 dx) {
    for (u32 i = 0; i < w; ++i) {
        u8 *o = rgba_row + (x0 + i * dx) * 4;
        u32 v;
        if (depth < 8) {
            u32 per = 8 / depth, shift = (per - 1 - (i % per)) * depth;
            v = (row_data[i / per] >> shift) & ((1u << depth) - 1);
        } else {
            v = 0;
        }
        switch (color) {
        case 0: { /* gray */
            u8 g = depth < 8 ? (u8)(v * 255 / ((1u << depth) - 1)) : row_data[i * (depth / 8)];
            o[0] = o[1] = o[2] = g; o[3] = 255;
            if (trns_n >= 2 && depth <= 8 && (u32)((trns[0] << 8) | trns[1]) == (depth < 8 ? v : g)) o[3] = 0;
        } break;
        case 2: { u32 s = depth / 8; o[0] = row_data[i * 3 * s]; o[1] = row_data[i * 3 * s + s]; o[2] = row_data[i * 3 * s + 2 * s]; o[3] = 255;
            if (trns_n >= 6 && depth == 8 && o[0] == trns[1] && o[1] == trns[3] && o[2] == trns[5]) o[3] = 0; } break;
        case 3: {
            u32 idx = depth < 8 ? v : row_data[i];
            if (idx < pal_n) { o[0] = pal[idx * 3]; o[1] = pal[idx * 3 + 1]; o[2] = pal[idx * 3 + 2]; }
            else { o[0] = o[1] = o[2] = 0; }
            o[3] = idx < trns_n ? trns[idx] : 255;
        } break;
        case 4: { u32 s = depth / 8; o[0] = o[1] = o[2] = row_data[i * 2 * s]; o[3] = row_data[i * 2 * s + s]; } break;
        case 6: { u32 s = depth / 8; for (int k = 0; k < 4; ++k) o[k] = row_data[i * 4 * s + (u32)k * s]; } break;
        }
    }
}

static A3Result png_decode(const u8 *d, usize n, A3Image *img, char *err, usize ecap) {
    usize pos = 8;
    u32 w = 0, h = 0, depth = 0, color = 0, interlace = 0;
    u8 pal[768]; u32 pal_n = 0;
    u8 trns[256]; u32 trns_n = 0;
    u8 *idat = 0;
    usize idat_size = 0, idat_cap = 0;
    b32 seen_ihdr = 0;
    while (pos + 12 <= n) {
        u32 len = be32(d + pos);
        const u8 *type = d + pos + 4;
        const u8 *body = d + pos + 8;
        if (pos + 12 + (usize)len > n) { seterr(err, ecap, "PNG chunk runs past end of file (truncated file?)"); a3_free(idat); return A3_ERR_PARSE; }
        if (a3_memcmp(type, "IHDR", 4) == 0 && len >= 13) {
            w = be32(body); h = be32(body + 4); depth = body[8]; color = body[9]; interlace = body[12];
            seen_ihdr = 1;
        } else if (a3_memcmp(type, "PLTE", 4) == 0) {
            pal_n = len / 3 > 256 ? 256 : len / 3;
            a3_memcpy(pal, body, pal_n * 3);
        } else if (a3_memcmp(type, "tRNS", 4) == 0) {
            trns_n = len > 256 ? 256 : len;
            a3_memcpy(trns, body, trns_n);
        } else if (a3_memcmp(type, "IDAT", 4) == 0) {
            if (idat_size + len > idat_cap) {
                usize nc = idat_cap ? idat_cap * 2 : 65536;
                while (nc < idat_size + len) nc *= 2;
                u8 *ni = (u8 *)a3_realloc(idat, nc, A3_MEM_TEMP);
                if (!ni) { a3_free(idat); return A3_ERR_OUT_OF_MEMORY; }
                idat = ni; idat_cap = nc;
            }
            a3_memcpy(idat + idat_size, body, len);
            idat_size += len;
        } else if (a3_memcmp(type, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + (usize)len;
    }
    if (!seen_ihdr || !w || !h || w > 16384 || h > 16384) { seterr(err, ecap, "PNG header missing or image too large (max 16384x16384)"); a3_free(idat); return A3_ERR_PARSE; }
    if (depth == 16 && color != 3) {
        /* 16-bit: handled by taking the high byte (below via depth/8 stride) */
    } else if (!(depth == 1 || depth == 2 || depth == 4 || depth == 8)) {
        seterr(err, ecap, "unsupported PNG bit depth"); a3_free(idat); return A3_ERR_UNSUPPORTED;
    }
    u32 channels = color == 0 ? 1 : color == 2 ? 3 : color == 3 ? 1 : color == 4 ? 2 : color == 6 ? 4 : 0;
    if (!channels) { seterr(err, ecap, "unsupported PNG color type"); a3_free(idat); return A3_ERR_UNSUPPORTED; }
    u32 bits_pp = channels * depth;
    u32 bpp_bytes = bits_pp >= 8 ? bits_pp / 8 : 1;
    u8 *raw = 0;
    usize raw_size = 0;
    if (a3_zlib_decompress(idat, idat_size, 1, (usize)h * ((w * bits_pp + 7) / 8 + 1) * 2 + 1024, &raw, &raw_size) != A3_OK) {
        seterr(err, ecap, "PNG image data is corrupt (deflate error)");
        a3_free(idat);
        return A3_ERR_PARSE;
    }
    a3_free(idat);
    img->width = (i32)w;
    img->height = (i32)h;
    img->pixels = (u8 *)a3_malloc((usize)w * h * 4, A3_MEM_RESOURCE);
    if (!img->pixels) { a3_free(raw); return A3_ERR_OUT_OF_MEMORY; }
    /* 16-bit samples: reduce to 8-bit by treating each sample as 2 bytes with stride */
    u32 eff_depth = depth == 16 ? 16 : depth;
    static const u32 AX0[7] = { 0, 4, 0, 2, 0, 1, 0 }, AY0[7] = { 0, 0, 4, 0, 2, 0, 1 };
    static const u32 ADX[7] = { 8, 8, 4, 4, 2, 2, 1 }, ADY[7] = { 8, 8, 8, 4, 4, 2, 2 };
    u32 passes = interlace ? 7 : 1;
    usize off = 0;
    for (u32 p = 0; p < passes; ++p) {
        u32 x0 = interlace ? AX0[p] : 0, y0 = interlace ? AY0[p] : 0;
        u32 dx = interlace ? ADX[p] : 1, dy = interlace ? ADY[p] : 1;
        if (x0 >= w || y0 >= h) continue;
        u32 pw = (w - x0 + dx - 1) / dx, ph = (h - y0 + dy - 1) / dy;
        u32 stride = (pw * bits_pp + 7) / 8;
        usize need = (usize)ph * (stride + 1);
        if (off + need > raw_size) { seterr(err, ecap, "PNG image data is too short"); a3_free(raw); a3_image_free(img); return A3_ERR_PARSE; }
        u8 *rows = (u8 *)a3_malloc((usize)ph * stride + 1, A3_MEM_TEMP);
        if (!rows) { a3_free(raw); a3_image_free(img); return A3_ERR_OUT_OF_MEMORY; }
        if (!png_unfilter(raw + off, pw, ph, bpp_bytes, stride, rows)) {
            seterr(err, ecap, "PNG has an invalid row filter");
            a3_free(rows); a3_free(raw); a3_image_free(img);
            return A3_ERR_PARSE;
        }
        for (u32 y = 0; y < ph; ++y) {
            u8 *dst_row = img->pixels + (usize)(y0 + y * dy) * w * 4;
            png_expand(rows + (usize)y * stride, pw, color, eff_depth, pal, pal_n, trns, trns_n, dst_row, x0, dx);
        }
        a3_free(rows);
        off += need;
    }
    a3_free(raw);
    return A3_OK;
}

/* ---- PNG encode (stored deflate) ---- */
static void put_be32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }

A3Result a3_png_encode(const u8 *rgba, i32 w, i32 h, b32 flip_y, u8 **out, usize *out_size) {
    if (!rgba || w <= 0 || h <= 0) return A3_ERR_INVALID_ARG;
    usize row = (usize)w * 4 + 1;
    usize raw_size = row * (usize)h;
    usize blocks = (raw_size + 65534) / 65535;
    usize zsize = 2 + raw_size + blocks * 5 + 4;
    usize total = 8 + 25 + 12 + zsize + 12;
    u8 *buf = (u8 *)a3_malloc(total, A3_MEM_TEMP);
    if (!buf) return A3_ERR_OUT_OF_MEMORY;
    u8 *p = buf;
    static const u8 sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    a3_memcpy(p, sig, 8); p += 8;
    put_be32(p, 13); a3_memcpy(p + 4, "IHDR", 4);
    put_be32(p + 8, (u32)w); put_be32(p + 12, (u32)h);
    p[16] = 8; p[17] = 6; p[18] = 0; p[19] = 0; p[20] = 0;
    put_be32(p + 21, a3_crc32(0, p + 4, 17));
    p += 25;
    put_be32(p, (u32)zsize); a3_memcpy(p + 4, "IDAT", 4);
    u8 *z = p + 8;
    z[0] = 0x78; z[1] = 0x01;
    u8 *zp = z + 2;
    u32 adler = 1;
    usize remaining = raw_size, emitted = 0;
    while (remaining) {
        usize n = remaining > 65535 ? 65535 : remaining;
        zp[0] = remaining == n ? 1 : 0;
        zp[1] = (u8)(n & 0xFF); zp[2] = (u8)(n >> 8);
        zp[3] = (u8)(~n & 0xFF); zp[4] = (u8)((~n >> 8) & 0xFF);
        zp += 5;
        for (usize i = 0; i < n; ++i) {
            usize idx = emitted + i;
            usize y = idx / row, x = idx % row;
            usize sy = flip_y ? (usize)h - 1 - y : y;
            u8 v = x == 0 ? 0 : rgba[sy * (usize)w * 4 + (x - 1)];
            zp[i] = v;
        }
        adler = a3_adler32(adler, zp, n);
        zp += n;
        emitted += n;
        remaining -= n;
    }
    put_be32(zp, adler); zp += 4;
    put_be32(zp, a3_crc32(0, p + 4, 4 + zsize));
    p = zp + 4;
    put_be32(p, 0); a3_memcpy(p + 4, "IEND", 4); put_be32(p + 8, a3_crc32(0, p + 4, 4));
    p += 12;
    *out = buf;
    *out_size = (usize)(p - buf);
    return A3_OK;
}

A3Result a3_png_write_file(const char *path, const u8 *rgba, i32 w, i32 h, b32 flip_y) {
    u8 *png = 0;
    usize size = 0;
    A3Result r = a3_png_encode(rgba, w, h, flip_y, &png, &size);
    if (r == A3_OK) r = a3_file_write_all(path, png, size);
    a3_free(png);
    return r;
}

/* ======================================================================== */
/* TGA / BMP / PPM                                                          */
/* ======================================================================== */

static A3Result tga_decode(const u8 *d, usize n, A3Image *img, char *err, usize ecap) {
    if (n < 18) { seterr(err, ecap, "TGA file too small"); return A3_ERR_PARSE; }
    u32 idlen = d[0], cmap = d[1], type = d[2];
    u32 w = d[12] | (d[13] << 8), h = d[14] | (d[15] << 8), bpp = d[16], desc = d[17];
    if (cmap || !(type == 2 || type == 3 || type == 10 || type == 11) || !(bpp == 8 || bpp == 24 || bpp == 32) || !w || !h) {
        seterr(err, ecap, "unsupported TGA variant (use 24/32-bit truecolor or 8-bit gray)");
        return A3_ERR_UNSUPPORTED;
    }
    u32 bytes = bpp / 8;
    img->width = (i32)w; img->height = (i32)h;
    img->pixels = (u8 *)a3_malloc((usize)w * h * 4, A3_MEM_RESOURCE);
    if (!img->pixels) return A3_ERR_OUT_OF_MEMORY;
    usize pos = 18 + idlen;
    b32 rle = type >= 9;
    b32 top_down = (desc & 0x20) != 0;
    usize total = (usize)w * h, i = 0;
    while (i < total) {
        u32 count = 1;
        b32 packet_rle = 0;
        if (rle) {
            if (pos >= n) break;
            u8 hdr = d[pos++];
            count = (hdr & 0x7F) + 1u;
            packet_rle = (hdr & 0x80) != 0;
        }
        u8 px[4] = { 0, 0, 0, 255 };
        for (u32 k = 0; k < count && i < total; ++k, ++i) {
            if (!packet_rle || k == 0) {
                if (pos + bytes > n) { seterr(err, ecap, "TGA data truncated"); a3_image_free(img); return A3_ERR_PARSE; }
                if (bytes == 1) { px[0] = px[1] = px[2] = d[pos]; }
                else { px[2] = d[pos]; px[1] = d[pos + 1]; px[0] = d[pos + 2]; px[3] = bytes == 4 ? d[pos + 3] : 255; }
                pos += bytes;
            }
            usize x = i % w, y = i / w;
            usize dy = top_down ? y : (h - 1 - y);
            a3_memcpy(img->pixels + (dy * w + x) * 4, px, 4);
        }
    }
    return A3_OK;
}

static u32 le32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }

static A3Result bmp_decode(const u8 *d, usize n, A3Image *img, char *err, usize ecap) {
    if (n < 54) { seterr(err, ecap, "BMP file too small"); return A3_ERR_PARSE; }
    u32 off = le32(d + 10);
    i32 w = (i32)le32(d + 18), h = (i32)le32(d + 22);
    u32 bpp = d[28] | (d[29] << 8), comp = le32(d + 30);
    if ((bpp != 24 && bpp != 32) || (comp != 0 && comp != 3) || w <= 0 || h == 0) {
        seterr(err, ecap, "unsupported BMP variant (use 24 or 32-bit uncompressed)");
        return A3_ERR_UNSUPPORTED;
    }
    b32 top_down = h < 0;
    if (h < 0) h = -h;
    u32 bytes = bpp / 8;
    usize stride = ((usize)w * bytes + 3) & ~(usize)3;
    if (off + stride * (usize)h > n) { seterr(err, ecap, "BMP data truncated"); return A3_ERR_PARSE; }
    img->width = w; img->height = h;
    img->pixels = (u8 *)a3_malloc((usize)w * h * 4, A3_MEM_RESOURCE);
    if (!img->pixels) return A3_ERR_OUT_OF_MEMORY;
    for (i32 y = 0; y < h; ++y) {
        const u8 *src = d + off + (usize)(top_down ? y : h - 1 - y) * stride;
        u8 *dst = img->pixels + (usize)y * w * 4;
        for (i32 x = 0; x < w; ++x) {
            dst[x * 4 + 0] = src[x * bytes + 2];
            dst[x * 4 + 1] = src[x * bytes + 1];
            dst[x * 4 + 2] = src[x * bytes + 0];
            dst[x * 4 + 3] = bytes == 4 ? src[x * bytes + 3] : 255;
        }
    }
    return A3_OK;
}

static A3Result ppm_decode(const u8 *d, usize n, A3Image *img, char *err, usize ecap) {
    b32 gray = d[1] == '5';
    usize pos = 2;
    u32 vals[3] = { 0, 0, 0 };
    for (int k = 0; k < 3; ++k) {
        while (pos < n && (a3_is_space(d[pos]) || d[pos] == '#')) {
            if (d[pos] == '#') while (pos < n && d[pos] != '\n') ++pos;
            else ++pos;
        }
        while (pos < n && a3_is_digit(d[pos])) vals[k] = vals[k] * 10 + (d[pos++] - '0');
    }
    ++pos;
    u32 w = vals[0], h = vals[1];
    if (!w || !h || vals[2] != 255) { seterr(err, ecap, "unsupported PPM/PGM (8-bit binary only)"); return A3_ERR_UNSUPPORTED; }
    usize need = (usize)w * h * (gray ? 1 : 3);
    if (pos + need > n) { seterr(err, ecap, "PPM data truncated"); return A3_ERR_PARSE; }
    img->width = (i32)w; img->height = (i32)h;
    img->pixels = (u8 *)a3_malloc((usize)w * h * 4, A3_MEM_RESOURCE);
    if (!img->pixels) return A3_ERR_OUT_OF_MEMORY;
    for (usize i = 0; i < (usize)w * h; ++i) {
        u8 *o = img->pixels + i * 4;
        if (gray) o[0] = o[1] = o[2] = d[pos + i];
        else { o[0] = d[pos + i * 3]; o[1] = d[pos + i * 3 + 1]; o[2] = d[pos + i * 3 + 2]; }
        o[3] = 255;
    }
    return A3_OK;
}

A3Result a3_image_decode(const u8 *d, usize n, A3Image *img, char *err, usize ecap) {
    a3_zero_struct(img);
    if (!d || n < 4) { seterr(err, ecap, "empty image data"); return A3_ERR_INVALID_ARG; }
    static const u8 PNG_SIG[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (n >= 8 && a3_memcmp(d, PNG_SIG, 8) == 0) return png_decode(d, n, img, err, ecap);
    if (d[0] == 'B' && d[1] == 'M') return bmp_decode(d, n, img, err, ecap);
    if (d[0] == 'P' && (d[1] == '6' || d[1] == '5')) return ppm_decode(d, n, img, err, ecap);
    if (n >= 3 && (d[0] == 0xFF && d[1] == 0xD8)) { seterr(err, ecap, "JPEG is not supported yet; convert the image to PNG"); return A3_ERR_UNSUPPORTED; }
    if (n >= 18 && (d[2] == 2 || d[2] == 3 || d[2] == 10 || d[2] == 11)) return tga_decode(d, n, img, err, ecap);
    seterr(err, ecap, "unrecognized image format (supported: PNG, TGA, BMP, PPM)");
    return A3_ERR_UNSUPPORTED;
}

A3Result a3_image_load(const char *path, A3Image *out, char *err, usize ecap) {
    A3FileData fd;
    A3Result r = a3_file_read_all(path, A3_MEM_TEMP, &fd);
    if (r != A3_OK) { a3_zero_struct(out); seterr(err, ecap, "file not found or unreadable"); return r; }
    r = a3_image_decode(fd.data, fd.size, out, err, ecap);
    a3_free(fd.data);
    return r;
}

void a3_image_free(A3Image *img) {
    if (!img) return;
    a3_free(img->pixels);
    img->pixels = 0;
    img->width = img->height = 0;
}

A3Result a3_image_resize(const A3Image *src, i32 w, i32 h, A3Image *out) {
    a3_zero_struct(out);
    if (!src || !src->pixels || w <= 0 || h <= 0) return A3_ERR_INVALID_ARG;
    out->pixels = (u8 *)a3_malloc((usize)w * h * 4, A3_MEM_RESOURCE);
    if (!out->pixels) return A3_ERR_OUT_OF_MEMORY;
    out->width = w; out->height = h;
    for (i32 y = 0; y < h; ++y) {
        i32 sy0 = y * src->height / h, sy1 = (y + 1) * src->height / h;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (i32 x = 0; x < w; ++x) {
            i32 sx0 = x * src->width / w, sx1 = (x + 1) * src->width / w;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            u32 acc[4] = { 0, 0, 0, 0 }, cnt = 0;
            for (i32 sy = sy0; sy < sy1; ++sy)
                for (i32 sx = sx0; sx < sx1; ++sx) {
                    const u8 *p = src->pixels + ((usize)sy * src->width + sx) * 4;
                    for (int k = 0; k < 4; ++k) acc[k] += p[k];
                    cnt++;
                }
            u8 *o = out->pixels + ((usize)y * w + x) * 4;
            for (int k = 0; k < 4; ++k) o[k] = (u8)(acc[k] / cnt);
        }
    }
    return A3_OK;
}
