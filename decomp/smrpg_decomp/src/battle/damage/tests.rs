extern crate std;
use super::*;
use crate::bridge::tests::{STORES, clear};
use std::boxed::Box;
use std::collections::BTreeMap;
use std::vec::Vec;

/// Fake graph: executer at word 0 (+0x18 result, +0x20 acter, +0x30 skill object), acter at word 8 (+0x6c byte),
/// result at word 24, skill object at word 40. The result starts filled with 0xaa bytes.
struct Graph(Box<[u64; 64]>);

impl Graph {
    fn new(skill_id: i32, player: u8) -> Self {
        let mut words = Box::new([0u64; 64]);
        let base = words.as_ptr() as u64;
        words[3] = base + 8 * 24;
        words[4] = base + 8 * 8;
        words[6] = base + 8 * 40;
        let bytes = words.as_mut_ptr().cast::<u8>();
        unsafe {
            bytes.add(8 * 8 + 0x6c).write(player);
            bytes
                .add(8 * 40 + 0x14)
                .cast::<i32>()
                .write_unaligned(skill_id);
        }
        Self(words)
    }

    fn executer(&self) -> SkillExecter {
        unsafe { SkillExecter::at(self.0.as_ptr() as u64) }
    }

    fn result_address(&self) -> u64 {
        self.0.as_ptr() as u64 + 8 * 24
    }
}

/// Run the port and fold the recorded stores into a (result offset -> value) map.
fn run(skill_id: i32, player: u8) -> BTreeMap<u64, u64> {
    clear();
    let graph = Graph::new(skill_id, player);
    graph.executer().set_acction_command_ratio(&mut Journal);
    let base = graph.result_address();
    let mut map = BTreeMap::new();
    STORES.with(|s| {
        for &(addr, size, value) in s.borrow().iter() {
            assert!(size == 1 || size == 4);
            map.insert(addr - base, value);
        }
    });
    map
}

/// The table of the compiled code, written the other way round: for each rule the list of table indexes (`id - 1`).
fn reference(skill_id: i32, player: u8) -> BTreeMap<u64, u64> {
    let mut m = BTreeMap::new();
    m.insert(0x58, 0x3f80_0000);
    m.insert(0x5c, 0);
    if player == 0 {
        m.insert(0x50, 0xbf00_0000);
        m.insert(0x54, 0xbf80_0000);
        if skill_id == 0x2d7 {
            m.insert(0x50, 0x8000_0000);
        }
        return m;
    }
    let index = (skill_id as u32).wrapping_sub(1);
    let in_list = |list: &[u32]| list.contains(&index);
    let (succeed, sweet): (u64, u64);
    if index == 0 {
        (succeed, sweet) = (0x3f00_0000, 0x3f80_0000);
    } else if in_list(&[102, 109, 111, 121]) {
        (succeed, sweet) = (0x3e80_0000, 0x3f00_0000);
    } else if in_list(&[103, 107, 105, 122, 104, 106, 112, 113, 119, 120, 128]) {
        m.insert(0x5c, 1);
        succeed = match index {
            103 | 107 | 105 | 122 => 0x3d80_0000,
            104 | 106 | 112 => 0x3e00_0000,
            _ => 0x3d80_0000,
        };
        if matches!(index, 103 | 107) {
            m.insert(0x58, 0x3f90_a3d7);
        }
        if matches!(index, 105 | 122) {
            m.insert(0x58, 0x3f99_999a);
        }
        m.insert(0x50, succeed);
        return m;
    } else if in_list(&[108, 123, 124]) {
        (succeed, sweet) = (0x3e80_0000, 0x3e80_0000);
    } else if in_list(&[114, 117, 118]) {
        m.insert(0x22, 1);
        (succeed, sweet) = (0x3e80_0000, 0x3e80_0000);
    } else if index == 126 {
        (succeed, sweet) = (0x3f00_0000, 0x3f00_0000);
    } else {
        (succeed, sweet) = (0, 0);
    }
    m.insert(0x50, succeed);
    m.insert(0x54, sweet);
    m
}

#[test]
fn ratios_follow_the_skill_table_for_every_id() {
    let ids: Vec<i32> = (-3..140)
        .chain([
            0x2d6,
            0x2d7,
            0x2d8,
            i32::MIN,
            i32::MAX,
            0x8000_0001u32 as i32,
        ])
        .collect();
    for id in ids {
        for player in [0u8, 1, 2, 0x80, 0xff] {
            assert_eq!(
                run(id, player),
                reference(id, player),
                "id {id} player {player}"
            );
        }
    }
}

#[test]
fn the_barrier_flag_and_count() {
    let mut words = Box::new([0u64; 48]);
    let base = words.as_ptr() as u64;
    words[0xa0 / 8] = base + 8 * 24;
    words[0xb8 / 8] = base + 8 * 32;
    let bytes = words.as_mut_ptr().cast::<u8>();
    let param = unsafe { BattleCharacterParam::at(base) };
    for (count, expected) in [
        (0i32, false),
        (1, true),
        (7, true),
        (-1, false),
        (i32::MIN, false),
    ] {
        unsafe {
            bytes
                .add(8 * 24 + 0x2c)
                .cast::<i32>()
                .write_unaligned(count)
        };
        assert_eq!(param.is_barriered(), expected, "count {count}");
    }
    clear();
    param.use_damage_guard(&mut Journal);
    STORES.with(|s| assert_eq!(*s.borrow(), [(base + 8 * 32 + 0x49, 1, 1)]));
}

#[test]
fn group_halving_rounds_toward_zero() {
    use super::{halve, three_halves};
    for (value, half) in [
        (0, 0),
        (1, 0),
        (2, 1),
        (3, 1),
        (-1, 0),
        (-2, -1),
        (-3, -1),
        (i32::MAX, i32::MAX / 2),
        (i32::MIN, i32::MIN / 2),
    ] {
        assert_eq!(halve(value), half, "halve({value})");
        assert_eq!(halve(value), value.wrapping_div(2), "div({value})");
    }
    // `value * 3` wraps to 32 bits before the halving, as `add w8, w8, w8, lsl #1` does
    assert_eq!(three_halves(10), 15);
    assert_eq!(three_halves(-11), -16);
    assert_eq!(
        three_halves(i32::MAX),
        (i32::MAX.wrapping_mul(3)).wrapping_div(2)
    );
}

#[test]
fn group_metadata_returns_step_by_twelve() {
    use super::{group_site, group_slots};
    assert_eq!(group_slots::INIT_ORDER.len(), 8);
    assert_eq!(
        group_site::FIRST_INIT_RETURN + group_site::INIT_STEP * 7,
        0x7102201b2c
    );
}

#[test]
fn driver_metadata_returns_step_by_twelve() {
    use super::{driver_site, driver_slots};
    assert_eq!(driver_slots::INIT_ORDER.len(), 26);
    assert_eq!(
        driver_site::FIRST_INIT_RETURN + driver_site::INIT_STEP * 25,
        0x71021ff6e4
    );
    // no slot is resolved twice
    let mut slots = driver_slots::INIT_ORDER.to_vec();
    slots.sort_unstable();
    slots.dedup();
    assert_eq!(slots.len(), 26);
}
