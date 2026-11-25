/* Star Road Recomp platform layer (task-22): writer for the hid shared memory block (the 0x40000 byte block IAppletResource hands out).
 * Our own code; layout facts from the public Switch documentation (switchbrew "HID shared memory", the libnx and yuzu structure
 * definitions), checked against the game's own SDK by tests/platform/test_platform.c and the guest-side log (docs/PLATFORM.md).
 *
 *   0x9A00 + 0x5000 * n   NpadInternalState for npad n (0..7 = No.1..No.8, 8 = handheld, 9 = other)
 *     +0x0000 u32 style_set (nn::hid::NpadStyleTag bits), +0x0004 u32 joy_assignment_mode (0 dual)
 *     +0x0028 full key lifo (the only one written: the game reads NpadFullKeyState through nn::hid::GetNpadStates)
 *     Lifo (0x350 bytes): u64 timestamp, u64 buffer_count (17), u64 tail (newest entry), u64 count, then 17 entries of 0x30 bytes:
 *     u64 atomic_sampling_number (the SDK reads it before and after copying the entry and retries when they differ), then the 0x28 byte
 *     NpadFullKeyState: u64 sampling_number, u64 buttons, s32 lx, s32 ly, s32 rx, s32 ry, u32 attributes, u32 pad.
 *     The SDK also requires consecutive sampling numbers inside the ring (it loops until it sees them), so every entry gets the next number.
 *     Confirmed on the game's own sdk by disassembling nn::hid::GetNpadStates (entry stride 0x30, count at lifo+0x18, tail at lifo+0x10).
 *   +0x4130.. battery levels (written as full). */
#pragma once
#include <stdint.h>

#include "padstate.h"

#define HIDSHM_SIZE          0x40000
#define HIDSHM_NPAD_BASE     0x9A00
#define HIDSHM_NPAD_STRIDE   0x5000
#define HIDSHM_NPAD_COUNT    10
#define HIDSHM_LIFO_FULLKEY  0x28
#define HIDSHM_LIFO_ENTRIES  17
#define HIDSHM_LIFO_HEADER   0x20
#define HIDSHM_ENTRY_SIZE    0x30
#define HIDSHM_STYLE_FULLKEY (1u << 0)           /* nn::hid::NpadStyleFullKey: the Pro Controller */
#define HIDSHM_ATTR_CONNECTED (1u << 0)

typedef struct HidShmWriter {
    uint64_t sampling[HIDSHM_NPAD_COUNT];       /* last sampling number written per npad */
    uint32_t tail[HIDSHM_NPAD_COUNT], count[HIDSHM_NPAD_COUNT];
    int started;
} HidShmWriter;

void hidshm_writer_init(HidShmWriter *w);
/* One 5 ms sample for players 0..7 (connected_mask bit n: npad n holds a Pro Controller, else it reads as no controller). Rewrites the
 * static fields too, so it stays correct when the guest maps the block after earlier writes. */
void hidshm_publish(HidShmWriter *w, void *shm, const PadState pads[PAD_MAX_PLAYERS], uint32_t connected_mask, uint64_t timestamp);

/* Reader for tests and logs: copies the newest `max` entries of an npad's full key lifo, newest first; returns how many were valid. */
typedef struct HidShmSample { uint64_t sampling, buttons; int32_t lx, ly, rx, ry; uint32_t attributes; } HidShmSample;
int hidshm_read_fullkey(const void *shm, int npad, HidShmSample *out, int max);
uint32_t hidshm_read_style(const void *shm, int npad);
