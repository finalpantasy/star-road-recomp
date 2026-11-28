//! Three callees of `Battle.SkillExecter.exec_each_targets` (task-63): `BattleCharacterParam.got_physics_attack`
//! (0x71021bdef0), `SkillExecter.resister_thank_you_message` (0x7102200060) and `SkillExecter.heal_calc` (0x71021fff20).
//!
//! In C# terms (names are the IL2CPP ones; the cells and slots the metadata dump does not name are named by what
//! they hold):
//!
//! ```text
//! void BattleCharacterParam.got_physics_attack(bool) {
//!     if (_badstatus_mng._status != 3) return;
//!     _result_logger.<set +0x60>(1, null);             // a tiny setter: `if (value != 0) this[+0x60] = value`
//!     <string literal>(<slot>, false);                  // lazy string literal lookup
//!     <debug enter>(...);
//!     Debug.write(<debug monitor>, [slot], null);       // a tail call into the empty debug writer
//! }
//!
//! void SkillExecter.resister_thank_you_message() {
//!     if (_skill_obj._skill_type != 3) return;           // +0x30 -> +0x3c
//!     if (!<acter/target check>(_acter, _target)) return;
//!     BattleManager.<class init, static field metadata>();
//!     BattleManager._instance.<+0x48 object>.<+0x38 int> |= 2;
//! }
//!
//! void SkillExecter.heal_calc() {
//!     _monitor.<debug enter>(null);
//!     int heal = _tmp_value;
//!     heal = Math.Max(heal, 1);
//!     set_acction_command_ratio();
//!     if (_skill_obj._skill_id >= 1000) { _monitor.write(<literal>); heal = 0; }
//!     _target._result_logger._heal = heal;               // +0x28
//!     _target._result_logger._is_heal = 1;               // +0x2c
//!     _monitor.write(<literal> + heal.ToString());
//! }
//! ```
//!
//! Every function keeps the lifted register residue (see `Frame`). The three first-call arms (lazy metadata
//! initialization) are ported too: `got_physics_attack` is compared in `ab` mode including that arm (its pre-state
//! hook snapshots the two slots it passes to the initializer), the other two defer the first run of a process like
//! their siblings (the hook returns 1 while the flag byte is clear: the class cell it reads is still a token).

use super::damage::smrpg_skill_set_acction_command_ratio_pre;
use super::damage::{SkillExecter, smrpg_skill_set_acction_command_ratio};
use super::frame::Frame;
use super::{BattleCharacterParam, GuestException, GuestObject, Regs};
use crate::bridge::prestate;
use crate::{guest_ld8, guest_ld32, guest_ld64};

const FLAG_PAGE: u64 = 0x7103cba000;

/// Lifted functions the three bodies call.
mod callee {
    pub const INITIALIZE_METADATA: u64 = 0x71000c4150;
    pub const CLASS_INITIALIZER: u64 = 0x71000c4330;
    pub const DEBUG_ENTER: u64 = 0x71018bebf0;
    pub const DEBUG_WRITE: u64 = 0x71018bebc0;
    pub const STRING_LITERAL: u64 = 0x71018bfa40;
    pub const LOGGER_SET_60: u64 = 0x71021f7690;
    pub const ACTER_TARGET_CHECK: u64 = 0x71021c3a20;
    pub const MATH_MAX: u64 = 0x710213e670;
    pub const INT32_TO_STRING: u64 = 0x710213a840;
    pub const STRING_CONCAT_2: u64 = 0x71020147d0;
}

/// Where the guest sees values after a collection safepoint or a lazy initialization: reads that must give the value at
/// the call (a lazy-initialization flag, a class initialized word) go through `prestate`; a read of a cell the
/// initializer just resolved is live. Unit tests have no guest memory at the global addresses, so both read the fake.
fn live_u64(address: u64) -> u64 {
    if cfg!(test) {
        prestate::read_u64(address)
    } else {
        unsafe { guest_ld64(address) }
    }
}

fn live_u32(address: u64) -> u32 {
    if cfg!(test) {
        prestate::read_u32(address)
    } else {
        unsafe { guest_ld32(address) }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// got_physics_attack
// ---------------------------------------------------------------------------------------------------------------

mod physics {
    pub const FLAG: u64 = super::FLAG_PAGE + 0xbef;
    pub const SLOT_A: u64 = 0x7103a82000 + 0x900;
    pub const SLOT_B: u64 = 0x7103a68000 + 0x978;
    pub const BADSTATUS_MNG: u64 = 0x98;
    pub const STATUS: u64 = 0x10;
    pub const RESULT_LOGGER: u64 = 0xb8;
    /// `_status` value the method acts on.
    pub const STATUS_VALUE: u32 = 3;
    pub const SP_DROP: u64 = 48;
    pub mod site {
        pub const INIT_A: u64 = 0x71021bdf1c;
        pub const INIT_B: u64 = 0x71021bdf28;
        pub const LOGGER_SET: u64 = 0x71021bdf60;
        pub const LITERAL: u64 = 0x71021bdf6c;
        pub const DEBUG_ENTER: u64 = 0x71021bdf78;
    }
}

impl BattleCharacterParam {
    /// `got_physics_attack(bool)`. `f` is the register file at entry (the bool is never read).
    pub fn got_physics_attack(self, f: &mut Frame) -> Result<(), GuestException> {
        use physics::*;
        // prologue: `stp x29, x30, [sp, #-48]!`, saves x19 to x21, `mov x29, sp`, then the lazy-initialization flag
        f.x[31] = f.entry[31].wrapping_sub(SP_DROP);
        f.x[29] = f.x[31];
        f.x[20] = FLAG_PAGE;
        f.x[8] = u64::from(prestate::read_u8(FLAG));
        f.x[19] = f.x[0];
        if f.x[8] & 1 == 0 {
            f.x[0] = prestate::read_u64(SLOT_A);
            f.call(
                callee::INITIALIZE_METADATA,
                site::INIT_A,
                &[0, 8, 19, 20, 29],
                true,
            )?;
            f.x[0] = prestate::read_u64(SLOT_B);
            f.call(callee::INITIALIZE_METADATA, site::INIT_B, &[0], false)?;
            f.x[8] = 1;
            prestate::store_u8(FLAG, 1);
        }
        // `if (_badstatus_mng._status != 3) return;`
        f.x[8] = prestate::read_u64(f.x[19].wrapping_add(BADSTATUS_MNG));
        f.x[8] = u64::from(prestate::read_u32(f.x[8].wrapping_add(STATUS)));
        if f.x[8] as u32 != STATUS_VALUE {
            f.leave(&[19, 20, 21, 29]);
            return Ok(());
        }
        f.x[20] = live_u64(SLOT_B);
        f.x[21] = live_u64(SLOT_A);
        f.x[1] = 1;
        f.x[2] = 0;
        f.x[0] = prestate::read_u64(f.x[19].wrapping_add(RESULT_LOGGER));
        f.call(
            callee::LOGGER_SET_60,
            site::LOGGER_SET,
            &[0, 1, 2, 8, 19, 20, 21, 29],
            true,
        )?;
        f.x[0] = prestate::read_u64(f.x[20]);
        f.x[1] = 0;
        f.call(callee::STRING_LITERAL, site::LITERAL, &[0, 1], false)?;
        f.x[1] = 0;
        f.x[19] = f.x[0];
        f.call(callee::DEBUG_ENTER, site::DEBUG_ENTER, &[1, 19], false)?;
        f.x[1] = prestate::read_u64(f.x[21]);
        f.x[0] = f.x[19];
        f.x[2] = 0;
        // the epilogue (x19 to x21, x29, x30 and sp come back from the frame), then a tail call
        for n in [19, 20, 21, 29, 30, 31] {
            f.x[n] = f.entry[n];
        }
        f.flush(&[0, 1, 2, 19, 20, 21, 29, 30], true);
        // an exception stays pending in the register file, which is all the lifted tail call leaves too
        let _ = f.regs.invoke(callee::DEBUG_WRITE);
        Ok(())
    }
}

/// 0x71021bdef0 `BattleCharacterParam.got_physics_attack` (replay row with a store journal).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_param_got_physics_attack(x: *mut u64) {
    unsafe {
        let regs = Regs::from_raw(x);
        let this = regs.x(0);
        let mut frame = Frame::new(regs);
        let _ = BattleCharacterParam::at(this).got_physics_attack(&mut frame);
    }
}

/// Pre-state hook: the flag byte (the port stores it on the first run), the two slots the first run passes to the
/// initializer, and the cells the port reads after the callees ran. Never defers.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_param_got_physics_attack_pre(x: *mut u64) -> i32 {
    unsafe {
        let regs = Regs::from_raw(x);
        let this = regs.x(0);
        let flag = guest_ld8(physics::FLAG);
        prestate::snapshot(physics::FLAG, 1, true);
        if this != 0 {
            let manager_cell = this.wrapping_add(physics::BADSTATUS_MNG);
            prestate::snapshot(manager_cell, 8, false);
            let manager = guest_ld64(manager_cell);
            if manager != 0 {
                prestate::snapshot(manager.wrapping_add(physics::STATUS), 4, false);
            }
            prestate::snapshot(this.wrapping_add(physics::RESULT_LOGGER), 8, false);
        }
        if flag & 1 == 0 {
            // the slots are still tokens: only the values the initializer receives are meaningful
            prestate::snapshot(physics::SLOT_A, 8, false);
            prestate::snapshot(physics::SLOT_B, 8, false);
            return 0;
        }
        let literal_cell = guest_ld64(physics::SLOT_B);
        let write_cell = guest_ld64(physics::SLOT_A);
        prestate::snapshot(literal_cell, 8, false);
        prestate::snapshot(write_cell, 8, false);
        0
    }
}

// ---------------------------------------------------------------------------------------------------------------
// resister_thank_you_message
// ---------------------------------------------------------------------------------------------------------------

mod thanks {
    pub const FLAG: u64 = super::FLAG_PAGE + 0xd6c;
    /// The `BattleManager` class cell (the slot holds a pointer to the cell that holds the class).
    pub const CLASS_SLOT: u64 = 0x7103a57000 + 0xe48;
    /// Pointer to the byte that says the static-field metadata of this method has been initialized.
    pub const STATIC_FLAG_POINTER: u64 = 0x7103a58000 + 0x1d0;
    pub const SKILL_OBJ: u64 = 0x30;
    pub const ACTER: u64 = 0x20;
    pub const TARGET: u64 = 0x28;
    pub const SKILL_TYPE: u64 = 0x3c;
    pub const SKILL_TYPE_VALUE: u32 = 3;
    pub const CLASS_INITIALIZED: u64 = 0xe0;
    pub const CLASS_STATIC_FIELDS: u64 = 0xb8;
    pub const STATICS_INSTANCE: u64 = 0x10;
    pub const INSTANCE_OBJECT: u64 = 0x48;
    pub const OBJECT_BITS: u64 = 0x38;
    pub const BIT: u32 = 2;
    pub mod site {
        pub const INIT_FIRST: u64 = 0x7102200088;
        pub const ACTER_TARGET_CHECK: u64 = 0x71022000ac;
        pub const CLASS_INIT_FIRST: u64 = 0x7102200108;
        pub const CLASS_INIT_FIRST_POLL: u64 = 0x7102200114;
        pub const METADATA: u64 = 0x7102200124;
        pub const METADATA_POLL: u64 = 0x7102200134;
        pub const CLASS_INIT_AGAIN: u64 = 0x710220013c;
        pub const CLASS_INIT_AGAIN_POLL: u64 = 0x7102200140;
    }
}

impl SkillExecter {
    /// `resister_thank_you_message()`. `f` is the register file at entry.
    pub fn resister_thank_you_message(self, f: &mut Frame) -> Result<(), GuestException> {
        use thanks::*;
        // prologue: `stp x29, x30, [sp, #-32]!`, `stp x20, x19, [sp, #0x10]`, `mov x29, sp`
        f.x[31] = f.entry[31].wrapping_sub(32);
        f.x[29] = f.x[31];
        f.x[20] = FLAG_PAGE;
        f.x[8] = u64::from(prestate::read_u8(FLAG));
        f.x[19] = f.x[0];
        if f.x[8] & 1 == 0 {
            f.x[0] = prestate::read_u64(CLASS_SLOT);
            f.call(
                callee::INITIALIZE_METADATA,
                site::INIT_FIRST,
                &[0, 8, 19, 20, 29],
                true,
            )?;
            f.x[8] = 1;
            prestate::store_u8(FLAG, 1);
        }
        // `if (_skill_obj._skill_type != 3) return;`
        f.x[8] = unsafe { guest_ld64(f.x[19].wrapping_add(SKILL_OBJ)) };
        f.x[8] = u64::from(unsafe { guest_ld32(f.x[8].wrapping_add(SKILL_TYPE)) });
        if f.x[8] as u32 != SKILL_TYPE_VALUE {
            return self.leave_thanks(f);
        }
        // `if (!check(_acter, _target)) return;`
        f.x[0] = unsafe { guest_ld64(f.x[19].wrapping_add(ACTER)) };
        f.x[1] = unsafe { guest_ld64(f.x[19].wrapping_add(TARGET)) };
        f.x[2] = 0;
        f.call(
            callee::ACTER_TARGET_CHECK,
            site::ACTER_TARGET_CHECK,
            &[0, 1, 2, 8, 19, 20, 29],
            true,
        )?;
        if f.x[0] & 1 == 0 {
            return self.leave_thanks(f);
        }
        // `BattleManager` class cell, its initialized word and the static-field flag byte, with their lazy initialization
        f.x[19] = live_u64(CLASS_SLOT);
        f.x[0] = live_u64(f.x[19]);
        f.x[8] = u64::from(prestate::read_u32(f.x[0].wrapping_add(CLASS_INITIALIZED)));
        let flag_clear;
        if f.x[8] as u32 == 0 {
            f.call(
                callee::CLASS_INITIALIZER,
                site::CLASS_INIT_FIRST,
                &[0, 8, 19],
                false,
            )?;
            f.x[20] = live_u64(STATIC_FLAG_POINTER);
            f.x[8] = u64::from(prestate::read_u8(f.x[20]));
            f.regs
                .safepoint_n(site::CLASS_INIT_FIRST_POLL, &[(8, f.x[8]), (20, f.x[20])]);
            flag_clear = f.x[8] as u32 == 0;
        } else {
            f.x[20] = live_u64(STATIC_FLAG_POINTER);
            f.x[8] = u64::from(prestate::read_u8(f.x[20]));
            flag_clear = f.x[8] as u32 == 0;
        }
        let mut class_ready;
        if flag_clear {
            // the static-field metadata of this method is not resolved yet
            f.x[0] = prestate::read_u64(CLASS_SLOT);
            f.call(
                callee::INITIALIZE_METADATA,
                site::METADATA,
                &[0, 8, 19, 20],
                false,
            )?;
            f.x[8] = 1;
            prestate::store_u8(f.x[20], 1);
            f.x[0] = live_u64(f.x[19]);
            f.x[8] = u64::from(live_u32(f.x[0].wrapping_add(CLASS_INITIALIZED)));
            f.regs
                .safepoint_n(site::METADATA_POLL, &[(0, f.x[0]), (8, f.x[8])]);
            class_ready = f.x[8] as u32 != 0;
        } else {
            f.x[0] = live_u64(f.x[19]);
            f.x[8] = u64::from(live_u32(f.x[0].wrapping_add(CLASS_INITIALIZED)));
            class_ready = f.x[8] as u32 != 0;
        }
        if !class_ready {
            f.call(
                callee::CLASS_INITIALIZER,
                site::CLASS_INIT_AGAIN,
                &[0, 8, 19, 20],
                false,
            )?;
            f.x[0] = live_u64(f.x[19]);
            f.regs
                .safepoint_n(site::CLASS_INIT_AGAIN_POLL, &[(0, f.x[0])]);
            class_ready = true;
        }
        debug_assert!(class_ready);
        // `BattleManager._instance.<+0x48>.<+0x38> |= 2`
        f.x[8] = unsafe { guest_ld64(f.x[0].wrapping_add(CLASS_STATIC_FIELDS)) };
        f.x[8] = unsafe { guest_ld64(f.x[8].wrapping_add(STATICS_INSTANCE)) };
        f.x[8] = unsafe { guest_ld64(f.x[8].wrapping_add(INSTANCE_OBJECT)) };
        let bits_cell = f.x[8].wrapping_add(OBJECT_BITS);
        f.x[9] = u64::from(prestate::read_u32(bits_cell) | BIT);
        prestate::store_u32(bits_cell, f.x[9] as u32);
        self.leave_thanks(f)
    }

    /// The epilogue shared by every exit: x19, x20 and x29 and the sp come back; x0 is flushed, x30 is not.
    fn leave_thanks(self, f: &mut Frame) -> Result<(), GuestException> {
        f.leave(&[0, 19, 20, 29]);
        Ok(())
    }
}

/// 0x7102200060 `SkillExecter.resister_thank_you_message` (replay row with a store journal).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_skill_resister_thank_you_message(x: *mut u64) {
    unsafe {
        let regs = Regs::from_raw(x);
        let this = regs.x(0);
        let mut frame = Frame::new(regs);
        let _ = SkillExecter::at(this).resister_thank_you_message(&mut frame);
    }
}

/// Pre-state hook. Returns 1 for the first run in a process (the flag byte clear: the class cell is still a token).
/// Otherwise the cells the port reads after the callees may have changed, and the two it stores to.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_skill_resister_thank_you_message_pre(x: *mut u64) -> i32 {
    unsafe {
        let flag = guest_ld8(thanks::FLAG);
        prestate::snapshot(thanks::FLAG, 1, true);
        if flag & 1 == 0 {
            return 1;
        }
        let regs = Regs::from_raw(x);
        let this = regs.x(0);
        let class_cell = guest_ld64(thanks::CLASS_SLOT);
        let class = guest_ld64(class_cell);
        let word = class.wrapping_add(thanks::CLASS_INITIALIZED);
        prestate::snapshot(word, 4, false);
        let flag_cell = guest_ld64(thanks::STATIC_FLAG_POINTER);
        prestate::snapshot(flag_cell, 1, true);
        if guest_ld32(word) != 0 {
            let statics = guest_ld64(class.wrapping_add(thanks::CLASS_STATIC_FIELDS));
            if statics != 0 {
                let instance = guest_ld64(statics.wrapping_add(thanks::STATICS_INSTANCE));
                if instance != 0 {
                    let object = guest_ld64(instance.wrapping_add(thanks::INSTANCE_OBJECT));
                    if object != 0 {
                        prestate::snapshot(object.wrapping_add(thanks::OBJECT_BITS), 4, true);
                    }
                }
            }
        }
        let _ = this;
        0
    }
}

// ---------------------------------------------------------------------------------------------------------------
// heal_calc
// ---------------------------------------------------------------------------------------------------------------

mod heal {
    pub const FLAG: u64 = super::FLAG_PAGE + 0xd72;
    pub const CLASS_SLOT: u64 = 0x7103a46000 + 0xa90;
    pub const STRING_SLOT_ENTER: u64 = 0x7103a83000 + 0xbd0;
    pub const STRING_SLOT_HEAL: u64 = 0x7103a83000 + 0xbd8;
    pub const MONITOR: u64 = 0x10;
    pub const TARGET: u64 = 0x28;
    pub const SKILL_OBJ: u64 = 0x30;
    pub const TMP_VALUE: u64 = 0x40;
    pub const SKILL_ID: u64 = 0x14;
    pub const RESULT_LOGGER: u64 = 0xb8;
    pub const LOGGER_HEAL: u64 = 0x28;
    pub const LOGGER_IS_HEAL: u64 = 0x2c;
    pub const CLASS_INITIALIZED: u64 = 0xe0;
    /// Skill ids from this one on do not heal.
    pub const SKILL_ID_LIMIT: i32 = 1000;
    /// `x29 = sp + 0x10`; the `int` cell is at `x29 - 4`.
    pub const SP_DROP: u64 = 0x40;
    pub mod site {
        pub const INIT_CLASS: u64 = 0x71021fff58;
        pub const INIT_ENTER: u64 = 0x71021fff64;
        pub const INIT_HEAL: u64 = 0x71021fff70;
        pub const DEBUG_ENTER: u64 = 0x71021fff84;
        pub const CLASS_INIT: u64 = 0x71021fff9c;
        pub const MATH_MAX: u64 = 0x71021fffb8;
        pub const SET_RATIO: u64 = 0x71021fffc8;
        pub const DEBUG_WRITE_LONG: u64 = 0x71021ffff0;
        pub const TO_STRING: u64 = 0x7102200020;
        pub const CONCAT: u64 = 0x7102200030;
        pub const DEBUG_WRITE: u64 = 0x7102200040;
    }
}

impl SkillExecter {
    /// `heal_calc()`. `f` is the register file at entry.
    pub fn heal_calc(self, f: &mut Frame) -> Result<(), GuestException> {
        use heal::*;
        // prologue: `sub sp, sp, #0x40`, saves x29 to x22, `add x29, sp, #0x10`
        f.x[31] = f.entry[31].wrapping_sub(SP_DROP);
        f.x[29] = f.x[31].wrapping_add(0x10);
        let cell = f.x[29].wrapping_sub(4);
        f.x[20] = FLAG_PAGE;
        f.x[21] = 0x7103a46000;
        f.x[8] = u64::from(prestate::read_u8(FLAG));
        f.x[21] = live_u64(CLASS_SLOT);
        f.x[19] = f.x[0];
        if f.x[8] & 1 == 0 {
            f.x[0] = prestate::read_u64(CLASS_SLOT);
            f.call(
                callee::INITIALIZE_METADATA,
                site::INIT_CLASS,
                &[0, 8, 19, 20, 21, 29],
                true,
            )?;
            f.x[0] = prestate::read_u64(STRING_SLOT_ENTER);
            f.call(callee::INITIALIZE_METADATA, site::INIT_ENTER, &[0], false)?;
            f.x[0] = prestate::read_u64(STRING_SLOT_HEAL);
            f.call(callee::INITIALIZE_METADATA, site::INIT_HEAL, &[0], false)?;
            f.x[8] = 1;
            prestate::store_u8(FLAG, 1);
        }
        // `_monitor.<debug enter>(null)`
        f.x[0] = unsafe { guest_ld64(f.x[19].wrapping_add(MONITOR)) };
        f.x[1] = 0;
        f.call(
            callee::DEBUG_ENTER,
            site::DEBUG_ENTER,
            &[0, 1, 8, 19, 20, 21, 29],
            true,
        )?;
        // `heal = _tmp_value` into the int cell, then the `System.Math` class check
        f.x[20] = u64::from(unsafe { guest_ld32(f.x[19].wrapping_add(TMP_VALUE)) });
        prestate::store_u32(cell, f.x[20] as u32);
        f.x[0] = live_u64(f.x[21]);
        f.x[8] = u64::from(prestate::read_u32(f.x[0].wrapping_add(CLASS_INITIALIZED)));
        if f.x[8] as u32 == 0 {
            f.call(
                callee::CLASS_INITIALIZER,
                site::CLASS_INIT,
                &[0, 8, 20],
                false,
            )?;
        }
        // `heal = Math.Max(heal, 1)`
        f.x[21] = live_u64(STRING_SLOT_HEAL);
        f.x[0] = f.x[20] & 0xffff_ffff;
        f.x[1] = 1;
        f.x[2] = 0;
        f.x[22] = 1;
        f.call(
            callee::MATH_MAX,
            site::MATH_MAX,
            &[0, 1, 2, 8, 20, 21, 22],
            false,
        )?;
        f.x[20] = f.x[0] & 0xffff_ffff;
        prestate::store_u32(cell, f.x[0] as u32);
        // `set_acction_command_ratio()`
        f.x[0] = f.x[19];
        f.call_port(
            smrpg_skill_set_acction_command_ratio,
            site::SET_RATIO,
            &[0, 20],
            false,
        )?;
        // `if (_skill_obj._skill_id >= 1000) { debug line; heal = 0; }`
        f.x[8] = unsafe { guest_ld64(f.x[19].wrapping_add(SKILL_OBJ)) };
        f.x[8] = u64::from(unsafe { guest_ld32(f.x[8].wrapping_add(SKILL_ID)) });
        if (f.x[8] as u32 as i32) >= SKILL_ID_LIMIT {
            f.x[8] = 0x7103a83000;
            f.x[0] = unsafe { guest_ld64(f.x[19].wrapping_add(MONITOR)) };
            f.x[2] = 0;
            f.x[8] = live_u64(f.x[8].wrapping_add(0xbd0));
            f.x[1] = live_u64(f.x[8]);
            f.call(
                callee::DEBUG_WRITE,
                site::DEBUG_WRITE_LONG,
                &[0, 1, 2, 8],
                false,
            )?;
            f.x[20] = 0;
            prestate::store_u32(cell, 0);
        }
        // `_target._result_logger._heal = heal; ._is_heal = 1`
        f.x[8] = unsafe { guest_ld64(f.x[19].wrapping_add(TARGET)) };
        f.x[0] = cell;
        f.x[1] = 0;
        f.x[8] = unsafe { guest_ld64(f.x[8].wrapping_add(RESULT_LOGGER)) };
        prestate::store_u32(f.x[8].wrapping_add(LOGGER_HEAL), f.x[20] as u32);
        f.x[8] = unsafe { guest_ld64(f.x[19].wrapping_add(TARGET)) };
        f.x[8] = unsafe { guest_ld64(f.x[8].wrapping_add(RESULT_LOGGER)) };
        prestate::store_u32(f.x[8].wrapping_add(LOGGER_IS_HEAL), f.x[22] as u32);
        f.x[19] = unsafe { guest_ld64(f.x[19].wrapping_add(MONITOR)) };
        // `_monitor.write(<literal> + heal.ToString())`
        let mut area = prestate::read_u32(cell).to_le_bytes();
        f.call_out(
            callee::INT32_TO_STRING,
            site::TO_STRING,
            &[0, 1, 8, 19, 20],
            false,
            &mut area,
        )?;
        f.x[1] = f.x[0];
        f.x[0] = unsafe { guest_ld64(f.x[21]) };
        f.x[2] = 0;
        f.call(callee::STRING_CONCAT_2, site::CONCAT, &[0, 1, 2], false)?;
        f.x[1] = f.x[0];
        f.x[0] = f.x[19];
        f.x[2] = 0;
        f.call(callee::DEBUG_WRITE, site::DEBUG_WRITE, &[0, 1, 2], false)?;
        f.leave(&[19, 20, 21, 22, 29]);
        Ok(())
    }
}

/// 0x71021fff20 `SkillExecter.heal_calc` (replay row with a store journal; `set_acction_command_ratio` is called through
/// its own shim).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_skill_heal_calc(x: *mut u64) {
    unsafe {
        let regs = Regs::from_raw(x);
        let this = regs.x(0);
        let mut frame = Frame::new(regs);
        let _ = SkillExecter::at(this).heal_calc(&mut frame);
    }
}

/// Pre-state hook. Returns 1 for the first run in a process (the flag byte clear, the slots still tokens). Otherwise the
/// class initialized word, the two logger cells the port stores to, and the cells of the nested `set_acction_command_ratio`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_skill_heal_calc_pre(x: *mut u64) -> i32 {
    unsafe {
        let flag = guest_ld8(heal::FLAG);
        prestate::snapshot(heal::FLAG, 1, true);
        if flag & 1 == 0 {
            return 1;
        }
        let regs = Regs::from_raw(x);
        let this = SkillExecter::at(regs.x(0));
        let class_cell = guest_ld64(heal::CLASS_SLOT);
        let class = guest_ld64(class_cell);
        prestate::snapshot(class.wrapping_add(heal::CLASS_INITIALIZED), 4, false);
        let target = this.target();
        let logger = guest_ld64(target.address().wrapping_add(heal::RESULT_LOGGER));
        prestate::snapshot(logger.wrapping_add(heal::LOGGER_HEAL), 4, true);
        prestate::snapshot(logger.wrapping_add(heal::LOGGER_IS_HEAL), 4, true);
        let mut nested = [0u64; 32];
        nested[0] = this.address();
        if smrpg_skill_set_acction_command_ratio_pre(nested.as_mut_ptr()) != 0 {
            return 1;
        }
        0
    }
}

#[cfg(test)]
mod tests;
