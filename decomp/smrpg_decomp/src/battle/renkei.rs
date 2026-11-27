//! `Battle.SkillExecter.calc_renkei_ratio(ref int damage)` (0x71021f5ed0, task-58), one of the formula callees of
//! `damage_calc`. Renkei is the combo bonus: a handful of skill ids scale the damage so far by a fixed factor, and
//! one of them (0x2c3) also rolls a random bonus for every hit of a multi-hit skill.
//!
//! In C# terms (names are IL2CPP's; the debug lines go to `_monitor`, a `DebugMonitor`, and are empty calls here):
//!
//! ```text
//! void calc_renkei_ratio(ref int damage) {
//!     switch (_skill_obj._skill_id) {
//!         case 0x2d2: _monitor.write(<text>); damage = damage * 9 / 8; break;
//!         case 0x2bd: _monitor.write(<text>); damage = damage * 5 / 4; break;
//!         case 0x2ba: _monitor.write(<text>); damage = damage * 8 / 4; break;   // `(n << 3) / 4`, rounded toward zero
//!         case 0x2c9: _monitor.write(<text>); damage = damage * 6 / 4; break;
//!         case 0x2c3:
//!             _monitor.write(<text>);
//!             damage = (int)((float)damage * 1.7f);                              // an infinite product becomes int.MinValue
//!             _monitor.write(<text> + damage.ToString()); _monitor.writeLine(); _monitor.write(<text>);
//!             List<int> numbers = new List<int>();
//!             _target._result_logger._multi_number = numbers;
//!             for (int i = 0; i < _target._target_counter; i++) {
//!                 float rate = ((float)Random.Range(0, 21) * 0.01f + 0.9f) / 5f * (float)damage;
//!                 int hit = Math.Max((int)rate, 1);                              // an infinite rate becomes int.MinValue first
//!                 _target._result_logger._multi_number.Add(hit);
//!                 _monitor.write(String.Concat(<text>, (i + 1).ToString(), <text>, hit.ToString(), <text>));
//!             }
//!             if (counter < 1) _target._result_logger._number_state = NumberState.DAMAGE;
//!             break;
//!     }
//!     (all but 0x2c3) _monitor.write(String.Concat(<text>, damage.ToString()));
//! }
//! ```
//!
//! (`(int)f` is `fcvtzs`: a NaN is 0, an overflow saturates, and the compiled code replaces the one value `+inf` by
//! `int.MinValue`.) The compiled function begins with IL2CPP's lazy initialization (a flag byte and 13 metadata slots).
//!
//! This port is a replay row with a pre-state hook (`QLEAF`). The float work goes through `Regs::f32_*` (`fp.rs`), so
//! results and the cumulative FPSR flags are the original's and `ab` mode checks each operation against the lifted
//! helper. It keeps the lifted function's registers in a `Frame` (see `frame.rs`) because the comparison looks at the
//! whole register file at every call.
//!
//! Callees (each is a traced row; see `leaves.def`): `DebugMonitor.write` and `writeLine` (empty lifted functions,
//! ported rows), `Int32.ToString` (0x710213a840, `OLEAF`: it reads the int through x0, a slot of this function's frame
//! or `damage` itself), `String.Concat` (two overloads), `Object.new` (0x71000c44d0), `List<int>..ctor` (0x7101b4fcc0,
//! `OLEAF`: it writes the list header), the write barrier (0x71000c4090), `Random.Range`, the class initializer check,
//! `Math.Max` (a ported row) and `List<int>.Add` (`OLEAF`, the list header is carried from call to call).

use super::damage::{CharaSkillResultLogger, SkillExecter};
use super::frame::Frame;
use super::{BattleCharacterParam, GuestException, GuestObject, Regs};
use crate::bridge::prestate;
use crate::guest_ld8;
use crate::guest_ld32;
use crate::guest_ld64;

/// `SkillObject._skill_id` values the compiled switch knows (the combo skills).
pub mod skill {
    pub const RENKEI_8_OVER_4: i32 = 0x2ba;
    pub const RENKEI_5_OVER_4: i32 = 0x2bd;
    pub const RENKEI_RANDOM_HITS: i32 = 0x2c3;
    pub const RENKEI_6_OVER_4: i32 = 0x2c9;
    pub const RENKEI_9_OVER_8: i32 = 0x2d2;
}

/// `CharaSkillResultLogger.NumberState.DAMAGE`.
const NUMBER_STATE_DAMAGE: u32 = 2;
/// `1.7f`, `0.01f`, `0.9f`, `5f` and `+inf` as the compiled code builds them.
const RENKEI_SCALE: f32 = f32::from_bits(0x3fd9_999a);
const HIT_STEP: f32 = f32::from_bits(0x3c23_d70a);
const HIT_BASE: f32 = f32::from_bits(0x3f66_6666);
const HIT_DIVISOR: f32 = f32::from_bits(0x40a0_0000);
const INFINITY_BITS: u32 = 0x7f80_0000;
/// `fcvtzs` leaves `i32::MAX` for `+inf`; the compiled code stores `int.MinValue` instead.
const INFINITY_RESULT: u32 = 0x8000_0000;
/// The upper bound of `Random.Range(0, 21)` as passed (exclusive).
const RANDOM_BOUND: u64 = 0x15;

/// The lazy-initialization flag byte and the metadata slots (data page + offset). The nine `STRING_*` slots are string
/// literals (`DebugMonitor` text), the others a class, a constructor method info and the generic `List<int>.Add` method.
pub mod slots {
    pub const INIT_FLAG: u64 = 0x7103cba000 + 0xd5e;
    pub const FLAG_PAGE: u64 = 0x7103cba000;
    /// `List<int>.Add` method info (its first word is the generic argument of the call).
    pub const LIST_ADD_METHOD: u64 = 0x7103a4d000 + 0x788;
    pub const LIST_CTOR_METHOD: u64 = 0x7103a4d000 + 0x700;
    pub const LIST_CLASS: u64 = 0x7103a4d000 + 0x6f8;
    /// The class whose initializer the hit loop checks (`+0xe0` is its initialized word).
    pub const INITIALIZED_CLASS: u64 = 0x7103a46000 + 0xa90;
    pub const STRING_HIT_PREFIX: u64 = 0x7103a83000 + 0x520;
    pub const STRING_HIT_NUMBER: u64 = 0x7103a83000 + 0x528;
    pub const STRING_5_OVER_4: u64 = 0x7103a83000 + 0x530;
    pub const STRING_6_OVER_4: u64 = 0x7103a83000 + 0x538;
    pub const STRING_DAMAGE_IS: u64 = 0x7103a83000 + 0x540;
    pub const STRING_8_OVER_4: u64 = 0x7103a83000 + 0x548;
    pub const STRING_9_OVER_8: u64 = 0x7103a83000 + 0x550;
    pub const STRING_FLOAT_SCALE: u64 = 0x7103a83000 + 0x558;
    pub const STRING_HITS: u64 = 0x7103a83000 + 0x560;
    /// The 13 slots the first run resolves, in the order of its 13 `initialize_runtime_metadata` calls.
    pub const INIT_ORDER: [u64; 13] = [
        LIST_ADD_METHOD,
        LIST_CTOR_METHOD,
        LIST_CLASS,
        INITIALIZED_CLASS,
        STRING_HIT_PREFIX,
        STRING_HIT_NUMBER,
        STRING_5_OVER_4,
        STRING_6_OVER_4,
        STRING_DAMAGE_IS,
        STRING_8_OVER_4,
        STRING_9_OVER_8,
        STRING_FLOAT_SCALE,
        STRING_HITS,
    ];
}

/// The lifted functions the body calls.
mod callee {
    pub const INITIALIZE_METADATA: u64 = 0x71000c4150;
    pub const CLASS_INITIALIZER: u64 = 0x71000c4330;
    pub const DEBUG_WRITE: u64 = 0x71018bebc0;
    pub const DEBUG_WRITE_LINE: u64 = 0x71018bebf0;
    pub const INT32_TO_STRING: u64 = 0x710213a840;
    pub const STRING_CONCAT_2: u64 = 0x71020147d0;
    pub const STRING_CONCAT_4: u64 = 0x7102023cb0;
    pub const OBJECT_NEW: u64 = 0x71000c44d0;
    pub const LIST_INT_CTOR: u64 = 0x7101b4fcc0;
    pub const WRITE_BARRIER: u64 = 0x71000c4090;
    pub const MATH_MAX: u64 = 0x710213e670;
    pub const LIST_INT_ADD: u64 = 0x710015b4c0;
    pub const RANDOM_RANGE: u64 = 0x7101797f10;
}

/// The address after each call (what the lifted code writes to x30) and the registers written before it. Every list
/// includes x30 (written at each call); `V` lists are the vector registers.
mod site {
    pub const FIRST_INIT_RETURN: u64 = 0x71021f5f10;
    /// Return addresses of the 13 initialization calls are 12 bytes apart.
    pub const INIT_STEP: u64 = 12;
    pub const FIRST_CALL: &[usize] = &[0, 8, 19, 20, 21, 29, 30];
    pub const LATER_INIT_CALL: &[usize] = &[0, 30];

    /// `DebugMonitor.write(text)` at the head of each skill case: x0 monitor, x1 text, x2 0, x8 the slot value.
    pub const CASE_WRITE: &[usize] = &[0, 1, 2, 8, 19, 20, 21, 29, 30];
    pub const CASE_2D2_RETURN: u64 = 0x71021f5ff0;
    pub const CASE_2BD_RETURN: u64 = 0x71021f6038;
    pub const CASE_2BA_RETURN: u64 = 0x71021f605c;
    pub const CASE_2C3_RETURN: u64 = 0x71021f6090;
    pub const CASE_2C9_RETURN: u64 = 0x71021f62b4;

    /// The float case: `ToString(damage)`, `Concat`, `write`, `writeLine`, `write`.
    pub const FLOAT_TO_STRING_RETURN: u64 = 0x71021f60d4;
    pub const FLOAT_TO_STRING: &[usize] = &[0, 1, 8, 9, 21, 30];
    pub const FLOAT_TO_STRING_V: &[usize] = &[0, 1];
    pub const FLOAT_CONCAT_RETURN: u64 = 0x71021f60ec;
    pub const FLOAT_WRITE_RETURN: u64 = 0x71021f60fc;
    pub const FLOAT_WRITE_LINE_RETURN: u64 = 0x71021f6108;
    pub const FLOAT_WRITE_LINE: &[usize] = &[0, 1, 30];
    pub const FLOAT_HITS_WRITE_RETURN: u64 = 0x71021f6120;
    pub const OBJECT_NEW_RETURN: u64 = 0x71021f6138;
    pub const OBJECT_NEW: &[usize] = &[0, 8, 21, 30];
    pub const LIST_CTOR_RETURN: u64 = 0x71021f614c;
    pub const LIST_CTOR: &[usize] = &[1, 8, 22, 30];
    pub const WRITE_BARRIER_RETURN: u64 = 0x71021f615c;
    pub const WRITE_BARRIER: &[usize] = &[0, 1, 21, 30];

    /// The shared tail of the integer cases: `ToString(damage)` (x8, x9, x10 flushed), `Concat`, `write`.
    pub const TAIL_TO_STRING_RETURN: u64 = 0x71021f62e4;
    pub const TAIL_TO_STRING: &[usize] = &[0, 1, 8, 9, 10, 20, 30];
    pub const TAIL_CONCAT_RETURN: u64 = 0x71021f62fc;
    pub const TAIL_WRITE_RETURN: u64 = 0x71021f630c;

    /// The loop body.
    pub const RANDOM_RETURN: u64 = 0x71021f623c;
    pub const RANDOM: &[usize] = &[0, 1, 2, 8, 9, 21, 24, 25, 26, 27, 28, 30];
    pub const RANDOM_V: &[usize] = &[8];
    pub const POLL_BEFORE_CLASS_TEST: u64 = 0x71021f6290;
    pub const CLASS_INIT_RETURN: u64 = 0x71021f6298;
    pub const CLASS_INIT: &[usize] = &[0, 8, 9, 22, 30];
    pub const CLASS_INIT_V: &[usize] = &[0, 1];
    pub const MAX_RETURN: u64 = 0x71021f61ac;
    pub const MAX: &[usize] = &[0, 1, 2, 8, 9, 22, 30];
    pub const MAX_V: &[usize] = &[0, 1];
    pub const ADD_RETURN: u64 = 0x71021f61c8;
    pub const ADD: &[usize] = &[0, 1, 2, 8, 30];
    pub const HIT_TO_STRING_RETURN: u64 = 0x71021f61e0;
    pub const HIT_TO_STRING: &[usize] = &[0, 1, 22, 25, 30];
    pub const NUMBER_TO_STRING_RETURN: u64 = 0x71021f61f0;
    pub const NUMBER_TO_STRING: &[usize] = &[0, 1, 23, 30];
    pub const HIT_CONCAT_RETURN: u64 = 0x71021f6208;
    pub const HIT_CONCAT: &[usize] = &[0, 1, 2, 3, 4, 30];
    pub const HIT_WRITE_RETURN: u64 = 0x71021f6218;
    pub const HIT_WRITE: &[usize] = &[0, 1, 2, 30];

    /// `write(..)` calls that follow a `Concat`: x0, x1, x2 flushed.
    pub const WRITE: &[usize] = &[0, 1, 2, 30];
    pub const CONCAT: &[usize] = &[0, 1, 2, 8, 30];

    pub const RETURN: &[usize] = &[19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29];
}

impl SkillExecter {
    const MONITOR: u64 = 0x10;
}

impl BattleCharacterParam {
    const TARGET_COUNTER: u64 = 0x8c;

    /// `_target_counter`: how many hits a multi-hit skill makes on this character.
    fn target_counter(self) -> i32 {
        self.i32_field(Self::TARGET_COUNTER)
    }
}

impl CharaSkillResultLogger {
    const NUMBER_STATE: u64 = 0x2c;

    pub(crate) fn number_state_address(self) -> u64 {
        self.address().wrapping_add(Self::NUMBER_STATE)
    }

    pub(crate) fn multi_number_address(self) -> u64 {
        self.address().wrapping_add(Self::MULTI_NUMBER)
    }
}

/// `x = sign-extended 32-bit pattern` as the lifted code leaves a `w` register: zero extended.
fn w(value: i32) -> u64 {
    u64::from(value as u32)
}

/// `(n * factor) / 4` as the compiled code does it: a signed divide by 4 through `add 3` on negative values and `asr`.
/// Returns (the value before the shift, x9 = the bias value) for the register file.
fn divide_by_four(product: i32) -> (i32, i32) {
    let biased = product.wrapping_add(3);
    ((if product < 0 { biased } else { product }) >> 2, biased)
}

impl SkillExecter {
    fn monitor(self) -> u64 {
        unsafe { guest_ld64(self.address().wrapping_add(Self::MONITOR)) }
    }

    /// `calc_renkei_ratio(ref int damage)`: `damage` is the guest address of the int.
    pub fn calc_renkei_ratio(self, damage: u64, f: &mut Frame) -> Result<(), GuestException> {
        let flag = prestate::read_u8(slots::INIT_FLAG);
        // prologue: `str d8, [sp, #-0x70]!`, `stp x29, x30, [sp, #0x10]`, `add x29, sp, #0x10`; x21 = the flag page,
        // x8 = the flag byte, x19 = damage, x20 = this
        f.x[31] = f.entry[31].wrapping_sub(0x70);
        f.x[29] = f.x[31].wrapping_add(0x10);
        f.x[21] = slots::FLAG_PAGE;
        f.x[8] = u64::from(flag);
        f.x[19] = damage;
        f.x[20] = self.address();
        if flag & 1 == 0 {
            self.resolve_renkei_metadata(f)?;
            prestate::store_u8(slots::INIT_FLAG, 1);
        }
        // The frame's two int slots: `[x29 - 4]` and `[sp + 8]`, both cleared here.
        let mut slot_a = [0u8; 4];
        let mut slot_b = [0u8; 4];
        let skill_id = self.skill_obj().skill_id();
        f.x[8] = w(skill_id);
        let result = match skill_id {
            skill::RENKEI_9_OVER_8 => self.case_nine_eighths(damage, f),
            skill::RENKEI_5_OVER_4 | skill::RENKEI_6_OVER_4 | skill::RENKEI_8_OVER_4 => {
                self.case_integer_scale(skill_id, damage, f)
            }
            skill::RENKEI_RANDOM_HITS => self.case_random_hits(damage, f, &mut slot_a, &mut slot_b),
            _ => Ok(()),
        };
        result?;
        f.leave_v8(site::RETURN);
        Ok(())
    }

    /// The 13 `initialize_runtime_metadata` calls of the first run, then `x8 = 1` (the flag store is the caller's).
    fn resolve_renkei_metadata(self, f: &mut Frame) -> Result<(), GuestException> {
        for (n, slot) in slots::INIT_ORDER.into_iter().enumerate() {
            f.x[0] = unsafe { guest_ld64(slot) };
            let list = if n == 0 {
                site::FIRST_CALL
            } else {
                site::LATER_INIT_CALL
            };
            f.call(
                callee::INITIALIZE_METADATA,
                site::FIRST_INIT_RETURN + site::INIT_STEP * n as u64,
                list,
                n == 0,
            )?;
        }
        f.x[8] = 1;
        Ok(())
    }

    /// `x8 = [slot]`, `x0 = _monitor`, `x2 = 0`, `x1 = [x8]`, then `DebugMonitor.write`.
    fn write_text(
        self,
        f: &mut Frame,
        slot: u64,
        return_address: u64,
        list: &[usize],
        with_sp: bool,
    ) -> Result<(), GuestException> {
        f.x[8] = unsafe { guest_ld64(slot) };
        f.x[0] = self.monitor();
        f.x[2] = 0;
        f.x[1] = unsafe { guest_ld64(f.x[8]) };
        f.call(callee::DEBUG_WRITE, return_address, list, with_sp)
    }

    /// Skill 0x2d2: `damage = damage * 9 / 8`. The registers the case leaves in x8 and x9 are never flushed again.
    fn case_nine_eighths(self, damage: u64, f: &mut Frame) -> Result<(), GuestException> {
        self.write_text(
            f,
            slots::STRING_9_OVER_8,
            site::CASE_2D2_RETURN,
            site::CASE_WRITE,
            true,
        )?;
        let value = prestate::read_u32(damage) as i32;
        let product = value.wrapping_add(value.wrapping_shl(3));
        let bias = product.wrapping_add(7);
        let scaled = (if product < 0 { bias } else { product }) >> 3;
        prestate::store_u32(damage, scaled as u32);
        Ok(())
    }

    /// Skills 0x2bd (x5/4), 0x2c9 (x6/4) and 0x2ba (x8/4): scale, store, then the shared tail
    /// `damage.ToString()`, `Concat(text, ..)`, `write`.
    fn case_integer_scale(
        self,
        skill_id: i32,
        damage: u64,
        f: &mut Frame,
    ) -> Result<(), GuestException> {
        let (slot, ret) = match skill_id {
            skill::RENKEI_5_OVER_4 => (slots::STRING_5_OVER_4, site::CASE_2BD_RETURN),
            skill::RENKEI_6_OVER_4 => (slots::STRING_6_OVER_4, site::CASE_2C9_RETURN),
            _ => (slots::STRING_8_OVER_4, site::CASE_2BA_RETURN),
        };
        self.write_text(f, slot, ret, site::CASE_WRITE, true)?;
        let value = prestate::read_u32(damage) as i32;
        let scaled = if skill_id == skill::RENKEI_8_OVER_4 {
            // `lsl w9, w8, #3`; `bfi w10, w8, #3, #29` with w10 = 3: ((n << 3) | 3); the sign test picks one
            let shifted = value.wrapping_shl(3);
            let with_low_bits = shifted | 3;
            f.x[9] = w(shifted);
            f.x[10] = w(with_low_bits);
            (if shifted < 0 { with_low_bits } else { shifted }) >> 2
        } else {
            let times = if skill_id == skill::RENKEI_5_OVER_4 {
                value.wrapping_add(value.wrapping_shl(2))
            } else {
                value.wrapping_add(value.wrapping_shl(1)).wrapping_shl(1)
            };
            let (shifted, bias) = divide_by_four(times);
            f.x[9] = w(bias);
            shifted
        };
        f.x[8] = w(scaled);
        prestate::store_u32(damage, scaled as u32);
        // x20 = _monitor from here (a callee-saved register, restored by the epilogue)
        f.x[20] = self.monitor();
        f.x[0] = damage;
        f.x[1] = 0;
        let mut area = (scaled as u32).to_le_bytes();
        f.call_out(
            callee::INT32_TO_STRING,
            site::TAIL_TO_STRING_RETURN,
            site::TAIL_TO_STRING,
            false,
            &mut area,
        )?;
        f.x[8] = unsafe { guest_ld64(slots::STRING_DAMAGE_IS) };
        f.x[1] = f.x[0];
        f.x[0] = unsafe { guest_ld64(f.x[8]) };
        f.x[2] = 0;
        f.call(
            callee::STRING_CONCAT_2,
            site::TAIL_CONCAT_RETURN,
            site::CONCAT,
            false,
        )?;
        f.x[1] = f.x[0];
        f.x[0] = f.x[20];
        f.x[2] = 0;
        f.call(
            callee::DEBUG_WRITE,
            site::TAIL_WRITE_RETURN,
            site::WRITE,
            false,
        )
    }

    /// Skill 0x2c3: float scale, then a random bonus for each of the target's hits.
    fn case_random_hits(
        self,
        damage: u64,
        f: &mut Frame,
        slot_a: &mut [u8; 4],
        slot_b: &mut [u8; 4],
    ) -> Result<(), GuestException> {
        self.write_text(
            f,
            slots::STRING_FLOAT_SCALE,
            site::CASE_2C3_RETURN,
            site::CASE_WRITE,
            true,
        )?;
        let mut value = prestate::read_u32(damage) as i32;
        // `ldr s0, [x19]`, `scvtf s0, s0`, `fmov s1, w8` (1.7f), `fmul`, `fcvtzs w8, s0`, the +inf test
        f.v[0] = (u64::from(value as u32), 0);
        let converted = f.regs.f32_from_i32(value);
        f.set_s(0, converted);
        f.x[8] = u64::from(RENKEI_SCALE.to_bits());
        f.set_s(1, RENKEI_SCALE);
        let product = f.regs.f32_arith(0, converted, RENKEI_SCALE);
        f.set_s(0, product);
        let truncated = f.regs.i32_from_f32_trunc(product);
        f.x[8] = w(truncated);
        f.x[9] = u64::from(INFINITY_BITS);
        f.set_s(1, f32::from_bits(INFINITY_BITS));
        let is_infinity = f.regs.f32_compare(product, f32::from_bits(INFINITY_BITS)).z;
        f.x[9] = u64::from(INFINITY_RESULT);
        if is_infinity {
            f.x[8] = u64::from(INFINITY_RESULT);
        }
        value = f.x[8] as u32 as i32;
        prestate::store_u32(damage, value as u32);
        // `_monitor.write(damage.ToString())` and the rest of the log lines
        f.x[21] = self.monitor();
        f.x[0] = damage;
        f.x[1] = 0;
        let mut area = (value as u32).to_le_bytes();
        f.call_out_v(
            callee::INT32_TO_STRING,
            site::FLOAT_TO_STRING_RETURN,
            site::FLOAT_TO_STRING,
            site::FLOAT_TO_STRING_V,
            false,
            &mut area,
        )?;
        value = i32::from_le_bytes(area);
        f.x[8] = unsafe { guest_ld64(slots::STRING_DAMAGE_IS) };
        f.x[1] = f.x[0];
        f.x[0] = unsafe { guest_ld64(f.x[8]) };
        f.x[2] = 0;
        f.call(
            callee::STRING_CONCAT_2,
            site::FLOAT_CONCAT_RETURN,
            site::CONCAT,
            false,
        )?;
        f.x[1] = f.x[0];
        f.x[0] = f.x[21];
        f.x[2] = 0;
        f.call(
            callee::DEBUG_WRITE,
            site::FLOAT_WRITE_RETURN,
            site::WRITE,
            false,
        )?;
        f.x[0] = self.monitor();
        f.x[1] = 0;
        f.call(
            callee::DEBUG_WRITE_LINE,
            site::FLOAT_WRITE_LINE_RETURN,
            site::FLOAT_WRITE_LINE,
            false,
        )?;
        self.write_text(
            f,
            slots::STRING_HITS,
            site::FLOAT_HITS_WRITE_RETURN,
            site::CONCAT,
            false,
        )?;
        let target = self.target();
        f.x[8] = target.address();
        let logger = target.result_logger();
        f.x[21] = logger.address();
        // `_multi_number = new List<int>()`
        f.x[8] = unsafe { guest_ld64(slots::LIST_CLASS) };
        f.x[0] = unsafe { guest_ld64(f.x[8]) };
        f.call(
            callee::OBJECT_NEW,
            site::OBJECT_NEW_RETURN,
            site::OBJECT_NEW,
            false,
        )?;
        f.x[8] = unsafe { guest_ld64(slots::LIST_CTOR_METHOD) };
        f.x[1] = unsafe { guest_ld64(f.x[8]) };
        f.x[22] = f.x[0];
        let mut header = [0u8; LIST_HEADER_BYTES];
        f.call_out(
            callee::LIST_INT_CTOR,
            site::LIST_CTOR_RETURN,
            site::LIST_CTOR,
            false,
            &mut header,
        )?;
        f.x[21] = f.x[21].wrapping_add(CharaSkillResultLogger::MULTI_NUMBER);
        prestate::store_u64(f.x[21], f.x[22]);
        f.x[0] = f.x[21];
        f.x[1] = f.x[22];
        f.call(
            callee::WRITE_BARRIER,
            site::WRITE_BARRIER_RETURN,
            site::WRITE_BARRIER,
            false,
        )?;
        f.x[8] = target.address();
        let mut count = target.target_counter();
        f.x[9] = w(count);
        if count < 1 {
            self.finish_without_hits(f, target);
            return Ok(());
        }
        // loop-invariant registers
        f.x[24] = unsafe { guest_ld64(slots::LIST_ADD_METHOD) };
        f.x[21] = unsafe { guest_ld64(slots::STRING_HIT_NUMBER) };
        f.x[27] = unsafe { guest_ld64(slots::STRING_HIT_PREFIX) };
        f.set_s(8, HIT_DIVISOR);
        f.x[25] = 0;
        f.x[28] = u64::from(HIT_BASE.to_bits());
        f.x[26] = u64::from(INFINITY_BITS);
        let mut first_iteration = true;
        loop {
            // 6228: `Random.Range(0, 21)`
            f.x[0] = 0;
            f.x[1] = RANDOM_BOUND;
            slot_a.fill(0);
            f.x[2] = 0;
            f.call_v(
                callee::RANDOM_RANGE,
                site::RANDOM_RETURN,
                site::RANDOM,
                site::RANDOM_V,
                false,
            )?;
            let roll = f.x[0] as u32 as i32;
            // rate = ((float)roll * 0.01f + 0.9f) / 5f * (float)damage
            let rolled = f.regs.f32_from_i32(roll);
            f.set_s(0, rolled);
            f.x[8] = u64::from(HIT_STEP.to_bits());
            f.set_s(1, HIT_STEP);
            let scaled = f.regs.f32_arith(0, rolled, HIT_STEP);
            f.set_s(0, scaled);
            f.set_s(1, f32::from_bits(f.x[28] as u32));
            let biased = f.regs.f32_arith(2, scaled, f32::from_bits(f.x[28] as u32));
            f.set_s(0, biased);
            f.v[1] = (u64::from(value as u32), 0);
            let divided = f.regs.f32_div(biased, f.s(8));
            f.set_s(0, divided);
            let damage_float = f.regs.f32_from_i32(value);
            f.set_s(1, damage_float);
            let rate = f.regs.f32_arith(0, divided, damage_float);
            f.set_s(0, rate);
            f.set_s(1, f32::from_bits(INFINITY_BITS));
            let infinite = f.regs.f32_compare(rate, f32::from_bits(INFINITY_BITS)).z;
            let hit = f.regs.i32_from_f32_trunc(rate);
            f.x[8] = w(hit);
            f.x[9] = u64::from(INFINITY_RESULT);
            f.x[22] = if infinite {
                u64::from(INFINITY_RESULT)
            } else {
                w(hit)
            };
            slot_a.copy_from_slice(&(f.x[22] as u32).to_le_bytes());
            // `GameClass.initialized`: the class word is read before the poll, the initializer runs when it is clear
            f.x[8] = unsafe { guest_ld64(slots::INITIALIZED_CLASS) };
            f.x[0] = unsafe { guest_ld64(f.x[8]) };
            let class_word = class_initialized_word(f.x[0], first_iteration);
            f.x[8] = u64::from(class_word);
            f.regs.safepoint_nv(
                site::POLL_BEFORE_CLASS_TEST,
                &[(0, f.x[0]), (8, f.x[8]), (9, f.x[9]), (22, f.x[22])],
                &[(0, f.v[0].0, f.v[0].1), (1, f.v[1].0, f.v[1].1)],
            );
            if class_word == 0 {
                f.call_v(
                    callee::CLASS_INITIALIZER,
                    site::CLASS_INIT_RETURN,
                    site::CLASS_INIT,
                    site::CLASS_INIT_V,
                    false,
                )?;
                f.regs.safepoint_n(site::CLASS_INIT_RETURN, &[]);
            }
            first_iteration = false;
            // 619c: `Math.Max(hit, 1)`
            f.x[0] = f.x[22] as u32 as u64;
            f.x[1] = 1;
            f.x[2] = 0;
            f.call_v(
                callee::MATH_MAX,
                site::MAX_RETURN,
                site::MAX,
                site::MAX_V,
                false,
            )?;
            let kept = f.x[0] as u32;
            slot_a.copy_from_slice(&kept.to_le_bytes());
            f.x[8] = unsafe { guest_ld64(target.address().wrapping_add(0xb8)) };
            f.x[1] = u64::from(kept);
            f.x[0] =
                unsafe { guest_ld64(f.x[8].wrapping_add(CharaSkillResultLogger::MULTI_NUMBER)) };
            f.x[2] = unsafe { guest_ld64(f.x[24]) };
            f.call_out(
                callee::LIST_INT_ADD,
                site::ADD_RETURN,
                site::ADD,
                false,
                &mut header,
            )?;
            // the debug line: `write(Concat(hit prefix, (i + 1).ToString(), hit number, hit.ToString(), ..))`
            f.x[22] = self.monitor();
            f.x[25] = u64::from((f.x[25] as u32).wrapping_add(1));
            f.x[0] = f.x[31].wrapping_add(8);
            f.x[1] = 0;
            slot_b.copy_from_slice(&(f.x[25] as u32).to_le_bytes());
            f.call_out(
                callee::INT32_TO_STRING,
                site::HIT_TO_STRING_RETURN,
                site::HIT_TO_STRING,
                false,
                &mut slot_b[..],
            )?;
            f.x[23] = f.x[0];
            f.x[0] = f.x[29].wrapping_sub(4);
            f.x[1] = 0;
            f.call_out(
                callee::INT32_TO_STRING,
                site::NUMBER_TO_STRING_RETURN,
                site::NUMBER_TO_STRING,
                false,
                &mut slot_a[..],
            )?;
            f.x[3] = f.x[0];
            f.x[0] = unsafe { guest_ld64(f.x[21]) };
            f.x[1] = f.x[23];
            f.x[2] = unsafe { guest_ld64(f.x[27]) };
            f.x[4] = 0;
            f.call(
                callee::STRING_CONCAT_4,
                site::HIT_CONCAT_RETURN,
                site::HIT_CONCAT,
                false,
            )?;
            f.x[1] = f.x[0];
            f.x[0] = f.x[22];
            f.x[2] = 0;
            f.call(
                callee::DEBUG_WRITE,
                site::HIT_WRITE_RETURN,
                site::HIT_WRITE,
                false,
            )?;
            f.x[8] = target.address();
            count = target.target_counter();
            f.x[9] = w(count);
            if (f.x[25] as u32 as i32) >= count {
                break;
            }
        }
        self.finish_without_hits(f, target);
        Ok(())
    }

    /// 6310: `_target._result_logger._number_state = NumberState.DAMAGE`.
    fn finish_without_hits(self, f: &mut Frame, target: BattleCharacterParam) {
        f.x[8] = target.result_logger().address();
        f.x[9] = 2;
        prestate::store_u32(
            target.result_logger().number_state_address(),
            NUMBER_STATE_DAMAGE,
        );
    }
}

/// The class's initialized word: read before the lifted body ran on the first loop pass, afterwards the initializer
/// the first pass called has set it (a trial reads memory after the lifted body, so the later passes read it live).
fn class_initialized_word(class: u64, first_iteration: bool) -> u32 {
    let address = class.wrapping_add(0xe0);
    if first_iteration {
        prestate::read_u32(address)
    } else {
        unsafe { guest_ld32(address) }
    }
}

/// Bytes of the `List<int>` header the constructor and `Add` edit (items pointer, size, version): their out footprint.
const LIST_HEADER_BYTES: usize = 16;

// ---- Shim ----

/// 0x71021f5ed0 `SkillExecter.calc_renkei_ratio` (replay row with a store journal and out footprint callees).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_skill_calc_renkei_ratio(x: *mut u64) {
    unsafe {
        let regs = Regs::from_raw(x);
        let (this, damage) = (regs.x(0), regs.x(1));
        let mut frame = Frame::new(regs);
        let _ = SkillExecter::at(this).calc_renkei_ratio(damage, &mut frame);
    }
}

/// Pre-state hook: the cells the port reads after the lifted body may have changed them, and the ones it stores to.
/// Returns 1 for the first run in a process (the lazy-initialization flag clear, the metadata slots unresolved).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_skill_calc_renkei_ratio_pre(x: *mut u64) -> i32 {
    unsafe {
        let flag = guest_ld8(slots::INIT_FLAG);
        prestate::snapshot(slots::INIT_FLAG, 1, true);
        if flag & 1 == 0 {
            return 1;
        }
        let regs = Regs::from_raw(x);
        let this = SkillExecter::at(regs.x(0));
        prestate::snapshot(regs.x(1), 4, true);
        if this.skill_obj().is_null() || this.skill_obj().skill_id() != skill::RENKEI_RANDOM_HITS {
            return 0;
        }
        let target = this.target();
        if target.is_null() {
            return 0;
        }
        let logger = target.result_logger();
        if logger.is_null() {
            return 0;
        }
        prestate::snapshot(logger.multi_number_address(), 8, true);
        prestate::snapshot(logger.number_state_address(), 4, true);
        let class = guest_ld64(guest_ld64(slots::INITIALIZED_CLASS));
        prestate::snapshot(class.wrapping_add(0xe0), 4, false);
        0
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn divide_by_four_rounds_toward_zero() {
        assert_eq!(divide_by_four(10).0, 2);
        assert_eq!(divide_by_four(-10).0, -2);
        assert_eq!(divide_by_four(-1).0, 0);
        assert_eq!(divide_by_four(i32::MIN).0, i32::MIN >> 2);
        assert_eq!(divide_by_four(i32::MAX).0, i32::MAX >> 2);
    }

    #[test]
    fn initialization_order_matches_the_return_addresses() {
        assert_eq!(slots::INIT_ORDER.len(), 13);
        assert_eq!(site::FIRST_INIT_RETURN + site::INIT_STEP * 12, 0x71021f5fa0);
    }

    #[test]
    fn nine_eighths_matches_the_compiled_bias() {
        for n in [
            0i32,
            1,
            7,
            8,
            9,
            -1,
            -7,
            -8,
            -9,
            1000,
            -1000,
            i32::MAX,
            i32::MIN,
        ] {
            let product = n.wrapping_add(n.wrapping_shl(3));
            let bias = product.wrapping_add(7);
            let got = (if product < 0 { bias } else { product }) >> 3;
            assert_eq!(got, product.wrapping_div(8), "n = {n}");
        }
    }
}
