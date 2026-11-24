/* Weak defaults so the runtime links alone (task-5). A real game library or interpreter, linked in the same
 * command, supplies strong definitions that win. Our own code. */
#include "rt_internal.h"

__attribute__((weak)) const RegFn g_reg_chunks[1] = {0};
__attribute__((weak)) const uint32_t g_reg_nchunks = 0;

__attribute__((weak)) void interp_run(Ctx *c, uint64_t pc) {
    rt_fatal(c, "guest address has no lifted function and no interpreter is linked", pc);
}
