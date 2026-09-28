/*
 * ASM3D - a3_test.h
 * Minimal test framework that runs identically on native and WebAssembly.
 * Tests are listed in tests/test_list.h (explicit registration keeps the
 * freestanding wasm build free of static constructors).
 */
#ifndef A3_TEST_H
#define A3_TEST_H

#include "../engine/core/a3_base.h"
#include "../engine/core/a3_log.h"
#include "../engine/core/a3_math.h"

typedef struct A3TestContext {
    const char *current;
    u32 checks;
    u32 failures;
    u32 test_failures;
} A3TestContext;

extern A3TestContext g_a3_test;

void a3_test_fail(const char *file, int line, const char *fmt, ...) A3_PRINTF_LIKE(3, 4);

#define A3_TEST(name) void a3_test_##name(void)

#define A3_CHECK(cond) do { g_a3_test.checks++; if (!(cond)) a3_test_fail(__FILE__, __LINE__, "CHECK(%s)", #cond); } while (0)
#define A3_CHECK_MSG(cond, ...) do { g_a3_test.checks++; if (!(cond)) a3_test_fail(__FILE__, __LINE__, __VA_ARGS__); } while (0)
#define A3_CHECK_EQ_INT(a, b) do { long long _a = (long long)(a), _b = (long long)(b); g_a3_test.checks++; \
    if (_a != _b) a3_test_fail(__FILE__, __LINE__, "%s == %s (%lld vs %lld)", #a, #b, _a, _b); } while (0)
#define A3_CHECK_NEAR(a, b, eps) do { double _a = (double)(a), _b = (double)(b); g_a3_test.checks++; \
    if (!(_a - _b <= (eps) && _b - _a <= (eps))) a3_test_fail(__FILE__, __LINE__, "%s ~= %s (%.9g vs %.9g)", #a, #b, _a, _b); } while (0)
#define A3_CHECK_STR(a, b) do { const char *_a = (a), *_b = (b); g_a3_test.checks++; \
    if (a3_strcmp(_a, _b) != 0) a3_test_fail(__FILE__, __LINE__, "%s == %s (\"%s\" vs \"%s\")", #a, #b, _a ? _a : "(null)", _b ? _b : "(null)"); } while (0)

#endif
