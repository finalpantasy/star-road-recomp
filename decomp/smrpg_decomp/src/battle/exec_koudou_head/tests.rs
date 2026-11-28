extern crate std;
use super::*;
use crate::bridge::tests::{CALLS, PRE, STORES, clear, install};
use std::boxed::Box;
use std::cell::{Cell, RefCell};
use std::vec::Vec;

std::thread_local! {
    /// What the fake `bd6c0` returns for each code (index = x1).
    static BD6C0: RefCell<[u64; 8]> = const { RefCell::new([0; 8]) };
    static GET_VALUE: Cell<u64> = const { Cell::new(0) };
    static CHECK: Cell<u64> = const { Cell::new(0) };
    static BARRIERED: Cell<u64> = const { Cell::new(0) };
    static SINGLETON_MODE: Cell<u64> = const { Cell::new(0) };
    static GETTER: Cell<u64> = const { Cell::new(0) };
}

/// The compiled table's entry (halfword, units of 4 bytes from 0x71021f6800) for the ported arms; any other type gets an
/// entry that selects no ported arm.
pub(super) fn fake_entry(index: u32) -> u16 {
    match index + 5 {
        5 => 0xb4,
        6 => 0xca,
        7 | 9 => 0xa8,
        8 => 0x3d,
        0xb..=0x10 => 0,
        0x12 => 0x386,
        0x14 | 0x16 | 0x1a => 0x384,
        0x15 | 0x19 | 0x46 => 0x10,
        0x4a => 0x4a,
        0x50 => 0x3a,
        _ => 0xffff,
    }
}

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
    unsafe { words.as_mut_ptr().cast::<u8>().add(byte).write(value) }
}

fn base(words: &[u64; 512]) -> u64 {
    words.as_ptr() as u64
}

fn calls() -> Vec<u64> {
    CALLS.with(|c| c.borrow().clone())
}

fn stores() -> Vec<(u64, u32, u64)> {
    STORES.with(|s| s.borrow().clone())
}

/// Words: executer 0 (+0x10 monitor, +0x20 acter 30, +0x28 target 50, +0x30 skill object 70), logger 90, class cells 100
/// (list owner), 120 (BattleManager), 125 (third), their classes 110, 130, 470, statics 420 (list 160, elements from word
/// 164) and 150 (+0x10 object 180, +0x60 value word 190), the element object 400, the static flag byte 300, literal cells
/// 310 to 315 pointing at 320 to 325, the singleton 330.
struct Setup {
    skill_type: u32,
    length: u32,
    /// The six flag bytes of the element: +0x18, +0x19, +0x1a, +0x1b, +0x1c, +0x1d.
    bytes: [u8; 6],
    class_a: u32,
    class_b: u32,
    class_c: u32,
    acter_byte: u8,
    flag_44: u8,
    logger_47: u8,
    static_flag: u8,
    /// `_skill_obj +0x14`.
    field_14: u32,
    /// The result object's flag word (+0x38).
    flags_38: u32,
}

fn plain(skill_type: u32) -> Setup {
    Setup {
        skill_type,
        length: 200,
        bytes: [0; 6],
        class_a: 1,
        class_b: 1,
        class_c: 1,
        acter_byte: 0,
        flag_44: 0,
        logger_47: 0,
        static_flag: 1,
        field_14: 0,
        flags_38: 0,
    }
}

fn graph(s: &Setup) -> Box<[u64; 512]> {
    let mut w = Box::new([0u64; 512]);
    let b = base(&w);
    w[2] = 0xdead;
    w[4] = b + 8 * 30;
    w[5] = b + 8 * 50;
    w[3] = b + 8 * 200;
    w[6] = b + 8 * 70;
    w[55] = b + 8 * 205; // target + 0x28 (the getters' object)
    put32(&mut w, 8 * 70 + 0x14, s.field_14);
    put32(&mut w, 8 * 200 + 0x38, s.flags_38);
    put8(&mut w, 0x44, s.flag_44);
    put32(&mut w, 8 * 70 + 0x28, s.skill_type);
    put8(&mut w, 8 * 30 + 0x6c, s.acter_byte);
    w[50 + 0xb8 / 8] = b + 8 * 90;
    put8(&mut w, 8 * 90 + 0x47, s.logger_47);
    w[100] = b + 8 * 110;
    w[120] = b + 8 * 130;
    w[125] = b + 8 * 470;
    put32(&mut w, 8 * 110 + 0xe0, s.class_a);
    put32(&mut w, 8 * 130 + 0xe0, s.class_b);
    put32(&mut w, 8 * 470 + 0xe0, s.class_c);
    w[110 + 0xb8 / 8] = b + 8 * 420;
    w[420 + 0xe0 / 8] = b + 8 * 160;
    put32(&mut w, 8 * 160 + 0x18, s.length);
    for index in [0usize, 4, 0x60, s.skill_type as usize] {
        w[164 + index] = b + 8 * 400;
    }
    w[130 + 0xb8 / 8] = b + 8 * 150;
    w[150 + 2] = b + 8 * 180;
    w[180 + 0x60 / 8] = b + 8 * 190;
    put8(&mut w, 8 * 400 + 0x18, s.bytes[0]);
    put8(&mut w, 8 * 400 + 0x19, s.bytes[1]);
    put8(&mut w, 8 * 400 + 0x1a, s.bytes[2]);
    put8(&mut w, 8 * 400 + 0x1b, s.bytes[3]);
    put8(&mut w, 8 * 400 + 0x1c, s.bytes[4]);
    put8(&mut w, 8 * 400 + 0x1d, s.bytes[5]);
    put8(&mut w, 8 * 300, s.static_flag);
    for k in 0..6usize {
        w[310 + k] = b + 8 * (320 + k as u64);
        w[320 + k] = 0x7770 + k as u64;
    }
    put32(
        &mut w,
        8 * 330 + 0x68,
        SINGLETON_MODE.with(Cell::get) as u32,
    );
    pre(cells::FLAG, 1, 1);
    pre(cells::CLASS_A_SLOT, 8, b + 8 * 100);
    pre(cells::CLASS_B_SLOT, 8, b + 8 * 120);
    pre(cells::CLASS_C_SLOT, 8, b + 8 * 125);
    pre(cells::STATIC_FLAG_POINTER, 8, b + 8 * 300);
    for (k, slot) in cells::LITERALS.iter().enumerate() {
        pre(*slot, 8, b + 8 * (310 + k as u64));
    }
    w
}

fn install_all(g: &[u64; 512]) {
    let singleton = base(g) + 8 * 330;
    install(callee::INITIALIZE_METADATA, |r| r[0] = 0x77);
    install(callee::CLASS_INITIALIZER, |r| r[0] = 0x78);
    install(callee::GET_INST, |r| r[0] = 0x6a);
    install(callee::GET_VALUE, |r| r[0] = GET_VALUE.with(Cell::get));
    install(callee::DEBUG_WRITE, |r| r[0] = 0x79);
    install(callee::CHECK_32, |r| r[0] = CHECK.with(Cell::get));
    install(callee::IS_BARRIERED, |r| r[0] = BARRIERED.with(Cell::get));
    install(callee::BD6C0, |r| {
        r[0] = BD6C0.with(|t| t.borrow()[r[1] as usize])
    });
    install(callee::USE_DAMAGE_GUARD, |_| {});
    install(callee::FLAG_10, |r| {
        r[8] = 0x1010;
        r[9] = 0x1011;
    });
    install(callee::FLAG_11, |r| {
        r[8] = 0x1110;
        r[9] = 0x1111;
    });
    install(callee::GETTER_10, |r| {
        r[0] = GETTER.with(Cell::get);
        r[8] = 0x2010;
    });
    install(callee::GETTER_18, |r| {
        r[0] = GETTER.with(Cell::get);
        r[8] = 0x2018;
    });
    install(callee::INDEX_OUT_OF_RANGE, |r| {
        r[0] = u64::MAX;
        r[1] = u64::MAX;
    });
    SINGLETON_ADDRESS.with(|a| a.set(singleton));
    install(callee::SINGLETON, |r| {
        r[0] = SINGLETON_ADDRESS.with(Cell::get)
    });
}

std::thread_local! {
    static SINGLETON_ADDRESS: Cell<u64> = const { Cell::new(0) };
}

fn reset() {
    clear();
    BD6C0.with(|t| *t.borrow_mut() = [0; 8]);
    GET_VALUE.with(|c| c.set(0));
    CHECK.with(|c| c.set(0));
    BARRIERED.with(|c| c.set(0));
    SINGLETON_MODE.with(|c| c.set(0));
    GETTER.with(|c| c.set(0));
}

fn run(g: &[u64; 512]) -> ([u64; 96], Result<(), GuestException>) {
    let stack = Box::leak(Box::new([0u64; 64]));
    let mut file = [0u64; 96];
    file[0] = base(g);
    for n in 19..30 {
        file[n] = 0x100 + n as u64;
    }
    file[31] = stack.as_ptr() as u64 + 8 * 60;
    for n in [8, 9, 10, 11, 12, 13] {
        file[n] = 0x500 + n as u64;
    }
    let regs = unsafe { Regs::from_raw(file.as_mut_ptr()) };
    let mut frame = Frame::new(regs);
    let outcome = unsafe { SkillExecter::at(file[0]) }.exec_koudou_head(&mut frame);
    (file, outcome)
}

fn go(s: &Setup) -> (Box<[u64; 512]>, [u64; 96], Result<(), GuestException>) {
    reset();
    let g = graph(s);
    install_all(&g);
    let (file, outcome) = run(&g);
    (g, file, outcome)
}

fn stored_at(g: &[u64; 512], offset: u64) -> Vec<u64> {
    stores()
        .into_iter()
        .filter(|e| e.0 == base(g) + offset)
        .map(|e| e.2)
        .collect()
}

fn assert_epilogue(file: &[u64; 96]) {
    for n in 19..30 {
        assert_eq!(file[n], 0x100 + n as u64, "x{n}");
    }
}

#[test]
fn the_arm_of_a_type_follows_the_table_and_defers_what_is_not_ported() {
    for kind in [0u32, 4, 0x60, 0xffff_ffff] {
        assert_eq!(arm_of(kind), Some(Arm::Epilogue), "type {kind:#x}");
    }
    assert_eq!(arm_of(5), Some(Arm::Half));
    assert_eq!(arm_of(6), Some(Arm::Plain));
    assert_eq!(arm_of(7), Some(Arm::Guard10));
    assert_eq!(arm_of(9), Some(Arm::Guard10));
    assert_eq!(arm_of(8), Some(Arm::Guard11));
    assert_eq!(arm_of(0x50), Some(Arm::ResultByteGuard11));
    assert_eq!(arm_of(0x4a), Some(Arm::ResultByte));
    for kind in 0xb..=0x10 {
        assert_eq!(arm_of(kind), Some(Arm::ClearFlag), "type {kind:#x}");
    }
    assert_eq!(arm_of(0x12), Some(Arm::StoreNine));
    assert_eq!(arm_of(0x14), Some(Arm::Epilogue));
    assert_eq!(arm_of(0x15), Some(Arm::StoreOne));
    assert_eq!(arm_of(0x13), None);
    assert_eq!(arm_of(0x2f), None);
}

#[test]
fn no_flag_bytes_make_only_the_instance_call_and_return() {
    for kind in [0u32, 4, 0x60] {
        let (g, file, outcome) = go(&plain(kind));
        assert!(outcome.is_ok());
        assert_eq!(calls(), [callee::GET_INST], "type {kind}");
        assert!(stores().is_empty());
        assert_epilogue(&file);
        assert_eq!(file[0], 0x6a);
        drop(g);
    }
}

#[test]
fn a_lifted_arm_runs_the_lifted_body_and_the_port_touches_nothing() {
    let (_g, file, outcome) = go(&plain(0x13));
    assert!(outcome.is_ok());
    assert!(calls().is_empty());
    assert!(stores().is_empty());
    assert_eq!(file[19], 0x100 + 19);
    assert_eq!(file[8], 0x508);
}

#[test]
fn flag_bytes_call_bd6c0_in_order_and_the_last_result_is_stored_twice() {
    let mut s = plain(0);
    s.bytes = [1, 0, 0, 1, 0, 1]; // +0x18, +0x1b, +0x1d
    reset();
    BD6C0.with(|t| {
        let mut t = t.borrow_mut();
        t[5] = 7;
        t[7] = 9;
        t[2] = 11;
    });
    let g = graph(&s);
    install_all(&g);
    let (file, outcome) = run(&g);
    assert!(outcome.is_ok());
    // GET_INST, then +0x1b (5), +0x1d (7), +0x18 (2), then the loop body: instance, value, singleton, barrier check
    assert_eq!(
        calls(),
        [
            callee::GET_INST,
            callee::BD6C0,
            callee::BD6C0,
            callee::BD6C0,
            callee::GET_INST,
            callee::GET_VALUE,
            callee::SINGLETON,
            callee::IS_BARRIERED,
        ]
    );
    assert_eq!(stored_at(&g, 8 * 90 + 0x60), [11, 11]);
    assert_epilogue(&file);
}

#[test]
fn a_zero_last_result_marks_the_logger_and_runs_the_loop_body() {
    let mut s = plain(0);
    s.bytes = [0, 0, 0, 1, 0, 0];
    let (g, file, outcome) = go(&s);
    assert!(outcome.is_ok());
    assert_eq!(stored_at(&g, 8 * 90 + 0x47), [1]);
    assert!(stored_at(&g, 8 * 90 + 0x60).is_empty());
    assert_eq!(file[24], 0x118);
    assert_eq!(
        calls(),
        [
            callee::GET_INST,
            callee::BD6C0,
            callee::GET_INST,
            callee::GET_VALUE,
            callee::SINGLETON,
            callee::IS_BARRIERED,
        ]
    );
}

#[test]
fn the_debug_block_runs_when_the_value_is_one_and_the_acter_byte_zero() {
    let mut s = plain(4);
    s.bytes = [1, 0, 0, 0, 0, 0];
    reset();
    BD6C0.with(|t| t.borrow_mut()[2] = 3);
    GET_VALUE.with(|c| c.set(1));
    CHECK.with(|c| c.set(1));
    let g = graph(&s);
    install_all(&g);
    let (file, outcome) = run(&g);
    assert!(outcome.is_ok());
    assert_eq!(
        calls(),
        [
            callee::GET_INST,
            callee::BD6C0,
            callee::GET_INST,
            callee::GET_VALUE,
            callee::DEBUG_WRITE,
            callee::DEBUG_WRITE,
            callee::CHECK_32,
            callee::SINGLETON,
            callee::IS_BARRIERED,
        ]
    );
    assert_eq!(stored_at(&g, 8 * 90 + 0x60), [3, 3]);
    assert_epilogue(&file);
}

#[test]
fn a_failed_check_writes_two_more_lines_and_clears_the_result() {
    let mut s = plain(4);
    s.bytes = [1, 0, 0, 0, 0, 0];
    reset();
    BD6C0.with(|t| t.borrow_mut()[2] = 3);
    GET_VALUE.with(|c| c.set(1));
    CHECK.with(|c| c.set(0));
    let g = graph(&s);
    install_all(&g);
    let (file, outcome) = run(&g);
    assert!(outcome.is_ok());
    assert_eq!(
        calls()
            .iter()
            .filter(|c| **c == callee::DEBUG_WRITE)
            .count(),
        4
    );
    assert!(stored_at(&g, 8 * 90 + 0x60).is_empty());
    assert_eq!(file[19], 0x113);
}

#[test]
fn singleton_mode_three_marks_the_logger_and_clears_the_result() {
    let mut s = plain(0);
    s.bytes = [0, 1, 0, 0, 0, 0];
    reset();
    BD6C0.with(|t| t.borrow_mut()[3] = 5);
    SINGLETON_MODE.with(|c| c.set(3));
    let g = graph(&s);
    install_all(&g);
    let (_file, outcome) = run(&g);
    assert!(outcome.is_ok());
    assert_eq!(stored_at(&g, 8 * 90 + 0x47), [1]);
    assert!(stored_at(&g, 8 * 90 + 0x60).is_empty());
    assert_eq!(
        calls()
            .iter()
            .filter(|c| **c == callee::DEBUG_WRITE)
            .count(),
        1
    );
}

#[test]
fn a_barriered_target_uses_the_damage_guard_unless_flag_44_is_set() {
    for (flag_44, guard_calls) in [(0u8, 1usize), (1, 0)] {
        let mut s = plain(0);
        s.flag_44 = flag_44;
        s.bytes = [0, 0, 1, 0, 0, 0];
        reset();
        BD6C0.with(|t| t.borrow_mut()[4] = 2);
        BARRIERED.with(|c| c.set(1));
        let g = graph(&s);
        install_all(&g);
        let (file, outcome) = run(&g);
        assert!(outcome.is_ok());
        assert_eq!(
            calls()
                .iter()
                .filter(|c| **c == callee::USE_DAMAGE_GUARD)
                .count(),
            guard_calls
        );
        // the 0x47 mark (once for the loop's first branch, once here) and no result store
        assert_eq!(stored_at(&g, 8 * 90 + 0x47).last(), Some(&1));
        assert!(stored_at(&g, 8 * 90 + 0x60).is_empty());
        assert_epilogue(&file);
    }
}

#[test]
fn a_barriered_target_whose_logger_byte_is_set_keeps_the_result() {
    let mut s = plain(0);
    s.bytes = [0, 0, 1, 0, 0, 0];
    s.logger_47 = 1;
    reset();
    BD6C0.with(|t| t.borrow_mut()[4] = 2);
    BARRIERED.with(|c| c.set(1));
    let g = graph(&s);
    install_all(&g);
    let (_file, outcome) = run(&g);
    assert!(outcome.is_ok());
    assert!(
        calls()
            .iter()
            .all(|c| *c != callee::USE_DAMAGE_GUARD && *c != callee::DEBUG_WRITE)
    );
    assert_eq!(stored_at(&g, 8 * 90 + 0x60), [2, 2]);
}

#[test]
fn a_type_past_the_list_length_calls_the_thrower_and_returns_the_exception() {
    let mut s = plain(0);
    s.length = 0;
    let (_g, _file, outcome) = go(&s);
    assert!(outcome.is_err());
    assert_eq!(calls(), [callee::INDEX_OUT_OF_RANGE]);
}

#[test]
fn the_first_run_resolves_27_slots_and_sets_the_flag_byte() {
    reset();
    let s = plain(0);
    let g = graph(&s);
    install_all(&g);
    PRE.with(|p| {
        p.borrow_mut().retain(|e| e.0 != cells::FLAG);
        p.borrow_mut().push((cells::FLAG, 1, 0));
    });
    for slot in cells::INIT_SLOTS {
        pre(slot, 8, 0x1234);
    }
    let (file, outcome) = run(&g);
    assert!(outcome.is_ok());
    let list = calls();
    assert_eq!(list.len(), 28);
    assert!(list[..27].iter().all(|c| *c == callee::INITIALIZE_METADATA));
    assert_eq!(list[27], callee::GET_INST);
    assert_eq!(stores(), [(cells::FLAG, 1, 1)]);
    assert_epilogue(&file);
}

#[test]
fn class_words_that_are_zero_run_the_initializers_and_the_metadata_byte() {
    let mut s = plain(0);
    s.class_a = 0;
    s.class_b = 0;
    s.class_c = 0;
    s.static_flag = 0;
    let (_g, file, outcome) = go(&s);
    assert!(outcome.is_ok());
    assert_eq!(
        calls(),
        [
            callee::CLASS_INITIALIZER,
            callee::CLASS_INITIALIZER,
            callee::INITIALIZE_METADATA,
            callee::CLASS_INITIALIZER,
            callee::CLASS_INITIALIZER,
            callee::GET_INST,
        ]
    );
    assert_eq!(stores().len(), 1);
    assert_eq!(stores()[0].1, 1);
    assert_epilogue(&file);
}

const LOGGER: u64 = 8 * 90;
const RESULT: u64 = 8 * 200;

#[test]
fn a_table_type_that_goes_to_the_epilogue_stores_a_nonzero_result_only() {
    let mut s = plain(0x14);
    s.bytes = [1, 0, 0, 0, 0, 0];
    reset();
    BD6C0.with(|t| t.borrow_mut()[2] = 3);
    let g = graph(&s);
    install_all(&g);
    let (file, outcome) = run(&g);
    assert!(outcome.is_ok());
    assert_eq!(stored_at(&g, LOGGER + 0x60), [3, 3]);
    assert_epilogue(&file);
    let (g, file, _) = go(&plain(0x14));
    assert!(stored_at(&g, LOGGER + 0x60).is_empty());
    assert_epilogue(&file);
}

#[test]
fn type_0x12_stores_nine_and_type_0x15_stores_one_without_a_result() {
    for (kind, value) in [(0x12u32, 9u64), (0x15, 1), (0x19, 1), (0x46, 1)] {
        let (g, file, outcome) = go(&plain(kind));
        assert!(outcome.is_ok());
        assert_eq!(stored_at(&g, LOGGER + 0x60), [value], "type {kind:#x}");
        assert_epilogue(&file);
    }
}

#[test]
fn types_0xb_to_0x10_clear_flag_44_and_mark_the_logger_when_the_loop_skipped() {
    // not skipped (the debug block does not run): only the flag byte
    for kind in 0xb..=0x10 {
        let mut s = plain(kind);
        s.flag_44 = 1;
        let (g, file, outcome) = go(&s);
        assert!(outcome.is_ok());
        assert_eq!(stored_at(&g, 0x44), [0]);
        assert!(stored_at(&g, LOGGER + 0x40).is_empty());
        assert_epilogue(&file);
    }
    // skipped: the debug check fails, so x24 is 1 and the logger byte +0x40 is set
    let mut s = plain(0xe);
    s.bytes = [1, 0, 0, 0, 0, 0];
    reset();
    BD6C0.with(|t| t.borrow_mut()[2] = 3);
    GET_VALUE.with(|c| c.set(1));
    CHECK.with(|c| c.set(0));
    let g = graph(&s);
    install_all(&g);
    let (file, outcome) = run(&g);
    assert!(outcome.is_ok());
    assert_eq!(stored_at(&g, LOGGER + 0x40), [1]);
    assert_eq!(stored_at(&g, 0x44), [0]);
    assert_epilogue(&file);
}

#[test]
fn type_0x4a_sets_the_result_byte_and_types_7_8_9_0x50_set_flags() {
    let (g, file, outcome) = go(&plain(0x4a));
    assert!(outcome.is_ok());
    assert_eq!(stored_at(&g, RESULT + 0x24), [1]);
    assert_eq!(calls(), [callee::GET_INST]);
    assert_epilogue(&file);
    // the guard arms: the nested call on the target (x1 = 0), then the flag word or-ed with 8 or 4
    for (kind, guard, bit, byte) in [
        (8u32, callee::FLAG_11, 8u64, false),
        (7, callee::FLAG_10, 4, false),
        (9, callee::FLAG_10, 4, false),
        (0x50, callee::FLAG_11, 8, true),
    ] {
        let mut s = plain(kind);
        s.flags_38 = 0x30;
        let (g, file, outcome) = go(&s);
        assert!(outcome.is_ok());
        assert_eq!(calls(), [callee::GET_INST, guard], "type {kind:#x}");
        assert_eq!(stored_at(&g, RESULT + 0x38), [0x30 | bit], "type {kind:#x}");
        assert_eq!(
            stored_at(&g, RESULT + 0x24),
            if byte { [1].to_vec() } else { Vec::new() }
        );
        assert_epilogue(&file);
    }
}

#[test]
fn type_5_stores_the_getter_value_or_half_of_it() {
    for (field, value, expected, with_byte) in [
        (0x7fu32, 11u64, 5u64, true),
        (0x7f, 0xffff_fffd, 0xffff_ffff, true), // -3: (-3 + 1) >> 1 = -1
        (0x7f, 0xffff_ffff, 0, true),
        (0x20, 11, 11, false),
    ] {
        let mut s = plain(5);
        s.field_14 = field;
        reset();
        GETTER.with(|c| c.set(value));
        let g = graph(&s);
        install_all(&g);
        let (file, outcome) = run(&g);
        assert!(outcome.is_ok());
        assert_eq!(
            stored_at(&g, LOGGER + 0x28),
            [expected],
            "field {field:#x} value {value:#x}"
        );
        assert_eq!(stored_at(&g, LOGGER + 0x2c), [1]);
        assert_eq!(stored_at(&g, LOGGER + 0x60), [1]);
        assert_eq!(
            stored_at(&g, LOGGER + 0x45),
            if with_byte { [1].to_vec() } else { Vec::new() }
        );
        assert_eq!(calls(), [callee::GET_INST, callee::GETTER_10]);
        assert_epilogue(&file);
        assert_eq!(file[0], value);
    }
}

#[test]
fn type_6_stores_the_other_getter_mode_3_and_a_byte() {
    let s = plain(6);
    reset();
    GETTER.with(|c| c.set(42));
    let g = graph(&s);
    install_all(&g);
    let (file, outcome) = run(&g);
    assert!(outcome.is_ok());
    assert_eq!(stored_at(&g, LOGGER + 0x28), [42]);
    assert_eq!(stored_at(&g, LOGGER + 0x2c), [3]);
    assert_eq!(stored_at(&g, LOGGER + 0x41), [1]);
    assert!(stored_at(&g, LOGGER + 0x60).is_empty());
    assert_eq!(calls(), [callee::GET_INST, callee::GETTER_18]);
    assert_epilogue(&file);
    assert_eq!(file[0], 42);
}
