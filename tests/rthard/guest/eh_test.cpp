// task-14 AC1 exception tests (our own code): throw, catch, finally (cleanups), rethrow, type matching, callee-saved
// register restore at landing pads, exceptions crossing the sdk, and frames run by the interpreter.
// Every check prints "T <name> ok" or "T <name> FAIL ..."; the last line is "EH DONE pass=<n> fail=<n>".
#include "guest.h"

extern "C" {
void sdk_throw_int(int v);
void sdk_throw_shared(int v);
void sdk_call_with_cleanup(void (*cb)(), int *flag);
int sdk_catch_from_main(void (*cb)());
}

static int g_pass, g_fail;
static void check(const char *name, bool ok, long got = 0) {
    char b[80];
    u64 n = 0;
    const char *p = "T ";
    while (*p) b[n++] = *p++;
    while (*name && n < 60) b[n++] = *name++;
    p = ok ? " ok" : " FAIL got=";
    while (*p) b[n++] = *p++;
    b[n] = 0;
    if (ok) { g_pass++; out(b); }
    else { g_fail++; out_num(b, got); }
}

struct A { int v; virtual ~A() {} };
struct B : A { int w; };
struct X { long pad[3]; virtual void f() {} };
struct C : X, A { int z; };                       // A at a nonzero offset (vmi, public)
struct V : virtual A { int y; };                  // virtual base
struct __attribute__((visibility("hidden"))) SharedErr { int code; };

static int g_dtor_count;
struct Counted { int id; Counted(int i) : id(i) {} Counted(const Counted &o) : id(o.id) {} ~Counted() { g_dtor_count++; } };
struct Flag { int *f; ~Flag() { (*f)++; } };

__attribute__((noinline)) static void throw_int(int v) { throw v; }
__attribute__((noinline)) static void throw_b(int v, int w) { B b; b.v = v; b.w = w; throw b; }
__attribute__((noinline)) static void throw_c(int v) { C c; c.v = v; c.z = 3; throw c; }
__attribute__((noinline)) static void throw_v(int v) { V x; x.v = v; x.y = 9; throw x; }
__attribute__((noinline)) static void throw_counted(int id) { throw Counted(id); }
static B g_b;
__attribute__((noinline)) static void throw_ptr() { g_b.v = 77; throw &g_b; }
__attribute__((noinline)) static void throw_str() { throw "literal"; }

// clobbers every callee-saved register (it saves them in its prologue, but the throw skips its epilogue)
__attribute__((noinline)) static void throw_clobber(int v) {
    __asm__ volatile("mov x19, #0x1901\n mov x20, #0x2001\n mov x21, #0x2101\n mov x22, #0x2201\n mov x23, #0x2301\n"
                     "mov x24, #0x2401\n mov x25, #0x2501\n mov x26, #0x2601\n mov x27, #0x2701\n mov x28, #0x2801\n"
                     "fmov d8, xzr\n fmov d9, xzr\n fmov d10, xzr\n fmov d11, xzr\n fmov d12, xzr\n fmov d13, xzr\n fmov d14, xzr\n fmov d15, xzr\n"
                     ::: "x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28",
                         "d8", "d9", "d10", "d11", "d12", "d13", "d14", "d15");
    throw v;
}
__attribute__((noinline)) static void middle_cleanup(int v, int *flag) { Flag f{flag}; throw_int(v); }
__attribute__((noinline)) static void rethrow_inner(int v, int *seen) {
    try { throw_int(v); } catch (...) { (*seen)++; throw; }
}
__attribute__((noinline)) static void rethrow_counted(int *seen) {
    try { throw_counted(5); } catch (Counted &c) { (*seen) += c.id; throw; }
}
__attribute__((noinline)) static void throw_in_catch(int *seen) {
    try { throw_int(1); } catch (int) { (*seen)++; throw_b(2, 3); }
}
static void cb_throw_42() { throw_int(42); }
static void cb_throw_b() { throw_b(5, 6); }

// run by the interpreter (the test lifter lists it with a reason): a catch and a cleanup inside interpreted code
extern "C" __attribute__((noinline)) int interp_frame(int v, int *flag) {
    try { Flag f{flag}; throw_int(v); } catch (int x) { return x + 1; }
    return -1;
}
extern "C" __attribute__((noinline)) void interp_cleanup_only(int v, int *flag) { Flag f{flag}; throw_int(v); }

// loops so the lifter emits back-edge polls (task-14 AC2 code path, here only run)
__attribute__((noinline)) static long loop_sum(long n) { long s = 0; for (long i = 0; i < n; i++) { s += i ^ (s >> 3); __asm__ volatile(""); } return s; }

extern "C" void eh_main() {
    out("EH START");
    // 1. throw and catch by value
    { int got = 0; try { throw_int(11); } catch (int v) { got = v; } check("catch_int", got == 11, got); }
    // 2. derived caught as base, and as itself
    { int got = 0; try { throw_b(7, 8); } catch (A &a) { got = a.v; } check("catch_base_si", got == 7, got); }
    { int got = 0; try { throw_b(7, 8); } catch (B &b) { got = b.w; } check("catch_exact", got == 8, got); }
    // 3. non-matching clauses are skipped
    { int got = 0; try { throw_b(1, 2); } catch (int) { got = -1; } catch (X &) { got = -2; } catch (A &a) { got = 10 + a.v; } check("skip_clauses", got == 11, got); }
    // 4. vmi base at an offset (adjusted pointer), virtual base
    { int got = 0; try { throw_c(21); } catch (A &a) { got = a.v; } check("catch_vmi_offset", got == 21, got); }
    { int got = 0; try { throw_v(31); } catch (A &a) { got = a.v; } check("catch_virtual_base", got == 31, got); }
    // 5. catch-all
    { int got = 0; try { throw_b(1, 1); } catch (...) { got = 1; } check("catch_all", got == 1, got); }
    // 6. cleanup (finally) in a frame without a handler, then the outer catch
    { int flag = 0, got = 0; try { middle_cleanup(4, &flag); } catch (int v) { got = v; } check("cleanup_runs", flag == 1 && got == 4, flag * 100 + got); }
    // 7. rethrow with catch-all and with a typed handler; the exception object is destroyed once
    { int seen = 0, got = 0; try { rethrow_inner(9, &seen); } catch (int v) { got = v; } check("rethrow", seen == 1 && got == 9, seen * 100 + got); }
    { int seen = 0, got = 0; g_dtor_count = 0;
      try { rethrow_counted(&seen); } catch (Counted &c) { got = c.id; }
      check("rethrow_object_lifetime", seen == 5 && got == 5 && g_dtor_count == 1, g_dtor_count * 100 + got); }
    // 8. throw from inside a catch block
    { int seen = 0, got = 0; try { throw_in_catch(&seen); } catch (B &b) { got = b.w; } catch (int) { got = -1; } check("throw_in_catch", seen == 1 && got == 3, got); }
    // 9. pointers: B* caught as A*, string literal as const char*
    { int got = 0; try { throw_ptr(); } catch (A *a) { got = a->v; } check("catch_ptr_base", got == 77, got); }
    { int got = 0; try { throw_str(); } catch (const char *s) { got = s[0]; } check("catch_const_char_ptr", got == 'l', got); }
    // 10. callee-saved registers survive the landing pad (the thrower skipped its epilogue)
    {
        volatile long seed = 5;
        long a = seed * 3, b = seed * 5, c = seed * 7, d = seed * 11, e = seed * 13, f = seed * 17, g = seed * 19, h = seed * 23;
        double fa = (double)seed * 1.5, fb = (double)seed * 2.5;
        int got = 0;
        try { throw_clobber(3); } catch (int v) { got = v; }
        long sum = a + b + c + d + e + f + g + h;
        check("callee_saved_restored", got == 3 && sum == 5 * (3 + 5 + 7 + 11 + 13 + 17 + 19 + 23) && fa == 7.5 && fb == 12.5, sum);
    }
    // 11. crossing the sdk: thrown by the sdk (its own __cxa_throw, redirected), caught here
    { int got = 0; try { sdk_throw_int(55); } catch (int v) { got = v; } check("sdk_throws_main_catches", got == 55, got); }
    { int got = 0; try { sdk_throw_shared(66); } catch (SharedErr &e) { got = e.code; } check("type_match_by_name", got == 66, got); }
    { int flag = 0, got = 0; try { sdk_call_with_cleanup(cb_throw_42, &flag); } catch (int v) { got = v; } check("cleanup_in_sdk_frame", flag == 1 && got == 42, flag * 100 + got); }
    { int got = sdk_catch_from_main(cb_throw_42); check("sdk_catches_main_throw", got == 42, got); }
    { int got = sdk_catch_from_main(cb_throw_b); check("sdk_catch_all", got == -1, got); }
    // 12. interpreted frames: catch and cleanup inside the interpreter, cleanup in an interpreted frame between lifted ones
    { int flag = 0; int got = interp_frame(70, &flag); check("interp_catch", got == 71 && flag == 1, got + flag * 1000); }
    { int flag = 0, got = 0; try { interp_cleanup_only(80, &flag); } catch (int v) { got = v; } check("interp_cleanup", flag == 1 && got == 80, got + flag * 1000); }
    // 13. many throws in a loop (no leak, no stale state)
    { int got = 0; for (int i = 0; i < 2000; i++) { try { throw_int(i); } catch (int v) { got += v == i; } } check("loop_2000_throws", got == 2000, got); }
    check("loop_poll_code_runs", loop_sum(100000) != 0);
    out_num("EH DONE fail=", g_fail);
    out_num("EH DONE pass=", g_pass);
}
