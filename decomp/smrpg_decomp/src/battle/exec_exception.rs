//! `Battle.SkillExecter.skill_calc_exception` (0x71021f8040), the second callee of `exec_each_targets` ported (task-64).
//!
//! In C# terms (names are the IL2CPP ones; the cells and callees the metadata dump does not name are named by what
//! they do):
//!
//! ```text
//! void SkillExecter.skill_calc_exception() {
//!     int cell = 0;                                         // a stack int the debug line passes by reference
//!     BattleManager <class init, static field metadata>;    // lazy
//!     var manager = BattleManager._instance;
//!     switch (_skill_obj._skill_id) {
//!     case 0x67:                                            // a counter in GlobalWork, a debug line
//!         var gw = GlobalWork.get_inst();
//!         var n = GlobalWork.get_inst().<get>(0);
//!         gw.<set>(n + 1 clamped to 250, null);
//!         int v = _tmp_value;
//!         _tmp_value = v + GlobalWork.get_inst().<get>(0) / 2;   // the half rounds toward zero
//!         cell = GlobalWork.get_inst().<get>(0);
//!         _monitor.write(<literal> + cell.ToString());
//!         break;
//!     case 0x25d:
//!         if (_acter._character_id != 0x657) <call>(_acter, 0x5f3);
//!         break;
//!     }
//!     if (_target._character_id != 0x5ed) return;
//!     if (manager.<+0x60>.<+0x24> != 2) return;
//!     if (<check>(manager.<+0x170>.<+0x68>) > 0) return;
//!     switch (_skill_obj._calc_type) {                      // 1, 2 and 4 test the element ratio, 12 a range check
//!     case 1: case 2: case 4: if (calc_element(_target, _skill_obj._element) == 0.0f) return; break;
//!     case 12: if (!in_range(_skill_obj._<+0x28>, 1, 0x49)) return; break;
//!     default: return;
//!     }
//!     _monitor.write(<literal>);
//!     manager.<+0x60>.<+0x20> = true;
//!     <target call>(); <target call>();
//!     _target._result_logger.<+0x41> = true;
//! }
//! ```
//!
//! The function keeps the lifted register residue (see `Frame`). Its first run of a process (the lazy-initialization flag
//! byte clear: the class cells are still tokens) is deferred in `ab` mode like its siblings'; the port runs it in `on` mode.

use super::damage::SkillExecter;
use super::element::smrpg_param_calc_element;
use super::frame::Frame;
use super::{GuestException, GuestObject, Regs};
use crate::bridge::prestate;
use crate::{guest_ld8, guest_ld32, guest_ld64};

const FLAG_PAGE: u64 = 0x7103cba000;

mod callee {
    pub const INITIALIZE_METADATA: u64 = 0x71000c4150;
    pub const CLASS_INITIALIZER: u64 = 0x71000c4330;
    pub const GET_INST: u64 = 0x7101886a00;
    pub const GW_GET: u64 = 0x7101887b40;
    pub const GW_SET: u64 = 0x7101887b50;
    pub const DEBUG_WRITE: u64 = 0x71018bebc0;
    pub const INT32_TO_STRING: u64 = 0x710213a840;
    pub const STRING_CONCAT_2: u64 = 0x71020147d0;
    pub const IN_RANGE: u64 = 0x71018ba2c0;
    pub const CLASS_CALL: u64 = 0x71021ba580;
    pub const MANAGER_CHECK: u64 = 0x7101bf43d0;
    pub const TARGET_CALL_A: u64 = 0x71021bde90;
    pub const TARGET_CALL_B: u64 = 0x71021bddd0;
}

mod cells {
    pub const FLAG: u64 = super::FLAG_PAGE + 0xd60;
    /// Slot (data page + offset) holding the `BattleManager` class cell.
    pub const CLASS_SLOT: u64 = 0x7103a57000 + 0xe48;
    /// Slot holding the pointer to a second class cell (the class `GlobalWork` lives in).
    pub const GLOBAL_SLOT: u64 = 0x7103a58000 + 0x48;
    pub const LITERAL_SLOT: u64 = 0x7103a83000 + 0x640;
    pub const DEBUG_SLOT: u64 = 0x7103a82000 + 0xb08;
    /// Pointer to the byte that says the static-field metadata of this method has been initialized.
    pub const STATIC_FLAG_POINTER: u64 = 0x7103a58000 + 0x1d0;
}

mod field {
    pub const MONITOR: u64 = 0x10;
    pub const ACTER: u64 = 0x20;
    pub const TARGET: u64 = 0x28;
    pub const SKILL_OBJ: u64 = 0x30;
    pub const TMP_VALUE: u64 = 0x40;
    pub const SKILL_ID: u64 = 0x14;
    pub const CALC_TYPE: u64 = 0x24;
    pub const SKILL_FIELD_28: u64 = 0x28;
    pub const ELEMENT: u64 = 0x38;
    pub const CHARACTER_ID: u64 = 0x20;
    pub const CLASS_INITIALIZED: u64 = 0xe0;
    pub const CLASS_STATIC_FIELDS: u64 = 0xb8;
    pub const STATICS_INSTANCE: u64 = 0x10;
    pub const INSTANCE_OBJECT: u64 = 0x60;
    pub const INSTANCE_OBJECT_STATE: u64 = 0x24;
    pub const INSTANCE_OBJECT_FLAG: u64 = 0x20;
    pub const INSTANCE_CHECKER: u64 = 0x170;
    pub const CHECKER_ARG: u64 = 0x68;
    pub const RESULT_LOGGER: u64 = 0xb8;
    pub const LOGGER_FLAG: u64 = 0x41;
}

mod value {
    pub const SKILL_COUNTER: u32 = 0x67;
    pub const SKILL_CLASS_CALL: u32 = 0x25d;
    pub const ACTER_SKIP: u32 = 0x657;
    pub const CLASS_CALL_ARG: u64 = 0x5f3;
    pub const TARGET_ID: u32 = 0x5ed;
    pub const OBJECT_STATE: u32 = 2;
    pub const COUNTER_LIMIT: i32 = 0xfa;
    pub const RANGE_LOW: u64 = 1;
    pub const RANGE_HIGH: u64 = 0x49;
}

mod site {
    pub const INIT_0: u64 = 0x71021f8078;
    pub const INIT_1: u64 = 0x71021f8084;
    pub const INIT_2: u64 = 0x71021f8090;
    pub const INIT_3: u64 = 0x71021f809c;
    pub const CLASS_INIT_A: u64 = 0x71021f80c0;
    pub const GET_INST_A: u64 = 0x71021f80c8;
    pub const CLASS_INIT_B: u64 = 0x71021f8120;
    pub const GET_INST_B: u64 = 0x71021f8128;
    pub const GET_INST_C: u64 = 0x71021f8134;
    pub const GW_GET_A: u64 = 0x71021f813c;
    pub const GW_SET: u64 = 0x71021f8158;
    pub const GET_INST_D: u64 = 0x71021f8164;
    pub const GW_GET_B: u64 = 0x71021f816c;
    pub const GET_INST_E: u64 = 0x71021f8188;
    pub const GW_GET_C: u64 = 0x71021f8190;
    pub const TO_STRING: u64 = 0x71021f81a0;
    pub const CONCAT: u64 = 0x71021f81b8;
    pub const DEBUG_COUNTER: u64 = 0x71021f81c8;
    pub const CLASS_INIT_C: u64 = 0x71021f81d0;
    pub const CLASS_INIT_C_POLL: u64 = 0x71021f81dc;
    pub const METADATA_D: u64 = 0x71021f81ec;
    pub const METADATA_D_POLL: u64 = 0x71021f81fc;
    pub const CLASS_INIT_D: u64 = 0x71021f8204;
    pub const CLASS_INIT_D_POLL: u64 = 0x71021f8208;
    pub const CLASS_CALL: u64 = 0x71021f8228;
    pub const MANAGER_CHECK: u64 = 0x71021f8258;
    pub const CALC_TYPE_POLL: u64 = 0x71021f8294;
    pub const CALC_ELEMENT: u64 = 0x71021f82a8;
    pub const ELEMENT_POLL: u64 = 0x71021f82ac;
    pub const IN_RANGE: u64 = 0x71021f82c8;
    pub const DEBUG_EXCEPTION: u64 = 0x71021f82e4;
    pub const TARGET_CALL_A: u64 = 0x71021f82fc;
    pub const TARGET_CALL_B: u64 = 0x71021f8308;
    pub const END_POLL: u64 = 0x71021f8314;
}

/// Where the code goes after the `BattleManager` class checks.
enum Check {
    /// 0x80d4: the static-field flag byte, then the class word again.
    StaticFlag,
    /// 0x80e4: the class word again.
    ClassWord,
    /// 0x81cc: the class word was zero, run the class initializer first.
    ClassInit,
    /// 0x81e0: the static-field metadata is not resolved yet.
    Metadata,
    /// 0x8200: the class word is still zero.
    ClassInitAgain,
}

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

fn w(value: u32) -> u64 {
    u64::from(value)
}

impl SkillExecter {
    /// `skill_calc_exception()`. `f` is the register file at entry.
    pub fn skill_calc_exception(self, f: &mut Frame) -> Result<(), GuestException> {
        use field::*;
        // prologue: `sub sp, sp, #0x40`, saves x29 to x19, `add x29, sp, #0x10`
        f.x[31] = f.entry[31].wrapping_sub(0x40);
        f.x[29] = f.x[31].wrapping_add(0x10);
        let cell = f.x[29].wrapping_sub(4);
        f.x[21] = FLAG_PAGE;
        f.x[20] = 0x7103a58000;
        f.x[8] = u64::from(prestate::read_u8(cells::FLAG));
        f.x[20] = live_u64(cells::GLOBAL_SLOT);
        f.x[19] = f.x[0];
        if f.x[8] & 1 == 0 {
            f.x[0] = prestate::read_u64(cells::CLASS_SLOT);
            f.call(
                callee::INITIALIZE_METADATA,
                site::INIT_0,
                &[0, 8, 19, 20, 21, 29],
                true,
            )?;
            f.x[0] = prestate::read_u64(cells::GLOBAL_SLOT);
            f.call(callee::INITIALIZE_METADATA, site::INIT_1, &[0], false)?;
            f.x[0] = prestate::read_u64(cells::LITERAL_SLOT);
            f.call(callee::INITIALIZE_METADATA, site::INIT_2, &[0], false)?;
            f.x[0] = prestate::read_u64(cells::DEBUG_SLOT);
            f.call(callee::INITIALIZE_METADATA, site::INIT_3, &[0], false)?;
            f.x[8] = 1;
            prestate::store_u8(cells::FLAG, 1);
        }
        f.x[21] = live_u64(cells::CLASS_SLOT);
        f.x[0] = live_u64(f.x[20]);
        prestate::store_u32(cell, 0);
        f.x[8] = w(prestate::read_u32(f.x[0].wrapping_add(CLASS_INITIALIZED)));
        if f.x[8] as u32 == 0 {
            f.call(
                callee::CLASS_INITIALIZER,
                site::CLASS_INIT_A,
                &[0, 8, 19, 20, 21, 29],
                true,
            )?;
        }
        // a call whose result is dropped (the next instruction overwrites x0)
        f.x[0] = 0;
        f.call(
            callee::GET_INST,
            site::GET_INST_A,
            &[0, 8, 19, 20, 21, 29],
            true,
        )?;
        f.x[0] = live_u64(f.x[21]);
        f.x[8] = w(prestate::read_u32(f.x[0].wrapping_add(CLASS_INITIALIZED)));
        let mut check = if f.x[8] as u32 == 0 {
            Check::ClassInit
        } else {
            Check::StaticFlag
        };
        loop {
            match check {
                Check::StaticFlag => {
                    f.x[22] = live_u64(cells::STATIC_FLAG_POINTER);
                    f.x[8] = u64::from(prestate::read_u8(f.x[22]));
                    check = if f.x[8] as u32 == 0 {
                        Check::Metadata
                    } else {
                        Check::ClassWord
                    };
                }
                Check::ClassWord => {
                    f.x[0] = live_u64(f.x[21]);
                    f.x[8] = w(live_u32(f.x[0].wrapping_add(CLASS_INITIALIZED)));
                    if f.x[8] as u32 == 0 {
                        check = Check::ClassInitAgain;
                    } else {
                        break;
                    }
                }
                Check::ClassInit => {
                    f.call(
                        callee::CLASS_INITIALIZER,
                        site::CLASS_INIT_C,
                        &[0, 8],
                        false,
                    )?;
                    f.x[22] = live_u64(cells::STATIC_FLAG_POINTER);
                    f.x[8] = u64::from(prestate::read_u8(f.x[22]));
                    f.regs
                        .safepoint_n(site::CLASS_INIT_C_POLL, &[(8, f.x[8]), (22, f.x[22])]);
                    check = if f.x[8] as u32 != 0 {
                        Check::ClassWord
                    } else {
                        Check::Metadata
                    };
                }
                Check::Metadata => {
                    f.x[0] = prestate::read_u64(cells::CLASS_SLOT);
                    f.call(
                        callee::INITIALIZE_METADATA,
                        site::METADATA_D,
                        &[0, 8, 22],
                        false,
                    )?;
                    f.x[8] = 1;
                    prestate::store_u8(f.x[22], 1);
                    f.x[0] = live_u64(f.x[21]);
                    f.x[8] = w(live_u32(f.x[0].wrapping_add(CLASS_INITIALIZED)));
                    f.regs
                        .safepoint_n(site::METADATA_D_POLL, &[(0, f.x[0]), (8, f.x[8])]);
                    if f.x[8] as u32 != 0 {
                        break;
                    }
                    check = Check::ClassInitAgain;
                }
                Check::ClassInitAgain => {
                    f.call(
                        callee::CLASS_INITIALIZER,
                        site::CLASS_INIT_D,
                        &[0, 8, 22],
                        false,
                    )?;
                    f.x[0] = live_u64(f.x[21]);
                    f.regs.safepoint_n(site::CLASS_INIT_D_POLL, &[(0, f.x[0])]);
                    break;
                }
            }
        }
        // the manager instance and the skill id
        f.x[8] = unsafe { guest_ld64(f.x[0].wrapping_add(CLASS_STATIC_FIELDS)) };
        f.x[21] = unsafe { guest_ld64(f.x[8].wrapping_add(STATICS_INSTANCE)) };
        f.x[8] = unsafe { guest_ld64(f.x[19].wrapping_add(SKILL_OBJ)) };
        f.x[8] = w(unsafe { guest_ld32(f.x[8].wrapping_add(SKILL_ID)) });
        let skill_id = f.x[8] as u32;
        if skill_id == value::SKILL_CLASS_CALL {
            f.x[0] = unsafe { guest_ld64(f.x[19].wrapping_add(ACTER)) };
            f.x[8] = w(unsafe { guest_ld32(f.x[0].wrapping_add(CHARACTER_ID)) });
            if f.x[8] as u32 != value::ACTER_SKIP {
                f.x[1] = value::CLASS_CALL_ARG;
                f.x[2] = 0;
                f.call(
                    callee::CLASS_CALL,
                    site::CLASS_CALL,
                    &[0, 1, 2, 8, 21, 22],
                    false,
                )?;
            }
        } else if skill_id == value::SKILL_COUNTER {
            self.counter_arm(f, cell)?;
        }
        // the target and the manager state
        f.x[8] = unsafe { guest_ld64(f.x[19].wrapping_add(TARGET)) };
        f.x[8] = w(unsafe { guest_ld32(f.x[8].wrapping_add(CHARACTER_ID)) });
        if f.x[8] as u32 != value::TARGET_ID {
            return self.leave_exception(f);
        }
        f.x[8] = unsafe { guest_ld64(f.x[21].wrapping_add(INSTANCE_OBJECT)) };
        f.x[8] = w(unsafe { guest_ld32(f.x[8].wrapping_add(INSTANCE_OBJECT_STATE)) });
        if f.x[8] as u32 != value::OBJECT_STATE {
            return self.leave_exception(f);
        }
        f.x[8] = unsafe { guest_ld64(f.x[21].wrapping_add(INSTANCE_CHECKER)) };
        f.x[1] = 0;
        f.x[0] = unsafe { guest_ld64(f.x[8].wrapping_add(CHECKER_ARG)) };
        f.call(
            callee::MANAGER_CHECK,
            site::MANAGER_CHECK,
            &[0, 1, 8, 21, 22],
            false,
        )?;
        if (f.x[0] as u32 as i32) > 0 {
            return self.leave_exception(f);
        }
        // the skill's calculation type decides which test must pass
        f.x[8] = unsafe { guest_ld64(f.x[19].wrapping_add(SKILL_OBJ)) };
        f.x[9] = w(unsafe { guest_ld32(f.x[8].wrapping_add(CALC_TYPE)) });
        f.x[10] = w((f.x[9] as u32).wrapping_sub(1));
        let by_element = (f.x[10] as u32) < 2;
        if !by_element {
            if f.x[9] as u32 == 0xc {
                f.x[0] = w(unsafe { guest_ld32(f.x[8].wrapping_add(SKILL_FIELD_28)) });
                f.x[2] = value::RANGE_HIGH;
                f.x[1] = value::RANGE_LOW;
                f.x[3] = 0;
                f.call(
                    callee::IN_RANGE,
                    site::IN_RANGE,
                    &[0, 1, 2, 3, 8, 9, 10],
                    false,
                )?;
                f.x[8] = f.regs.x(8);
                f.x[9] = f.regs.x(9);
                f.regs.safepoint_n(site::IN_RANGE, &[]);
                if f.x[0] & 1 == 0 {
                    return self.leave_exception(f);
                }
            } else {
                f.regs.safepoint_n(
                    site::CALC_TYPE_POLL,
                    &[(8, f.x[8]), (9, f.x[9]), (10, f.x[10])],
                );
                if f.x[9] as u32 != 4 {
                    return self.leave_exception(f);
                }
                if !self.element_is_set(f)? {
                    return self.leave_exception(f);
                }
            }
        } else if !self.element_is_set(f)? {
            return self.leave_exception(f);
        }
        // the exception applies: a debug line, a flag in the manager, two calls on the target, a flag in its result logger
        f.x[8] = 0x7103a82000;
        f.x[0] = unsafe { guest_ld64(f.x[19].wrapping_add(MONITOR)) };
        f.x[2] = 0;
        f.x[8] = live_u64(f.x[8].wrapping_add(0xb08));
        f.x[1] = live_u64(f.x[8]);
        f.call(
            callee::DEBUG_WRITE,
            site::DEBUG_EXCEPTION,
            &[0, 1, 2, 8],
            false,
        )?;
        f.x[8] = unsafe { guest_ld64(f.x[21].wrapping_add(INSTANCE_OBJECT)) };
        f.x[20] = 1;
        prestate::store_u8(f.x[8].wrapping_add(INSTANCE_OBJECT_FLAG), 1);
        f.x[0] = unsafe { guest_ld64(f.x[19].wrapping_add(TARGET)) };
        f.x[1] = 0;
        f.call(
            callee::TARGET_CALL_A,
            site::TARGET_CALL_A,
            &[0, 1, 8, 20],
            false,
        )?;
        f.x[0] = unsafe { guest_ld64(f.x[19].wrapping_add(TARGET)) };
        f.x[1] = 0;
        f.call(callee::TARGET_CALL_B, site::TARGET_CALL_B, &[0, 1], false)?;
        f.x[8] = unsafe { guest_ld64(f.x[19].wrapping_add(TARGET)) };
        f.x[8] = unsafe { guest_ld64(f.x[8].wrapping_add(RESULT_LOGGER)) };
        prestate::store_u8(f.x[8].wrapping_add(LOGGER_FLAG), f.x[20] as u8);
        f.regs.safepoint_n(site::END_POLL, &[(8, f.x[8])]);
        self.leave_exception(f)
    }

    /// `calc_element(_target, element) != 0.0f` for calculation types 1, 2 and 4 (x8 = the skill object).
    fn element_is_set(self, f: &mut Frame) -> Result<bool, GuestException> {
        use field::*;
        f.x[0] = unsafe { guest_ld64(f.x[19].wrapping_add(TARGET)) };
        f.x[2] = 0;
        f.x[1] = w(unsafe { guest_ld32(f.x[8].wrapping_add(ELEMENT)) });
        f.call_port(
            smrpg_param_calc_element,
            site::CALC_ELEMENT,
            &[0, 1, 2, 8, 9, 10],
            false,
        )?;
        let ratio = f32::from_bits(f.v[0].0 as u32);
        let zero = f.regs.f32_compare(ratio, 0.0).z;
        f.regs.safepoint_n(site::ELEMENT_POLL, &[]);
        Ok(!zero)
    }

    /// Skill 0x67: bump a `GlobalWork` counter (at most 250), add half of another to `_tmp_value`, write a debug line.
    fn counter_arm(self, f: &mut Frame, cell: u64) -> Result<(), GuestException> {
        use field::*;
        f.x[0] = live_u64(f.x[20]);
        f.x[8] = w(prestate::read_u32(f.x[0].wrapping_add(CLASS_INITIALIZED)));
        if f.x[8] as u32 == 0 {
            f.call(
                callee::CLASS_INITIALIZER,
                site::CLASS_INIT_B,
                &[0, 8, 21, 22],
                false,
            )?;
        }
        f.x[0] = 0;
        f.call(callee::GET_INST, site::GET_INST_B, &[0, 8, 21, 22], false)?;
        f.x[20] = f.x[0];
        f.x[0] = 0;
        f.call(callee::GET_INST, site::GET_INST_C, &[0, 20], false)?;
        f.x[1] = 0;
        f.call(callee::GW_GET, site::GW_GET_A, &[1], false)?;
        f.x[8] = f.regs.x(8);
        // `min(n + 1, 250)`: the compare of the sum against the limit picks the limit (signed)
        let sum = (f.x[0] as u32).wrapping_add(1);
        f.x[8] = w(sum);
        let at_limit = (sum as i32) >= value::COUNTER_LIMIT;
        f.x[8] = value::COUNTER_LIMIT as u64;
        f.x[1] = if at_limit { f.x[8] } else { w(sum) };
        f.x[0] = f.x[20];
        f.x[2] = 0;
        f.call(callee::GW_SET, site::GW_SET, &[0, 1, 2, 8], false)?;
        f.x[8] = f.regs.x(8);
        f.x[20] = w(prestate::read_u32(f.x[19].wrapping_add(TMP_VALUE)));
        f.x[0] = 0;
        f.call(callee::GET_INST, site::GET_INST_D, &[0, 20], false)?;
        f.x[1] = 0;
        f.call(callee::GW_GET, site::GW_GET_B, &[1], false)?;
        f.x[8] = f.regs.x(8);
        // half of the value, rounded toward zero: `(v >= 0 ? v : v + 1) >> 1` (arithmetic)
        let v = f.x[0] as u32 as i32;
        let adjusted = if v >= 0 { v } else { v.wrapping_add(1) };
        f.x[8] = w(adjusted as u32);
        f.x[0] = 0;
        f.x[8] = w((f.x[20] as u32).wrapping_add((adjusted >> 1) as u32));
        f.x[20] = unsafe { guest_ld64(f.x[19].wrapping_add(MONITOR)) };
        prestate::store_u32(f.x[19].wrapping_add(TMP_VALUE), f.x[8] as u32);
        f.call(callee::GET_INST, site::GET_INST_E, &[0, 8, 20], false)?;
        f.x[1] = 0;
        f.call(callee::GW_GET, site::GW_GET_C, &[1], false)?;
        f.x[8] = f.regs.x(8);
        prestate::store_u32(cell, f.x[0] as u32);
        f.x[0] = cell;
        f.x[1] = 0;
        let mut area = prestate::read_u32(cell).to_le_bytes();
        f.call_out(
            callee::INT32_TO_STRING,
            site::TO_STRING,
            &[0, 1],
            false,
            &mut area,
        )?;
        f.x[8] = 0x7103a83000;
        f.x[8] = live_u64(f.x[8].wrapping_add(0x640));
        f.x[1] = f.x[0];
        f.x[0] = unsafe { guest_ld64(f.x[8]) };
        f.x[2] = 0;
        f.call(callee::STRING_CONCAT_2, site::CONCAT, &[0, 1, 2, 8], false)?;
        f.x[1] = f.x[0];
        f.x[0] = f.x[20];
        f.x[2] = 0;
        f.call(callee::DEBUG_WRITE, site::DEBUG_COUNTER, &[0, 1, 2], false)?;
        Ok(())
    }

    /// The epilogue shared by every exit: x19 to x22 and x29 and the sp come back; x0 is flushed, x30 is not.
    fn leave_exception(self, f: &mut Frame) -> Result<(), GuestException> {
        f.leave(&[0, 19, 20, 21, 22, 29]);
        Ok(())
    }
}

/// 0x71021f8040 `SkillExecter.skill_calc_exception` (replay row with a store journal).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_skill_calc_exception(x: *mut u64) {
    unsafe {
        let regs = Regs::from_raw(x);
        let this = regs.x(0);
        let mut frame = Frame::new(regs);
        let _ = SkillExecter::at(this).skill_calc_exception(&mut frame);
    }
}

/// Pre-state hook. Returns 1 for the first run in a process (the flag byte clear: the class cells are still tokens).
/// Otherwise the cells the port reads after callees may have changed (class words, the static-field flag byte,
/// `_tmp_value`) and the cells it stores to.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_skill_calc_exception_pre(x: *mut u64) -> i32 {
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
        let global_cell = guest_ld64(cells::GLOBAL_SLOT);
        if global_cell != 0 {
            let class = guest_ld64(global_cell);
            prestate::snapshot(class.wrapping_add(field::CLASS_INITIALIZED), 4, false);
        }
        let class_cell = guest_ld64(cells::CLASS_SLOT);
        let class = guest_ld64(class_cell);
        let word = class.wrapping_add(field::CLASS_INITIALIZED);
        prestate::snapshot(word, 4, false);
        let flag_cell = guest_ld64(cells::STATIC_FLAG_POINTER);
        prestate::snapshot(flag_cell, 1, true);
        prestate::snapshot(this.wrapping_add(field::TMP_VALUE), 4, true);
        let target = guest_ld64(this.wrapping_add(field::TARGET));
        if target != 0 {
            let logger = guest_ld64(target.wrapping_add(field::RESULT_LOGGER));
            if logger != 0 {
                prestate::snapshot(logger.wrapping_add(field::LOGGER_FLAG), 1, true);
            }
        }
        if guest_ld32(word) != 0 {
            let statics = guest_ld64(class.wrapping_add(field::CLASS_STATIC_FIELDS));
            if statics != 0 {
                let instance = guest_ld64(statics.wrapping_add(field::STATICS_INSTANCE));
                if instance != 0 {
                    let object = guest_ld64(instance.wrapping_add(field::INSTANCE_OBJECT));
                    if object != 0 {
                        prestate::snapshot(
                            object.wrapping_add(field::INSTANCE_OBJECT_FLAG),
                            1,
                            true,
                        );
                    }
                }
            }
        }
        0
    }
}

#[cfg(test)]
mod tests;
