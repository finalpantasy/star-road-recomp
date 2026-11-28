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
    }
}

fn graph(s: &Setup) -> Box<[u64; 512]> {
    let mut w = Box::new([0u64; 512]);
    let b = base(&w);
    w[2] = 0xdead;
    w[4] = b + 8 * 30;
    w[5] = b + 8 * 50;
    w[6] = b + 8 * 70;
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
    for index in [0usize, 4, 0x60] {
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
fn the_jump_table_range_is_type_minus_five_up_to_0x5a() {
    assert!(!takes_table(4));
    assert!(takes_table(5));
    assert!(takes_table(0x5f));
    assert!(!takes_table(0x60));
    assert!(!takes_table(0));
    assert!(!takes_table(0xffff_ffff));
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
fn a_table_type_runs_the_lifted_body_and_the_port_touches_nothing() {
    let (_g, file, outcome) = go(&plain(5));
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
