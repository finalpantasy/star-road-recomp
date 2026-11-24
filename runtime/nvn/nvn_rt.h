/* Star Road Recomp: NVN runtime interface (task-12). Our own code (D-008).
 *
 * C interface between the NVN library (C++) and the runtime (task-5, C). See docs/NVN.md.
 *
 *   The runtime calls nvn_rt_register() once at startup (after the HLE table exists). It creates one HLE slot per NVN proc and
 *   defines the import `nvnBootstrapLoader`. Guest code calls nvnBootstrapLoader("nvnDeviceInitialize" / "nvnDeviceGetProcAddress");
 *   everything else comes from nvnDeviceGetProcAddress, which returns the slot's synthetic guest address.
 */
#ifndef SR_NVN_RT_H
#define SR_NVN_RT_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
struct Ctx;
size_t nvn_proc_count(void);
const char* nvn_proc_name(size_t i);
int nvn_proc_implemented(size_t i);                       /* 1: real implementation, 0: logging stub */
void (*nvn_proc_thunk(size_t i))(struct Ctx*);            /* Ctx adapter for proc i (real or stub) */
void nvn_set_proc_resolver(void* (*fn)(const char* name));/* name -> address handed to the guest (null: host pointer) */
void nvn_rt_register(void);                               /* implemented in nvn_register.c */
#ifdef __cplusplus
}
#endif
#endif
