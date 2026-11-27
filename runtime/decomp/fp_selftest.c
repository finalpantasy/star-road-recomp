/* Startup selftest of the Rust float layer (decomp/smrpg_decomp/src/fp.rs, task-53) against the lifted C helpers of
 * runtime/ir_simd_a.h, the ones every lifted float instruction calls (fast paths included). Runs with the other
 * selftests (SMRPG_DECOMP=ab SMRPG_DECOMP_SELFTEST=1): edge and random operands, every FPCR mode the soft path
 * implements (flush to zero, default NaN, the four rounding modes and their mixes), a random FPSR going in, and the
 * result and the FPSR coming out must be identical. Not part of a normal run. */
#include "rt_internal.h"
#include "ir_ops.h"
#include "swap.h"

extern uint64_t smrpg_fp_selftest_op(uint32_t op, uint32_t ft, uint32_t fpcr, uint32_t *fpsr, uint64_t a, uint64_t b,
                                     uint64_t c, uint32_t extra);

static uint64_t next_word(uint64_t *state) {
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}

/* Operand sets: zeros, denormals (smallest, largest), normal edges, the rounding boundaries around 1, large and
 * small exponents, infinities, quiet and signaling NaNs with payloads. */
static const uint32_t edges32[] = {
    0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x007fffff, 0x807fffff, 0x00800000, 0x80800000, 0x00800001,
    0x3f7fffff, 0x3f800000, 0xbf800000, 0x3f800001, 0x3f800002, 0x3fffffff, 0x40000000, 0x40400000, 0x33800000,
    0x33800001, 0x34000000, 0x4b000000, 0x4b7fffff, 0x4b800000, 0x4effffff, 0x4f000000, 0xcf000000, 0x5f000000,
    0x7e800000, 0x7f000000, 0x7f7fffff, 0xff7fffff, 0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000, 0x7fc00001,
    0x7f800001, 0xff800001, 0x7fbfffff, 0x7fffffff, 0x3eaaaaab, 0x3dcccccd, 0x42c80000, 0x0000ffff, 0x00400000};
static const uint64_t edges64[] = {
    0x0000000000000000ull, 0x8000000000000000ull, 0x0000000000000001ull, 0x8000000000000001ull, 0x000fffffffffffffull,
    0x0010000000000000ull, 0x8010000000000000ull, 0x0010000000000001ull, 0x3feffffffffffffull, 0x3ff0000000000000ull,
    0xbff0000000000000ull, 0x3ff0000000000001ull, 0x3ff0000000000002ull, 0x4000000000000000ull, 0x4008000000000000ull,
    0x3ca0000000000000ull, 0x3ca0000000000001ull, 0x4330000000000000ull, 0x433fffffffffffffull, 0x43e0000000000000ull,
    0xc3e0000000000000ull, 0x7fefffffffffffffull, 0xffefffffffffffffull, 0x7fe0000000000000ull, 0x7ff0000000000000ull,
    0xfff0000000000000ull, 0x7ff8000000000000ull, 0xfff8000000000000ull, 0x7ff8000000000001ull, 0x7ff0000000000001ull,
    0x7ff7ffffffffffffull, 0x3fd5555555555555ull, 0x47efffffe0000000ull, 0x36a0000000000000ull, 0x380ffffffffffffull,
    0x47f0000000000000ull, 0x7ff0000020000000ull};

static const uint32_t fpcr_modes[] = {
    0, 1u << 24, 1u << 25, (1u << 24) | (1u << 25), 1u << 22, 2u << 22, 3u << 22,
    (1u << 24) | (1u << 22), (1u << 25) | (2u << 22), (1u << 24) | (1u << 25) | (3u << 22)};

static uint64_t operand(int ft, uint64_t *state) {
    uint64_t r = next_word(state);
    uint64_t pick = r >> 56;
    if (pick < 100) return ft ? edges64[(r >> 8) % RT_ARRAY_LEN(edges64)] : edges32[(r >> 8) % RT_ARRAY_LEN(edges32)];
    uint64_t w = next_word(state);
    if (!ft) w = (uint32_t)w;
    /* a random exponent in a window around 1 (cancellation and rounding), or fully random */
    if (pick < 190) {
        if (ft) w = (w & 0x800fffffffffffffull) | ((uint64_t)(1023 - 30 + (next_word(state) % 61)) << 52);
        else w = (w & 0x807fffffu) | ((uint32_t)(127 - 12 + (next_word(state) % 25)) << 23);
    }
    return w;
}

static unsigned long long checked, differences;

static void report(const char *what, int ft, uint32_t fpcr, uint32_t fpsr_in, uint64_t a, uint64_t b, uint64_t c, uint32_t extra,
                   uint64_t rust, uint32_t rust_fpsr, uint64_t ref, uint32_t ref_fpsr) {
    ++differences;
    if (differences <= 12)
        rt_log("decomp fp selftest MISMATCH %s ft=%d fpcr=%#x fpsr_in=%#x a=%#llx b=%#llx c=%#llx extra=%#x: rust=%#llx/%#x lifted=%#llx/%#x",
               what, ft, fpcr, fpsr_in, (unsigned long long)a, (unsigned long long)b, (unsigned long long)c, extra,
               (unsigned long long)rust, rust_fpsr, (unsigned long long)ref, ref_fpsr);
}

static void check(const char *what, uint32_t op, int ft, uint32_t fpcr, uint32_t fpsr_in, uint64_t a, uint64_t b, uint64_t c,
                  uint32_t extra) {
    Ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.fpcr = fpcr;
    ctx.fpsr = fpsr_in;
    v128 va = {(long long)a, 0}, vb = {(long long)b, 0}, vc = {(long long)c, 0};
    uint64_t ref;
    int sf = extra & 1, uns = (extra >> 1) & 1, rm = (extra >> 2) & 7, fbits = (int)(extra >> 8);
    switch (op) {
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7:
        ref = sa_get(ft, sa_fp2(&ctx, ft, (int)op, va, vb));
        break;
    case 16: case 17: case 18: case 19:
        ref = sa_get(ft, sa_fp3(&ctx, ft, (int)op - 16, va, vb, vc));
        break;
    case 32: case 33: {
        u8 fl[4];
        sa_fcmp(&ctx, ft, op == 33, va, vb, 0, fl);
        ref = (uint64_t)fl[0] << 3 | (uint64_t)fl[1] << 2 | (uint64_t)fl[2] << 1 | fl[3];
        break; }
    case 40: ref = sa_fcvt(&ctx, 0, 1, a); break;
    case 41: ref = sa_fcvt(&ctx, 1, 0, a); break;
    case 48: ref = sa_fcvti(&ctx, ft, sf, rm, uns, fbits, va); break;
    case 64: ref = sa_get(ft, sa_cvtf(&ctx, ft, sf, uns, fbits, a)); break;
    default: return;
    }
    uint32_t rust_fpsr = fpsr_in;
    uint64_t rust = smrpg_fp_selftest_op(op, (uint32_t)ft, fpcr, &rust_fpsr, a, b, c, extra);
    ++checked;
    if (rust != ref || rust_fpsr != ctx.fpsr)
        report(what, ft, fpcr, fpsr_in, a, b, c, extra, rust, rust_fpsr, ref, ctx.fpsr);
}

void decomp_fp_selftest(void) {
    static const struct { const char *name; uint32_t op; } arith[] = {
        {"fmul", 0}, {"fdiv", 1}, {"fadd", 2}, {"fsub", 3}, {"fmax", 4}, {"fmin", 5}, {"fmaxnm", 6}, {"fminnm", 7},
        {"fmadd", 16}, {"fmsub", 17}, {"fnmadd", 18}, {"fnmsub", 19}, {"fcmp", 32}, {"fcmpe", 33}};
    uint64_t state = 0x5eed0f1a7e57ull;
    unsigned long long start_checked = checked;
    for (int ft = 0; ft < 2; ++ft) {
        const unsigned nedge = ft ? RT_ARRAY_LEN(edges64) : RT_ARRAY_LEN(edges32);
        for (unsigned k = 0; k < RT_ARRAY_LEN(arith); ++k) {
            const int three = arith[k].op >= 16 && arith[k].op <= 19;
            /* every pair of edge operands, with a random third operand for the fused ops, in every mode */
            for (unsigned mi = 0; mi < RT_ARRAY_LEN(fpcr_modes); ++mi)
                for (unsigned i = 0; i < nedge; ++i)
                    for (unsigned j = 0; j < nedge; ++j) {
                        uint64_t a = ft ? edges64[i] : edges32[i], b = ft ? edges64[j] : edges32[j];
                        uint64_t c = three ? operand(ft, &state) : 0;
                        check(arith[k].name, arith[k].op, ft, fpcr_modes[mi], (uint32_t)next_word(&state) & 0x9f, a, b, c, 0);
                    }
            for (unsigned i = 0; i < 60000; ++i) {
                uint32_t fpcr = (next_word(&state) & 3) ? 0 : fpcr_modes[next_word(&state) % RT_ARRAY_LEN(fpcr_modes)];
                check(arith[k].name, arith[k].op, ft, fpcr, (uint32_t)next_word(&state) & 0x9f, operand(ft, &state),
                      operand(ft, &state), three ? operand(ft, &state) : 0, 0);
            }
        }
        /* conversions: format change, float to integer in every rounding mode, integer to float */
        for (unsigned mi = 0; mi < RT_ARRAY_LEN(fpcr_modes); ++mi) {
            for (unsigned i = 0; i < nedge * 3 + 4000; ++i) {
                uint64_t a = i < nedge ? (ft ? edges64[i] : edges32[i]) : operand(ft, &state);
                uint32_t fpcr = fpcr_modes[mi], fpsr = (uint32_t)next_word(&state) & 0x9f;
                check(ft ? "fcvt d to s" : "fcvt s to d", ft ? 41 : 40, ft, fpcr, fpsr, a, 0, 0, 0);
                static const unsigned fbit_choices[] = {0, 1, 7, 16, 31, 32};
                for (unsigned rm = 0; rm < 5; ++rm) {
                    uint32_t extra = (uint32_t)(next_word(&state) & 3) | (rm << 2) | (fbit_choices[next_word(&state) % 6] << 8);
                    check("fcvt to int", 48, ft, fpcr, fpsr, a, 0, 0, extra);
                }
                uint64_t iv = next_word(&state);
                switch (next_word(&state) & 3) {   /* small, power-of-two edges, and full-width values */
                case 0: iv = (uint64_t)(int64_t)(int16_t)iv; break;
                case 1: iv = 1ull << (iv & 63); if (iv & 0x1000) iv = ~iv + 1; break;
                default: break;
                }
                uint32_t extra = (uint32_t)(next_word(&state) & 3) | (fbit_choices[next_word(&state) % 6] << 8);
                check("int to float", 64, ft, fpcr, fpsr, iv, 0, 0, extra);
            }
        }
    }
    rt_log("decomp fp selftest cases=%llu differences=%llu", checked - start_checked, differences);
    if (differences) rt_exit(10, "decomp fp selftest failed");
}
