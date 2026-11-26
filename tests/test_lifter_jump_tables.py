"""Bounded recovery tests for separate address calculation and table loads."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "lifter"))
from lift import decode_range, match_split_jump_table

BASE = 0x100000
TABLE = BASE + 0x100
WORDS = [
    0x10000808,  # adr x8, TABLE
    0x10000069,  # adr x9, instruction 4
    0x7100041F,  # cmp w0, #1
    0x540000C8,  # b.hi instruction 9
    0x8B20490A,  # add x10, x8, w0, uxtw #2
    0xB980014B,  # ldrsw x11, [x10]
    0x8B0B012C,  # add x12, x9, x11
    0xD61F0180,  # br x12
    0xD65F03C0,  # ret
    0xD65F03C0,  # ret
]


class SplitJumpTableTests(unittest.TestCase):
    def match(self, words=None, entries=(BASE,), values=(-12, 16), forbidden=()):
        words = WORDS if words is None else words
        end = BASE + len(words) * 4
        insns = decode_range(words, BASE, BASE, end)
        data = {TABLE + i * 4: value & 0xFFFFFFFF for i, value in enumerate(values)}
        return match_split_jump_table(insns, 7, lambda addr, size: data.get(addr) if size == 4 else None,
                                      (BASE, end), entries, lambda addr: addr not in forbidden)

    def test_unsigned_guard_and_signed_offsets(self):
        self.assertEqual(self.match(), (0, 0, [BASE + 4, BASE + 32]))

    def test_load_then_sign_extend_in_add(self):
        words = WORDS.copy()
        words[5:7] = [0xB940014B, 0x8B2BC12C]  # ldr w11; add x12, x9, w11, sxtw
        self.assertEqual(self.match(words), self.match())

    def test_unguarded_and_bypassed(self):
        words = WORDS.copy()
        words[3] = 0xD503201F  # nop instead of b.hi
        self.assertIsNone(self.match(words))
        self.assertIsNone(self.match(entries=(BASE, BASE + 16)))
        self.assertIsNone(self.match(entries=(BASE, BASE + 28)))

    def test_guard_on_stale_value(self):
        words = WORDS.copy()
        words[4] |= 1 << 16  # the address calculation uses w1
        self.assertIsNone(self.match(words))

    def test_wrong_scale(self):
        words = WORDS.copy()
        words[4] ^= 1 << 10  # uxtw #3 for 4-byte entries
        self.assertIsNone(self.match(words))

    def test_unreadable_or_invalid_target(self):
        for values in [(-12,), (-12, 17), (-12, 28)]:
            self.assertIsNone(self.match(values=values))
        self.assertIsNone(self.match(forbidden=(BASE + 32,)))


if __name__ == "__main__":
    unittest.main()
