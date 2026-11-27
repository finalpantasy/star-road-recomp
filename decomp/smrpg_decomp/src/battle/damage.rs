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

use super::belome::{
    ListInt, smrpg_skill_calc_belome_barrior_ratio, smrpg_skill_calc_belome_barrior_ratio_pre,
};
use super::element::smrpg_param_calc_element;
use super::frame::Frame;
use super::renkei::{smrpg_skill_calc_renkei_ratio, smrpg_skill_calc_renkei_ratio_pre};
use super::{BattleCharacterParam, BuffManager, GuestException, GuestObject, Regs};
use crate::bridge::prestate;
use crate::object::guest_class;
use crate::{guest_ld8, guest_ld32, guest_ld64};

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
    const SKILL_TYPE: u64 = 0x20;

    /// `_skill_type` (`SkillObject.SkillType`, an int enum).
    pub fn skill_type(self) -> u32 {
        self.u32_field(Self::SKILL_TYPE)
    }

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

// ---- group_a_b (task-59) ----
//
// `Battle.SkillExecter.group_a_b(ref int damage)` (0x7102201aa0, 390 instructions, `QLEAF`) is the last step of the damage
// formula before the result is stored: the buff and status groups of acter and target scale the damage so far. In C# terms
// (names are IL2CPP's; every `_monitor.write(..)` line is a debug call that does nothing visible):
//
// ```text
// void group_a_b(ref int damage) {
//     if (_skill_obj._skill_type == SkillType.RENKEI) {
//         if (!_target.is_defense_up() && !_target._is_guard) {
//             damage = (int)(((float)CountOf(p => p.is_fear()) / -3f * 0.5f + 1f) * (float)damage);
//             _monitor.write(<text> + <the factor>.ToString());
//         } else {
//             damage = damage / 2;                                                 // signed: `cinc lt; asr 1`
//         }
//         _monitor.write(<text> + damage.ToString());
//         if (_target.is_fear()) damage = damage * 3 / 2;
//         else damage = (int)(((float)CountOf(p => p.is_attack_up()) / 3f * 0.5f + 1f) * (float)damage);
//         _monitor.write(<text> + damage.ToString());
//     } else {
//         if (_acter.is_fear() || _target.is_defense_up() || _target._is_guard) {
//             damage = damage / 2;
//             _monitor.write(<text> + damage.ToString());
//         }
//         if (_acter.is_attack_up() || _target.is_fear()) {
//             damage = damage * 3 / 2;
//             _monitor.write(<text> + damage.ToString());
//         }
//     }
// }
// ```
//
// `CountOf` is a `foreach` over `BattleManager._instance._pc_list` (a `List<BattleCharacterParam>`) that counts the
// elements the test accepts; the compiled code lazily initializes the `BattleManager` class and a static-field flag at
// each of its two copies. The scaled value `(int)f` is `fcvtzs` where the one value `+inf` is replaced by `int.MinValue`,
// as in `calc_renkei_ratio`. Exception landing pads (the enumerator's `finally`) are not ported: a callee that throws
// makes the port return at once, as the lifted body does.
//
// Like `calc_renkei_ratio` this is a replay row: the port keeps the lifted function's registers in a `Frame` and writes
// the lists the lifted code writes at each call, because the comparison looks at the whole register file at every call.

/// `SkillObject.SkillType.RENKEI`.
const SKILL_TYPE_RENKEI: u32 = 13;
/// `BattleCharacterParam._is_guard`.
const IS_GUARD: u64 = 0x76;
/// `+inf` as the compiled code builds it, and `-0` for `fcvtzs` of it.
const INFINITY_BITS: u32 = 0x7f80_0000;
/// `fcvtzs` leaves `i32::MAX` for `+inf`; the compiled code stores `int.MinValue` instead.
const INFINITY_RESULT: u32 = 0x8000_0000;
const HALF: f32 = f32::from_bits(0x3f00_0000);
const ONE: f32 = f32::from_bits(0x3f80_0000);

/// The lazy-initialization flag byte, the metadata slots and the static cells (data page + offset).
pub mod group_slots {
    pub const INIT_FLAG: u64 = 0x7103cba000 + 0xd73;
    pub const FLAG_PAGE: u64 = 0x7103cba000;
    /// `BattleManager` (the slot's value points at a cell that holds the class).
    pub const BATTLE_MANAGER_CLASS: u64 = 0x7103a57000 + 0xe48;
    /// Pointer to the byte that says the static-field metadata of this method has been initialized.
    pub const STATIC_FLAG_POINTER: u64 = 0x7103a58000 + 0x1d0;
    pub const GET_ENUMERATOR_METHOD: u64 = 0x7103a58000 + 0x240;
    pub const DISPOSE_METHOD: u64 = 0x7103a58000 + 0x248;
    pub const MOVE_NEXT_METHOD: u64 = 0x7103a58000 + 0x250;
    pub const GET_CURRENT_METHOD: u64 = 0x7103a58000 + 0x258;
    pub const STRING_DAMAGE_IS: u64 = 0x7103a83000 + 0x540;
    pub const STRING_HALVED: u64 = 0x7103a83000 + 0xc28;
    pub const STRING_FACTOR: u64 = 0x7103a83000 + 0xc30;
    /// The 8 slots the first run resolves, in the order of its 8 `initialize_runtime_metadata` calls.
    pub const INIT_ORDER: [u64; 8] = [
        BATTLE_MANAGER_CLASS,
        DISPOSE_METHOD,
        MOVE_NEXT_METHOD,
        GET_CURRENT_METHOD,
        GET_ENUMERATOR_METHOD,
        STRING_DAMAGE_IS,
        STRING_HALVED,
        STRING_FACTOR,
    ];
}

/// The lifted functions the body calls.
mod group_callee {
    pub const INITIALIZE_METADATA: u64 = 0x71000c4150;
    pub const CLASS_INITIALIZER: u64 = 0x71000c4330;
    pub const DEBUG_WRITE: u64 = 0x71018bebc0;
    pub const INT32_TO_STRING: u64 = 0x710213a840;
    pub const SINGLE_TO_STRING: u64 = 0x7101d6a050;
    pub const STRING_CONCAT_2: u64 = 0x71020147d0;
    /// `BattleCharacterParam.is_attack_up` (`_buff_mng._is_attack_up`).
    pub const IS_ATTACK_UP: u64 = 0x71021bc4b0;
    /// `BattleCharacterParam.is_defense_up` (`_buff_mng._is_defense_up`).
    pub const IS_DEFENSE_UP: u64 = 0x71021bc4c0;
    /// `BattleCharacterParam.is_fear` (`_badstatus_mng._status == 5`).
    pub const IS_FEAR: u64 = 0x71021be020;
    pub const LIST_GET_ENUMERATOR: u64 = 0x7101b52040;
    pub const ENUMERATOR_MOVE_NEXT: u64 = 0x71018fe040;
    pub const ENUMERATOR_GET_CURRENT: u64 = 0x7100183d20;
    pub const ENUMERATOR_DISPOSE: u64 = 0x71018fe030;
}

/// Registers written at call sites (x30 is always written) and the return address of each site.
mod group_site {
    use super::{CountSite, LogSite, group_callee, group_slots};

    pub const FIRST_INIT_RETURN: u64 = 0x7102201ad8;
    pub const INIT_STEP: u64 = 12;
    pub const FIRST_CALL: &[usize] = &[0, 8, 19, 20, 21, 29];
    pub const LATER_INIT_CALL: &[usize] = &[0];
    pub const CONCAT: &[usize] = &[0, 1, 2, 8];
    pub const WRITE: &[usize] = &[0, 1, 2];
    pub const PREDICATE: &[usize] = &[0, 1];
    pub const FIRST_PREDICATE: &[usize] = &[0, 1, 8, 19, 20, 21, 29];
    pub const ATTACK_UP_PREDICATE: &[usize] = &[0, 1, 8];
    pub const VECTORS: &[usize] = &[0, 1];
    pub const RETURN: &[usize] = &[19, 20, 21, 22, 23, 29];

    /// The renkei branch: `is_defense_up(target)`, the halved line, `is_fear(target)`, the closing line.
    pub const R_DEFENSE_UP_RETURN: u64 = 0x7102201b5c;
    pub const R_HALVED: LogSite = LogSite {
        monitor: 21,
        to_string_return: 0x7102201b90,
        to_string: &[0, 1, 8, 9, 21],
        vectors: Some(VECTORS),
        text: group_slots::STRING_HALVED,
        concat_return: 0x7102201ba8,
        write_return: 0x7102201bb8,
    };
    pub const R_FEAR_RETURN: u64 = 0x7102201bc4;
    /// The closing line (`damage is`), shared by every path of the renkei branch and the `3 / 2` case of the other.
    pub const CLOSING: LogSite = LogSite {
        monitor: 20,
        to_string_return: 0x7102201dcc,
        to_string: &[0, 1, 8, 9, 20],
        vectors: Some(VECTORS),
        text: group_slots::STRING_DAMAGE_IS,
        concat_return: 0x7102201de4,
        write_return: 0x7102201df4,
    };

    /// The other branch.
    pub const O_FEAR_ACTER_RETURN: u64 = 0x7102201c80;
    pub const O_DEFENSE_UP_RETURN: u64 = 0x7102201c90;
    pub const O_HALVED: LogSite = LogSite {
        monitor: 21,
        to_string_return: 0x7102201cc4,
        to_string: &[0, 1, 8, 21],
        vectors: None,
        text: group_slots::STRING_HALVED,
        concat_return: 0x7102201cdc,
        write_return: 0x7102201cec,
    };
    pub const O_ATTACK_UP_RETURN: u64 = 0x7102201cf8;
    pub const O_FEAR_TARGET_RETURN: u64 = 0x7102201d08;

    pub const POLL_AFTER_SCALE_B: u64 = 0x7102201f8c;

    /// `foreach` copy A (counts `is_attack_up`, scale 3f) and copy B (counts `is_fear`, scale -3f).
    pub const COUNT_A: CountSite = CountSite {
        class_init_return: 0x7102201ebc,
        flag_poll: 0x7102201ec8,
        metadata_return: 0x7102201ed8,
        metadata_poll: 0x7102201ee8,
        class_init_again_return: 0x7102201ef0,
        class_init_again_poll: 0x7102201ef4,
        enumerator_return: 0x7102201c1c,
        move_next_return: 0x7102201c4c,
        current_return: 0x7102201c5c,
        predicate: group_callee::IS_ATTACK_UP,
        predicate_return: 0x7102201c68,
        loop_poll: 0x7102201c70,
        dispose_return: 0x7102201d38,
        factor_slot: 0x18,
        divisor_bits: 0x4040_0000,
        to_string_return: 0x7102201d68,
        concat_return: 0x7102201d80,
        write_return: 0x7102201d90,
    };
    pub const COUNT_B: CountSite = CountSite {
        class_init_return: 0x7102201f94,
        flag_poll: 0x7102201fa0,
        metadata_return: 0x7102201fb0,
        metadata_poll: 0x7102201fc0,
        class_init_again_return: 0x7102201fc8,
        class_init_again_poll: 0x7102201fcc,
        enumerator_return: 0x7102201e60,
        move_next_return: 0x7102201e90,
        current_return: 0x7102201ea0,
        predicate: group_callee::IS_FEAR,
        predicate_return: 0x7102201eac,
        loop_poll: 0x7102201eb4,
        dispose_return: 0x7102201f0c,
        factor_slot: 0x1c,
        divisor_bits: 0xc040_0000,
        to_string_return: 0x7102201f3c,
        concat_return: 0x7102201f54,
        write_return: 0x7102201f64,
    };
}

/// One `_monitor.write(Concat(text, damage.ToString()))` line: the register and call-site lists differ per copy.
pub(crate) struct LogSite {
    monitor: usize,
    to_string_return: u64,
    to_string: &'static [usize],
    vectors: Option<&'static [usize]>,
    text: u64,
    concat_return: u64,
    write_return: u64,
}

/// One copy of the `foreach` over the character list plus the scale that follows it.
pub(crate) struct CountSite {
    class_init_return: u64,
    flag_poll: u64,
    metadata_return: u64,
    metadata_poll: u64,
    class_init_again_return: u64,
    class_init_again_poll: u64,
    enumerator_return: u64,
    move_next_return: u64,
    current_return: u64,
    predicate: u64,
    predicate_return: u64,
    loop_poll: u64,
    dispose_return: u64,
    /// The float slot in the frame: `[x29 + factor_slot]`.
    factor_slot: u64,
    divisor_bits: u32,
    to_string_return: u64,
    concat_return: u64,
    write_return: u64,
}

/// What one call of the port keeps besides the registers: the damage cell, its value, and whether the
/// lazy-initialization cells are still to be read from the pre-state (the first `foreach` of the call).
struct GroupRun {
    damage: u64,
    value: i32,
    first_visit: bool,
}

impl GroupRun {
    /// `*damage = value` (the cell is the caller's int, the journal checks the store).
    fn store(&mut self, value: u64) {
        self.value = value as u32 as i32;
        prestate::store_u32(self.damage, value as u32);
    }
}

/// `x = 32-bit value` as the lifted code leaves a `w` register: zero extended.
fn w(value: i32) -> u64 {
    u64::from(value as u32)
}

/// `cinc w8, w8, lt; asr w8, w8, #1`: a signed divide by 2 that rounds toward zero.
fn halve(value: i32) -> i32 {
    value.wrapping_add(i32::from(value < 0)) >> 1
}

/// `add w8, w8, w8, lsl #1` and then `halve`: `value * 3 / 2`.
fn three_halves(value: i32) -> i32 {
    halve(value.wrapping_add(value.wrapping_shl(1)))
}

impl BattleCharacterParam {
    /// `_is_guard` as the byte the compiled code loads.
    fn is_guard_byte(self) -> u8 {
        unsafe { guest_ld8(self.address().wrapping_add(IS_GUARD)) }
    }
}

impl SkillExecter {
    const MONITOR_FIELD: u64 = 0x10;

    fn debug_monitor(self) -> u64 {
        unsafe { guest_ld64(self.address().wrapping_add(Self::MONITOR_FIELD)) }
    }

    /// `group_a_b(ref int damage)`: `damage` is the guest address of the int.
    pub fn group_a_b(self, damage: u64, f: &mut Frame) -> Result<(), GuestException> {
        let flag = prestate::read_u8(group_slots::INIT_FLAG);
        // prologue: `sub sp, sp, #0x70`, `add x29, sp, #0x30`, x19 = damage, x20 = this, x21 = the flag page, x8 = the flag byte
        f.x[31] = f.entry[31].wrapping_sub(0x70);
        f.x[29] = f.x[31].wrapping_add(0x30);
        f.x[19] = damage;
        f.x[20] = self.address();
        f.x[21] = group_slots::FLAG_PAGE;
        f.x[8] = u64::from(flag);
        if flag & 1 == 0 {
            self.resolve_group_metadata(f)?;
            prestate::store_u8(group_slots::INIT_FLAG, 1);
        }
        let skill_type = self.skill_obj().skill_type();
        f.x[8] = u64::from(skill_type);
        let mut run = GroupRun {
            damage,
            value: prestate::read_u32(damage) as i32,
            first_visit: true,
        };
        if skill_type == SKILL_TYPE_RENKEI {
            self.group_for_renkei(f, &mut run)?;
        } else {
            self.group_for_other_skill(f, &mut run)?;
        }
        f.leave(group_site::RETURN);
        Ok(())
    }

    /// The 8 `initialize_runtime_metadata` calls of the first run, then `x8 = 1` (the flag store is the caller's).
    fn resolve_group_metadata(self, f: &mut Frame) -> Result<(), GuestException> {
        for (n, slot) in group_slots::INIT_ORDER.into_iter().enumerate() {
            f.x[0] = unsafe { guest_ld64(slot) };
            let (list, with_sp) = if n == 0 {
                (group_site::FIRST_CALL, true)
            } else {
                (group_site::LATER_INIT_CALL, false)
            };
            f.call(
                group_callee::INITIALIZE_METADATA,
                group_site::FIRST_INIT_RETURN + group_site::INIT_STEP * n as u64,
                list,
                with_sp,
            )?;
        }
        f.x[8] = 1;
        Ok(())
    }

    /// A `bool` method of a character (`is_fear`, `is_defense_up`, `is_attack_up`): x0 = the character, x1 = 0; the
    /// callee leaves its field pointer in x8, which the lifted code reloads after the call.
    fn character_test(
        f: &mut Frame,
        callee: u64,
        character: u64,
        return_address: u64,
        list: &[usize],
        with_sp: bool,
    ) -> Result<bool, GuestException> {
        f.x[0] = character;
        f.x[1] = 0;
        f.call(callee, return_address, list, with_sp)?;
        f.x[8] = f.regs.x(8);
        Ok(f.x[0] & 1 != 0)
    }

    /// `_monitor.write(Concat(text, damage.ToString()))`.
    fn log_damage(
        self,
        f: &mut Frame,
        run: &GroupRun,
        site: &LogSite,
    ) -> Result<(), GuestException> {
        f.x[site.monitor] = self.debug_monitor();
        f.x[0] = run.damage;
        f.x[1] = 0;
        let mut area = (run.value as u32).to_le_bytes();
        match site.vectors {
            Some(vectors) => f.call_out_v(
                group_callee::INT32_TO_STRING,
                site.to_string_return,
                site.to_string,
                vectors,
                false,
                &mut area,
            )?,
            None => f.call_out(
                group_callee::INT32_TO_STRING,
                site.to_string_return,
                site.to_string,
                false,
                &mut area,
            )?,
        }
        self.write_with_text(
            f,
            site.monitor,
            site.text,
            site.concat_return,
            site.write_return,
        )
    }

    /// `x8 = [slot]`, `x1 = <the string just built>`, `x0 = [x8]`, `Concat(text, string)`, then `write(monitor, result)`.
    fn write_with_text(
        self,
        f: &mut Frame,
        monitor: usize,
        text: u64,
        concat_return: u64,
        write_return: u64,
    ) -> Result<(), GuestException> {
        f.x[8] = unsafe { guest_ld64(text) };
        f.x[1] = f.x[0];
        f.x[0] = unsafe { guest_ld64(f.x[8]) };
        f.x[2] = 0;
        f.call(
            group_callee::STRING_CONCAT_2,
            concat_return,
            group_site::CONCAT,
            false,
        )?;
        f.x[1] = f.x[0];
        f.x[0] = f.x[monitor];
        f.x[2] = 0;
        f.call(
            group_callee::DEBUG_WRITE,
            write_return,
            group_site::WRITE,
            false,
        )
    }

    /// `_skill_type == RENKEI` (compiled block 0x7102201b50 onward).
    fn group_for_renkei(self, f: &mut Frame, run: &mut GroupRun) -> Result<(), GuestException> {
        let target = self.target();
        let defense_up = Self::character_test(
            f,
            group_callee::IS_DEFENSE_UP,
            target.address(),
            group_site::R_DEFENSE_UP_RETURN,
            group_site::FIRST_PREDICATE,
            true,
        )?;
        let mut scaled_by_fear_count = false;
        if !defense_up {
            f.x[8] = target.address();
            let guard = target.is_guard_byte();
            f.x[8] = u64::from(guard);
            if guard == 0 {
                // neither defense up nor guarding: the damage grows with the number of afraid characters
                self.count_and_scale(f, run, &group_site::COUNT_B)?;
                let (v0, v1) = (f.v[0], f.v[1]);
                f.regs.safepoint_nv(
                    group_site::POLL_AFTER_SCALE_B,
                    &[(8, f.x[8]), (9, f.x[9])],
                    &[(0, v0.0, v0.1), (1, v1.0, v1.1)],
                );
                scaled_by_fear_count = true;
            }
        }
        if !scaled_by_fear_count {
            f.x[8] = w(halve(run.value));
        }
        run.store(f.x[8]);
        self.log_damage(f, run, &group_site::R_HALVED)?;
        let afraid = Self::character_test(
            f,
            group_callee::IS_FEAR,
            target.address(),
            group_site::R_FEAR_RETURN,
            group_site::PREDICATE,
            false,
        )?;
        if afraid {
            f.x[8] = w(three_halves(run.value));
        } else {
            self.count_and_scale(f, run, &group_site::COUNT_A)?;
        }
        run.store(f.x[8]);
        self.log_damage(f, run, &group_site::CLOSING)
    }

    /// Every other skill type (compiled block 0x7102201c74 onward).
    fn group_for_other_skill(
        self,
        f: &mut Frame,
        run: &mut GroupRun,
    ) -> Result<(), GuestException> {
        let (acter, target) = (self.acter(), self.target());
        let mut reduce = Self::character_test(
            f,
            group_callee::IS_FEAR,
            acter.address(),
            group_site::O_FEAR_ACTER_RETURN,
            group_site::FIRST_PREDICATE,
            true,
        )?;
        if !reduce {
            reduce = Self::character_test(
                f,
                group_callee::IS_DEFENSE_UP,
                target.address(),
                group_site::O_DEFENSE_UP_RETURN,
                group_site::PREDICATE,
                false,
            )?;
        }
        if !reduce {
            f.x[8] = target.address();
            let guard = target.is_guard_byte();
            f.x[8] = u64::from(guard);
            reduce = guard != 0;
        }
        if reduce {
            f.x[8] = w(halve(run.value));
            run.store(f.x[8]);
            self.log_damage(f, run, &group_site::O_HALVED)?;
        }
        let mut grow = Self::character_test(
            f,
            group_callee::IS_ATTACK_UP,
            acter.address(),
            group_site::O_ATTACK_UP_RETURN,
            group_site::ATTACK_UP_PREDICATE,
            false,
        )?;
        if !grow {
            grow = Self::character_test(
                f,
                group_callee::IS_FEAR,
                target.address(),
                group_site::O_FEAR_TARGET_RETURN,
                group_site::PREDICATE,
                false,
            )?;
        }
        if grow {
            f.x[8] = w(three_halves(run.value));
            run.store(f.x[8]);
            self.log_damage(f, run, &group_site::CLOSING)?;
        }
        Ok(())
    }

    /// The `foreach` over `BattleManager._instance._pc_list`, the factor `count / divisor * 0.5 + 1`, its debug line and
    /// the scaled damage `(int)(factor * (float)damage)`, left in x8 (not stored yet).
    fn count_and_scale(
        self,
        f: &mut Frame,
        run: &mut GroupRun,
        site: &CountSite,
    ) -> Result<(), GuestException> {
        let mut enumerator = [0u8; ENUMERATOR_BYTES];
        self.open_character_list(f, run, site, &mut enumerator)?;
        // the copy of the enumerator the loop works on (`[sp + 0x18]`); x8 to x10 hold its three words
        f.x[8] = u64::from_le_bytes(enumerator[0..8].try_into().unwrap());
        f.x[9] = u64::from_le_bytes(enumerator[8..16].try_into().unwrap());
        f.x[10] = u64::from_le_bytes(enumerator[16..24].try_into().unwrap());
        f.x[21] = unsafe { guest_ld64(group_slots::MOVE_NEXT_METHOD) };
        f.x[22] = unsafe { guest_ld64(group_slots::GET_CURRENT_METHOD) };
        f.x[23] = 0;
        let enumerator_copy = f.x[31].wrapping_add(0x18);
        loop {
            f.x[1] = unsafe { guest_ld64(f.x[21]) };
            f.x[0] = enumerator_copy;
            f.call_out(
                group_callee::ENUMERATOR_MOVE_NEXT,
                site.move_next_return,
                &[0, 1, 8, 9, 10, 21, 22, 23],
                false,
                &mut enumerator,
            )?;
            if f.x[0] & 1 == 0 {
                break;
            }
            f.x[1] = unsafe { guest_ld64(f.x[22]) };
            f.x[0] = enumerator_copy;
            f.call(
                group_callee::ENUMERATOR_GET_CURRENT,
                site.current_return,
                group_site::PREDICATE,
                false,
            )?;
            // `p = current; if (p.<test>()) count++`; the element's +0x20 field is the character
            let character = unsafe { guest_ld64(f.x[0].wrapping_add(0x20)) };
            let counted = Self::character_test(
                f,
                site.predicate,
                character,
                site.predicate_return,
                group_site::PREDICATE,
                false,
            )?;
            f.x[8] = u64::from(counted);
            f.x[23] = u64::from((f.x[23] as u32).wrapping_add(f.x[8] as u32));
            f.regs
                .safepoint_n(site.loop_poll, &[(8, f.x[8]), (23, f.x[23])]);
        }
        f.x[8] = unsafe { guest_ld64(group_slots::DISPOSE_METHOD) };
        f.x[1] = unsafe { guest_ld64(f.x[8]) };
        f.x[0] = enumerator_copy;
        f.call(
            group_callee::ENUMERATOR_DISPOSE,
            site.dispose_return,
            &[0, 1, 8],
            false,
        )?;
        let count = f.x[23] as u32 as i32;
        self.scale_by_count(f, run, site, count)
    }

    /// `BattleManager` class and static-field checks (with their lazy initialization), then
    /// `_instance._pc_list.GetEnumerator()` into `enumerator`.
    fn open_character_list(
        self,
        f: &mut Frame,
        run: &mut GroupRun,
        site: &CountSite,
        enumerator: &mut [u8; ENUMERATOR_BYTES],
    ) -> Result<(), GuestException> {
        let first = run.first_visit;
        run.first_visit = false;
        f.x[21] = unsafe { guest_ld64(group_slots::BATTLE_MANAGER_CLASS) };
        f.x[0] = unsafe { guest_ld64(f.x[21]) };
        let word_address = f.x[0].wrapping_add(CLASS_INITIALIZED);
        let word = if first {
            prestate::read_u32(word_address)
        } else {
            unsafe { guest_ld32(word_address) }
        };
        f.x[8] = u64::from(word);
        if word == 0 {
            f.call(
                group_callee::CLASS_INITIALIZER,
                site.class_init_return,
                &[0, 8, 21],
                false,
            )?;
        }
        f.x[22] = unsafe { guest_ld64(group_slots::STATIC_FLAG_POINTER) };
        let flag = if first {
            prestate::read_u8(f.x[22])
        } else {
            unsafe { guest_ld8(f.x[22]) }
        };
        f.x[8] = u64::from(flag);
        if word == 0 {
            f.regs
                .safepoint_n(site.flag_poll, &[(8, f.x[8]), (22, f.x[22])]);
        }
        let again = if flag == 0 {
            // the static-field metadata of this method is not resolved yet
            f.x[0] = unsafe { guest_ld64(group_slots::BATTLE_MANAGER_CLASS) };
            f.call(
                group_callee::INITIALIZE_METADATA,
                site.metadata_return,
                &[0, 8, 21, 22],
                false,
            )?;
            f.x[8] = 1;
            prestate::store_u8(f.x[22], 1);
            f.x[0] = unsafe { guest_ld64(f.x[21]) };
            let again = unsafe { guest_ld32(f.x[0].wrapping_add(CLASS_INITIALIZED)) };
            f.x[8] = u64::from(again);
            f.regs
                .safepoint_n(site.metadata_poll, &[(0, f.x[0]), (8, f.x[8])]);
            again
        } else {
            f.x[0] = unsafe { guest_ld64(f.x[21]) };
            let again = unsafe { guest_ld32(f.x[0].wrapping_add(CLASS_INITIALIZED)) };
            f.x[8] = u64::from(again);
            again
        };
        if again == 0 {
            f.call(
                group_callee::CLASS_INITIALIZER,
                site.class_init_again_return,
                &[0, 8, 21, 22],
                false,
            )?;
            f.x[0] = unsafe { guest_ld64(f.x[21]) };
            f.regs
                .safepoint_n(site.class_init_again_poll, &[(0, f.x[0])]);
        }
        // `BattleManager._instance._pc_list` and its list
        f.x[8] = unsafe { guest_ld64(f.x[0].wrapping_add(CLASS_STATIC_FIELDS)) };
        f.x[1] = f.x[31];
        f.x[8] = unsafe { guest_ld64(f.x[8].wrapping_add(STATICS_INSTANCE)) };
        f.x[8] = unsafe { guest_ld64(f.x[8].wrapping_add(MANAGER_PC_LIST)) };
        f.x[0] = unsafe { guest_ld64(f.x[8].wrapping_add(PC_LIST_ITEMS)) };
        f.x[8] = unsafe { guest_ld64(group_slots::GET_ENUMERATOR_METHOD) };
        f.x[2] = unsafe { guest_ld64(f.x[8]) };
        f.call_out(
            group_callee::LIST_GET_ENUMERATOR,
            site.enumerator_return,
            &[0, 1, 2, 8, 21, 22],
            false,
            enumerator,
        )
    }

    /// `factor = (float)count / divisor * 0.5f + 1f`, its debug line, then `(int)(factor * (float)damage)` in x8.
    fn scale_by_count(
        self,
        f: &mut Frame,
        run: &GroupRun,
        site: &CountSite,
        count: i32,
    ) -> Result<(), GuestException> {
        let divisor = f32::from_bits(site.divisor_bits);
        let converted = f.regs.f32_from_i32(count);
        f.set_s(0, converted);
        f.set_s(1, divisor);
        let share = f.regs.f32_div(converted, divisor);
        f.set_s(0, share);
        f.set_s(1, HALF);
        let halved = f.regs.f32_arith(0, share, HALF);
        f.set_s(0, halved);
        f.set_s(1, ONE);
        let factor = f.regs.f32_arith(2, halved, ONE);
        f.set_s(0, factor);
        // the factor goes to `[x29 + factor_slot]`, then `_monitor.write(Concat(text, factor.ToString()))`
        let mut slot = factor.to_bits().to_le_bytes();
        f.x[21] = self.debug_monitor();
        f.x[0] = f.x[29].wrapping_add(site.factor_slot);
        f.x[1] = 0;
        f.call_out_v(
            group_callee::SINGLE_TO_STRING,
            site.to_string_return,
            &[0, 1, 21],
            group_site::VECTORS,
            false,
            &mut slot,
        )?;
        self.write_with_text(
            f,
            21,
            group_slots::STRING_FACTOR,
            site.concat_return,
            site.write_return,
        )?;
        // `ldr s1, [x19]`, `x9 = +inf`, `ldr s0, [slot]`, `scvtf s1, s1`, `fmul`, `fcmp` with +inf, `fcvtzs`
        f.v[1] = (u64::from(run.value as u32), 0);
        f.x[9] = u64::from(INFINITY_BITS);
        let stored = f32::from_bits(u32::from_le_bytes(slot));
        f.v[0] = (u64::from(stored.to_bits()), 0);
        let damage_float = f.regs.f32_from_i32(run.value);
        f.set_s(1, damage_float);
        let product = f.regs.f32_arith(0, stored, damage_float);
        f.set_s(0, product);
        f.set_s(1, f32::from_bits(INFINITY_BITS));
        f.x[9] = u64::from(INFINITY_RESULT);
        let infinite = f.regs.f32_compare(product, f32::from_bits(INFINITY_BITS)).z;
        let truncated = f.regs.i32_from_f32_trunc(product);
        f.x[8] = w(truncated);
        if infinite {
            f.x[8] = f.x[9];
        }
        Ok(())
    }
}

/// `List<T>.Enumerator`: the list, the index and version words, the current element.
const ENUMERATOR_BYTES: usize = 24;
/// `Il2CppClass.static_fields` and the word the compiled code tests before it calls the class initializer.
const CLASS_STATIC_FIELDS: u64 = 0xb8;
const CLASS_INITIALIZED: u64 = 0xe0;
/// `BattleManager` statics `_instance`, `BattleManager._pc_list`, and the list a `BattleJoinCharacterList` holds.
const STATICS_INSTANCE: u64 = 0x10;
const MANAGER_PC_LIST: u64 = 0xd8;
const PC_LIST_ITEMS: u64 = 0x10;

// ---- damage_calc driver (task-60) ----
//
// `Battle.SkillExecter.damage_calc()` (0x71021ff580, 613 instructions, `QLEAF`) finishes the damage of one hit. It starts from
// `_tmp_value` (0x40, the product of the attack and defence methods of an earlier caller), applies the group, element and
// barrier rules and stores the result into the target's `CharaSkillResultLogger`. In C# terms (names are IL2CPP's; every
// `_monitor.write(..)` is a debug line that does nothing visible; `damage` is the stack int at `x29 - 4`, `ratio` the stack
// float at `sp + 8`; the two `BattleManager` tests are the lazy class initialization the compiled code repeats at each use):
//
// ```text
// void damage_calc() {
//     _monitor.insert_indent();
//     int damage = _tmp_value;
//     _monitor.write(<text> + damage.ToString());
//     group_a_b(ref damage);
//     switch (_skill_obj._calc_type) {                  // EN_SPECIAL_1_POINT_5, EN_SPECIAL_2, EN_SPECIAL_4
//         case 8:  write(..); damage = damage * 3 / 2; goto log;
//         case 9:  write(..); damage = damage << 1;    goto log;
//         case 10: write(..); damage = damage << 2;
//         log:     _monitor.write(<text> + damage.ToString());
//     }
//     calc_renkei_ratio(ref damage);
//     damage = Math.Max(damage, 1);
//     set_acction_command_ratio();
//     float ratio = _target.calc_element(_skill_obj._element);
//     if (ratio == 0f && _acter._is_equip_kaiten_shoes && (uint)(_skill_obj._skill_id - 103) <= 4
//         && ((1 << (_skill_obj._skill_id - 103)) & 0x15) != 0) {
//         write(..); ratio = 1f;
//     }
//     calc_belome_barrior_ratio(_skill_obj, ref ratio);
//     if (ratio == 0f)      { write(..); _target._result_logger._effective_state = 1; }
//     else if (ratio == 2f) { write(..); _target._result_logger._effective_state = 2; }
//     _monitor.write(<text> + ratio.ToString());
//     damage = (int)(ratio * (float)damage);            // +inf becomes int.MinValue
//     if (ratio != 0f) { write(..); damage = damage > 1 ? damage : 1; }
//     if (BattleManager._instance._encounter_id != 0x819 && _skill_obj._skill_id != 0x2bd) {
//         write(..); write(..); damage <<= 2;
//     }
//     switch (_target._character_id) {
//         case 0x64f:
//             if (_skill_obj._skill_id == 0x2e7) { write(..); damage = 0x50; }
//             else if (_skill_obj._calc_type == 4) damage = 0;
//             break;
//         case 0x5dd: case 0x5ec: case 0x615: case 0x653: <not ported, see UNPORTED_CHARACTERS>
//     }
//     bool invincible = false;
//     if (_skill_obj._skill_id >= 1000) { write(..); damage = 0; invincible = _target.is_invincible(); }
//     else invincible = _target.is_invincible();
//     bool barriered;
//     if (invincible) { write(..); damage = 0; barriered = _target.is_barriered(); } else barriered = _target.is_barriered();
//     if (barriered) { write(..); damage = 0; _target.use_damage_guard(); }
//     if (!CommonRoutine.is_in(_skill_obj._skill_id, 0x2c3, 0x2c5)) {
//         _target._result_logger._number = damage;
//         _target._result_logger._number_state = 2;
//         _monitor.write(<text> + damage.ToString());
//     }
// }
// ```
//
// The four enemy rules (task-61): the cases `0x5dd`, `0x5ec`, `0x615` and `0x653` of the `switch` above, written out here.
//
// ```text
//     case 0x5dd:
//         if (BattleManager._instance._unnamed_d0 >= 7) {          // after the class and static flag checks
//             write(..); _target._result_logger._flag_4a = true; damage = 0;
//         }
//         break;
//     case 0x5ec: case 0x653:
//         write(..); _target._result_logger._flag_4a = true; damage = 0;
//         _target._result_logger._multi_number.Clear(); then Add(0) as often as it held entries;   // inlined List<int>.Clear
//         break;
//     case 0x615:
//         if (BattleManager._instance._unnamed_60._flag_20) {      // after the class and static flag checks
//             write(..); _target._result_logger._flag_4a = true; damage = 0;
//             if (_skill_obj._skill_id == 0x75) GlobalWork.get_inst()._flag_d8 = true;   // after GlobalWork's class check
//             <the same Clear and Add(0) loop as 0x5ec>
//         }
//         break;
// ```
//
// In `on` mode the whole function is the port; in `ab` mode the pre-state hook names the list header, the logger flag, the
// GlobalWork instance flag and the GlobalWork class word besides the cells of the earlier steps. The hook still defers skill
// 0x2c3 on the three characters that read the list (`calc_renkei_ratio` replaces the list for that skill, so its header at the
// clear is not the pre-state one).
//
// The five nested rows (`group_a_b`, `calc_renkei_ratio`, `set_acction_command_ratio`, `calc_element`,
// `calc_belome_barrior_ratio`, `use_damage_guard`) are called through their own shims (`Frame::call_port`). In a replay trial
// they are not recorded (a replay row reached from inside another recorded call runs lifted), so their traced callees
// belong to this call's recording and the port makes the same bridge calls in the same order. The pre-state hook calls
// each nested hook with the registers the lifted code would give it, so the nested ports read the same snapshots; the two
// stack cells (`damage`, `ratio`) are read back from the journal (`smrpg_guest_pre_read` returns a cell's own earlier store).

/// The four characters with an enemy rule that sets the result logger flag at +0x4a (task-61).
const CHARACTER_THRESHOLD: u32 = 0x5dd;
const CHARACTER_CLEAR_A: u32 = 0x5ec;
const CHARACTER_CLEAR_B: u32 = 0x653;
const CHARACTER_FLAGGED_CLEAR: u32 = 0x615;
/// `BattleManager` +0xd0 (an int the 0x5dd rule compares with 7), +0x60 (an object whose byte +0x20 the 0x615 rule tests).
const MANAGER_COUNT: u64 = 0xd0;
const MANAGER_COUNT_LIMIT: i32 = 7;
const MANAGER_FLAG_OBJECT: u64 = 0x60;
const FLAG_OBJECT_BYTE: u64 = 0x20;
/// `CharaSkillResultLogger` byte +0x4a, set by the three rules.
const LOGGER_RULE_FLAG: u64 = 0x4a;
/// Skill id that sets `GlobalWork` byte +0xd8 under the 0x615 rule.
const SKILL_GLOBAL_WORK_FLAG: i32 = 0x75;
const GLOBAL_WORK_FLAG: u64 = 0xd8;
/// `BattleCharacterParam._character_id` of the one character with a fixed damage rule.
const CHARACTER_WITH_FIXED_DAMAGE: u32 = 0x64f;
const SKILL_FIXED_DAMAGE: i32 = 0x2e7;
const FIXED_DAMAGE: u32 = 0x50;
const CALC_TYPE_ZERO_DAMAGE: u32 = 4;
const SKILL_ENCOUNTER_QUADRUPLE: i32 = 0x2bd;
const ENCOUNTER_QUADRUPLE: i32 = 0x819;
/// `BattleManager._encounter_id`.
const MANAGER_ENCOUNTER_ID: u64 = 0x168;
/// `SkillObject.CalcType`: `EN_SPECIAL_1_POINT_5`, `EN_SPECIAL_2`, `EN_SPECIAL_4`.
const CALC_TYPE_ONE_AND_A_HALF: u32 = 8;
const CALC_TYPE_DOUBLE: u32 = 9;
const CALC_TYPE_QUADRUPLE: u32 = 10;
/// Skill ids from here on never leave damage.
const SKILL_FIRST_NO_DAMAGE: i32 = 1000;
const SKILL_NO_RESULT_FIRST: u64 = 0x2c3;
const SKILL_NO_RESULT_LAST: u64 = 0x2c5;
/// `BattleCharacterParam._result_logger`.
const RESULT_LOGGER_FIELD: u64 = 0xb8;

/// The lazy-initialization flag and the data page cells `damage_calc` uses.
pub mod driver_slots {
    pub const INIT_FLAG: u64 = 0x7103cba000 + 0xd71;
    pub const FLAG_PAGE: u64 = 0x7103cba000;
    pub const STRING_PAGE: u64 = 0x7103a83000;
    /// The `System.Math` class cell (class initializer test before `Math.Max`).
    pub const MATH_CLASS: u64 = 0x7103a46000 + 0xa90;
    pub const BATTLE_MANAGER_CLASS: u64 = super::group_slots::BATTLE_MANAGER_CLASS;
    pub const STATIC_FLAG_POINTER: u64 = super::group_slots::STATIC_FLAG_POINTER;
    /// String literal cells (`STRING_PAGE` + offset).
    pub const fn text(offset: u64) -> u64 {
        STRING_PAGE + offset
    }
    pub const TEXT_TMP_VALUE: u64 = text(0xb40);
    pub const TEXT_CALC_TYPE_8: u64 = text(0xbb8);
    pub const TEXT_CALC_TYPE_9: u64 = text(0xbc0);
    pub const TEXT_CALC_TYPE_10: u64 = text(0xb90);
    pub const TEXT_DAMAGE_IS: u64 = text(0x540);
    pub const TEXT_KAITEN_SHOES: u64 = text(0xba8);
    pub const TEXT_RATIO: u64 = text(0xb88);
    pub const TEXT_RATIO_ZERO: u64 = text(0xb80);
    pub const TEXT_RATIO_DOUBLE: u64 = text(0xbc8);
    pub const TEXT_RATIO_NOT_ZERO: u64 = text(0xb48);
    pub const TEXT_ENCOUNTER_A: u64 = text(0xb78);
    pub const TEXT_ENCOUNTER_B: u64 = text(0xb58);
    pub const TEXT_FIXED_DAMAGE: u64 = text(0xb60);
    pub const TEXT_NO_DAMAGE: u64 = text(0xb68);
    pub const TEXT_INVINCIBLE: u64 = text(0xba0);
    pub const TEXT_BARRIER: u64 = text(0xb70);
    pub const TEXT_RESULT: u64 = text(0x5b0);
    /// The 26 slots the first run resolves, in the order of its `initialize_runtime_metadata` calls.
    pub const INIT_ORDER: [u64; 26] = [
        0x7103a57e48,
        0x7103a58048,
        0x7103a4d788,
        0x7103a4e8c8,
        0x7103a4d790,
        0x7103a46a90,
        0x7103a83b48,
        0x7103a83b50,
        0x7103a83b58,
        0x7103a83540,
        0x7103a83b60,
        0x7103a83b68,
        0x7103a83b70,
        0x7103a83b78,
        0x7103a83b80,
        0x7103a83b88,
        0x7103a83b90,
        0x7103a83b98,
        0x7103a83ba0,
        0x7103a83ba8,
        0x7103a83bb0,
        0x7103a83b40,
        0x7103a83bb8,
        0x7103a835b0,
        0x7103a83bc0,
        0x7103a83bc8,
    ];
}

mod driver_callee {
    /// `StellaDebug.DebugMonitor.insert_indent`.
    pub const INSERT_INDENT: u64 = 0x71018bebf0;
    /// `System.Math.Max(int, int)`.
    pub const MATH_MAX: u64 = 0x710213e670;
    /// `Common.CommonRoutine.is_in(int, int, int)`.
    pub const IS_IN: u64 = 0x71018ba2c0;
    pub const IS_INVINCIBLE: u64 = 0x71021bcb30;
    pub const IS_BARRIERED: u64 = 0x71021c3bb0;
}

/// Return addresses (the `x30` each call site writes) and the register lists it flushes.
mod driver_site {
    pub const FIRST_INIT_RETURN: u64 = 0x71021ff5b8;
    pub const INIT_STEP: u64 = 12;
    pub const FIRST_INIT_CALL: &[usize] = &[0, 8, 19, 20, 21, 29];
    pub const INSERT_INDENT_RETURN: u64 = 0x71021ff704;
    pub const INSERT_INDENT: &[usize] = &[0, 1, 8, 19, 20, 21, 22, 29];
    pub const VECTORS_01: &[usize] = &[0, 1];
    pub const VECTORS_012: &[usize] = &[0, 1, 2];
    pub const CONCAT: &[usize] = &[0, 1, 2];
    pub const WRITE: &[usize] = &[0, 1, 2];
}

/// One `_monitor.write(Concat(<text>, damage.ToString()))` line: the registers that hold the monitor and the text cell,
/// the flush list and return addresses of its three calls.
struct NumberLine {
    monitor: usize,
    text: usize,
    to_string: &'static [usize],
    to_string_return: u64,
    concat_return: u64,
    write_return: u64,
}

const LINE_TMP_VALUE: NumberLine = NumberLine {
    monitor: 20,
    text: 21,
    to_string: &[0, 1, 8, 20],
    to_string_return: 0x71021ff71c,
    concat_return: 0x71021ff72c,
    write_return: 0x71021ff73c,
};
const LINE_CALC_TYPE: NumberLine = NumberLine {
    monitor: 20,
    text: 21,
    to_string: &[0, 1, 8, 20, 21],
    to_string_return: 0x71021ff7f8,
    concat_return: 0x71021ff808,
    write_return: 0x71021ff818,
};
const LINE_RESULT: NumberLine = NumberLine {
    monitor: 19,
    text: 20,
    to_string: &[0, 1, 8, 9, 19, 20],
    to_string_return: 0x71021ffe84,
    concat_return: 0x71021ffe94,
    write_return: 0x71021ffea4,
};

/// One `_monitor.write(<literal>)` call: the literal's cell, the call's return address and flush lists.
struct LiteralLine {
    text: u64,
    return_address: u64,
    list: &'static [usize],
    vectors: &'static [usize],
    with_sp: bool,
}

const fn literal(
    text: u64,
    return_address: u64,
    list: &'static [usize],
    vectors: &'static [usize],
) -> LiteralLine {
    LiteralLine {
        text,
        return_address,
        list,
        vectors,
        with_sp: false,
    }
}

const WRITE_8: &[usize] = &[0, 1, 2, 8];
const WRITE_8_9: &[usize] = &[0, 1, 2, 8, 9];
const WRITE_8_9_20: &[usize] = &[0, 1, 2, 8, 9, 20];
const WRITE_8_20: &[usize] = &[0, 1, 2, 8, 20];
const NO_VECTORS: &[usize] = &[];

const LINE_CALC_TYPE_10: LiteralLine = literal(
    driver_slots::TEXT_CALC_TYPE_10,
    0x71021ff7b0,
    WRITE_8,
    NO_VECTORS,
);
const LINE_CALC_TYPE_9: LiteralLine = literal(
    driver_slots::TEXT_CALC_TYPE_9,
    0x71021ff7d4,
    WRITE_8,
    NO_VECTORS,
);
const LINE_CALC_TYPE_8: LiteralLine = literal(
    driver_slots::TEXT_CALC_TYPE_8,
    0x71021ff780,
    WRITE_8,
    NO_VECTORS,
);
const LINE_KAITEN_SHOES: LiteralLine = literal(
    driver_slots::TEXT_KAITEN_SHOES,
    0x71021ff8bc,
    WRITE_8_9,
    NO_VECTORS,
);
const LINE_ENCOUNTER_A: LiteralLine = literal(
    driver_slots::TEXT_ENCOUNTER_A,
    0x71021ff9fc,
    WRITE_8_9_20,
    driver_site::VECTORS_012,
);
const LINE_ENCOUNTER_B: LiteralLine = literal(
    driver_slots::TEXT_ENCOUNTER_B,
    0x71021ffa14,
    WRITE_8,
    NO_VECTORS,
);
const LINE_RATIO_NOT_ZERO: LiteralLine = literal(
    driver_slots::TEXT_RATIO_NOT_ZERO,
    0x71021ffb00,
    WRITE_8_9,
    driver_site::VECTORS_012,
);
const LINE_FIXED_DAMAGE: LiteralLine = literal(
    driver_slots::TEXT_FIXED_DAMAGE,
    0x71021ffce4,
    WRITE_8_9_20,
    driver_site::VECTORS_012,
);
const LINE_BARRIER: LiteralLine = literal(
    driver_slots::TEXT_BARRIER,
    0x71021ffe20,
    WRITE_8,
    NO_VECTORS,
);
const LINE_INVINCIBLE: LiteralLine = literal(
    driver_slots::TEXT_INVINCIBLE,
    0x71021ffefc,
    WRITE_8,
    NO_VECTORS,
);
/// `skill id >= 1000`: the write that follows a join, so the compiled code also flushes x9, x20 and x21 (and v0 to v2).
const LINE_NO_DAMAGE: LiteralLine = LiteralLine {
    text: driver_slots::TEXT_NO_DAMAGE,
    return_address: 0x71021ffed0,
    list: &[0, 1, 2, 8, 9, 20, 21],
    vectors: driver_site::VECTORS_012,
    with_sp: false,
};

/// What one call of the port keeps besides the registers: the two stack cells (`damage` at `x29 - 4`, `ratio` at
/// `sp + 8`) and the damage value the port holds.
struct DriverRun {
    damage: u64,
    ratio: u64,
    value: i32,
}

impl DriverRun {
    /// `str w, [x29, #-4]`: a stack store, journaled in a trial so that later reads and the final comparison see it.
    fn store(&mut self, value: u64) {
        self.value = value as u32 as i32;
        prestate::store_u32(self.damage, value as u32);
    }

    /// After a nested port that edits the cell: the value it left (its journaled store in a trial).
    fn refresh(&mut self) {
        self.value = prestate::read_u32(self.damage) as i32;
    }
}

impl CharaSkillResultLogger {
    /// `_number` (the damage), `_number_state` (2 = damage) and `_effective_state` (1 = none, 2 = double).
    const DAMAGE_NUMBER: u64 = 0x28;
    const DAMAGE_STATE: u64 = 0x2c;
    const EFFECTIVE_STATE: u64 = 0x4c;
}

impl SkillExecter {
    /// `_tmp_value`.
    const TMP_VALUE: u64 = 0x40;

    /// `damage_calc()`. `f` is the register file at entry.
    pub fn damage_calc(self, f: &mut Frame) -> Result<(), GuestException> {
        let character = self.target().character_id();
        let flag = prestate::read_u8(driver_slots::INIT_FLAG);
        // prologue: `sub sp, sp, #0x40`, `add x29, sp, #0x10`; x20 = the flag page, x21 = the string page, x8 = the flag byte,
        // x21 = the first text cell, x19 = this
        f.x[31] = f.entry[31].wrapping_sub(0x40);
        f.x[29] = f.x[31].wrapping_add(0x10);
        f.x[20] = driver_slots::FLAG_PAGE;
        f.x[21] = driver_slots::STRING_PAGE;
        f.x[8] = u64::from(flag);
        f.x[21] = unsafe { guest_ld64(driver_slots::TEXT_TMP_VALUE) };
        f.x[19] = self.address();
        if flag & 1 == 0 {
            self.resolve_driver_metadata(f)?;
            prestate::store_u8(driver_slots::INIT_FLAG, 1);
        }
        let mut run = DriverRun {
            damage: f.x[29].wrapping_sub(4),
            ratio: f.x[31].wrapping_add(8),
            value: 0,
        };
        let monitor = self.debug_monitor();
        f.x[22] = unsafe { guest_ld64(driver_slots::MATH_CLASS) };
        prestate::store_u32(run.ratio, 0);
        // `_monitor.insert_indent()`
        f.x[0] = monitor;
        f.x[1] = 0;
        f.call(
            driver_callee::INSERT_INDENT,
            driver_site::INSERT_INDENT_RETURN,
            driver_site::INSERT_INDENT,
            true,
        )?;
        // `int damage = _tmp_value; _monitor.write(<text> + damage.ToString())`
        let tmp_value = unsafe { guest_ld32(self.address().wrapping_add(Self::TMP_VALUE)) };
        f.x[8] = u64::from(tmp_value);
        run.store(f.x[8]);
        f.x[20] = monitor;
        self.write_number(f, &run, &LINE_TMP_VALUE)?;
        // `group_a_b(ref damage)`
        f.x[0] = self.address();
        f.x[1] = run.damage;
        f.call_port(smrpg_skill_group_a_b, 0x71021ff748, &[0, 1], false)?;
        run.refresh();
        // the `_calc_type` switch: 8 is x1.5, 9 is x2, 10 is x4; the debug line follows in the three cases
        let skill_obj = self.skill_obj();
        f.x[8] = skill_obj.address();
        let calc_type = skill_obj.calc_type();
        f.x[8] = u64::from(calc_type);
        if matches!(
            calc_type,
            CALC_TYPE_ONE_AND_A_HALF | CALC_TYPE_DOUBLE | CALC_TYPE_QUADRUPLE
        ) {
            match calc_type {
                CALC_TYPE_QUADRUPLE => {
                    self.write_literal(f, &LINE_CALC_TYPE_10)?;
                    f.x[8] = w(run.value.wrapping_shl(2));
                }
                CALC_TYPE_DOUBLE => {
                    self.write_literal(f, &LINE_CALC_TYPE_9)?;
                    f.x[8] = w(run.value.wrapping_shl(1));
                }
                _ => {
                    self.write_literal(f, &LINE_CALC_TYPE_8)?;
                    f.x[8] = w(three_halves(run.value));
                }
            }
            run.store(f.x[8]);
            f.x[21] = unsafe { guest_ld64(driver_slots::TEXT_DAMAGE_IS) };
            f.x[20] = monitor;
            self.write_number(f, &run, &LINE_CALC_TYPE)?;
        }
        // `calc_renkei_ratio(ref damage)`
        f.x[1] = run.damage;
        f.x[0] = self.address();
        f.call_port(
            smrpg_skill_calc_renkei_ratio,
            0x71021ff824,
            &[0, 1, 8],
            false,
        )?;
        run.refresh();
        // `damage = Math.Max(damage, 1)` after the lazy class initialization of `System.Math`
        f.x[0] = unsafe { guest_ld64(f.x[22]) };
        f.x[20] = u64::from(run.value as u32);
        let math_word = prestate::read_u32(f.x[0].wrapping_add(CLASS_INITIALIZED));
        f.x[8] = u64::from(math_word);
        if math_word == 0 {
            f.call(
                group_callee::CLASS_INITIALIZER,
                0x71021ff838,
                &[0, 8, 20],
                false,
            )?;
        }
        f.x[0] = f.x[20] & 0xffff_ffff;
        f.x[1] = 1;
        f.x[2] = 0;
        f.call(
            driver_callee::MATH_MAX,
            0x71021ff848,
            &[0, 1, 2, 8, 20],
            false,
        )?;
        run.store(f.x[0] & 0xffff_ffff);
        // `set_acction_command_ratio()`
        f.x[0] = self.address();
        f.call_port(
            smrpg_skill_set_acction_command_ratio,
            0x71021ff854,
            &[0],
            false,
        )?;
        // `ratio = _target.calc_element(_skill_obj._element)`
        f.x[0] = self.target().address();
        f.x[8] = skill_obj.address();
        f.x[2] = 0;
        f.x[1] = u64::from(skill_obj.element());
        f.call_port(smrpg_param_calc_element, 0x71021ff864, &[0, 1, 2, 8], false)?;
        let element_ratio = f32::from_bits(f.v[0].0 as u32);
        let ratio_is_zero = f.regs.f32_compare(element_ratio, 0.0).z;
        prestate::store_u32(run.ratio, element_ratio.to_bits());
        if ratio_is_zero {
            // no effect by element: an acter with Kaiten shoes using skill 103, 105 or 107 keeps a ratio of 1
            let acter = self.acter();
            f.x[8] = acter.address();
            let shoes = acter.is_equip_kaiten_shoes_byte();
            f.x[8] = u64::from(shoes);
            if shoes != 0 {
                f.x[8] = skill_obj.address();
                f.x[8] = w(skill_obj.skill_id().wrapping_sub(0x67));
                if f.x[8] as u32 <= 4 {
                    f.x[9] = 1;
                    f.x[8] = w(1i32.wrapping_shl(f.x[8] as u32));
                    f.x[9] = 0x15;
                    if f.x[8] & f.x[9] != 0 {
                        self.write_literal(f, &LINE_KAITEN_SHOES)?;
                        f.x[8] = 0x3f80_0000;
                        prestate::store_u32(run.ratio, 0x3f80_0000);
                    }
                }
            }
        }
        // `calc_belome_barrior_ratio(_skill_obj, ref ratio)`
        f.x[22] = unsafe { guest_ld64(driver_slots::TEXT_RATIO) };
        f.x[1] = skill_obj.address();
        f.x[2] = run.ratio;
        f.x[0] = self.address();
        f.call_port(
            smrpg_skill_calc_belome_barrior_ratio,
            0x71021ff8dc,
            &[0, 1, 2, 8, 9, 22],
            false,
        )?;
        let ratio = f32::from_bits(prestate::read_u32(run.ratio));
        f.v[0] = (u64::from(ratio.to_bits()), 0);
        let ratio_zero = f.regs.f32_compare(ratio, 0.0).z;
        let mut effective_state = 0;
        if ratio_zero {
            f.x[8] = unsafe { guest_ld64(driver_slots::TEXT_RATIO_ZERO) };
            f.x[20] = 1;
            effective_state = 1;
        } else {
            f.set_s(1, 2.0);
            if f.regs.f32_compare(ratio, 2.0).z {
                f.x[8] = unsafe { guest_ld64(driver_slots::TEXT_RATIO_DOUBLE) };
                f.x[20] = 2;
                effective_state = 2;
            }
        }
        if effective_state != 0 {
            f.x[0] = monitor;
            f.x[2] = 0;
            f.x[1] = unsafe { guest_ld64(f.x[8]) };
            f.call_v(
                group_callee::DEBUG_WRITE,
                0x71021ff920,
                WRITE_8_20,
                driver_site::VECTORS_01,
                false,
            )?;
            f.x[8] = self.target().address();
            f.x[8] = unsafe { guest_ld64(f.x[8].wrapping_add(RESULT_LOGGER_FIELD)) };
            prestate::store_u32(
                f.x[8].wrapping_add(CharaSkillResultLogger::EFFECTIVE_STATE),
                effective_state,
            );
        }
        // `_monitor.write(<text> + ratio.ToString())`
        f.x[21] = unsafe { guest_ld64(driver_slots::BATTLE_MANAGER_CLASS) };
        f.x[20] = monitor;
        self.write_ratio(f, &run)?;
        // `damage = (int)(ratio * (float)damage)`, `+inf` stored as `int.MinValue`
        f.v[1] = (u64::from(run.value as u32), 0);
        f.x[9] = u64::from(INFINITY_BITS);
        f.v[0] = (u64::from(ratio.to_bits()), 0);
        let damage_float = f.regs.f32_from_i32(run.value);
        f.set_s(1, damage_float);
        let product = f.regs.f32_arith(0, ratio, damage_float);
        f.set_s(1, product);
        let truncated = f.regs.i32_from_f32_trunc(product);
        f.x[8] = w(truncated);
        f.set_s(2, f32::from_bits(INFINITY_BITS));
        let infinite = f.regs.f32_compare(product, f32::from_bits(INFINITY_BITS)).z;
        f.x[9] = u64::from(INFINITY_RESULT);
        if infinite {
            f.x[8] = f.x[9];
        }
        let ratio_zero = f.regs.f32_compare(ratio, 0.0).z;
        run.store(f.x[8]);
        if !ratio_zero {
            self.write_literal(f, &LINE_RATIO_NOT_ZERO)?;
            // `cmp w8, #1; csinc w8, w8, wzr, gt`: at least 1
            f.x[8] = w(if run.value > 1 { run.value } else { 1 });
            run.store(f.x[8]);
        }
        // `BattleManager._instance._encounter_id`, after the class and static flag checks
        self.open_manager(f, !ratio_zero)?;
        let class = f.x[0];
        f.x[8] = unsafe { guest_ld64(class.wrapping_add(CLASS_STATIC_FIELDS)) };
        f.x[8] = unsafe { guest_ld64(f.x[8].wrapping_add(STATICS_INSTANCE)) };
        let encounter = unsafe { guest_ld32(f.x[8].wrapping_add(MANAGER_ENCOUNTER_ID)) } as i32;
        f.x[8] = w(encounter);
        if encounter == ENCOUNTER_QUADRUPLE {
            f.x[8] = skill_obj.address();
            let id = skill_obj.skill_id();
            f.x[8] = w(id);
            if id == SKILL_ENCOUNTER_QUADRUPLE {
                self.write_literal(f, &LINE_ENCOUNTER_A)?;
                self.write_literal(f, &LINE_ENCOUNTER_B)?;
                f.x[8] = w(run.value.wrapping_shl(2));
                run.store(f.x[8]);
            }
        }
        // the target's character id: the five rules of the `switch`
        f.x[8] = self.target().address();
        f.x[8] = u64::from(character);
        if character == CHARACTER_WITH_FIXED_DAMAGE {
            f.x[8] = skill_obj.address();
            let id = skill_obj.skill_id();
            f.x[9] = u64::from(id as u32);
            if id == SKILL_FIXED_DAMAGE {
                self.write_literal(f, &LINE_FIXED_DAMAGE)?;
                f.x[8] = u64::from(FIXED_DAMAGE);
                run.store(f.x[8]);
            } else {
                f.x[8] = u64::from(skill_obj.calc_type());
                if skill_obj.calc_type() == CALC_TYPE_ZERO_DAMAGE {
                    run.store(0);
                }
            }
        }
        match character {
            CHARACTER_THRESHOLD => self.rule_threshold(f, &mut run)?,
            CHARACTER_CLEAR_A | CHARACTER_CLEAR_B => self.rule_clear(f, &mut run)?,
            CHARACTER_FLAGGED_CLEAR => self.rule_flagged_clear(f, &mut run)?,
            _ => {}
        }
        self.finish(f, &mut run, monitor)
    }

    /// The 26 `initialize_runtime_metadata` calls of the first run, then `x8 = 1` (the flag store is the caller's).
    fn resolve_driver_metadata(self, f: &mut Frame) -> Result<(), GuestException> {
        for (n, slot) in driver_slots::INIT_ORDER.into_iter().enumerate() {
            f.x[0] = unsafe { guest_ld64(slot) };
            let (list, with_sp) = if n == 0 {
                (driver_site::FIRST_INIT_CALL, true)
            } else {
                (group_site::LATER_INIT_CALL, false)
            };
            f.call(
                group_callee::INITIALIZE_METADATA,
                driver_site::FIRST_INIT_RETURN + driver_site::INIT_STEP * n as u64,
                list,
                with_sp,
            )?;
        }
        f.x[8] = 1;
        Ok(())
    }

    /// `_monitor.write(Concat(<text>, damage.ToString()))` for the `int` at `x29 - 4`.
    fn write_number(
        self,
        f: &mut Frame,
        run: &DriverRun,
        line: &NumberLine,
    ) -> Result<(), GuestException> {
        f.x[0] = run.damage;
        f.x[1] = 0;
        let mut area = (run.value as u32).to_le_bytes();
        f.call_out(
            group_callee::INT32_TO_STRING,
            line.to_string_return,
            line.to_string,
            false,
            &mut area,
        )?;
        f.x[1] = f.x[0];
        f.x[0] = unsafe { guest_ld64(f.x[line.text]) };
        f.x[2] = 0;
        f.call(
            group_callee::STRING_CONCAT_2,
            line.concat_return,
            driver_site::CONCAT,
            false,
        )?;
        f.x[1] = f.x[0];
        f.x[0] = f.x[line.monitor];
        f.x[2] = 0;
        f.call(
            group_callee::DEBUG_WRITE,
            line.write_return,
            driver_site::WRITE,
            false,
        )
    }

    /// `_monitor.write(Concat(<text>, ratio.ToString()))` for the `float` at `sp + 8`.
    fn write_ratio(self, f: &mut Frame, run: &DriverRun) -> Result<(), GuestException> {
        f.x[0] = run.ratio;
        f.x[1] = 0;
        let mut area = prestate::read_u32(run.ratio).to_le_bytes();
        f.call_out_v(
            group_callee::SINGLE_TO_STRING,
            0x71021ff944,
            &[0, 1, 8, 20, 21],
            driver_site::VECTORS_01,
            false,
            &mut area,
        )?;
        f.x[1] = f.x[0];
        f.x[0] = unsafe { guest_ld64(f.x[22]) };
        f.x[2] = 0;
        f.call(
            group_callee::STRING_CONCAT_2,
            0x71021ff954,
            driver_site::CONCAT,
            false,
        )?;
        f.x[1] = f.x[0];
        f.x[0] = f.x[20];
        f.x[2] = 0;
        f.call(
            group_callee::DEBUG_WRITE,
            0x71021ff964,
            driver_site::WRITE,
            false,
        )
    }

    /// `x8 = [cell]`, `x0 = _monitor`, `x2 = 0`, `x1 = [x8]`, `_monitor.write(<literal>)`.
    fn write_literal(self, f: &mut Frame, line: &LiteralLine) -> Result<(), GuestException> {
        f.x[8] = unsafe { guest_ld64(line.text) };
        f.x[0] = self.debug_monitor();
        f.x[2] = 0;
        f.x[1] = unsafe { guest_ld64(f.x[8]) };
        if line.vectors.is_empty() {
            f.call(
                group_callee::DEBUG_WRITE,
                line.return_address,
                line.list,
                line.with_sp,
            )
        } else {
            f.call_v(
                group_callee::DEBUG_WRITE,
                line.return_address,
                line.list,
                line.vectors,
                line.with_sp,
            )
        }
    }

    /// The `BattleManager` class initialized word, the static-field flag byte and the class initialized word again, each
    /// with its lazy initialization call (the three tests `group_a_b` makes before its `foreach`); leaves x0 = the class.
    /// `poll_first` is the path from the `ratio != 0` branch, which reaches the first test through a back edge.
    fn open_manager(self, f: &mut Frame, poll_first: bool) -> Result<(), GuestException> {
        f.x[0] = unsafe { guest_ld64(f.x[21]) };
        let word = prestate::read_u32(f.x[0].wrapping_add(CLASS_INITIALIZED));
        f.x[8] = u64::from(word);
        if poll_first {
            f.regs
                .safepoint_n(0x71021ffb18, &[(0, f.x[0]), (8, f.x[8])]);
        }
        let flag_byte = if word == 0 {
            f.call_v(
                group_callee::CLASS_INITIALIZER,
                0x71021ffb20,
                &[0, 8, 9],
                driver_site::VECTORS_012,
                false,
            )?;
            f.x[20] = unsafe { guest_ld64(driver_slots::STATIC_FLAG_POINTER) };
            let flag = prestate::read_u8(f.x[20]);
            f.x[8] = u64::from(flag);
            f.regs
                .safepoint_n(0x71021ffb2c, &[(8, f.x[8]), (20, f.x[20])]);
            flag
        } else {
            f.x[20] = unsafe { guest_ld64(driver_slots::STATIC_FLAG_POINTER) };
            let flag = prestate::read_u8(f.x[20]);
            f.x[8] = u64::from(flag);
            flag
        };
        let class_ready = if flag_byte == 0 {
            // the static-field metadata of this method is not resolved yet
            f.x[0] = unsafe { guest_ld64(driver_slots::BATTLE_MANAGER_CLASS) };
            f.call_v(
                group_callee::INITIALIZE_METADATA,
                0x71021ffb3c,
                &[0, 8, 9, 20],
                driver_site::VECTORS_012,
                false,
            )?;
            f.x[8] = 1;
            prestate::store_u8(f.x[20], 1);
            f.x[0] = unsafe { guest_ld64(f.x[21]) };
            let again = unsafe { guest_ld32(f.x[0].wrapping_add(CLASS_INITIALIZED)) };
            f.x[8] = u64::from(again);
            f.regs
                .safepoint_n(0x71021ffb4c, &[(0, f.x[0]), (8, f.x[8])]);
            again != 0
        } else {
            f.x[0] = unsafe { guest_ld64(f.x[21]) };
            let again = unsafe { guest_ld32(f.x[0].wrapping_add(CLASS_INITIALIZED)) };
            f.x[8] = u64::from(again);
            again != 0
        };
        if !class_ready {
            f.call_v(
                group_callee::CLASS_INITIALIZER,
                0x71021ffb54,
                &[0, 8, 9, 20],
                driver_site::VECTORS_012,
                false,
            )?;
            f.x[0] = unsafe { guest_ld64(f.x[21]) };
            f.regs.safepoint_n(0x71021ffb58, &[(0, f.x[0])]);
        }
        Ok(())
    }

    /// The tail: skills that never leave damage, invincibility and barrier, then the result logger and the last debug line.
    fn finish(
        self,
        f: &mut Frame,
        run: &mut DriverRun,
        monitor: u64,
    ) -> Result<(), GuestException> {
        let skill_obj = self.skill_obj();
        let target = self.target();
        f.x[8] = skill_obj.address();
        let id = skill_obj.skill_id();
        f.x[8] = w(id);
        let invincible;
        if id >= SKILL_FIRST_NO_DAMAGE {
            self.write_literal(f, &LINE_NO_DAMAGE)?;
            run.store(0);
            f.x[0] = target.address();
            f.x[1] = 0;
            f.call(driver_callee::IS_INVINCIBLE, 0x71021ffee0, &[0, 1], false)?;
            f.x[8] = f.regs.x(8);
            f.regs.safepoint_n(0x71021ffee0, &[]);
            invincible = f.x[0] & 1 != 0;
        } else {
            f.x[0] = target.address();
            f.x[1] = 0;
            f.call_v(
                driver_callee::IS_INVINCIBLE,
                0x71021ffdf4,
                &[0, 1, 2, 8, 9, 20, 21],
                driver_site::VECTORS_012,
                false,
            )?;
            f.x[8] = f.regs.x(8);
            invincible = f.x[0] & 1 != 0;
        }
        let barriered;
        if invincible {
            self.write_literal(f, &LINE_INVINCIBLE)?;
            run.store(0);
            f.x[0] = target.address();
            f.x[1] = 0;
            f.call(driver_callee::IS_BARRIERED, 0x71021fff0c, &[0, 1], false)?;
            f.x[8] = f.regs.x(8);
            f.regs.safepoint_n(0x71021fff0c, &[]);
            barriered = f.x[0] & 1 != 0;
            if !barriered {
                f.regs.safepoint_n(0x71021fff10, &[]);
            }
        } else {
            f.x[0] = target.address();
            f.x[1] = 0;
            f.call(driver_callee::IS_BARRIERED, 0x71021ffe04, &[0, 1], false)?;
            f.x[8] = f.regs.x(8);
            barriered = f.x[0] & 1 != 0;
        }
        if barriered {
            self.write_literal(f, &LINE_BARRIER)?;
            run.store(0);
            f.x[0] = target.address();
            f.x[1] = 0;
            f.call_port(smrpg_param_use_damage_guard, 0x71021ffe30, &[0, 1], false)?;
            f.x[8] = f.regs.x(8);
            f.x[9] = f.regs.x(9);
        }
        // `Common.CommonRoutine.is_in(_skill_obj._skill_id, 0x2c3, 0x2c5)` skips the result store
        f.x[8] = skill_obj.address();
        f.x[1] = SKILL_NO_RESULT_FIRST;
        f.x[2] = SKILL_NO_RESULT_LAST;
        f.x[0] = u64::from(skill_obj.skill_id() as u32);
        f.x[3] = 0;
        f.call(driver_callee::IS_IN, 0x71021ffe48, &[0, 1, 2, 3, 8], false)?;
        f.x[8] = f.regs.x(8);
        f.x[9] = f.regs.x(9);
        if f.x[0] & 1 == 0 {
            // `_target._result_logger._number = damage; _number_state = 2; _monitor.write(<text> + damage.ToString())`
            f.x[20] = unsafe { guest_ld64(driver_slots::TEXT_RESULT) };
            f.x[8] = target.address();
            f.x[0] = run.damage;
            f.x[1] = 0;
            f.x[8] = unsafe { guest_ld64(f.x[8].wrapping_add(RESULT_LOGGER_FIELD)) };
            f.x[9] = u64::from(run.value as u32);
            prestate::store_u32(
                f.x[8].wrapping_add(CharaSkillResultLogger::DAMAGE_NUMBER),
                f.x[9] as u32,
            );
            f.x[8] = target.address();
            f.x[8] = unsafe { guest_ld64(f.x[8].wrapping_add(RESULT_LOGGER_FIELD)) };
            f.x[9] = 2;
            prestate::store_u32(f.x[8].wrapping_add(CharaSkillResultLogger::DAMAGE_STATE), 2);
            f.x[19] = monitor;
            self.write_number(f, run, &LINE_RESULT)?;
        }
        f.leave(&[19, 20, 21, 22, 29]);
        Ok(())
    }
}

// ---- The enemy rules (task-61) ----

/// Return addresses and safepoints of one copy of the `BattleManager` class and flag checks that precede a rule's reads.
struct ManagerTestSite {
    first_init_return: u64,
    first_init_poll: u64,
    metadata_return: u64,
    metadata_poll: u64,
    /// The poll on the edge that falls through when the class word is still clear after the metadata call.
    metadata_clear_poll: Option<u64>,
    last_init_return: u64,
    last_init_poll: Option<u64>,
}

/// The 0x5dd rule's copy of the checks.
const MANAGER_TEST_THRESHOLD: ManagerTestSite = ManagerTestSite {
    first_init_return: 0x71021ffd2c,
    first_init_poll: 0x71021ffd30,
    metadata_return: 0x71021ffd40,
    metadata_poll: 0x71021ffd50,
    metadata_clear_poll: Some(0x71021ffd54),
    last_init_return: 0x71021ffb80,
    last_init_poll: None,
};

/// The 0x615 rule's copy of the checks.
const MANAGER_TEST_FLAGGED: ManagerTestSite = ManagerTestSite {
    first_init_return: 0x71021ffd70,
    first_init_poll: 0x71021ffd74,
    metadata_return: 0x71021ffd84,
    metadata_poll: 0x71021ffd94,
    metadata_clear_poll: None,
    last_init_return: 0x71021ffd9c,
    last_init_poll: Some(0x71021ffda0),
};

/// The inlined `List<int>.Clear` and the `Add(0)` loop: two copies in the compiled code.
struct ClearSite {
    references_return: u64,
    clear_return: u64,
    add_return: u64,
    loop_poll: u64,
}

const CLEAR_SITE_A: ClearSite = ClearSite {
    references_return: 0x71021ffac0,
    clear_return: 0x71021ffae4,
    add_return: 0x71021ffd1c,
    loop_poll: 0x71021ffd20,
};

const CLEAR_SITE_B: ClearSite = ClearSite {
    references_return: 0x71021ffc94,
    clear_return: 0x71021ffcb8,
    add_return: 0x71021ffdd0,
    loop_poll: 0x71021ffdd4,
};

const LINE_RULE_THRESHOLD: LiteralLine = literal(
    driver_slots::text(0xbb0),
    0x71021ffbb0,
    WRITE_8_9_20,
    driver_site::VECTORS_012,
);
const LINE_RULE_CLEAR: LiteralLine = literal(
    driver_slots::text(0xb98),
    0x71021ffa74,
    WRITE_8_9_20,
    driver_site::VECTORS_012,
);
const LINE_RULE_FLAGGED: LiteralLine = literal(
    driver_slots::text(0xb50),
    0x71021ffc10,
    WRITE_8_9_20,
    driver_site::VECTORS_012,
);

/// Callees of the rules.
mod rule_callee {
    pub const IS_REFERENCE_OR_CONTAINS_REFERENCES: u64 = 0x71000c5060;
    pub const ARRAY_CLEAR: u64 = 0x7101d7c0c0;
    pub const LIST_INT_ADD: u64 = 0x710015b4c0;
    pub const GLOBAL_WORK_GET_INST: u64 = 0x7101886a00;
}

impl SkillExecter {
    /// The class initialized word, the flag byte and the class word again, as the 0x5dd and 0x615 rules test them (x21 = the
    /// class cell, x20 = the flag byte's address, both left by the encounter step); leaves x0 = the `BattleManager` class.
    fn manager_test(self, f: &mut Frame, site: &ManagerTestSite) -> Result<(), GuestException> {
        let lazy = driver_site::VECTORS_012;
        f.x[0] = unsafe { guest_ld64(f.x[21]) };
        let word = unsafe { guest_ld32(f.x[0].wrapping_add(CLASS_INITIALIZED)) };
        f.x[8] = u64::from(word);
        if word == 0 {
            f.call_v(
                group_callee::CLASS_INITIALIZER,
                site.first_init_return,
                &[0, 8, 9, 20],
                lazy,
                false,
            )?;
            f.x[8] = u64::from(unsafe { guest_ld8(f.x[20]) });
            f.regs.safepoint_n(site.first_init_poll, &[(8, f.x[8])]);
        } else {
            f.x[8] = u64::from(unsafe { guest_ld8(f.x[20]) });
        }
        if f.x[8] as u8 != 0 {
            f.x[0] = unsafe { guest_ld64(f.x[21]) };
            let again = unsafe { guest_ld32(f.x[0].wrapping_add(CLASS_INITIALIZED)) };
            f.x[8] = u64::from(again);
            if again != 0 {
                return Ok(());
            }
        } else {
            // the static-field metadata of this method is not resolved yet
            f.x[0] = unsafe { guest_ld64(driver_slots::BATTLE_MANAGER_CLASS) };
            f.call_v(
                group_callee::INITIALIZE_METADATA,
                site.metadata_return,
                &[0, 8, 9, 20],
                lazy,
                false,
            )?;
            f.x[8] = 1;
            prestate::store_u8(f.x[20], 1);
            f.x[0] = unsafe { guest_ld64(f.x[21]) };
            let again = unsafe { guest_ld32(f.x[0].wrapping_add(CLASS_INITIALIZED)) };
            f.x[8] = u64::from(again);
            f.regs
                .safepoint_n(site.metadata_poll, &[(0, f.x[0]), (8, f.x[8])]);
            if again != 0 {
                return Ok(());
            }
            if let Some(pc) = site.metadata_clear_poll {
                f.regs.safepoint_n(pc, &[(0, f.x[0]), (8, f.x[8])]);
            }
        }
        f.call_v(
            group_callee::CLASS_INITIALIZER,
            site.last_init_return,
            &[0, 8, 9, 20],
            lazy,
            false,
        )?;
        f.x[0] = unsafe { guest_ld64(f.x[21]) };
        if let Some(pc) = site.last_init_poll {
            f.regs.safepoint_n(pc, &[(0, f.x[0])]);
        }
        Ok(())
    }

    /// `BattleManager._instance` from the class in x0 (`x8 = statics`, `x8 = instance`).
    fn manager_instance(f: &mut Frame) -> u64 {
        f.x[8] = unsafe { guest_ld64(f.x[0].wrapping_add(CLASS_STATIC_FIELDS)) };
        f.x[8] = unsafe { guest_ld64(f.x[8].wrapping_add(STATICS_INSTANCE)) };
        f.x[8]
    }

    /// `_target._result_logger._flag_4a = true`, then `damage = 0`.
    fn set_rule_flag(self, f: &mut Frame, run: &mut DriverRun) {
        let target = self.target();
        f.x[8] = target.address();
        f.x[9] = 1;
        f.x[8] = target.result_logger().address();
        prestate::store_u8(f.x[8].wrapping_add(LOGGER_RULE_FLAG), 1);
        run.store(0);
    }

    /// Character 0x5dd.
    fn rule_threshold(self, f: &mut Frame, run: &mut DriverRun) -> Result<(), GuestException> {
        self.manager_test(f, &MANAGER_TEST_THRESHOLD)?;
        let instance = Self::manager_instance(f);
        let count = unsafe { guest_ld32(instance.wrapping_add(MANAGER_COUNT)) } as i32;
        f.x[8] = w(count);
        if count < MANAGER_COUNT_LIMIT {
            return Ok(());
        }
        self.write_literal(f, &LINE_RULE_THRESHOLD)?;
        self.set_rule_flag(f, run);
        Ok(())
    }

    /// Characters 0x5ec and 0x653.
    fn rule_clear(self, f: &mut Frame, run: &mut DriverRun) -> Result<(), GuestException> {
        self.write_literal(f, &LINE_RULE_CLEAR)?;
        self.set_rule_flag(f, run);
        self.clear_hit_numbers(f, &CLEAR_SITE_A)
    }

    /// Character 0x615.
    fn rule_flagged_clear(self, f: &mut Frame, run: &mut DriverRun) -> Result<(), GuestException> {
        self.manager_test(f, &MANAGER_TEST_FLAGGED)?;
        let instance = Self::manager_instance(f);
        f.x[8] = unsafe { guest_ld64(instance.wrapping_add(MANAGER_FLAG_OBJECT)) };
        let flag = unsafe { guest_ld8(f.x[8].wrapping_add(FLAG_OBJECT_BYTE)) };
        f.x[8] = u64::from(flag);
        if flag == 0 {
            return Ok(());
        }
        self.write_literal(f, &LINE_RULE_FLAGGED)?;
        self.set_rule_flag(f, run);
        let skill_obj = self.skill_obj();
        f.x[8] = skill_obj.address();
        let id = skill_obj.skill_id();
        f.x[8] = w(id);
        if id == SKILL_GLOBAL_WORK_FLAG {
            // `GlobalWork.get_inst()._flag_d8 = true`, after the class initialized test of `GlobalWork`
            f.x[8] = unsafe { guest_ld64(super::belome::slots::GLOBAL_WORK_CLASS) };
            f.x[0] = unsafe { guest_ld64(f.x[8]) };
            let word = prestate::read_u32(f.x[0].wrapping_add(CLASS_INITIALIZED));
            f.x[8] = u64::from(word);
            if word == 0 {
                f.call(
                    group_callee::CLASS_INITIALIZER,
                    0x71021ffc4c,
                    &[0, 8, 9],
                    false,
                )?;
            }
            f.x[0] = 0;
            f.call(
                rule_callee::GLOBAL_WORK_GET_INST,
                0x71021ffc54,
                &[0, 8, 9],
                false,
            )?;
            f.x[8] = 1;
            prestate::store_u8(f.x[0].wrapping_add(GLOBAL_WORK_FLAG), 1);
        }
        self.clear_hit_numbers(f, &CLEAR_SITE_B)
    }

    /// `_target._result_logger._multi_number.Clear()` (inlined) and `Add(0)` once per entry it held. Ends at the join of the
    /// rules (x21 = 0 when the logger has no list).
    fn clear_hit_numbers(self, f: &mut Frame, site: &ClearSite) -> Result<(), GuestException> {
        let target = self.target();
        f.x[8] = target.address();
        let logger = target.result_logger();
        f.x[8] = logger.address();
        let Some(numbers) = logger.multi_number() else {
            f.x[21] = 0;
            return Ok(());
        };
        f.x[21] = numbers.address();
        // the generic context of `IsReferenceOrContainsReferences<int>`
        let context = unsafe { guest_ld64(guest_ld64(super::belome::slots::LIST_CONTEXT)) };
        f.x[8] = context;
        let count = prestate::read_u32(numbers.size_address());
        let version = prestate::read_u32(numbers.version_address());
        f.x[20] = u64::from(count);
        f.x[9] = u64::from(version.wrapping_add(1));
        prestate::store_u32(numbers.version_address(), version.wrapping_add(1));
        let level2 = unsafe { guest_ld64(context.wrapping_add(0x20)) };
        f.x[8] = level2;
        let level3 = unsafe { guest_ld64(level2.wrapping_add(0xc0)) };
        f.x[8] = level3;
        f.x[0] = unsafe { guest_ld64(level3.wrapping_add(0xc8)) };
        f.call(
            rule_callee::IS_REFERENCE_OR_CONTAINS_REFERENCES,
            site.references_return,
            &[0, 8, 9, 20, 21],
            false,
        )?;
        if f.x[0] & 1 == 0 {
            prestate::store_u32(numbers.size_address(), 0);
        } else {
            f.x[2] = u64::from(count);
            prestate::store_u32(numbers.size_address(), 0);
            if (count as i32) >= 1 {
                f.x[0] = prestate::read_u64(numbers.items_address());
                f.x[1] = 0;
                f.x[3] = 0;
                f.call(
                    rule_callee::ARRAY_CLEAR,
                    site.clear_return,
                    &[0, 1, 2, 3],
                    false,
                )?;
            }
        }
        if (count as i32) < 1 {
            return Ok(());
        }
        f.x[21] = unsafe { guest_ld64(super::belome::slots::LIST_ADD_METHOD) };
        // The list header as it stands after the stores above: the `Add` callee edits it (and the comparison gives back what
        // it left), so the port carries it from call to call.
        let mut header = [0u8; ListInt::HEADER_BYTES];
        header[..8].copy_from_slice(&prestate::read_u64(numbers.items_address()).to_le_bytes());
        header[8..12].copy_from_slice(&0u32.to_le_bytes());
        header[12..].copy_from_slice(&version.wrapping_add(1).to_le_bytes());
        let mut remaining = count;
        loop {
            f.x[8] = target.address();
            f.x[1] = 0;
            f.x[2] = unsafe { guest_ld64(f.x[21]) };
            f.x[8] = logger.address();
            f.x[0] = numbers.address();
            f.call_out(
                rule_callee::LIST_INT_ADD,
                site.add_return,
                &[0, 1, 2, 8, 20, 21],
                false,
                &mut header,
            )?;
            remaining = remaining.wrapping_sub(1);
            f.x[20] = u64::from(remaining);
            f.regs.safepoint(site.loop_poll, 20, f.x[20]);
            if remaining == 0 {
                break;
            }
        }
        Ok(())
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

/// 0x7102201aa0 `SkillExecter.group_a_b` (replay row with a store journal and out footprint callees).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_skill_group_a_b(x: *mut u64) {
    unsafe {
        let regs = Regs::from_raw(x);
        let (this, damage) = (regs.x(0), regs.x(1));
        let mut frame = Frame::new(regs);
        let _ = SkillExecter::at(this).group_a_b(damage, &mut frame);
    }
}

/// Pre-state hook: the cells the port reads after the lifted body may have changed them, and the ones it stores to.
/// Returns 1 for the first run in a process (the lazy-initialization flag clear, the metadata slots unresolved).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_skill_group_a_b_pre(x: *mut u64) -> i32 {
    unsafe {
        let flag = guest_ld8(group_slots::INIT_FLAG);
        prestate::snapshot(group_slots::INIT_FLAG, 1, true);
        if flag & 1 == 0 {
            return 1;
        }
        let regs = Regs::from_raw(x);
        let this = SkillExecter::at(regs.x(0));
        prestate::snapshot(regs.x(1), 4, true);
        if this.skill_obj().is_null() || this.skill_obj().skill_type() != SKILL_TYPE_RENKEI {
            return 0;
        }
        let class = guest_ld64(guest_ld64(group_slots::BATTLE_MANAGER_CLASS));
        prestate::snapshot(class.wrapping_add(CLASS_INITIALIZED), 4, false);
        prestate::snapshot(guest_ld64(group_slots::STATIC_FLAG_POINTER), 1, true);
        0
    }
}

/// 0x71021ff580 `SkillExecter.damage_calc` (replay row with a store journal; its nested ports are called through their shims).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_skill_damage_calc(x: *mut u64) {
    unsafe {
        let regs = Regs::from_raw(x);
        let this = regs.x(0);
        let mut frame = Frame::new(regs);
        let _ = SkillExecter::at(this).damage_calc(&mut frame);
    }
}

/// Pre-state hook of `damage_calc`: its own cells, and the cells of each nested port, by calling the nested hooks with the
/// argument registers the lifted code would give them. Returns 1 (defer, run lifted and compare nothing) for the first run in
/// a process (any lazy-initialization flag clear), for the four character ids the port does not cover, and when the lifted
/// `group_a_b` or `calc_renkei_ratio` could initialize a class the driver tests itself afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_skill_damage_calc_pre(x: *mut u64) -> i32 {
    unsafe {
        let flag = guest_ld8(driver_slots::INIT_FLAG);
        prestate::snapshot(driver_slots::INIT_FLAG, 1, true);
        if flag & 1 == 0 {
            return 1;
        }
        let regs = Regs::from_raw(x);
        let this = SkillExecter::at(regs.x(0));
        let target = this.target();
        let skill = this.skill_obj();
        // the stack cells: `damage` at `x29 - 4` = entry sp - 0x34, `ratio` at `sp + 8` = entry sp - 0x38
        let damage_cell = regs.sp().wrapping_sub(0x34);
        let ratio_cell = regs.sp().wrapping_sub(0x38);
        let mut nested = [0u64; 32];
        nested[0] = this.address();
        nested[1] = damage_cell;
        if smrpg_skill_group_a_b_pre(nested.as_mut_ptr()) != 0
            || smrpg_skill_calc_renkei_ratio_pre(nested.as_mut_ptr()) != 0
        {
            return 1;
        }
        nested[1] = skill.address();
        nested[2] = ratio_cell;
        if smrpg_skill_calc_belome_barrior_ratio_pre(nested.as_mut_ptr()) != 0
            || smrpg_skill_set_acction_command_ratio_pre(nested.as_mut_ptr()) != 0
        {
            return 1;
        }
        nested[0] = target.address();
        let _ = smrpg_param_use_damage_guard_pre(nested.as_mut_ptr());
        // the cells the port itself stores to or reads after the lifted body ran
        let logger = target.result_logger().address();
        for offset in [
            CharaSkillResultLogger::DAMAGE_NUMBER,
            CharaSkillResultLogger::DAMAGE_STATE,
            CharaSkillResultLogger::EFFECTIVE_STATE,
        ] {
            prestate::snapshot(logger.wrapping_add(offset), 4, true);
        }
        let character = target.character_id();
        if matches!(
            character,
            CHARACTER_CLEAR_A | CHARACTER_CLEAR_B | CHARACTER_FLAGGED_CLEAR
        ) {
            // the list header at the clear, and the logger flag the three rules set
            let logger_cell = target.result_logger();
            prestate::snapshot(
                logger_cell.address().wrapping_add(LOGGER_RULE_FLAG),
                1,
                true,
            );
            if let Some(numbers) = logger_cell.multi_number() {
                prestate::snapshot(numbers.items_address(), 8, false);
                prestate::snapshot(numbers.size_address(), 4, true);
                prestate::snapshot(numbers.version_address(), 4, true);
            }
        }
        if character == CHARACTER_THRESHOLD {
            prestate::snapshot(
                target
                    .result_logger()
                    .address()
                    .wrapping_add(LOGGER_RULE_FLAG),
                1,
                true,
            );
        }
        if character == CHARACTER_FLAGGED_CLEAR {
            // `GlobalWork`: the class initialized word, and the flag of its instance (the static field that `get_inst` returns)
            let class = guest_ld64(guest_ld64(super::belome::slots::GLOBAL_WORK_CLASS));
            prestate::snapshot(class.wrapping_add(CLASS_INITIALIZED), 4, false);
            let statics = guest_ld64(class.wrapping_add(CLASS_STATIC_FIELDS));
            if statics != 0 {
                let instance = guest_ld64(statics);
                if instance != 0 {
                    prestate::snapshot(instance.wrapping_add(GLOBAL_WORK_FLAG), 1, true);
                }
            }
        }
        let math_word =
            guest_ld64(guest_ld64(driver_slots::MATH_CLASS)).wrapping_add(CLASS_INITIALIZED);
        let manager_word = guest_ld64(guest_ld64(driver_slots::BATTLE_MANAGER_CLASS))
            .wrapping_add(CLASS_INITIALIZED);
        let static_flag = guest_ld64(driver_slots::STATIC_FLAG_POINTER);
        prestate::snapshot(math_word, 4, false);
        prestate::snapshot(manager_word, 4, false);
        prestate::snapshot(static_flag, 1, true);
        let nested_may_initialize = skill.skill_type() == SKILL_TYPE_RENKEI
            || skill.skill_id() == super::renkei::skill::RENKEI_RANDOM_HITS;
        if nested_may_initialize
            && (guest_ld32(math_word) == 0
                || guest_ld32(manager_word) == 0
                || guest_ld8(static_flag) == 0)
        {
            return 1;
        }
        // skill 0x2c3 makes `calc_renkei_ratio` replace the logger's hit list with a new one, and `calc_belome_barrior_ratio` (which
        // follows) then reads the fresh list's header, which the pre-state cannot hold: that call runs the lifted body only
        if skill.skill_id() == super::renkei::skill::RENKEI_RANDOM_HITS
            && (character == super::belome::BELOME_CHARACTER_ID
                || matches!(
                    character,
                    CHARACTER_CLEAR_A | CHARACTER_CLEAR_B | CHARACTER_FLAGGED_CLEAR
                ))
        {
            return 1;
        }
        0
    }
}

#[cfg(test)]
mod tests;
