/* Star Road Recomp C++ exceptions, unwind by return (task-14, docs/RECOMPILER.md 4.4). Our own code.
 * Runtime-side API for the interpreter and the boot driver. Generated code uses only rt.h. */
#pragma once
#include "rt_internal.h"

/* Interpreter landing pad: called with c->exc set right after a call whose guest return address is ra.
 * Returns 1 and the pad address when the frame lands (c->x[0] = exception record, c->x[1] = selector). */
int eh_interp_land(Ctx *c, uint64_t ra, uint64_t *pad);

/* Static call-site lookup through .eh_frame_hdr, FDE and LSDA (the same answer the lifter bakes in).
 * Returns 0 no FDE or no LSDA, 1 site with a pad, 2 site without a pad, 3 no call-site entry (terminate). */
int eh_lookup_site(uint64_t ra, uint64_t *pad, uint64_t *lsda, uint64_t *action);

const char *eh_type_name(uint64_t type_info, char *buf, size_t n);
void eh_summary(void);
