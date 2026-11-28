//! Small lifted callees of `Battle.SkillExecter.exec_koudou_kouka` (0x71021f6340), ported in task-67.
//!
//! `exec_koudou_kouka` is 4,193 lifted lines. Its callees on a `BattleCharacterParam` are tiny; most go through the
//! character's `BuffManager` (`param +0xa0`), whose `+0x28` is `_invincible_turn` (task-55). In C# terms (names are
//! IL2CPP's where the dump has them; the rest are named by what they hold):
//!
//! ```text
//! void BattleCharacterParam.<be0f0>()  { _buff_mng._invincible_turn = 0; }                       // 0x71021be0f0
//! void BattleCharacterParam.<ba6d0>()  { if (_buff_mng._invincible_turn <= 0) _buff_mng.<+0x11> = 1; }
//! void BattleCharacterParam.<ba6a0>()  { if (_buff_mng._invincible_turn <= 0 && _badstatus_mng._status != 5)
//!                                            _buff_mng.<+0x10> = 1; }                            // with a collection safepoint
//! void BattleCharacterParam.<be1f0>(int i, int v) { _buff_mng._values[i] += v; }                 // bounds-checked
//! ```
//!
//! and five functions that load their arguments and tail-call another lifted function: 0x71021bd6c0 (returns 0 while
//! `_invincible_turn > 0`, otherwise calls 0x7101e30240 on `_badstatus_mng`), 0x71021be2f0 (`_buff_mng`), 0x71021c3c00
//! (`+0x30`, with `x2 = w1` and `x1 = [x0 + 0x10]`), 0x71021c68c0 (`x1 = [x1 + 0x10]`, `x2 = [x2 + 0x10]`) and
//! 0x71021bf040 (a call, a u16 copy inside the buff manager, then a tail call).
//!
//! Every row keeps the lifted register residue: a lifted function writes `c->x[n]` only at a call, a safepoint and the
//! return, and only the registers that changed. The rows that return plainly write nothing.

use super::frame::Frame;
use super::{BattleCharacterParam, BuffManager, GuestException, GuestObject, Regs};
use crate::bridge::prestate;
use crate::{guest_ld16, guest_ld32, guest_ld64};

const BADSTATUS_MNG: u64 = 0x98;
const BUFF_MNG: u64 = 0xa0;
/// `BuffManager` cells the small functions set.
const FLAG_10: u64 = 0x10;
const FLAG_11: u64 = 0x11;
const HALF_12: u64 = 0x12;
const VALUES: u64 = 0x18;
/// `BadStatusManager._status`.
const STATUS: u64 = 0x10;
/// `int[]`: length and the first element.
const ARRAY_LENGTH: u64 = 0x18;
const ARRAY_FIRST: u64 = 0x20;
/// The `_status` the second flag function skips.
const STATUS_SKIPPED: u32 = 5;

/// Lifted functions the rows call.
pub mod callee {
    pub const INDEX_OUT_OF_RANGE: u64 = 0x71000c4510;
    pub const BE300: u64 = 0x71021be300;
    pub const C060B0: u64 = 0x7101c060b0;
    pub const C68D0: u64 = 0x71021c68d0;
    pub const C06250: u64 = 0x7101c06250;
    pub const E22A40: u64 = 0x7101e22a40;
    pub const E30240: u64 = 0x7101e30240;
}

mod site {
    pub const ADD_BUFF_RANGE: u64 = 0x71021be228;
    pub const GC_POLL_A6A0: u64 = 0x71021ba6c0;
    pub const BF040_CALL: u64 = 0x71021bf05c;
}

fn buff_of(param: u64) -> u64 {
    unsafe { guest_ld64(param.wrapping_add(BUFF_MNG)) }
}

fn invincible_turn(buff: u64) -> i32 {
    unsafe { guest_ld32(buff.wrapping_add(BuffManager::INVINCIBLE_TURN)) as i32 }
}

impl BattleCharacterParam {
    /// 0x71021be0f0: `_buff_mng._invincible_turn = 0`.
    pub fn clear_invincible_turn(self) {
        let buff = buff_of(self.address());
        prestate::store_u32(buff.wrapping_add(BuffManager::INVINCIBLE_TURN), 0);
    }

    /// 0x71021ba6d0: with no invincible turn left, the buff manager's byte +0x11 is set.
    pub fn set_flag_11_when_not_invincible(self) {
        let buff = buff_of(self.address());
        if invincible_turn(buff) <= 0 {
            prestate::store_u8(buff.wrapping_add(FLAG_11), 1);
        }
    }

    /// 0x71021ba6a0: with no invincible turn left and `_status != 5`, the byte +0x10 is set. The lifted code has a
    /// collection safepoint after the status compare; it flushes x8 (the buff manager) and x9 (the status).
    pub fn set_flag_10_when_not_invincible(self, regs: &mut Regs) {
        let buff = buff_of(self.address());
        if invincible_turn(buff) > 0 {
            return;
        }
        let status_mng = unsafe { guest_ld64(self.address().wrapping_add(BADSTATUS_MNG)) };
        let status = unsafe { guest_ld32(status_mng.wrapping_add(STATUS)) };
        regs.safepoint_n(site::GC_POLL_A6A0, &[(8, buff), (9, u64::from(status))]);
        if status != STATUS_SKIPPED {
            prestate::store_u8(buff.wrapping_add(FLAG_10), 1);
        }
    }

    /// 0x71021be1f0: `_buff_mng._values[index] += delta`; an index at or past the length raises (`f.x[1]` is the index,
    /// `f.x[2]` the delta).
    pub fn add_buff_value(self, f: &mut Frame) -> Result<(), GuestException> {
        // prologue: `stp x29, x30, [sp, #-16]!`, `mov x29, sp`
        f.x[31] = f.entry[31].wrapping_sub(16);
        f.x[29] = f.x[31];
        f.x[8] = buff_of(self.address());
        f.x[8] = unsafe { guest_ld64(f.x[8].wrapping_add(VALUES)) };
        f.x[9] = u64::from(unsafe { guest_ld32(f.x[8].wrapping_add(ARRAY_LENGTH)) });
        if f.x[9] as u32 <= f.x[1] as u32 {
            return f.call(
                callee::INDEX_OUT_OF_RANGE,
                site::ADD_BUFF_RANGE,
                &[8, 9, 29],
                true,
            );
        }
        // `add x8, x8, w1, sxtw #2`
        f.x[8] = f.x[8].wrapping_add(((f.x[1] as u32 as i32 as i64) << 2) as u64);
        let cell = f.x[8].wrapping_add(ARRAY_FIRST);
        f.x[9] = u64::from(prestate::read_u32(cell));
        f.x[9] = u64::from((f.x[9] as u32).wrapping_add(f.x[2] as u32));
        prestate::store_u32(cell, f.x[9] as u32);
        f.leave(&[29]);
        Ok(())
    }

    /// 0x71021bd6c0: 0 while `_invincible_turn > 0`, otherwise a tail call to 0x7101e30240 with `x0 = _badstatus_mng` and
    /// `x2 = 0` (x8 holds the turn count, flushed there).
    pub fn bd6c0(self, regs: &mut Regs) {
        let buff = buff_of(self.address());
        let turn = unsafe { guest_ld32(buff.wrapping_add(BuffManager::INVINCIBLE_TURN)) };
        if (turn as i32) > 0 {
            regs.set_x(0, 0);
            return;
        }
        let status_mng = unsafe { guest_ld64(self.address().wrapping_add(BADSTATUS_MNG)) };
        regs.set_x(0, status_mng);
        regs.set_x(2, 0);
        regs.set_x(8, u64::from(turn));
        let _ = regs.invoke(callee::E30240);
    }

    /// 0x71021bf040: `0x7101c06250(this[+0x28], 0)`, then the buff manager's u16 at +0x12 is copied to +0x10, then a tail
    /// call to 0x7101e22a40 with `x0 = _badstatus_mng`.
    pub fn bf040(self, f: &mut Frame) -> Result<(), GuestException> {
        // prologue: `stp x29, x30, [sp, #-32]!`, `str x19, [sp, #0x10]`, `mov x29, sp`, `mov x19, x0`
        f.x[31] = f.entry[31].wrapping_sub(32);
        f.x[29] = f.x[31];
        f.x[19] = f.x[0];
        f.x[0] = unsafe { guest_ld64(f.x[0].wrapping_add(0x28)) };
        f.x[1] = 0;
        f.call(callee::C06250, site::BF040_CALL, &[0, 1, 19, 29], true)?;
        f.x[8] = buff_of(f.x[19]);
        f.x[1] = 0;
        f.x[9] = u64::from(unsafe { guest_ld16(f.x[8].wrapping_add(HALF_12)) });
        prestate::store_u16(f.x[8].wrapping_add(FLAG_10), f.x[9] as u16);
        f.x[0] = unsafe { guest_ld64(f.x[19].wrapping_add(BADSTATUS_MNG)) };
        for n in [19, 29, 30, 31] {
            f.x[n] = f.entry[n];
        }
        f.flush(&[0, 1, 8, 9, 19, 29, 30], true);
        // an exception stays pending in the register file, which is all the lifted tail call leaves too
        let _ = f.regs.invoke(callee::E22A40);
        Ok(())
    }
}

/// 0x71021be2f0: `x0 = _buff_mng`, tail call to 0x71021be300.
pub fn buff_tail(regs: &mut Regs) {
    let buff = buff_of(regs.x(0));
    regs.set_x(0, buff);
    let _ = regs.invoke(callee::BE300);
}

/// 0x71021c3c00: `x0 = [x0 + 0x30]`, `x2 = w1`, `x3 = 0`, `x1 = w[x0 + 0x10]` (the new x0), tail call to 0x7101c060b0.
pub fn field_30_tail(regs: &mut Regs) {
    let object = unsafe { guest_ld64(regs.x(0).wrapping_add(0x30)) };
    let x2 = u64::from(regs.x(1) as u32);
    let x1 = u64::from(unsafe { guest_ld32(object.wrapping_add(0x10)) });
    regs.set_x(0, object);
    regs.set_x(1, x1);
    regs.set_x(2, x2);
    regs.set_x(3, 0);
    let _ = regs.invoke(callee::C060B0);
}

/// 0x71021c68c0: `x1 = [x1 + 0x10]`, `x2 = [x2 + 0x10]`, tail call to 0x71021c68d0.
pub fn pair_tail(regs: &mut Regs) {
    let x1 = unsafe { guest_ld64(regs.x(1).wrapping_add(0x10)) };
    let x2 = unsafe { guest_ld64(regs.x(2).wrapping_add(0x10)) };
    regs.set_x(1, x1);
    regs.set_x(2, x2);
    let _ = regs.invoke(callee::C68D0);
}

/// Where `add_buff_value` stores, or `None` when it raises.
fn add_cell(param: u64, index: u64) -> Option<u64> {
    let values = unsafe { guest_ld64(buff_of(param).wrapping_add(VALUES)) };
    let length = unsafe { guest_ld32(values.wrapping_add(ARRAY_LENGTH)) };
    if length <= index as u32 {
        return None;
    }
    Some(
        values
            .wrapping_add(((index as u32 as i32 as i64) << 2) as u64)
            .wrapping_add(ARRAY_FIRST),
    )
}

/// 0x71021be0f0.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_param_clear_invincible_turn(x: *mut u64) {
    unsafe { BattleCharacterParam::at(Regs::from_raw(x).x(0)) }.clear_invincible_turn();
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_param_clear_invincible_turn_pre(x: *mut u64) -> i32 {
    unsafe {
        let buff = buff_of(Regs::from_raw(x).x(0));
        prestate::snapshot(buff.wrapping_add(BuffManager::INVINCIBLE_TURN), 4, true);
    }
    0
}

/// 0x71021ba6d0.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_param_flag_11(x: *mut u64) {
    unsafe { BattleCharacterParam::at(Regs::from_raw(x).x(0)) }.set_flag_11_when_not_invincible();
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_param_flag_11_pre(x: *mut u64) -> i32 {
    unsafe {
        let buff = buff_of(Regs::from_raw(x).x(0));
        prestate::snapshot(buff.wrapping_add(FLAG_11), 1, true);
    }
    0
}

/// 0x71021ba6a0.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_param_flag_10(x: *mut u64) {
    unsafe {
        let mut regs = Regs::from_raw(x);
        BattleCharacterParam::at(regs.x(0)).set_flag_10_when_not_invincible(&mut regs);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_param_flag_10_pre(x: *mut u64) -> i32 {
    unsafe {
        let buff = buff_of(Regs::from_raw(x).x(0));
        prestate::snapshot(buff.wrapping_add(FLAG_10), 1, true);
    }
    0
}

/// 0x71021be1f0.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_param_add_buff_value(x: *mut u64) {
    unsafe {
        let regs = Regs::from_raw(x);
        let param = regs.x(0);
        let mut frame = Frame::new(regs);
        let _ = BattleCharacterParam::at(param).add_buff_value(&mut frame);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_param_add_buff_value_pre(x: *mut u64) -> i32 {
    unsafe {
        let regs = Regs::from_raw(x);
        if let Some(cell) = add_cell(regs.x(0), regs.x(1)) {
            prestate::snapshot(cell, 4, true);
        }
    }
    0
}

/// 0x71021bd6c0.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_param_bd6c0(x: *mut u64) {
    unsafe {
        let mut regs = Regs::from_raw(x);
        BattleCharacterParam::at(regs.x(0)).bd6c0(&mut regs);
    }
}

/// 0x71021be2f0.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_param_be2f0(x: *mut u64) {
    unsafe { buff_tail(&mut Regs::from_raw(x)) }
}

/// 0x71021c3c00.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_obj_c3c00(x: *mut u64) {
    unsafe { field_30_tail(&mut Regs::from_raw(x)) }
}

/// 0x71021c68c0.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_obj_c68c0(x: *mut u64) {
    unsafe { pair_tail(&mut Regs::from_raw(x)) }
}

/// 0x71021bf040.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_param_bf040(x: *mut u64) {
    unsafe {
        let regs = Regs::from_raw(x);
        let param = regs.x(0);
        let mut frame = Frame::new(regs);
        let _ = BattleCharacterParam::at(param).bf040(&mut frame);
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_param_bf040_pre(x: *mut u64) -> i32 {
    unsafe {
        let buff = buff_of(Regs::from_raw(x).x(0));
        prestate::snapshot(buff.wrapping_add(FLAG_10), 2, true);
    }
    0
}

#[cfg(test)]
mod tests;
