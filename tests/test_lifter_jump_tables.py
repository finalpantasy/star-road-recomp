"""Bounded recovery tests for separate address calculation and table loads."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "lifter"))
from lift import cfg_preds, decode_range, match_compact_dataflow, match_compact_jump_table, match_split_jump_table

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


def b_(pc, target):
    return 0x14000000 | (((target - pc) >> 2) & 0x3FFFFFF)


def cbz_x(rt, pc, target):
    return 0xB4000000 | (((target - pc) >> 2) & 0x7FFFF) << 5 | rt


ADD_W1_1 = 0x11000421  # add w1, w1, #1
MOV_X10_X3 = 0xAA0303EA  # mov x10, x3
CTAB2 = BASE + 0x180


class CompactDataflowTests(unittest.TestCase):
    """The table base and the bound guard sit in earlier basic blocks than the switch."""

    def build(self, pre, tail_adr=True):
        """pre: words before the dispatch block. Dispatch: adr x11; ldrb; add; br; 4 targets; exit ret.
        Returns (words, dispatch index)."""
        n = len(pre)
        disp = [adr(11, BASE + 4 * n, BASE + 4 * (n + 4)), LDRB | 9 << 16 | 10 << 5 | 12,
                ADD_LSL2 | 12 << 16 | 11 << 5 | 11, BR_X11]
        return pre + disp + [RET] * 5, n

    def run_df(self, words, entries=(BASE,), data=None, forbidden=()):
        end = BASE + len(words) * 4
        insns = decode_range(words, BASE, BASE, end)
        pos = {a: i for i, (a, _, _) in enumerate(insns)}
        idx = next(i for i, x in enumerate(insns) if x[1] == "br")
        data = {CTAB + i: v for i, v in enumerate((0, 1, 2, 3))} if data is None else data
        rd = lambda addr, size: data.get(addr)
        valid = lambda addr: addr not in forbidden
        lin = match_compact_jump_table(insns, idx, rd, (BASE, end), entries, valid)
        df = match_compact_dataflow(insns, pos, cfg_preds(insns, pos), idx, rd, (BASE, end), entries, valid)
        return lin, (df[0] if df else None)

    def expect(self, words):
        base = BASE + 4 * (len(words) - 5)
        return (0, 0, [base + 4 * i for i in range(4)])

    def exit_of(self, n_pre):
        return BASE + 4 * (n_pre + 4 + 4)  # the final ret

    def test_base_and_guard_in_earlier_block(self):
        # 0 adr x10; 1 cmp; 2 b.hi exit; 3 mov w9,w1; 4 b 5 (new block); dispatch
        pre = [adr(10, BASE, CTAB), cmp_w(1, 3), 0, MOV_W9_W1, b_(BASE + 16, BASE + 20)]
        w, n = self.build(pre)
        pre[2] = bhi(BASE + 8, self.exit_of(n))
        w, n = self.build(pre)
        lin, df = self.run_df(w)
        self.assertIsNone(lin)
        self.assertEqual(df, self.expect(w))

    def test_diamond_where_both_paths_agree(self):
        # 0 adr; 1 cmp; 2 b.hi exit; 3 mov; 4 cbz x2 -> 7; 5 nop; 6 b 8; 7 nop; 8 dispatch
        pre = [adr(10, BASE, CTAB), cmp_w(1, 3), 0, MOV_W9_W1, cbz_x(2, BASE + 16, BASE + 28), NOP,
               b_(BASE + 24, BASE + 32), NOP]
        n = len(pre)
        pre[2] = bhi(BASE + 8, self.exit_of(n))
        w, n = self.build(pre)
        lin, df = self.run_df(w)
        self.assertIsNone(lin)
        self.assertEqual(df, self.expect(w))

    def test_rejects_two_reaching_bases(self):
        # cbz x2 picks adr x10 = CTAB or CTAB2; both tables hold different entries
        pre = [cmp_w(1, 3), 0, MOV_W9_W1, cbz_x(2, BASE + 12, BASE + 24), adr(10, BASE + 16, CTAB),
               b_(BASE + 20, BASE + 28), adr(10, BASE + 24, CTAB2), NOP]
        n = len(pre)
        pre[1] = bhi(BASE + 4, self.exit_of(n))
        w, n = self.build(pre)
        data = {CTAB + i: v for i, v in enumerate((0, 1, 2, 3))}
        data.update({CTAB2 + i: v for i, v in enumerate((3, 2, 1, 0))})
        lin, df = self.run_df(w, data=data)
        self.assertIsNone(df)

    def test_rejects_guard_bypassed_on_one_path(self):
        # 0 adr; 1 mov; 2 cbz x2 -> 5 (skips the guard); 3 cmp; 4 b.hi exit; 5 dispatch
        pre = [adr(10, BASE, CTAB), MOV_W9_W1, cbz_x(2, BASE + 8, BASE + 20), cmp_w(1, 3), 0]
        n = len(pre)
        pre[4] = bhi(BASE + 16, self.exit_of(n))
        w, n = self.build(pre)
        lin, df = self.run_df(w)
        self.assertIsNone(df)

    def test_rejects_register_redefined_after_guard(self):
        # guard on w1, then w1 is rewritten before it is copied to the index register
        pre = [adr(10, BASE, CTAB), cmp_w(1, 3), 0, ADD_W1_1, MOV_W9_W1, b_(BASE + 16, BASE + 20)]
        n = len(pre)
        pre[2] = bhi(BASE + 8, self.exit_of(n))
        w, n = self.build(pre)
        self.assertIsNone(self.run_df(w)[1])

    def test_rejects_table_base_redefined(self):
        pre = [adr(10, BASE, CTAB), cmp_w(1, 3), 0, MOV_W9_W1, MOV_X10_X3, b_(BASE + 20, BASE + 24)]
        n = len(pre)
        pre[2] = bhi(BASE + 8, self.exit_of(n))
        w, n = self.build(pre)
        self.assertIsNone(self.run_df(w)[1])

    def test_rejects_unknown_entry_into_the_middle(self):
        # the second block can be entered from outside (landing pad or table target): no guard on that path
        pre = [adr(10, BASE, CTAB), cmp_w(1, 3), 0, MOV_W9_W1, b_(BASE + 16, BASE + 20)]
        n = len(pre)
        pre[2] = bhi(BASE + 8, self.exit_of(n))
        w, n = self.build(pre)
        self.assertIsNone(self.run_df(w, entries=(BASE, BASE + 12))[1])

    def test_rejects_guard_taken_edge_only(self):
        # b.hi falls into the dispatch via its taken edge: the guard says nothing about the in-range case
        pre = [adr(10, BASE, CTAB), cmp_w(1, 3), 0, MOV_W9_W1]
        n = len(pre)
        pre[2] = bhi(BASE + 8, BASE + 4 * (n))
        w, n = self.build(pre)
        self.assertIsNone(self.run_df(w)[1])

    # ---- rules added for task-49: calls keep callee-saved registers, narrow loads zero-extend, table targets are orphans
    def test_callee_saved_base_survives_a_call(self):
        # adr x19; bl; cmp w1; b.hi; mov w9,w1; dispatch through x19. A caller-saved base (x10) must not survive.
        def words(base_reg):
            pre = [adr(base_reg, BASE, CTAB), 0x94000001, cmp_w(1, 3), 0, MOV_W9_W1]
            n = len(pre)
            pre[3] = bhi(BASE + 12, self.exit_of(n))
            disp = [adr(11, BASE + 4 * n, BASE + 4 * (n + 4)), LDRB | 9 << 16 | base_reg << 5 | 12,
                    ADD_LSL2 | 12 << 16 | 11 << 5 | 11, BR_X11]
            return pre + disp + [RET] * 5
        w = words(19)
        self.assertEqual(self.run_df(w)[1], self.expect(w))
        self.assertIsNone(self.run_df(words(10))[1])

    def test_narrow_load_index_is_zero_extended(self):
        # ldrh w8 then ldrb w12,[x10,x8] (64-bit index register) is bounded by cmp w8,#3
        LDRH_IMM_W8 = 0x79400028  # ldrh w8, [x1]
        pre = [adr(10, BASE, CTAB), LDRH_IMM_W8, 0x71000D1F, 0]  # cmp w8,#3
        n = len(pre)
        pre[3] = bhi(BASE + 12, self.exit_of(n))
        disp = [adr(11, BASE + 4 * n, BASE + 4 * (n + 4)), LDRB | 8 << 16 | 10 << 5 | 12,
                ADD_LSL2 | 12 << 16 | 11 << 5 | 11, BR_X11]
        w = pre + disp + [RET] * 5
        self.assertEqual(self.run_df(w)[1], self.expect(w))

    def orphan_words(self, extra):
        # target 0 is a block `b <mov w9,w1>` that nothing else reaches: the dispatch feeds its own loop
        pre = [adr(10, BASE, CTAB), cmp_w(1, 3), 0, MOV_W9_W1]
        n = len(pre)
        pre[2] = bhi(BASE + 8, self.exit_of(n))
        w, n = self.build(pre)
        w[n + 4] = b_(BASE + 4 * (n + 4), BASE + 12)
        return w + extra

    def test_table_target_loop_back_is_resolved(self):
        w = self.orphan_words([])
        self.assertEqual(self.run_df(w)[1], self.expect(w))

    def test_rejects_orphan_block_that_is_not_a_target(self):
        w = self.orphan_words([])
        w.append(b_(BASE + 4 * len(w), BASE + 12))
        self.assertIsNone(self.run_df(w)[1])


if __name__ == "__main__":
    unittest.main()
