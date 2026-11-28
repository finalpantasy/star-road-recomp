extern crate std;
use super::*;
use crate::bridge::tests::{CALLS, PRE, STORES, clear, install};
use std::boxed::Box;
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

fn put8(words: &mut [u64; 256], byte: usize, value: u8) {
    unsafe { words.as_mut_ptr().cast::<u8>().add(byte).write(value) }
}

fn base(words: &[u64; 256]) -> u64 {
    words.as_ptr() as u64
}

fn calls() -> Vec<u64> {
    CALLS.with(|c| c.borrow().clone())
}

fn stores() -> Vec<(u64, u32, u64)> {
    STORES.with(|s| s.borrow().clone())
}

/// Words: executer 0 (+0x18 result object 20, +0x20 acter 30, +0x28 target 50, +0x30 skill object 70), logger 90,
/// BattleManager cell 100 and class 110, GlobalWork cell 120 and class 130, statics 140, instance 150, counter object
/// 170, static flag byte 200, literal slot cells 210 to 213 and the cells they point at 220 to 223.
struct Setup {
    calc_type: u32,
    skill_id: u32,
    value: u32,
    acter_id: u32,
    target_id: u32,
    byte: u8,
    counter: u32,
}

fn plain(calc_type: u32) -> Setup {
    Setup {
        calc_type,
        skill_id: 5,
        value: 40,
        acter_id: 1,
        target_id: 2,
        byte: 3,
        counter: 1000,
    }
}

fn graph(s: &Setup) -> Box<[u64; 256]> {
    let mut w = Box::new([0u64; 256]);
    let b = base(&w);
    w[2] = 0xdead;
    w[3] = b + 8 * 20;
    w[4] = b + 8 * 30;
    w[5] = b + 8 * 50;
    w[6] = b + 8 * 70;
    put32(&mut w, 8 * 70 + 0x14, s.skill_id);
    put32(&mut w, 8 * 70 + 0x24, s.calc_type);
    put32(&mut w, 8 * 70 + 0x2c, s.value);
    put32(&mut w, 8 * 30 + 0x20, s.acter_id);
    put8(&mut w, 8 * 30 + 0x6c, s.byte);
    put32(&mut w, 8 * 50 + 0x20, s.target_id);
    w[50 + 0xb8 / 8] = b + 8 * 90;
    w[100] = b + 8 * 110;
    w[120] = b + 8 * 130;
    put32(&mut w, 8 * 110 + 0xe0, 1);
    put32(&mut w, 8 * 130 + 0xe0, 1);
    w[110 + 0xb8 / 8] = b + 8 * 140;
    w[140 + 2] = b + 8 * 150;
    w[150 + 0x60 / 8] = b + 8 * 170;
    put32(&mut w, 8 * 170 + 0x1c, s.counter);
    w[200] = 1;
    for k in 0..4u64 {
        w[210 + k as usize] = b + 8 * (220 + k);
        w[220 + k as usize] = 0x7770 + k;
    }
    pre(cells::FLAG, 1, 1);
    pre(cells::CLASS_SLOT, 8, b + 8 * 100);
    pre(cells::GLOBAL_SLOT, 8, b + 8 * 120);
    pre(cells::STATIC_FLAG_POINTER, 8, b + 8 * 200);
    for (k, slot) in cells::LITERALS.iter().enumerate() {
        pre(*slot, 8, b + 8 * (210 + k as u64));
    }
    w
}

fn install_all() {
    install(callee::INITIALIZE_METADATA, |r| r[0] = 0x77);
    install(callee::CLASS_INITIALIZER, |r| r[0] = 0x78);
    install(callee::GET_INST, |r| {
        assert_eq!(r[0], 0);
        r[0] = 0x6a;
    });
    install(callee::CALC_ATTACK, |r| r[0] = 100);
    install(callee::CALC_DEFENCE, |r| r[0] = 30);
    install(callee::CALC_MAGIC_ATTACK, |r| r[0] = 50);
    install(callee::CALC_MAGIC_DEFENCE, |r| r[0] = 20);
    install(callee::INT32_TO_STRING, |r| r[0] = 0x5555);
    install(callee::STRING_CONCAT_2, |r| {
        assert_eq!(r[1], 0x5555);
        r[0] = 0x6666;
    });
    install(callee::DEBUG_WRITE, |r| assert_eq!(r[0], 0xdead));
    install(callee::GETTER_A, |r| r[0] = 11);
    install(callee::GETTER_B, |r| r[0] = 12);
    install(callee::SETTER_C, |r| r[0] = 13);
    install(callee::OBJECT_GETTER, |r| r[0] = 14);
    install(callee::PREPARE, |_| {});
    install(callee::SKILL_CALL, |_| {});
}

fn run(graph: &[u64; 256]) -> [u64; 96] {
    let stack = Box::leak(Box::new([0u64; 32]));
    let mut file = [0u64; 96];
    file[0] = base(graph);
    file[19] = 0x19;
    file[20] = 0x20;
    file[21] = 0x21;
    file[22] = 0x22;
    file[29] = 0x29;
    file[31] = stack.as_ptr() as u64 + 8 * 28;
    let regs = unsafe { Regs::from_raw(file.as_mut_ptr()) };
    let mut frame = Frame::new(regs);
    let _ = unsafe { SkillExecter::at(file[0]) }.exec_calc_type(&mut frame);
    file
}

fn lines() -> [u64; 3] {
    [
        callee::INT32_TO_STRING,
        callee::STRING_CONCAT_2,
        callee::DEBUG_WRITE,
    ]
}

fn run_type(s: &Setup) -> (Box<[u64; 256]>, [u64; 96]) {
    clear();
    let g = graph(s);
    install_all();
    let file = run(&g);
    (g, file)
}

fn stored_at(g: &[u64; 256], offset: u64) -> Vec<u64> {
    stores()
        .into_iter()
        .filter(|e| e.0 == base(g) + offset)
        .map(|e| e.2)
        .collect()
}

#[test]
fn the_jump_table_matches_the_dispatch_addresses() {
    let targets: Vec<u64> = JUMP_TABLE_ENTRIES
        .iter()
        .map(|e| JUMP_BASE + 4 * u64::from(*e))
        .collect();
    assert_eq!(
        targets,
        [
            arm::SKILL_286,
            arm::PHYSICAL_COUNTER,
            arm::MAGIC_COUNTER,
            arm::MAGIC_SUM,
            arm::SUM_COUNTER,
            arm::VALUE_ONLY,
            arm::RESULT_A,
            arm::RESULT_B,
            arm::PHYSICAL,
            arm::PHYSICAL,
            arm::PHYSICAL,
            arm::NOTHING,
            arm::PREPARE_THEN_CALL,
            arm::CALL
        ]
    );
}

#[test]
fn types_above_13_and_11_only_make_the_class_calls() {
    for calc_type in [11, 14, 0xffff_ffff, 0x8000_0000] {
        let (g, file) = run_type(&plain(calc_type));
        assert_eq!(calls(), [callee::GET_INST], "type {calc_type:#x}");
        assert!(stored_at(&g, 0x40).is_empty());
        assert_eq!(
            (file[19], file[20], file[21], file[22], file[29]),
            (0x19, 0x20, 0x21, 0x22, 0x29)
        );
    }
}

#[test]
fn physical_types_8_to_10_store_attack_minus_defence() {
    for calc_type in [8, 9, 10] {
        let (g, _) = run_type(&plain(calc_type));
        let mut expected = std::vec![callee::GET_INST, callee::CALC_ATTACK, callee::CALC_DEFENCE];
        expected.extend(lines());
        expected.extend(lines());
        assert_eq!(calls(), expected);
        assert_eq!(stored_at(&g, 0x40), [70]);
        assert_eq!(stored_at(&g, 0x44), [1]);
        assert_eq!(stored_at(&g, 0x46), [1]);
        assert!(stored_at(&g, 8 * 170 + 0x1c).is_empty());
    }
}

#[test]
fn type_1_adds_four_to_the_counter_unless_the_byte_is_zero() {
    for (byte, expected) in [(3u8, 1004u32), (0, 1000), (0xff, 1004)] {
        let mut s = plain(1);
        s.byte = byte;
        let (g, _) = run_type(&s);
        assert_eq!(stored_at(&g, 0x40), [70]);
        assert_eq!(stored_at(&g, 8 * 170 + 0x1c), [u64::from(expected)]);
        assert_eq!(calls().len(), 9);
    }
}

#[test]
fn type_2_adds_two_and_uses_the_magic_stats() {
    let mut s = plain(2);
    s.value = 5;
    let (g, _) = run_type(&s);
    let mut expected = std::vec![
        callee::GET_INST,
        callee::CALC_MAGIC_ATTACK,
        callee::CALC_MAGIC_DEFENCE
    ];
    expected.extend(lines());
    expected.extend(lines());
    assert_eq!(calls(), expected);
    // 5 + 50 - 20
    assert_eq!(stored_at(&g, 0x40), [35]);
    assert_eq!(stored_at(&g, 0x44), [1]);
    assert_eq!(stored_at(&g, 8 * 170 + 0x1c), [1002]);
    s.byte = 0;
    let (g, _) = run_type(&s);
    assert_eq!(stored_at(&g, 8 * 170 + 0x1c), [1000]);
}

#[test]
fn types_3_4_5_store_the_skill_value() {
    let (g, _) = run_type(&plain(3));
    assert_eq!(calls()[..2], [callee::GET_INST, callee::CALC_MAGIC_ATTACK]);
    assert_eq!(stored_at(&g, 0x40), [90]);
    assert_eq!(stored_at(&g, 0x45), [1]);
    let (g, _) = run_type(&plain(5));
    let mut expected = std::vec![callee::GET_INST];
    expected.extend(lines());
    assert_eq!(calls(), expected);
    assert_eq!(stored_at(&g, 0x40), [40]);
    assert_eq!(stored_at(&g, 0x45), [1]);
    let (g, _) = run_type(&plain(4));
    assert_eq!(stored_at(&g, 0x40), [40]);
    assert_eq!(stored_at(&g, 0x44), [1]);
    // the counter object gets the acter's byte
    assert_eq!(stored_at(&g, 8 * 170 + 0x1c), [1003]);
}

#[test]
fn type_0_needs_skill_0x286_and_two_different_characters() {
    for (skill, acter, target, runs) in [
        (5u32, 1u32, 2u32, false),
        (0x286, 7, 7, false),
        (0x286, 7, 8, true),
        (0x285, 7, 8, false),
    ] {
        let mut s = plain(0);
        s.skill_id = skill;
        s.acter_id = acter;
        s.target_id = target;
        let (g, _) = run_type(&s);
        if runs {
            assert_eq!(
                calls(),
                [callee::GET_INST, lines()[0], lines()[1], lines()[2]]
            );
            assert_eq!(stored_at(&g, 8 * 90 + 0x28), [40]);
            assert_eq!(stored_at(&g, 8 * 90 + 0x2c), [2]);
        } else {
            assert_eq!(calls(), [callee::GET_INST]);
            assert!(stores().iter().all(|e| e.2 == 0));
        }
    }
}

#[test]
fn types_6_and_7_flag_the_result_object_and_use_the_getters() {
    let (g, _) = run_type(&plain(6));
    assert_eq!(
        calls()[..3],
        [callee::GET_INST, callee::GETTER_A, callee::SETTER_C]
    );
    assert_eq!(stored_at(&g, 8 * 20 + 0x23), [1]);
    assert_eq!(stored_at(&g, 8 * 20 + 0x40), [11]);
    assert_eq!(stored_at(&g, 8 * 20 + 0x44), [13]);
    assert_eq!(stored_at(&g, 0x40), [40]);
    assert_eq!(stored_at(&g, 8 * 90 + 0x28), [40]);
    assert_eq!(stored_at(&g, 8 * 90 + 0x2c), [1]);
    assert_eq!(stored_at(&g, 8 * 90 + 0x41), [1]);
    let (g, _) = run_type(&plain(7));
    assert_eq!(
        calls()[..4],
        [
            callee::GET_INST,
            callee::GETTER_A,
            callee::GETTER_B,
            callee::OBJECT_GETTER
        ]
    );
    assert_eq!(stored_at(&g, 8 * 20 + 0x44), [12]);
    assert_eq!(stored_at(&g, 0x45), [1]);
    assert_eq!(stored_at(&g, 0x40), [14]);
}

#[test]
fn types_12_and_13_call_the_skill_function_with_the_skill_id() {
    let mut s = plain(13);
    s.skill_id = 0x1234;
    let (_, file) = run_type(&s);
    assert_eq!(calls(), [callee::GET_INST, callee::SKILL_CALL]);
    assert_eq!(file[19], 0x19);
    let mut s = plain(12);
    s.skill_id = 0x1234;
    let _ = run_type(&s);
    assert_eq!(
        calls(),
        [callee::GET_INST, callee::PREPARE, callee::SKILL_CALL]
    );
}

#[test]
fn a_clear_flag_resolves_six_slots_first() {
    clear();
    let g = graph(&plain(11));
    PRE.with(|p| p.borrow_mut().retain(|e| e.0 != cells::FLAG));
    pre(cells::FLAG, 1, 0);
    install_all();
    let _ = run(&g);
    assert_eq!(calls()[..6], [callee::INITIALIZE_METADATA; 6]);
    assert_eq!(calls()[6], callee::GET_INST);
    let flag: Vec<_> = stores()
        .into_iter()
        .filter(|e| e.0 == cells::FLAG)
        .collect();
    assert_eq!(flag, [(cells::FLAG, 1, 1)]);
}

#[test]
fn class_words_and_the_static_flag_run_their_initializers() {
    // BattleManager class word zero: the class initializer runs
    clear();
    let mut g = graph(&plain(4));
    put32(&mut g, 8 * 110 + 0xe0, 0);
    install_all();
    let _ = run(&g);
    assert!(calls().contains(&callee::CLASS_INITIALIZER));
    // the static flag byte clear: the metadata initializer runs and the byte is stored
    clear();
    let mut g = graph(&plain(4));
    g[200] = 0;
    install_all();
    let _ = run(&g);
    assert!(calls().contains(&callee::INITIALIZE_METADATA));
    assert!(
        stores()
            .iter()
            .any(|e| e.0 == base(&g) + 8 * 200 && e.2 == 1)
    );
    // the GlobalWork class word zero: the initializer runs before get_inst
    clear();
    let mut g = graph(&plain(11));
    put32(&mut g, 8 * 130 + 0xe0, 0);
    install_all();
    let _ = run(&g);
    assert_eq!(calls(), [callee::CLASS_INITIALIZER, callee::GET_INST]);
}
