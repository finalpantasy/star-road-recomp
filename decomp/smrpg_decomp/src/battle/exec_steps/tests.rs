extern crate std;
use super::*;
use crate::bridge::tests::{CALLS, Fake, PRE, STORES, clear, install};
use std::boxed::Box;
use std::vec::Vec;

fn pre(address: u64, size: u32, value: u64) {
    PRE.with(|p| p.borrow_mut().push((address, size, value)));
}

fn words() -> Box<[u64; 128]> {
    Box::new([0u64; 128])
}

fn put32(words: &mut [u64; 128], byte: usize, value: u32) {
    unsafe {
        words
            .as_mut_ptr()
            .cast::<u8>()
            .add(byte)
            .cast::<u32>()
            .write_unaligned(value);
    }
}

fn base(words: &[u64; 128]) -> u64 {
    words.as_ptr() as u64
}

fn file(this: u64) -> [u64; 96] {
    let mut file = [0u64; 96];
    file[0] = this;
    file[1] = 0x11;
    file[8] = 0x88;
    file[19] = 0x19;
    file[20] = 0x20;
    file[21] = 0x21;
    file[22] = 0x22;
    file[29] = 0x29;
    file[30] = 0xabc;
    file[31] = 0x1000;
    file
}

fn calls() -> Vec<u64> {
    CALLS.with(|c| c.borrow().clone())
}

// ---- got_physics_attack --------------------------------------------------------------------------------------

/// Target at word 0 (+0x98 manager, +0xb8 logger), manager at word 32 (+0x10 status), logger at word 48, the literal cell
/// at word 64, the write cell at word 72.
fn physics_graph(status: u32) -> Box<[u64; 128]> {
    let mut w = words();
    let b = base(&w);
    w[0x98 / 8] = b + 8 * 32;
    w[0xb8 / 8] = b + 8 * 48;
    put32(&mut w, 8 * 32 + 0x10, status);
    w[64] = 0x5151;
    w[72] = 0x7272;
    pre(physics::FLAG, 1, 1);
    pre(physics::SLOT_B, 8, b + 8 * 64);
    pre(physics::SLOT_A, 8, b + 8 * 72);
    w
}

fn install_physics() {
    install(callee::LOGGER_SET_60, |r| {
        assert_eq!((r[1], r[2], r[30]), (1, 0, physics::site::LOGGER_SET));
        r[0] = 0x600;
    });
    install(callee::STRING_LITERAL, |r| {
        assert_eq!((r[0], r[1], r[30]), (0x5151, 0, physics::site::LITERAL));
        r[0] = 0x1234;
    });
    install(callee::DEBUG_ENTER, |r| {
        assert_eq!(
            (r[1], r[19], r[30]),
            (0, 0x1234, physics::site::DEBUG_ENTER)
        );
        r[0] = 0x99;
    });
    install(callee::DEBUG_WRITE, |r| {
        assert_eq!((r[0], r[1], r[2]), (0x1234, 0x7272, 0));
        // the tail call: the frame is popped, x30 is the entry value
        assert_eq!(
            (r[19], r[20], r[21], r[29], r[30], r[31]),
            (0x19, 0x20, 0x21, 0x29, 0xabc, 0x1000)
        );
    });
    install(callee::INITIALIZE_METADATA, |r| r[0] = 0x77);
}

fn run_physics(graph: &[u64; 128]) -> [u64; 96] {
    let mut file = file(base(graph));
    let regs = unsafe { Regs::from_raw(file.as_mut_ptr()) };
    let mut frame = Frame::new(regs);
    let _ = unsafe { BattleCharacterParam::at(file[0]) }.got_physics_attack(&mut frame);
    file
}

#[test]
fn physics_with_status_3_makes_the_four_calls_and_tail_calls() {
    clear();
    let graph = physics_graph(3);
    install_physics();
    let file = run_physics(&graph);
    assert_eq!(
        calls(),
        [
            callee::LOGGER_SET_60,
            callee::STRING_LITERAL,
            callee::DEBUG_ENTER,
            callee::DEBUG_WRITE
        ]
    );
    assert_eq!(file[31], 0x1000);
    assert!(STORES.with(|s| s.borrow().is_empty()));
}

#[test]
fn physics_with_another_status_returns_with_the_saved_registers_only() {
    for status in [0, 2, 4, 0xffff_ffff, 0x1_0003] {
        clear();
        let graph = physics_graph(status);
        install_physics();
        let file = run_physics(&graph);
        assert!(calls().is_empty(), "status {status:#x}");
        // x19 to x21 and x29 come back; x0, x1, x8 and x30 stay the entry values
        assert_eq!(
            (
                file[0], file[1], file[8], file[19], file[20], file[21], file[29], file[30],
                file[31]
            ),
            (
                base(&graph),
                0x11,
                0x88,
                0x19,
                0x20,
                0x21,
                0x29,
                0xabc,
                0x1000
            )
        );
    }
}

#[test]
fn physics_first_run_resolves_both_slots_and_stores_the_flag() {
    clear();
    let graph = physics_graph(3);
    PRE.with(|p| p.borrow_mut().retain(|e| e.0 != physics::FLAG));
    pre(physics::FLAG, 1, 0);
    install_physics();
    let _ = run_physics(&graph);
    let list = calls();
    assert_eq!(
        &list[..3],
        [
            callee::INITIALIZE_METADATA,
            callee::INITIALIZE_METADATA,
            callee::LOGGER_SET_60
        ]
    );
    assert_eq!(STORES.with(|s| s.borrow().clone()), [(physics::FLAG, 1, 1)]);
}

// ---- resister_thank_you_message --------------------------------------------------------------------------------

/// Executer at word 0 (+0x20 acter, +0x28 target, +0x30 skill object), skill object at word 16 (+0x3c type), class cell at
/// word 32 (holds the class), class at word 40 (+0xb8 statics, +0xe0 word), statics at word 60 (+0x10 instance), instance at
/// word 64 (+0x48 object), object at word 80 (+0x38 word), the static flag byte at word 90.
fn thanks_graph(skill_type: u32, class_word: u32, static_flag: u8, bits: u32) -> Box<[u64; 128]> {
    let mut w = words();
    let b = base(&w);
    w[4] = 0xa0;
    w[5] = 0xb0;
    w[6] = b + 8 * 16;
    put32(&mut w, 8 * 16 + 0x3c, skill_type);
    w[32] = b + 8 * 40;
    w[40 + 0xb8 / 8] = b + 8 * 60;
    put32(&mut w, 8 * 40 + 0xe0, class_word);
    w[60 + 2] = b + 8 * 64;
    w[64 + 9] = b + 8 * 80;
    put32(&mut w, 8 * 80 + 0x38, bits);
    unsafe { w.as_mut_ptr().cast::<u8>().add(8 * 90).write(static_flag) };
    pre(thanks::FLAG, 1, 1);
    pre(thanks::CLASS_SLOT, 8, b + 8 * 32);
    pre(thanks::STATIC_FLAG_POINTER, 8, b + 8 * 90);
    w
}

fn install_thanks(check: u64) {
    clear_installed();
    let body: Fake = if check == 1 {
        |r| r[0] = 1
    } else {
        |r| r[0] = 2
    };
    install(callee::ACTER_TARGET_CHECK, body);
    install(callee::INITIALIZE_METADATA, |r| r[0] = 0x77);
    install(callee::CLASS_INITIALIZER, |r| r[0] = 0x78);
}

fn clear_installed() {}

fn run_thanks(graph: &[u64; 128]) -> [u64; 96] {
    let mut file = file(base(graph));
    let regs = unsafe { Regs::from_raw(file.as_mut_ptr()) };
    let mut frame = Frame::new(regs);
    let _ = unsafe { SkillExecter::at(file[0]) }.resister_thank_you_message(&mut frame);
    file
}

#[test]
fn thanks_for_another_skill_type_returns_at_once() {
    for skill_type in [0, 2, 4, 0x1_0003, 0xffff_ffff] {
        clear();
        let graph = thanks_graph(skill_type, 1, 1, 0);
        install_thanks(1);
        let file = run_thanks(&graph);
        assert!(calls().is_empty());
        assert!(STORES.with(|s| s.borrow().is_empty()));
        assert_eq!(
            (file[0], file[19], file[20], file[29], file[31]),
            (base(&graph), 0x19, 0x20, 0x29, 0x1000)
        );
        assert_eq!(file[30], 0xabc);
    }
}

#[test]
fn thanks_with_a_failed_check_makes_one_call() {
    clear();
    let graph = thanks_graph(3, 1, 1, 0);
    install_thanks(2);
    let file = run_thanks(&graph);
    assert_eq!(calls(), [callee::ACTER_TARGET_CHECK]);
    assert_eq!(file[0], 2);
    assert!(STORES.with(|s| s.borrow().is_empty()));
}

#[test]
fn thanks_sets_bit_two_when_everything_is_initialized() {
    for bits in [0u32, 1, 2, 0x8000_0000, 0xffff_ffff] {
        clear();
        let graph = thanks_graph(3, 1, 1, bits);
        install_thanks(1);
        let _ = run_thanks(&graph);
        assert_eq!(calls(), [callee::ACTER_TARGET_CHECK]);
        let object = base(&graph) + 8 * 80 + 0x38;
        assert_eq!(
            STORES.with(|s| s.borrow().clone()),
            [(object, 4, u64::from(bits | 2))]
        );
    }
}

#[test]
fn thanks_resolves_the_static_metadata_once_when_the_flag_byte_is_clear() {
    clear();
    let graph = thanks_graph(3, 1, 0, 4);
    install_thanks(1);
    let file = run_thanks(&graph);
    let flag = base(&graph) + 8 * 90;
    assert_eq!(
        calls(),
        [callee::ACTER_TARGET_CHECK, callee::INITIALIZE_METADATA]
    );
    assert_eq!(
        STORES.with(|s| s.borrow().clone()),
        [(flag, 1, 1), (base(&graph) + 8 * 80 + 0x38, 4, 6)]
    );
    assert_eq!(
        crate::bridge::tests::SAFEPOINTS.with(|s| s.borrow().clone()),
        [thanks::site::METADATA_POLL]
    );
    assert_eq!(file[31], 0x1000);
}

#[test]
fn thanks_runs_the_class_initializer_while_the_word_is_zero() {
    clear();
    let graph = thanks_graph(3, 0, 1, 0);
    // the first read is the pre-state (0); the initializer (a fake) then drops that entry and sets the word
    let word = base(&graph) + 8 * 40 + 0xe0;
    pre(word, 4, 0);
    install(callee::CLASS_INITIALIZER, |r| {
        PRE.with(|p| p.borrow_mut().retain(|e| e.1 != 4));
        r[0] = 0x78;
    });
    install_thanks(1);
    // after the initializer the live word must be non-zero: put it into the graph
    unsafe { *(word as *mut u32) = 1 };
    let _ = run_thanks(&graph);
    assert_eq!(
        calls(),
        [callee::ACTER_TARGET_CHECK, callee::CLASS_INITIALIZER]
    );
    assert_eq!(
        crate::bridge::tests::SAFEPOINTS.with(|s| s.borrow().clone()),
        [thanks::site::CLASS_INIT_FIRST_POLL]
    );
}

// ---- heal_calc -----------------------------------------------------------------------------------------------------

/// Executer at word 0 (+0x10 monitor, +0x18 result record, +0x20 acter, +0x28 target, +0x30 skill object, +0x40 tmp value),
/// target at word 16 (+0xb8 logger), logger at word 32, skill object at word 48, acter at word 56, result record at word 64,
/// class cell at word 80, class at word 84 (+0xe0 word), the string cells at words 100 and 104.
fn heal_graph(tmp: u32, skill_id: i32, class_word: u32) -> Box<[u64; 128]> {
    let mut w = words();
    let b = base(&w);
    w[2] = 0xdead;
    w[3] = b + 8 * 64;
    w[4] = b + 8 * 56;
    w[5] = b + 8 * 16;
    w[6] = b + 8 * 48;
    put32(&mut w, 0x40, tmp);
    w[16 + 0xb8 / 8] = b + 8 * 32;
    put32(&mut w, 8 * 48 + 0x14, skill_id as u32);
    w[80] = b + 8 * 84;
    put32(&mut w, 8 * 84 + 0xe0, class_word);
    w[100] = 0x1010;
    w[104] = 0x2020;
    pre(heal::FLAG, 1, 1);
    pre(heal::CLASS_SLOT, 8, b + 8 * 80);
    pre(heal::STRING_SLOT_ENTER, 8, b + 8 * 100);
    pre(heal::STRING_SLOT_HEAL, 8, b + 8 * 104);
    w
}

fn install_heal() {
    install(callee::DEBUG_ENTER, |r| {
        assert_eq!((r[0], r[1]), (0xdead, 0));
    });
    install(callee::INITIALIZE_METADATA, |r| r[0] = 0x77);
    install(callee::CLASS_INITIALIZER, |r| r[0] = 0x78);
    install(callee::MATH_MAX, |r| {
        r[0] = u64::from((r[0] as i32).max(r[1] as i32) as u32);
    });
    install(callee::INT32_TO_STRING, |r| r[0] = 0x5555);
    install(callee::STRING_CONCAT_2, |r| {
        assert_eq!(r[1], 0x5555);
        r[0] = 0x6666;
    });
    install(callee::DEBUG_WRITE, |_| {});
}

fn run_heal(graph: &[u64; 128]) -> [u64; 96] {
    // the int cell lives on the stack and the out-call fake reads it, so the sp must be real memory
    let stack = Box::leak(Box::new([0u64; 32]));
    let mut file = file(base(graph));
    file[31] = stack.as_ptr() as u64 + 8 * 28;
    let regs = unsafe { Regs::from_raw(file.as_mut_ptr()) };
    let mut frame = Frame::new(regs);
    let _ = unsafe { SkillExecter::at(file[0]) }.heal_calc(&mut frame);
    file
}

fn logger_stores(graph: &[u64; 128]) -> Vec<(u64, u32, u64)> {
    let logger = base(graph) + 8 * 32;
    STORES.with(|s| {
        s.borrow()
            .iter()
            .copied()
            .filter(|e| e.0 == logger + 0x28 || e.0 == logger + 0x2c)
            .collect()
    })
}

#[test]
fn heal_is_the_larger_of_the_tmp_value_and_one() {
    for (tmp, expected) in [
        (0u32, 1u32),
        (1, 1),
        (7, 7),
        (0x7fff_ffff, 0x7fff_ffff),
        (0xffff_ffff, 1),
        (0x8000_0000, 1),
    ] {
        clear();
        let graph = heal_graph(tmp, 5, 1);
        install_heal();
        let _ = run_heal(&graph);
        let logger = base(&graph) + 8 * 32;
        assert_eq!(
            logger_stores(&graph),
            [
                (logger + 0x28, 4, u64::from(expected)),
                (logger + 0x2c, 4, 1)
            ]
        );
        assert_eq!(
            calls(),
            [
                callee::DEBUG_ENTER,
                callee::MATH_MAX,
                callee::INT32_TO_STRING,
                callee::STRING_CONCAT_2,
                callee::DEBUG_WRITE
            ]
        );
    }
}

#[test]
fn skills_from_1000_on_heal_nothing_and_write_a_debug_line() {
    for (skill_id, long) in [
        (999, false),
        (1000, true),
        (1001, true),
        (i32::MIN, false),
        (i32::MAX, true),
        (-1, false),
    ] {
        clear();
        let graph = heal_graph(50, skill_id, 1);
        pre(heal::STRING_SLOT_ENTER, 8, base(&graph) + 8 * 100);
        install_heal();
        let _ = run_heal(&graph);
        let logger = base(&graph) + 8 * 32;
        let heal = if long { 0 } else { 50 };
        assert_eq!(
            logger_stores(&graph),
            [(logger + 0x28, 4, heal), (logger + 0x2c, 4, 1)],
            "skill {skill_id}"
        );
        let writes = calls()
            .iter()
            .filter(|c| **c == callee::DEBUG_WRITE)
            .count();
        assert_eq!(writes, if long { 2 } else { 1 });
    }
}

#[test]
fn heal_runs_the_class_initializer_when_the_math_word_is_zero() {
    clear();
    let graph = heal_graph(5, 3, 0);
    pre(base(&graph) + 8 * 84 + 0xe0, 4, 0);
    install_heal();
    let _ = run_heal(&graph);
    assert_eq!(
        calls()[..2],
        [callee::DEBUG_ENTER, callee::CLASS_INITIALIZER]
    );
}

#[test]
fn heal_first_run_resolves_three_slots_and_stores_the_flag() {
    clear();
    let graph = heal_graph(5, 3, 1);
    PRE.with(|p| p.borrow_mut().retain(|e| e.0 != heal::FLAG));
    pre(heal::FLAG, 1, 0);
    install_heal();
    let _ = run_heal(&graph);
    assert_eq!(
        calls()[..4],
        [
            callee::INITIALIZE_METADATA,
            callee::INITIALIZE_METADATA,
            callee::INITIALIZE_METADATA,
            callee::DEBUG_ENTER
        ]
    );
    let flag_stores: Vec<_> = STORES
        .with(|s| s.borrow().clone())
        .into_iter()
        .filter(|e| e.0 == heal::FLAG)
        .collect();
    assert_eq!(flag_stores, [(heal::FLAG, 1, 1)]);
}
