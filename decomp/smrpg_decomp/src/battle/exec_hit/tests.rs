extern crate std;
use super::*;
use crate::bridge::tests::{CALLS, OUT_FROM_X0, PRE, STORES, clear, install};
use std::boxed::Box;
use std::cell::{Cell, RefCell};
use std::vec::Vec;

fn pre(address: u64, size: u32, value: u64) {
    PRE.with(|p| p.borrow_mut().push((address, size, value)));
}

fn put32(words: &mut [u64; 512], byte: usize, value: u32) {
    unsafe {
        words
            .as_mut_ptr()
            .cast::<u8>()
            .add(byte)
            .cast::<u32>()
            .write_unaligned(value);
    }
}

fn put8(words: &mut [u64; 512], byte: usize, value: u8) {
    unsafe { words.as_mut_ptr().cast::<u8>().add(byte).write(value) };
}

fn base(words: &[u64; 512]) -> u64 {
    words.as_ptr() as u64
}

fn calls() -> Vec<u64> {
    CALLS.with(|c| c.borrow().clone())
}

std::thread_local! {
    static BASE: Cell<u64> = const { Cell::new(0) };
    static ROLL: Cell<u64> = const { Cell::new(0) };
    static RANDOM_ARGS: RefCell<Vec<(u32, u32)>> = const { RefCell::new(Vec::new()) };
    static FLOAT_ARGS: RefCell<Vec<u32>> = const { RefCell::new(Vec::new()) };
}

/// Words: executer 0, skill object 350, target 30, logger 60, BattleManager cell 22 and class 90, statics 160,
/// instance 300, object 180, GlobalWork cell 23 and class 125, get_inst object 400, singleton object 430,
/// static flag byte 460, literal cells 470 to 478.
struct Setup {
    skill_id: u32,
    rate: u32,
    counter: u32,
    target_id: u32,
    no_modifier: u8,
    modifier: u32,
    state: u32,
    object_flag: u8,
    global_flag: u8,
    singleton_flag: u8,
    mode: u32,
}

fn plain(skill_id: u32) -> Setup {
    Setup {
        skill_id,
        rate: 55,
        counter: 5,
        target_id: 0,
        no_modifier: 1,
        modifier: 0,
        state: 0,
        object_flag: 0,
        global_flag: 0,
        singleton_flag: 0,
        mode: 0,
    }
}

fn graph(s: &Setup) -> Box<[u64; 512]> {
    let mut w = Box::new([0u64; 512]);
    let b = base(&w);
    BASE.with(|c| c.set(b));
    w[2] = 0xdead;
    w[5] = b + 8 * 30;
    w[6] = b + 8 * 350;
    put32(&mut w, 8 * 350 + 0x14, s.skill_id);
    put32(&mut w, 8 * 350 + 0x34, s.rate);
    put32(&mut w, 8 * 30 + 0x20, s.target_id);
    put32(&mut w, 8 * 30 + 0x8c, s.counter);
    put8(&mut w, 8 * 30 + 0x6c, s.no_modifier);
    put32(&mut w, 8 * 30 + 0x58, s.modifier);
    w[30 + 0xb8 / 8] = b + 8 * 60;
    w[22] = b + 8 * 90;
    w[23] = b + 8 * 125;
    put32(&mut w, 8 * 90 + 0xe0, 1);
    put32(&mut w, 8 * 125 + 0xe0, 1);
    w[90 + 0xb8 / 8] = b + 8 * 160;
    w[160 + 2] = b + 8 * 300;
    w[300 + 0x60 / 8] = b + 8 * 180;
    put32(&mut w, 8 * 300 + 0x168, s.state);
    put8(&mut w, 8 * 180 + 0x30, s.object_flag);
    put8(&mut w, 8 * 400 + 0x61, s.global_flag);
    put8(&mut w, 8 * 430 + 0x5c, s.singleton_flag);
    put32(&mut w, 8 * 430 + 0x68, s.mode);
    w[460] = 1;
    pre(cells::FLAG, 1, 1);
    pre(cells::CLASS_SLOT, 8, b + 8 * 22);
    pre(cells::GLOBAL_SLOT, 8, b + 8 * 23);
    pre(cells::STATIC_FLAG_POINTER, 8, b + 8 * 460);
    for (i, slot) in cells::LITERALS.iter().enumerate() {
        w[470 + i] = 0x1000 + i as u64;
        pre(*slot, 8, b + 8 * (470 + i as u64));
    }
    w
}

fn install_all() {
    OUT_FROM_X0.with(|c| c.set(true));
    ROLL.with(|c| c.set(0));
    RANDOM_ARGS.with(|c| c.borrow_mut().clear());
    FLOAT_ARGS.with(|c| c.borrow_mut().clear());
    install(callee::INITIALIZE_METADATA, |r| r[0] = 0x77);
    install(callee::CLASS_INITIALIZER, |r| r[0] = 0x78);
    install(callee::GET_INST, |r| {
        assert_eq!(r[0], 0);
        r[0] = BASE.with(Cell::get) + 8 * 400;
    });
    install(callee::SINGLETON, |r| {
        assert_eq!(r[0], 0);
        r[0] = BASE.with(Cell::get) + 8 * 430;
    });
    install(callee::IN_RANGE, |r| {
        assert_eq!((r[1], r[2], r[3]), (0x2c3, 0x2c5, 0));
        r[0] = u64::from((0x2c3..=0x2c5).contains(&(r[0] as u32)));
    });
    install(callee::FLOAT_TO_STRING, |r| {
        assert_eq!(r[1], 0);
        r[0] = 0x5555;
    });
    install(callee::STRING_CONCAT_2, |r| {
        assert_eq!(r[1], 0x5555);
        r[0] = 0x6666;
    });
    install(callee::DEBUG_WRITE, |_| {});
    install(callee::DEBUG_FLOAT, |r| {
        FLOAT_ARGS.with(|c| c.borrow_mut().push(r[32] as u32));
    });
    install(callee::RANDOM_CHECK, |r| {
        RANDOM_ARGS.with(|c| c.borrow_mut().push((r[32] as u32, r[34] as u32)));
        r[0] = ROLL.with(Cell::get);
    });
}

fn run(graph: &[u64; 512]) -> [u64; 100] {
    let stack = Box::leak(Box::new([0u64; 32]));
    let mut file = [0u64; 100];
    file[0] = base(graph);
    file[19] = 0x19;
    file[20] = 0x20;
    file[21] = 0x21;
    file[29] = 0x29;
    file[31] = stack.as_ptr() as u64 + 8 * 28;
    let regs = unsafe { Regs::from_raw(file.as_mut_ptr()) };
    let mut frame = Frame::new(regs);
    let _ = unsafe { SkillExecter::at(file[0]) }.is_hit_calc_succeed(&mut frame);
    file
}

fn stores() -> Vec<(u64, u32, u64)> {
    STORES.with(|s| s.borrow().clone())
}

fn random_args() -> Vec<(u32, u32)> {
    RANDOM_ARGS.with(|c| c.borrow().clone())
}

const HUNDRED: u32 = 0x42c8_0000;

fn miss_flag_stores(g: &[u64; 512]) -> usize {
    let flag = base(g) + 8 * 60 + 0x40;
    stores().iter().filter(|e| e.0 == flag && e.2 == 1).count()
}

#[test]
fn the_registers_come_back_and_x0_is_the_verdict() {
    clear();
    let mut s = plain(5);
    s.global_flag = 1;
    let g = graph(&s);
    install_all();
    let file = run(&g);
    assert_eq!(file[0], 1);
    assert_eq!(
        (file[19], file[20], file[21], file[29]),
        (0x19, 0x20, 0x21, 0x29)
    );
}

#[test]
fn a_counter_below_one_misses_for_the_three_range_skills_only() {
    for (skill, counter, miss) in [
        (0x2c3, 0, true),
        (0x2c4, 0xffff_ffff, true),
        (0x2c5, 0, true),
        (0x2c3, 1, false),
        (0x2c2, 0, false),
        (0x2c6, 0, false),
    ] {
        clear();
        let mut s = plain(skill);
        s.counter = counter;
        s.global_flag = 1;
        let g = graph(&s);
        install_all();
        let file = run(&g);
        if miss {
            assert_eq!(calls(), [callee::IN_RANGE], "skill {skill:#x}");
            assert_eq!(file[0], 0);
        } else {
            assert_eq!(calls(), [callee::IN_RANGE, callee::GET_INST]);
            assert_eq!(file[0], 1);
        }
    }
}

#[test]
fn the_global_flag_and_the_singleton_flag_are_sure_hits() {
    clear();
    let mut s = plain(5);
    s.singleton_flag = 0x80;
    let g = graph(&s);
    install_all();
    let file = run(&g);
    assert_eq!(
        calls(),
        [callee::IN_RANGE, callee::GET_INST, callee::SINGLETON]
    );
    assert_eq!(file[0], 1);
}

#[test]
fn state_0x7db_with_the_object_flag_makes_skill_0x2ba_a_sure_hit() {
    for (skill, flag, state, sure) in [
        (0x2ba, 1u8, 0x7db, true),
        (0x2bb, 1, 0x7db, false),
        (0x2ba, 0, 0x7db, false),
        (0x2ba, 1, 0x7dc, false),
    ] {
        clear();
        let mut s = plain(skill);
        s.object_flag = flag;
        s.state = state;
        s.mode = 1;
        let g = graph(&s);
        install_all();
        let file = run(&g);
        let rolled = calls().contains(&callee::FLOAT_TO_STRING);
        assert_eq!(
            rolled, !sure,
            "skill {skill:#x} flag {flag} state {state:#x}"
        );
        assert_eq!(file[0], 1);
    }
}

#[test]
fn skill_1_without_a_modifier_flag_scales_the_rate_by_the_target_modifier() {
    for (modifier, expected_rate, rolls) in [
        (0.0f32, 100.0f32, false),
        (40.0, 80.0, true),
        (-40.0, 120.0, false),
        (f32::INFINITY, f32::NEG_INFINITY, true),
    ] {
        clear();
        let mut s = plain(1);
        s.no_modifier = 0;
        s.modifier = modifier.to_bits();
        let g = graph(&s);
        install_all();
        ROLL.with(|c| c.set(1));
        let file = run(&g);
        if rolls {
            assert_eq!(random_args(), [(expected_rate.to_bits(), HUNDRED)]);
            assert_eq!(file[0], 1);
        } else {
            assert!(random_args().is_empty());
            assert_eq!(file[0], 1);
        }
    }
}

#[test]
fn the_rate_is_the_skill_integer_converted_to_float() {
    for (raw, expected) in [
        (55u32, 55.0f32),
        (0, 0.0),
        (0xffff_ffff, -1.0),
        (99, 99.0),
        (0x8000_0000, -2147483648.0),
    ] {
        clear();
        let mut s = plain(5);
        s.rate = raw;
        let g = graph(&s);
        install_all();
        let file = run(&g);
        assert_eq!(
            random_args(),
            [(expected.to_bits(), HUNDRED)],
            "raw {raw:#x}"
        );
        assert_eq!(file[0], 0);
        assert_eq!(miss_flag_stores(&g), 1);
    }
}

#[test]
fn a_large_rate_is_a_hit_and_an_inexact_conversion_sets_the_flag() {
    clear();
    let mut s = plain(5);
    s.rate = 0x7fff_ffff;
    let g = graph(&s);
    install_all();
    let file = run(&g);
    assert_eq!(file[0], 1);
    assert!(random_args().is_empty());
    // fpsr is the low half of word 98: IXC (bit 4) from the rounding of 2^31 - 1 to single
    assert_eq!(file[98] & 0x10, 0x10);
    // the exact one raises nothing
    clear();
    let mut s = plain(5);
    s.rate = 200;
    let g = graph(&s);
    install_all();
    let file = run(&g);
    assert_eq!(file[0], 1);
    assert_eq!(file[98] & 0x9f, 0);
}

#[test]
fn modes_1_and_2_force_the_rate_to_100_and_0() {
    for (mode, rolls, verdict) in [
        (1u32, false, 1u64),
        (2, true, 0),
        (3, true, 0),
        (0, true, 0),
    ] {
        clear();
        let mut s = plain(5);
        s.mode = mode;
        let g = graph(&s);
        install_all();
        let file = run(&g);
        assert_eq!(file[0], verdict, "mode {mode}");
        let forced = FLOAT_ARGS.with(|c| c.borrow().clone());
        match mode {
            1 => assert_eq!(forced, [HUNDRED]),
            2 => assert_eq!(forced, [0]),
            _ => assert!(forced.is_empty()),
        }
        if rolls {
            let expected = if mode == 2 { 0 } else { 55.0f32.to_bits() };
            assert_eq!(random_args(), [(expected, HUNDRED)]);
        } else {
            assert!(random_args().is_empty());
        }
    }
}

#[test]
fn a_nan_rate_is_not_100_or_more_and_rolls() {
    clear();
    let mut s = plain(1);
    s.no_modifier = 0;
    s.modifier = 0x7fc0_1234;
    let g = graph(&s);
    install_all();
    let file = run(&g);
    let args = random_args();
    assert_eq!(args.len(), 1);
    assert!(f32::from_bits(args[0].0).is_nan());
    assert_eq!(file[0], 0);
    // a signaling NaN raises IOC (bit 0)
    clear();
    let mut s = plain(1);
    s.no_modifier = 0;
    s.modifier = 0x7f80_0001;
    let g = graph(&s);
    install_all();
    let file = run(&g);
    assert_eq!(file[98] & 1, 1);
}

#[test]
fn the_roll_decides_and_a_miss_sets_the_flag_in_the_result_logger() {
    for roll in [0u64, 1, 2, 3, 0x1_0000_0001] {
        clear();
        let g = graph(&plain(5));
        install_all();
        ROLL.with(|c| c.set(roll));
        let file = run(&g);
        assert_eq!(file[0], roll & 1, "roll {roll:#x}");
        assert_eq!(miss_flag_stores(&g), usize::from(roll & 1 == 0));
    }
}

#[test]
fn skill_1_against_target_0x434_always_misses() {
    for (target, skill, roll, verdict) in [
        (0x434, 1, 1, 0u64),
        (0x434, 5, 1, 1),
        (0x433, 1, 1, 1),
        (0x434, 1, 0, 0),
    ] {
        clear();
        let mut s = plain(skill);
        s.target_id = target;
        s.rate = 50;
        s.no_modifier = 0;
        s.modifier = 40.0f32.to_bits();
        let g = graph(&s);
        install_all();
        ROLL.with(|c| c.set(roll));
        let file = run(&g);
        assert_eq!(file[0], verdict, "target {target:#x} skill {skill}");
        assert_eq!(miss_flag_stores(&g), usize::from(verdict == 0));
    }
}

#[test]
fn the_first_run_resolves_eleven_metadata_slots_and_sets_the_flag() {
    clear();
    let mut s = plain(5);
    s.global_flag = 1;
    let g = graph(&s);
    install_all();
    PRE.with(|p| p.borrow_mut().retain(|e| e.0 != cells::FLAG));
    pre(cells::FLAG, 1, 0);
    let file = run(&g);
    let all = calls();
    assert_eq!(
        all.iter()
            .filter(|c| **c == callee::INITIALIZE_METADATA)
            .count(),
        11
    );
    assert!(stores().contains(&(cells::FLAG, 1, 1)));
    assert_eq!(file[0], 1);
}

#[test]
fn a_zero_class_word_runs_the_class_initializer_before_the_flag_test() {
    clear();
    let g = graph(&plain(5));
    install_all();
    // the BattleManager class word reads zero first (pre-state), then nonzero (live)
    pre(base(&g) + 8 * 90 + 0xe0, 4, 0);
    let _ = run(&g);
    assert!(calls().contains(&callee::CLASS_INITIALIZER));
}
