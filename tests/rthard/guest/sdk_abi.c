/* The test sdk's own EH ABI entry points (our own code). They must never run: the runtime replaces them at their
 * addresses (eh_post_register) and the lifter routes direct calls to them through the dispatch table. */
#include "guest.h"
#define TRAP_FN(n) __attribute__((visibility("default"), noinline)) void n(void) { out("FAIL sdk " #n " ran (redirect missing)"); __builtin_trap(); }
TRAP_FN(__cxa_free_exception)
TRAP_FN(__cxa_rethrow)
TRAP_FN(__cxa_end_catch)
TRAP_FN(_Unwind_Resume)
TRAP_FN(__gxx_personality_v0)
TRAP_FN(__cxa_call_unexpected)
TRAP_FN(__cxa_allocate_exception)
TRAP_FN(__cxa_throw)
TRAP_FN(__cxa_begin_catch)
TRAP_FN(__cxa_get_exception_ptr)
TRAP_FN(_ZSt9terminatev)
