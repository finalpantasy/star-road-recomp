/* A64 crypto helpers for generated code. AES and SHA semantics adapted from
 * runtime/crypto_a64.c. Immutable FIPS 197 tables require no lazy initialization.
 * As in the rest of the lifter, vector lane order is little endian. */
#include <stdint.h>
#include <string.h>

static const uint8_t ir_crypto_g_sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

static const uint8_t ir_crypto_g_isbox[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d,
};

static inline uint8_t ir_crypto_xtime(uint8_t a) { return (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1B : 0)); }
static inline uint8_t ir_crypto_gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) { if (b & 1) r ^= a; a = ir_crypto_xtime(a); b >>= 1; }
    return r;
}
static inline void ir_crypto_load(uint8_t *d, v128 v) { memcpy(d, &v, 16); }
static inline v128 ir_crypto_store(const uint8_t *d) { v128 v; memcpy(&v, d, 16); return v; }

static const uint8_t ir_crypto_k_shift[16] = {0, 5, 10, 15, 4, 9, 14, 3, 8, 13, 2, 7, 12, 1, 6, 11};
static const uint8_t ir_crypto_k_ishift[16] = {0, 13, 10, 7, 4, 1, 14, 11, 8, 5, 2, 15, 12, 9, 6, 3};

static inline v128 ir_crypto_aes_round(v128 d, v128 n, int decrypt) {
    uint8_t a[16], b[16], t[16];
    ir_crypto_load(a, d); ir_crypto_load(b, n);
    for (int i = 0; i < 16; i++) a[i] ^= b[i];
    for (int i = 0; i < 16; i++) t[i] = a[decrypt ? ir_crypto_k_ishift[i] : ir_crypto_k_shift[i]];
    for (int i = 0; i < 16; i++) t[i] = decrypt ? ir_crypto_g_isbox[t[i]] : ir_crypto_g_sbox[t[i]];
    return ir_crypto_store(t);
}
static inline v128 ir_crypto_aes_mix(v128 n, int inverse) {
    uint8_t a[16], t[16];
    ir_crypto_load(a, n);
    for (int c = 0; c < 4; c++) {
        const uint8_t *s = a + 4 * c;
        uint8_t *o = t + 4 * c;
        if (!inverse) {
            o[0] = (uint8_t)(ir_crypto_gmul(s[0], 2) ^ ir_crypto_gmul(s[1], 3) ^ s[2] ^ s[3]);
            o[1] = (uint8_t)(s[0] ^ ir_crypto_gmul(s[1], 2) ^ ir_crypto_gmul(s[2], 3) ^ s[3]);
            o[2] = (uint8_t)(s[0] ^ s[1] ^ ir_crypto_gmul(s[2], 2) ^ ir_crypto_gmul(s[3], 3));
            o[3] = (uint8_t)(ir_crypto_gmul(s[0], 3) ^ s[1] ^ s[2] ^ ir_crypto_gmul(s[3], 2));
        } else {
            o[0] = (uint8_t)(ir_crypto_gmul(s[0], 14) ^ ir_crypto_gmul(s[1], 11) ^ ir_crypto_gmul(s[2], 13) ^ ir_crypto_gmul(s[3], 9));
            o[1] = (uint8_t)(ir_crypto_gmul(s[0], 9) ^ ir_crypto_gmul(s[1], 14) ^ ir_crypto_gmul(s[2], 11) ^ ir_crypto_gmul(s[3], 13));
            o[2] = (uint8_t)(ir_crypto_gmul(s[0], 13) ^ ir_crypto_gmul(s[1], 9) ^ ir_crypto_gmul(s[2], 14) ^ ir_crypto_gmul(s[3], 11));
            o[3] = (uint8_t)(ir_crypto_gmul(s[0], 11) ^ ir_crypto_gmul(s[1], 13) ^ ir_crypto_gmul(s[2], 9) ^ ir_crypto_gmul(s[3], 14));
        }
    }
    return ir_crypto_store(t);
}

/* ---- SHA-1 */
static inline uint32_t ir_crypto_rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static inline uint32_t ir_crypto_ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static inline uint32_t ir_crypto_sha_choose(uint32_t x, uint32_t y, uint32_t z) { return ((y ^ z) & x) ^ z; }
static inline uint32_t ir_crypto_sha_parity(uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; }
static inline uint32_t ir_crypto_sha_major(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | ((x ^ y) & z); }

static inline v128 ir_crypto_sha1_hash(v128 vd, v128 vn, v128 vm, int kind) {
    uint32_t X[4], W[4], Y;
    memcpy(X, &vd, 16); memcpy(W, &vm, 16);
    uint32_t tmp[4]; memcpy(tmp, &vn, 16); Y = tmp[0];
    for (int e = 0; e < 4; e++) {
        uint32_t t = kind == 0 ? ir_crypto_sha_choose(X[1], X[2], X[3]) : kind == 1 ? ir_crypto_sha_parity(X[1], X[2], X[3]) : ir_crypto_sha_major(X[1], X[2], X[3]);
        Y = Y + ir_crypto_rol(X[0], 5) + t;
        Y = Y + W[e];
        X[1] = ir_crypto_rol(X[1], 30);
        /* <Y, X> = ROL(Y : X, 32): the new Y is the old top word of X, X shifts up one word and takes Y at the bottom */
        uint32_t ny = X[3];
        X[3] = X[2]; X[2] = X[1]; X[1] = X[0]; X[0] = Y;
        Y = ny;
    }
    v128 r; memcpy(&r, X, 16); return r;
}
static inline v128 ir_crypto_sha1_su0(v128 vd, v128 vn, v128 vm) {
    uint64_t d[2], n[2], m[2], r[2];
    memcpy(d, &vd, 16); memcpy(n, &vn, 16); memcpy(m, &vm, 16);
    uint64_t op2lo = d[1], op2hi = n[0];                 /* op2 = op2<63:0> : op1<127:64> */
    r[0] = d[0] ^ op2lo ^ m[0];
    r[1] = d[1] ^ op2hi ^ m[1];
    v128 v; memcpy(&v, r, 16); return v;
}
static inline v128 ir_crypto_sha1_su1(v128 vd, v128 vn) {
    uint32_t d[4], n[4], T[4], r[4];
    memcpy(d, &vd, 16); memcpy(n, &vn, 16);
    T[0] = d[0] ^ n[1]; T[1] = d[1] ^ n[2]; T[2] = d[2] ^ n[3]; T[3] = d[3];   /* op1 EOR (op2 LSR 32) */
    r[0] = ir_crypto_rol(T[0], 1); r[1] = ir_crypto_rol(T[1], 1); r[2] = ir_crypto_rol(T[2], 1);
    r[3] = ir_crypto_rol(T[3], 1) ^ ir_crypto_rol(T[0], 2);
    v128 v; memcpy(&v, r, 16); return v;
}

/* ---- SHA-256 */
static inline uint32_t ir_crypto_sigma0(uint32_t x) { return ir_crypto_ror(x, 2) ^ ir_crypto_ror(x, 13) ^ ir_crypto_ror(x, 22); }
static inline uint32_t ir_crypto_sigma1(uint32_t x) { return ir_crypto_ror(x, 6) ^ ir_crypto_ror(x, 11) ^ ir_crypto_ror(x, 25); }
static inline v128 ir_crypto_sha256_hash(v128 vx, v128 vy, v128 vw, int part1) {
    uint32_t X[4], Y[4], W[4];
    memcpy(X, &vx, 16); memcpy(Y, &vy, 16); memcpy(W, &vw, 16);
    for (int e = 0; e < 4; e++) {
        uint32_t chs = ir_crypto_sha_choose(Y[0], Y[1], Y[2]);
        uint32_t maj = ir_crypto_sha_major(X[0], X[1], X[2]);
        uint32_t t = Y[3] + ir_crypto_sigma1(Y[0]) + chs + W[e];
        X[3] = t + X[3];
        Y[3] = t + ir_crypto_sigma0(X[0]) + maj;
        /* <Y, X> = ROL(Y : X, 32) over 256 bits (Y high) */
        uint32_t ty = Y[3], tx = X[3];
        Y[3] = Y[2]; Y[2] = Y[1]; Y[1] = Y[0]; Y[0] = tx;
        X[3] = X[2]; X[2] = X[1]; X[1] = X[0]; X[0] = ty;
    }
    v128 r; memcpy(&r, part1 ? X : Y, 16); return r;
}
static inline v128 ir_crypto_sha256_su0(v128 vd, v128 vn) {
    uint32_t X[4], Y[4], T[4], r[4];
    memcpy(X, &vd, 16); memcpy(Y, &vn, 16);
    T[0] = X[1]; T[1] = X[2]; T[2] = X[3]; T[3] = Y[0];   /* T = Y<31:0> : X<127:32> */
    for (int e = 0; e < 4; e++) r[e] = X[e] + (ir_crypto_ror(T[e], 7) ^ ir_crypto_ror(T[e], 18) ^ (T[e] >> 3));
    v128 v; memcpy(&v, r, 16); return v;
}
static inline v128 ir_crypto_sha256_su1(v128 vd, v128 vn, v128 vm) {
    uint32_t X[4], Y[4], Z[4], T0[4], T1[4], r[4];
    memcpy(X, &vd, 16); memcpy(Y, &vn, 16); memcpy(Z, &vm, 16);
    T0[0] = Y[1]; T0[1] = Y[2]; T0[2] = Y[3]; T0[3] = Z[0];   /* T0 = Z<31:0> : Y<127:32> */
    T1[0] = Z[2]; T1[1] = Z[3];
    for (int e = 0; e < 2; e++) r[e] = (ir_crypto_ror(T1[e], 17) ^ ir_crypto_ror(T1[e], 19) ^ (T1[e] >> 10)) + X[e] + T0[e];
    T1[2] = r[0]; T1[3] = r[1];
    for (int e = 2; e < 4; e++) r[e] = (ir_crypto_ror(T1[e], 17) ^ ir_crypto_ror(T1[e], 19) ^ (T1[e] >> 10)) + X[e] + T0[e];
    v128 v; memcpy(&v, r, 16); return v;
}

/* ---- PMULL / PMULL2 with 64-bit elements: carry-less multiply, 128-bit product */
static inline v128 ir_crypto_pmull64(uint64_t a, uint64_t b) {
    uint64_t lo = 0, hi = 0;
    for (int i = 0; i < 64; i++)
        if ((b >> i) & 1) {
            lo ^= a << i;
            if (i) hi ^= a >> (64 - i);
        }
    uint64_t r[2] = {lo, hi};
    v128 v; memcpy(&v, r, 16); return v;
}


static inline v128 ir_crypto_sha1_h(v128 n) {
    uint32_t x;
    memcpy(&x, &n, sizeof x);
    uint32_t lanes[4] = {ir_crypto_rol(x, 30), 0, 0, 0};
    v128 result;
    memcpy(&result, lanes, sizeof result);
    return result;
}

static inline v128 ir_crypto_pmull(v128 n, v128 m, int high, int wide) {
    uint8_t a[16], b[16];
    ir_crypto_load(a, n);
    ir_crypto_load(b, m);
    unsigned offset = high ? 8 : 0;
    if (wide) {
        uint64_t x, y;
        memcpy(&x, a + offset, sizeof x);
        memcpy(&y, b + offset, sizeof y);
        return ir_crypto_pmull64(x, y);
    }

    uint16_t lanes[8] = {0};
    for (unsigned i = 0; i < 8; i++)
        for (unsigned j = 0; j < 8; j++)
            if ((b[offset + i] >> j) & 1)
                lanes[i] ^= (uint16_t)a[offset + i] << j;
    v128 result;
    memcpy(&result, lanes, sizeof result);
    return result;
}

static inline uint32_t ir_crypto_crc32(uint32_t crc, uint64_t data, unsigned size, int castagnoli) {
    uint32_t polynomial = castagnoli ? UINT32_C(0x82f63b78) : UINT32_C(0xedb88320);
    for (unsigned i = 0; i < size; i++) {
        crc ^= (uint8_t)data;
        data >>= 8;
        for (unsigned j = 0; j < 8; j++)
            crc = (crc >> 1) ^ ((crc & 1) ? polynomial : 0);
    }
    return crc;
}
