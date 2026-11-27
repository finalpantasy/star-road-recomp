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
        self.assertIsNone(match(0xBD400800, RET))   # ldr s0, [x0, #8] (vector register)
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
        self.assertTrue(re.fullmatch(r"[SCGN]?LEAF\((main_[0-9]+), ([0-9a-f]+), \w+(?:, \d+)*\)", row))


if __name__ == "__main__":
    unittest.main()
