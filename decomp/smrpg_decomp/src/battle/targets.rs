//! `Battle.SkillExecter.exec_each_targets` (0x71021fe920, task-62): the only direct caller of `damage_calc`, 42 instructions.
//!
//! In C# terms (names are the IL2CPP ones; the three flag fields have no name in the metadata dump, so they are named by
//! what they gate):
//!
//! ```text
//! void SkillExecter.exec_each_targets() {
//!     if (!is_hit_calc_succeed()) return;
//!     _tmp_value = 0;
//!     exec_calc_type();
//!     skill_calc_exception();
//!     exec_koudou_kouka();
//!     if (_do_damage_calc /* +0x44 */) damage_calc();
//!     if (_do_heal_calc /* +0x45 */) heal_calc();
//!     if (_do_physics_flag /* +0x46 */ && _target._result_logger /* +0xb8 */._is_physics /* +0x46 */) {
//!         _target.got_physics_attack(false);
//!         if (_target._character_id == 0x650) _target.<byte +0x74> = true;
//!     }
//!     resister_thank_you_message();   // a tail call
//! }
//! ```
//!
//! Every callee is a lifted function, so each goes through the bridge, including `damage_calc`: in `ab` mode the
//! recorder keeps its own comparison of the driver (swap.c, `run_nested_replay`) and hands this row the registers the
//! call left, in `on` mode the call reaches the Rust driver through its row. The function is a stack frame of 32 bytes
//! that saves x29, x30 and x19 and ends in `musttail return resister_thank_you_message`, which flushes x0, x8, x9, x19,
//! x29, x30 and sp (x30 is the saved entry value, unlike the early return, which leaves x30 alone).

use super::damage::SkillExecter;
use super::frame::Frame;
use super::{GuestException, GuestObject, Regs};
use crate::bridge::prestate;
use crate::{guest_ld8, guest_ld64};

const IS_HIT_CALC_SUCCEED: u64 = 0x71021fe9d0;
const EXEC_CALC_TYPE: u64 = 0x71021fee50;
const SKILL_CALC_EXCEPTION: u64 = 0x71021f8040;
const EXEC_KOUDOU_KOUKA: u64 = 0x71021f6340;
const DAMAGE_CALC: u64 = 0x71021ff580;
const HEAL_CALC: u64 = 0x71021fff20;
const GOT_PHYSICS_ATTACK: u64 = 0x71021bdef0;
const RESISTER_THANK_YOU_MESSAGE: u64 = 0x7102200060;

/// The return addresses the lifted code puts in x30 (the instruction after each `bl`).
mod site {
    pub const IS_HIT: u64 = 0x71021fe934;
    pub const EXEC_CALC_TYPE: u64 = 0x71021fe944;
    pub const SKILL_CALC_EXCEPTION: u64 = 0x71021fe94c;
    pub const EXEC_KOUDOU_KOUKA: u64 = 0x71021fe954;
    pub const DAMAGE_CALC: u64 = 0x71021fe964;
    pub const HEAL_CALC: u64 = 0x71021fe974;
    pub const GOT_PHYSICS_ATTACK: u64 = 0x71021fe994;
}

/// `_tmp_value`, zeroed before the calc type runs.
const TMP_VALUE: u64 = 0x40;
const DO_DAMAGE_CALC: u64 = 0x44;
const DO_HEAL_CALC: u64 = 0x45;
const DO_PHYSICS: u64 = 0x46;
const RESULT_LOGGER: u64 = 0xb8;
const LOGGER_PHYSICS: u64 = 0x46;
const CHARACTER_WITH_FLAG: u32 = 0x650;
const TARGET_FLAG: u64 = 0x74;

impl SkillExecter {
    /// `exec_each_targets()`. `f` is the register file at entry.
    pub fn exec_each_targets(self, f: &mut Frame) -> Result<(), GuestException> {
        let this = self.address();
        // prologue: `stp x29, x30, [sp, #-32]!`, `str x19, [sp, #0x10]`, `mov x29, sp`, `mov x19, x0`
        f.x[31] = f.entry[31].wrapping_sub(32);
        f.x[29] = f.x[31];
        f.x[19] = f.x[0];
        f.call(IS_HIT_CALC_SUCCEED, site::IS_HIT, &[19, 29], true)?;
        if f.x[0] & 1 == 0 {
            // the early return restores x19 and x29 and the sp; x30 is popped and not written back
            f.leave(&[19, 29]);
            return Ok(());
        }
        // `_tmp_value = 0` (a store the later callees overwrite: the A/B checks it at the next call)
        f.x[0] = f.x[19];
        prestate::store_u32(this.wrapping_add(TMP_VALUE), 0);
        f.call(EXEC_CALC_TYPE, site::EXEC_CALC_TYPE, &[0], false)?;
        f.x[0] = f.x[19];
        f.call(
            SKILL_CALC_EXCEPTION,
            site::SKILL_CALC_EXCEPTION,
            &[0],
            false,
        )?;
        f.x[0] = f.x[19];
        f.call(EXEC_KOUDOU_KOUKA, site::EXEC_KOUDOU_KOUKA, &[0], false)?;
        f.x[8] = u64::from(self.u8_field(DO_DAMAGE_CALC));
        if f.x[8] as u32 != 0 {
            f.x[0] = f.x[19];
            f.call(DAMAGE_CALC, site::DAMAGE_CALC, &[0, 8], false)?;
        }
        f.x[8] = u64::from(self.u8_field(DO_HEAL_CALC));
        if f.x[8] as u32 != 0 {
            f.x[0] = f.x[19];
            f.call(HEAL_CALC, site::HEAL_CALC, &[0, 8], false)?;
        }
        f.x[8] = u64::from(self.u8_field(DO_PHYSICS));
        if f.x[8] as u32 != 0 {
            let target = self.target();
            f.x[0] = target.address();
            f.x[8] = unsafe { guest_ld64(target.address().wrapping_add(RESULT_LOGGER)) };
            f.x[8] = u64::from(unsafe { guest_ld8(f.x[8].wrapping_add(LOGGER_PHYSICS)) });
            if f.x[8] as u32 != 0 {
                f.x[1] = 0;
                f.call(
                    GOT_PHYSICS_ATTACK,
                    site::GOT_PHYSICS_ATTACK,
                    &[0, 1, 8],
                    false,
                )?;
                let target = self.target();
                f.x[8] = target.address();
                f.x[9] = u64::from(target.character_id());
                if f.x[9] as u32 == CHARACTER_WITH_FLAG {
                    f.x[9] = 1;
                    prestate::store_u8(target.address().wrapping_add(TARGET_FLAG), 1);
                }
            }
        }
        // `resister_thank_you_message()` as a tail call: x0 = this, the saved registers come back from the frame
        f.x[0] = f.x[19];
        for n in [19, 29, 30, 31] {
            f.x[n] = f.entry[n];
        }
        f.flush(&[0, 8, 9, 19, 29, 30], true);
        // an exception stays pending in the register file, which is all the lifted tail call leaves too
        let _ = f.regs.invoke(RESISTER_THANK_YOU_MESSAGE);
        Ok(())
    }
}

/// 0x71021fe920 `SkillExecter.exec_each_targets` (replay row with a store journal; every callee goes through the bridge).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_skill_exec_each_targets(x: *mut u64) {
    unsafe {
        let regs = Regs::from_raw(x);
        let this = regs.x(0);
        let mut frame = Frame::new(regs);
        let _ = SkillExecter::at(this).exec_each_targets(&mut frame);
    }
}

/// Pre-state hook: the two cells the port stores to. The lifted callees rewrite them, so the A/B checks each store
/// against the cell's value at the next recorded call (`prestate::snapshot_timeline`). Never defers.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_skill_exec_each_targets_pre(x: *mut u64) -> i32 {
    unsafe {
        let regs = Regs::from_raw(x);
        let this = SkillExecter::at(regs.x(0));
        prestate::snapshot_timeline(this.address().wrapping_add(TMP_VALUE), 4);
        let target = this.target();
        if !target.is_null() {
            prestate::snapshot_timeline(target.address().wrapping_add(TARGET_FLAG), 1);
        }
        0
    }
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use crate::bridge::tests::{CALLS, Fake, STORES, clear, install};
    use std::boxed::Box;
    use std::vec::Vec;

    /// Fake graph: executer at word 0 (+0x28 target, +0x40..0x46 the flags), target at word 16 (+0x20 id, +0xb8 logger),
    /// logger at word 48 (+0x46 flag).
    struct Graph(Box<[u64; 64]>);

    impl Graph {
        fn new(flags: [u8; 3], logger_flag: u8, id: u32) -> Self {
            let mut words = Box::new([0u64; 64]);
            let base = words.as_ptr() as u64;
            words[5] = base + 8 * 16;
            words[16 + 23] = base + 8 * 48;
            let bytes = words.as_mut_ptr().cast::<u8>();
            unsafe {
                for (k, flag) in flags.iter().enumerate() {
                    bytes.add(0x44 + k).write(*flag);
                }
                bytes.add(8 * 48 + 0x46).write(logger_flag);
                bytes.add(8 * 16 + 0x20).cast::<u32>().write_unaligned(id);
            }
            Self(words)
        }
    }

    fn install_all(hit: u64) {
        clear();
        for target in [
            IS_HIT_CALC_SUCCEED,
            EXEC_CALC_TYPE,
            SKILL_CALC_EXCEPTION,
            EXEC_KOUDOU_KOUKA,
            DAMAGE_CALC,
            HEAL_CALC,
            GOT_PHYSICS_ATTACK,
            RESISTER_THANK_YOU_MESSAGE,
        ] {
            let body: Fake = if target != IS_HIT_CALC_SUCCEED {
                |r| r[0] = 0x77
            } else if hit == 1 {
                |r| r[0] = 1
            } else {
                |r| r[0] = 2
            };
            install(target, body);
        }
    }

    fn run(graph: &Graph, hit: u64) -> ([u64; 96], Vec<u64>) {
        install_all(hit);
        let mut file = [0u64; 96];
        file[0] = graph.0.as_ptr() as u64;
        file[31] = 0x1000;
        file[30] = 0xabc;
        file[19] = 0x19;
        file[29] = 0x29;
        let regs = unsafe { Regs::from_raw(file.as_mut_ptr()) };
        let mut frame = Frame::new(regs);
        let _ = unsafe { SkillExecter::at(file[0]) }.exec_each_targets(&mut frame);
        (file, CALLS.with(|c| c.borrow().clone()))
    }

    #[test]
    fn a_miss_returns_after_one_call() {
        let graph = Graph::new([1, 1, 1], 1, 0x650);
        let (file, calls) = run(&graph, 2);
        assert_eq!(calls, [IS_HIT_CALC_SUCCEED]);
        assert_eq!(
            (file[19], file[29], file[31], file[30]),
            (0x19, 0x29, 0x1000, site::IS_HIT)
        );
        assert!(STORES.with(|s| s.borrow().is_empty()));
    }

    #[test]
    fn all_flags_call_every_callee_in_order_and_store_the_target_flag() {
        let graph = Graph::new([1, 2, 0xff], 1, 0x650);
        let (file, calls) = run(&graph, 1);
        assert_eq!(
            calls,
            [
                IS_HIT_CALC_SUCCEED,
                EXEC_CALC_TYPE,
                SKILL_CALC_EXCEPTION,
                EXEC_KOUDOU_KOUKA,
                DAMAGE_CALC,
                HEAL_CALC,
                GOT_PHYSICS_ATTACK,
                RESISTER_THANK_YOU_MESSAGE
            ]
        );
        let base = graph.0.as_ptr() as u64;
        let stores = STORES.with(|s| s.borrow().clone());
        assert_eq!(stores, [(base + 0x40, 4, 0), (base + 8 * 16 + 0x74, 1, 1)]);
        // the tail call's registers: x0 = this was overwritten by the fake, x30 = the entry value, x9 = 1
        assert_eq!(
            (file[30], file[31], file[19], file[29]),
            (0xabc, 0x1000, 0x19, 0x29)
        );
        assert_eq!(file[9], 1);
    }

    #[test]
    fn other_characters_and_clear_flags_skip_their_calls() {
        let graph = Graph::new([0, 0, 1], 1, 0x64f);
        let (file, calls) = run(&graph, 1);
        assert_eq!(
            calls,
            [
                IS_HIT_CALC_SUCCEED,
                EXEC_CALC_TYPE,
                SKILL_CALC_EXCEPTION,
                EXEC_KOUDOU_KOUKA,
                GOT_PHYSICS_ATTACK,
                RESISTER_THANK_YOU_MESSAGE
            ]
        );
        assert_eq!(file[9], 0x64f);
        let base = graph.0.as_ptr() as u64;
        assert_eq!(STORES.with(|s| s.borrow().clone()), [(base + 0x40, 4, 0)]);
        let graph = Graph::new([0, 0, 1], 0, 0x650);
        let (_, calls) = run(&graph, 1);
        assert_eq!(calls.len(), 5);
    }
}
