/* Star Road Recomp nn::movie HLE (task-21, D-011). Our own code.
 *
 * subsdk0 is Nintendo's movie library (Android stagefright + nvdec). The game (CRIWARE Sofdec, USM with VP9 video) uses a small part of
 * it: it demuxes the USM itself and feeds compressed VP9 frames to a movie::Decoder, then reads NV12 pictures back. All 31 imports that
 * `main` takes from subsdk0 are replaced here (HLE_OVR beats the lifted export), so the lifted decoder, its nvmap / nvhost-nvdec
 * traffic and its stagefright threads are never reached.
 *
 * Objects the game allocates itself (Decoder, Extractor, MediaData, Buffer) hold a vtable at +0 and an 8 byte handle to our host state at
 * +8 (MediaData and Buffer: the handle or the state sits at +0). Guest pointers are host pointers (identity mapping).
 * The vtable layouts below were read from what the game calls (SMRPG_MOVIE_TRACE=1 logs every virtual call with its arguments).
 *
 * Decoding is libavcodec's VP9 decoder from a separate LGPL-only DLL (tools/ffmpeg/build.py), loaded with LoadLibrary; a missing DLL makes
 * Configure fail (the movie never starts) instead of stopping the process. No other FFmpeg library is used: the NV12 packing is ours. */
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>

#if __has_include(<libavcodec/avcodec.h>)
#define MOVIE_HAVE_FFMPEG 1
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/error.h>
#else
#define MOVIE_HAVE_FFMPEG 0
#endif

/* ------------------------------------------------------------------ libavcodec, loaded at run time */
#if MOVIE_HAVE_FFMPEG
static struct {
    HMODULE avcodec, avutil;
    int tried, ok;
    const AVCodec *(*find_decoder)(enum AVCodecID);
    AVCodecContext *(*alloc_context)(const AVCodec *);
    int (*open2)(AVCodecContext *, const AVCodec *, void **);
    void (*free_context)(AVCodecContext **);
    int (*send_packet)(AVCodecContext *, const AVPacket *);
    int (*receive_frame)(AVCodecContext *, AVFrame *);
    void (*flush_buffers)(AVCodecContext *);
    AVPacket *(*packet_alloc)(void);
    void (*packet_free)(AVPacket **);
    AVFrame *(*frame_alloc)(void);
    void (*frame_free)(AVFrame **);
    void (*frame_unref)(AVFrame *);
    void (*frame_move_ref)(AVFrame *, AVFrame *);
    const char *(*license)(void);
    unsigned (*version)(void);
    int (*strerror)(int, char *, size_t);
} g_av;
static CRITICAL_SECTION g_av_cs;
static volatile LONG g_av_cs_state;

static void av_lock(void) {
    if (InterlockedCompareExchange(&g_av_cs_state, 1, 0) == 0) { InitializeCriticalSection(&g_av_cs); g_av_cs_state = 2; }
    while (g_av_cs_state != 2) Sleep(0);
    EnterCriticalSection(&g_av_cs);
}
static void av_unlock(void) { LeaveCriticalSection(&g_av_cs); }

static HMODULE load_one(const char *dir, const char *prefix) {
    char pat[MAX_PATH + 64];
    snprintf(pat, sizeof pat, "%s\\%s-*.dll", dir, prefix);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    char best[MAX_PATH] = "";
    do { if (strcmp(fd.cFileName, best) > 0) snprintf(best, sizeof best, "%s", fd.cFileName); } while (FindNextFileA(h, &fd));
    FindClose(h);
    char full[MAX_PATH * 2];
    snprintf(full, sizeof full, "%s\\%s", dir, best);
    return LoadLibraryExA(full, 0, LOAD_WITH_ALTERED_SEARCH_PATH);
}

/* directories searched: SMRPG_FFMPEG_DIR (the install root or its bin), the exe directory, then the install the build script makes */
static int av_load(void) {
    av_lock();
    if (!g_av.tried) {
        g_av.tried = 1;
        char exe[MAX_PATH], dirs[3][MAX_PATH * 2] = {"", "", "A:\\AI\\Recomps\\ffmpeg\\install\\bin"};
        const char *env = getenv("SMRPG_FFMPEG_DIR");
        if (env) { snprintf(dirs[0], sizeof dirs[0], "%s\\bin", env); }
        if (GetModuleFileNameA(0, exe, sizeof exe)) { char *s = strrchr(exe, '\\'); if (s) { *s = 0; snprintf(dirs[1], sizeof dirs[1], "%s", exe); } }
        for (int i = 0; i < 3 && !g_av.avcodec; i++) {
            if (!dirs[i][0]) continue;
            SetDllDirectoryA(dirs[i]);          /* avcodec's import of avutil is found beside it */
            g_av.avutil = load_one(dirs[i], "avutil");
            if (g_av.avutil) g_av.avcodec = load_one(dirs[i], "avcodec");
            if (!g_av.avcodec && g_av.avutil) { FreeLibrary(g_av.avutil); g_av.avutil = 0; }
            if (g_av.avcodec) rt_log("movie: libavcodec loaded from %s", dirs[i]);
        }
        SetDllDirectoryA(NULL);
        if (g_av.avcodec) {
#define LOADF(m, f, name) (*(FARPROC *)&g_av.f = GetProcAddress(g_av.m, name))
            LOADF(avcodec, find_decoder, "avcodec_find_decoder"); LOADF(avcodec, alloc_context, "avcodec_alloc_context3");
            LOADF(avcodec, open2, "avcodec_open2"); LOADF(avcodec, free_context, "avcodec_free_context");
            LOADF(avcodec, send_packet, "avcodec_send_packet"); LOADF(avcodec, receive_frame, "avcodec_receive_frame");
            LOADF(avcodec, flush_buffers, "avcodec_flush_buffers"); LOADF(avcodec, packet_alloc, "av_packet_alloc");
            LOADF(avcodec, packet_free, "av_packet_free"); LOADF(avcodec, license, "avcodec_license");
            LOADF(avcodec, version, "avcodec_version");
            LOADF(avutil, frame_alloc, "av_frame_alloc"); LOADF(avutil, frame_free, "av_frame_free");
            LOADF(avutil, frame_unref, "av_frame_unref"); LOADF(avutil, frame_move_ref, "av_frame_move_ref"); LOADF(avutil, strerror, "av_strerror");
            g_av.ok = g_av.find_decoder && g_av.alloc_context && g_av.open2 && g_av.free_context && g_av.send_packet && g_av.receive_frame &&
                      g_av.flush_buffers && g_av.packet_alloc && g_av.packet_free && g_av.frame_alloc && g_av.frame_free && g_av.frame_unref && g_av.frame_move_ref;
            if (g_av.ok) {
                unsigned v = g_av.version ? g_av.version() : 0;
                rt_log("movie: libavcodec %u.%u.%u license: %s", v >> 16, (v >> 8) & 255, v & 255, g_av.license ? g_av.license() : "?");
                /* the headers this file was compiled with must match the DLL's ABI (AVCodecContext layout changes on a major bump) */
                if ((v >> 16) != LIBAVCODEC_VERSION_MAJOR) { rt_log("movie: libavcodec major %u does not match the headers (%d): decoder disabled", v >> 16, LIBAVCODEC_VERSION_MAJOR); g_av.ok = 0; }
            }
        } else {
            rt_log("movie: no avcodec-*.dll found (build it with tools/ffmpeg/build.py); movies will not decode");
        }
    }
    int ok = g_av.ok;
    av_unlock();
    return ok;
}
#endif /* MOVIE_HAVE_FFMPEG */

/* ------------------------------------------------------------------ small helpers */
static int g_mtrace = -1;
static int movie_trace(void) { if (g_mtrace < 0) g_mtrace = getenv("SMRPG_MOVIE_TRACE") != NULL; return g_mtrace; }
#define MTRACE(...) do { if (movie_trace()) rt_log("movie: " __VA_ARGS__); } while (0)

#define A(c, i) ((c)->x[i])
#define PTR(T, v) ((T *)(uintptr_t)(v))

static uint64_t g_sym_signal, g_sym_clear;
static void guest_event(Ctx *c, uint64_t ev, int clear) {
    if (!ev) return;
    uint64_t fn = clear ? g_sym_clear : g_sym_signal;
    if (!fn) {
        fn = ld_find_export(clear ? "_ZN2nn2os10ClearEventEPNS0_9EventTypeE" : "_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
        if (clear) g_sym_clear = fn; else g_sym_signal = fn;
        if (!fn) { rt_log("movie: nn::os::%sEvent not found in the sdk", clear ? "Clear" : "Signal"); return; }
    }
    uint64_t save[9];
    memcpy(save, c->x, sizeof save);
    c->x[0] = ev;
    rt_call_guest(c, fn);
    memcpy(c->x, save, sizeof save);
}

/* vtable of HLE entries, 64 slots; slots we do not implement log once per slot and return success */
typedef struct { const char *name; HleFn fn; } VSlot;
static uint64_t make_vtable(const char *cls, const VSlot *s, int n, HleFn unimpl) {
    uint64_t *t = VirtualAlloc(NULL, 64 * 8, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    char nm[96];
    for (int i = 0; i < 64; i++) {
        if (i < n && s[i].fn) { snprintf(nm, sizeof nm, "movie::%s::%s", cls, s[i].name); t[i] = hle_slot(nm, SLOT_HLE, s[i].fn, 0)->addr; }
        else { snprintf(nm, sizeof nm, "movie::%s::<slot %d unimplemented>", cls, i); t[i] = hle_slot(nm, SLOT_HLE, unimpl, 0)->addr; }
    }
    return (uint64_t)(uintptr_t)t;
}
static void unimpl_slot(Ctx *c) {
    HleSlot *s = hle_slot_at(c->pc);
    rt_log("movie: UNIMPLEMENTED virtual %s (x0=%#llx x1=%#llx x2=%#llx lr=%#llx)", s ? s->name : "?", (unsigned long long)A(c, 0),
           (unsigned long long)A(c, 1), (unsigned long long)A(c, 2), (unsigned long long)A(c, 30));
    A(c, 0) = 0;
}

/* ------------------------------------------------------------------ MediaData: string keyed table behind an 8 byte handle */
typedef enum { MD_INT, MD_STR } MdKind;
typedef struct { char key[40]; MdKind kind; int64_t i; char *s; } MdEntry;
typedef struct { MdEntry e[40]; int n; int codec; int w, h, fps_milli; } MediaData;

static MediaData *md_of(uint64_t obj) {
    MediaData **p = PTR(MediaData *, obj);
    if (!*p) *p = calloc(1, sizeof(MediaData));
    return *p;
}
static void md_clear(MediaData *m) {
    for (int i = 0; i < m->n; i++) free(m->e[i].s);
    memset(m, 0, sizeof *m);
}
static MdEntry *md_slot(MediaData *m, const char *k) {
    for (int i = 0; i < m->n; i++) if (!strcmp(m->e[i].key, k)) return &m->e[i];
    if (m->n >= (int)(sizeof m->e / sizeof m->e[0])) return 0;
    MdEntry *e = &m->e[m->n++];
    memset(e, 0, sizeof *e);
    snprintf(e->key, sizeof e->key, "%s", k);
    return e;
}
static void md_set_int(MediaData *m, const char *k, int64_t v) { MdEntry *e = md_slot(m, k); if (e) { free(e->s); e->s = 0; e->kind = MD_INT; e->i = v; } }
static void md_set_str(MediaData *m, const char *k, const char *v) { MdEntry *e = md_slot(m, k); if (e) { free(e->s); e->kind = MD_STR; e->s = _strdup(v); } }
static void md_set_rect(MediaData *m, const char *k, int l, int t, int r, int b) {
    char n[64];
    const char *sfx[4] = {".left", ".top", ".right", ".bottom"};
    int v[4] = {l, t, r, b};
    for (int i = 0; i < 4; i++) { snprintf(n, sizeof n, "%s%s", k, sfx[i]); md_set_int(m, n, v[i]); }
}
static void md_copy(MediaData *d, const MediaData *s) {
    md_clear(d);
    memcpy(d, s, sizeof *d);
    for (int i = 0; i < d->n; i++) if (s->e[i].s) d->e[i].s = _strdup(s->e[i].s);
}
static int md_find_int(const MediaData *m, const char *k, int64_t *v) {
    for (int i = 0; i < m->n; i++) if (m->e[i].kind == MD_INT && !strcmp(m->e[i].key, k)) { *v = m->e[i].i; return 1; }
    return 0;
}
static void trace_key(const char *what, const char *k, int found) {
    static volatile LONG seen;
    if (movie_trace() || InterlockedIncrement(&seen) <= 24) rt_log("movie: %s(\"%s\") -> %s", what, k, found ? "found" : "absent");
}

HLE_OVR(md_ctor, "_ZN5movie9MediaDataC1Ev") { *PTR(uint64_t, A(c, 0)) = (uint64_t)(uintptr_t)calloc(1, sizeof(MediaData)); }
HLE_OVR(md_copyctor, "_ZN5movie9MediaDataC1ERKS0_") {
    MediaData *d = calloc(1, sizeof *d);
    md_copy(d, md_of(A(c, 1)));
    *PTR(uint64_t, A(c, 0)) = (uint64_t)(uintptr_t)d;
}
HLE_OVR(md_dtor, "_ZN5movie9MediaDataD1Ev") {
    MediaData *m = *PTR(MediaData *, A(c, 0));
    if (m) { md_clear(m); free(m); }
    *PTR(uint64_t, A(c, 0)) = 0;
}
HLE_OVR(md_clear_fn, "_ZN5movie9MediaData5ClearEv") { md_clear(md_of(A(c, 0))); }
HLE_OVR(md_assign, "_ZN5movie9MediaDataaSERKS0_") {
    MediaData *d = md_of(A(c, 0)), *s = md_of(A(c, 1));
    if (d != s) md_copy(d, s);                          /* returns *this: x0 stays */
}
HLE_OVR(md_find_i32, "_ZNK5movie9MediaData9FindInt32EPKcPi") {
    int64_t v;
    int ok = md_find_int(md_of(A(c, 0)), PTR(const char, A(c, 1)), &v);
    trace_key("FindInt32", PTR(const char, A(c, 1)), ok);
    if (ok) *PTR(int32_t, A(c, 2)) = (int32_t)v;
    A(c, 0) = (uint64_t)ok;
}
HLE_OVR(md_find_i64, "_ZNK5movie9MediaData9FindInt64EPKcPl") {
    int64_t v;
    int ok = md_find_int(md_of(A(c, 0)), PTR(const char, A(c, 1)), &v);
    trace_key("FindInt64", PTR(const char, A(c, 1)), ok);
    if (ok) *PTR(int64_t, A(c, 2)) = v;
    A(c, 0) = (uint64_t)ok;
}
HLE_OVR(md_find_str, "_ZNK5movie9MediaData10FindStringEPKcPS2_") {
    MediaData *m = md_of(A(c, 0));
    const char *k = PTR(const char, A(c, 1));
    for (int i = 0; i < m->n; i++)
        if (m->e[i].kind == MD_STR && !strcmp(m->e[i].key, k)) { *PTR(const char *, A(c, 2)) = m->e[i].s; trace_key("FindString", k, 1); A(c, 0) = 1; return; }
    trace_key("FindString", k, 0);
    A(c, 0) = 0;
}
HLE_OVR(md_find_rect, "_ZNK5movie9MediaData8FindRectEPKcPiS3_S3_S3_") {
    MediaData *m = md_of(A(c, 0));
    const char *k = PTR(const char, A(c, 1));
    const char *sfx[4] = {".left", ".top", ".right", ".bottom"};
    int64_t v[4];
    char n[64];
    for (int i = 0; i < 4; i++) {
        snprintf(n, sizeof n, "%s%s", k, sfx[i]);
        if (!md_find_int(m, n, &v[i])) { trace_key("FindRect", k, 0); A(c, 0) = 0; return; }
    }
    trace_key("FindRect", k, 1);
    for (int i = 0; i < 4; i++) if (A(c, 2 + i)) *PTR(int32_t, A(c, 2 + i)) = (int32_t)v[i];
    A(c, 0) = 1;
}

/* ------------------------------------------------------------------ Buffer (the game keeps it on its stack: base, capacity, offset, size) */
typedef struct { uint64_t base, capacity; int32_t offset, size; } MBuf;
_Static_assert(sizeof(MBuf) <= 0x20, "movie::Buffer host layout");
#define BUF(v) PTR(MBuf, v)
HLE_OVR(buf_ctor, "_ZN5movie6BufferC1Ev") { memset(BUF(A(c, 0)), 0, sizeof(MBuf)); }
HLE_OVR(buf_dtor, "_ZN5movie6BufferD1Ev") { (void)c; }
HLE_OVR(buf_set_data, "_ZN5movie6Buffer18SetDataAndCapacityEPvm") { MBuf *b = BUF(A(c, 0)); b->base = A(c, 1); b->capacity = A(c, 2); b->offset = 0; b->size = (int32_t)A(c, 2); }
HLE_OVR(buf_set_range, "_ZN5movie6Buffer8SetRangeEii") { MBuf *b = BUF(A(c, 0)); b->offset = (int32_t)A(c, 1); b->size = (int32_t)A(c, 2); }
HLE_OVR(buf_base, "_ZNK5movie6Buffer4BaseEv") { A(c, 0) = BUF(A(c, 0))->base; }
HLE_OVR(buf_capacity, "_ZNK5movie6Buffer8CapacityEv") { A(c, 0) = BUF(A(c, 0))->capacity; }
HLE_OVR(buf_size, "_ZNK5movie6Buffer4SizeEv") { A(c, 0) = (uint32_t)BUF(A(c, 0))->size; }
HLE_OVR(buf_offset, "_ZNK5movie6Buffer6OffsetEv") { A(c, 0) = (uint32_t)BUF(A(c, 0))->offset; }
HLE_OVR(buf_data, "_ZNK5movie6Buffer4DataEv") { MBuf *b = BUF(A(c, 0)); A(c, 0) = b->base + (uint64_t)(int64_t)b->offset; }

/* ------------------------------------------------------------------ library-wide settings and configuration data */
HLE_OVR(mv_set_allocator, "_ZN5movie12SetAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_") { (void)c; }
HLE_OVR(mv_set_core_mask, "_ZN5movie11SetCoreMaskEm") { (void)c; }
HLE_OVR(mv_set_thread_prio, "_ZN5movie17SetThreadPriorityEi") { (void)c; }
HLE_OVR(mv_get_thread_prio, "_ZN5movie17GetThreadPriorityEv") { A(c, 0) = 16; }

/* CreateVp9DecoderConfigurationData(width, height, frame rate x 1000 (third argument, seen in the log), MediaData *out) -> bool */
HLE_OVR(mv_vp9_config, "_ZN5movie33CreateVp9DecoderConfigurationDataEiiiPNS_9MediaDataE") {
    int w = (int)A(c, 0), h = (int)A(c, 1), extra = (int)A(c, 2);
    MediaData *m = md_of(A(c, 3));
    md_clear(m);
    m->codec = 9;                                       /* VP9 */
    m->w = w; m->h = h; m->fps_milli = extra;
    md_set_str(m, "mime", "video/x-vnd.on2.vp9");
    md_set_int(m, "width", w);
    md_set_int(m, "height", h);
    rt_log("movie: CreateVp9DecoderConfigurationData %dx%d, third argument %d", w, h, extra);
    A(c, 0) = 1;
}
HLE_OVR(mv_h264_config, "_ZN5movie34CreateH264DecoderConfigurationDataEPKhmS1_mPNS_9MediaDataE") {
    rt_log("movie: CreateH264DecoderConfigurationData called (the movies are VP9): reporting failure");
    A(c, 0) = 0;
}
HLE_OVR(mv_h264_nal, "_ZN5movie37GetH264NalUnitStartPositionAndNalSizeEPPhPmS1_S2_") {
    rt_log("movie: GetH264NalUnitStartPositionAndNalSize called (the movies are VP9): reporting failure");
    A(c, 0) = 0;
}

/* ------------------------------------------------------------------ Extractor: the game demuxes the USM itself, so no container is ever opened here */
static uint64_t g_ex_vt;
static void ex_dtor(Ctx *c) { MTRACE("Extractor dtor"); A(c, 0) = 0; }
static void ex_track_count(Ctx *c) { rt_log("movie: Extractor::GetTrackCount: 0 tracks"); if (A(c, 1)) *PTR(int32_t, A(c, 1)) = 0; A(c, 0) = 0; }
static void ex_set_source(Ctx *c) { rt_log("movie: Extractor::SetDataSource(\"%s\") refused (no container support)", PTR(const char, A(c, 1))); A(c, 0) = 1; }
HLE_OVR(ex_ctor, "_ZN5movie9ExtractorC1ENS_13ContainerTypeE") {
    if (!g_ex_vt) {
        VSlot s[16] = {{"dtor", ex_dtor}, {"dtor0", ex_dtor}, {"slot2", 0}, {"slot3", 0}, {"GetTrackCount", ex_track_count}};
        s[15] = (VSlot){"SetDataSource", ex_set_source};
        g_ex_vt = make_vtable("Extractor", s, 16, unimpl_slot);
    }
    PTR(uint64_t, A(c, 0))[0] = g_ex_vt;
    rt_log("movie: Extractor constructed (container type %d)", (int)A(c, 1));
}
HLE_OVR(ex_dtor_fn, "_ZN5movie9ExtractorD1Ev") { (void)c; }
HLE_OVR(ex_track_count_fn, "_ZN5movie9Extractor13GetTrackCountEPi") { if (A(c, 1)) *PTR(int32_t, A(c, 1)) = 0; A(c, 0) = 0; }
HLE_OVR(ex_set_source_fn, "_ZN5movie9Extractor13SetDataSourceEPKc") { rt_log("movie: Extractor::SetDataSource(\"%s\") refused", PTR(const char, A(c, 1))); A(c, 0) = 1; }
HLE_OVR(ex_track_cfg_fn, "_ZN5movie9Extractor21GetTrackConfigurationEiPNS_9MediaDataE") { A(c, 0) = 1; }

/* ------------------------------------------------------------------ Decoder */
#define NIN 4
#define NOUT 8
#define IN_CAP (2u << 20)
#define EOS_FLAG 4u                                     /* flags of SendInputBufferForDecode and AcquireOutputBufferIndex: end of stream */
#define RES_NOT_AVAIL_IN  0x1011u
#define RES_NOT_AVAIL_OUT 0x100Eu

#if MOVIE_HAVE_FFMPEG
/* runtime/nvn/nvn_api_mem.cpp: the pool memory behind a live NVNtexture, and a way to make NVN upload it again */
typedef struct { uint8_t *data; uint32_t width, height, format, flags; uint64_t stride, size; } MovieTexTarget;
int nvn_movie_texture_target(const void *tex, MovieTexTarget *out);
void nvn_movie_texture_dirty(const void *tex);
#endif

typedef struct { int idx; int64_t time; uint32_t flags; } Ready;
typedef struct MovieDec {
    CRITICAL_SECTION lock;
    int type, fmt, mode, arg;                           /* constructor arguments */
    int configured, started, stopped;
    int w, h, stride, slice;                            /* NV12: Y plane stride x slice rows, then interleaved UV at stride x slice */
    int cs, full;                                       /* color space of the first picture: cs 0 BT.601, 1 BT.709 */
    uint64_t ev[4];                                     /* input available, output available, format changed, error */
    uint8_t *in[NIN], *out[NOUT];
    uint32_t out_cap, out_size[NOUT];
    int in_busy[NIN], out_busy[NOUT];
    Ready ready[NOUT + 2];
    int nready;
    int eos_sent, eos_queued;
    /* decode statistics (AC3) */
    uint64_t pk_in, fr_out, errors, bytes_in;
    int64_t last_time;
#if MOVIE_HAVE_FFMPEG
    AVCodecContext *ctx;
    AVPacket *pkt;
    AVFrame *frame;
    AVFrame *pic[NOUT];                                 /* the decoded picture of each output slot until the game takes it */
#endif
    uint64_t tex_writes, tex_fail;
    int serial;                                         /* 1-based order of the decoder that first wrote a texture (diagnostics) */
} MovieDec;

static uint64_t g_dec_vt;
#define SELF(c) PTR(MovieDec, PTR(uint64_t, A(c, 0))[1])

static uint8_t *pool_alloc(size_t n) { return VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); }

static void dec_signal(Ctx *c, MovieDec *d, int i, int clear) {
    uint64_t ev;
    EnterCriticalSection(&d->lock);
    ev = d->stopped ? 0 : d->ev[i];
    LeaveCriticalSection(&d->lock);
    guest_event(c, ev, clear);
}

static void dec_report(MovieDec *d, const char *when) {
    rt_log("movie: decoder %s: %llu packets in (%llu bytes), %llu pictures out, %llu decode errors, %llu textures written, %llu texture failures, %dx%d", when, (unsigned long long)d->pk_in,
           (unsigned long long)d->bytes_in, (unsigned long long)d->fr_out, (unsigned long long)d->errors, (unsigned long long)d->tex_writes, (unsigned long long)d->tex_fail, d->w, d->h);
}

/* slot 2 Configure(MediaData *) */
static void dec_configure(Ctx *c) {
    MovieDec *d = SELF(c);
    MediaData *m = md_of(A(c, 1));
    MTRACE("Decoder::Configure codec %d %dx%d", m->codec, m->w, m->h);
#if MOVIE_HAVE_FFMPEG
    if (m->codec != 9) { rt_log("movie: Configure with codec %d (only VP9 is supported)", m->codec); A(c, 0) = 1; return; }
    if (!av_load()) { rt_log("movie: Configure failed: libavcodec is not available"); A(c, 0) = 1; return; }
    const AVCodec *codec = g_av.find_decoder(AV_CODEC_ID_VP9);
    if (!codec) { rt_log("movie: the FFmpeg DLL has no VP9 decoder"); A(c, 0) = 1; return; }
    d->ctx = g_av.alloc_context(codec);
    d->ctx->width = m->w; d->ctx->height = m->h;
    d->ctx->thread_count = 4;
    if (g_av.open2(d->ctx, codec, NULL) < 0) { rt_log("movie: cannot open the VP9 decoder"); g_av.free_context(&d->ctx); A(c, 0) = 1; return; }
    d->pkt = g_av.packet_alloc();
    d->frame = g_av.frame_alloc();
    d->w = m->w; d->h = m->h;
    d->cs = d->h >= 720; d->full = 0;
    d->stride = d->w;                                   /* tightly packed NV12 */
    d->slice = d->h;
    d->out_cap = (uint32_t)(d->stride * d->slice * 3 / 2);
    for (int i = 0; i < NIN; i++) if (!d->in[i]) d->in[i] = pool_alloc(IN_CAP + 64);
    for (int i = 0; i < NOUT; i++) if (!d->out[i]) d->out[i] = pool_alloc(d->out_cap);
    d->configured = 1;
    rt_log("movie: decoder configured: VP9 %dx%d NV12 stride %d slice %d (type %d fmt %d mode %d)", d->w, d->h, d->stride, d->slice, d->type, d->fmt, d->mode);
    A(c, 0) = 0;
#else
    rt_log("movie: built without the FFmpeg headers: movies cannot decode");
    (void)d; (void)m;
    A(c, 0) = 1;
#endif
}

/* slot 3 RegisterClientEvents(EventType *events[4]) */
static void dec_register_events(Ctx *c) {
    MovieDec *d = SELF(c);
    memcpy(d->ev, PTR(void, A(c, 1)), sizeof d->ev);
    MTRACE("RegisterClientEvents %#llx %#llx %#llx %#llx", (unsigned long long)d->ev[0], (unsigned long long)d->ev[1], (unsigned long long)d->ev[2], (unsigned long long)d->ev[3]);
    A(c, 0) = 0;
}

/* the format of one decoded picture, with the keys the SDK's DecoderImpl sets (strings read from subsdk0) */
static void dec_format(MovieDec *d, MediaData *m) {
    md_clear(m);
    md_set_str(m, "mime", "video/raw");
    md_set_int(m, "width", d->w);
    md_set_int(m, "height", d->h);
    md_set_rect(m, "crop", 0, 0, d->w - 1, d->h - 1);
    md_set_int(m, "stride", d->stride);
    md_set_int(m, "slice-height", d->slice);
    md_set_int(m, "nv12-width", d->w);
    md_set_int(m, "nv12-height", d->h);
    md_set_int(m, "nv12-y-offset", 0);
    md_set_int(m, "nv12-y-stride", d->stride);
    md_set_int(m, "nv12-uv-offset", (int64_t)d->stride * d->slice);
    md_set_int(m, "nv12-uv-stride", d->stride);
    md_set_int(m, "nv12-colorspace", (d->cs ? 3 : 1) + (d->full ? 1 : 0));   /* 1 BT.601 limited, 2 BT.601 full, 3 BT.709 limited, 4 BT.709 full */
}
/* slot 4 GetOutputFormat(MediaData *) */
static void dec_get_output_format(Ctx *c) { MTRACE("GetOutputFormat"); dec_format(SELF(c), md_of(A(c, 1))); A(c, 0) = 0; }
/* slot 18 GetOutputBufferFormat(int idx, MediaData *) */
static void dec_get_output_buffer_format(Ctx *c) { MTRACE("GetOutputBufferFormat idx %d", (int)A(c, 1)); dec_format(SELF(c), md_of(A(c, 2))); A(c, 0) = 0; }

/* slot 5 Start: all input buffers are free */
static void dec_start(Ctx *c) {
    MovieDec *d = SELF(c);
    EnterCriticalSection(&d->lock);
    d->started = 1; d->stopped = 0;
    LeaveCriticalSection(&d->lock);
    MTRACE("Decoder::Start");
    dec_signal(c, d, 0, 0);
    A(c, 0) = 0;
}

/* slot 6 Stop: no event is delivered after it returns; events still set are cleared (the player deletes the decoder right after) */
static void dec_stop(Ctx *c) {
    MovieDec *d = SELF(c);
    uint64_t ev[4];
    EnterCriticalSection(&d->lock);
    int was = !d->stopped;
    d->stopped = 1;
    memcpy(ev, d->ev, sizeof ev);
    LeaveCriticalSection(&d->lock);
    if (was) {
        dec_report(d, "stopped");
        for (int i = 0; i < 4; i++) guest_event(c, ev[i], 1);
    }
    A(c, 0) = 0;
}

/* slot 9 AcquireInputBufferIndex(int *index, int *freeAfter) */
static void dec_acquire_input(Ctx *c) {
    MovieDec *d = SELF(c);
    int idx = -1, left = 0;
    EnterCriticalSection(&d->lock);
    int out_free = 0;
    for (int i = 0; i < NOUT; i++) if (!d->out_busy[i]) out_free++;
    int pending = d->nready;
    if (out_free > pending + 1) {                       /* back pressure: keep a spare output slot for the pictures one input can release */
        for (int i = 0; i < NIN; i++) if (!d->in_busy[i]) { if (idx < 0) idx = i; else left++; }
        if (idx >= 0) d->in_busy[idx] = 1;
    }
    LeaveCriticalSection(&d->lock);
    if (idx < 0) { A(c, 0) = RES_NOT_AVAIL_IN; return; }
    *PTR(int32_t, A(c, 1)) = idx;
    if (A(c, 2)) *PTR(int32_t, A(c, 2)) = left;
    A(c, 0) = 0;
}

/* slot 10 GetInputBuffer(int index, Buffer *) */
static void dec_get_input_buffer(Ctx *c) {
    MovieDec *d = SELF(c);
    int i = (int)A(c, 1);
    MBuf *b = BUF(A(c, 2));
    if (i < 0 || i >= NIN || !d->in[i]) { A(c, 0) = 1; return; }
    b->base = (uint64_t)(uintptr_t)d->in[i]; b->capacity = IN_CAP; b->offset = 0; b->size = 0;
    A(c, 0) = 0;
}

/* Queue one decoded picture in a free output slot (the frame moves into the slot); the caller holds the lock. The pixels are produced when the
 * game asks for the output (GetOutputBuffer): RGBA into its texture in texture mode, NV12 into its buffer in CPU mode. */
#if MOVIE_HAVE_FFMPEG
static int emit_picture(MovieDec *d, AVFrame *f, int64_t time) {
    int slot = -1;
    for (int i = 0; i < NOUT; i++) if (!d->out_busy[i]) { slot = i; break; }
    if (slot < 0) return 0;
    if (!d->fr_out) {
        d->cs = f->colorspace == AVCOL_SPC_BT709 || (f->colorspace == AVCOL_SPC_UNSPECIFIED && f->height >= 720);
        d->full = f->color_range == AVCOL_RANGE_JPEG;
        rt_log("movie: first picture %dx%d format %d colorspace %d range %d pts %lld", f->width, f->height, f->format, (int)f->colorspace, (int)f->color_range, (long long)time);
    }
    if (!d->pic[slot]) d->pic[slot] = g_av.frame_alloc();
    g_av.frame_unref(d->pic[slot]);
    g_av.frame_move_ref(d->pic[slot], f);
    d->out_busy[slot] = 1;
    d->out_size[slot] = (uint32_t)((size_t)d->stride * d->slice * 3 / 2);
    d->ready[d->nready++] = (Ready){slot, time, 0};
    d->fr_out++;
    d->last_time = time;
    return 1;
}

/* NV12 for CPU mode: Y plane at stride, interleaved UV after slice rows */
static void pack_nv12(const MovieDec *d, const AVFrame *f, uint8_t *o) {
    int w = d->w < f->width ? d->w : f->width, h = d->h < f->height ? d->h : f->height;
    size_t ysz = (size_t)d->stride * d->slice;
    if (f->format != AV_PIX_FMT_YUV420P) { memset(o, 16, ysz); memset(o + ysz, 128, ysz / 2); return; }
    if (w < d->w || h < d->h) { memset(o, 16, ysz); memset(o + ysz, 128, ysz / 2); }
    for (int y = 0; y < h; y++) memcpy(o + (size_t)y * d->stride, f->data[0] + (size_t)y * f->linesize[0], (size_t)w);
    uint8_t *uv = o + ysz;
    for (int y = 0; y < h / 2; y++) {
        const uint8_t *u = f->data[1] + (size_t)y * f->linesize[1], *v = f->data[2] + (size_t)y * f->linesize[2];
        uint8_t *r = uv + (size_t)y * d->stride;
        for (int x = 0; x < w / 2; x++) { r[2 * x] = u[x]; r[2 * x + 1] = v[x]; }
    }
}

/* YUV 4:2:0 to 8-bit RGBA or BGRA rows (limited or full range; BT.601 or BT.709). Chroma is nearest-neighbor per 2x2 block; the per-value
 * terms come from 256-entry tables so the pixel loop is adds and clamps. */
static inline uint8_t clamp8(int32_t v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
static void yuv_to_rgba(const AVFrame *f, int w, int h, int cs709, int full, int bgra, uint8_t *dst, size_t pitch) {
    double kr = cs709 ? 1.5748 : 1.402, kb = cs709 ? 1.8556 : 1.772, gu = cs709 ? 0.1873 : 0.34414, gv = cs709 ? 0.4681 : 0.71414;
    double ys = full ? 1.0 : 255.0 / 219.0, cm = full ? 1.0 : 255.0 / 224.0;
    int32_t ty[256], trv[256], tbu[256], tgu[256], tgv[256];
    for (int i = 0; i < 256; i++) {
        ty[i] = (int32_t)(((full ? i : i - 16) * ys) * 65536.0 + 32768.0);
        trv[i] = (int32_t)((i - 128) * cm * kr * 65536.0);
        tbu[i] = (int32_t)((i - 128) * cm * kb * 65536.0);
        tgu[i] = (int32_t)((i - 128) * cm * gu * 65536.0);
        tgv[i] = (int32_t)((i - 128) * cm * gv * 65536.0);
    }
    int ri = bgra ? 2 : 0, bi = bgra ? 0 : 2;
    for (int y = 0; y < h; y++) {
        const uint8_t *py = f->data[0] + (size_t)y * f->linesize[0];
        const uint8_t *pu = f->data[1] + (size_t)(y >> 1) * f->linesize[1], *pv = f->data[2] + (size_t)(y >> 1) * f->linesize[2];
        uint8_t *o = dst + (size_t)y * pitch;
        for (int x = 0; x < w; x += 2) {
            int32_t u = pu[x >> 1], v = pv[x >> 1];
            int32_t dr = trv[v], db = tbu[u], dg = -(tgu[u] + tgv[v]);
            for (int k = 0; k < 2 && x + k < w; k++) {
                int32_t Y = ty[py[x + k]];
                uint8_t *q = o + 4 * (x + k);
                q[ri] = clamp8((Y + dr) >> 16);
                q[1] = clamp8((Y + dg) >> 16);
                q[bi] = clamp8((Y + db) >> 16);
                q[3] = 255;
            }
        }
    }
}
#endif

/* slot 11 SendInputBufferForDecode(int index, int offset, int size, int64 timeUs, uint32 flags) */
static void dec_send_input(Ctx *c) {
    MovieDec *d = SELF(c);
    int idx = (int)A(c, 1), off = (int)A(c, 2), size = (int)A(c, 3);
    int64_t time = (int64_t)A(c, 4);
    uint32_t flags = (uint32_t)A(c, 5);
    MTRACE("SendInputBufferForDecode idx %d off %d size %d time %lld flags %#x", idx, off, size, (long long)time, flags);
    int produced = 0;
    EnterCriticalSection(&d->lock);
#if MOVIE_HAVE_FFMPEG
    if (d->ctx) {
        if (size > 0 && idx >= 0 && idx < NIN && off >= 0 && (uint32_t)(off + size) <= IN_CAP) {
            d->pkt->data = d->in[idx] + off;
            d->pkt->size = size;
            d->pkt->pts = time;
            d->pk_in++; d->bytes_in += (uint64_t)size;
            int r = g_av.send_packet(d->ctx, d->pkt);
            if (r < 0 && r != AVERROR(EAGAIN)) {
                char eb[80] = "";
                if (g_av.strerror) g_av.strerror(r, eb, sizeof eb);
                if (++d->errors <= 8) rt_log("movie: avcodec_send_packet failed (%d: %s) on packet %llu", r, eb, (unsigned long long)d->pk_in);
            }
        }
        if ((flags & EOS_FLAG) && !d->eos_sent) { g_av.send_packet(d->ctx, NULL); d->eos_sent = 1; }
        /* every picture the decoder has ready; the frame threads of libavcodec delay them by a few packets */
        for (;;) {
            if (d->nready >= NOUT) break;
            int r = g_av.receive_frame(d->ctx, d->frame);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
            if (r < 0) { d->errors++; rt_log("movie: avcodec_receive_frame failed (%d)", r); break; }
            int64_t t = d->frame->pts == AV_NOPTS_VALUE ? time : d->frame->pts;
            if (!emit_picture(d, d->frame, t)) { g_av.frame_unref(d->frame); break; }
            g_av.frame_unref(d->frame);
            produced = 1;
        }
        if (d->eos_sent && !d->eos_queued && d->nready < NOUT + 1) {
            /* decoder drained: the end of the stream is its own output entry (an empty buffer flagged EOS) */
            int r = g_av.receive_frame(d->ctx, d->frame);
            if (r == AVERROR_EOF) {
                int slot = -1;
                for (int i = 0; i < NOUT; i++) if (!d->out_busy[i]) { slot = i; break; }
                if (slot >= 0) { d->out_busy[slot] = 1; d->out_size[slot] = 0; d->ready[d->nready++] = (Ready){slot, time, EOS_FLAG}; d->eos_queued = 1; produced = 1; dec_report(d, "end of stream"); }
            } else if (r >= 0) { g_av.frame_unref(d->frame); }
        }
    }
#endif
    if (idx >= 0 && idx < NIN) d->in_busy[idx] = 0;
    LeaveCriticalSection(&d->lock);
    dec_signal(c, d, 0, 0);
    if (produced) dec_signal(c, d, 1, 0);
    A(c, 0) = 0;
}

/* slot 12 AcquireOutputBufferIndex(int *index, int64 *timeUs, uint32 *flags, int *remaining) */
static void dec_acquire_output(Ctx *c) {
    MovieDec *d = SELF(c);
    EnterCriticalSection(&d->lock);
    if (!d->nready) { LeaveCriticalSection(&d->lock); A(c, 0) = RES_NOT_AVAIL_OUT; return; }
    Ready r = d->ready[0];
    memmove(d->ready, d->ready + 1, (size_t)(--d->nready) * sizeof d->ready[0]);
    int left = d->nready;
    LeaveCriticalSection(&d->lock);
    *PTR(int32_t, A(c, 1)) = r.idx;
    if (A(c, 2)) *PTR(int64_t, A(c, 2)) = r.time;
    if (A(c, 3)) *PTR(uint32_t, A(c, 3)) = r.flags;
    if (A(c, 4)) *PTR(int32_t, A(c, 4)) = left;
    A(c, 0) = 0;
}

/* slot 13 GetOutputBuffer(int index, Buffer *). Texture mode: the client's buffer is an 8-byte slot holding an NVNtexture; the picture is converted to
 * RGBA into that texture's memory. CPU mode: the picture is NV12 in the client's own buffer, or in ours. */
static void dec_get_output_buffer(Ctx *c) {
    MovieDec *d = SELF(c);
    int i = (int)A(c, 1);
    MBuf *b = BUF(A(c, 2));
    if (i < 0 || i >= NOUT || !d->out[i]) { A(c, 0) = 1; return; }
    uint32_t n = d->out_size[i];
    int own = 0;
    for (int k = 0; k < NOUT; k++) own |= b->base == (uint64_t)(uintptr_t)d->out[k];
    MTRACE("GetOutputBuffer idx %d client buffer base %#llx cap %llu (%s), picture %u bytes", i, (unsigned long long)b->base, (unsigned long long)b->capacity, own ? "ours" : "client", n);
#if MOVIE_HAVE_FFMPEG
    if (b->base && b->capacity == 8 && !own) {
        const void *tex = PTR(const void, b->base);                 /* SetDataAndCapacity(texture, 8): the pointer is the NVNtexture itself */
        MovieTexTarget tg;
        if (!n || !d->pic[i] || !d->pic[i]->data[0]) { b->offset = 0; b->size = 8; A(c, 0) = 0; return; }
        if (!tex || !nvn_movie_texture_target(tex, &tg)) {
            if (d->tex_fail++ < 4) rt_log("movie: the output buffer %p is not a live NVN texture", tex);
            b->offset = 0; b->size = 0; A(c, 0) = 0x100B; return;
        }
        const AVFrame *f = d->pic[i];
        int w = (int)tg.width < f->width ? (int)tg.width : f->width, h = (int)tg.height < f->height ? (int)tg.height : f->height;
        int bgra = tg.format == 0x9A;
        if (!(tg.flags & 0x52) || tg.stride < (uint64_t)w * 4 || tg.size < tg.stride * (uint64_t)(h - 1) + (uint64_t)w * 4 || f->format != AV_PIX_FMT_YUV420P) {
            if (d->tex_fail++ < 4) rt_log("movie: texture %ux%u format %#x flags %#x stride %llu size %llu (picture format %d) cannot take the picture", tg.width, tg.height, tg.format, tg.flags,
                                          (unsigned long long)tg.stride, (unsigned long long)tg.size, f->format);
            b->offset = 0; b->size = 0; A(c, 0) = 0x100B; return;
        }
        if (!d->tex_writes) rt_log("movie: texture mode: %ux%u format %#x flags %#x stride %llu -> %s", tg.width, tg.height, tg.format, tg.flags, (unsigned long long)tg.stride, bgra ? "BGRA" : "RGBA");
        yuv_to_rgba(f, w, h, d->cs, d->full, bgra, tg.data, (size_t)tg.stride);
        if (!d->serial) { static int n; d->serial = ++n; }
        {   /* diagnostics (task-21 banding check). SMRPG_MOVIE_DUMP=<dir>: the decoder's planes and our RGBA for pictures 120 and 300 of each decoder.
             * SMRPG_MOVIE_DUMP_RANGE=<a>-<b> and SMRPG_MOVIE_DUMP_DEC=<n>: only our RGBA (tightly packed rows) of pictures a..b-1 of the n-th decoder. */
            static const char *ra; static int ra0, ra1, radec, rinit;
            if (!rinit) { ra = getenv("SMRPG_MOVIE_DUMP_RANGE"); if (ra) sscanf(ra, "%d-%d", &ra0, &ra1); const char *e = getenv("SMRPG_MOVIE_DUMP_DEC"); radec = e ? atoi(e) : 1; rinit = 1; }
            const char *rd = getenv("SMRPG_MOVIE_DUMP");
            if (rd && ra && d->serial == radec && d->tex_writes >= (uint64_t)ra0 && d->tex_writes < (uint64_t)ra1) {
                char fn[512];
                snprintf(fn, sizeof fn, "%s/rgba_dec%d_f%llu.bin", rd, d->serial, (unsigned long long)d->tex_writes);
                FILE *fp = fopen(fn, "wb");
                if (fp) { for (int y = 0; y < h; y++) fwrite(tg.data + (size_t)y * tg.stride, 1, (size_t)w * 4, fp); fclose(fp); }
            }
        }
        {
            static const char *dir; static int init;
            if (!init) { dir = getenv("SMRPG_MOVIE_DUMP"); init = 1; }
            if (dir && (d->tex_writes == 120 || d->tex_writes == 300)) {
                char fn[512];
                snprintf(fn, sizeof fn, "%s/pic%llu_%p.bin", dir, (unsigned long long)d->tex_writes, (void *)d);
                FILE *fp = fopen(fn, "wb");
                if (fp) {
                    int hdr[8] = {f->width, f->height, f->linesize[0], f->linesize[1], d->cs, d->full, (int)tg.stride, w};
                    fwrite(hdr, sizeof hdr, 1, fp);
                    for (int y = 0; y < f->height; y++) fwrite(f->data[0] + (size_t)y * f->linesize[0], 1, (size_t)f->width, fp);
                    for (int y = 0; y < f->height / 2; y++) fwrite(f->data[1] + (size_t)y * f->linesize[1], 1, (size_t)f->width / 2, fp);
                    for (int y = 0; y < f->height / 2; y++) fwrite(f->data[2] + (size_t)y * f->linesize[2], 1, (size_t)f->width / 2, fp);
                    for (int y = 0; y < h; y++) fwrite(tg.data + (size_t)y * tg.stride, 1, (size_t)w * 4, fp);
                    fclose(fp);
                    rt_log("movie: dumped picture %llu to %s", (unsigned long long)d->tex_writes, fn);
                }
            }
        }
        nvn_movie_texture_dirty(tex);
        d->tex_writes++;
        b->offset = 0; b->size = 8;
        A(c, 0) = 0;
        return;
    }
    if (n && d->pic[i] && d->pic[i]->data[0]) pack_nv12(d, d->pic[i], d->out[i]);
#endif
    if (b->base && b->capacity && !own) {
        if (b->capacity < n) { rt_log("movie: the client buffer holds %llu bytes, the picture takes %u", (unsigned long long)b->capacity, n); b->offset = 0; b->size = 0; A(c, 0) = 0x100B; return; }
        if (n) memcpy(PTR(void, b->base), d->out[i], n);
        b->offset = 0; b->size = (int32_t)n;
    } else {
        b->base = (uint64_t)(uintptr_t)d->out[i]; b->capacity = d->out_cap; b->offset = 0; b->size = (int32_t)n;
    }
    A(c, 0) = 0;
}
/* slot 17 GetOutputBuffer(int index, Buffer *, MediaData *): the picture and its format together */
static void dec_get_output_buffer_fmt(Ctx *c) {
    MovieDec *d = SELF(c);
    uint64_t md = A(c, 3);
    if (md) dec_format(d, md_of(md));
    dec_get_output_buffer(c);
}

/* slot 15 ReleaseOutputBufferIndex(int index): the slot is free again; input may be accepted */
static void dec_release_output(Ctx *c) {
    MovieDec *d = SELF(c);
    int i = (int)A(c, 1);
    EnterCriticalSection(&d->lock);
    if (i >= 0 && i < NOUT) d->out_busy[i] = 0;
    LeaveCriticalSection(&d->lock);
    dec_signal(c, d, 0, 0);
    A(c, 0) = 0;
}

/* slot 14 GetOutputBufferSize(int *size, int *mode) */
static void dec_get_sizes(Ctx *c) {
    MovieDec *d = SELF(c);
    MTRACE("slot 14 (sizes) -> %u bytes, mode %d", d->out_cap, d->mode);
    if (A(c, 1)) *PTR(int32_t, A(c, 1)) = (int32_t)d->out_cap;
    if (A(c, 2)) *PTR(int32_t, A(c, 2)) = d->mode;
    A(c, 0) = 0;
}

static void dec_ok(Ctx *c) { A(c, 0) = 0; }

static void dec_free_state(MovieDec *d) {
#if MOVIE_HAVE_FFMPEG
    if (d->ctx) { g_av.free_context(&d->ctx); }
    if (d->pkt) { d->pkt->data = 0; d->pkt->size = 0; g_av.packet_free(&d->pkt); }
    if (d->frame) g_av.frame_free(&d->frame);
    for (int i = 0; i < NOUT; i++) if (d->pic[i]) g_av.frame_free(&d->pic[i]);
#endif
    for (int i = 0; i < NIN; i++) if (d->in[i]) { VirtualFree(d->in[i], 0, MEM_RELEASE); d->in[i] = 0; }
    for (int i = 0; i < NOUT; i++) if (d->out[i]) { VirtualFree(d->out[i], 0, MEM_RELEASE); d->out[i] = 0; }
}
/* slots 0 and 1: the game owns the object's memory; only our state goes */
static void dec_dtor(Ctx *c) {
    uint64_t *self = PTR(uint64_t, A(c, 0));
    MovieDec *d = PTR(MovieDec, self[1]);
    if (!d) { A(c, 0) = 0; return; }
    dec_stop(c);
    dec_report(d, "destroyed");
    EnterCriticalSection(&d->lock);
    dec_free_state(d);
    LeaveCriticalSection(&d->lock);
    self[1] = 0;
    A(c, 0) = 0;
}

HLE_OVR(dec_ctor, "_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi") {
    if (!g_dec_vt) {
        VSlot s[19] = {{"~Decoder", dec_dtor}, {"~Decoder(deleting)", dec_dtor}, {"Configure", dec_configure}, {"RegisterClientEvents", dec_register_events},
                       {"GetOutputFormat", dec_get_output_format}, {"Start", dec_start}, {"Stop", dec_stop}, {"slot7", dec_ok}, {"slot8", dec_ok},
                       {"AcquireInputBufferIndex", dec_acquire_input}, {"GetInputBuffer", dec_get_input_buffer}, {"SendInputBufferForDecode", dec_send_input},
                       {"AcquireOutputBufferIndex", dec_acquire_output}, {"GetOutputBuffer", dec_get_output_buffer}, {"GetOutputBufferSize", dec_get_sizes},
                       {"ReleaseOutputBufferIndex", dec_release_output}, {"slot16", dec_ok}, {"GetOutputBufferWithFormat", dec_get_output_buffer_fmt},
                       {"GetOutputBufferFormat", dec_get_output_buffer_format}};
        g_dec_vt = make_vtable("Decoder", s, 19, unimpl_slot);
    }
    MovieDec *d = calloc(1, sizeof *d);
    InitializeCriticalSection(&d->lock);
    d->type = (int)A(c, 1); d->fmt = (int)A(c, 2); d->mode = (int)A(c, 3); d->arg = (int)A(c, 4);
    uint64_t *self = PTR(uint64_t, A(c, 0));
    self[0] = g_dec_vt;
    self[1] = (uint64_t)(uintptr_t)d;
    rt_log("movie: Decoder constructed: type %d, output format %d, mode %d, arg %d", d->type, d->fmt, d->mode, d->arg);
}
