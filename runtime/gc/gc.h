/* Star Road Recomp cooperative GC safepoints (task-14, docs/RECOMPILER.md 4.5). Our own code.
 * Boehm stops the world through nn::os::SuspendThread / GetThreadContext, which the lifted sdk turns into
 * svcSetThreadActivity (0x32) and svcGetThreadContext3 (0x33). A pause request is honored at the target's
 * next safepoint: a loop back-edge poll (gc_poll), the exit of an svc, or the end of a blocking HLE call. */
#pragma once
#include "rt_internal.h"

/* svc 0x32: pause = 1 suspends (returns once the target is stopped), 0 resumes. Returns a Horizon result. */
uint32_t gc_set_activity(Ctx *c, RtThread *t, int pause);
/* svc 0x33: 1 if t is paused and stopped (its Ctx then holds every register), else 0 (RES_INVALID_STATE). */
int gc_thread_stopped(RtThread *t);

/* Safe region: the calling thread's Ctx is complete and it runs no guest code until gc_safe_leave.
 * rt_svc brackets every svc with these; a blocking HLE call (host wait) must do the same. */
void gc_safe_enter(Ctx *c);
void gc_safe_leave(Ctx *c);

void gc_summary(void);
