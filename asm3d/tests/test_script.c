/*
 * ASM3D - test_script.c : A3Script compiler, VM, core library, errors, host bindings
 */
#include "a3_test.h"
#include "../engine/script/a3_script.h"
#include "../engine/core/a3_string.h"
#include "../engine/core/a3_format.h"

static char g_out[4096];

static void capture_print(A3SInstance *inst, const char *text) {
    (void)inst;
    usize l = a3_strlen(g_out);
    a3_snprintf(g_out + l, sizeof(g_out) - l, "%s\n", text);
}

static A3SHost g_test_host;

static void host_reset(void) {
    a3_zero_struct(&g_test_host);
    g_test_host.print = capture_print;
    a3s_set_host(&g_test_host);
    g_out[0] = 0;
}

/* Compiles + runs top level; returns the instance (NULL on error, err filled). */
static A3SInstance *load(const char *src, A3SError *err) {
    A3SModule *m = a3s_compile("test", src, a3_strlen(src), err);
    if (!m) return 0;
    A3SInstance *inst = a3s_instance_create(m, a3s_nil(), 0, err);
    a3s_module_release(m);
    return inst;
}

/* Runs `main()` of src and returns its printed output (in g_out). */
static b32 run_main(const char *src, A3SError *err) {
    host_reset();
    A3SInstance *inst = load(src, err);
    if (!inst) return 0;
    b32 ok = a3s_call(inst, "main", 0, 0, 0, 0, err);
    a3s_instance_destroy(inst);
    return ok;
}

static f64 eval_num(const char *expr) {
    char src[512];
    a3_snprintf(src, sizeof(src), "fn main() { return %s }", expr);
    host_reset();
    A3SError err;
    A3SInstance *inst = load(src, &err);
    if (!inst) { a3_test_fail(__FILE__, __LINE__, "compile '%s': %s", expr, err.message); return -12345; }
    A3SValue r;
    f64 out = -12345;
    if (a3s_call(inst, "main", 0, 0, &r, 0, &err) && r.type == A3S_NUM) out = r.as.num;
    else a3_test_fail(__FILE__, __LINE__, "eval '%s': %s", expr, err.message);
    a3s_release(&r);
    a3s_instance_destroy(inst);
    return out;
}

A3_TEST(script_expressions) {
    i64 live = a3s_live_objects();
    A3_CHECK(eval_num("1 + 2 * 3") == 7);
    A3_CHECK(eval_num("(1 + 2) * 3") == 9);
    A3_CHECK(eval_num("10 - 4 - 3") == 3);        /* left associative */
    A3_CHECK(eval_num("-7 % 3") == 2);            /* result has the sign of the divisor */
    A3_CHECK(eval_num("min(4, 2, 8) + max([1, 9, 3])") == 11);
    A3_CHECK(eval_num("clamp(15, 0, 10)") == 10);
    A3_CHECK(eval_num("len(\"hello\") + len([1, 2, 3])") == 8);
    A3_CHECK(eval_num("floor(2.7) + ceil(2.1) + round(2.5)") == 8);
    A3_CHECK(eval_num("round(3.14159, 2)") == 3.14);
    A3_CHECK(eval_num("pow(2, 10)") == 1024);
    A3_CHECK(eval_num("sqrt(81)") == 9);
    A3_CHECK_NEAR(eval_num("sin(pi / 2)"), 1, 1e-6);
    A3_CHECK_NEAR(eval_num("degrees(atan2(1, 1))"), 45, 1e-4);
    A3_CHECK(eval_num("length(vec3(3, 4, 0))") == 5);
    A3_CHECK(eval_num("dot(vec3(1, 2, 3), vec3(4, 5, 6))") == 32);
    A3_CHECK(eval_num("(vec3(1, 2, 3) * 2).z") == 6);
    A3_CHECK(eval_num("cross(vec3(1, 0, 0), vec3(0, 1, 0)).z") == 1);
    A3_CHECK(eval_num("num(\" 42 \") + 1") == 43);
    A3_CHECK(eval_num("index_of(\"hello world\", \"world\")") == 6);
    A3_CHECK(eval_num("[10, 20, 30][-1]") == 30);
    A3_CHECK(eval_num("move_toward(0, 10, 3)") == 3);
    A3_CHECK(eval_num("lerp(vec3(0), vec3(10), 0.5).y") == 5);
    A3_CHECK(eval_num("1_000_000 / 1e3") == 1000);
    A3_CHECK_EQ_INT(a3s_live_objects(), live);
}

A3_TEST(script_statements) {
    i64 live = a3s_live_objects();
    A3SError err;
    const char *src =
        "let counter = 0\n"
        "fn fib(n) { if n < 2 { return n } return fib(n - 1) + fib(n - 2) }\n"
        "fn main() {\n"
        "    print(\"fib\", fib(15))\n"
        "    let total = 0\n"
        "    for i in 0..10 {\n"
        "        if i == 3 { continue }\n"
        "        if i == 8 { break }\n"
        "        total += i\n"
        "    }\n"
        "    print(\"total\", total)\n"
        "    let names = [\"ada\", \"bob\"]\n"
        "    push(names, \"cy\")\n"
        "    let s = \"\"\n"
        "    for n in names { s = s + upper(n) + \";\" }\n"
        "    print(s, len(names))\n"
        "    let v = vec3(1, 2, 3)\n"
        "    v.y = 10\n"
        "    v.x += 5\n"
        "    print(v)\n"
        "    let grid = [[1, 2], [3, 4]]\n"
        "    grid[1][0] = 99\n"
        "    print(grid)\n"
        "    let w = 0\n"
        "    while w < 5 { w += 2 }\n"
        "    print(w, 7 / 2, true and nil, false or \"x\", not 0)\n"
        "    print(join(split(\"a,b,c\", \",\"), \"-\"), substr(\"engine\", 1, 3), \"n=\" + 3)\n"
        "    counter += 1\n"
        "}\n";
    A3_CHECK_MSG(run_main(src, &err), "%s (line %d)", err.message, err.line);
    A3_CHECK_STR(g_out,
        "fib 610\n"
        "total 25\n"
        "ADA;BOB;CY; 3\n"
        "(6, 10, 3)\n"
        "[[1, 2], [99, 4]]\n"
        "6 3.5 nil x true\n"
        "a-b-c ngi n=3\n");
    /* globals live per instance and persist between calls */
    host_reset();
    A3SInstance *inst = load(src, &err);
    A3_CHECK(inst != 0);
    for (int i = 0; i < 3; ++i) a3s_call(inst, "main", 0, 0, 0, 0, &err);
    A3_CHECK(a3s_instance_global_count(inst) == 1);
    A3_CHECK_STR(a3s_instance_global_name(inst, 0), "counter");
    A3_CHECK(a3s_instance_global(inst, 0)->as.num == 3);
    A3SValue ten = a3s_num(10);
    A3_CHECK(a3s_instance_set_global(inst, "counter", &ten));
    a3s_call(inst, "main", 0, 0, 0, 0, &err);
    A3_CHECK(a3s_instance_global(inst, 0)->as.num == 11);
    /* optional callbacks: missing is fine, arguments are padded / truncated */
    A3_CHECK(a3s_call(inst, "on_update", 0, 0, 0, 1, &err));
    A3_CHECK(!a3s_call(inst, "mian", 0, 0, 0, 0, &err));
    A3_CHECK_STR(err.hint, "Did you mean 'main'?");
    A3SValue args[2] = { a3s_num(20), a3s_num(99) };
    A3SValue r;
    A3_CHECK(a3s_call(inst, "fib", args, 2, &r, 0, &err) && r.as.num == 6765);
    a3s_instance_destroy(inst);
    A3_CHECK(a3s_module_has_function(0, "x") == 0);
    A3_CHECK_EQ_INT(a3s_live_objects(), live);
}

typedef struct ErrCase { const char *src; i32 line; const char *msg; const char *hint; } ErrCase;

A3_TEST(script_errors) {
    i64 live = a3s_live_objects();
    static const ErrCase cases[] = {
        /* compile errors */
        { "fn main() {\n  let speed = 3\n  print(sped)\n}", 3, "unknown name 'sped'", "Did you mean 'speed'?" },
        { "fn main() {\n  prnt(1)\n}", 2, "unknown name 'prnt'", "Did you mean 'print'?" },
        { "fn main() {\n  if 1 < 2 {\n    print(1)\n", 2, "this { is never closed", 0 },
        { "fn main() {\n  clamp(1, 2)\n}", 2, "'clamp' was given 2 values, but it needs clamp(x, lo, hi)", 0 },
        { "fn main() {\n  let y = x + 1\n}", 2, "unknown name 'x'", 0 },
        { "fn main() {\n  let a = 1\n  a + 1\n}", 3, "this line has a value but does nothing with it", 0 },
        { "fn main() { return \"abc }", 1, "a text in quotes is never closed", 0 },
        { "fn main() { break }", 1, "'break' must be inside a loop", 0 },
        { "fn f() {}\nfn f() {}", 2, "the function 'f' is written twice", 0 },
        { "fn main() { let x = 3 @ 4 }", 1, "unexpected character '@'", 0 },
        /* runtime errors (line of the failing instruction) */
        { "fn main() {\n  let a = 0\n  print(10 / a)\n}", 3, "division by zero", 0 },
        { "fn main() {\n  let l = [1, 2]\n\n  print(l[5])\n}", 4, "index 5 is outside the list (it has 2 items)", 0 },
        { "fn main() {\n  print(\"a\" - 1)\n}", 2, "cannot subtract text \"a\" and number 1", 0 },
        { "fn main() {\n  let v = vec3(1, 2, 3)\n  print(v.w)\n}", 3, "a vec3 has no field 'w'", 0 },
        { "fn g(a, b) { return a }\nfn main() {\n  g(1)\n}", 3, "'g' needs 2 values, but was given 1", 0 },
        { "fn main() {\n  sqrt(-1)\n}", 2, "sqrt() of a negative number (-1)", "Use it like this: sqrt(x)" },
        { "fn main() {\n  let n = nil\n  print(n.pos)\n}", 3, "tried to read '.pos' of nil", 0 },
        { "fn r(n) { return r(n + 1) }\nfn main() { r(0) }", 1, "too many nested function calls", 0 },
        { "fn main() {\n  let x = 1\n  x()\n}", 3, "tried to call number 1, which is not a function", 0 },
        { "fn main() {\n  push(3, 4)\n}", 2, "value 1 should be a list, but it is number", 0 },
        { "fn main() {\n  assert(1 > 2, \"math is broken\")\n}", 2, "assertion failed: math is broken", 0 },
    };
    for (u32 i = 0; i < A3_ARRAY_COUNT(cases); ++i) {
        A3SError err;
        b32 ok = run_main(cases[i].src, &err);
        A3_CHECK_MSG(!ok, "case %u should fail", i);
        A3_CHECK_MSG(err.line == cases[i].line, "case %u: line %d, expected %d (%s)", i, err.line, cases[i].line, err.message);
        A3_CHECK_MSG(a3_streq(err.message, cases[i].msg), "case %u: message \"%s\"", i, err.message);
        if (cases[i].hint) A3_CHECK_MSG(a3_streq(err.hint, cases[i].hint), "case %u: hint \"%s\"", i, err.hint);
    }
    /* endless loops stop at the budget instead of freezing the game */
    a3s_set_budget(100000);
    A3SError err;
    A3_CHECK(!run_main("fn main() {\n  let i = 0\n  while true { i += 1 }\n}", &err));
    A3_CHECK(a3_str_starts_with(err.message, "the script ran too long"));
    A3_CHECK(err.line == 3);
    A3_CHECK_STR(err.function, "main");
    a3s_set_budget(5000000);
    /* errors in the middle of expressions do not leak */
    for (int k = 0; k < 10; ++k) run_main("fn main() { let l = [\"a\", [\"b\"]]\n print(\"x\" + l[1][0] + str(l) + [1][3]) }", &err);
    A3_CHECK_EQ_INT(a3s_live_objects(), live);
}

/* ---- host bindings: a fake entity with a position field ---- */

static f32 g_pos[3];
static i32 g_sets;

static b32 host_get(A3SVM *vm, const A3SValue *obj, const char *name, A3SValue *out) {
    if (obj->type == A3S_ENTITY && a3_streq(name, "position")) { *out = a3s_vec3(g_pos[0], g_pos[1], g_pos[2]); return 1; }
    if (obj->type == A3S_ENTITY && a3_streq(name, "Transform")) { *out = a3s_comp(obj->as.guid, 1); return 1; }
    if (obj->type == A3S_COMP && a3_streq(name, "position")) { *out = a3s_vec3(g_pos[0], g_pos[1], g_pos[2]); return 1; }
    return a3s_fail(vm, "no field '%s'", name);
}

static b32 host_set(A3SVM *vm, A3SValue *obj, const char *name, const A3SValue *v) {
    if (a3_streq(name, "position") && v->type == A3S_VEC3) { g_pos[0] = v->as.v[0]; g_pos[1] = v->as.v[1]; g_pos[2] = v->as.v[2]; g_sets++; return 1; }
    if (obj->type == A3S_ENTITY && a3_streq(name, "Transform") && v->type == A3S_COMP) return 1; /* write-back of the same component */
    return a3s_fail(vm, "cannot set '%s'", name);
}

A3_TEST(script_host_bindings) {
    i64 live = a3s_live_objects();
    host_reset();
    g_test_host.get_field = host_get;
    g_test_host.set_field = host_set;
    a3s_set_host(&g_test_host);
    g_pos[0] = 1; g_pos[1] = 2; g_pos[2] = 3;
    g_sets = 0;
    const char *src =
        "let speed = 2\n"
        "fn on_update(dt) {\n"
        "    self.position.y = 10\n"
        "    self.Transform.position.x += speed * dt\n"
        "    self.position = self.position + vec3(0, 0, 1)\n"
        "}\n";
    A3SError err;
    A3SModule *m = a3s_compile("mover", src, a3_strlen(src), &err);
    A3_CHECK_MSG(m != 0, "%s", err.message);
    A3SInstance *inst = a3s_instance_create(m, a3s_entity(77), 0, &err);
    a3s_module_release(m);
    A3_CHECK(inst != 0);
    A3SValue dt = a3s_num(0.5);
    A3_CHECK_MSG(a3s_call(inst, "on_update", &dt, 1, 0, 1, &err), "%s line %d", err.message, err.line);
    A3_CHECK(g_pos[0] == 2 && g_pos[1] == 10 && g_pos[2] == 4);
    A3_CHECK(g_sets == 3);
    a3s_instance_destroy(inst);
    host_reset();
    A3_CHECK_EQ_INT(a3s_live_objects(), live);
}

A3_TEST(script_natives_and_lists) {
    i64 live = a3s_live_objects();
    A3SError err;
    const char *src =
        "fn main() {\n"
        "    let l = range(5)\n"
        "    let shared = l\n"
        "    push(shared, 5)\n"            /* lists are shared */
        "    let c = copy(l)\n"
        "    push(c, 6)\n"
        "    print(len(l), len(c), pop(c), remove_at(l, 0), remove(l, 3), contains(l, 3))\n"
        "    insert(l, 0, \"first\")\n"
        "    print(l, index_of(l, 4), type(l), type(1), type(\"s\"), type(true), type(vec3()))\n"
        "    let big = list(3, [0])\n"
        "    print(big, big == [[0], [0], [0]], [1, 2] + [3])\n"
        "    random_seed(4)\n"
        "    let a = random_int(1, 6)\n"
        "    random_seed(4)\n"
        "    print(a == random_int(1, 6), format_number(3.14159, 2), trim(\"  hi  \"), starts_with(\"engine\", \"eng\"))\n"
        "    for ch in \"abc\" { print(ch) }\n"
        "}\n";
    A3_CHECK_MSG(run_main(src, &err), "%s (line %d)", err.message, err.line);
    A3_CHECK_STR(g_out,
        "6 7 6 0 true false\n"
        "[\"first\", 1, 2, 4, 5] 3 list number text bool vec3\n"
        "[[0], [0], [0]] true [1, 2, 3]\n"
        "true 3.14 hi true\n"
        "a\nb\nc\n");
    A3_CHECK_EQ_INT(a3s_live_objects(), live);
}
