//! The head of `Battle.SkillExecter.exec_koudou_kouka` (0x71021f6340): prologue, lazy metadata initialization, the class
//! checks and the first loop, up to label 0x71021f67d0 (task-70, step 1 of the split plan in docs/DECOMP.md).
//!
//! In C# terms (IL2CPP names where the dump has them, the rest named by what they hold):
//!
//! ```text
//! void SkillExecter.exec_koudou_kouka() {
//!     <lazy metadata initialization: 27 slots>
//!     <class initializers: the table owner, BattleManager, a third class>
//!     var element = <table owner>._list[_skill_obj._type];          // IndexOutOfRange when past the length
//!     var global = GlobalWork.get_inst();
//!     int last = 0;                                                  // w19
//!     if (element.<+0x1b>) last = _target.<bd6c0>(5);                // six flag bytes, six calls, in this order:
//!     if (element.<+0x1c>) last = _target.<bd6c0>(6);                // +0x1b 5, +0x1c 6, +0x1a 4, +0x1d 7, +0x18 2, +0x19 3
//!     ...
//!     if (any of the six bytes is set) {
//!         if (last != 0) {                                           // else: _target.result_logger.<+0x47> = true
//!             <GlobalWork>.get_inst(); if (<get2> == 1 && acter.<+0x6c> == 0) { <two debug lines>;
//!                 if (!<18bfbc0>(0x32, monitor)) { <two debug lines>; last = 0; skipped = true } }
//!             singleton = <892aa0>(); if (singleton.<+0x68> == 3) { logger.<+0x47> = true; <debug line>; last = 0 }
//!             if (_target.is_barriered(false) && !logger.<+0x47>) { <debug line>; logger.<+0x47> = true;
//!                 if (!_flag_44) _target.use_damage_guard(); last = 0 }
//!         }
//!     } else skipped = false;
//!     if (last != 0) _target.result_logger.<+0x60> = last;
//!     switch (_skill_obj._type) {                                    // table 1 (task-72): `Arm`, ported arms only
//!         case <outside 5..=0x5f>, <the 52 types whose target is the epilogue>: break;
//!         case 0xb..=0x10: if (skipped) logger.<+0x40> = 1; this._flag_44 = false; break;
//!         case 0x12: logger.<+0x60> = 9; return;  case 0x15, 0x19, 0x46: logger.<+0x60> = 1; return;
//!         case 0x4a: _result.<+0x24> = 1; break;
//!         case 0x50: _result.<+0x24> = 1; goto case 8;
//!         case 8: _target.<ba6d0>(); _result.<+0x38> |= 8; break;    // 7 and 9: <ba6a0>, |= 4
//!         case 5: <halved or plain getter value, two marks>;  case 6: <getter value, mode 3, a byte>;
//!         default: <lifted: the whole call runs in the lifted body>
//!     }
//!     if (last != 0) _target.result_logger.<+0x60> = last;           // the epilogue's own store (not for 0x12, 0x15, ...)
//! }
//! ```
//!
//! The port covers every path that leaves through one of those arms. A call whose arm is not ported is run lifted: in `ab`
//! mode the pre-state hook returns 1 (deferred, counted, the lifted body runs alone and nothing is compared); in `on` mode
//! the port starts with the same test and hands the untouched register file to the lifted body (`Regs::run_lifted`). The
//! table entry is read from the guest's table (halfwords at 0x7102b04ef0, units of 4 bytes from 0x71021f6800). A bounds
//! failure of the list index (the lifted code calls the IndexOutOfRange thrower, which never returns) is deferred in `ab`
//! mode too: the selftest's stand-in callee returns where the real one throws, so it cannot be compared there.
//!
//! The function keeps the lifted register residue (see `Frame`): each call site writes the register list the lifted C
//! writes there, and the stack stores of the frame (dead locals, never read on these paths) are not journaled.

use super::damage::{SkillExecter, smrpg_param_use_damage_guard, smrpg_param_use_damage_guard_pre};
use super::exec_koudou_leaves::{
    smrpg_param_bd6c0, smrpg_param_flag_10, smrpg_param_flag_10_pre, smrpg_param_flag_11,
    smrpg_param_flag_11_pre,
};
use super::frame::Frame;
use super::{GuestException, GuestObject, Regs};
use crate::bridge::prestate;
#[cfg(not(test))]
use crate::guest_ld16;
use crate::{guest_ld8, guest_ld32, guest_ld64};

/// The function this port heads.
pub const ENTRY: u64 = 0x71021f6340;

const FLAG_PAGE: u64 = 0x7103cba000;
const LITERAL_PAGE: u64 = 0x7103a83000;

mod callee {
    pub const INITIALIZE_METADATA: u64 = 0x71000c4150;
    pub const CLASS_INITIALIZER: u64 = 0x71000c4330;
    pub const INDEX_OUT_OF_RANGE: u64 = 0x71000c4510;
    pub const GET_INST: u64 = 0x7101886a00;
    pub const GET_VALUE: u64 = 0x71018876b0;
    pub const DEBUG_WRITE: u64 = 0x71018bebc0;
    pub const CHECK_32: u64 = 0x71018bfbc0;
    pub const SINGLETON: u64 = 0x7101892aa0;
    pub const IS_BARRIERED: u64 = 0x71021c3bb0;
    pub const BD6C0: u64 = 0x71021bd6c0;
    pub const USE_DAMAGE_GUARD: u64 = 0x71021be280;
    pub const FLAG_10: u64 = 0x71021ba6a0;
    pub const FLAG_11: u64 = 0x71021ba6d0;
    /// Getters of the target: `[[x0 + 0x28] + 0x10]` and `[[x0 + 0x28] + 0x18]`.
    pub const GETTER_10: u64 = 0x71021b7fb0;
    pub const GETTER_18: u64 = 0x71021b7fa0;
}

mod cells {
    /// Lazy-initialization flag byte (bit 0).
    pub const FLAG: u64 = super::FLAG_PAGE + 0xd5f;
    /// Slot holding the class cell of the list owner.
    pub const CLASS_A_SLOT: u64 = 0x7103a58000 + 0x1c0;
    /// Slot holding the `BattleManager` class cell.
    pub const CLASS_B_SLOT: u64 = 0x7103a57000 + 0xe48;
    /// Slot holding the third class cell.
    pub const CLASS_C_SLOT: u64 = 0x7103a58000 + 0x48;
    /// Pointer to the byte that says the static-field metadata has been initialized.
    pub const STATIC_FLAG_POINTER: u64 = 0x7103a58000 + 0x1d0;
    /// The six literal slots the debug lines use (data page 0x7103a83000 + offset): in the order of the lines.
    pub const LITERAL_5A0: u64 = super::LITERAL_PAGE + 0x5a0;
    pub const LITERAL_588: u64 = super::LITERAL_PAGE + 0x588;
    pub const LITERAL_590: u64 = super::LITERAL_PAGE + 0x590;
    pub const LITERAL_5D0: u64 = super::LITERAL_PAGE + 0x5d0;
    pub const LITERAL_578: u64 = super::LITERAL_PAGE + 0x578;
    pub const LITERAL_580: u64 = super::LITERAL_PAGE + 0x580;
    pub const LITERALS: [u64; 6] = [
        LITERAL_578,
        LITERAL_580,
        LITERAL_588,
        LITERAL_590,
        LITERAL_5A0,
        LITERAL_5D0,
    ];
    /// The 27 slots the lazy initialization resolves, in call order (the first call returns to 0x71021f638c, then every 12).
    pub const INIT_SLOTS: [u64; 27] = [
        0x7103a6d000 + 0x5b8,
        0x7103a6c000 + 0x3b8,
        0x7103a57000 + 0xe48,
        0x7103a58000 + 0x248,
        0x7103a58000 + 0x250,
        0x7103a58000 + 0x258,
        0x7103a58000 + 0x1c0,
        0x7103a58000 + 0x48,
        0x7103a58000 + 0xb38,
        0x7103a59000 + 0x250,
        0x7103a58000 + 0x240,
        0x7103a58000 + 0x478,
        0x7103a58000 + 0xa00,
        super::LITERAL_PAGE + 0x568,
        super::LITERAL_PAGE + 0x570,
        super::LITERAL_PAGE + 0x578,
        super::LITERAL_PAGE + 0x580,
        super::LITERAL_PAGE + 0x588,
        super::LITERAL_PAGE + 0x590,
        super::LITERAL_PAGE + 0x598,
        super::LITERAL_PAGE + 0x5a0,
        super::LITERAL_PAGE + 0x5a8,
        super::LITERAL_PAGE + 0x5b0,
        super::LITERAL_PAGE + 0x5b8,
        super::LITERAL_PAGE + 0x5c0,
        super::LITERAL_PAGE + 0x5c8,
        super::LITERAL_PAGE + 0x5d0,
    ];
}

mod field {
    pub const MONITOR: u64 = 0x10;
    pub const ACTER: u64 = 0x20;
    pub const TARGET: u64 = 0x28;
    pub const SKILL_OBJ: u64 = 0x30;
    pub const FLAG_44: u64 = 0x44;
    /// The result object (`this + 0x18`): byte +0x24, flag word +0x38.
    pub const RESULT_OBJECT: u64 = 0x18;
    pub const RESULT_BYTE_24: u64 = 0x24;
    pub const RESULT_FLAGS_38: u64 = 0x38;
    /// `_skill_obj` field the type 5 arm compares with 0x7f.
    pub const SKILL_FIELD_14: u64 = 0x14;
    /// Logger cells the arms store.
    pub const LOGGER_VALUE_28: u64 = 0x28;
    pub const LOGGER_MODE_2C: u64 = 0x2c;
    pub const LOGGER_MARK_40: u64 = 0x40;
    pub const LOGGER_BYTE_41: u64 = 0x41;
    pub const LOGGER_BYTE_45: u64 = 0x45;
    /// `_skill_obj`: the type that indexes the list and the jump table.
    pub const SKILL_TYPE: u64 = 0x28;
    /// The acter's byte the debug block tests.
    pub const PARAM_BYTE: u64 = 0x6c;
    pub const RESULT_LOGGER: u64 = 0xb8;
    pub const LOGGER_FLAG_47: u64 = 0x47;
    pub const LOGGER_VALUE: u64 = 0x60;
    pub const CLASS_INITIALIZED: u64 = 0xe0;
    pub const CLASS_STATIC_FIELDS: u64 = 0xb8;
    /// The list (an array) in the first class's statics; length at +0x18, elements from +0x20.
    pub const STATICS_LIST: u64 = 0xe0;
    pub const ARRAY_LENGTH: u64 = 0x18;
    pub const ARRAY_FIRST: u64 = 0x20;
    pub const STATICS_OBJECT: u64 = 0x10;
    pub const OBJECT_PTR: u64 = 0x60;
    /// The singleton's mode int.
    pub const SINGLETON_MODE: u64 = 0x68;
    /// The six flag bytes of a list element.
    pub const BYTE_18: u64 = 0x18;
    pub const BYTE_19: u64 = 0x19;
    pub const BYTE_1A: u64 = 0x1a;
    pub const BYTE_1B: u64 = 0x1b;
    pub const BYTE_1C: u64 = 0x1c;
    pub const BYTE_1D: u64 = 0x1d;
}

/// `_skill_obj._type - 5` at or below this value takes the compiled jump table (not ported).
const TABLE_RANGE: u32 = 0x5a;
/// The mode of the singleton that marks the logger.
const MODE_MARK: u32 = 3;

fn w(value: u32) -> u64 {
    u64::from(value)
}

fn ld64(address: u64) -> u64 {
    unsafe { guest_ld64(address) }
}

fn ld32(address: u64) -> u32 {
    unsafe { guest_ld32(address) }
}

fn ld8(address: u64) -> u8 {
    unsafe { guest_ld8(address) }
}

/// A metadata slot or other cell at a fixed address (data pages): the unit tests answer through the pre-state fakes.
fn live_u64(address: u64) -> u64 {
    if cfg!(test) {
        prestate::read_u64(address)
    } else {
        ld64(address)
    }
}

fn live_u32(address: u64) -> u32 {
    if cfg!(test) {
        prestate::read_u32(address)
    } else {
        ld32(address)
    }
}

/// A call to a function that is itself a port goes through its shim in a real run; the unit tests install fakes instead.
#[cfg(not(test))]
fn call_nested(
    f: &mut Frame,
    port: unsafe extern "C" fn(*mut u64),
    _target: u64,
    return_address: u64,
    list: &[usize],
) -> Result<(), GuestException> {
    f.call_port(port, return_address, list, false)
}

#[cfg(test)]
fn call_nested(
    f: &mut Frame,
    _port: unsafe extern "C" fn(*mut u64),
    target: u64,
    return_address: u64,
    list: &[usize],
) -> Result<(), GuestException> {
    f.call(target, return_address, list, false)
}

/// `_skill_obj._type` of an executer, or `None` when a pointer on the way is null.
fn skill_type(this: u64) -> Option<u32> {
    let skill_obj = ld64(this.wrapping_add(field::SKILL_OBJ));
    if skill_obj == 0 {
        None
    } else {
        Some(ld32(skill_obj.wrapping_add(field::SKILL_TYPE)))
    }
}

/// The arms of the compiled jump table (table 1) that are ported, named by the label they sit at and by what they do.
/// Every other table target stays in the lifted body (the pre-state hook defers it).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Arm {
    /// 0x71021f7610: the common epilogue (also the target of every type outside the table).
    Epilogue,
    /// 0x71021f7618: the epilogue's logger store, entered with `w9` as the dispatch left it (9).
    StoreNine,
    /// 0x71021f6840: `w9 = 1`, then the epilogue's logger store.
    StoreOne,
    /// 0x71021f6800: mark the logger when the loop skipped, clear `_flag_44`.
    ClearFlag,
    /// 0x71021f6928: the result object's byte +0x24 = 1.
    ResultByte,
    /// 0x71021f68e8: `ResultByte`, then `Guard11`.
    ResultByteGuard11,
    /// 0x71021f68f4: `ba6d0` on the target, then `result +0x38 |= 8`.
    Guard11,
    /// 0x71021f6aa0: `ba6a0` on the target, then `result +0x38 |= 4`.
    Guard10,
    /// 0x71021f6ad0 (type 5): half of a getter value, or the value itself, into the logger.
    Half,
    /// 0x71021f6b28 (type 6): a getter value into the logger.
    Plain,
}

/// The table's code base (`adr x12`), its halfword entries, and the entry for a type.
const TABLE_CODE: u64 = 0x71021f6800;
const TABLE_DATA: u64 = 0x7102b04ef0;

#[cfg(not(test))]
fn table_entry(index: u32) -> u16 {
    unsafe { guest_ld16(TABLE_DATA.wrapping_add(u64::from(index) << 1)) }
}

#[cfg(test)]
fn table_entry(index: u32) -> u16 {
    tests::fake_entry(index)
}

fn arm_at(target: u64) -> Option<Arm> {
    Some(match target {
        0x71021f7610 => Arm::Epilogue,
        0x71021f7618 => Arm::StoreNine,
        0x71021f6840 => Arm::StoreOne,
        0x71021f6800 => Arm::ClearFlag,
        0x71021f6928 => Arm::ResultByte,
        0x71021f68e8 => Arm::ResultByteGuard11,
        0x71021f68f4 => Arm::Guard11,
        0x71021f6aa0 => Arm::Guard10,
        0x71021f6ad0 => Arm::Half,
        0x71021f6b28 => Arm::Plain,
        _ => return None,
    })
}

/// The arm a skill type selects: the epilogue for a type outside the table, else the table's target when it is ported.
fn arm_of(skill_type: u32) -> Option<Arm> {
    let index = skill_type.wrapping_sub(5);
    if index > TABLE_RANGE {
        return Some(Arm::Epilogue);
    }
    arm_at(TABLE_CODE.wrapping_add(u64::from(table_entry(index)) << 2))
}

impl SkillExecter {
    fn this_head(f: &Frame) -> u64 {
        f.x[20]
    }

    /// `f` is the register file at entry. Returns Ok after the epilogue, Err when a callee left an exception pending.
    pub fn exec_koudou_head(self, f: &mut Frame) -> Result<(), GuestException> {
        use field::*;
        // `on` mode: a call that would take a lifted arm runs lifted, whole, before the port touches anything
        match skill_type(f.x[0]) {
            Some(kind) if arm_of(kind).is_some() => {}
            _ => {
                f.regs.run_lifted(ENTRY);
                return Ok(());
            }
        }
        // prologue: `sub sp, sp, #0xd0`, the saves, `add x29, sp, #0x70`
        f.x[31] = f.entry[31].wrapping_sub(0xd0);
        f.x[29] = f.x[31].wrapping_add(0x70);
        f.x[19] = FLAG_PAGE;
        f.x[21] = 0x7103a58000;
        f.x[8] = u64::from(prestate::read_u8(cells::FLAG));
        f.x[21] = live_u64(cells::CLASS_A_SLOT);
        f.x[20] = f.x[0];
        if f.x[8] & 1 == 0 {
            self.lazy_initialization(f)?;
        }
        // 0x71021f64cc: the first class (the list owner)
        f.x[0] = live_u64(f.x[21]);
        f.x[8] = w(prestate::read_u32(f.x[0].wrapping_add(CLASS_INITIALIZED)));
        if f.x[8] as u32 == 0 {
            f.call(
                callee::CLASS_INITIALIZER,
                0x71021f64f0,
                &[0, 8, 19, 20, 21, 29],
                true,
            )?;
            f.x[0] = live_u64(f.x[21]);
        }
        // the list element for this skill type
        f.x[8] = ld64(f.x[0].wrapping_add(CLASS_STATIC_FIELDS));
        f.x[8] = ld64(f.x[8].wrapping_add(STATICS_LIST));
        f.x[9] = ld64(Self::this_head(f).wrapping_add(SKILL_OBJ));
        f.x[9] = ld32(f.x[9].wrapping_add(SKILL_TYPE)) as i32 as i64 as u64;
        f.x[10] = w(ld32(f.x[8].wrapping_add(ARRAY_LENGTH)));
        if f.x[9] as u32 >= f.x[10] as u32 {
            // 0x71021f7560: the thrower never returns; a return without a pending exception is not a state the game has
            f.call(
                callee::INDEX_OUT_OF_RANGE,
                0x71021f7564,
                &[0, 8, 9, 10, 11, 12, 13, 19, 20, 21, 22, 23, 24, 26, 28, 29],
                true,
            )?;
            return Err(GuestException);
        }
        // 0x71021f6510: the BattleManager class cell, the element, the class checks
        f.x[19] = 0x7103a57000;
        f.x[19] = live_u64(f.x[19].wrapping_add(0xe48));
        f.x[0] = live_u64(f.x[19]);
        f.x[8] = f.x[8].wrapping_add(f.x[9] << 3);
        f.x[28] = ld64(f.x[8].wrapping_add(ARRAY_FIRST));
        f.x[8] = w(prestate::read_u32(f.x[0].wrapping_add(CLASS_INITIALIZED)));
        let mut metadata_pending;
        if f.x[8] as u32 == 0 {
            // 0x71021f677c
            f.call(
                callee::CLASS_INITIALIZER,
                0x71021f6780,
                &[0, 8, 9, 10, 19, 20, 21, 28, 29],
                true,
            )?;
            f.x[25] = 0x7103a58000;
            f.x[25] = live_u64(f.x[25].wrapping_add(0x1d0));
            f.x[8] = u64::from(prestate::read_u8(f.x[25]));
            f.regs
                .safepoint_n(0x71021f678c, &[(8, f.x[8]), (25, f.x[25])]);
            metadata_pending = f.x[8] as u32 == 0;
        } else {
            // 0x71021f652c
            f.x[25] = 0x7103a58000;
            f.x[25] = live_u64(f.x[25].wrapping_add(0x1d0));
            f.x[8] = u64::from(prestate::read_u8(f.x[25]));
            metadata_pending = f.x[8] as u32 == 0;
        }
        if metadata_pending {
            // 0x71021f6790: resolve the static-field metadata of this method and set its byte
            f.x[0] = 0x7103a57000;
            f.x[0] = live_u64(f.x[0].wrapping_add(0xe48));
            f.call(
                callee::INITIALIZE_METADATA,
                0x71021f679c,
                &[0, 8, 9, 10, 19, 20, 21, 25, 28, 29],
                true,
            )?;
            f.x[8] = 1;
            prestate::store_u8(f.x[25], 1);
            f.regs.safepoint_n(0x71021f67a4, &[(8, 0)]);
            metadata_pending = false;
        }
        debug_assert!(!metadata_pending);
        // 0x71021f653c: the BattleManager class again (read live: the initializer above may have run), then the third class
        f.x[26] = 0x7103a58000;
        f.x[26] = live_u64(f.x[26].wrapping_add(0x48));
        f.x[0] = live_u64(f.x[19]);
        f.x[8] = w(live_u32(f.x[0].wrapping_add(CLASS_INITIALIZED)));
        if f.x[8] as u32 == 0 {
            f.call(
                callee::CLASS_INITIALIZER,
                0x71021f6554,
                &[0, 8, 9, 10, 19, 20, 21, 25, 26, 28, 29],
                true,
            )?;
            f.x[0] = live_u64(f.x[19]);
        }
        f.x[8] = ld64(f.x[0].wrapping_add(CLASS_STATIC_FIELDS));
        f.x[0] = live_u64(f.x[26]);
        f.x[22] = ld64(f.x[8].wrapping_add(STATICS_OBJECT));
        f.x[8] = w(prestate::read_u32(f.x[0].wrapping_add(CLASS_INITIALIZED)));
        f.x[27] = ld64(f.x[22].wrapping_add(OBJECT_PTR));
        if f.x[8] as u32 == 0 {
            f.call(
                callee::CLASS_INITIALIZER,
                0x71021f6574,
                &[0, 8, 9, 10, 19, 20, 21, 22, 25, 26, 27, 28, 29],
                true,
            )?;
        }
        f.x[0] = 0;
        f.call(
            callee::GET_INST,
            0x71021f657c,
            &[0, 8, 9, 10, 19, 20, 21, 22, 25, 26, 27, 28, 29],
            true,
        )?;
        f.x[23] = f.x[0];
        self.first_loop(f)?;
        self.dispatch(f)
    }

    /// 0x71021f67d0: the jump table test and dispatch (table 1), then the arm, then the common epilogue.
    fn dispatch(self, f: &mut Frame) -> Result<(), GuestException> {
        use field::*;
        f.x[8] = ld64(Self::this_head(f).wrapping_add(SKILL_OBJ));
        f.x[9] = w(ld32(f.x[8].wrapping_add(SKILL_TYPE)));
        f.x[10] = w((f.x[9] as u32).wrapping_sub(5));
        if f.x[10] as u32 > TABLE_RANGE {
            return Self::epilogue(f, false);
        }
        f.x[11] = TABLE_DATA;
        f.x[12] = TABLE_CODE;
        f.x[13] = u64::from(table_entry(f.x[10] as u32));
        f.x[12] = f.x[12].wrapping_add(f.x[13] << 2);
        f.x[9] = 9;
        match arm_at(f.x[12]) {
            Some(Arm::Epilogue) => Self::epilogue(f, false),
            Some(Arm::StoreNine) => Self::epilogue(f, true),
            Some(Arm::StoreOne) => {
                f.x[9] = 1;
                Self::epilogue(f, true)
            }
            Some(Arm::ClearFlag) => {
                // 0x71021f6800
                if f.x[24] as u32 != 0 {
                    f.x[8] = ld64(f.x[28]);
                    f.x[9] = 1;
                    f.x[8] = ld64(f.x[8].wrapping_add(RESULT_LOGGER));
                    prestate::store_u8(f.x[8].wrapping_add(LOGGER_MARK_40), 1);
                }
                prestate::store_u8(Self::this_head(f).wrapping_add(FLAG_44), 0);
                Self::epilogue(f, false)
            }
            Some(Arm::ResultByte) => {
                self.result_byte(f);
                Self::epilogue(f, false)
            }
            Some(Arm::ResultByteGuard11) => {
                self.result_byte(f);
                self.guard(f, true)
            }
            Some(Arm::Guard11) => self.guard(f, true),
            Some(Arm::Guard10) => self.guard(f, false),
            Some(Arm::Half) => self.type_five(f),
            Some(Arm::Plain) => self.type_six(f),
            None => unreachable!("the pre-state hook defers every arm that is not ported"),
        }
    }

    /// 0x71021f7610 (`direct` false: `w9 = w19`, the store only for a nonzero result) or 0x71021f7618 (`direct`: the store
    /// with `w9` as it is) and the function epilogue.
    fn epilogue(f: &mut Frame, direct: bool) -> Result<(), GuestException> {
        use field::*;
        let store = if direct {
            true
        } else {
            f.x[9] = w(f.x[19] as u32);
            f.x[19] as u32 != 0
        };
        if store {
            f.x[8] = ld64(f.x[28]);
            f.x[8] = ld64(f.x[8].wrapping_add(RESULT_LOGGER));
            prestate::store_u32(f.x[8].wrapping_add(LOGGER_VALUE), f.x[9] as u32);
        }
        f.v[8] = (f.v_entry[8].0, 0);
        f.v[9] = (f.v_entry[9].0, 0);
        f.v[10] = (f.v_entry[10].0, 0);
        f.leave(&[0, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29]);
        f.flush_v(&[8, 9, 10]);
        Ok(())
    }

    /// 0x71021f68e8 / 0x71021f6928: the result object's byte +0x24 = 1.
    fn result_byte(self, f: &mut Frame) {
        use field::*;
        f.x[8] = ld64(Self::this_head(f).wrapping_add(RESULT_OBJECT));
        f.x[9] = 1;
        prestate::store_u8(f.x[8].wrapping_add(RESULT_BYTE_24), 1);
    }

    /// 0x71021f68f4 (`guard11`: `ba6d0`, then `+0x38 |= 8`) and 0x71021f6aa0 (`ba6a0`, then `+0x38 |= 4`), both on the
    /// target and with `x1 = 0`.
    fn guard(self, f: &mut Frame, guard11: bool) -> Result<(), GuestException> {
        use field::*;
        f.x[0] = ld64(Self::this_head(f).wrapping_add(TARGET));
        f.x[1] = 0;
        let list = &[0, 1, 8, 9, 10, 11, 12, 13, 19, 23, 24, 28];
        if guard11 {
            call_nested(f, smrpg_param_flag_11, callee::FLAG_11, 0x71021f6900, list)?;
        } else {
            call_nested(f, smrpg_param_flag_10, callee::FLAG_10, 0x71021f6aac, list)?;
        }
        f.x[8] = f.regs.x(8);
        f.x[9] = f.regs.x(9);
        // 0x71021f6f38 (or the same three lines in the other arm)
        f.x[8] = ld64(Self::this_head(f).wrapping_add(RESULT_OBJECT));
        let cell = f.x[8].wrapping_add(RESULT_FLAGS_38);
        f.x[9] = w(prestate::read_u32(cell));
        f.x[9] = w((f.x[9] as u32) | if guard11 { 8 } else { 4 });
        prestate::store_u32(cell, f.x[9] as u32);
        Self::epilogue(f, false)
    }

    /// 0x71021f6ad0, skill type 5: `x1 = 0; x21 = skill_obj[+0x14]; x20 = target.result_logger; v = <getter>(target)`;
    /// the logger gets `v` and two marks when `x21` is not 0x7f, else `v / 2` (rounded toward zero), two marks and a byte.
    fn type_five(self, f: &mut Frame) -> Result<(), GuestException> {
        use field::*;
        f.x[0] = ld64(f.x[28]);
        f.x[1] = 0;
        f.x[21] = w(ld32(f.x[8].wrapping_add(SKILL_FIELD_14)));
        f.x[20] = ld64(f.x[0].wrapping_add(RESULT_LOGGER));
        f.call(
            callee::GETTER_10,
            0x71021f6ae4,
            &[0, 1, 8, 9, 10, 11, 12, 13, 19, 20, 21, 23, 24, 28],
            false,
        )?;
        f.x[8] = f.regs.x(8);
        if f.x[21] as u32 == 0x7f {
            let v = f.x[0] as u32 as i32;
            f.x[8] = w((v.wrapping_add(i32::from(v < 0)) >> 1) as u32);
            prestate::store_u32(f.x[20].wrapping_add(LOGGER_VALUE_28), f.x[8] as u32);
            Self::logger_marks(f, true);
        } else {
            prestate::store_u32(f.x[20].wrapping_add(LOGGER_VALUE_28), f.x[0] as u32);
            Self::logger_marks(f, false);
        }
        Self::epilogue(f, false)
    }

    /// 0x71021f6fe4 / 0x71021f6afc: `logger +0x2c = 1; logger +0x60 = 1`, and for the halved value a byte +0x45 = 1.
    fn logger_marks(f: &mut Frame, with_byte: bool) {
        use field::*;
        f.x[9] = 1;
        for (offset, value) in [(LOGGER_MODE_2C, 1u32), (LOGGER_VALUE, 1)] {
            f.x[8] = ld64(f.x[28]);
            f.x[8] = ld64(f.x[8].wrapping_add(RESULT_LOGGER));
            prestate::store_u32(f.x[8].wrapping_add(offset), value);
        }
        if with_byte {
            f.x[8] = ld64(f.x[28]);
            f.x[8] = ld64(f.x[8].wrapping_add(RESULT_LOGGER));
            prestate::store_u8(f.x[8].wrapping_add(LOGGER_BYTE_45), 1);
        }
    }

    /// 0x71021f6b28, skill type 6: the other getter's value, mode 3 and a byte +0x41 in the logger.
    fn type_six(self, f: &mut Frame) -> Result<(), GuestException> {
        use field::*;
        f.x[0] = ld64(f.x[28]);
        f.x[1] = 0;
        f.x[20] = ld64(f.x[0].wrapping_add(RESULT_LOGGER));
        f.call(
            callee::GETTER_18,
            0x71021f6b38,
            &[0, 1, 8, 9, 10, 11, 12, 13, 19, 20, 23, 24, 28],
            false,
        )?;
        f.x[8] = f.regs.x(8);
        prestate::store_u32(f.x[20].wrapping_add(LOGGER_VALUE_28), f.x[0] as u32);
        f.x[8] = ld64(f.x[28]);
        f.x[8] = ld64(f.x[8].wrapping_add(RESULT_LOGGER));
        f.x[9] = 3;
        prestate::store_u32(f.x[8].wrapping_add(LOGGER_MODE_2C), 3);
        f.x[8] = ld64(f.x[28]);
        f.x[8] = ld64(f.x[8].wrapping_add(RESULT_LOGGER));
        f.x[9] = 1;
        prestate::store_u8(f.x[8].wrapping_add(LOGGER_BYTE_41), 1);
        Self::epilogue(f, false)
    }

    /// 0x71021f6380: 27 metadata slots, then the flag byte.
    fn lazy_initialization(self, f: &mut Frame) -> Result<(), GuestException> {
        for (k, slot) in cells::INIT_SLOTS.iter().enumerate() {
            f.x[0] = prestate::read_u64(*slot);
            let ret = 0x71021f638c + 12 * k as u64;
            if k == 0 {
                f.call(
                    callee::INITIALIZE_METADATA,
                    ret,
                    &[0, 8, 19, 20, 21, 29],
                    true,
                )?;
            } else {
                f.call(callee::INITIALIZE_METADATA, ret, &[0], false)?;
            }
        }
        f.x[8] = 1;
        prestate::store_u8(cells::FLAG, 1);
        Ok(())
    }

    /// One `bd6c0(code)` call on the target: `x0 = this._target`, `x1 = code`, `x2 = 0`; the result becomes `x19`.
    fn flag_call(
        f: &mut Frame,
        this: u64,
        code: u64,
        return_address: u64,
        list: &[usize],
    ) -> Result<(), GuestException> {
        f.x[0] = ld64(this.wrapping_add(field::TARGET));
        f.x[1] = code;
        f.x[2] = 0;
        call_nested(f, smrpg_param_bd6c0, callee::BD6C0, return_address, list)?;
        f.x[2] = f.regs.x(2);
        f.x[8] = f.regs.x(8);
        f.x[19] = w(f.x[0] as u32);
        Ok(())
    }

    /// Labels 0x71021f6588 to 0x71021f67bc: the six flag bytes of the element each call `bd6c0` on the target, then the
    /// rest of the first loop. Leaves `x19` (the last result), `x24` and `x28` (the address of the `_target` field).
    fn first_loop(self, f: &mut Frame) -> Result<(), GuestException> {
        use field::*;
        let this = Self::this_head(f);
        let element = f.x[28];
        f.x[8] = u64::from(ld8(element.wrapping_add(BYTE_1B)));
        let mut second = true;
        if f.x[8] as u32 != 0 {
            Self::flag_call(f, this, 5, 0x71021f6598, &[0, 1, 2, 8, 23])?;
            f.x[8] = u64::from(ld8(element.wrapping_add(BYTE_1C)));
            second = f.x[8] as u32 != 0;
        } else {
            // 0x71021f67a8: no call yet; x19 = 0 and a poll before the second byte
            f.x[19] = 0;
            f.x[8] = u64::from(ld8(element.wrapping_add(BYTE_1C)));
            f.regs
                .safepoint_n(0x71021f67b0, &[(8, f.x[8]), (19, f.x[19]), (23, f.x[23])]);
            if f.x[8] as u32 == 0 {
                f.regs
                    .safepoint_n(0x71021f67b4, &[(19, f.x[19]), (23, f.x[23]), (8, 0)]);
                second = false;
            }
        }
        if second {
            Self::flag_call(f, this, 6, 0x71021f65b4, &[0, 1, 2, 8, 19, 23])?;
        }
        for (byte, code, ret) in [
            (BYTE_1A, 4, 0x71021f65d0),
            (BYTE_1D, 7, 0x71021f65ec),
            (BYTE_18, 2, 0x71021f6608),
        ] {
            f.x[8] = u64::from(ld8(element.wrapping_add(byte)));
            if f.x[8] as u32 != 0 {
                Self::flag_call(f, this, code, ret, &[0, 1, 2, 8, 19, 23])?;
            }
        }
        f.x[8] = u64::from(ld8(element.wrapping_add(BYTE_19)));
        if f.x[8] as u32 != 0 {
            f.x[0] = ld64(this.wrapping_add(TARGET));
            f.x[1] = 3;
            f.x[2] = 0;
            call_nested(
                f,
                smrpg_param_bd6c0,
                callee::BD6C0,
                0x71021f6624,
                &[0, 1, 2, 8, 19, 23],
            )?;
            f.x[8] = u64::from(ld8(element.wrapping_add(BYTE_19)));
            f.x[19] = w(f.x[0] as u32);
        }
        // 0x71021f662c: the OR of the six bytes (x8 holds the last one)
        f.x[9] = u64::from(ld8(element.wrapping_add(BYTE_1B)));
        f.x[10] = u64::from(ld8(element.wrapping_add(BYTE_1C)));
        f.x[9] = w((f.x[10] | f.x[9]) as u32);
        f.x[11] = u64::from(ld8(element.wrapping_add(BYTE_1A)));
        f.x[12] = u64::from(ld8(element.wrapping_add(BYTE_1D)));
        f.x[13] = u64::from(ld8(element.wrapping_add(BYTE_18)));
        f.x[9] = w((f.x[9] | f.x[11]) as u32);
        f.x[9] = w((f.x[9] | f.x[12]) as u32);
        f.x[9] = w((f.x[9] | f.x[13]) as u32);
        f.x[8] = w((f.x[9] | f.x[8]) as u32);
        if f.x[8] as u32 == 0 {
            // 0x71021f67b8
            f.x[24] = 0;
            return self.close_loop(f);
        }
        if f.x[19] as u32 == 0 {
            // 0x71021f681c: the target's logger byte is set, the third class is checked again
            f.x[8] = ld64(this.wrapping_add(TARGET));
            f.x[9] = 1;
            f.x[8] = ld64(f.x[8].wrapping_add(RESULT_LOGGER));
            prestate::store_u8(f.x[8].wrapping_add(LOGGER_FLAG_47), 1);
            f.x[0] = live_u64(f.x[26]);
            f.x[8] = w(live_u32(f.x[0].wrapping_add(CLASS_INITIALIZED)));
            f.regs.safepoint_n(
                0x71021f6834,
                &[
                    (0, f.x[0]),
                    (8, f.x[8]),
                    (9, f.x[9]),
                    (10, f.x[10]),
                    (11, f.x[11]),
                    (12, f.x[12]),
                    (13, f.x[13]),
                    (19, f.x[19]),
                    (23, f.x[23]),
                ],
            );
        } else {
            // 0x71021f665c
            f.x[0] = live_u64(f.x[26]);
            f.x[8] = w(live_u32(f.x[0].wrapping_add(CLASS_INITIALIZED)));
        }
        if f.x[8] as u32 == 0 {
            // 0x71021f6838
            f.call(
                callee::CLASS_INITIALIZER,
                0x71021f683c,
                &[0, 8, 9, 10, 11, 12, 13, 19, 23],
                false,
            )?;
            f.regs.safepoint_n(0x71021f683c, &[]);
        }
        // 0x71021f6668
        f.x[0] = 0;
        f.call(
            callee::GET_INST,
            0x71021f6670,
            &[0, 8, 9, 10, 11, 12, 13, 19, 23],
            false,
        )?;
        f.x[1] = 0;
        f.call(callee::GET_VALUE, 0x71021f6678, &[1], false)?;
        f.x[8] = f.regs.x(8);
        let mut skipped = false;
        let mut debug_open = f.x[0] as u32 == 1;
        if debug_open {
            // 0x71021f6680: the acter's byte must be zero
            f.x[8] = ld64(this.wrapping_add(ACTER));
            f.x[8] = u64::from(ld8(f.x[8].wrapping_add(PARAM_BYTE)));
            debug_open = f.x[8] as u32 == 0;
        }
        if debug_open {
            // 0x71021f668c
            Self::debug_line(f, this, cells::LITERAL_5A0, 0x71021f66a4, &[0, 1, 2, 8])?;
            Self::debug_line(f, this, cells::LITERAL_588, 0x71021f66bc, &[0, 1, 2, 8])?;
            f.x[1] = ld64(this.wrapping_add(MONITOR));
            f.x[0] = 0x32;
            f.x[2] = 0;
            f.call(callee::CHECK_32, 0x71021f66cc, &[0, 1, 2], false)?;
            if f.x[0] & 1 == 0 {
                // 0x71021f6848
                Self::debug_line(f, this, cells::LITERAL_578, 0x71021f6860, &[0, 1, 2, 8])?;
                Self::debug_line(f, this, cells::LITERAL_580, 0x71021f6878, &[0, 1, 2, 8])?;
                f.x[19] = 0;
                f.x[24] = 1;
                f.regs
                    .safepoint_n(0x71021f6880, &[(19, f.x[19]), (24, f.x[24])]);
                skipped = true;
            }
        }
        if !skipped {
            f.x[24] = 0;
        }
        // 0x71021f66d4: the singleton's mode
        f.x[0] = 0;
        f.call(callee::SINGLETON, 0x71021f66dc, &[0, 8, 19, 24], false)?;
        f.x[8] = w(ld32(f.x[0].wrapping_add(SINGLETON_MODE)));
        if f.x[8] as u32 == MODE_MARK {
            // 0x71021f66e8
            f.x[8] = ld64(this.wrapping_add(TARGET));
            f.x[9] = 1;
            f.x[2] = 0;
            f.x[8] = ld64(f.x[8].wrapping_add(RESULT_LOGGER));
            prestate::store_u8(f.x[8].wrapping_add(LOGGER_FLAG_47), 1);
            Self::debug_line(f, this, cells::LITERAL_590, 0x71021f6710, &[0, 1, 2, 8, 9])?;
            f.x[19] = 0;
        }
        // 0x71021f6714: x28 = &this._target; is_barriered(target, false)
        f.x[28] = this;
        f.x[28] = f.x[28].wrapping_add(TARGET);
        f.x[0] = ld64(f.x[28]);
        f.x[1] = 0;
        f.call(
            callee::IS_BARRIERED,
            0x71021f6724,
            &[0, 1, 8, 19, 28],
            false,
        )?;
        f.x[8] = f.regs.x(8);
        if f.x[0] & 1 != 0 {
            // 0x71021f6728
            f.x[8] = ld64(f.x[28]);
            f.x[8] = ld64(f.x[8].wrapping_add(RESULT_LOGGER));
            f.x[8] = u64::from(prestate::read_u8(f.x[8].wrapping_add(LOGGER_FLAG_47)));
            if f.x[8] as u32 == 0 {
                // 0x71021f6738
                Self::debug_line(f, this, cells::LITERAL_5D0, 0x71021f6750, &[0, 1, 2, 8])?;
                f.x[8] = ld64(this.wrapping_add(TARGET));
                f.x[9] = 1;
                f.x[8] = ld64(f.x[8].wrapping_add(RESULT_LOGGER));
                prestate::store_u8(f.x[8].wrapping_add(LOGGER_FLAG_47), 1);
                f.x[8] = u64::from(prestate::read_u8(this.wrapping_add(FLAG_44)));
                if f.x[8] as u32 == 0 {
                    // 0x71021f6768
                    f.x[0] = ld64(f.x[28]);
                    f.x[1] = 0;
                    call_nested(
                        f,
                        smrpg_param_use_damage_guard,
                        callee::USE_DAMAGE_GUARD,
                        0x71021f6774,
                        &[0, 1, 8, 9],
                    )?;
                    f.x[8] = f.regs.x(8);
                    f.x[9] = f.regs.x(9);
                }
                // 0x71021f6774
                f.x[19] = 0;
                return Ok(());
            }
        }
        self.close_loop(f)
    }

    /// 0x71021f67bc: `x28 = &this._target`; a nonzero last result is stored in the target's logger.
    fn close_loop(self, f: &mut Frame) -> Result<(), GuestException> {
        use field::*;
        f.x[28] = Self::this_head(f).wrapping_add(TARGET);
        if f.x[19] as u32 != 0 {
            f.x[8] = ld64(f.x[28]);
            f.x[8] = ld64(f.x[8].wrapping_add(RESULT_LOGGER));
            prestate::store_u32(f.x[8].wrapping_add(LOGGER_VALUE), f.x[19] as u32);
        }
        Ok(())
    }

    /// `monitor.write(literal)`: `x8 = page`, `x0 = this._monitor`, `x2 = 0`, `x8 = [slot]`, `x1 = [x8]`.
    fn debug_line(
        f: &mut Frame,
        this: u64,
        slot: u64,
        return_address: u64,
        list: &[usize],
    ) -> Result<(), GuestException> {
        f.x[8] = LITERAL_PAGE;
        f.x[0] = ld64(this.wrapping_add(field::MONITOR));
        f.x[2] = 0;
        f.x[8] = live_u64(slot);
        f.x[1] = ld64(f.x[8]);
        f.call(callee::DEBUG_WRITE, return_address, list, false)
    }
}

/// The cells an arm stores or reads that the lifted body may have changed before the port runs. False when a pointer the
/// arm follows is null (the lifted body then runs alone, as everywhere in the hook).
unsafe fn arm_snapshots(arm: Arm, this: u64, target: u64, logger: u64) -> bool {
    use field::*;
    unsafe {
        let result = guest_ld64(this.wrapping_add(RESULT_OBJECT));
        match arm {
            Arm::Epilogue | Arm::StoreNine | Arm::StoreOne => {}
            Arm::ClearFlag => prestate::snapshot(logger.wrapping_add(LOGGER_MARK_40), 1, true),
            Arm::ResultByte | Arm::ResultByteGuard11 | Arm::Guard11 | Arm::Guard10 => {
                if result == 0 {
                    return false;
                }
                if matches!(arm, Arm::ResultByte | Arm::ResultByteGuard11) {
                    prestate::snapshot(result.wrapping_add(RESULT_BYTE_24), 1, true);
                }
                if arm != Arm::ResultByte {
                    // the nested flag port: its buff manager (and, for the second flag, the status manager) must exist
                    let buff = guest_ld64(target.wrapping_add(0xa0));
                    if buff == 0 || guest_ld64(target.wrapping_add(0x98)) == 0 {
                        return false;
                    }
                    let mut nested = [0u64; 32];
                    nested[0] = target;
                    if arm == Arm::Guard10 {
                        let _ = smrpg_param_flag_10_pre(nested.as_mut_ptr());
                    } else {
                        let _ = smrpg_param_flag_11_pre(nested.as_mut_ptr());
                    }
                    prestate::snapshot(result.wrapping_add(RESULT_FLAGS_38), 4, true);
                }
            }
            Arm::Half | Arm::Plain => {
                if guest_ld64(target.wrapping_add(0x28)) == 0 {
                    return false;
                }
                prestate::snapshot(logger.wrapping_add(LOGGER_VALUE_28), 4, true);
                prestate::snapshot(logger.wrapping_add(LOGGER_MODE_2C), 4, true);
                prestate::snapshot(logger.wrapping_add(LOGGER_BYTE_41), 1, true);
                prestate::snapshot(logger.wrapping_add(LOGGER_BYTE_45), 1, true);
            }
        }
    }
    true
}

/// 0x71021f6340 `SkillExecter.exec_koudou_kouka`, head only (replay row with a store journal; the pre-state hook defers every
/// call that would leave the ported part).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_skill_exec_koudou_kouka(x: *mut u64) {
    unsafe {
        let regs = Regs::from_raw(x);
        let this = regs.x(0);
        let mut frame = Frame::new(regs);
        let _ = SkillExecter::at(this).exec_koudou_head(&mut frame);
    }
}

/// Pre-state hook. Returns 1 (deferred: the lifted body runs alone) for the first run in a process (the lazy-initialization
/// flag clear), when a pointer the port follows is null, when the skill type takes the jump table, and when the type is
/// past the list's length. Otherwise it snapshots the class words, the static-field byte, the cells the port stores and
/// the nested ports' cells.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_skill_exec_koudou_kouka_pre(x: *mut u64) -> i32 {
    unsafe {
        let flag = guest_ld8(cells::FLAG);
        prestate::snapshot(cells::FLAG, 1, true);
        if flag & 1 == 0 {
            return 1;
        }
        let regs = Regs::from_raw(x);
        let this = regs.x(0);
        if this == 0 {
            return 1;
        }
        let Some(kind) = skill_type(this) else {
            return 1;
        };
        let Some(arm) = arm_of(kind) else {
            return 1;
        };
        let target = guest_ld64(this.wrapping_add(field::TARGET));
        let acter = guest_ld64(this.wrapping_add(field::ACTER));
        if target == 0 || acter == 0 {
            return 1;
        }
        let logger = guest_ld64(target.wrapping_add(field::RESULT_LOGGER));
        if logger == 0 || guest_ld64(this.wrapping_add(field::MONITOR)) == 0 {
            return 1;
        }
        // the class cells and what hangs off them
        let cell_a = guest_ld64(cells::CLASS_A_SLOT);
        let cell_b = guest_ld64(cells::CLASS_B_SLOT);
        let cell_c = guest_ld64(cells::CLASS_C_SLOT);
        if cell_a == 0 || cell_b == 0 || cell_c == 0 {
            return 1;
        }
        let class_a = guest_ld64(cell_a);
        let class_b = guest_ld64(cell_b);
        let class_c = guest_ld64(cell_c);
        if class_a == 0 || class_b == 0 || class_c == 0 {
            return 1;
        }
        let statics_a = guest_ld64(class_a.wrapping_add(field::CLASS_STATIC_FIELDS));
        if statics_a == 0 {
            return 1;
        }
        let list = guest_ld64(statics_a.wrapping_add(field::STATICS_LIST));
        if list == 0 {
            return 1;
        }
        if kind >= guest_ld32(list.wrapping_add(field::ARRAY_LENGTH)) {
            return 1;
        }
        let element = guest_ld64(
            list.wrapping_add(field::ARRAY_FIRST)
                .wrapping_add(u64::from(kind) << 3),
        );
        let statics_b = guest_ld64(class_b.wrapping_add(field::CLASS_STATIC_FIELDS));
        if element == 0 || statics_b == 0 {
            return 1;
        }
        let object_b = guest_ld64(statics_b.wrapping_add(field::STATICS_OBJECT));
        if object_b == 0 {
            return 1;
        }
        let static_flag = guest_ld64(cells::STATIC_FLAG_POINTER);
        if static_flag == 0 {
            return 1;
        }
        for slot in cells::LITERALS {
            if guest_ld64(slot) == 0 {
                return 1;
            }
        }
        for class in [class_a, class_b, class_c] {
            prestate::snapshot(class.wrapping_add(field::CLASS_INITIALIZED), 4, false);
        }
        prestate::snapshot(static_flag, 1, true);
        prestate::snapshot(logger.wrapping_add(field::LOGGER_FLAG_47), 1, true);
        prestate::snapshot(logger.wrapping_add(field::LOGGER_VALUE), 4, true);
        prestate::snapshot(this.wrapping_add(field::FLAG_44), 1, arm == Arm::ClearFlag);
        if !arm_snapshots(arm, this, target, logger) {
            return 1;
        }
        // the nested use_damage_guard the loop may reach
        let mut nested = [0u64; 32];
        nested[0] = target;
        let _ = smrpg_param_use_damage_guard_pre(nested.as_mut_ptr());
        0
    }
}

#[cfg(test)]
mod tests;
