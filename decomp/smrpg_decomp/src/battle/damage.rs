//! The small methods around `Battle.SkillExecter.damage_calc` (task-56): the action command ratios, the barrier test and
//! the damage guard flag. They are the callees of the damage formula that only read a few fields and store results into
//! the skill result object, so they can be compared with the lifted body by the replay mechanism plus the store journal
//! (`docs/DECOMP.md`, "Pre-state and store journal"). The formula driver itself is not ported yet, see "damage_calc
//! (task-56)" in that file for the call-site breakdown and what is left.
//!
//! In C# terms (names are the IL2CPP ones):
//!
//! ```text
//! void SkillExecter.set_acction_command_ratio() {
//!     _cur_skill_result._hosei_ratio = 1f;
//!     _cur_skill_result._is_renzoku_cmd_skill = false;
//!     if (!_acter._is_player) {
//!         _cur_skill_result._action_command_succeed_ratio = -0.5f;
//!         _cur_skill_result._action_command_sweetspot_ratio = -1f;
//!         if (_skill_obj._skill_id == 727) _cur_skill_result._action_command_succeed_ratio = -0f;
//!         return;
//!     }
//!     switch (_skill_obj._skill_id) { ...a table of skill ids, see `CommandRule::of`... }
//! }
//! bool BattleCharacterParam.is_barriered() => _buff_mng._barrier_num > 0;
//! void BattleCharacterParam.use_damage_guard() { _result_logger._is_use_barrior = true; }
//! ```
//!
//! The compiled `set_acction_command_ratio` jumps through a 129 entry byte table indexed by `skill id - 1` (any id whose
//! `id - 1` is above 0x80 as an unsigned number takes the default). The table is spelled out as the `match` in
//! `CommandRule::of`; ids that are not listed take the default rule, which zeroes both ratios.
//!
//! None of the three flushes a register: a port that leaves the register file alone is exact. Its only effects are
//! guest stores, made through [`GuestStore`] (a real store, or a journal entry inside a replay trial).

use super::{BattleCharacterParam, BuffManager, GuestObject};
use crate::bridge::prestate;
use crate::object::guest_class;

/// Where the port's stores go. The shim uses [`Journal`]; a unit test substitutes a log.
pub trait GuestStore {
    fn store_u8(&mut self, address: u64, value: u8);
    fn store_u32(&mut self, address: u64, value: u32);
}

/// Real stores outside a replay trial, journal entries inside one (see `prestate::store_u8`).
pub struct Journal;

impl GuestStore for Journal {
    fn store_u8(&mut self, address: u64, value: u8) {
        prestate::store_u8(address, value);
    }

    fn store_u32(&mut self, address: u64, value: u32) {
        prestate::store_u32(address, value);
    }
}

guest_class!(
    /// `Battle.SkillObject`
    SkillObject
);
impl SkillObject {
    const SKILL_ID: u64 = 0x14;

    pub fn skill_id(self) -> i32 {
        self.i32_field(Self::SKILL_ID)
    }
}

guest_class!(
    /// `Battle.SkillResultOneSkill`: the result record of the skill being executed. Only the fields written here.
    SkillResultOneSkill
);
impl SkillResultOneSkill {
    const IS_GENO_HOARDING_SKILL: u64 = 0x22;
    const ACTION_COMMAND_SUCCEED_RATIO: u64 = 0x50;
    const ACTION_COMMAND_SWEETSPOT_RATIO: u64 = 0x54;
    const HOSEI_RATIO: u64 = 0x58;
    const IS_RENZOKU_CMD_SKILL: u64 = 0x5c;

    fn set_f32<S: GuestStore>(self, store: &mut S, offset: u64, value: f32) {
        store.store_u32(self.address().wrapping_add(offset), value.to_bits());
    }

    fn set_bool<S: GuestStore>(self, store: &mut S, offset: u64, value: bool) {
        store.store_u8(self.address().wrapping_add(offset), u8::from(value));
    }
}

guest_class!(
    /// `Battle.SkillExecter`
    SkillExecter
);
impl SkillExecter {
    const CUR_SKILL_RESULT: u64 = 0x18;
    const ACTER: u64 = 0x20;
    const SKILL_OBJ: u64 = 0x30;

    pub fn cur_skill_result(self) -> SkillResultOneSkill {
        self.object_field(Self::CUR_SKILL_RESULT)
    }

    pub fn acter(self) -> BattleCharacterParam {
        self.object_field(Self::ACTER)
    }

    pub fn skill_obj(self) -> SkillObject {
        self.object_field(Self::SKILL_OBJ)
    }
}

/// `SkillObject._skill_id` of the one enemy skill that has its own action command ratio.
const ENEMY_SKILL_WITH_OWN_RATIO: i32 = 727;

/// What a player skill does to the result record. A rule is a set of stores; `ratios` is
/// (succeed ratio, sweetspot ratio) when the rule writes both.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum CommandRule {
    /// Both ratios 0 (the default: most ids).
    None,
    /// Succeed 0.5, sweetspot 1.0.
    Half,
    /// Succeed 0.25, sweetspot 0.5.
    Quarter,
    /// Succeed 0.25, sweetspot 0.25.
    QuarterBoth,
    /// Succeed 0.25, sweetspot 0.25, and the skill is a Geno hoarding skill.
    QuarterBothGeno,
    /// Succeed 0.5, sweetspot 0.5.
    HalfBoth,
    /// Succeed `succeed`, a renzoku (repeated command) skill; the sweetspot ratio is not written.
    Renzoku { succeed: f32 },
    /// A renzoku skill with a hosei (correction) ratio `hosei`; succeed 0.0625.
    RenzokuHosei { hosei: f32 },
}

impl CommandRule {
    /// The rule for a player skill id (the original's byte table, entry `id - 1`).
    pub fn of(skill_id: i32) -> Self {
        match skill_id {
            1 => Self::Half,
            103 | 110 | 112 | 122 => Self::Quarter,
            104 | 108 => Self::RenzokuHosei {
                hosei: f32::from_bits(0x3f90_a3d7),
            },
            106 | 123 => Self::RenzokuHosei {
                hosei: f32::from_bits(0x3f99_999a),
            },
            105 | 107 | 113 => Self::Renzoku { succeed: 0.125 },
            114 | 120 | 121 | 129 => Self::Renzoku { succeed: 0.0625 },
            109 | 124 | 125 => Self::QuarterBoth,
            115 | 118 | 119 => Self::QuarterBothGeno,
            127 => Self::HalfBoth,
            _ => Self::None,
        }
    }
}

impl SkillExecter {
    /// `set_acction_command_ratio` (the original's spelling): reset the result's ratios for the skill being executed.
    pub fn set_acction_command_ratio<S: GuestStore>(self, store: &mut S) {
        let result = self.cur_skill_result();
        result.set_f32(store, SkillResultOneSkill::HOSEI_RATIO, 1.0);
        result.set_bool(store, SkillResultOneSkill::IS_RENZOKU_CMD_SKILL, false);
        let succeed = SkillResultOneSkill::ACTION_COMMAND_SUCCEED_RATIO;
        let sweetspot = SkillResultOneSkill::ACTION_COMMAND_SWEETSPOT_RATIO;
        if self.acter().is_player_byte() == 0 {
            result.set_f32(store, succeed, -0.5);
            result.set_f32(store, sweetspot, -1.0);
            if self.skill_obj().skill_id() == ENEMY_SKILL_WITH_OWN_RATIO {
                result.set_f32(store, succeed, -0.0);
            }
            return;
        }
        let rule = CommandRule::of(self.skill_obj().skill_id());
        match rule {
            CommandRule::None => {
                result.set_f32(store, succeed, 0.0);
                result.set_f32(store, sweetspot, 0.0);
            }
            CommandRule::Half => {
                result.set_f32(store, succeed, 0.5);
                result.set_f32(store, sweetspot, 1.0);
            }
            CommandRule::Quarter => {
                result.set_f32(store, succeed, 0.25);
                result.set_f32(store, sweetspot, 0.5);
            }
            CommandRule::QuarterBoth => {
                result.set_f32(store, succeed, 0.25);
                result.set_f32(store, sweetspot, 0.25);
            }
            CommandRule::QuarterBothGeno => {
                result.set_f32(store, succeed, 0.25);
                result.set_f32(store, sweetspot, 0.25);
                result.set_bool(store, SkillResultOneSkill::IS_GENO_HOARDING_SKILL, true);
            }
            CommandRule::HalfBoth => {
                result.set_f32(store, succeed, 0.5);
                result.set_f32(store, sweetspot, 0.5);
            }
            CommandRule::Renzoku { succeed: value } => {
                result.set_f32(store, succeed, value);
                result.set_bool(store, SkillResultOneSkill::IS_RENZOKU_CMD_SKILL, true);
            }
            CommandRule::RenzokuHosei { hosei } => {
                result.set_f32(store, succeed, 0.0625);
                result.set_f32(store, SkillResultOneSkill::HOSEI_RATIO, hosei);
                result.set_bool(store, SkillResultOneSkill::IS_RENZOKU_CMD_SKILL, true);
            }
        }
    }
}

impl BuffManager {
    const BARRIER_NUM: u64 = 0x2c;

    /// Barriers (damage guards) the character still has.
    pub fn barrier_num(self) -> i32 {
        self.i32_field(Self::BARRIER_NUM)
    }
}

guest_class!(
    /// `Battle.CharaSkillResultLogger`: only the flag `use_damage_guard` sets.
    CharaSkillResultLogger
);
impl CharaSkillResultLogger {
    const IS_USE_BARRIOR: u64 = 0x49;
}

impl BattleCharacterParam {
    const RESULT_LOGGER: u64 = 0xb8;

    pub(crate) fn result_logger(self) -> CharaSkillResultLogger {
        self.object_field(Self::RESULT_LOGGER)
    }

    /// `is_barriered`: at least one barrier left.
    pub fn is_barriered(self) -> bool {
        self.buff_mng().barrier_num() > 0
    }

    /// `use_damage_guard`: the logger records that a barrier absorbed the hit.
    pub fn use_damage_guard<S: GuestStore>(self, store: &mut S) {
        store.store_u8(
            self.result_logger()
                .address()
                .wrapping_add(CharaSkillResultLogger::IS_USE_BARRIOR),
            1,
        );
    }
}

// ---- Shim ----

/// 0x71021c3bb0 `BattleCharacterParam.is_barriered`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_param_is_barriered(this: u64, _: u64, _: u64) -> u64 {
    u64::from(unsafe { BattleCharacterParam::at(this) }.is_barriered())
}

/// 0x71021f7ea0 `SkillExecter.set_acction_command_ratio` (replay row with a store journal).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_skill_set_acction_command_ratio(x: *mut u64) {
    let this = unsafe { SkillExecter::at(x.read()) };
    this.set_acction_command_ratio(&mut Journal);
}

/// Pre-state hook of `set_acction_command_ratio`: the cells of the result record the port stores to.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_skill_set_acction_command_ratio_pre(x: *mut u64) -> i32 {
    let result = unsafe { SkillExecter::at(x.read()) }.cur_skill_result();
    let base = result.address();
    for (offset, size) in [
        (SkillResultOneSkill::IS_GENO_HOARDING_SKILL, 1),
        (SkillResultOneSkill::ACTION_COMMAND_SUCCEED_RATIO, 4),
        (SkillResultOneSkill::ACTION_COMMAND_SWEETSPOT_RATIO, 4),
        (SkillResultOneSkill::HOSEI_RATIO, 4),
        (SkillResultOneSkill::IS_RENZOKU_CMD_SKILL, 1),
    ] {
        prestate::snapshot(base.wrapping_add(offset), size, true);
    }
    0
}

/// 0x71021be280 `BattleCharacterParam.use_damage_guard` (replay row with a store journal).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_param_use_damage_guard(x: *mut u64) {
    unsafe { BattleCharacterParam::at(x.read()) }.use_damage_guard(&mut Journal);
}

/// Pre-state hook of `use_damage_guard`: the flag byte the port stores.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_param_use_damage_guard_pre(x: *mut u64) -> i32 {
    let logger = unsafe { BattleCharacterParam::at(x.read()) }.result_logger();
    prestate::snapshot(
        logger
            .address()
            .wrapping_add(CharaSkillResultLogger::IS_USE_BARRIOR),
        1,
        true,
    );
    0
}

#[cfg(test)]
mod tests;
