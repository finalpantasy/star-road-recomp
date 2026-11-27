extern crate std;
use super::*;
use crate::bridge::tests::{PRE, STORES, clear};
use crate::fp::FpEnv;
use std::boxed::Box;
use std::cell::RefCell;
use std::vec::Vec;

std::thread_local! {
    static SLOTS: RefCell<[u64; 3]> = const { RefCell::new([0; 3]) };
}

pub fn slot_value(slot: u64) -> u64 {
    SLOTS.with(|s| match slot {
        slots::STRAWBERRY_CLASS => s.borrow()[0],
        slots::BATTLE_MANAGER_CLASS => s.borrow()[1],
        slots::STATIC_FLAG_POINTER => s.borrow()[2],
        _ => panic!("unexpected slot {slot:#x}"),
    })
}

/// The fake graph of the selftest: param at word 0, buff 24, `int[]` 32, weapon 40, `BattleManager` class 64, its
/// statics 100, the manager 104, strawberry class 120, slot holders 150 and 152, the flag byte at word 154.
struct Graph(Box<[u64; 192]>);

#[derive(Clone, Copy)]
struct Setup {
    stat: AttackStat,
    flag: u8,
    length: u32,
    attack: u32,
    added: u32,
    player: u8,
    weapon_exist: u8,
    base_value: u32,
    class_initialized: u32,
    flag_b: u8,
    encounter: u32,
    strawberry_initialized: u32,
}

impl Setup {
    fn stat_offset(&self) -> usize {
        match self.stat {
            AttackStat::Physical => 0x3c,
            AttackStat::Magic => 0x44,
        }
    }

    fn index(&self) -> usize {
        self.stat.add_status_index() as usize
    }
}

impl Graph {
    fn new(s: &Setup) -> Self {
        let mut g = Self(Box::new([0; 192]));
        let rb = g.0.as_ptr() as u64;
        g.0[0xa0 / 8] = rb + 8 * 24;
        g.0[0xb0 / 8] = rb + 8 * 40;
        g.0[24 + 3] = rb + 8 * 32;
        g.0[64 + 0xb8 / 8] = rb + 8 * 100;
        g.0[100 + 2] = rb + 8 * 104;
        g.0[150] = rb + 8 * 64;
        g.0[152] = rb + 8 * 120;
        g.set32(s.stat_offset(), s.attack);
        g.set8(0x6c, s.player);
        g.set32(8 * 32 + 0x18, s.length);
        g.set32(8 * 32 + 0x20 + 4 * s.index(), s.added);
        g.set8(8 * 40 + 0x10, s.weapon_exist);
        g.set32(8 * 40 + 0x14, s.base_value);
        g.set32(8 * 40 + 0x18, 3);
        g.set32(8 * 40 + 0x1c, 9);
        g.set32(8 * 64 + 0xe0, s.class_initialized);
        g.set32(8 * 104 + 0x168, s.encounter);
        g.set32(8 * 120 + 0xe0, s.strawberry_initialized);
        g.set8(8 * 154, s.flag_b);
        SLOTS.with(|slot| *slot.borrow_mut() = [rb + 8 * 152, rb + 8 * 150, rb + 8 * 154]);
        clear();
        PRE.with(|p| {
            p.borrow_mut()
                .push((s.stat.init_flag(), 1, u64::from(s.flag)))
        });
        g
    }

    fn set32(&mut self, byte: usize, v: u32) {
        let p = self.0.as_mut_ptr() as *mut u8;
        unsafe { p.add(byte).cast::<u32>().write_unaligned(v) };
    }

    fn set8(&mut self, byte: usize, v: u8) {
        unsafe { (self.0.as_mut_ptr() as *mut u8).add(byte).write(v) };
    }

    fn param(&self) -> BattleCharacterParam {
        unsafe { BattleCharacterParam::at(self.0.as_ptr() as u64) }
    }
}

#[derive(Debug, PartialEq, Clone)]
enum Ev {
    Enter(u8),
    InitSlot(bool),
    Mark,
    Throw,
    Weapon(u8),
    NoBonus,
    Roll(u32, u32, u32),
    Ratio,
    Total(i32),
    Class(u32),
    ClassInit(ClassInitSite),
    Flag(u8),
    Poll(PollSite),
    Meta,
    Again(u32),
    Reload,
    Encounter(u32),
    Strawberry(u32),
    Candle(i32),
    Leave(i32),
}

struct Recorder {
    env: FpEnv,
    ratio: f32,
    candles: i32,
    events: Vec<Ev>,
}

impl Float32Ops for Recorder {
    fn fadd(&mut self, a: f32, b: f32) -> f32 {
        self.env.fadd32(a, b)
    }
    fn fsub(&mut self, a: f32, b: f32) -> f32 {
        self.env.fsub32(a, b)
    }
    fn fmul(&mut self, a: f32, b: f32) -> f32 {
        self.env.fmul32(a, b)
    }
}

impl Float32Convert for Recorder {
    fn float_from_i32(&mut self, v: i32) -> f32 {
        self.env.float_from_i32(v)
    }
    fn trunc_to_i32(&mut self, v: f32) -> i32 {
        self.env.trunc_to_i32(v)
    }
    fn compare(&mut self, a: f32, b: f32) -> Nzcv {
        self.env.compare(a, b)
    }
}

impl AttackCalls for Recorder {
    fn enter(&mut self, _: AttackStat, _: BattleCharacterParam, flag: u8) {
        self.events.push(Ev::Enter(flag));
    }
    fn initialize_metadata_slot(
        &mut self,
        _: AttackStat,
        second: bool,
    ) -> Result<(), GuestException> {
        self.events.push(Ev::InitSlot(second));
        Ok(())
    }
    fn mark_initialized(&mut self) {
        self.events.push(Ev::Mark);
    }
    fn load_add_status(&mut self, _: BuffManager, _: Int32Array, _: u32) {}
    fn throw_index_out_of_range(&mut self, _: AttackStat) -> GuestException {
        self.events.push(Ev::Throw);
        GuestException
    }
    fn load_stat(&mut self, _: StatLoads) {}
    fn load_weapon(&mut self, _: BattleCharacterWeapon, b: u8) {
        self.events.push(Ev::Weapon(b));
    }
    fn no_weapon_bonus(&mut self) {
        self.events.push(Ev::NoBonus);
    }
    fn roll_weapon(&mut self, r: WeaponRoll) -> Result<i32, GuestException> {
        self.events
            .push(Ev::Roll(r.base_value, r.random_min, r.random_max));
        Ok(r.base_value as i32 + 4)
    }
    fn attack_up_ratio(&mut self, _: AttackStat) -> Result<f32, GuestException> {
        self.events.push(Ev::Ratio);
        Ok(self.ratio)
    }
    fn player_total(&mut self, total: i32) {
        self.events.push(Ev::Total(total));
    }
    fn load_battle_manager_class(&mut self, _: ClassView, initialized: u32) {
        self.events.push(Ev::Class(initialized));
    }
    fn run_class_initializer(
        &mut self,
        _: AttackStat,
        site: ClassInitSite,
    ) -> Result<(), GuestException> {
        self.events.push(Ev::ClassInit(site));
        if site == ClassInitSite::FirstCheck {
            // the real initializer sets the class's initialized word
            let class = unsafe { guest_ld64(slot_value(slots::BATTLE_MANAGER_CLASS)) };
            unsafe { ((class + 0xe0) as *mut u32).write(1) };
        }
        Ok(())
    }
    fn load_static_flag(&mut self, _: u64, flag: u8) {
        self.events.push(Ev::Flag(flag));
    }
    fn poll(&mut self, _: AttackStat, site: PollSite) {
        self.events.push(Ev::Poll(site));
    }
    fn initialize_static_metadata(&mut self, _: AttackStat) -> Result<(), GuestException> {
        self.events.push(Ev::Meta);
        Ok(())
    }
    fn load_battle_manager_class_again(&mut self, initialized: u32) {
        self.events.push(Ev::Again(initialized));
    }
    fn reload_battle_manager_class(&mut self) {
        self.events.push(Ev::Reload);
    }
    fn load_encounter_id(&mut self, id: u32) {
        self.events.push(Ev::Encounter(id));
    }
    fn load_strawberry_class(&mut self, _: ClassView, initialized: u32) {
        self.events.push(Ev::Strawberry(initialized));
    }
    fn candle_bonus(&mut self, _: AttackStat, sum: i32) -> Result<i32, GuestException> {
        let r = sum.wrapping_add(5 * self.candles);
        self.events.push(Ev::Candle(r));
        Ok(r)
    }
    fn leave(&mut self, result: i32) {
        self.events.push(Ev::Leave(result));
    }
}

fn host_scale(total: i32, ratio: f32) -> i32 {
    let product = ratio * (total as f32);
    if product == f32::INFINITY {
        i32::MIN
    } else {
        product as i32
    }
}

#[derive(Clone, Copy)]
enum Label {
    Flag,
    MetaInit,
    SecondTest,
    SecondInit,
    Done,
}

/// The control flow of the lifted code, written as the C reads it (labels as states), for one setup.
fn model(s: &Setup, ratio: f32, candles: i32) -> (Vec<Ev>, Option<i32>) {
    let mut e = Vec::new();
    e.push(Ev::Enter(s.flag));
    if s.flag & 1 == 0 {
        e.extend([Ev::InitSlot(false), Ev::InitSlot(true), Ev::Mark]);
    }
    if u64::from(s.length) <= s.stat.add_status_index() {
        e.push(Ev::Throw);
        return (e, None);
    }
    let sum = (s.attack as i32).wrapping_add(s.added as i32);
    if s.player != 0 {
        let mut total = sum;
        if s.stat == AttackStat::Physical {
            e.push(Ev::Weapon(s.weapon_exist));
            if s.weapon_exist == 0 {
                e.push(Ev::NoBonus);
            } else {
                e.push(Ev::Roll(s.base_value, 3, 9));
                total = (s.base_value as i32 + 4).wrapping_add(sum);
            }
        }
        e.push(Ev::Ratio);
        if s.stat == AttackStat::Physical {
            e.push(Ev::Total(total));
        }
        let r = host_scale(total, ratio);
        e.push(Ev::Leave(r));
        return (e, Some(r));
    }
    e.push(Ev::Class(s.class_initialized));
    let mut ran = false;
    if s.class_initialized == 0 {
        e.push(Ev::ClassInit(ClassInitSite::FirstCheck));
        ran = true;
    }
    let mut state = Label::Flag;
    loop {
        let word = if ran { 1 } else { s.class_initialized };
        match state {
            Label::Flag => {
                e.push(Ev::Flag(s.flag_b));
                if ran {
                    e.push(Ev::Poll(PollSite::AfterFlag));
                }
                state = if s.flag_b == 0 {
                    Label::MetaInit
                } else {
                    Label::SecondTest
                };
            }
            Label::MetaInit => {
                e.push(Ev::Meta);
                e.push(Ev::Again(word));
                e.push(Ev::Poll(PollSite::AfterMetadata));
                state = if word == 0 {
                    Label::SecondInit
                } else {
                    Label::Done
                };
            }
            Label::SecondTest => {
                e.push(Ev::Again(word));
                state = if word == 0 {
                    Label::SecondInit
                } else {
                    Label::Done
                };
            }
            Label::SecondInit => {
                e.extend([
                    Ev::ClassInit(ClassInitSite::SecondCheck),
                    Ev::Reload,
                    Ev::Poll(PollSite::AfterSecondInit),
                ]);
                state = Label::Done;
            }
            Label::Done => break,
        }
    }
    e.push(Ev::Encounter(s.encounter));
    let mut result = sum;
    if s.encounter == 0x81c {
        e.push(Ev::Strawberry(s.strawberry_initialized));
        if s.strawberry_initialized == 0 {
            e.push(Ev::ClassInit(ClassInitSite::Strawberry));
        }
        result = sum.wrapping_add(5 * candles);
        e.push(Ev::Candle(result));
    }
    e.push(Ev::Leave(result));
    (e, Some(result))
}

#[test]
fn attack_follows_the_lifted_control_flow_and_the_float_cast() {
    let ratios = [
        0x3f80_0000u32,
        0x3fc0_0000,
        0,
        0x7f80_0000,
        0x7fc0_0000,
        0x4f00_0000,
        0xbf80_0000,
    ];
    let attacks = [100u32, 0x7fff_ffff, 0xffff_ff9c, 7, 0, 55, 9];
    let addeds = [5u32, 5, 0x10, 0x7fff_ffff, 0, 1, 2];
    let mut cases = 0;
    for stat in [AttackStat::Physical, AttackStat::Magic] {
        for flag in [0u8, 1, 3] {
            for length in [0u32, 1, 2, 3, 9] {
                for player in [0u8, 1, 0xff] {
                    for weapon_exist in [0u8, 1, 0x80] {
                        for class_initialized in [0u32, 1] {
                            for flag_b in [0u8, 1] {
                                for encounter in [0x81cu32, 0x81b, 0] {
                                    for strawberry_initialized in [0u32, 1] {
                                        for i in 0..ratios.len() {
                                            let s = Setup {
                                                stat,
                                                flag,
                                                length,
                                                attack: attacks[i],
                                                added: addeds[i],
                                                player,
                                                weapon_exist,
                                                base_value: 0x1234,
                                                class_initialized,
                                                flag_b,
                                                encounter,
                                                strawberry_initialized,
                                            };
                                            let g = Graph::new(&s);
                                            let ratio = f32::from_bits(ratios[i]);
                                            let mut rec = Recorder {
                                                env: FpEnv::new(0, 0),
                                                ratio,
                                                candles: 3,
                                                events: Vec::new(),
                                            };
                                            let result = match stat {
                                                AttackStat::Physical => {
                                                    g.param().calc_attack(&mut rec)
                                                }
                                                AttackStat::Magic => {
                                                    g.param().calc_magic_attack(&mut rec)
                                                }
                                            };
                                            let (events, expected) = model(&s, ratio, 3);
                                            assert_eq!(
                                                rec.events, events,
                                                "{stat:?} flag {flag} len {length} player {player}"
                                            );
                                            match expected {
                                                Some(v) => assert_eq!(result, Ok(v)),
                                                None => assert_eq!(result, Err(GuestException)),
                                            }
                                            // The port's own stores: the method flag when it was clear, the static-field flag when it was.
                                            let stores = STORES.with(|st| st.borrow().len());
                                            let reached_flag_b = player == 0
                                                && u64::from(length) > stat.add_status_index();
                                            let expected_stores = usize::from(flag & 1 == 0)
                                                + usize::from(reached_flag_b && flag_b == 0);
                                            assert_eq!(stores, expected_stores);
                                            cases += 1;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    assert!(cases > 10_000, "{cases}");
}
