extern crate std;
use super::*;
use crate::bridge::tests::{CALLS, SAFEPOINTS, STORES, clear, install};
use std::boxed::Box;
use std::vec::Vec;

/// Words: param 0 (+0x28 field 50, +0x98 status manager 30, +0xa0 buff manager 40), status manager 30 (+0x10 status),
/// buff manager 40 (+0x10, +0x11 flag bytes, +0x12 u16, +0x18 int[] 60, +0x28 invincible turn), int[] 60 (+0x18 length,
/// +0x20 elements).
struct Graph(Box<[u64; 128]>);

impl Graph {
    fn new(turn: i32, status: u32, length: u32) -> Self {
        let mut words = Box::new([0u64; 128]);
        let base = words.as_ptr() as u64;
        words[0x98 / 8] = base + 8 * 30;
        words[0xa0 / 8] = base + 8 * 40;
        words[0x28 / 8] = base + 8 * 50;
        words[40 + 3] = base + 8 * 60;
        let g = Self(words);
        g.put32(8 * 30 + 0x10, status);
        g.put32(8 * 40 + 0x28, turn as u32);
        g.put32(8 * 60 + 0x18, length);
        g.put16(8 * 40 + 0x12, 0x1234);
        g
    }

    fn base(&self) -> u64 {
        self.0.as_ptr() as u64
    }

    fn put32(&self, byte: usize, value: u32) {
        unsafe {
            self.0
                .as_ptr()
                .cast::<u8>()
                .add(byte)
                .cast::<u32>()
                .cast_mut()
                .write_unaligned(value);
        }
    }

    fn put16(&self, byte: usize, value: u16) {
        unsafe {
            self.0
                .as_ptr()
                .cast::<u8>()
                .add(byte)
                .cast::<u16>()
                .cast_mut()
                .write_unaligned(value);
        }
    }

    fn put64(&self, word: usize, value: u64) {
        unsafe { self.0.as_ptr().add(word).cast_mut().write(value) }
    }

    fn buff(&self) -> u64 {
        self.base() + 8 * 40
    }
}

fn stores() -> Vec<(u64, u32, u64)> {
    STORES.with(|s| s.borrow().clone())
}

fn calls() -> Vec<u64> {
    CALLS.with(|c| c.borrow().clone())
}

fn file(g: &Graph) -> [u64; 96] {
    let mut f = [0u64; 96];
    f[0] = g.base();
    f[1] = 3;
    f[2] = 100;
    f[8] = 0x88;
    f[9] = 0x99;
    f[19] = 0x19;
    f[29] = 0x29;
    f[30] = 0xabc;
    f[31] = 0x1000;
    f
}

fn regs(f: &mut [u64; 96]) -> Regs {
    unsafe { Regs::from_raw(f.as_mut_ptr()) }
}

#[test]
fn clear_stores_zero_to_the_turn() {
    clear();
    let g = Graph::new(7, 0, 4);
    unsafe { BattleCharacterParam::at(g.base()) }.clear_invincible_turn();
    assert_eq!(stores(), [(g.buff() + 0x28, 4, 0)]);
}

#[test]
fn flag_11_follows_the_sign_of_the_turn() {
    for (turn, expect) in [
        (1, false),
        (2, false),
        (0, true),
        (-1, true),
        (i32::MIN, true),
    ] {
        clear();
        let g = Graph::new(turn, 0, 4);
        unsafe { BattleCharacterParam::at(g.base()) }.set_flag_11_when_not_invincible();
        let want: Vec<_> = if expect {
            std::vec![(g.buff() + 0x11, 1, 1)]
        } else {
            Vec::new()
        };
        assert_eq!(stores(), want, "turn {turn}");
    }
}

#[test]
fn flag_10_needs_no_turn_and_a_status_other_than_5() {
    for (turn, status, expect, polled) in [
        (1, 3, false, false),
        (0, 3, true, true),
        (0, 5, false, true),
        (-4, 0, true, true),
        (0, 0x105, true, true),
    ] {
        clear();
        let g = Graph::new(turn, status, 4);
        let mut f = file(&g);
        let mut r = regs(&mut f);
        unsafe { BattleCharacterParam::at(g.base()) }.set_flag_10_when_not_invincible(&mut r);
        let want: Vec<_> = if expect {
            std::vec![(g.buff() + 0x10, 1, 1)]
        } else {
            Vec::new()
        };
        assert_eq!(stores(), want, "turn {turn} status {status}");
        let seen = SAFEPOINTS.with(|s| s.borrow().clone());
        assert_eq!(!seen.is_empty(), polled);
        assert_eq!((f[8], f[9]), (0x88, 0x99));
    }
}

#[test]
fn add_buff_value_adds_in_range_and_keeps_the_register_residue() {
    clear();
    let g = Graph::new(0, 0, 4);
    g.put32(8 * 60 + 0x20 + 4 * 3, 1000);
    let mut f = file(&g);
    f[1] = 0xdead_0000_0000_0003;
    f[2] = 0xffff_ffff_ffff_fffe; // -2 as a 32-bit int
    let mut frame = Frame::new(regs(&mut f));
    let out = unsafe { BattleCharacterParam::at(g.base()) }.add_buff_value(&mut frame);
    assert!(out.is_ok());
    let cell = g.base() + 8 * 60 + 0x20 + 12;
    assert_eq!(stores(), [(cell, 4, 998)]);
    assert!(calls().is_empty());
    assert_eq!(f[29], 0x29);
    assert_eq!(f[31], 0x1000);
    // x8 and x9 are not written on the normal path
    assert_eq!((f[8], f[9]), (0x88, 0x99));
}

#[test]
fn add_buff_value_raises_at_and_past_the_length() {
    for index in [4u64, 5, 0xffff_ffff, 0x8000_0000, 0xdead_0000_0000_0004] {
        clear();
        install(callee::INDEX_OUT_OF_RANGE, |r| r[0] = 0x55);
        let g = Graph::new(0, 0, 4);
        let mut f = file(&g);
        f[1] = index;
        let mut frame = Frame::new(regs(&mut f));
        let _ = unsafe { BattleCharacterParam::at(g.base()) }.add_buff_value(&mut frame);
        assert_eq!(calls(), [callee::INDEX_OUT_OF_RANGE], "index {index:#x}");
        assert!(stores().is_empty());
        assert_eq!(f[30], site::ADD_BUFF_RANGE);
        assert_eq!(f[31], 0x1000 - 16);
        assert_eq!(f[29], 0x1000 - 16);
        assert_eq!(f[9], 4);
    }
}

#[test]
fn bd6c0_returns_zero_while_the_turn_is_positive() {
    clear();
    let g = Graph::new(2, 0, 4);
    let mut f = file(&g);
    let mut r = regs(&mut f);
    unsafe { BattleCharacterParam::at(g.base()) }.bd6c0(&mut r);
    assert!(calls().is_empty());
    assert_eq!((f[0], f[2], f[8]), (0, 100, 0x88));
}

#[test]
fn bd6c0_tail_calls_with_the_status_manager() {
    for turn in [0, -1] {
        clear();
        install(callee::E30240, |r| r[3] = r[0]);
        let g = Graph::new(turn, 0, 4);
        let mut f = file(&g);
        let mut r = regs(&mut f);
        unsafe { BattleCharacterParam::at(g.base()) }.bd6c0(&mut r);
        assert_eq!(calls(), [callee::E30240]);
        assert_eq!((f[2], f[8]), (0, u64::from(turn as u32)));
        assert_eq!(f[3], g.base() + 8 * 30);
    }
}

#[test]
fn the_loader_tails_set_their_registers_then_call() {
    clear();
    install(callee::BE300, |r| r[3] = r[0]);
    let g = Graph::new(0, 0, 4);
    let mut f = file(&g);
    buff_tail(&mut regs(&mut f));
    assert_eq!((f[0], f[3]), (g.buff(), g.buff()));

    clear();
    install(callee::C060B0, |r| r[4] = 1);
    let g = Graph::new(0, 0, 4);
    let object = g.base() + 8 * 80;
    g.put64(0x30 / 8, object);
    g.put64(80 + 2, 0x1_2345_6789);
    let mut f = file(&g);
    f[1] = 0x9_8765_4321;
    field_30_tail(&mut regs(&mut f));
    assert_eq!(calls(), [callee::C060B0]);
    assert_eq!(
        (f[0], f[1], f[2], f[3]),
        (object, 0x2345_6789, 0x8765_4321, 0)
    );

    clear();
    install(callee::C68D0, |r| r[4] = 1);
    let g = Graph::new(0, 0, 4);
    g.put64(90 + 2, 0xaaaa);
    g.put64(100 + 2, 0xbbbb);
    let mut f = file(&g);
    f[1] = g.base() + 8 * 90;
    f[2] = g.base() + 8 * 100;
    pair_tail(&mut regs(&mut f));
    assert_eq!((f[0], f[1], f[2]), (g.base(), 0xaaaa, 0xbbbb));
    assert_eq!(calls(), [callee::C68D0]);
}

#[test]
fn bf040_calls_copies_the_half_and_tail_calls() {
    clear();
    install(callee::C06250, |r| r[7] = 0x66);
    install(callee::E22A40, |r| r[5] = 5);
    let g = Graph::new(0, 0, 4);
    let mut f = file(&g);
    let mut frame = Frame::new(regs(&mut f));
    let _ = unsafe { BattleCharacterParam::at(g.base()) }.bf040(&mut frame);
    assert_eq!(calls(), [callee::C06250, callee::E22A40]);
    assert_eq!(stores(), [(g.buff() + 0x10, 2, 0x1234)]);
    assert_eq!(f[0], g.base() + 8 * 30);
    assert_eq!(
        (f[1], f[8], f[9], f[19], f[29], f[30], f[31]),
        (0, g.buff(), 0x1234, 0x19, 0x29, 0xabc, 0x1000)
    );
}
