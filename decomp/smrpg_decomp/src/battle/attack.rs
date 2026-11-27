//! `BattleCharacterParam.get_calc_attack` and `get_calc_magic_attack` (task-55): the attack stat the damage code reads.
//!
//! In C# terms (names are the IL2CPP ones):
//!
//! ```text
//! int get_calc_attack() {
//!     int sum = _attack + _buff_mng._add_status[0];
//!     if (_is_player) {
//!         int bonus = _weapon._is_exist ? _weapon._base_value + Random.Range(_weapon._random_min, _weapon._random_max) : 0;
//!         return (int)((bonus + sum) * _buff_mng.attack_up_ratio());
//!     }
//!     if (BattleManager._instance._encounter_id == 2076) sum += BattleEventParam_StrawberryAdd.get_on_candle_num() * 5;
//!     return sum;
//! }
//! ```
//!
//! `get_calc_magic_attack` is the same with `_magic_attack`, `_add_status[2]`, `magic_attack_up_ratio` and no weapon
//! bonus. Both begin with IL2CPP's lazy initialization (a flag byte per method, then the metadata initializer for two
//! slots), and the non-player branch reads static fields, so it first tests the class-initialized word of
//! `BattleManager` and the static-field metadata flag, with a garbage collection safepoint after some of the loads.
//! All of that is spelled out in the body because the comparison sees it: every call, and every register at each call.
//!
//! Replay and pre-state: the port runs after the lifted body, so the flag bytes and class-initialized words that the
//! body (or a class initializer it called) may have written are read through `prestate` (see `docs/DECOMP.md`,
//! "Pre-state and store journal"). The hook functions at the bottom snapshot them before the lifted body runs.

use super::{
    BattleCharacterParam, BuffManager, Float32Convert, Float32Ops, GuestException, GuestObject,
    Nzcv, Regs, scale_by_ratio,
};
use crate::bridge::prestate;
use crate::object::{Int32Array, guest_class};
use crate::{guest_ld8, guest_ld32, guest_ld64};

guest_class!(
    /// `Battle.BattleCharacterWeapon`
    BattleCharacterWeapon
);
impl BattleCharacterWeapon {
    const IS_EXIST: u64 = 0x10;
    const BASE_VALUE: u64 = 0x14;
    const RANDOM_MIN: u64 = 0x18;
    const RANDOM_MAX: u64 = 0x1c;
}

guest_class!(
    /// An `Il2CppClass`: the two fields the compiled code reads for static access.
    Il2CppClass
);
impl Il2CppClass {
    const STATIC_FIELDS: u64 = 0xb8;
    /// The word the compiled code tests before it calls the class initializer (`il2cpp_runtime_class_init`).
    const INITIALIZED: u64 = 0xe0;

    fn initialized_address(self) -> u64 {
        self.address().wrapping_add(Self::INITIALIZED)
    }
}

guest_class!(
    /// `Battle.BattleManager`
    BattleManager
);
impl BattleManager {
    const ENCOUNTER_ID: u64 = 0x168;
}

guest_class!(
    /// The static fields of `Battle.BattleManager`: `_instance` at +0x10.
    BattleManagerStatics
);
impl BattleManagerStatics {
    const INSTANCE: u64 = 0x10;
}

/// `_encounter_id` of the strawberry candle battle (2076).
const STRAWBERRY_ENCOUNTER_ID: u32 = 0x81c;

/// IL2CPP metadata slots the compiled code loads (data page plus offset).
pub mod slots {
    /// `BattleEventParam_StrawberryAdd`.
    pub const STRAWBERRY_CLASS: u64 = 0x7103a59000 + 0x328;
    /// `BattleManager`.
    pub const BATTLE_MANAGER_CLASS: u64 = 0x7103a57000 + 0xe48;
    /// Pointer to the byte that says the static-field metadata of this branch has been initialized.
    pub const STATIC_FLAG_POINTER: u64 = 0x7103a58000 + 0x1d0;
}

/// Which of the two methods.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AttackStat {
    /// `get_calc_attack` (0x71021c3ea0)
    Physical,
    /// `get_calc_magic_attack` (0x71021bc2e0)
    Magic,
}

impl AttackStat {
    fn base_offset(self) -> u64 {
        match self {
            Self::Physical => BattleCharacterParam::ATTACK,
            Self::Magic => BattleCharacterParam::MAGIC_ATTACK,
        }
    }

    /// Index into `_add_status` (0 attack, 2 magic attack).
    fn add_status_index(self) -> u64 {
        match self {
            Self::Physical => 0,
            Self::Magic => 2,
        }
    }

    /// The method's lazy-initialization flag byte.
    pub fn init_flag(self) -> u64 {
        0x7103cba000
            + match self {
                Self::Physical => 0xc09,
                Self::Magic => 0xc0b,
            }
    }
}

impl BattleCharacterParam {
    const ATTACK: u64 = 0x3c;
    const MAGIC_ATTACK: u64 = 0x44;
    const WEAPON: u64 = 0xb0;

    fn weapon(self) -> BattleCharacterWeapon {
        self.object_field(Self::WEAPON)
    }
}

/// What the compiled code has loaded into registers when it calls `Random.Range`.
#[derive(Clone, Copy, Debug)]
pub struct WeaponRoll {
    pub weapon: BattleCharacterWeapon,
    pub base_value: u32,
    pub random_min: u32,
    pub random_max: u32,
}

/// The loads before the branch on `_is_player`; they stay in registers.
#[derive(Clone, Copy, Debug)]
pub struct StatLoads {
    pub base: u32,
    pub sum: u32,
    pub is_player_byte: u8,
}

/// A class as the compiled code holds it: the metadata slot's value (x20) and the class (x0).
#[derive(Clone, Copy, Debug)]
pub struct ClassView {
    pub slot_value: u64,
    pub class: Il2CppClass,
}

/// The value of a metadata slot (a game global; unit tests substitute their own).
fn read_slot(slot: u64) -> u64 {
    #[cfg(test)]
    {
        tests::slot_value(slot)
    }
    #[cfg(not(test))]
    unsafe {
        guest_ld64(slot)
    }
}

impl ClassView {
    fn load(slot: u64) -> Self {
        let slot_value = read_slot(slot);
        Self {
            slot_value,
            class: unsafe { Il2CppClass::at(guest_ld64(slot_value)) },
        }
    }
}

/// Where the compiled code calls a class initializer in the non-player branch (the register sets differ).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ClassInitSite {
    /// The first test of the `BattleManager` class found it uninitialized (x21 not loaded yet).
    FirstCheck,
    /// A later test found it uninitialized.
    SecondCheck,
    /// The strawberry class, just before `get_on_candle_num`.
    Strawberry,
}

/// Where a garbage-collection safepoint follows in the non-player branch.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PollSite {
    /// After the flag load that follows the first class initializer call (x8, x21 flushed).
    AfterFlag,
    /// After the class test that follows the metadata initializer (x0, x8 flushed).
    AfterMetadata,
    /// After the second class initializer call (x0 flushed).
    AfterSecondInit,
}

/// The game functions and register moves of the two methods. The shim `LiftedAttack` writes at each step the
/// registers the lifted code writes; the recorder in the tests only logs.
pub trait AttackCalls: Float32Convert {
    /// Prologue: frame, x19 = flag page, x20 = `this`, x8 = the flag byte (pre-state value).
    fn enter(&mut self, stat: AttackStat, this: BattleCharacterParam, flag: u8);
    /// `initialize_runtime_metadata` for the strawberry slot (`second` false) and the `BattleManager` slot (true).
    fn initialize_metadata_slot(
        &mut self,
        stat: AttackStat,
        second: bool,
    ) -> Result<(), GuestException>;
    /// `x8 = 1` (the flag store itself is the body's).
    fn mark_initialized(&mut self);
    /// x0 = `_buff_mng`, x8 = `_add_status`, x9 = its length.
    fn load_add_status(&mut self, buff: BuffManager, array: Int32Array, length: u32);
    /// The index throw (0x71000c4510): `_add_status` too short. The function ends there.
    fn throw_index_out_of_range(&mut self, stat: AttackStat) -> GuestException;
    /// x9 = `_attack`, x19 = sum, x8 = `_is_player`.
    fn load_stat(&mut self, loads: StatLoads);
    /// x8 = `_weapon`, x9 = its `_is_exist` byte.
    fn load_weapon(&mut self, weapon: BattleCharacterWeapon, is_exist_byte: u8);
    /// No weapon bonus: x21 = 0.
    fn no_weapon_bonus(&mut self);
    /// `Random.Range(min, max)` with the weapon's fields; returns x21 = `_base_value` + roll.
    fn roll_weapon(&mut self, roll: WeaponRoll) -> Result<i32, GuestException>;
    /// `attack_up_ratio` or `magic_attack_up_ratio` (x0 = the buff manager).
    fn attack_up_ratio(&mut self, stat: AttackStat) -> Result<f32, GuestException>;
    /// Physical player attack: x8 = `bonus + sum`, the value converted to float next.
    fn player_total(&mut self, total: i32);
    // ---- the non-player branch ----
    /// x20 = the class slot's value, x0 = the class, x8 = its initialized word.
    fn load_battle_manager_class(&mut self, view: ClassView, initialized: u32);
    /// A class initializer call (0x71000c4330) at `site`; the class is in x0.
    fn run_class_initializer(
        &mut self,
        stat: AttackStat,
        site: ClassInitSite,
    ) -> Result<(), GuestException>;
    /// x21 = the flag pointer, x8 = the flag byte (pre-state value).
    fn load_static_flag(&mut self, flag_pointer: u64, flag: u8);
    fn poll(&mut self, stat: AttackStat, site: PollSite);
    /// The metadata initializer for the `BattleManager` slot, then `x8 = 1` (the store is the body's).
    fn initialize_static_metadata(&mut self, stat: AttackStat) -> Result<(), GuestException>;
    /// x0 = the class again (`[x20]`), x8 = its initialized word.
    fn load_battle_manager_class_again(&mut self, initialized: u32);
    /// x0 = `[x20]` again after a class initializer ran.
    fn reload_battle_manager_class(&mut self);
    /// x8 = `_encounter_id`, after the loads through the static fields (x0 stays the class).
    fn load_encounter_id(&mut self, encounter_id: u32);
    /// x8 = the strawberry slot, x0 = its class, x8 = its initialized word.
    fn load_strawberry_class(&mut self, view: ClassView, initialized: u32);
    /// `get_on_candle_num()` with x0 = 0, then `x19 = sum + 5 * candles`; returns x19.
    fn candle_bonus(&mut self, stat: AttackStat, sum: i32) -> Result<i32, GuestException>;
    /// Return `result`.
    fn leave(&mut self, result: i32);
}

/// A class initialized word: a word read before any class initializer ran in this call has the pre-state value, a
/// word read after one has the value the call left (the initializer is what changes it).
fn initialized_word(class: Il2CppClass, initializer_ran: bool) -> u32 {
    if initializer_ran {
        unsafe { guest_ld32(class.initialized_address()) }
    } else {
        prestate::read_u32(class.initialized_address())
    }
}

impl BattleCharacterParam {
    /// `get_calc_attack`.
    pub fn calc_attack<C: AttackCalls>(self, calls: &mut C) -> Result<i32, GuestException> {
        self.calc_attack_stat(AttackStat::Physical, calls)
    }

    /// `get_calc_magic_attack`.
    pub fn calc_magic_attack<C: AttackCalls>(self, calls: &mut C) -> Result<i32, GuestException> {
        self.calc_attack_stat(AttackStat::Magic, calls)
    }

    fn calc_attack_stat<C: AttackCalls>(
        self,
        stat: AttackStat,
        calls: &mut C,
    ) -> Result<i32, GuestException> {
        let flag = prestate::read_u8(stat.init_flag());
        calls.enter(stat, self, flag);
        if flag & 1 == 0 {
            calls.initialize_metadata_slot(stat, false)?;
            calls.initialize_metadata_slot(stat, true)?;
            calls.mark_initialized();
            prestate::store_u8(stat.init_flag(), 1);
        }
        let buff = self.buff_mng();
        let added_status = buff.add_status();
        let length = added_status.len();
        calls.load_add_status(buff, added_status, length);
        if u64::from(length) <= stat.add_status_index() {
            return Err(calls.throw_index_out_of_range(stat));
        }
        let base = self.u32_field(stat.base_offset());
        let added = added_status.element(stat.add_status_index()) as u32;
        let sum = base.wrapping_add(added);
        let is_player_byte = self.is_player_byte();
        calls.load_stat(StatLoads {
            base,
            sum,
            is_player_byte,
        });
        let sum = sum as i32;
        let result = if is_player_byte != 0 {
            self.player_attack(stat, sum, calls)?
        } else {
            self.non_player_attack(stat, sum, calls)?
        };
        calls.leave(result);
        Ok(result)
    }

    /// `(int)((bonus + sum) * attack_up_ratio())`; the weapon bonus is physical attack only.
    fn player_attack<C: AttackCalls>(
        self,
        stat: AttackStat,
        sum: i32,
        calls: &mut C,
    ) -> Result<i32, GuestException> {
        let mut total = sum;
        if stat == AttackStat::Physical {
            let weapon = self.weapon();
            let is_exist_byte = weapon.u8_field(BattleCharacterWeapon::IS_EXIST);
            calls.load_weapon(weapon, is_exist_byte);
            if is_exist_byte == 0 {
                calls.no_weapon_bonus();
            } else {
                let bonus = calls.roll_weapon(WeaponRoll {
                    weapon,
                    base_value: weapon.u32_field(BattleCharacterWeapon::BASE_VALUE),
                    random_min: weapon.u32_field(BattleCharacterWeapon::RANDOM_MIN),
                    random_max: weapon.u32_field(BattleCharacterWeapon::RANDOM_MAX),
                })?;
                total = bonus.wrapping_add(sum);
            }
        }
        let ratio = calls.attack_up_ratio(stat)?;
        if stat == AttackStat::Physical {
            calls.player_total(total);
        }
        Ok(scale_by_ratio(total, ratio, calls))
    }

    /// The strawberry candle battle adds 5 attack per lit candle; every other encounter leaves the sum alone.
    fn non_player_attack<C: AttackCalls>(
        self,
        stat: AttackStat,
        sum: i32,
        calls: &mut C,
    ) -> Result<i32, GuestException> {
        let view = ClassView::load(slots::BATTLE_MANAGER_CLASS);
        let class = view.class;
        let initialized = initialized_word(class, false);
        calls.load_battle_manager_class(view, initialized);
        let mut initializer_ran = false;
        if initialized == 0 {
            calls.run_class_initializer(stat, ClassInitSite::FirstCheck)?;
            initializer_ran = true;
        }
        let flag_pointer = read_slot(slots::STATIC_FLAG_POINTER);
        let flag = prestate::read_u8(flag_pointer);
        calls.load_static_flag(flag_pointer, flag);
        if initializer_ran {
            calls.poll(stat, PollSite::AfterFlag);
        }
        let after_metadata = flag == 0;
        if after_metadata {
            calls.initialize_static_metadata(stat)?;
            prestate::store_u8(flag_pointer, 1);
        }
        let initialized = initialized_word(class, initializer_ran);
        calls.load_battle_manager_class_again(initialized);
        if after_metadata {
            calls.poll(stat, PollSite::AfterMetadata);
        }
        if initialized == 0 {
            calls.run_class_initializer(stat, ClassInitSite::SecondCheck)?;
            calls.reload_battle_manager_class();
            calls.poll(stat, PollSite::AfterSecondInit);
        }
        // `BattleManager._instance._encounter_id`
        let statics: BattleManagerStatics = class.object_field(Il2CppClass::STATIC_FIELDS);
        let manager: BattleManager = statics.object_field(BattleManagerStatics::INSTANCE);
        let encounter_id = manager.u32_field(BattleManager::ENCOUNTER_ID);
        calls.load_encounter_id(encounter_id);
        if encounter_id != STRAWBERRY_ENCOUNTER_ID {
            return Ok(sum);
        }
        let strawberry_view = ClassView::load(slots::STRAWBERRY_CLASS);
        let strawberry_initialized = initialized_word(strawberry_view.class, false);
        calls.load_strawberry_class(strawberry_view, strawberry_initialized);
        if strawberry_initialized == 0 {
            calls.run_class_initializer(stat, ClassInitSite::Strawberry)?;
        }
        calls.candle_bonus(stat, sum)
    }
}

// ---- Shim ----

/// Addresses of the lifted code the two methods call, and the address after each `bl` / safepoint.
mod site {
    use super::{AttackStat, ClassInitSite, PollSite};

    pub const INITIALIZE_METADATA: u64 = 0x71000c4150;
    pub const CLASS_INITIALIZER: u64 = 0x71000c4330;
    pub const THROW_INDEX: u64 = 0x71000c4510;
    pub const RANDOM_RANGE: u64 = 0x7101797f10;
    pub const CANDLE_NUM: u64 = 0x7101bf7a10;
    pub const RANDOM_RET: u64 = 0x71021c3f20;

    pub fn ratio_target(stat: AttackStat) -> u64 {
        match stat {
            AttackStat::Physical => 0x71021c4070,
            AttackStat::Magic => 0x71021c43d0,
        }
    }

    /// (physical, magic)
    fn pick(stat: AttackStat, pair: (u64, u64)) -> u64 {
        match stat {
            AttackStat::Physical => pair.0,
            AttackStat::Magic => pair.1,
        }
    }

    pub fn init_ret(stat: AttackStat, second: bool) -> u64 {
        if second {
            pick(stat, (0x71021c3ed8, 0x71021bc318))
        } else {
            pick(stat, (0x71021c3ecc, 0x71021bc30c))
        }
    }

    pub fn ratio_ret(stat: AttackStat) -> u64 {
        pick(stat, (0x71021c3fa4, 0x71021bc34c))
    }

    pub fn class_init_ret(stat: AttackStat, site: ClassInitSite) -> u64 {
        match site {
            ClassInitSite::FirstCheck => pick(stat, (0x71021c3fe0, 0x71021bc3f4)),
            ClassInitSite::SecondCheck => pick(stat, (0x71021c4014, 0x71021bc428)),
            ClassInitSite::Strawberry => pick(stat, (0x71021c3f88, 0x71021bc3cc)),
        }
    }

    pub fn metadata_ret(stat: AttackStat) -> u64 {
        pick(stat, (0x71021c3ffc, 0x71021bc410))
    }

    pub fn candle_ret(stat: AttackStat) -> u64 {
        pick(stat, (0x71021c3f90, 0x71021bc3d4))
    }

    pub fn throw_ret(stat: AttackStat) -> u64 {
        pick(stat, (0x71021c4020, 0x71021bc434))
    }

    pub fn poll_pc(stat: AttackStat, site: PollSite) -> u64 {
        match site {
            PollSite::AfterFlag => pick(stat, (0x71021c3fec, 0x71021bc400)),
            PollSite::AfterMetadata => pick(stat, (0x71021c400c, 0x71021bc420)),
            PollSite::AfterSecondInit => pick(stat, (0x71021c4018, 0x71021bc42c)),
        }
    }
}

/// Registers flushed at each call (read off the lifted C: the lifter writes a register to the context only where
/// it changed since the last flush; `sp` is always written).
mod flush {
    pub const INIT_FIRST: &[usize] = &[0, 8, 19, 20, 29, 30];
    pub const INIT_SECOND: &[usize] = &[0, 30];
    pub const RANDOM: &[usize] = &[0, 1, 2, 8, 9, 19, 20, 21, 29, 30];
    pub const WITH_X21: &[usize] = &[0, 8, 9, 19, 20, 21, 29, 30];
    pub const WITHOUT_X21: &[usize] = &[0, 8, 9, 19, 20, 29, 30];
}

/// The lifted function's locals (`u64 x0, x1, ...`) and the context the shim writes them to. The prologue saves
/// x19, x20, x21, x29 and sp, and the epilogue restores them, so the entry values are kept.
struct LiftedAttack {
    regs: Regs,
    x: [u64; 32],
    entry: [u64; 32],
    buff: u64,
}

const FLAG_PAGE: u64 = 0x7103cba000;
const POSITIVE_INFINITY_BITS: u64 = 0x7f80_0000;

impl LiftedAttack {
    fn new(regs: Regs) -> Self {
        let mut x = [0u64; 32];
        for (n, slot) in x.iter_mut().enumerate() {
            *slot = regs.x(n);
        }
        Self {
            regs,
            entry: x,
            x,
            buff: 0,
        }
    }

    /// Write the listed locals and sp to the context, as the lifted code does before a call.
    fn flush(&mut self, list: &[usize]) {
        for &n in list {
            self.regs.set_x(n, self.x[n]);
        }
        self.regs.set_x(31, self.x[31]);
    }

    /// `x30 = return_address`, flush, call, and reload x0, x1 (v0 and v1 live in the context in this model).
    fn call(
        &mut self,
        target: u64,
        return_address: u64,
        list: &[usize],
    ) -> Result<(), GuestException> {
        self.x[30] = return_address;
        self.flush(list);
        self.regs.invoke(target)?;
        self.x[0] = self.regs.x(0);
        self.x[1] = self.regs.x(1);
        Ok(())
    }
}

impl Float32Ops for LiftedAttack {
    fn fadd(&mut self, a: f32, b: f32) -> f32 {
        let r = self.regs.f32_arith(2, a, b);
        self.regs.set_f32(0, r);
        r
    }
    fn fsub(&mut self, a: f32, b: f32) -> f32 {
        let r = self.regs.f32_arith(3, a, b);
        self.regs.set_f32(0, r);
        r
    }
    /// `fmul s0, s0, s1`.
    fn fmul(&mut self, a: f32, b: f32) -> f32 {
        let r = self.regs.f32_arith(0, a, b);
        self.regs.set_f32(0, r);
        r
    }
}

impl Float32Convert for LiftedAttack {
    /// `scvtf s1, w8` (physical) or `scvtf s1, w19` (magic).
    fn float_from_i32(&mut self, value: i32) -> f32 {
        let r = self.regs.f32_from_i32(value);
        self.regs.set_f32(1, r);
        r
    }
    /// `fcvtzs w8, s0`.
    fn trunc_to_i32(&mut self, value: f32) -> i32 {
        let r = self.regs.i32_from_f32_trunc(value);
        self.x[8] = u64::from(r as u32);
        r
    }
    /// `mov w9, #0x7f800000`, `fmov s1, w9`, `fcmp s0, s1`, then the saturating `csel` (`mov w9, #0x80000000`).
    fn compare(&mut self, a: f32, b: f32) -> Nzcv {
        self.x[9] = POSITIVE_INFINITY_BITS;
        self.regs.set_f32(1, b);
        let flags = self.regs.f32_compare(a, b);
        self.x[9] = 0x8000_0000;
        flags
    }
}

impl AttackCalls for LiftedAttack {
    fn enter(&mut self, _: AttackStat, this: BattleCharacterParam, flag: u8) {
        self.x[31] = self.entry[31].wrapping_sub(48);
        self.x[29] = self.x[31];
        self.x[19] = FLAG_PAGE;
        self.x[20] = this.address();
        self.x[8] = u64::from(flag);
    }

    fn initialize_metadata_slot(
        &mut self,
        stat: AttackStat,
        second: bool,
    ) -> Result<(), GuestException> {
        let slot = if second {
            slots::BATTLE_MANAGER_CLASS
        } else {
            slots::STRAWBERRY_CLASS
        };
        self.x[0] = read_slot(slot);
        let list = if second {
            flush::INIT_SECOND
        } else {
            flush::INIT_FIRST
        };
        self.call(
            site::INITIALIZE_METADATA,
            site::init_ret(stat, second),
            list,
        )
    }

    fn mark_initialized(&mut self) {
        self.x[8] = 1;
    }

    fn load_add_status(&mut self, buff: BuffManager, array: Int32Array, length: u32) {
        self.buff = buff.address();
        self.x[0] = buff.address();
        self.x[8] = array.address();
        self.x[9] = u64::from(length);
    }

    fn throw_index_out_of_range(&mut self, stat: AttackStat) -> GuestException {
        // If the thrower returns without an exception the compiled function simply ends: no epilogue.
        let _ = self.call(site::THROW_INDEX, site::throw_ret(stat), flush::WITHOUT_X21);
        GuestException
    }

    fn load_stat(&mut self, loads: StatLoads) {
        self.x[9] = u64::from(loads.base);
        self.x[19] = u64::from(loads.sum);
        self.x[8] = u64::from(loads.is_player_byte);
    }

    fn load_weapon(&mut self, weapon: BattleCharacterWeapon, is_exist_byte: u8) {
        self.x[8] = weapon.address();
        self.x[9] = u64::from(is_exist_byte);
    }

    fn no_weapon_bonus(&mut self) {
        self.x[21] = 0;
    }

    fn roll_weapon(&mut self, roll: WeaponRoll) -> Result<i32, GuestException> {
        self.x[21] = u64::from(roll.base_value);
        self.x[0] = u64::from(roll.random_min);
        self.x[2] = 0;
        self.x[1] = u64::from(roll.random_max);
        self.call(site::RANDOM_RANGE, site::RANDOM_RET, flush::RANDOM)?;
        // `add w21, w0, w21`, then `x0 = _buff_mng` again
        self.x[21] = u64::from((self.x[0] as u32).wrapping_add(self.x[21] as u32));
        self.x[0] = self.buff;
        Ok(self.x[21] as u32 as i32)
    }

    fn attack_up_ratio(&mut self, stat: AttackStat) -> Result<f32, GuestException> {
        let list = match stat {
            AttackStat::Physical => flush::WITH_X21,
            AttackStat::Magic => flush::WITHOUT_X21,
        };
        self.call(site::ratio_target(stat), site::ratio_ret(stat), list)?;
        Ok(self.regs.f32_result())
    }

    fn player_total(&mut self, total: i32) {
        self.x[8] = u64::from(total as u32);
    }

    fn load_battle_manager_class(&mut self, view: ClassView, initialized: u32) {
        self.x[20] = view.slot_value;
        self.x[0] = view.class.address();
        self.x[8] = u64::from(initialized);
    }

    fn run_class_initializer(
        &mut self,
        stat: AttackStat,
        site: ClassInitSite,
    ) -> Result<(), GuestException> {
        let list = match site {
            ClassInitSite::FirstCheck => flush::WITHOUT_X21,
            _ => flush::WITH_X21,
        };
        self.call(
            site::CLASS_INITIALIZER,
            site::class_init_ret(stat, site),
            list,
        )
    }

    fn load_static_flag(&mut self, flag_pointer: u64, flag: u8) {
        self.x[21] = read_slot(slots::STATIC_FLAG_POINTER);
        debug_assert_eq!(self.x[21], flag_pointer);
        self.x[8] = u64::from(flag);
    }

    fn poll(&mut self, stat: AttackStat, point: PollSite) {
        let pc = site::poll_pc(stat, point);
        let flushed: &[(u32, u64)] = match point {
            PollSite::AfterFlag => &[(8, self.x[8]), (21, self.x[21])],
            PollSite::AfterMetadata => &[(0, self.x[0]), (8, self.x[8])],
            PollSite::AfterSecondInit => &[(0, self.x[0])],
        };
        self.regs.safepoint_n(pc, flushed);
    }

    fn initialize_static_metadata(&mut self, stat: AttackStat) -> Result<(), GuestException> {
        self.x[0] = read_slot(slots::BATTLE_MANAGER_CLASS);
        self.call(
            site::INITIALIZE_METADATA,
            site::metadata_ret(stat),
            flush::WITH_X21,
        )?;
        self.x[8] = 1;
        Ok(())
    }

    fn load_battle_manager_class_again(&mut self, initialized: u32) {
        self.x[0] = unsafe { guest_ld64(self.x[20]) };
        self.x[8] = u64::from(initialized);
    }

    fn reload_battle_manager_class(&mut self) {
        self.x[0] = unsafe { guest_ld64(self.x[20]) };
    }

    fn load_encounter_id(&mut self, encounter_id: u32) {
        self.x[8] = u64::from(encounter_id);
    }

    fn load_strawberry_class(&mut self, view: ClassView, initialized: u32) {
        self.x[0] = view.class.address();
        self.x[8] = u64::from(initialized);
    }

    fn candle_bonus(&mut self, stat: AttackStat, sum: i32) -> Result<i32, GuestException> {
        self.x[0] = 0;
        self.call(site::CANDLE_NUM, site::candle_ret(stat), flush::WITH_X21)?;
        // `add w8, w0, w0, lsl #2`, `add w19, w8, w19`
        let candles = self.x[0] as u32;
        let five_times = candles.wrapping_add(candles << 2);
        self.x[8] = u64::from(five_times);
        self.x[19] = u64::from(five_times.wrapping_add(sum as u32));
        Ok(self.x[19] as u32 as i32)
    }

    fn leave(&mut self, result: i32) {
        // `mov w0, w19`, then the epilogue restores x19, x20, x21, x29 and sp from the frame; x0, those four, v0, v1
        // and sp are flushed.
        self.regs.set_x(0, u64::from(result as u32));
        for n in [19, 20, 21, 29] {
            self.regs.set_x(n, self.entry[n]);
        }
        self.regs.set_x(31, self.entry[31]);
    }
}

fn run(x: *mut u64, stat: AttackStat) {
    unsafe {
        let regs = Regs::from_raw(x);
        let param = BattleCharacterParam::at(regs.x(0));
        let mut lifted = LiftedAttack::new(regs);
        let _ = match stat {
            AttackStat::Physical => param.calc_attack(&mut lifted),
            AttackStat::Magic => param.calc_magic_attack(&mut lifted),
        };
    }
}

/// 0x71021c3ea0 `BattleCharacterParam.get_calc_attack`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_param_calc_attack(x: *mut u64) {
    run(x, AttackStat::Physical);
}

/// 0x71021bc2e0 `BattleCharacterParam.get_calc_magic_attack`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_param_calc_magic_attack(x: *mut u64) {
    run(x, AttackStat::Magic);
}

// ---- Pre-state hooks (swap.c, QLEAF): run before the lifted body ----

/// Snapshot the cells the port reads or writes. Returns 1 when the method's flag says its lazy initialization has
/// not run yet (the metadata slots are unresolved, so nothing below can be read): that call is not compared.
fn capture(x: *mut u64, stat: AttackStat) -> i32 {
    let flag = unsafe { guest_ld8(stat.init_flag()) };
    prestate::snapshot(stat.init_flag(), 1, true);
    if flag & 1 == 0 {
        return 1;
    }
    let param = unsafe { BattleCharacterParam::at(Regs::from_raw(x).x(0)) };
    if param.is_player_byte() != 0 {
        return 0;
    }
    let battle_manager = ClassView::load(slots::BATTLE_MANAGER_CLASS).class;
    prestate::snapshot(battle_manager.initialized_address(), 4, false);
    let flag_pointer = read_slot(slots::STATIC_FLAG_POINTER);
    prestate::snapshot(flag_pointer, 1, true);
    let strawberry = ClassView::load(slots::STRAWBERRY_CLASS).class;
    prestate::snapshot(strawberry.initialized_address(), 4, false);
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_param_calc_attack_pre(x: *mut u64) -> i32 {
    capture(x, AttackStat::Physical)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_param_calc_magic_attack_pre(x: *mut u64) -> i32 {
    capture(x, AttackStat::Magic)
}

#[cfg(test)]
pub(crate) mod tests;
