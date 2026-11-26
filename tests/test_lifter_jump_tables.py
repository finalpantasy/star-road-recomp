"""Bounded recovery tests for separate address calculation and table loads."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "lifter"))
from lift import decode_range, match_compact_jump_table, match_split_jump_table

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


def adr(rd, pc, target):
    off = target - pc
    return 0x10000000 | (off & 3) << 29 | ((off >> 2) & 0x7FFFF) << 5 | rd


def bhi(pc, target):
    return 0x54000008 | (((target - pc) >> 2) & 0x7FFFF) << 5


def cmp_w(rn, imm):
    return 0x7100001F | imm << 10 | rn << 5


NOP = 0xD503201F
RET = 0xD65F03C0
MOV_W9_W1 = 0x2A0103E9
AND_W9_W1_3 = 0x12000429  # and w9, w1, #3
LDRB = 0x38606800  # ldrb w12, [x10, x9] after adding rm<<16 | rn<<5 | rt
LDRH = 0x78607800  # ldrh w12, [x10, x9, lsl #1]
ADD_LSL2 = 0x8B000800  # add x11, x11, x12, lsl #2 after adding rm<<16 | rn<<5 | rd
BR_X11 = 0xD61F0160
CTAB = BASE + 0x100  # table address
CTARGET = 7  # word index of the br


def compact_words(load=LDRB, index=None, guard=None):
    """adr table; guard; index; adr base; load; add; br; four return targets; guard exit."""
    guard = [cmp_w(1, 3), bhi(BASE + 8, BASE + 48)] if guard is None else guard
    index = [MOV_W9_W1] if index is None else index
    head = [adr(10, BASE, CTAB)] + guard + index
    tail = [adr(11, BASE + 4 * (len(head)), BASE + 4 * (len(head) + 4)), load | 9 << 16 | 10 << 5 | 12,
            ADD_LSL2 | 12 << 16 | 11 << 5 | 11, BR_X11]
    return head + tail + [RET] * 5


class CompactJumpTableTests(unittest.TestCase):
    def match(self, words, entries=(BASE,), data=None, forbidden=()):
        end = BASE + len(words) * 4
        insns = decode_range(words, BASE, BASE, end)
        idx = next(i for i, x in enumerate(insns) if x[1] == "br")
        data = {CTAB + i: v for i, v in enumerate((0, 1, 2, 3))} if data is None else data
        return match_compact_jump_table(insns, idx, lambda addr, size: data.get(addr),
                                        (BASE, end), entries, lambda addr: addr not in forbidden)

    def targets(self, words):
        base = BASE + 4 * (len(words) - 5)
        return (0, 0, [base + 4 * i for i in range(4)])

    def test_guarded_byte_table(self):
        w = compact_words()
        self.assertEqual(self.match(w), self.targets(w))

    def test_conditional_compare_guard(self):
        # cmp x8, #0 ; ccmp w1, #3, #2, ne ; b.hi: the false arm sets C=1 Z=0 so b.hi leaves
        guard = [0xF100011F, 0x7A430800 | 1 << 12 | 1 << 5 | 2, bhi(BASE + 12, BASE + 52)]  # ccmp w1, #3, #2, ne
        w = compact_words(guard=guard)
        self.assertEqual(self.match(w), self.targets(w))
        guard[1] = 0x7A430800 | 1 << 12 | 1 << 5 | 0  # nzcv 0: b.hi not taken when the first compare fails
        self.assertIsNone(self.match(compact_words(guard=guard)))

    def test_mask_bounds_the_index(self):
        w = compact_words(guard=[NOP], index=[AND_W9_W1_3])
        self.assertEqual(self.match(w), self.targets(w))

    def test_halfword_table(self):
        w = compact_words(load=LDRH, guard=[cmp_w(1, 3), bhi(BASE + 8, BASE + 48)])
        data = {CTAB + 2 * i: v for i, v in enumerate((0, 1, 2, 3))}
        self.assertEqual(self.match(w, data=data), self.targets(w))

    def test_rejects_missing_or_wrong_guard(self):
        self.assertIsNone(self.match(compact_words(guard=[NOP, NOP])))
        self.assertIsNone(self.match(compact_words(guard=[cmp_w(2, 3), bhi(BASE + 8, BASE + 48)])))  # other register
        self.assertIsNone(self.match(compact_words(guard=[cmp_w(1, 3), NOP])))  # no branch
        self.assertIsNone(self.match(compact_words(guard=[cmp_w(1, 3), bhi(BASE + 8, BASE + 20)])))  # exits to the table

    def test_rejects_bypassed_guard_and_bad_targets(self):
        w = compact_words()
        self.assertIsNone(self.match(w, entries=(BASE, BASE + 16)))  # something jumps in after the guard
        self.assertIsNone(self.match(w, data={CTAB: 0, CTAB + 1: 1, CTAB + 2: 2, CTAB + 3: 40}))  # outside the function
        self.assertIsNone(self.match(w, data={CTAB: 0, CTAB + 1: 1, CTAB + 2: 2}))  # unreadable entry
        self.assertIsNone(self.match(w, forbidden=(BASE + 4 * (len(w) - 5),)))  # not an instruction


if __name__ == "__main__":
    unittest.main()
