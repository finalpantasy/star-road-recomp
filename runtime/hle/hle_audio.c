/* Star Road Recomp audio services (task-13): audren:u, audout:u, hwopus and acc:u0 with a null sink. Our own code.
 *
 * The game never needs real sound to boot, so the renderer is a null renderer: RequestUpdate answers with a correctly
 * shaped update buffer (header, memory pool states, empty voice/effect/sink statuses, error info, renderer info) and a
 * host timer signals the system event once per render frame, which is what the guest audio thread waits on. Layout facts
 * (field offsets and section sizes) come from the public Horizon audio renderer documentation and from checking the
 * guest's own sizes against them at run time. No Windows audio device is opened here; --audio is a later step. */
#include "hle.h"
#include "../platform/plat.h"

/* "Auto" commands send both the map-alias and the pointer form of a buffer; the unused one is empty. Pick the one with data. */
static int real_in(IpcCtx *x) {
    for (int i = 0; i < x->npa + x->npx; i++) { IpcBuf *b = ipc_in_buf(x, i); if (b && b->size) return i; }
    return 0;
}
static int real_out(IpcCtx *x) {
    for (int i = 0; i < x->npb + x->npc; i++) { IpcBuf *b = ipc_out_buf(x, i); if (b && b->size) return i; }
    return 0;
}

/* ------------------------------------------------------------------ timer: one host thread paces every frame event */
#define AT_MAX 32
typedef struct AudioTick {
    KObj *ev;
    uint32_t period_ms;
    uint64_t next_ms;
    volatile int active;
    void (*on_tick)(void *user);      /* optional, called under the audio lock */
    void *user;
} AudioTick;
static AudioTick g_ticks[AT_MAX];
static int g_nticks;
static CRITICAL_SECTION g_alock;
static volatile LONG g_timer_started;

static DWORD WINAPI audio_timer_thread(LPVOID unused) {
    (void)unused;
    for (;;) {
        Sleep(2);
        uint64_t now = GetTickCount64();
        EnterCriticalSection(&g_alock);
        for (int i = 0; i < g_nticks; i++) {
            AudioTick *t = &g_ticks[i];
            if (!t->active || now < t->next_ms) continue;
            t->next_ms = now + t->period_ms;
            if (t->on_tick) t->on_tick(t->user);
            if (t->ev) k_event_set(t->ev, 1);
        }
        LeaveCriticalSection(&g_alock);
    }
    return 0;
}

static AudioTick *tick_new(KObj *ev, uint32_t period_ms, void (*on_tick)(void *), void *user) {
    AudioTick *t = 0;
    EnterCriticalSection(&g_alock);
    if (g_nticks < AT_MAX) {
        t = &g_ticks[g_nticks++];
        t->ev = ev; t->period_ms = period_ms ? period_ms : 5; t->next_ms = 0; t->active = 0;
        t->on_tick = on_tick; t->user = user;
    }
    LeaveCriticalSection(&g_alock);
    if (!InterlockedCompareExchange(&g_timer_started, 1, 0)) {
        HANDLE h = CreateThread(0, 0, audio_timer_thread, 0, 0, 0);
        if (h) { SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(h); }
    }
    return t;
}
static void tick_set(AudioTick *t, int active) {
    if (!t) return;
    EnterCriticalSection(&g_alock);
    t->active = active; t->next_ms = 0;
    LeaveCriticalSection(&g_alock);
}

/* ------------------------------------------------------------------ audren:u : IAudioRendererManager */
typedef struct {            /* AudioRendererParameterInternal, 0x34 bytes */
    uint32_t sample_rate, sample_count, mix_buffer_count, submix_count, voice_count, sink_count, effect_count,
             perf_manager_count, voice_drop_enable, splitter_count, splitter_dest_count, external_ctx_size, revision;
} ArParams;

typedef struct {            /* UpdateDataHeader, 0x40 bytes */
    uint32_t revision, behaviour, pools, voices, voice_res, effects, mixes, sinks, perf, splitter, render_info;
    uint32_t reserved[4];
    uint32_t total;
} ArHdr;

#define AR_MAX_POOLS 4096
typedef struct {
    ArParams p;
    KObj *ev;
    AudioTick *tick;
    uint32_t state;         /* 0 started, 1 stopped */
    uint32_t time_limit;
    uint64_t frames;
    uint8_t pool_state[AR_MAX_POOLS];
    int logged;
} AudioRenderer;

static void ar_get_params(IpcCtx *x, ArParams *p) { memset(p, 0, sizeof *p); memcpy(p, x->in, x->in_size < sizeof *p ? x->in_size : sizeof *p); }

static uint64_t ar_work_buffer_size(const ArParams *p) {
    /* The guest allocates this much and hands it back as transfer memory; the null renderer never touches it, so any
     * page-aligned size that grows with the parameters is acceptable. */
    uint64_t n = 0x40000ull + 0x1000ull * (p->voice_count + p->effect_count + p->sink_count + p->submix_count + 4) + 0x400ull * p->mix_buffer_count;
    return (n + 0xFFF) & ~0xFFFull;
}

static void ar_open(IpcCtx *x) {              /* 0 OpenAudioRenderer / 3 OpenAudioRendererForManualExecution */
    extern const SvcIface g_audioren_iface;
    AudioRenderer *r = (AudioRenderer *)calloc(1, sizeof *r);
    ar_get_params(x, &r->p);
    r->time_limit = 80;
    r->ev = k_event_named("audren_frame", 0);
    uint32_t period = r->p.sample_rate ? (uint32_t)((1000ull * r->p.sample_count + r->p.sample_rate / 2) / r->p.sample_rate) : 5;
    r->tick = tick_new(r->ev, period, 0, 0);
    r->state = 1;
    rt_log("[hle] audren: OpenAudioRenderer rate=%u samples=%u mix=%u sub=%u voices=%u sinks=%u effects=%u perf=%u splitter=%u/%u rev=0x%x\n",
            r->p.sample_rate, r->p.sample_count, r->p.mix_buffer_count, r->p.submix_count, r->p.voice_count, r->p.sink_count, r->p.effect_count,
            r->p.perf_manager_count, r->p.splitter_count, r->p.splitter_dest_count, r->p.revision);
    out_obj(x, &g_audioren_iface, r);
}
static void ar_work_size(IpcCtx *x) {         /* 1 GetWorkBufferSize */
    ArParams p; ar_get_params(x, &p);
    out_u64(x, 0, ar_work_buffer_size(&p));
}
extern const SvcIface g_audiodev_iface;
static void ar_get_device(IpcCtx *x) { out_obj(x, &g_audiodev_iface, 0); }   /* 2 GetAudioDeviceService, 4 ...WithRevisionInfo */

CMD_TABLE(g_audren_cmds) = {CMD(0, ar_open), CMD(1, ar_work_size), CMD(2, ar_get_device), CMD(3, ar_open), CMD(4, ar_get_device)};
IFACE(g_audren_iface, "nn::audio::detail::IAudioRendererManager", g_audren_cmds);

/* ------------------------------------------------------------------ IAudioRenderer */
static void r_sample_rate(IpcCtx *x)  { AudioRenderer *r = (AudioRenderer *)x->self->state; out_u32(x, 0, r->p.sample_rate); }
static void r_sample_count(IpcCtx *x) { AudioRenderer *r = (AudioRenderer *)x->self->state; out_u32(x, 0, r->p.sample_count); }
static void r_mix_count(IpcCtx *x)    { AudioRenderer *r = (AudioRenderer *)x->self->state; out_u32(x, 0, r->p.mix_buffer_count); }
static void r_get_state(IpcCtx *x)    { AudioRenderer *r = (AudioRenderer *)x->self->state; out_u32(x, 0, r->state); }
static void r_start(IpcCtx *x)        { AudioRenderer *r = (AudioRenderer *)x->self->state; r->state = 0; tick_set(r->tick, 1); }
static void r_stop(IpcCtx *x)         { AudioRenderer *r = (AudioRenderer *)x->self->state; r->state = 1; tick_set(r->tick, 0); }
static void r_query_event(IpcCtx *x)  { AudioRenderer *r = (AudioRenderer *)x->self->state; out_copy_handle(x, k_handle_for(r->ev, 1)); }
static void r_set_limit(IpcCtx *x)    { AudioRenderer *r = (AudioRenderer *)x->self->state; r->time_limit = in_u32(x, 0); }
static void r_get_limit(IpcCtx *x)    { AudioRenderer *r = (AudioRenderer *)x->self->state; out_u32(x, 0, r->time_limit); }
static void r_exec(IpcCtx *x)         { AudioRenderer *r = (AudioRenderer *)x->self->state; k_event_set(r->ev, 1); }

/* RequestUpdate / RequestUpdateAuto: in buffer 0 is the update request, out buffer 0 the response, out buffer 1 the performance data. */
static void r_update(IpcCtx *x) {
    AudioRenderer *r = (AudioRenderer *)x->self->state;
    int oi = real_out(x);
    IpcBuf *in = ipc_in_buf(x, real_in(x)), *out = ipc_out_buf(x, oi);
    if (!in || !out || in->size < sizeof(ArHdr)) { x->result = MAKE_RES(153, 4); return; }   /* audio: invalid update info */
    const ArHdr *ih = (const ArHdr *)(uintptr_t)in->addr;
    uint32_t pools = ih->pools / 0x20, voices = ih->voices / 0x170, effects = ih->effects / 0xC0, sinks = ih->sinks / 0x140;
    int eff_v2 = ih->revision >= 0x39564552u;                   /* REV9 and later add the result state to the effect status */
    uint32_t eff_sz = eff_v2 ? 0x20 : 0x10;
    uint64_t sz_pools = 0x10ull * pools, sz_voices = 0x10ull * voices, sz_effects = (uint64_t)eff_sz * effects, sz_sinks = 0x20ull * sinks;
    uint64_t sz_perf = ih->perf ? 0x10 : 0, sz_err = 0xB0, sz_info = 0x10;
    uint64_t total = sizeof(ArHdr) + sz_pools + sz_voices + sz_effects + sz_sinks + sz_perf + sz_err + sz_info;
    if (!r->logged) {
        r->logged = 1;
        rt_log("[hle] audren: first RequestUpdate rev=0x%x in=%u out=%llu pools=%u voices=%u effects=%u mixes=%u sinks=%u perf=%u splitter=%u total=%llu\n",
                ih->revision, (unsigned)in->size, (unsigned long long)out->size, pools, voices, effects, ih->mixes, sinks, ih->perf, ih->splitter, (unsigned long long)total);
    }
    if (pools > AR_MAX_POOLS || total > out->size) { x->result = MAKE_RES(153, 4); return; }
    uint8_t *o = (uint8_t *)(uintptr_t)out->addr;
    memset(o, 0, (size_t)total);
    ArHdr *oh = (ArHdr *)o;
    oh->revision = ih->revision;
    oh->behaviour = (uint32_t)sz_err; oh->pools = (uint32_t)sz_pools; oh->voices = (uint32_t)sz_voices; oh->effects = (uint32_t)sz_effects;
    oh->sinks = (uint32_t)sz_sinks; oh->perf = (uint32_t)sz_perf; oh->render_info = (uint32_t)sz_info;
    oh->total = (uint32_t)total;
    /* the sections of the request follow the header in the order behaviour, pools, voice resources, voices, effects, splitter, mixes, sinks, perf */
    const uint8_t *pin = (const uint8_t *)ih + sizeof(ArHdr) + ih->behaviour;
    uint8_t *po = o + sizeof(ArHdr);
    for (uint32_t i = 0; i < pools; i++) {
        uint32_t req = 0;
        memcpy(&req, pin + (uint64_t)i * 0x20 + 0x10, 4);
        if (req == 4) r->pool_state[i] = 5;          /* RequestAttach -> Attached */
        else if (req == 2) r->pool_state[i] = 3;     /* RequestDetach -> Detached */
        uint32_t st = r->pool_state[i];
        memcpy(po + (uint64_t)i * 0x10, &st, 4);
    }
    /* error info: count 0 (already zero); renderer info: elapsed frame count after the performance block */
    uint64_t info_off = sizeof(ArHdr) + sz_pools + sz_voices + sz_effects + sz_sinks + sz_perf + sz_err;
    r->frames += r->p.sample_count ? 1 : 0;
    memcpy(o + info_off, &r->frames, 8);
    k_event_set(r->ev, 0);
}

CMD_TABLE(g_audioren_cmds) = {CMD(0, r_sample_rate), CMD(1, r_sample_count), CMD(2, r_mix_count), CMD(3, r_get_state), CMD(4, r_update),
                              CMD(5, r_start), CMD(6, r_stop), CMD(7, r_query_event), CMD(8, r_set_limit), CMD(9, r_get_limit),
                              CMD(10, r_update), CMD(11, r_exec)};
const SvcIface g_audioren_iface = {"nn::audio::detail::IAudioRenderer", g_audioren_cmds, (int)(sizeof g_audioren_cmds / sizeof g_audioren_cmds[0]), 0};

/* ------------------------------------------------------------------ IAudioDevice */
static const char k_dev_name[] = "AudioTvOutput";
static void dev_list(IpcCtx *x) {              /* 0 ListAudioDeviceName, 6 ...Auto: array of 0x100 byte names */
    char name[0x100] = {0};
    memcpy(name, k_dev_name, sizeof k_dev_name);
    ipc_write_out(x, 0, name, sizeof name);
    out_u32(x, 0, 1);
}
static void dev_set_vol(IpcCtx *x) { (void)x; }
static void dev_get_vol(IpcCtx *x) { float v = 1.0f; out_bytes(x, 0, &v, 4); }
static void dev_active_name(IpcCtx *x) {       /* 3, 10 */
    char name[0x100] = {0};
    memcpy(name, k_dev_name, sizeof k_dev_name);
    ipc_write_out(x, 0, name, sizeof name);
}
static KObj *g_dev_ev[2];
static void dev_event(IpcCtx *x) {             /* 4 QueryAudioDeviceSystemEvent, 11 input, 12 output */
    int i = x->cmd == 12 ? 1 : 0;
    if (!g_dev_ev[i]) g_dev_ev[i] = k_event_named("audio_device", 0);
    out_copy_handle(x, k_handle_for(g_dev_ev[i], 1));
}
static void dev_channels(IpcCtx *x) { out_u32(x, 0, 2); }
static void dev_master_vol(IpcCtx *x) { float v = 1.0f; out_bytes(x, 0, &v, 4); }
CMD_TABLE(g_audiodev_cmds) = {CMD(0, dev_list), CMD(1, dev_set_vol), CMD(2, dev_get_vol), CMD(3, dev_active_name), CMD(4, dev_event),
                              CMD(5, dev_channels), CMD(6, dev_list), CMD(7, dev_set_vol), CMD(8, dev_get_vol), CMD(10, dev_active_name),
                              CMD(11, dev_event), CMD(12, dev_event), CMD(13, dev_master_vol)};
const SvcIface g_audiodev_iface = {"nn::audio::detail::IAudioDevice", g_audiodev_cmds, (int)(sizeof g_audiodev_cmds / sizeof g_audiodev_cmds[0]), 0};

/* ------------------------------------------------------------------ audout:u : IAudioOutManager / IAudioOut */
#define AO_Q 64
typedef struct {
    uint32_t rate, channels, started;
    KObj *ev;
    AudioTick *tick;
    uint64_t pend[AO_Q]; uint32_t pend_n;
    uint64_t rel[AO_Q];  uint32_t rel_n;
    uint64_t played;
#ifdef SMRPG_PLATFORM
    PlatAudioSink *sink;                 /* task-22: a real device (--audio) or NULL: the null sink that consumes everything at once */
    uint64_t wr_total;                   /* bytes handed to the sink so far */
    uint64_t pend_end[AO_Q];             /* wr_total after each pending buffer: it is released once the device has played that far */
#endif
} AudioOut;

static void ao_tick(void *user) {              /* under the audio lock: pretend the DSP consumed every appended buffer */
    AudioOut *a = (AudioOut *)user;
    int any = 0;
#ifdef SMRPG_PLATFORM
    uint64_t consumed = a->sink ? a->wr_total - plat_audio_queued(a->sink) : 0;
#endif
    while (a->pend_n && a->rel_n < AO_Q) {
#ifdef SMRPG_PLATFORM
        if (a->sink && a->pend_end[0] > consumed) break;      /* the device is still playing it: hand it back at the speed it really plays */
        memmove(a->pend_end, a->pend_end + 1, (a->pend_n - 1) * sizeof a->pend_end[0]);
#endif
        a->rel[a->rel_n++] = a->pend[0];
        memmove(a->pend, a->pend + 1, (a->pend_n - 1) * sizeof a->pend[0]);
        a->pend_n--;
        any = 1;
    }
    if (any) k_event_set(a->ev, 1);      /* the guest does not check for an empty result: signal only when a buffer was released */
}
extern const SvcIface g_audioout_iface;
static void am_list(IpcCtx *x) {               /* 0 ListAudioOuts, 2 ...Auto */
    char name[0x100] = {0};
    memcpy(name, "DeviceOut", 10);
    ipc_write_out(x, 0, name, sizeof name);
    out_u32(x, 0, 1);
}
static void am_open(IpcCtx *x) {               /* 1 OpenAudioOut, 3 ...Auto */
    AudioOut *a = (AudioOut *)calloc(1, sizeof *a);
    a->rate = in_u32(x, 0); if (!a->rate) a->rate = 48000;
    a->channels = in_u32(x, 4) & 0xFFFF; if (!a->channels) a->channels = 2;
    a->ev = k_event_named("audout_buffer", 0);
    a->tick = tick_new(0, 10, ao_tick, a);
#ifdef SMRPG_PLATFORM
    a->sink = plat_audio_open(a->rate, a->channels);        /* NULL unless --audio: the default run opens no audio device */
#endif
    char name[0x100] = {0};
    memcpy(name, "DeviceOut", 10);
    ipc_write_out(x, 0, name, sizeof name);
    out_u32(x, 0, a->rate); out_u32(x, 4, a->channels); out_u32(x, 8, 2 /* PcmInt16 */); out_u32(x, 12, 1 /* stopped */);
    rt_log("[hle] audout: OpenAudioOut rate=%u channels=%u\n", a->rate, a->channels);
    out_obj(x, &g_audioout_iface, a);
}
CMD_TABLE(g_audout_cmds) = {CMD(0, am_list), CMD(1, am_open), CMD(2, am_list), CMD(3, am_open)};
IFACE(g_audout_iface, "nn::audio::detail::IAudioOutManager", g_audout_cmds);

static void ao_state(IpcCtx *x)  { AudioOut *a = (AudioOut *)x->self->state; out_u32(x, 0, a->started ? 0 : 1); }
static void ao_start(IpcCtx *x)  {
    AudioOut *a = (AudioOut *)x->self->state; a->started = 1; tick_set(a->tick, 1);
#ifdef SMRPG_PLATFORM
    plat_audio_pause(a->sink, 0);
#endif
}
static void ao_stop(IpcCtx *x)   {
    AudioOut *a = (AudioOut *)x->self->state; a->started = 0; tick_set(a->tick, 0);
#ifdef SMRPG_PLATFORM
    plat_audio_pause(a->sink, 1);
#endif
}
static void ao_append(IpcCtx *x) {             /* 3, 7: AppendAudioOutBuffer(u64 tag, buffer) */
    AudioOut *a = (AudioOut *)x->self->state;
    uint64_t tag = in_u64(x, 0);
    IpcBuf *b = ipc_in_buf(x, real_in(x));
    uint64_t bytes = 0;
    if (b && b->size >= 0x20) memcpy(&bytes, (const uint8_t *)(uintptr_t)b->addr + 0x18, 8);
    EnterCriticalSection(&g_alock);
#ifdef SMRPG_PLATFORM
    if (a->sink && b && b->size >= 0x28 && bytes) {        /* AudioOutBuffer: data pointer @8, size @0x18, data offset @0x20 */
        uint64_t data = 0, off = 0;
        memcpy(&data, (const uint8_t *)(uintptr_t)b->addr + 8, 8);
        memcpy(&off, (const uint8_t *)(uintptr_t)b->addr + 0x20, 8);
        if (data > 0x10000 && bytes <= (1u << 24) && !IsBadReadPtr((void *)(uintptr_t)(data + off), (size_t)bytes)) {
            plat_audio_write(a->sink, (const void *)(uintptr_t)(data + off), (uint32_t)bytes);
            a->wr_total += bytes;
        }
    }
    if (a->pend_n < AO_Q) a->pend_end[a->pend_n] = a->wr_total;
#endif
    if (a->pend_n < AO_Q) a->pend[a->pend_n++] = tag;
    a->played += a->channels ? bytes / (2ull * a->channels) : 0;
    LeaveCriticalSection(&g_alock);
}
static void ao_event(IpcCtx *x) { AudioOut *a = (AudioOut *)x->self->state; out_copy_handle(x, k_handle_for(a->ev, 1)); }
static void ao_released(IpcCtx *x) {           /* 5, 8 */
    AudioOut *a = (AudioOut *)x->self->state;
    int oi = real_out(x);
    IpcBuf *b = ipc_out_buf(x, oi);
    uint32_t cap = b ? (uint32_t)(b->size / 8) : 0, n = 0;
    uint64_t tags[AO_Q] = {0};      /* the guest returns tag[0] whatever the count, so unused entries must read as 0 */
    EnterCriticalSection(&g_alock);
    while (n < cap && n < a->rel_n) { tags[n] = a->rel[n]; n++; }
    memmove(a->rel, a->rel + n, (a->rel_n - n) * sizeof a->rel[0]);
    a->rel_n -= n;
    if (!a->rel_n) k_event_set(a->ev, 0);
    LeaveCriticalSection(&g_alock);
    if (cap) ipc_write_out(x, oi, tags, (uint64_t)(cap < AO_Q ? cap : AO_Q) * 8);
    out_u32(x, 0, n);
}
static void ao_contains(IpcCtx *x) {
    AudioOut *a = (AudioOut *)x->self->state;
    uint64_t tag = in_u64(x, 0);
    uint8_t found = 0;
    EnterCriticalSection(&g_alock);
    for (uint32_t i = 0; i < a->pend_n; i++) if (a->pend[i] == tag) found = 1;
    for (uint32_t i = 0; i < a->rel_n; i++) if (a->rel[i] == tag) found = 1;
    LeaveCriticalSection(&g_alock);
    out_bytes(x, 0, &found, 1);
}
static void ao_count(IpcCtx *x)  { AudioOut *a = (AudioOut *)x->self->state; out_u32(x, 0, a->pend_n + a->rel_n); }
static void ao_played(IpcCtx *x) { AudioOut *a = (AudioOut *)x->self->state; out_u64(x, 0, a->played); }
static void ao_flush(IpcCtx *x) {
    AudioOut *a = (AudioOut *)x->self->state;
    uint8_t any;
    EnterCriticalSection(&g_alock);
    any = a->pend_n != 0;
    while (a->pend_n && a->rel_n < AO_Q) {
#ifdef SMRPG_PLATFORM
        memmove(a->pend_end, a->pend_end + 1, (a->pend_n - 1) * 8);
#endif
        a->rel[a->rel_n++] = a->pend[0]; memmove(a->pend, a->pend + 1, (a->pend_n - 1) * 8); a->pend_n--;
    }
    LeaveCriticalSection(&g_alock);
    out_bytes(x, 0, &any, 1);
}
static void ao_set_vol(IpcCtx *x) { (void)x; }
static void ao_get_vol(IpcCtx *x) { float v = 1.0f; out_bytes(x, 0, &v, 4); }
CMD_TABLE(g_audioout_cmds) = {CMD(0, ao_state), CMD(1, ao_start), CMD(2, ao_stop), CMD(3, ao_append), CMD(4, ao_event), CMD(5, ao_released),
                              CMD(6, ao_contains), CMD(7, ao_append), CMD(8, ao_released), CMD(9, ao_count), CMD(10, ao_played),
                              CMD(11, ao_flush), CMD(12, ao_set_vol), CMD(13, ao_get_vol)};
const SvcIface g_audioout_iface = {"nn::audio::detail::IAudioOut", g_audioout_cmds, (int)(sizeof g_audioout_cmds / sizeof g_audioout_cmds[0]), 0};

/* ------------------------------------------------------------------ hwopus : IHardwareOpusDecoderManager (null decoder: silence) */
typedef struct { uint32_t rate, channels; } OpusDec;
extern const SvcIface g_opusdec_iface;
static void om_open(IpcCtx *x) {               /* 0 OpenHardwareOpusDecoder(rate, channels, work size) */
    OpusDec *d = (OpusDec *)calloc(1, sizeof *d);
    d->rate = in_u32(x, 0); d->channels = in_u32(x, 4);
    if (!d->rate) d->rate = 48000;
    if (!d->channels) d->channels = 2;
    out_obj(x, &g_opusdec_iface, d);
}
static void om_work_size(IpcCtx *x) { out_u32(x, 0, 0x4000); }   /* 1 GetWorkBufferSize */
CMD_TABLE(g_hwopus_cmds) = {CMD(0, om_open), CMD(1, om_work_size), CMD(2, om_open), CMD(3, om_work_size), CMD(4, om_open), CMD(5, om_work_size),
                            CMD(6, om_open), CMD(7, om_work_size), CMD(8, om_work_size), CMD(9, om_work_size)};
IFACE(g_hwopus_iface, "nn::codec::detail::IHardwareOpusDecoderManager", g_hwopus_cmds);

static void od_decode(IpcCtx *x) {             /* DecodeInterleaved*: out u32 consumed, u32 samples (and a u64 perf time on the WithPerf forms) */
    OpusDec *d = (OpusDec *)x->self->state;
    IpcBuf *in = ipc_in_buf(x, 0);
    uint32_t consumed = in ? (uint32_t)in->size : 0, samples = d->rate / 50;
    IpcBuf *o = ipc_out_buf(x, 0);
    if (o) {
        uint64_t bytes = (uint64_t)samples * d->channels * 2;
        if (bytes > o->size) { bytes = o->size; samples = (uint32_t)(bytes / (d->channels * 2)); }
        memset((void *)(uintptr_t)o->addr, 0, (size_t)bytes);
        ipc_write_out(x, 0, (void *)(uintptr_t)o->addr, bytes);
    }
    out_u32(x, 0, consumed); out_u32(x, 4, samples);
    if (x->cmd == 4 || x->cmd == 5 || x->cmd == 6 || x->cmd == 7 || x->cmd == 8 || x->cmd == 9 || x->cmd == 10) out_u64(x, 8, 0);
}
static void od_set_ctx(IpcCtx *x) { (void)x; }
CMD_TABLE(g_opusdec_cmds) = {CMD(0, od_decode), CMD(1, od_set_ctx), CMD(2, od_decode), CMD(3, od_set_ctx), CMD(4, od_decode), CMD(5, od_decode),
                             CMD(6, od_decode), CMD(7, od_decode), CMD(8, od_decode), CMD(9, od_decode)};
const SvcIface g_opusdec_iface = {"nn::codec::detail::IHardwareOpusDecoder", g_opusdec_cmds, (int)(sizeof g_opusdec_cmds / sizeof g_opusdec_cmds[0]), 0};

/* ------------------------------------------------------------------ acc:u0 : IAccountServiceForApplication (one local user) */
static const uint8_t k_uid[16] = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static void acc_count(IpcCtx *x)  { out_u32(x, 0, 1); }
static void acc_exists(IpcCtx *x) { uint8_t v = 1; out_bytes(x, 0, &v, 1); }
static void acc_list(IpcCtx *x) {              /* 2 ListAllUsers, 3 ListOpenUsers: array of 16 byte user ids */
    IpcBuf *b = ipc_out_buf(x, 0);
    if (b && b->size >= 16) ipc_write_out(x, 0, k_uid, 16);
}
static void acc_last(IpcCtx *x)   { out_bytes(x, 0, k_uid, 16); }
static void acc_init(IpcCtx *x)   { (void)x; }
static void acc_false(IpcCtx *x)  { uint8_t v = 0; out_bytes(x, 0, &v, 1); }
/* nn::account::detail::INotifier: GetSystemEvent hands back an event that never fires (no account changes on this console) */
static KObj *g_acc_notify_ev;
static void accn_event(IpcCtx *x) {
    if (!g_acc_notify_ev) g_acc_notify_ev = k_event_named("acc_notifier", 0);
    out_copy_handle(x, k_handle_for(g_acc_notify_ev, 1));
}
CMD_TABLE(g_accn_cmds) = {CMD(0, accn_event)};
IFACE(g_accn_iface, "nn::account::detail::INotifier", g_accn_cmds);
static void acc_notifier(IpcCtx *x) { out_obj(x, &g_accn_iface, 0); }
CMD_TABLE(g_acc_cmds) = {CMD(0, acc_count), CMD(1, acc_exists), CMD(2, acc_list), CMD(3, acc_list), CMD(4, acc_last), CMD(51, acc_last), CMD(100, acc_notifier), CMD(101, acc_notifier), CMD(103, acc_notifier), CMD(104, acc_notifier), CMD(106, acc_notifier), CMD(140, acc_init), CMD(141, acc_list), CMD(150, acc_false), CMD(160, acc_init)};
IFACE(g_acc_iface, "nn::account::detail::IAccountServiceForApplication", g_acc_cmds);

static const ServiceDef g_audio_defs[] = {{"audren:u", &g_audren_iface, 0}, {"audout:u", &g_audout_iface, 0}, {"hwopus", &g_hwopus_iface, 0},
                                          {"acc:u0", &g_acc_iface, 0}};

void hle_audio_init(void) {
    InitializeCriticalSection(&g_alock);
    for (size_t i = 0; i < sizeof g_audio_defs / sizeof g_audio_defs[0]; i++) hle_register_service(&g_audio_defs[i]);
}
