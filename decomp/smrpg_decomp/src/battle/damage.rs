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

use super::frame::Frame;
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

#[cfg(test)]
mod tests;
