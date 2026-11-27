"""Unit tests for the decompgen matcher: hand-encoded A64 words, exact shapes only (stdlib unittest).

  python -I tools/decompgen/test_decompgen.py

The semantic proof of each generated function is the A/B selftest against the lifted C (runtime/decomp/swap.c);
these tests pin which instruction shapes are accepted and which near misses are refused.
"""
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "lifter"))
import dec  # noqa: E402
import patterns  # noqa: E402

RET = 0xD65F03C0
PC = 0x7100001000


def match(*words):
    insns = [dec.decode(w, PC + 4 * i) for i, w in enumerate(words)]
    return patterns.match(insns, PC)


class Matcher(unittest.TestCase):
    def test_empty(self):
        self.assertEqual(match(RET).pattern, "empty")

    def test_getters(self):
        cases = {0xF9400800: "u64", 0xB9400800: "u32", 0x39400400: "u8", 0x79400400: "u16",
                 0x39C00400: "s8w", 0xB9800400: "s32x", 0x39800400: "s8x", 0x79C00400: "s16w"}
        for word, variant in cases.items():
            spec = match(word, RET)
            self.assertEqual((spec.pattern, spec.variant), ("get", variant), hex(word))
        spec = match(0xB9800400, RET)          # ldrsw x0, [x0, #4]: sign extended to 64 bits
        self.assertIn("as i32 as i64 as u64", spec.rust_body)
        spec = match(0x39C00400, RET)          # ldrsb w0, [x0, #1]: sign extended to 32 bits, upper half cleared
        self.assertIn("as i8 as i32 as u32", spec.rust_body)
        self.assertEqual((spec.base_reg, spec.offset, spec.nbytes), (0, 1, 1))

    def test_getter_refusals(self):
        self.assertIsNone(match(0xF8408C00, RET))   # ldr x0, [x0, #8]! (write-back)
        self.assertIsNone(match(0xF9400401, RET))   # ldr x1, [x0, #8] (writes x1, not x0)
        self.assertEqual(match(0xBD400800, RET).pattern, "vget")   # ldr s0, [x0, #8]: a vector register is never `get`
        self.assertIsNone(match(0xF9400800))        # no ret
        self.assertIsNone(match(0xF9400800, 0xD503201F, RET))   # extra instruction

    def test_nullsafe_getter(self):
        spec = match(0xB4000040, 0xF9400800, RET)   # cbz x0, +8; ldr x0, [x0, #0x10]; ret
        self.assertEqual((spec.pattern, spec.kind, spec.offset), ("get_nullsafe", "NLEAF", 0x10))
        self.assertIsNone(match(0xB4000060, 0xF9400800, RET))   # cbz target is not the ret
        self.assertIsNone(match(0x34000040, 0xF9400800, RET))   # cbz w0 tests only 32 bits
        self.assertIsNone(match(0xB5000040, 0xF9400800, RET))   # cbnz

    def test_setters(self):
        spec = match(0xB9000401, RET)               # str w1, [x0, #4]
        self.assertEqual((spec.pattern, spec.kind, spec.base_reg, spec.offset, spec.nbytes), ("set", "SLEAF", 0, 4, 4))
        self.assertIn("b as u32", spec.rust_body)
        self.assertIn("guest_st8(a.wrapping_add(delta).wrapping_add(0x10), 0)", match(0x3900401F, RET).rust_body)
        self.assertEqual(match(0xF9000401, RET).nbytes, 8)
        self.assertIsNone(match(0xB8004C01, RET))   # str w1, [x0, #4]! (write-back) must not match
        self.assertIsNone(match(0xB9000408, RET))   # str w8, [x0, #4]: value from a scratch register
        self.assertIsNone(match(0x2D000401, RET))   # stp s1, s1, [x0]: vector pair

    def test_set_bool(self):
        spec = match(0x12000028, 0x39010008, RET)   # and w8, w1, #1; strb w8, [x0, #0x40]; ret
        self.assertEqual((spec.pattern, spec.variant, spec.kind, spec.base_reg, spec.offset, spec.nbytes),
                         ("set_bool", "w8", "SLEAF", 0, 0x40, 1))
        self.assertEqual(spec.rust_body, "unsafe { store_low_bit(a.wrapping_add(delta).wrapping_add(0x40), b) };\na")
        self.assertEqual(match(0x12000049, 0x39000409, RET).rust_body,       # and w9, w2, #1; strb w9, [x0, #1]
                         "unsafe { store_low_bit(a.wrapping_add(delta).wrapping_add(0x1), c) };\na")
        self.assertIsNone(match(0x12000028, 0x39004009, RET))   # strb stores w9, not the masked w8
        self.assertIsNone(match(0x12000428, 0x39004008, RET))   # and w8, w1, #3: a wider mask
        self.assertIsNone(match(0x92400028, 0x39004008, RET))   # and x8, x1, #1: 64-bit form
        self.assertIsNone(match(0x72000028, 0x39004008, RET))   # ands: sets flags
        self.assertIsNone(match(0x12000020, 0x39004000, RET))   # and w0, w1, #1: dest is x0 (the argument)
        self.assertIsNone(match(0x12000028, 0x79004008, RET))   # strh stores 16 bits
        self.assertIsNone(match(0x12000028, 0x38010C08, RET))   # strb w8, [x0, #16]! (write-back)
        self.assertIsNone(match(0x12000028, 0x39004008))        # no ret
        self.assertIsNone(match(0x12000068, 0x39004008, RET))   # and w8, w3, #1: source is not an argument
        import generate
        row = generate.manifest_row(0x71002cdf10, 63, spec)
        self.assertEqual(row, "SLEAF(main_0063, 71002cdf10, smrpg_g_set_bool_71002cdf10, 0, 64, 1)")
        self.assertIn("(a: u64, b: u64, _: u64, delta: u64)", generate.rust_function(0x71002cdf10, spec, None))

    def test_constants(self):
        self.assertEqual(match(0x52800020, RET).rust_body, "0x1")                   # mov w0, #1
        self.assertEqual(match(0x52824680, 0x72AACF00, RET).rust_body, "0x56781234")   # movz + movk lsl 16
        self.assertEqual(match(0x2A1F03E0, RET).rust_body, "0")                     # mov w0, wzr
        self.assertEqual(match(0x12800000, RET).rust_body, "0xffffffff")            # movn w0, #0
        self.assertEqual(match(0x92800000, RET).rust_body, "0xffffffffffffffff")    # movn x0, #0
        self.assertIsNone(match(0x52800021, RET))                                   # mov w1, #1
        self.assertIsNone(match(0x72A00000, RET))                                   # movk first
        self.assertIsNone(match(0x52800020, 0x72800040 | 1, RET))                   # movk into another register

    def test_add_and_mov(self):
        self.assertEqual(match(0x91006000, RET).rust_body, "a.wrapping_add(0x18)")  # add x0, x0, #0x18
        self.assertEqual(match(0xD1002000, RET).variant, "sub_x")                   # sub x0, x0, #8
        self.assertIsNone(match(0xB1006000, RET))                                   # adds sets flags
        self.assertEqual(match(0xAA0103E0, RET).rust_body, "b")                     # mov x0, x1
        self.assertEqual(match(0x2A0103E0, RET).rust_body, "u64::from(b as u32)")   # mov w0, w1

    def test_manifest_rows_match_the_wrapper_regex(self):
        import generate
        row = generate.manifest_row(0x7101347ed8, 207, match(0xB9000401, RET))
        self.assertEqual(row, "SLEAF(main_0207, 7101347ed8, smrpg_g_set_7101347ed8, 0, 4, 4)")
        self.assertTrue(re.fullmatch(r"[SCGNVD]?LEAF\((main_[0-9]+), ([0-9a-f]+), \w+(?:, \d+)*\)", row))

    # ---- task-50: the words below are real functions of the main module (address in each comment) ----
    def test_float_getter_and_setter(self):
        spec = match(0xBD401C00, RET)               # 0x710003f1e0 ldr s0, [x0, #0x1c]
        self.assertEqual((spec.pattern, spec.kind, spec.vkind, spec.vreg, spec.base_reg, spec.offset, spec.nbytes),
                         ("vget", "VLEAF", 1, 0, 0, 0x1c, 4))
        self.assertEqual(spec.rust_body, "unsafe { u64::from(guest_ld32(a.wrapping_add(0x1c))) }")
        spec = match(0xFD400800, RET)               # 0x71001f7270 ldr d0, [x0, #0x10]
        self.assertEqual((spec.vkind, spec.nbytes, spec.rust_body), (1, 8, "unsafe { guest_ld64(a.wrapping_add(0x10)) }"))
        spec = match(0xBD001C00, RET)               # 0x710003f1f0 str s0, [x0, #0x1c]
        self.assertEqual((spec.pattern, spec.vkind, spec.nbytes), ("vset", 2, 4))
        self.assertIn("guest_st32(a.wrapping_add(delta).wrapping_add(0x1c), vlo as u32)", spec.rust_body)
        self.assertEqual(match(0xFD000800, RET).nbytes, 8)    # 0x71001f6340 str d0, [x0, #0x10]
        self.assertIsNone(match(0x3D400800, RET))             # ldr b0, [x0, #2]: 1 byte, not s or d
        self.assertIsNone(match(0x3DC00800, RET))             # ldr q0, [x0, #0x20]: 16 bytes
        self.assertIsNone(match(0xBD401C10, RET))             # ldr s16, [x0, #0x1c]: v16 is not flushed at ret
        self.assertIsNone(match(0xBC401400, RET))             # ldr s0, [x0], #1 (post-index write-back)
        self.assertIsNone(match(0xBD401C00))                  # no ret

    def test_float_constants(self):
        spec = match(0x1E2E1000, RET)               # 0x71002bd6d0 fmov s0, #1.0
        self.assertEqual((spec.pattern, spec.vkind, spec.rust_body), ("vconst", 3, "0x3f800000"))
        self.assertEqual(match(0x1E669000, RET).rust_body, "0x4034000000000000")   # 0x7100f7a81c fmov d0, #20.0
        self.assertEqual(match(0x1E2703E0, RET).rust_body, "0")      # 0x7100290590 fmov s0, wzr
        self.assertEqual(match(0x9E6703E0, RET).variant, "d0_zr")    # 0x7100c18580 fmov d0, xzr
        self.assertEqual(match(0x1E2703E2, RET).vreg, 2)             # 0x7102059d80 fmov s2, wzr
        self.assertEqual(match(0x6F00E400, RET).variant, "q0_zero")  # 0x7101359ec0 movi v0.2d, #0
        self.assertIsNone(match(0x1E2703F1, RET))                    # fmov s17, wzr: v17 is not flushed at ret
        self.assertIsNone(match(0x1E2603E0, RET))                    # fmov w0, s31: to a general register
        self.assertIsNone(match(0x1E2703E0))                         # no ret

    def test_expand_imm_matches_the_lifter(self):
        import lower_simd_a
        for dbl in (0, 1):
            for imm8 in range(256):
                self.assertEqual(patterns.vfp_expand_imm(dbl, imm8), lower_simd_a.vfp_expand_imm(dbl, imm8), (dbl, imm8))

    def test_field_of_field(self):
        spec = match(0xF9400808, 0xB9401900, RET)   # 0x7100171d60 ldr x8, [x0, #0x10]; ldr w0, [x8, #0x18]
        self.assertEqual((spec.pattern, spec.kind, spec.variant, spec.base_reg, spec.offset, spec.offset2),
                         ("get2", "DLEAF", "u32", 0, 0x10, 0x18))
        self.assertIn("guest_ld64(a.wrapping_add(0x10))", spec.rust_body)
        self.assertEqual(match(0xF9400028, 0x79C0A900, RET).variant, "s16w")   # 0x710033f0a4 ldrsh w0, [x8, #0x54]
        self.assertEqual(match(0xF9400008, 0xF9400500, RET).variant, "u64")     # 0x710005f524 ldr x8, [x0]; ldr x0, [x8, #8]
        self.assertEqual(match(0xF9400008, 0x3944B900, RET).variant, "u8")      # 0x710004c9d8
        spec = match(0xF940FC08, 0xBD400900, RET)   # 0x71002e3158 ldr x8, [x0, #0x1f8]; ldr s0, [x8, #8]
        self.assertEqual((spec.pattern, spec.vkind, spec.offset, spec.offset2, spec.nbytes), ("vget2", 4, 0x1f8, 8, 8))
        self.assertIsNone(match(0xF9400808, 0xB9401920, RET))   # ldr w0, [x9, #0x18]: not the loaded register
        self.assertIsNone(match(0xF9400808, 0xB9401901, RET))   # ldr w1, [x8, #0x18]: not x0
        self.assertIsNone(match(0xB9400808, 0xB9401900, RET))   # first load is 32 bits: not a pointer
        self.assertIsNone(match(0xF9400803, 0xB9401860, RET))   # the loaded register is x3 (flushed at ret)
        self.assertIsNone(match(0xF9400808, 0xB9401900))        # no ret

    def test_boolean_getter(self):
        spec = match(0xB9401008, 0x7100011F, 0x1A9F17E0, RET)   # 0x7100156b00 ldr w8, [x0, #0x10]; cmp w8, #0; cset w0, eq
        self.assertEqual((spec.pattern, spec.kind, spec.variant, spec.offset, spec.nbytes),
                         ("get_cmp", "GLEAF", "u32_eq_imm_w", 0x10, 4))
        self.assertIn("(v & 0xffffffff) == (0x0 & 0xffffffff)", spec.rust_body)
        self.assertEqual(match(0xB9401008, 0x6B01011F, 0x1A9F17E0, RET).variant, "u32_eq_x1_w")    # 0x7100164c20
        self.assertEqual(match(0xB9401008, 0x7100011F, 0x1A9F07E0, RET).variant, "u32_ne_imm_w")   # 0x7100162980
        self.assertEqual(match(0xB9402028, 0x6B02011F, 0x1A9F07E0, RET).variant, "u32_ne_x2_w")    # 0x7100302df8
        self.assertEqual(match(0xF9400808, 0xF100011F, 0x1A9F17E0, RET).variant, "u64_eq_imm_x")   # 0x71001a3800
        self.assertEqual(match(0xF9400808, 0xEB01011F, 0x1A9F17E0, RET).variant, "u64_eq_x1_x")    # 0x710017dcc0
        self.assertEqual(match(0xF9403008, 0xF100011F, 0x1A9F07E0, RET).variant, "u64_ne_imm_x")   # 0x710005bce4
        self.assertIsNone(match(0xB9401008, 0x7100411F, 0x1A9F17E0, RET))   # cmp w8, #16: the selftest cannot hit equality
        self.assertIsNone(match(0xB9401008, 0x6B03011F, 0x1A9F17E0, RET))   # cmp w8, w3: not an argument register
        self.assertIsNone(match(0xB9400428, 0x6B01011F, 0x1A9F17E0, RET))  # ldr w8, [x1, ...] compared with w1
        self.assertIsNone(match(0xB9401008, 0x7100011F, 0x1A9F27E0, RET))   # cset w0, hs: not an equality test
        self.assertIsNone(match(0xB9401008, 0x7100011F, 0x9A9F17E0, RET))   # cset x0: 64-bit result
        self.assertIsNone(match(0xB9401008, 0x3100011F, 0x1A9F17E0, RET))   # cmn w8, #0 (adds)
        self.assertIsNone(match(0xB9801008, 0x7100011F, 0x1A9F17E0, RET))   # ldrsw: signed load
        self.assertIsNone(match(0xB9401008, 0x7100011F, 0x1A9F17E0))        # no ret

    def test_new_manifest_rows(self):
        import generate
        row = generate.manifest_row(0x710003f1e0, 11, match(0xBD401C00, RET))
        self.assertEqual(row, "VLEAF(main_0011, 710003f1e0, smrpg_g_vget_710003f1e0, 1, 0, 0, 28, 4, 0)")
        row = generate.manifest_row(0x7100171d60, 38, match(0xF9400808, 0xB9401900, RET))
        self.assertEqual(row, "DLEAF(main_0038, 7100171d60, smrpg_g_get2_7100171d60, 0, 16, 8, 24)")
        row = generate.manifest_row(0x71002e3158, 63, match(0xF940FC08, 0xBD400900, RET))
        self.assertEqual(row, "VLEAF(main_0063, 71002e3158, smrpg_g_vget2_71002e3158, 4, 0, 0, 504, 8, 8)")
        self.assertTrue(re.fullmatch(r"[SCGNVD]?LEAF\((main_[0-9]+), ([0-9a-f]+), \w+(?:, \d+)*\)", row))
        fn = generate.rust_function(0x710003f1f0, match(0xBD001C00, RET), None)
        self.assertIn("(a: u64, _: u64, _: u64, vlo: u64, delta: u64) -> u64", fn)
        fn = generate.rust_function(0x7100290590, match(0x1E2703E0, RET), None)
        self.assertIn('fn smrpg_g_vconst_7100290590(_: u64, _: u64, _: u64, _: u64, _: u64) -> u64', fn)


if __name__ == "__main__":
    unittest.main()
