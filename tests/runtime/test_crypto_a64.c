/* Checks runtime/crypto_a64.c against published vectors (FIPS 197 appendix B round 1, SHA-1 and SHA-256 of "abc"), driving each
 * instruction through its machine word as the interpreter does. Our own code (task-13). Build: see tests/test_crypto_a64.py. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef long long v128 __attribute__((vector_size(16), aligned(16)));
int a64_crypto_exec(v128 *v, uint32_t w);

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL %s\n", msg); fails++; } } while (0)

static void hex(const char *h, uint8_t *o) { for (; h[0]; h += 2) { unsigned x; sscanf(h, "%2x", &x); *o++ = (uint8_t)x; } }
static void run(v128 *v, uint32_t w) { CHECK(a64_crypto_exec(v, w), "instruction not handled"); }
static uint32_t ROR(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74,
    0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d,
    0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e,
    0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static void test_aes(void) {
    v128 v[32];
    uint8_t pt[16], key[16], exp1[16], exp2[16], out[16];
    hex("193de3bea0f4e22b9ac68d2ae9f84808", pt);   /* state after AddRoundKey, with a zero key below */
    memset(key, 0, 16);
    memcpy(&v[0], pt, 16); memcpy(&v[1], key, 16);
    hex("d4bf5d30e0b452aeb84111f11e2798e5", exp1);
    hex("046681e5e0cb199a48f8d37a2806264c", exp2);
    run(v, 0x4e284820u);                           /* aese v0.16b, v1.16b */
    memcpy(out, &v[0], 16);
    CHECK(!memcmp(out, exp1, 16), "aese");
    memcpy(&v[1], &v[0], 16);
    run(v, 0x4e286820u);                           /* aesmc v0.16b, v1.16b */
    memcpy(out, &v[0], 16);
    CHECK(!memcmp(out, exp2, 16), "aesmc");
    memcpy(&v[1], &v[0], 16);
    run(v, 0x4e287820u);                           /* aesimc v0.16b, v1.16b */
    memcpy(out, &v[0], 16);
    CHECK(!memcmp(out, exp1, 16), "aesimc");
    memset(&v[1], 0, 16);
    memcpy(&v[0], exp1, 16);
    run(v, 0x4e285820u);                           /* aesd v0.16b, v1.16b: InvShiftRows + InvSubBytes of the shifted state */
    memcpy(out, &v[0], 16);
    uint8_t sub[16];
    hex("d42711aee0bf98f1b8b45de51e415230", sub);   /* SubBytes output of the same round: the inverse path must recover the input */
    CHECK(!memcmp(out, pt, 16), "aesd");
    (void)sub;
}

static void test_sha256(void) {
    uint32_t W[64], H[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memset(W, 0, sizeof W);
    W[0] = 0x61626380; W[15] = 24;                   /* "abc" padded */
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(W[i - 15], 7) ^ ROR(W[i - 15], 18) ^ (W[i - 15] >> 3), s1 = ROR(W[i - 2], 17) ^ ROR(W[i - 2], 19) ^ (W[i - 2] >> 10);
        W[i] = W[i - 16] + s0 + W[i - 7] + s1;
    }
    /* schedule through the instructions: groups of four words */
    uint32_t S[64];
    memcpy(S, W, 16 * 4);
    v128 v[32];
    for (int g = 4; g < 16; g++) {
        memcpy(&v[0], &S[(g - 4) * 4], 16); memcpy(&v[1], &S[(g - 3) * 4], 16);
        run(v, 0x5e282820u);                         /* sha256su0 v0.4s, v1.4s */
        memcpy(&v[2], &S[(g - 2) * 4], 16); memcpy(&v[3], &S[(g - 1) * 4], 16);
        run(v, 0x5e036040u);                         /* sha256su1 v0.4s, v2.4s, v3.4s */
        memcpy(&S[g * 4], &v[0], 16);
    }
    CHECK(!memcmp(S, W, sizeof W), "sha256su0/su1 schedule");
    /* rounds through sha256h / sha256h2 */
    uint32_t abcd[4] = {H[0], H[1], H[2], H[3]}, efgh[4] = {H[4], H[5], H[6], H[7]};
    for (int g = 0; g < 16; g++) {
        uint32_t wk[4];
        for (int e = 0; e < 4; e++) wk[e] = W[g * 4 + e] + K256[g * 4 + e];
        memcpy(&v[0], abcd, 16); memcpy(&v[1], efgh, 16); memcpy(&v[2], wk, 16);
        v128 save = v[0];
        run(v, 0x5e024020u);                         /* sha256h q0, q1, v2.4s */
        v128 nabcd = v[0];
        memcpy(&v[0], efgh, 16); v[1] = save;
        run(v, 0x5e025020u);                         /* sha256h2 q0, q1, v2.4s : d = EFGH, n = old ABCD */
        memcpy(efgh, &v[0], 16); memcpy(abcd, &nabcd, 16);
    }
    uint32_t out[8];
    for (int i = 0; i < 4; i++) { out[i] = abcd[i] + H[i]; out[i + 4] = efgh[i] + H[i + 4]; }
    static const uint32_t want[8] = {0xba7816bf, 0x8f01cfea, 0x414140de, 0x5dae2223, 0xb00361a3, 0x96177a9c, 0xb410ff61, 0xf20015ad};
    CHECK(!memcmp(out, want, 32), "sha256 abc digest");
}

static uint32_t ROL(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void test_sha1(void) {
    uint32_t W[80], h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    memset(W, 0, sizeof W);
    W[0] = 0x61626380; W[15] = 24;
    for (int i = 16; i < 80; i++) W[i] = ROL(W[i - 3] ^ W[i - 8] ^ W[i - 14] ^ W[i - 16], 1);
    static const uint32_t K[4] = {0x5a827999, 0x6ed9eba1, 0x8f1bbcdc, 0xca62c1d6};
    /* schedule words 16..79 through sha1su0 / sha1su1 */
    uint32_t S[80];
    memcpy(S, W, 16 * 4);
    v128 v[32];
    for (int g = 4; g < 20; g++) {
        memcpy(&v[0], &S[(g - 4) * 4], 16); memcpy(&v[1], &S[(g - 3) * 4], 16); memcpy(&v[2], &S[(g - 2) * 4], 16);
        run(v, 0x5e023020u);                         /* sha1su0 v0.4s, v1.4s, v2.4s */
        memcpy(&v[1], &S[(g - 1) * 4], 16);
        run(v, 0x5e281820u);                         /* sha1su1 v0.4s, v1.4s */
        memcpy(&S[g * 4], &v[0], 16);
    }
    CHECK(!memcmp(S, W, sizeof W), "sha1su0/su1 schedule");
    uint32_t abcd[4] = {h[0], h[1], h[2], h[3]}, e = h[4];
    for (int g = 0; g < 20; g++) {
        uint32_t wk[4];
        for (int j = 0; j < 4; j++) wk[j] = W[g * 4 + j] + K[g / 5];
        uint32_t lo = abcd[0];
        memcpy(&v[3], &lo, 4);
        memset((uint8_t *)&v[3] + 4, 0, 12);
        run(v, 0x5e280863u);                         /* sha1h s3, s3 */
        uint32_t e1; memcpy(&e1, &v[3], 4);
        memcpy(&v[0], abcd, 16);
        uint32_t ee[4] = {e, 0, 0, 0};
        memcpy(&v[1], ee, 16); memcpy(&v[2], wk, 16);
        uint32_t w = g < 5 ? 0x5e020020u : g < 10 ? 0x5e021020u : g < 15 ? 0x5e022020u : 0x5e021020u;   /* c, p, m, p */
        run(v, w);
        memcpy(abcd, &v[0], 16);
        e = e1;
    }
    uint32_t out[5] = {abcd[0] + h[0], abcd[1] + h[1], abcd[2] + h[2], abcd[3] + h[3], e + h[4]};
    static const uint32_t want[5] = {0xa9993e36, 0x4706816a, 0xba3e2571, 0x7850c26c, 0x9cd0d89d};
    CHECK(!memcmp(out, want, 20), "sha1 abc digest");
}

static void test_pmull(void) {
    v128 v[32];
    uint64_t a[2] = {3, 0}, b[2] = {3, 0}, r[2];
    memcpy(&v[1], a, 16); memcpy(&v[2], b, 16);
    run(v, 0x0ee2e020u);                            /* pmull v0.1q, v1.1d, v2.1d : (x+1)^2 = x^2+1 over GF(2) */
    memcpy(r, &v[0], 16);
    CHECK(r[0] == 5 && r[1] == 0, "pmull 3*3");
    uint64_t c[2] = {0x8000000000000000ull, 0};
    memcpy(&v[1], c, 16); memcpy(&v[2], c, 16);
    run(v, 0x0ee2e020u);
    memcpy(r, &v[0], 16);
    CHECK(r[0] == 0 && r[1] == 0x4000000000000000ull, "pmull high");
}

int main(void) {
    test_aes();
    test_sha256();
    test_sha1();
    test_pmull();
    if (fails) printf("TEST crypto_a64 failed %d checks\n", fails);
    else printf("TEST crypto_a64 passed AES, SHA-1, SHA-256, PMULL vectors\n");
    return fails != 0;
}
