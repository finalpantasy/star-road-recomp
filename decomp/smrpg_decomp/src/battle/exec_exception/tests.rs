extern crate std;
use super::*;
use crate::bridge::tests::{CALLS, PRE, STORES, clear, install};
use std::boxed::Box;
use std::cell::Cell;
use std::vec::Vec;

fn pre(address: u64, size: u32, value: u64) {
    PRE.with(|p| p.borrow_mut().push((address, size, value)));
}

fn put32(words: &mut [u64; 256], byte: usize, value: u32) {
    unsafe {
        words
            .as_mut_ptr()
            .cast::<u8>()
            .add(byte)
            .cast::<u32>()
            .write_unaligned(value);
    }
}

fn base(words: &[u64; 256]) -> u64 {
    words.as_ptr() as u64
}

fn calls() -> Vec<u64> {
    CALLS.with(|c| c.borrow().clone())
}

std::thread_local! {
    static GW_VALUE: Cell<u64> = const { Cell::new(0) };
    static CHECKER: Cell<u64> = const { Cell::new(0) };
    static RANGE: Cell<u64> = const { Cell::new(0) };
}

/// Words: executer 0, acter 10, skill object 16, class cell 22, second class cell 23, target 30, logger 60,
/// class 90, second class 125, statics 160, instance 165, object 180, checker 215, static flag byte 234.
struct Setup {
    skill_id: u32,
    calc_type: u32,
    acter_id: u32,
    target_id: u32,
    state: u32,
    tmp: u32,
}

fn graph(s: &Setup) -> Box<[u64; 256]> {
    let mut w = Box::new([0u64; 256]);
    let b = base(&w);
    w[2] = 0xdead;
    w[4] = b + 8 * 10;
    w[5] = b + 8 * 30;
    w[6] = b + 8 * 16;
    put32(&mut w, 0x40, s.tmp);
    put32(&mut w, 8 * 10 + 0x20, s.acter_id);
    put32(&mut w, 8 * 16 + 0x14, s.skill_id);
    put32(&mut w, 8 * 16 + 0x24, s.calc_type);
    put32(&mut w, 8 * 16 + 0x28, 7);
    put32(&mut w, 8 * 30 + 0x20, s.target_id);
    w[30 + 0xb8 / 8] = b + 8 * 60;
    w[22] = b + 8 * 90;
    w[23] = b + 8 * 125;
    put32(&mut w, 8 * 90 + 0xe0, 1);
    put32(&mut w, 8 * 125 + 0xe0, 1);
    w[90 + 0xb8 / 8] = b + 8 * 160;
    w[160 + 2] = b + 8 * 165;
    w[165 + 0x60 / 8] = b + 8 * 180;
    w[165 + 0x170 / 8] = b + 8 * 215;
    put32(&mut w, 8 * 180 + 0x24, s.state);
    w[215 + 0x68 / 8] = 0x4141;
    w[232] = 0x3232;
    w[233] = 0x3333;
    w[234] = 1;
    pre(cells::FLAG, 1, 1);
    pre(cells::CLASS_SLOT, 8, b + 8 * 22);
    pre(cells::GLOBAL_SLOT, 8, b + 8 * 23);
    pre(cells::STATIC_FLAG_POINTER, 8, b + 8 * 234);
    pre(cells::LITERAL_SLOT, 8, b + 8 * 232);
    pre(cells::DEBUG_SLOT, 8, b + 8 * 233);
    w
}

fn plain(skill_id: u32) -> Setup {
    Setup {
        skill_id,
        calc_type: 12,
        acter_id: 0x657,
        target_id: 0,
        state: 2,
        tmp: 100,
    }
}

fn install_all() {
    install(callee::INITIALIZE_METADATA, |r| r[0] = 0x77);
    install(callee::CLASS_INITIALIZER, |r| r[0] = 0x78);
    install(callee::GET_INST, |r| {
        assert_eq!(r[0], 0);
        r[0] = 0x6a;
    });
    install(callee::GW_GET, |r| {
        assert_eq!(r[1], 0);
        r[0] = GW_VALUE.with(Cell::get);
    });
    install(callee::GW_SET, |r| {
        let sum = (GW_VALUE.with(Cell::get) as u32 as i32).wrapping_add(1);
        let expected = if sum >= 250 { 250 } else { sum };
        assert_eq!(r[1] as u32, expected as u32);
        assert_eq!(r[2], 0);
    });
    install(callee::INT32_TO_STRING, |r| r[0] = 0x5555);
    install(callee::STRING_CONCAT_2, |r| {
        assert_eq!(r[1], 0x5555);
        r[0] = 0x6666;
    });
    install(callee::DEBUG_WRITE, |_| {});
    install(callee::CLASS_CALL, |r| {
        assert_eq!((r[1], r[2]), (0x5f3, 0));
    });
    install(callee::MANAGER_CHECK, |r| {
        assert_eq!(r[0], 0x4141);
        r[0] = CHECKER.with(Cell::get);
    });
    install(callee::IN_RANGE, |r| {
        assert_eq!((r[0], r[1], r[2], r[3]), (7, 1, 0x49, 0));
        r[0] = RANGE.with(Cell::get);
    });
    install(callee::TARGET_CALL_A, |_| {});
    install(callee::TARGET_CALL_B, |_| {});
}

fn run(graph: &[u64; 256]) -> [u64; 96] {
    let stack = Box::leak(Box::new([0u64; 32]));
    let mut file = [0u64; 96];
    file[0] = base(graph);
    file[22] = 0x22;
    file[29] = 0x29;
    file[31] = stack.as_ptr() as u64 + 8 * 28;
    let regs = unsafe { Regs::from_raw(file.as_mut_ptr()) };
    let mut frame = Frame::new(regs);
    let _ = unsafe { SkillExecter::at(file[0]) }.skill_calc_exception(&mut frame);
    file
}

fn stores() -> Vec<(u64, u32, u64)> {
    STORES.with(|s| s.borrow().clone())
}

#[test]
fn another_target_returns_after_the_manager_checks() {
    clear();
    let g = graph(&plain(5));
    install_all();
    let file = run(&g);
    assert_eq!(calls(), [callee::GET_INST]);
    assert_eq!((file[19], file[22], file[29]), (0, 0x22, 0x29));
    let zero_stores: Vec<_> = stores().into_iter().filter(|e| e.2 == 0).collect();
    assert_eq!(zero_stores.len(), 1);
}

#[test]
fn skill_0x25d_calls_the_class_helper_unless_the_acter_is_0x657() {
    for (acter, helper) in [(0x657, false), (0x658, true), (0, true)] {
        clear();
        let mut s = plain(0x25d);
        s.acter_id = acter;
        let g = graph(&s);
        install_all();
        let _ = run(&g);
        let expected: Vec<u64> = if helper {
            std::vec![callee::GET_INST, callee::CLASS_CALL]
        } else {
            std::vec![callee::GET_INST]
        };
        assert_eq!(calls(), expected, "acter {acter:#x}");
    }
}

#[test]
fn skill_0x67_adds_half_of_the_counter_to_the_tmp_value() {
    for (counter, tmp, expected) in [
        (10u32, 100u32, 105u32),
        (11, 100, 105),
        (0, 7, 7),
        (0xffff_fff1, 100, 93),
        (0xffff_ffff, 50, 50),
        (300, 1, 151),
    ] {
        clear();
        GW_VALUE.with(|v| v.set(u64::from(counter)));
        let mut s = plain(0x67);
        s.tmp = tmp;
        let g = graph(&s);
        install_all();
        let _ = run(&g);
        let tmp_stores: Vec<_> = stores()
            .into_iter()
            .filter(|e| e.0 == base(&g) + 0x40)
            .collect();
        assert_eq!(
            tmp_stores,
            [(base(&g) + 0x40, 4, u64::from(expected))],
            "counter {counter:#x}"
        );
        assert_eq!(
            calls(),
            [
                callee::GET_INST,
                callee::GET_INST,
                callee::GET_INST,
                callee::GW_GET,
                callee::GW_SET,
                callee::GET_INST,
                callee::GW_GET,
                callee::GET_INST,
                callee::GW_GET,
                callee::INT32_TO_STRING,
                callee::STRING_CONCAT_2,
                callee::DEBUG_WRITE
            ]
        );
    }
}

fn exception_setup() -> Setup {
    Setup {
        skill_id: 5,
        calc_type: 12,
        acter_id: 0,
        target_id: 0x5ed,
        state: 2,
        tmp: 0,
    }
}

#[test]
fn the_exception_sets_both_flags_and_makes_two_target_calls() {
    clear();
    CHECKER.with(|c| c.set(0));
    RANGE.with(|c| c.set(1));
    let g = graph(&exception_setup());
    install_all();
    let _ = run(&g);
    assert_eq!(
        calls(),
        [
            callee::GET_INST,
            callee::MANAGER_CHECK,
            callee::IN_RANGE,
            callee::DEBUG_WRITE,
            callee::TARGET_CALL_A,
            callee::TARGET_CALL_B
        ]
    );
    let flags: Vec<_> = stores().into_iter().filter(|e| e.1 == 1).collect();
    assert_eq!(
        flags,
        [
            (base(&g) + 8 * 180 + 0x20, 1, 1),
            (base(&g) + 8 * 60 + 0x41, 1, 1)
        ]
    );
}

#[test]
fn each_failed_test_leaves_before_the_flags() {
    // (checker result, range result, manager state, calc type)
    for (checker, range, state, calc_type) in [
        (1u64, 1u64, 2u32, 12u32),
        (0, 0, 2, 12),
        (0, 1, 3, 12),
        (0, 1, 2, 7),
        (0, 1, 2, 0),
    ] {
        clear();
        CHECKER.with(|c| c.set(checker));
        RANGE.with(|c| c.set(range));
        let mut s = exception_setup();
        s.state = state;
        s.calc_type = calc_type;
        let g = graph(&s);
        install_all();
        let _ = run(&g);
        assert!(!calls().contains(&callee::TARGET_CALL_A));
        assert!(stores().iter().all(|e| e.1 != 1));
    }
}

#[test]
fn first_run_resolves_four_slots_and_stores_the_flag() {
    clear();
    let g = graph(&plain(5));
    PRE.with(|p| p.borrow_mut().retain(|e| e.0 != cells::FLAG));
    pre(cells::FLAG, 1, 0);
    install_all();
    let _ = run(&g);
    assert_eq!(calls()[..4], [callee::INITIALIZE_METADATA; 4]);
    let flag_stores: Vec<_> = stores()
        .into_iter()
        .filter(|e| e.0 == cells::FLAG)
        .collect();
    assert_eq!(flag_stores, [(cells::FLAG, 1, 1)]);
}
