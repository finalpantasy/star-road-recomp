/* Star Road Recomp (task-22): the hid shared memory writer lives in runtime/platform/hidshm.c (no SDL needed, also used by the platform
 * unit test). Including it here puts it in the hle/*.c set that the runtime build and every test harness compile, so hle_hid.c always links. */
#include "../platform/hidshm.c"
