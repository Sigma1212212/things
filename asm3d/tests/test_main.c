/*
 * ASM3D - test_main.c
 * Test runner. Native: executable `asm3d_tests [filter]`.
 * Web: exported `a3_run_tests` called by tools/run_wasm_tests.mjs under Node.
 */
#include "a3_test.h"
#include "../engine/core/a3_format.h"
#include "../engine/core/a3_string.h"
#include "../engine/core/a3_memory.h"
#include "../engine/platform/a3_platform.h"

A3TestContext g_a3_test;

#define X(name) void a3_test_##name(void);
#include "test_list.h"
#undef X

typedef struct TestEntry { const char *name; void (*fn)(void); } TestEntry;
static const TestEntry g_tests[] = {
#define X(name) { #name, a3_test_##name },
#include "test_list.h"
#undef X
};

static b32 g_current_failed;

void a3_test_fail(const char *file, int line, const char *fmt, ...) {
    char msg[512];
    va_list args;
    va_start(args, fmt);
    a3_vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    g_a3_test.failures++;
    g_current_failed = 1;
    a3_log(A3_LOG_ERROR, "test", "%s: %s:%d: %s", g_a3_test.current, file, line, msg);
}

static int run_tests(const char *filter) {
    a3_platform_init();
    u32 ran = 0;
    u64 t0 = a3_time_ns();
    for (u32 i = 0; i < A3_ARRAY_COUNT(g_tests); ++i) {
        if (filter && *filter && !a3_strstr(g_tests[i].name, filter)) continue;
        g_a3_test.current = g_tests[i].name;
        g_current_failed = 0;
        u64 s = a3_time_ns();
        g_tests[i].fn();
        f64 ms = (f64)(a3_time_ns() - s) / 1e6;
        if (g_current_failed) g_a3_test.test_failures++;
        a3_log(g_current_failed ? A3_LOG_ERROR : A3_LOG_INFO, "test", "%s %-28s (%.2f ms)", g_current_failed ? "FAIL" : "ok  ", g_tests[i].name, ms);
        ran++;
    }
    f64 total = (f64)(a3_time_ns() - t0) / 1e6;
    a3_log(g_a3_test.test_failures ? A3_LOG_ERROR : A3_LOG_INFO, "test",
           "%s: %u tests, %u checks, %u failed checks, %u failed tests on %s [simd: %s] in %.1f ms",
           g_a3_test.test_failures ? "FAILED" : "PASSED", ran, g_a3_test.checks, g_a3_test.failures,
           g_a3_test.test_failures, a3_platform_name(), a3_simd_backend_name(), total);
    return (int)g_a3_test.test_failures;
}

#if A3_PLATFORM_WEB
A3_WASM_EXPORT("a3_run_tests") int a3_run_tests(void) { return run_tests(0); }
#else
int main(int argc, char **argv) {
    /* asm3d_tests [filter] [-v]   (-v enables debug logging) */
    if (argc > 2 && a3_streq(argv[2], "-v")) a3_log_set_min_level(A3_LOG_DEBUG);
    return run_tests(argc > 1 ? argv[1] : 0) ? 1 : 0;
}
#endif
