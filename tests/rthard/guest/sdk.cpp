// Test "sdk" module for task-14 (our own code). Stands in for the game's sdk: it defines the cxxabiv1 type_info
// vtables and its OWN __cxa_* entry points (traps), and throws and catches with direct `bl` calls to them, the way the
// real sdk calls its libc++abi internally. The runtime must redirect those addresses to runtime/eh, or the traps fire.
#include "guest.h"

extern "C" {
// type_info vtables: contents never read; the runtime only compares type_info vtable pointers against symbol+16
__attribute__((used, visibility("default"))) void *vt_class[8] __asm__("_ZTVN10__cxxabiv117__class_type_infoE");
__attribute__((used, visibility("default"))) void *vt_si[8] __asm__("_ZTVN10__cxxabiv120__si_class_type_infoE");
__attribute__((used, visibility("default"))) void *vt_vmi[8] __asm__("_ZTVN10__cxxabiv121__vmi_class_type_infoE");
__attribute__((used, visibility("default"))) void *vt_ptr[8] __asm__("_ZTVN10__cxxabiv119__pointer_type_infoE");
__attribute__((used, visibility("default"))) void *vt_fund[8] __asm__("_ZTVN10__cxxabiv123__fundamental_type_infoE");

// fundamental type_infos live in the C++ runtime (the sdk), as in the game
struct TI { void **vt; const char *name; };
struct PTI { void **vt; const char *name; unsigned flags; const void *pointee; };
__attribute__((used, visibility("default"))) TI ti_int __asm__("_ZTIi") = {vt_fund + 2, "i"};
__attribute__((used, visibility("default"))) TI ti_char __asm__("_ZTIc") = {vt_fund + 2, "c"};
__attribute__((used, visibility("default"))) PTI ti_pkc __asm__("_ZTIPKc") = {vt_ptr + 2, "PKc", 1, &ti_char};

}
// the sdk's own EH ABI entry points are in sdk_abi.c (a separate file, so the compiler cannot see that they trap)
extern "C" {
void *__cxa_allocate_exception(unsigned long);
void __cxa_throw(void *, void *, void (*)(void *));
}

// same name in both modules, hidden: two distinct type_info objects that must match by name
struct __attribute__((visibility("hidden"))) SharedErr { int code; };
struct __attribute__((visibility("hidden"))) Guard { int *f; ~Guard() { (*f)++; } };

extern "C" {
__attribute__((visibility("default"), noinline)) void sdk_throw_int(int v) { throw v; }
__attribute__((visibility("default"), noinline)) void sdk_throw_shared(int v) { throw SharedErr{v}; }
// an exception from main passes through this sdk frame: its cleanup must run
__attribute__((visibility("default"), noinline)) void sdk_call_with_cleanup(void (*cb)(), int *flag) {
    Guard g{flag};
    cb();
    out("FAIL sdk_call_with_cleanup: callback returned");
}
// the sdk catches what main throws (sdk-internal begin/end catch are redirected)
__attribute__((visibility("default"), noinline)) int sdk_catch_from_main(void (*cb)()) {
    try { cb(); } catch (int v) { return v; } catch (...) { return -1; }
    return 0;
}
}
