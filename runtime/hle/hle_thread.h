/* Star Road Recomp guest thread control API (task-13). ONE header for thread enumeration, suspend and resume; task-14
 * (exceptions, GC) uses it for stop-the-world and for reading a suspended thread's registers. Our own code.
 *
 * Model: every guest thread is a host thread running lifted code with its own Ctx. Lifted code keeps registers in host
 * registers between svc boundaries, so a suspended thread's Ctx can be stale; read the host CONTEXT as well
 * (hle_thread_host_context) when scanning conservatively. Guest memory is identity mapped, so stacks are plain host memory.
 *
 * Rules for a caller that holds threads suspended:
 *   - do not allocate from the CRT heap or take a lock a suspended thread may hold (a thread can be suspended inside malloc);
 *     use hle_stop_world() which also takes the HLE service lock so no thread is suspended inside a service handler.
 *   - resume exactly as many times as you suspended (counts nest).
 * Thread ids are the guest tids (svcGetThreadId values). */
#pragma once
#include "hle.h"

typedef struct HleThreadInfo {
    uint32_t tid, handle;
    int prio, core, suspended;                 /* suspended: nesting count */
    uint64_t stack_base, stack_top, tlr;       /* guest stack range as given at creation (base 0 if unknown), thread local region */
    Ctx *ctx;                                  /* guest register file (may be stale between svc boundaries) */
    HANDLE host;                               /* host thread handle (not to be closed) */
    int running;                               /* started and not exited */
    char name[24];
} HleThreadInfo;

/* snapshot of all threads ever created that are started and not exited; returns the number written (at most max) */
int hle_thread_list(HleThreadInfo *out, int max);
int hle_thread_find(uint32_t tid, HleThreadInfo *out);        /* 0 found, -1 not found */

/* single thread control; 0 on success. Suspending the calling thread is refused (-2). */
int hle_thread_suspend(uint32_t tid);
int hle_thread_resume(uint32_t tid);
int hle_thread_host_context(uint32_t tid, CONTEXT *out);      /* thread must be suspended; fills the host registers */

/* stop every other running guest thread, holding the HLE service lock so none is parked inside a service; nesting is not supported */
int hle_stop_world(void);                                     /* returns the number of threads stopped */
void hle_resume_world(void);

/* svcSetThreadActivity (0x32) backend: paused 1 suspends, 0 resumes */
int hle_thread_set_activity(RtThread *t, int paused);

/* implemented in svc.c / hle_ipc.c */
int svc_thread_registry(RtThread **out, int max);
void hle_ipc_lock(void);
void hle_ipc_unlock(void);
