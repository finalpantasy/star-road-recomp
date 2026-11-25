/* Star Road Recomp platform layer (task-22): hid shared memory writer. Our own code. See hidshm.h for the layout. */
#include "hidshm.h"

#include <stdatomic.h>
#include <string.h>

static uint8_t *npad_base(void *shm, int n) { return (uint8_t *)shm + HIDSHM_NPAD_BASE + (size_t)n * HIDSHM_NPAD_STRIDE; }

void hidshm_writer_init(HidShmWriter *w) { memset(w, 0, sizeof *w); }

typedef struct { uint64_t timestamp, buffer_count, tail, count; } LifoHeader;
typedef struct { uint64_t atomic_sampling, sampling, buttons; int32_t lx, ly, rx, ry; uint32_t attributes, pad; } LifoEntry;
_Static_assert(sizeof(LifoHeader) == HIDSHM_LIFO_HEADER, "lifo header");
_Static_assert(sizeof(LifoEntry) == HIDSHM_ENTRY_SIZE, "lifo entry");

void hidshm_publish(HidShmWriter *w, void *shm, const PadState pads[PAD_MAX_PLAYERS], uint32_t connected_mask, uint64_t timestamp) {
    for (int n = 0; n < PAD_MAX_PLAYERS; n++) {
        uint8_t *base = npad_base(shm, n);
        int conn = (connected_mask >> n) & 1;
        uint32_t style = conn ? HIDSHM_STYLE_FULLKEY : 0, assign = 0;
        memcpy(base + 0x0, &style, 4);
        memcpy(base + 0x4, &assign, 4);
        if (conn) {                                                               /* battery full (nn::hid::NpadPowerInfo level 4) */
            uint32_t full = 4;
            memcpy(base + 0x4130, &full, 4); memcpy(base + 0x4134, &full, 4); memcpy(base + 0x4138, &full, 4);
        }
        if (!conn) continue;
        uint8_t *lifo = base + HIDSHM_LIFO_FULLKEY;
        uint32_t tail = w->started ? (w->tail[n] + 1) % HIDSHM_LIFO_ENTRIES : 0;
        LifoEntry e;
        memset(&e, 0, sizeof e);
        e.sampling = e.atomic_sampling = ++w->sampling[n];
        e.buttons = pads[n].buttons;
        e.lx = pads[n].lx; e.ly = pads[n].ly; e.rx = pads[n].rx; e.ry = pads[n].ry;
        e.attributes = HIDSHM_ATTR_CONNECTED;
        memcpy(lifo + HIDSHM_LIFO_HEADER + (size_t)tail * HIDSHM_ENTRY_SIZE, &e, sizeof e);   /* entry first, then the header that points at it */
        atomic_thread_fence(memory_order_seq_cst);
        w->tail[n] = tail;
        if (w->count[n] < HIDSHM_LIFO_ENTRIES) w->count[n]++;
        LifoHeader h = {timestamp, HIDSHM_LIFO_ENTRIES, tail, w->count[n]};
        memcpy(lifo, &h, sizeof h);
    }
    w->started = 1;
}

int hidshm_read_fullkey(const void *shm, int npad, HidShmSample *out, int max) {
    const uint8_t *lifo = (const uint8_t *)shm + HIDSHM_NPAD_BASE + (size_t)npad * HIDSHM_NPAD_STRIDE + HIDSHM_LIFO_FULLKEY;
    LifoHeader h;
    memcpy(&h, lifo, sizeof h);
    int n = 0;
    if (h.buffer_count != HIDSHM_LIFO_ENTRIES || h.tail >= HIDSHM_LIFO_ENTRIES) return 0;
    for (uint64_t i = 0; i < h.count && n < max; i++) {
        uint64_t idx = (h.tail + HIDSHM_LIFO_ENTRIES - i) % HIDSHM_LIFO_ENTRIES;
        LifoEntry e;
        memcpy(&e, lifo + HIDSHM_LIFO_HEADER + idx * HIDSHM_ENTRY_SIZE, sizeof e);
        out[n].sampling = e.sampling; out[n].buttons = e.buttons;
        out[n].lx = e.lx; out[n].ly = e.ly; out[n].rx = e.rx; out[n].ry = e.ry; out[n].attributes = e.attributes;
        n++;
    }
    return n;
}

uint32_t hidshm_read_style(const void *shm, int npad) {
    uint32_t s;
    memcpy(&s, (const uint8_t *)shm + HIDSHM_NPAD_BASE + (size_t)npad * HIDSHM_NPAD_STRIDE, 4);
    return s;
}
