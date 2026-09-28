/*
 * ASM3D - a3_hash.c
 */
#include "a3_hash.h"
#include "a3_math.h"
#include "a3_string.h"
#include "a3_log.h"

u64 a3_fnv1a64(const void *data, usize len) {
    const u8 *p = (const u8 *)data;
    u64 h = 0xcbf29ce484222325ull;
    for (usize i = 0; i < len; ++i) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

u64 a3_hash_str(const char *s) { return a3_fnv1a64(s, a3_strlen(s)); }
u32 a3_hash_str32(const char *s) { u64 h = a3_hash_str(s); return (u32)(h ^ (h >> 32)); }

static u64 read64(const u8 *p) { u64 v; a3_memcpy(&v, p, 8); return v; }

u64 a3_hash64(const void *data, usize len, u64 seed) {
    /* Four independent lanes over 32-byte stripes, then mixed. Endianness:
     * all supported targets are little-endian, so results are stable. */
    const u8 *p = (const u8 *)data;
    const u64 K1 = 0x9e3779b97f4a7c15ull, K2 = 0xc2b2ae3d27d4eb4full, K3 = 0x165667b19e3779f9ull;
    u64 a = seed ^ K1, b = seed ^ K2, c = seed ^ K3, d = seed + len;
    usize i = 0;
    for (; i + 32 <= len; i += 32) {
        a = a3_hash_mix64(a ^ read64(p + i)) * K1;
        b = a3_hash_mix64(b ^ read64(p + i + 8)) * K2;
        c = a3_hash_mix64(c ^ read64(p + i + 16)) * K3;
        d = a3_hash_mix64(d ^ read64(p + i + 24)) * K1;
    }
    u64 h = a3_hash_combine(a3_hash_combine(a, b), a3_hash_combine(c, d));
    for (; i + 8 <= len; i += 8) h = a3_hash_mix64(h ^ read64(p + i)) * K2;
    u64 tail = 0;
    for (usize s = 0; i < len; ++i, s += 8) tail |= (u64)p[i] << s;
    return a3_hash_mix64(h ^ tail ^ (u64)len);
}

void a3_hash_to_hex(u64 h, char buf[17]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 15; i >= 0; --i) { buf[i] = hex[h & 15]; h >>= 4; }
    buf[16] = 0;
}

/* ---- PCG32 ---- */
void a3_rng_seed(A3Rng *rng, u64 seed, u64 stream) {
    rng->state = 0;
    rng->inc = (stream << 1u) | 1u;
    a3_rng_u32(rng);
    rng->state += seed;
    a3_rng_u32(rng);
}

u32 a3_rng_u32(A3Rng *rng) {
    u64 old = rng->state;
    rng->state = old * 6364136223846793005ull + rng->inc;
    u32 xorshifted = (u32)(((old >> 18u) ^ old) >> 27u);
    u32 rot = (u32)(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((-(i32)rot) & 31));
}

u32 a3_rng_range_u32(A3Rng *rng, u32 bound) {
    if (bound == 0) return 0;
    u32 threshold = (0u - bound) % bound;
    for (;;) { u32 r = a3_rng_u32(rng); if (r >= threshold) return r % bound; }
}

i32 a3_rng_range_i32(A3Rng *rng, i32 lo, i32 hi) {
    if (hi <= lo) return lo;
    return lo + (i32)a3_rng_range_u32(rng, (u32)(hi - lo) + 1u);
}

f32 a3_rng_f32(A3Rng *rng) { return (f32)(a3_rng_u32(rng) >> 8) * (1.0f / 16777216.0f); }
f32 a3_rng_range_f32(A3Rng *rng, f32 lo, f32 hi) { return lo + (hi - lo) * a3_rng_f32(rng); }
b32 a3_rng_chance(A3Rng *rng, f32 p) { return a3_rng_f32(rng) < p; }

u32 a3_hash_coords2(i32 x, i32 y, u32 seed) {
    u64 h = a3_hash_mix64(((u64)(u32)x << 32) ^ (u64)(u32)y ^ ((u64)seed * 0x9e3779b97f4a7c15ull));
    return (u32)(h ^ (h >> 32));
}

u32 a3_hash_coords3(i32 x, i32 y, i32 z, u32 seed) {
    u64 h = a3_hash_mix64(((u64)(u32)x << 32) ^ (u64)(u32)y ^ ((u64)seed * 0x9e3779b97f4a7c15ull));
    h = a3_hash_mix64(h ^ ((u64)(u32)z * 0xc2b2ae3d27d4eb4full));
    return (u32)(h ^ (h >> 32));
}

static f32 lattice(i32 x, i32 y, u32 seed) {
    return (f32)(a3_hash_coords2(x, y, seed) & 0xFFFFFF) * (2.0f / 16777215.0f) - 1.0f;
}

f32 a3_noise2(f32 x, f32 y, u32 seed) {
    f32 fx = a3_floorf(x), fy = a3_floorf(y);
    i32 ix = (i32)fx, iy = (i32)fy;
    f32 tx = x - fx, ty = y - fy;
    f32 sx = tx * tx * (3.0f - 2.0f * tx), sy = ty * ty * (3.0f - 2.0f * ty);
    f32 a = lattice(ix, iy, seed), b = lattice(ix + 1, iy, seed);
    f32 c = lattice(ix, iy + 1, seed), d = lattice(ix + 1, iy + 1, seed);
    return a3_lerpf(a3_lerpf(a, b, sx), a3_lerpf(c, d, sx), sy);
}

f32 a3_fbm2(f32 x, f32 y, u32 seed, int octaves, f32 lacunarity, f32 gain) {
    f32 sum = 0, amp = 1, norm = 0, freq = 1;
    for (int i = 0; i < octaves; ++i) {
        sum += a3_noise2(x * freq, y * freq, seed + (u32)i * 1013u) * amp;
        norm += amp;
        amp *= gain;
        freq *= lacunarity;
    }
    return norm > 0 ? sum / norm : 0;
}

/* ---- Hash map ---- */
void a3_hashmap_init(A3HashMap *m, u32 cap, A3MemTag tag) {
    a3_zero_struct(m);
    m->tag = tag;
    if (cap) {
        u32 c = 16;
        while (c < cap * 2) c <<= 1;
        m->keys = A3_NEW_ARRAY(u64, c, tag);
        m->values = A3_NEW_ARRAY(u64, c, tag);
        if (m->keys && m->values) m->capacity = c;
    }
}

void a3_hashmap_free(A3HashMap *m) {
    a3_free(m->keys);
    a3_free(m->values);
    m->keys = m->values = 0;
    m->capacity = m->count = m->tombstones = 0;
}

void a3_hashmap_clear(A3HashMap *m) {
    if (m->keys) a3_zero(m->keys, sizeof(u64) * m->capacity);
    m->count = m->tombstones = 0;
}

static b32 hashmap_grow(A3HashMap *m) {
    u32 nc = m->capacity ? m->capacity * 2 : 16;
    if (m->count * 2 < m->capacity) nc = m->capacity; /* only tombstones: rehash in place size */
    u64 *nk = A3_NEW_ARRAY(u64, nc, m->tag);
    u64 *nv = A3_NEW_ARRAY(u64, nc, m->tag);
    if (!nk || !nv) { a3_free(nk); a3_free(nv); A3_ERROR("hashmap", "grow failed"); return 0; }
    for (u32 i = 0; i < m->capacity; ++i) {
        u64 k = m->keys[i];
        if (k == 0 || k == A3_HASHMAP_TOMBSTONE) continue;
        u32 j = (u32)a3_hash_mix64(k) & (nc - 1);
        while (nk[j]) j = (j + 1) & (nc - 1);
        nk[j] = k; nv[j] = m->values[i];
    }
    a3_free(m->keys); a3_free(m->values);
    m->keys = nk; m->values = nv; m->capacity = nc; m->tombstones = 0;
    return 1;
}

b32 a3_hashmap_put(A3HashMap *m, u64 key, u64 value) {
    if (key == 0 || key == A3_HASHMAP_TOMBSTONE) { A3_ERROR("hashmap", "reserved key"); return 0; }
    if ((m->count + m->tombstones + 1) * 4 >= m->capacity * 3) if (!hashmap_grow(m)) return 0;
    u32 mask = m->capacity - 1, j = (u32)a3_hash_mix64(key) & mask;
    i64 first_tomb = -1;
    for (;;) {
        u64 k = m->keys[j];
        if (k == key) { m->values[j] = value; return 1; }
        if (k == 0) break;
        if (k == A3_HASHMAP_TOMBSTONE && first_tomb < 0) first_tomb = j;
        j = (j + 1) & mask;
    }
    if (first_tomb >= 0) { j = (u32)first_tomb; m->tombstones--; }
    m->keys[j] = key; m->values[j] = value; m->count++;
    return 1;
}

b32 a3_hashmap_get(const A3HashMap *m, u64 key, u64 *out) {
    if (!m->capacity || key == 0 || key == A3_HASHMAP_TOMBSTONE) return 0;
    u32 mask = m->capacity - 1, j = (u32)a3_hash_mix64(key) & mask;
    for (u32 probes = 0; probes < m->capacity; ++probes) {
        u64 k = m->keys[j];
        if (k == key) { if (out) *out = m->values[j]; return 1; }
        if (k == 0) return 0;
        j = (j + 1) & mask;
    }
    return 0;
}

b32 a3_hashmap_remove(A3HashMap *m, u64 key) {
    if (!m->capacity || key == 0 || key == A3_HASHMAP_TOMBSTONE) return 0;
    u32 mask = m->capacity - 1, j = (u32)a3_hash_mix64(key) & mask;
    for (u32 probes = 0; probes < m->capacity; ++probes) {
        u64 k = m->keys[j];
        if (k == key) { m->keys[j] = A3_HASHMAP_TOMBSTONE; m->count--; m->tombstones++; return 1; }
        if (k == 0) return 0;
        j = (j + 1) & mask;
    }
    return 0;
}
