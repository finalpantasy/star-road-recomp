/* Tiny PNG writer (stored deflate, no compression) for texture dumps. Our own code. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];
static int crc_ready;
static uint32_t crc32_upd(uint32_t c, const uint8_t* b, size_t n) {
    if (!crc_ready) {
        for (uint32_t i = 0; i < 256; i++) { uint32_t x = i; for (int k = 0; k < 8; k++) x = (x & 1) ? 0xEDB88320u ^ (x >> 1) : x >> 1; crc_table[i] = x; }
        crc_ready = 1;
    }
    c = ~c;
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ b[i]) & 255] ^ (c >> 8);
    return ~c;
}
static void be32w(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void chunk(FILE* f, const char* type, const uint8_t* d, uint32_t n) {
    uint8_t h[8];
    be32w(h, n); memcpy(h + 4, type, 4); fwrite(h, 1, 8, f);
    if (n) fwrite(d, 1, n, f);
    uint32_t c = crc32_upd(0, h + 4, 4);
    if (n) c = crc32_upd(c, d, n);
    uint8_t t[4]; be32w(t, c); fwrite(t, 1, 4, f);
}

int png_write_rgba(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    be32w(ihdr, w); be32w(ihdr + 4, h); ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    size_t rowb = (size_t)w * 4 + 1, raw = rowb * h;
    uint8_t* r = (uint8_t*)malloc(raw);
    for (uint32_t y = 0; y < h; y++) { r[y * rowb] = 0; memcpy(r + y * rowb + 1, rgba + (size_t)y * w * 4, (size_t)w * 4); }
    size_t nblk = (raw + 65534) / 65535, zn = 2 + raw + nblk * 5 + 4;
    uint8_t* z = (uint8_t*)malloc(zn);
    size_t zp = 0;
    z[zp++] = 0x78; z[zp++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t o = 0; o < raw; o += 65535) {
        size_t n = raw - o < 65535 ? raw - o : 65535;
        z[zp++] = (o + n >= raw) ? 1 : 0;
        z[zp++] = (uint8_t)n; z[zp++] = (uint8_t)(n >> 8); z[zp++] = (uint8_t)~n; z[zp++] = (uint8_t)(~n >> 8);
        memcpy(z + zp, r + o, n); zp += n;
    }
    for (size_t i = 0; i < raw; i++) { a = (a + r[i]) % 65521; b = (b + a) % 65521; }
    be32w(z + zp, (b << 16) | a); zp += 4;
    chunk(f, "IDAT", z, (uint32_t)zp);
    chunk(f, "IEND", NULL, 0);
    fclose(f); free(r); free(z);
    return 0;
}
