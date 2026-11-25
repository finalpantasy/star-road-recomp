/* AArch64 cryptographic extension instructions (AES, SHA-1, SHA-256, PMULL 64x64) for the IR interpreter. Our own code (task-13).
 * The lifter has no decoder for these, so a function that contains one is left to the interpreter, which hands the raw instruction
 * word to a64_crypto_exec. Semantics follow the architecture's pseudocode (Arm ARM, "Cryptographic" instruction pages).
 * Registers are the guest vector file as an array of 16-byte values (little endian, byte 0 = bits 7:0). */
#include <stdint.h>
#include <string.h>

typedef long long v128 __attribute__((vector_size(16), aligned(16)));

static uint8_t g_sbox[256], g_isbox[256];
static volatile int g_tab_ready;

static uint8_t xtime(uint8_t a) { return (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1B : 0)); }
static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) { if (b & 1) r ^= a; a = xtime(a); b >>= 1; }
    return r;
}
static void tab_init(void) {
    if (g_tab_ready) return;
    /* S-box from the multiplicative inverse in GF(2^8) followed by the affine transform */
    uint8_t inv[256] = {0};
    for (int a = 1; a < 256; a++)
        for (int b = 1; b < 256; b++)
            if (gmul((uint8_t)a, (uint8_t)b) == 1) { inv[a] = (uint8_t)b; break; }
    for (int a = 0; a < 256; a++) {
        uint8_t x = inv[a], s = x;
        for (int k = 1; k <= 4; k++) s ^= (uint8_t)((x << k) | (x >> (8 - k)));
        s ^= 0x63;
        g_sbox[a] = s;
        g_isbox[s] = (uint8_t)a;
    }
    g_tab_ready = 1;
}

static void load(uint8_t *d, v128 v) { memcpy(d, &v, 16); }
static v128 store(const uint8_t *d) { v128 v; memcpy(&v, d, 16); return v; }

static const uint8_t k_shift[16] = {0, 5, 10, 15, 4, 9, 14, 3, 8, 13, 2, 7, 12, 1, 6, 11};
static const uint8_t k_ishift[16] = {0, 13, 10, 7, 4, 1, 14, 11, 8, 5, 2, 15, 12, 9, 6, 3};

static v128 aes_round(v128 d, v128 n, int decrypt) {
    uint8_t a[16], b[16], t[16];
    load(a, d); load(b, n);
    for (int i = 0; i < 16; i++) a[i] ^= b[i];
    for (int i = 0; i < 16; i++) t[i] = a[decrypt ? k_ishift[i] : k_shift[i]];
    for (int i = 0; i < 16; i++) t[i] = decrypt ? g_isbox[t[i]] : g_sbox[t[i]];
    return store(t);
}
static v128 aes_mix(v128 n, int inverse) {
    uint8_t a[16], t[16];
    load(a, n);
    for (int c = 0; c < 4; c++) {
        const uint8_t *s = a + 4 * c;
        uint8_t *o = t + 4 * c;
        if (!inverse) {
            o[0] = (uint8_t)(gmul(s[0], 2) ^ gmul(s[1], 3) ^ s[2] ^ s[3]);
            o[1] = (uint8_t)(s[0] ^ gmul(s[1], 2) ^ gmul(s[2], 3) ^ s[3]);
            o[2] = (uint8_t)(s[0] ^ s[1] ^ gmul(s[2], 2) ^ gmul(s[3], 3));
            o[3] = (uint8_t)(gmul(s[0], 3) ^ s[1] ^ s[2] ^ gmul(s[3], 2));
        } else {
            o[0] = (uint8_t)(gmul(s[0], 14) ^ gmul(s[1], 11) ^ gmul(s[2], 13) ^ gmul(s[3], 9));
            o[1] = (uint8_t)(gmul(s[0], 9) ^ gmul(s[1], 14) ^ gmul(s[2], 11) ^ gmul(s[3], 13));
            o[2] = (uint8_t)(gmul(s[0], 13) ^ gmul(s[1], 9) ^ gmul(s[2], 14) ^ gmul(s[3], 11));
            o[3] = (uint8_t)(gmul(s[0], 11) ^ gmul(s[1], 13) ^ gmul(s[2], 9) ^ gmul(s[3], 14));
        }
    }
    return store(t);
}

/* ---- SHA-1 */
static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static uint32_t sha_choose(uint32_t x, uint32_t y, uint32_t z) { return ((y ^ z) & x) ^ z; }
static uint32_t sha_parity(uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; }
static uint32_t sha_major(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | ((x ^ y) & z); }

static v128 sha1_hash(v128 vd, v128 vn, v128 vm, int kind) {
    uint32_t X[4], W[4], Y;
    memcpy(X, &vd, 16); memcpy(W, &vm, 16);
    uint32_t tmp[4]; memcpy(tmp, &vn, 16); Y = tmp[0];
    for (int e = 0; e < 4; e++) {
        uint32_t t = kind == 0 ? sha_choose(X[1], X[2], X[3]) : kind == 1 ? sha_parity(X[1], X[2], X[3]) : sha_major(X[1], X[2], X[3]);
        Y = Y + rol(X[0], 5) + t;
        Y = Y + W[e];
        X[1] = rol(X[1], 30);
        /* <Y, X> = ROL(Y : X, 32): the new Y is the old top word of X, X shifts up one word and takes Y at the bottom */
        uint32_t ny = X[3];
        X[3] = X[2]; X[2] = X[1]; X[1] = X[0]; X[0] = Y;
        Y = ny;
    }
    v128 r; memcpy(&r, X, 16); return r;
}
static v128 sha1_su0(v128 vd, v128 vn, v128 vm) {
    uint64_t d[2], n[2], m[2], r[2];
    memcpy(d, &vd, 16); memcpy(n, &vn, 16); memcpy(m, &vm, 16);
    uint64_t op2lo = d[1], op2hi = n[0];                 /* op2 = op2<63:0> : op1<127:64> */
    r[0] = d[0] ^ op2lo ^ m[0];
    r[1] = d[1] ^ op2hi ^ m[1];
    v128 v; memcpy(&v, r, 16); return v;
}
static v128 sha1_su1(v128 vd, v128 vn) {
    uint32_t d[4], n[4], T[4], r[4];
    memcpy(d, &vd, 16); memcpy(n, &vn, 16);
    T[0] = d[0] ^ n[1]; T[1] = d[1] ^ n[2]; T[2] = d[2] ^ n[3]; T[3] = d[3];   /* op1 EOR (op2 LSR 32) */
    r[0] = rol(T[0], 1); r[1] = rol(T[1], 1); r[2] = rol(T[2], 1);
    r[3] = rol(T[3], 1) ^ rol(T[0], 2);
    v128 v; memcpy(&v, r, 16); return v;
}

/* ---- SHA-256 */
static uint32_t sigma0(uint32_t x) { return ror(x, 2) ^ ror(x, 13) ^ ror(x, 22); }
static uint32_t sigma1(uint32_t x) { return ror(x, 6) ^ ror(x, 11) ^ ror(x, 25); }
static v128 sha256_hash(v128 vx, v128 vy, v128 vw, int part1) {
    uint32_t X[4], Y[4], W[4];
    memcpy(X, &vx, 16); memcpy(Y, &vy, 16); memcpy(W, &vw, 16);
    for (int e = 0; e < 4; e++) {
        uint32_t chs = sha_choose(Y[0], Y[1], Y[2]);
        uint32_t maj = sha_major(X[0], X[1], X[2]);
        uint32_t t = Y[3] + sigma1(Y[0]) + chs + W[e];
        X[3] = t + X[3];
        Y[3] = t + sigma0(X[0]) + maj;
        /* <Y, X> = ROL(Y : X, 32) over 256 bits (Y high) */
        uint32_t ty = Y[3], tx = X[3];
        Y[3] = Y[2]; Y[2] = Y[1]; Y[1] = Y[0]; Y[0] = tx;
        X[3] = X[2]; X[2] = X[1]; X[1] = X[0]; X[0] = ty;
    }
    v128 r; memcpy(&r, part1 ? X : Y, 16); return r;
}
static v128 sha256_su0(v128 vd, v128 vn) {
    uint32_t X[4], Y[4], T[4], r[4];
    memcpy(X, &vd, 16); memcpy(Y, &vn, 16);
    T[0] = X[1]; T[1] = X[2]; T[2] = X[3]; T[3] = Y[0];   /* T = Y<31:0> : X<127:32> */
    for (int e = 0; e < 4; e++) r[e] = X[e] + (ror(T[e], 7) ^ ror(T[e], 18) ^ (T[e] >> 3));
    v128 v; memcpy(&v, r, 16); return v;
}
static v128 sha256_su1(v128 vd, v128 vn, v128 vm) {
    uint32_t X[4], Y[4], Z[4], T0[4], T1[4], r[4];
    memcpy(X, &vd, 16); memcpy(Y, &vn, 16); memcpy(Z, &vm, 16);
    T0[0] = Y[1]; T0[1] = Y[2]; T0[2] = Y[3]; T0[3] = Z[0];   /* T0 = Z<31:0> : Y<127:32> */
    T1[0] = Z[2]; T1[1] = Z[3];
    for (int e = 0; e < 2; e++) r[e] = (ror(T1[e], 17) ^ ror(T1[e], 19) ^ (T1[e] >> 10)) + X[e] + T0[e];
    T1[2] = r[0]; T1[3] = r[1];
    for (int e = 2; e < 4; e++) r[e] = (ror(T1[e], 17) ^ ror(T1[e], 19) ^ (T1[e] >> 10)) + X[e] + T0[e];
    v128 v; memcpy(&v, r, 16); return v;
}

/* ---- PMULL / PMULL2 with 64-bit elements: carry-less multiply, 128-bit product */
static v128 pmull64(uint64_t a, uint64_t b) {
    uint64_t lo = 0, hi = 0;
    for (int i = 0; i < 64; i++)
        if ((b >> i) & 1) {
            lo ^= a << i;
            if (i) hi ^= a >> (64 - i);
        }
    uint64_t r[2] = {lo, hi};
    v128 v; memcpy(&v, r, 16); return v;
}

/* Executes one instruction word if it is a cryptographic one. Returns 1 when handled, 0 when the word is not ours. */
int a64_crypto_exec(v128 *v, uint32_t w) {
    uint32_t rd = w & 31, rn = (w >> 5) & 31, rm = (w >> 16) & 31;
    tab_init();
    if ((w & 0xFFFF0C00u) == 0x4E280800u) {                         /* AESE/AESD/AESMC/AESIMC */
        switch ((w >> 12) & 15) {
        case 4: v[rd] = aes_round(v[rd], v[rn], 0); return 1;
        case 5: v[rd] = aes_round(v[rd], v[rn], 1); return 1;
        case 6: v[rd] = aes_mix(v[rn], 0); return 1;
        case 7: v[rd] = aes_mix(v[rn], 1); return 1;
        default: return 0;
        }
    }
    if ((w & 0xFFFE0C00u) == 0x5E280800u) {                         /* SHA1H, SHA1SU1, SHA256SU0 */
        switch ((w >> 12) & 31) {
        case 0: { uint32_t x; memcpy(&x, &v[rn], 4); uint32_t r[4] = {rol(x, 30), 0, 0, 0}; memcpy(&v[rd], r, 16); return 1; }
        case 1: v[rd] = sha1_su1(v[rd], v[rn]); return 1;
        case 2: v[rd] = sha256_su0(v[rd], v[rn]); return 1;
        default: return 0;
        }
    }
    if ((w & 0xFFE08C00u) == 0x5E000000u) {                         /* SHA1C/P/M, SHA1SU0, SHA256H/H2/SU1 */
        switch ((w >> 12) & 7) {
        case 0: v[rd] = sha1_hash(v[rd], v[rn], v[rm], 0); return 1;
        case 1: v[rd] = sha1_hash(v[rd], v[rn], v[rm], 1); return 1;
        case 2: v[rd] = sha1_hash(v[rd], v[rn], v[rm], 2); return 1;
        case 3: v[rd] = sha1_su0(v[rd], v[rn], v[rm]); return 1;
        case 4: v[rd] = sha256_hash(v[rd], v[rn], v[rm], 1); return 1;
        case 5: v[rd] = sha256_hash(v[rn], v[rd], v[rm], 0); return 1;
        case 6: v[rd] = sha256_su1(v[rd], v[rn], v[rm]); return 1;
        default: return 0;
        }
    }
    if ((w & 0xBFE0FC00u) == 0x0EE0E000u) {                         /* PMULL / PMULL2, 1D x 1D -> 1Q */
        int hi = (w >> 30) & 1;
        uint64_t a[2], b[2];
        memcpy(a, &v[rn], 16); memcpy(b, &v[rm], 16);
        v[rd] = pmull64(a[hi], b[hi]);
        return 1;
    }
    return 0;
}
