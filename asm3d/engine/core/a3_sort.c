/*
 * ASM3D - a3_sort.c
 */
#include "a3_sort.h"
#include "a3_memory.h"
#include "a3_string.h"

void a3_radix_sort_pairs(A3SortPair *pairs, A3SortPair *tmp, u32 count) {
    if (count < 2) return;
    A3SortPair *src = pairs, *dst = tmp;
    for (u32 pass = 0; pass < 8; ++pass) {
        u32 shift = pass * 8;
        u32 hist[256];
        a3_zero(hist, sizeof(hist));
        for (u32 i = 0; i < count; ++i) hist[(src[i].key >> shift) & 0xFF]++;
        /* skip passes where every key has the same byte */
        if (hist[(src[0].key >> shift) & 0xFF] == count) continue;
        u32 sum = 0;
        for (u32 b = 0; b < 256; ++b) { u32 c = hist[b]; hist[b] = sum; sum += c; }
        for (u32 i = 0; i < count; ++i) dst[hist[(src[i].key >> shift) & 0xFF]++] = src[i];
        A3SortPair *t = src; src = dst; dst = t;
    }
    if (src != pairs) a3_memcpy(pairs, src, sizeof(A3SortPair) * count);
}

static void merge_sort(u8 *a, u8 *tmp, u32 n, usize es, A3CompareFn cmp, void *user) {
    if (n < 24) {
        for (u32 i = 1; i < n; ++i) {
            u32 j = i;
            a3_memcpy(tmp, a + i * es, es);
            while (j > 0 && cmp(a + (j - 1) * es, tmp, user) > 0) { a3_memcpy(a + j * es, a + (j - 1) * es, es); --j; }
            a3_memcpy(a + j * es, tmp, es);
        }
        return;
    }
    u32 mid = n / 2;
    merge_sort(a, tmp, mid, es, cmp, user);
    merge_sort(a + mid * es, tmp, n - mid, es, cmp, user);
    if (cmp(a + (mid - 1) * es, a + mid * es, user) <= 0) return;
    u32 i = 0, j = mid, k = 0;
    while (i < mid && j < n) {
        if (cmp(a + j * es, a + i * es, user) < 0) { a3_memcpy(tmp + k * es, a + j * es, es); ++j; }
        else { a3_memcpy(tmp + k * es, a + i * es, es); ++i; }
        ++k;
    }
    while (i < mid) { a3_memcpy(tmp + k * es, a + i * es, es); ++i; ++k; }
    while (j < n) { a3_memcpy(tmp + k * es, a + j * es, es); ++j; ++k; }
    a3_memcpy(a, tmp, (usize)n * es);
}

void a3_sort(void *base, u32 count, usize es, A3CompareFn cmp, void *user) {
    if (count < 2 || !base || !cmp) return;
    u8 *tmp = (u8 *)a3_malloc((usize)count * es + es, A3_MEM_TEMP);
    if (!tmp) return;
    merge_sort((u8 *)base, tmp, count, es, cmp, user);
    a3_free(tmp);
}
