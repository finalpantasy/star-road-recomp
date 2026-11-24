/* Minimal UnityFS reader for the offline texture checker. Our own code. */
#include "tex_unityfs.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define SEEK64(f, o) _fseeki64((f), (long long)(o), SEEK_SET)
#define SEEKEND64(f) _fseeki64((f), 0, SEEK_END)
#define TELL64(f) ((uint64_t)_ftelli64(f))
#else
#define SEEK64(f, o) fseeko((f), (off_t)(o), SEEK_SET)
#define SEEKEND64(f) fseeko((f), 0, SEEK_END)
#define TELL64(f) ((uint64_t)ftello(f))
#endif

int lz4_decompress(const uint8_t* s, size_t sn, uint8_t* d, size_t dn) {
    size_t si = 0, di = 0;
    while (si < sn) {
        unsigned tok = s[si++];
        size_t lit = tok >> 4;
        if (lit == 15) { unsigned b; do { if (si >= sn) return -1; b = s[si++]; lit += b; } while (b == 255); }
        if (si + lit > sn || di + lit > dn) return -1;
        memcpy(d + di, s + si, lit); si += lit; di += lit;
        if (si >= sn) break;
        if (si + 2 > sn) return -1;
        size_t off = s[si] | (size_t)s[si + 1] << 8; si += 2;
        if (off == 0 || off > di) return -1;
        size_t ml = tok & 15;
        if (ml == 15) { unsigned b; do { if (si >= sn) return -1; b = s[si++]; ml += b; } while (b == 255); }
        ml += 4;
        if (di + ml > dn) return -1;
        for (size_t i = 0; i < ml; i++) d[di + i] = d[di - off + i];
        di += ml;
    }
    return (int)di;
}

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t* p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

static int dec_blob(const uint8_t* in, size_t cn, uint8_t* out, size_t un, int comp) {
    if (comp == 0) { if (cn != un) return -1; memcpy(out, in, un); return 0; }
    if (comp == 2 || comp == 3) { int r = lz4_decompress(in, cn, out, un); return (r >= 0 && (size_t)r == un) ? 0 : -1; }
    return -1;
}

int ufs_open(ufs* u, const char* path) {
    memset(u, 0, sizeof *u);
    u->cache_idx = -1;
    u->f = fopen(path, "rb");
    if (!u->f) return -1;
    uint8_t h[0x200];
    size_t got = fread(h, 1, sizeof h, u->f);
    if (got < 64 || memcmp(h, "UnityFS", 8) != 0) { fclose(u->f); u->f = NULL; return -2; }
    size_t p = 8;
    uint32_t version = be32(h + p); p += 4;
    p += strlen((char*)h + p) + 1;   /* player */
    p += strlen((char*)h + p) + 1;   /* engine */
    p += 8;                          /* size */
    uint32_t csz = be32(h + p); p += 4;
    uint32_t usz = be32(h + p); p += 4;
    uint32_t flags = be32(h + p); p += 4;
    uint64_t hdr_end = p;
    if (version >= 7) hdr_end = (hdr_end + 15) / 16 * 16;
    int comp = (int)(flags & 0x3f);
    uint8_t* cbi = (uint8_t*)malloc(csz);
    uint8_t* bi = (uint8_t*)malloc(usz);
    if (!cbi || !bi) return -3;
    if (flags & 0x80) {
        SEEKEND64(u->f);
        uint64_t end = TELL64(u->f);
        SEEK64(u->f, end - csz);
        u->data_start = hdr_end;
    } else {
        SEEK64(u->f, hdr_end);
        u->data_start = hdr_end + csz;
        if (flags & 0x200) u->data_start = (u->data_start + 15) / 16 * 16;
    }
    if (fread(cbi, 1, csz, u->f) != csz || dec_blob(cbi, csz, bi, usz, comp)) { free(cbi); free(bi); return -4; }
    free(cbi);
    size_t q = 16;
    u->nblocks = be32(bi + q); q += 4;
    u->blocks = (ufs_block*)calloc(u->nblocks, sizeof(ufs_block));
    uint64_t uo = 0, co = u->data_start;
    for (uint32_t i = 0; i < u->nblocks; i++) {
        u->blocks[i].usize = be32(bi + q); u->blocks[i].csize = be32(bi + q + 4);
        u->blocks[i].flags = (uint16_t)(bi[q + 8] << 8 | bi[q + 9]); q += 10;
        u->blocks[i].uoff = uo; u->blocks[i].coff = co; uo += u->blocks[i].usize; co += u->blocks[i].csize;
    }
    u->nnodes = be32(bi + q); q += 4;
    u->nodes = (ufs_node*)calloc(u->nnodes, sizeof(ufs_node));
    for (uint32_t i = 0; i < u->nnodes; i++) {
        u->nodes[i].off = be64(bi + q); u->nodes[i].size = be64(bi + q + 8); u->nodes[i].flags = be32(bi + q + 16); q += 20;
        size_t l = strlen((char*)bi + q); if (l > 159) l = 159;
        memcpy(u->nodes[i].name, bi + q, l); u->nodes[i].name[l] = 0; q += strlen((char*)bi + q) + 1;
    }
    free(bi);
    return 0;
}

static int load_block(ufs* u, uint32_t i) {
    if (u->cache_idx == (int)i) return 0;
    ufs_block* b = &u->blocks[i];
    if (u->cache_cap < b->usize) { free(u->cache); u->cache = (uint8_t*)malloc(b->usize); u->cache_cap = b->usize; }
    if (u->cbuf_cap < b->csize) { free(u->cbuf); u->cbuf = (uint8_t*)malloc(b->csize); u->cbuf_cap = b->csize; }
    SEEK64(u->f, b->coff);
    if (fread(u->cbuf, 1, b->csize, u->f) != b->csize) return -1;
    if (dec_blob(u->cbuf, b->csize, u->cache, b->usize, b->flags & 0x3f)) return -2;
    u->cache_idx = (int)i;
    return 0;
}

int ufs_read(ufs* u, uint64_t off, uint64_t size, uint8_t* dst) {
    uint64_t pos = off, end = off + size;
    for (uint32_t i = 0; i < u->nblocks && pos < end; i++) {
        uint64_t bs = u->blocks[i].uoff, be = bs + u->blocks[i].usize;
        if (be <= pos) continue;
        if (bs >= end) break;
        int r = load_block(u, i);
        if (r) return r;
        uint64_t a = pos > bs ? pos : bs, b = end < be ? end : be;
        memcpy(dst + (a - off), u->cache + (a - bs), (size_t)(b - a));
        pos = b;
    }
    return pos == end ? 0 : -3;
}

void ufs_close(ufs* u) {
    if (u->f) fclose(u->f);
    free(u->blocks); free(u->nodes); free(u->cache); free(u->cbuf);
    memset(u, 0, sizeof *u);
}
