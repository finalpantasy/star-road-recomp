//! `Battle.SkillExecter.exec_calc_type` (0x71021fee50), the fourth callee of `exec_each_targets` ported (task-66).
//!
//! In C# terms (names are the IL2CPP ones; the cells and callees the metadata dump does not name are named by what
//! they do):
//!
//! ```text
//! void SkillExecter.exec_calc_type() {
//!     int a = 0, b = 0;                                     // two stack ints the debug lines pass by reference
//!     <lazy metadata initialization: 6 slots>
//!     <GlobalWork class init>; var singleton = GlobalWork.get_inst();
//!     switch (_skill_obj._calc_type) {                      // a compiled jump table, 0 to 13, anything else returns
//!     case 8: case 9: case 10:                              // physical: attack against defence
//!         _flag_44 = _flag_46 = true;
//!         a = _acter.get_calc_attack(); b = _target.get_calc_defence();
//!         <debug lines for a and b>;  _tmp_value = a - b;
//!         break;
//!     case 0:                                               // skill 0x286 against another character: logger fields, a debug line
//!         if (_skill_obj._skill_id != 0x286 || _acter._character_id == _target._character_id) return;
//!         _target._result_logger.<+0x28> = _skill_obj.<+0x2c>; _target._result_logger.<+0x2c> = 2;
//!         <debug line for _skill_obj.<+0x2c>>;
//!         break;
//!     case 1:  same as 8 to 10, then add 4 to a counter object of the BattleManager (unless the acter's byte <+0x6c> is 0);
//!     case 2:  magic: a = _skill_obj.<+0x2c> + _acter.get_calc_magic_attack(); b = _target.get_calc_magic_defence();
//!              _tmp_value = a - b, then add 2 to the same counter object (unless the byte is 0);
//!     case 3:  _flag_45 = true; a = _skill_obj.<+0x2c> + _acter.get_calc_magic_attack(); <debug line>; _tmp_value = a;
//!     case 4:  _flag_44 = true; a = _skill_obj.<+0x2c>; <debug line>; _tmp_value = a;
//!              then add the acter's byte <+0x6c> to the same counter object;
//!     case 5:  _flag_45 = true; a = _skill_obj.<+0x2c>; <debug line>; _tmp_value = a;
//!     case 6, 7: a result object (<+0x18>) is flagged and gets two values from GlobalWork getters; case 6 then sets the
//!              target's logger fields from _skill_obj.<+0x2c>, case 7 reads one more value; both write a debug line and _tmp_value;
//!     case 11: nothing;  case 12: <call>(this) and then case 13; case 13: <class init>; <call>(_skill_obj._skill_id).
//!     }
//! }
//! ```
//!
//! The function keeps the lifted register residue (see `Frame`). Its first run of a process (the lazy-initialization flag
//! byte clear: the class cells are still tokens) is deferred in `ab` mode like its siblings'; the port runs it in `on` mode.
//! The four stat getters (`get_calc_attack`, `get_calc_defence`, `get_calc_magic_attack`, `get_calc_magic_defence`) are called
//! through their own shims (`Frame::call_port`): in a replay trial they are not recorded, so their traced callees belong to this
//! call's recording and the port makes the same bridge calls in the same order. The pre-state hook calls the attack hooks.

use super::attack::{
    smrpg_param_calc_attack, smrpg_param_calc_attack_pre, smrpg_param_calc_magic_attack,
    smrpg_param_calc_magic_attack_pre,
};
use super::damage::SkillExecter;
use super::frame::Frame;
use super::{
    GuestException, GuestObject, Regs, smrpg_param_calc_defence, smrpg_param_calc_magic_defence,
};
use crate::bridge::prestate;
use crate::{guest_ld8, guest_ld32, guest_ld64};

const FLAG_PAGE: u64 = 0x7103cba000;
const LITERAL_PAGE: u64 = 0x7103a83000;

mod callee {
    pub const INITIALIZE_METADATA: u64 = 0x71000c4150;
    pub const CLASS_INITIALIZER: u64 = 0x71000c4330;
    pub const GET_INST: u64 = 0x7101886a00;
    pub const CALC_ATTACK: u64 = 0x71021c3ea0;
    pub const CALC_DEFENCE: u64 = 0x71021bc270;
    pub const CALC_MAGIC_ATTACK: u64 = 0x71021bc2e0;
    pub const CALC_MAGIC_DEFENCE: u64 = 0x71021bc440;
    pub const INT32_TO_STRING: u64 = 0x710213a840;
    pub const STRING_CONCAT_2: u64 = 0x71020147d0;
    pub const DEBUG_WRITE: u64 = 0x71018bebc0;
    pub const GETTER_A: u64 = 0x7101887ac0;
    pub const GETTER_B: u64 = 0x7101887b00;
    pub const SETTER_C: u64 = 0x710188bf40;
    pub const OBJECT_GETTER: u64 = 0x71021b7fb0;
    pub const PREPARE: u64 = 0x7102200150;
    pub const SKILL_CALL: u64 = 0x710188dd30;
}

mod cells {
    pub const FLAG: u64 = super::FLAG_PAGE + 0xd6a;
    /// Slot (data page + offset) holding the `BattleManager` class cell.
    pub const CLASS_SLOT: u64 = 0x7103a57000 + 0xe48;
    /// Slot holding the pointer to the second class cell (the class `GlobalWork` lives in).
    pub const GLOBAL_SLOT: u64 = 0x7103a58000 + 0x48;
    /// Pointer to the byte that says the static-field metadata of this method has been initialized.
    pub const STATIC_FLAG_POINTER: u64 = 0x7103a58000 + 0x1d0;
    /// The four literal slots of the debug lines (data page 0x7103a83000 + offset) and the two the init arm loads first.
    pub const LITERALS: [u64; 4] = [
        super::LITERAL_PAGE + 0x598,
        super::LITERAL_PAGE + 0x5a8,
        super::LITERAL_PAGE + 0x5b0,
        super::LITERAL_PAGE + 0x5b8,
    ];
}

mod field {
    pub const MONITOR: u64 = 0x10;
    pub const OBJ18: u64 = 0x18;
    pub const ACTER: u64 = 0x20;
    pub const TARGET: u64 = 0x28;
    pub const SKILL_OBJ: u64 = 0x30;
    pub const TMP_VALUE: u64 = 0x40;
    pub const FLAG_44: u64 = 0x44;
    pub const FLAG_45: u64 = 0x45;
    pub const FLAG_46: u64 = 0x46;
    pub const SKILL_ID: u64 = 0x14;
    pub const CALC_TYPE: u64 = 0x24;
    pub const SKILL_VALUE: u64 = 0x2c;
    pub const CHARACTER_ID: u64 = 0x20;
    pub const PARAM_BYTE: u64 = 0x6c;
    pub const RESULT_LOGGER: u64 = 0xb8;
    pub const LOGGER_A: u64 = 0x28;
    pub const LOGGER_B: u64 = 0x2c;
    pub const LOGGER_FLAG: u64 = 0x41;
    pub const OBJ18_FLAG: u64 = 0x23;
    pub const OBJ18_A: u64 = 0x40;
    pub const OBJ18_B: u64 = 0x44;
    pub const CLASS_INITIALIZED: u64 = 0xe0;
    pub const CLASS_STATIC_FIELDS: u64 = 0xb8;
    pub const STATICS_INSTANCE: u64 = 0x10;
    pub const INSTANCE_OBJECT: u64 = 0x60;
    pub const COUNTER: u64 = 0x1c;
}

mod value {
    pub const SKILL_SPECIAL: u32 = 0x286;
    pub const MAX_CALC_TYPE: u32 = 13;
}

/// The compiled switch is `br x10` with `x10 = 0x71021fef1c + 4 * table[calc_type]`; the halfword table is at 0x7102b050a4.
const JUMP_TABLE: u64 = 0x7102b050a4;
const JUMP_BASE: u64 = 0x71021fef1c;
const JUMP_TABLE_ENTRIES: [u16; 14] = [43, 146, 209, 75, 84, 128, 277, 321, 0, 0, 0, 355, 134, 136];

mod arm {
    pub const SKILL_286: u64 = 0x71021fefc8;
    pub const PHYSICAL_COUNTER: u64 = 0x71021ff164;
    pub const MAGIC_COUNTER: u64 = 0x71021ff260;
    pub const MAGIC_SUM: u64 = 0x71021ff048;
    pub const SUM_COUNTER: u64 = 0x71021ff06c;
    pub const VALUE_ONLY: u64 = 0x71021ff11c;
    pub const RESULT_A: u64 = 0x71021ff370;
    pub const RESULT_B: u64 = 0x71021ff420;
    pub const PHYSICAL: u64 = 0x71021fef1c;
    pub const NOTHING: u64 = 0x71021ff4a8;
    pub const PREPARE_THEN_CALL: u64 = 0x71021ff134;
    pub const CALL: u64 = 0x71021ff13c;
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

fn ld64(address: u64) -> u64 {
    unsafe { guest_ld64(address) }
}

fn ld32(address: u64) -> u32 {
    unsafe { guest_ld32(address) }
}

fn ld8(address: u64) -> u8 {
    unsafe { guest_ld8(address) }
}

/// The four stat getters go through their own shims; the unit tests install fakes on the guest addresses instead.
#[cfg(not(test))]
fn call_stat(
    f: &mut Frame,
    port: unsafe extern "C" fn(*mut u64),
    _target: u64,
    return_address: u64,
    list: &[usize],
) -> Result<(), GuestException> {
    f.call_port(port, return_address, list, false)
}

#[cfg(test)]
fn call_stat(
    f: &mut Frame,
    _port: unsafe extern "C" fn(*mut u64),
    target: u64,
    return_address: u64,
    list: &[usize],
) -> Result<(), GuestException> {
    f.call(target, return_address, list, false)
}

/// Where the code goes after the `BattleManager` class checks of the three arms that end in the counter object, and the
/// call sites of each (the lifted code has one copy of the checks per arm, each with its own flush lists).
struct Tail {
    class_init_first: (u64, &'static [usize], u64),
    metadata: (u64, &'static [usize], u64),
    /// A second poll the arm that jumps backwards has after the metadata block.
    metadata_loop_poll: Option<u64>,
    class_init_again: (u64, &'static [usize], Option<u64>),
    kind: CounterKind,
}

#[derive(Clone, Copy)]
enum CounterKind {
    /// Add the acter's byte.
    AddByte,
    /// Add the constant unless the acter's byte is zero.
    AddIfByte(u32),
}

const TAIL_SUM: Tail = Tail {
    class_init_first: (0x71021ff4c0, &[0, 8, 20], 0x71021ff4cc),
    metadata: (0x71021ff4dc, &[0, 8, 20, 21], 0x71021ff4ec),
    metadata_loop_poll: Some(0x71021ff4f0),
    class_init_again: (0x71021ff0f4, &[0, 8, 20, 21], None),
    kind: CounterKind::AddByte,
};

const TAIL_PHYSICAL: Tail = Tail {
    class_init_first: (0x71021ff4f8, &[0, 8, 9, 20], 0x71021ff504),
    metadata: (0x71021ff514, &[0, 8, 9, 20, 21], 0x71021ff524),
    metadata_loop_poll: None,
    class_init_again: (0x71021ff52c, &[0, 8, 9, 20, 21], Some(0x71021ff530)),
    kind: CounterKind::AddIfByte(4),
};

const TAIL_MAGIC: Tail = Tail {
    class_init_first: (0x71021ff538, &[0, 8, 9, 20], 0x71021ff544),
    metadata: (0x71021ff554, &[0, 8, 9, 20, 21], 0x71021ff564),
    metadata_loop_poll: None,
    class_init_again: (0x71021ff56c, &[0, 8, 9, 20, 21], Some(0x71021ff570)),
    kind: CounterKind::AddIfByte(2),
};

enum Check {
    /// The static-field flag byte, then the class word again.
    StaticFlag,
    /// The class word again.
    ClassWord,
    /// The class word was zero at the start: run the class initializer first.
    ClassInitFirst,
    /// The static-field metadata is not resolved yet.
    Metadata,
    /// The class word is still zero.
    ClassInitAgain,
}

impl SkillExecter {
    fn this(f: &Frame) -> u64 {
        f.x[19]
    }

    /// `exec_calc_type()`. `f` is the register file at entry.
    pub fn exec_calc_type(self, f: &mut Frame) -> Result<(), GuestException> {
        use field::*;
        // prologue: `sub sp, sp, #0x40`, saves x29 to x19, `add x29, sp, #0x10`
        f.x[31] = f.entry[31].wrapping_sub(0x40);
        f.x[29] = f.x[31].wrapping_add(0x10);
        let cell_a = f.x[29].wrapping_sub(4);
        let cell_b = f.x[31].wrapping_add(8);
        f.x[20] = FLAG_PAGE;
        f.x[21] = 0x7103a58000;
        f.x[8] = u64::from(prestate::read_u8(cells::FLAG));
        f.x[21] = live_u64(cells::GLOBAL_SLOT);
        f.x[19] = f.x[0];
        if f.x[8] & 1 == 0 {
            f.x[0] = prestate::read_u64(cells::CLASS_SLOT);
            f.call(
                callee::INITIALIZE_METADATA,
                0x71021fee88,
                &[0, 8, 19, 20, 21, 29],
                true,
            )?;
            f.x[0] = prestate::read_u64(cells::GLOBAL_SLOT);
            f.call(callee::INITIALIZE_METADATA, 0x71021fee94, &[0], false)?;
            for (slot, ret) in
                cells::LITERALS
                    .iter()
                    .zip([0x71021feea0, 0x71021feeac, 0x71021feeb8, 0x71021feec4])
            {
                f.x[0] = prestate::read_u64(*slot);
                f.call(callee::INITIALIZE_METADATA, ret, &[0], false)?;
            }
            f.x[8] = 1;
            prestate::store_u8(cells::FLAG, 1);
        }
        let this = Self::this(f);
        f.x[8] = ld64(this.wrapping_add(SKILL_OBJ));
        f.x[0] = live_u64(f.x[21]);
        f.x[22] = w(ld32(f.x[8].wrapping_add(CALC_TYPE)));
        prestate::store_u32(cell_a, 0);
        prestate::store_u32(cell_b, 0);
        f.x[8] = w(prestate::read_u32(f.x[0].wrapping_add(CLASS_INITIALIZED)));
        if f.x[8] as u32 == 0 {
            f.call(
                callee::CLASS_INITIALIZER,
                0x71021feeec,
                &[0, 8, 19, 20, 21, 22, 29],
                true,
            )?;
        }
        f.x[0] = 0;
        f.call(
            callee::GET_INST,
            0x71021feef4,
            &[0, 8, 19, 20, 21, 22, 29],
            true,
        )?;
        if f.x[22] as u32 > value::MAX_CALC_TYPE {
            return self.leave_type(f);
        }
        // the jump table: x8 = the calc type, x9 = the table, x10 = the base plus four times the entry, x20 = the singleton
        f.x[8] = w(f.x[22] as u32);
        f.x[9] = JUMP_TABLE;
        f.x[10] = JUMP_BASE;
        f.x[11] = u64::from(JUMP_TABLE_ENTRIES[f.x[8] as usize]);
        f.x[10] = f.x[10].wrapping_add(f.x[11] << 2);
        f.x[20] = f.x[0];
        match f.x[10] {
            arm::PHYSICAL => self.arm_physical(f, cell_a, cell_b),
            arm::SKILL_286 => self.arm_skill_286(f),
            arm::PHYSICAL_COUNTER => self.arm_physical_counter(f, cell_a, cell_b),
            arm::MAGIC_COUNTER => self.arm_magic_counter(f, cell_a, cell_b),
            arm::MAGIC_SUM => self.arm_magic_sum(f, cell_a),
            arm::SUM_COUNTER => self.arm_sum_counter(f, cell_a),
            arm::VALUE_ONLY => self.arm_value_only(f, cell_a),
            arm::RESULT_A => self.arm_result_a(f, cell_a),
            arm::RESULT_B => self.arm_result_b(f, cell_a),
            arm::PREPARE_THEN_CALL => self.arm_prepare_then_call(f),
            arm::CALL => self.arm_call(f),
            _ => {
                debug_assert_eq!(f.x[10], arm::NOTHING);
                self.leave_type(f)
            }
        }
    }

    /// The epilogue shared by every exit: x19 to x22 and x29 and the sp come back; x0 is flushed, x30 is not.
    fn leave_type(self, f: &mut Frame) -> Result<(), GuestException> {
        f.leave(&[0, 19, 20, 21, 22, 29]);
        Ok(())
    }

    /// `Int32.ToString` on the int at `cell`, called with x20 = the monitor already set by the caller.
    fn to_string(
        f: &mut Frame,
        cell: u64,
        return_address: u64,
        list: &[usize],
    ) -> Result<(), GuestException> {
        f.x[0] = cell;
        f.x[1] = 0;
        let mut area = prestate::read_u32(cell).to_le_bytes();
        f.call_out(
            callee::INT32_TO_STRING,
            return_address,
            list,
            false,
            &mut area,
        )
    }

    /// `monitor.write(<literal> + string)`: the string is in x0, the monitor in x20.
    fn text_line(
        f: &mut Frame,
        slot_offset: u64,
        concat: u64,
        debug: u64,
    ) -> Result<(), GuestException> {
        f.x[8] = LITERAL_PAGE;
        f.x[8] = live_u64(f.x[8].wrapping_add(slot_offset));
        f.x[1] = f.x[0];
        f.x[0] = ld64(f.x[8]);
        f.x[2] = 0;
        f.call(callee::STRING_CONCAT_2, concat, &[0, 1, 2, 8], false)?;
        f.x[1] = f.x[0];
        f.x[0] = f.x[20];
        f.x[2] = 0;
        f.call(callee::DEBUG_WRITE, debug, &[0, 1, 2], false)
    }

    /// The shared end of the arms that put one int into `_tmp_value`: write it, a debug line, store it (0x71021ff468).
    fn tmp_value_tail(self, f: &mut Frame, cell_a: u64) -> Result<(), GuestException> {
        f.x[20] = ld64(Self::this(f).wrapping_add(field::MONITOR));
        Self::to_string(f, cell_a, 0x71021ff478, &[0, 1, 8, 9, 10, 11, 20])?;
        Self::text_line(f, 0x598, 0x71021ff490, 0x71021ff4a0)?;
        f.x[8] = w(prestate::read_u32(cell_a));
        prestate::store_u32(Self::this(f).wrapping_add(field::TMP_VALUE), f.x[8] as u32);
        self.leave_type(f)
    }

    /// Attack and defence into the two stack ints, then the two debug lines (the start of calc types 8 to 10 and 1).
    fn physical_pair(
        self,
        f: &mut Frame,
        cell_a: u64,
        cell_b: u64,
        sites: [u64; 8],
    ) -> Result<(), GuestException> {
        use field::*;
        let this = Self::this(f);
        f.x[0] = ld64(this.wrapping_add(ACTER));
        f.x[8] = 1;
        f.x[1] = 0;
        prestate::store_u8(this.wrapping_add(FLAG_44), 1);
        prestate::store_u8(this.wrapping_add(FLAG_46), 1);
        call_stat(
            f,
            smrpg_param_calc_attack,
            callee::CALC_ATTACK,
            sites[0],
            &[0, 1, 8, 9, 10, 11, 20],
        )?;
        prestate::store_u32(cell_a, f.x[0] as u32);
        f.x[0] = ld64(this.wrapping_add(TARGET));
        f.x[1] = 0;
        call_stat(
            f,
            smrpg_param_calc_defence,
            callee::CALC_DEFENCE,
            sites[1],
            &[0, 1],
        )?;
        prestate::store_u32(cell_b, f.x[0] as u32);
        self.two_lines(
            f,
            cell_a,
            cell_b,
            [sites[2], sites[3], sites[4], sites[5], sites[6], sites[7]],
        )
    }

    /// The two debug lines of the stat arms: the first int (literal 0x5a8), then the second (literal 0x5b8).
    fn two_lines(
        self,
        f: &mut Frame,
        cell_a: u64,
        cell_b: u64,
        sites: [u64; 6],
    ) -> Result<(), GuestException> {
        let monitor = Self::this(f).wrapping_add(field::MONITOR);
        f.x[20] = ld64(monitor);
        Self::to_string(f, cell_a, sites[0], &[0, 1, 20])?;
        Self::text_line(f, 0x5a8, sites[1], sites[2])?;
        f.x[20] = ld64(monitor);
        Self::to_string(f, cell_b, sites[3], &[0, 1, 20])?;
        Self::text_line(f, 0x5b8, sites[4], sites[5])
    }

    /// Calc types 8 to 10: attack minus defence into `_tmp_value`.
    fn arm_physical(self, f: &mut Frame, cell_a: u64, cell_b: u64) -> Result<(), GuestException> {
        self.physical_pair(
            f,
            cell_a,
            cell_b,
            [
                0x71021fef34,
                0x71021fef44,
                0x71021fef58,
                0x71021fef70,
                0x71021fef80,
                0x71021fef90,
                0x71021fefa8,
                0x71021fefb8,
            ],
        )?;
        f.x[8] = w(prestate::read_u32(cell_a));
        f.x[9] = w(prestate::read_u32(cell_b));
        f.x[8] = w((f.x[8] as u32).wrapping_sub(f.x[9] as u32));
        prestate::store_u32(Self::this(f).wrapping_add(field::TMP_VALUE), f.x[8] as u32);
        self.leave_type(f)
    }

    /// Calc type 1: the same, then the counter object of the `BattleManager` gets 4 unless the acter's byte is 0.
    fn arm_physical_counter(
        self,
        f: &mut Frame,
        cell_a: u64,
        cell_b: u64,
    ) -> Result<(), GuestException> {
        self.physical_pair(
            f,
            cell_a,
            cell_b,
            [
                0x71021ff17c,
                0x71021ff18c,
                0x71021ff1a0,
                0x71021ff1b8,
                0x71021ff1c8,
                0x71021ff1d8,
                0x71021ff1f0,
                0x71021ff200,
            ],
        )?;
        self.difference_then_counter(f, cell_a, cell_b, &TAIL_PHYSICAL)
    }

    /// `_tmp_value = a - b`, then the class checks and the counter object (calc types 1 and 2).
    fn difference_then_counter(
        self,
        f: &mut Frame,
        cell_a: u64,
        cell_b: u64,
        tail: &Tail,
    ) -> Result<(), GuestException> {
        f.x[8] = w(prestate::read_u32(cell_a));
        f.x[20] = 0x7103a57000;
        f.x[9] = w(prestate::read_u32(cell_b));
        f.x[8] = w((f.x[8] as u32).wrapping_sub(f.x[9] as u32));
        f.x[20] = live_u64(f.x[20].wrapping_add(0xe48));
        prestate::store_u32(Self::this(f).wrapping_add(field::TMP_VALUE), f.x[8] as u32);
        self.counter_object(f, tail)
    }

    /// Calc type 2: `a = skill value + calc_magic_attack(acter)`, `b = calc_magic_defence(target)`, `_tmp_value = a - b`,
    /// then the counter object gets 2 unless the acter's byte is 0.
    fn arm_magic_counter(
        self,
        f: &mut Frame,
        cell_a: u64,
        cell_b: u64,
    ) -> Result<(), GuestException> {
        use field::*;
        let this = Self::this(f);
        f.x[0] = ld64(this.wrapping_add(ACTER));
        f.x[8] = 1;
        f.x[1] = 0;
        prestate::store_u8(this.wrapping_add(FLAG_44), 1);
        call_stat(
            f,
            smrpg_param_calc_magic_attack,
            callee::CALC_MAGIC_ATTACK,
            0x71021ff274,
            &[0, 1, 8, 9, 10, 11, 20],
        )?;
        f.x[8] = ld64(this.wrapping_add(SKILL_OBJ));
        f.x[1] = 0;
        f.x[8] = w(ld32(f.x[8].wrapping_add(SKILL_VALUE)));
        f.x[8] = w((f.x[8] as u32).wrapping_add(f.x[0] as u32));
        prestate::store_u32(cell_a, f.x[8] as u32);
        f.x[0] = ld64(this.wrapping_add(TARGET));
        call_stat(
            f,
            smrpg_param_calc_magic_defence,
            callee::CALC_MAGIC_DEFENCE,
            0x71021ff290,
            &[0, 1, 8],
        )?;
        prestate::store_u32(cell_b, f.x[0] as u32);
        self.two_lines(
            f,
            cell_a,
            cell_b,
            [
                0x71021ff2a4,
                0x71021ff2bc,
                0x71021ff2cc,
                0x71021ff2dc,
                0x71021ff2f4,
                0x71021ff304,
            ],
        )?;
        self.difference_then_counter(f, cell_a, cell_b, &TAIL_MAGIC)
    }

    /// Calc type 3: `a = skill value + calc_magic_attack(acter)` into `_tmp_value` with a debug line.
    fn arm_magic_sum(self, f: &mut Frame, cell_a: u64) -> Result<(), GuestException> {
        use field::*;
        let this = Self::this(f);
        f.x[0] = ld64(this.wrapping_add(ACTER));
        f.x[8] = 1;
        f.x[1] = 0;
        prestate::store_u8(this.wrapping_add(FLAG_45), 1);
        call_stat(
            f,
            smrpg_param_calc_magic_attack,
            callee::CALC_MAGIC_ATTACK,
            0x71021ff05c,
            &[0, 1, 8, 9, 10, 11, 20],
        )?;
        f.x[8] = ld64(this.wrapping_add(SKILL_OBJ));
        f.x[8] = w(ld32(f.x[8].wrapping_add(SKILL_VALUE)));
        f.x[8] = w((f.x[8] as u32).wrapping_add(f.x[0] as u32));
        prestate::store_u32(cell_a, f.x[8] as u32);
        self.tmp_value_tail(f, cell_a)
    }

    /// Calc type 5: the skill value into `_tmp_value` with a debug line.
    fn arm_value_only(self, f: &mut Frame, cell_a: u64) -> Result<(), GuestException> {
        use field::*;
        let this = Self::this(f);
        f.x[8] = 1;
        prestate::store_u8(this.wrapping_add(FLAG_45), 1);
        f.x[8] = ld64(this.wrapping_add(SKILL_OBJ));
        f.x[8] = w(ld32(f.x[8].wrapping_add(SKILL_VALUE)));
        prestate::store_u32(cell_a, f.x[8] as u32);
        self.tmp_value_tail(f, cell_a)
    }

    /// Calc type 4: the skill value into `_tmp_value` with a debug line, then the counter object gets the acter's byte.
    fn arm_sum_counter(self, f: &mut Frame, cell_a: u64) -> Result<(), GuestException> {
        use field::*;
        let this = Self::this(f);
        f.x[8] = 1;
        prestate::store_u8(this.wrapping_add(FLAG_44), 1);
        f.x[8] = ld64(this.wrapping_add(SKILL_OBJ));
        f.x[8] = w(ld32(f.x[8].wrapping_add(SKILL_VALUE)));
        prestate::store_u32(cell_a, f.x[8] as u32);
        f.x[20] = ld64(this.wrapping_add(MONITOR));
        Self::to_string(f, cell_a, 0x71021ff090, &[0, 1, 8, 9, 10, 11, 20])?;
        Self::text_line(f, 0x5a8, 0x71021ff0a8, 0x71021ff0b8)?;
        f.x[8] = w(prestate::read_u32(cell_a));
        f.x[20] = 0x7103a57000;
        f.x[20] = live_u64(f.x[20].wrapping_add(0xe48));
        prestate::store_u32(this.wrapping_add(TMP_VALUE), f.x[8] as u32);
        self.counter_object(f, &TAIL_SUM)
    }

    /// Calc type 0 (the compiled switch's second label): skill 0x286 against another character fills the target's logger.
    fn arm_skill_286(self, f: &mut Frame) -> Result<(), GuestException> {
        use field::*;
        let this = Self::this(f);
        f.x[8] = ld64(this.wrapping_add(SKILL_OBJ));
        f.x[9] = w(ld32(f.x[8].wrapping_add(SKILL_ID)));
        if f.x[9] as u32 != value::SKILL_SPECIAL {
            return self.leave_type(f);
        }
        f.x[10] = ld64(this.wrapping_add(ACTER));
        f.x[9] = ld64(this.wrapping_add(TARGET));
        f.x[10] = w(ld32(f.x[10].wrapping_add(CHARACTER_ID)));
        f.x[11] = w(ld32(f.x[9].wrapping_add(CHARACTER_ID)));
        if f.x[10] as u32 == f.x[11] as u32 {
            return self.leave_type(f);
        }
        f.x[9] = ld64(f.x[9].wrapping_add(RESULT_LOGGER));
        f.x[1] = 0;
        f.x[8] = w(ld32(f.x[8].wrapping_add(SKILL_VALUE)));
        prestate::store_u32(f.x[9].wrapping_add(LOGGER_A), f.x[8] as u32);
        f.x[8] = ld64(this.wrapping_add(TARGET));
        f.x[8] = ld64(f.x[8].wrapping_add(RESULT_LOGGER));
        f.x[9] = 2;
        prestate::store_u32(f.x[8].wrapping_add(LOGGER_B), 2);
        f.x[8] = ld64(this.wrapping_add(SKILL_OBJ));
        f.x[20] = ld64(this.wrapping_add(MONITOR));
        f.x[0] = f.x[8].wrapping_add(SKILL_VALUE);
        let mut area = prestate::read_u32(f.x[0]).to_le_bytes();
        f.call_out(
            callee::INT32_TO_STRING,
            0x71021ff01c,
            &[0, 1, 8, 9, 10, 11, 20],
            false,
            &mut area,
        )?;
        Self::text_line(f, 0x5b0, 0x71021ff034, 0x71021ff044)?;
        self.leave_type(f)
    }

    /// Calc type 6: a result object (`_obj18`) is flagged and gets two values from the `GlobalWork` getters; the target's
    /// logger gets the skill value.
    fn arm_result_a(self, f: &mut Frame, cell_a: u64) -> Result<(), GuestException> {
        use field::*;
        let this = Self::this(f);
        f.x[8] = ld64(this.wrapping_add(OBJ18));
        f.x[21] = 1;
        prestate::store_u8(f.x[8].wrapping_add(OBJ18_FLAG), 1);
        f.x[22] = ld64(this.wrapping_add(OBJ18));
        f.x[0] = f.x[20];
        f.x[1] = 0;
        f.call(
            callee::GETTER_A,
            0x71021ff38c,
            &[0, 1, 8, 9, 10, 11, 20, 21, 22],
            false,
        )?;
        f.x[8] = f.regs.x(8);
        prestate::store_u32(f.x[22].wrapping_add(OBJ18_A), f.x[0] as u32);
        f.x[8] = ld64(this.wrapping_add(SKILL_OBJ));
        f.x[1] = w(ld32(f.x[8].wrapping_add(SKILL_VALUE)));
        f.x[22] = ld64(this.wrapping_add(OBJ18));
        f.x[0] = f.x[20];
        f.x[2] = 0;
        f.call(callee::SETTER_C, 0x71021ff3a8, &[0, 1, 2, 8, 22], false)?;
        f.x[8] = f.regs.x(8);
        f.x[9] = f.regs.x(9);
        prestate::store_u32(f.x[22].wrapping_add(OBJ18_B), f.x[0] as u32);
        f.x[8] = ld64(this.wrapping_add(SKILL_OBJ));
        f.x[8] = w(ld32(f.x[8].wrapping_add(SKILL_VALUE)));
        prestate::store_u32(cell_a, f.x[8] as u32);
        f.x[20] = ld64(this.wrapping_add(MONITOR));
        Self::to_string(f, cell_a, 0x71021ff3c8, &[0, 1, 8, 20])?;
        Self::text_line(f, 0x598, 0x71021ff3e0, 0x71021ff3f0)?;
        f.x[8] = w(prestate::read_u32(cell_a));
        prestate::store_u32(this.wrapping_add(TMP_VALUE), f.x[8] as u32);
        f.x[9] = ld64(this.wrapping_add(TARGET));
        f.x[9] = ld64(f.x[9].wrapping_add(RESULT_LOGGER));
        prestate::store_u32(f.x[9].wrapping_add(LOGGER_A), f.x[8] as u32);
        f.x[8] = ld64(this.wrapping_add(TARGET));
        f.x[8] = ld64(f.x[8].wrapping_add(RESULT_LOGGER));
        prestate::store_u32(f.x[8].wrapping_add(LOGGER_B), f.x[21] as u32);
        f.x[8] = ld64(this.wrapping_add(TARGET));
        f.x[8] = ld64(f.x[8].wrapping_add(RESULT_LOGGER));
        prestate::store_u8(f.x[8].wrapping_add(LOGGER_FLAG), f.x[21] as u8);
        self.leave_type(f)
    }

    /// Calc type 7: the same two getters, then `_flag_45` and a value from the target's object getter into `_tmp_value`.
    fn arm_result_b(self, f: &mut Frame, cell_a: u64) -> Result<(), GuestException> {
        use field::*;
        let this = Self::this(f);
        f.x[8] = ld64(this.wrapping_add(OBJ18));
        f.x[21] = 1;
        prestate::store_u8(f.x[8].wrapping_add(OBJ18_FLAG), 1);
        f.x[22] = ld64(this.wrapping_add(OBJ18));
        f.x[0] = f.x[20];
        f.x[1] = 0;
        f.call(
            callee::GETTER_A,
            0x71021ff43c,
            &[0, 1, 8, 9, 10, 11, 20, 21, 22],
            false,
        )?;
        f.x[8] = f.regs.x(8);
        prestate::store_u32(f.x[22].wrapping_add(OBJ18_A), f.x[0] as u32);
        f.x[22] = ld64(this.wrapping_add(OBJ18));
        f.x[0] = f.x[20];
        f.x[1] = 0;
        f.call(callee::GETTER_B, 0x71021ff450, &[0, 1, 22], false)?;
        f.x[8] = f.regs.x(8);
        prestate::store_u32(f.x[22].wrapping_add(OBJ18_B), f.x[0] as u32);
        f.x[0] = ld64(this.wrapping_add(TARGET));
        f.x[1] = 0;
        prestate::store_u8(this.wrapping_add(FLAG_45), f.x[21] as u8);
        f.call(callee::OBJECT_GETTER, 0x71021ff464, &[0, 1], false)?;
        f.x[8] = f.regs.x(8);
        prestate::store_u32(cell_a, f.x[0] as u32);
        self.tmp_value_tail(f, cell_a)
    }

    /// Calc type 12: a preparation call on the executer, then the same as calc type 13.
    fn arm_prepare_then_call(self, f: &mut Frame) -> Result<(), GuestException> {
        f.x[0] = Self::this(f);
        f.call(callee::PREPARE, 0x71021ff13c, &[0, 8, 9, 10, 11, 20], false)?;
        self.arm_call(f)
    }

    /// Calc type 13: `<class init>; <call>(_skill_obj._skill_id)`.
    fn arm_call(self, f: &mut Frame) -> Result<(), GuestException> {
        use field::*;
        let this = Self::this(f);
        f.x[8] = ld64(this.wrapping_add(SKILL_OBJ));
        f.x[0] = live_u64(f.x[21]);
        f.x[19] = w(ld32(f.x[8].wrapping_add(SKILL_ID)));
        f.x[8] = w(live_u32(f.x[0].wrapping_add(CLASS_INITIALIZED)));
        if f.x[8] as u32 == 0 {
            f.call(
                callee::CLASS_INITIALIZER,
                0x71021ff154,
                &[0, 8, 9, 10, 11, 19, 20],
                false,
            )?;
        }
        f.x[0] = w(f.x[19] as u32);
        f.x[1] = 0;
        f.call(
            callee::SKILL_CALL,
            0x71021ff160,
            &[0, 1, 8, 9, 10, 11, 19, 20],
            false,
        )?;
        self.leave_type(f)
    }

    /// The `BattleManager` class checks (class word, static-field flag byte, metadata, class initializer; x20 = the class
    /// cell) and then the counter object: `manager._instance.<+0x60>.<+0x1c>` gets the acter's byte or a constant.
    fn counter_object(self, f: &mut Frame, tail: &Tail) -> Result<(), GuestException> {
        use field::*;
        let this = Self::this(f);
        f.x[0] = live_u64(f.x[20]);
        f.x[8] = w(prestate::read_u32(f.x[0].wrapping_add(CLASS_INITIALIZED)));
        let mut check = if f.x[8] as u32 == 0 {
            Check::ClassInitFirst
        } else {
            Check::StaticFlag
        };
        loop {
            match check {
                Check::StaticFlag => {
                    f.x[21] = 0x7103a58000;
                    f.x[21] = live_u64(f.x[21].wrapping_add(0x1d0));
                    f.x[8] = u64::from(prestate::read_u8(f.x[21]));
                    check = if f.x[8] as u32 == 0 {
                        Check::Metadata
                    } else {
                        Check::ClassWord
                    };
                }
                Check::ClassWord => {
                    f.x[0] = live_u64(f.x[20]);
                    f.x[8] = w(live_u32(f.x[0].wrapping_add(CLASS_INITIALIZED)));
                    if f.x[8] as u32 != 0 {
                        break;
                    }
                    check = Check::ClassInitAgain;
                }
                Check::ClassInitFirst => {
                    let (ret, list, poll) = tail.class_init_first;
                    f.call(callee::CLASS_INITIALIZER, ret, list, false)?;
                    f.x[21] = 0x7103a58000;
                    f.x[21] = live_u64(f.x[21].wrapping_add(0x1d0));
                    f.x[8] = u64::from(prestate::read_u8(f.x[21]));
                    f.regs.safepoint_n(poll, &[(8, f.x[8]), (21, f.x[21])]);
                    check = if f.x[8] as u32 != 0 {
                        Check::ClassWord
                    } else {
                        Check::Metadata
                    };
                }
                Check::Metadata => {
                    let (ret, list, poll) = tail.metadata;
                    f.x[0] = prestate::read_u64(cells::CLASS_SLOT);
                    f.call(callee::INITIALIZE_METADATA, ret, list, false)?;
                    f.x[8] = 1;
                    prestate::store_u8(f.x[21], 1);
                    f.x[0] = live_u64(f.x[20]);
                    f.x[8] = w(live_u32(f.x[0].wrapping_add(CLASS_INITIALIZED)));
                    f.regs.safepoint_n(poll, &[(0, f.x[0]), (8, f.x[8])]);
                    if f.x[8] as u32 != 0 {
                        break;
                    }
                    if let Some(pc) = tail.metadata_loop_poll {
                        f.regs.safepoint_n(pc, &[(0, f.x[0]), (8, f.x[8])]);
                    }
                    check = Check::ClassInitAgain;
                }
                Check::ClassInitAgain => {
                    let (ret, list, poll) = tail.class_init_again;
                    f.call(callee::CLASS_INITIALIZER, ret, list, false)?;
                    f.x[0] = live_u64(f.x[20]);
                    if let Some(pc) = poll {
                        f.regs.safepoint_n(pc, &[(0, f.x[0])]);
                    }
                    break;
                }
            }
        }
        f.x[8] = ld64(f.x[0].wrapping_add(CLASS_STATIC_FIELDS));
        f.x[8] = ld64(f.x[8].wrapping_add(STATICS_INSTANCE));
        f.x[8] = ld64(f.x[8].wrapping_add(INSTANCE_OBJECT));
        f.x[10] = ld64(this.wrapping_add(ACTER));
        f.x[10] = u64::from(ld8(f.x[10].wrapping_add(PARAM_BYTE)));
        let counter = f.x[8].wrapping_add(COUNTER);
        f.x[9] = w(prestate::read_u32(counter));
        match tail.kind {
            CounterKind::AddByte => {
                f.x[9] = w((f.x[9] as u32).wrapping_add(f.x[10] as u32));
            }
            CounterKind::AddIfByte(amount) => {
                f.x[11] = w((f.x[9] as u32).wrapping_add(amount));
                if f.x[10] as u32 != 0 {
                    f.x[9] = f.x[11];
                }
            }
        }
        prestate::store_u32(counter, f.x[9] as u32);
        self.leave_type(f)
    }
}

/// 0x71021fee50 `SkillExecter.exec_calc_type` (replay row with a store journal).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_skill_exec_calc_type(x: *mut u64) {
    unsafe {
        let regs = Regs::from_raw(x);
        let this = regs.x(0);
        let mut frame = Frame::new(regs);
        let _ = SkillExecter::at(this).exec_calc_type(&mut frame);
    }
}

/// Pre-state hook. Returns 1 for the first run in a process (the flag byte clear: the class cells are still tokens) and when
/// a stat getter the call will make defers. Otherwise the class words the port reads after callees may have changed, the
/// static-field flag byte, and the cells the port stores to or reads back (the counter object's int is read-modify-write).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_skill_exec_calc_type_pre(x: *mut u64) -> i32 {
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
            if class != 0 {
                prestate::snapshot(class.wrapping_add(field::CLASS_INITIALIZED), 4, false);
            }
        }
        let class_cell = guest_ld64(cells::CLASS_SLOT);
        let class = if class_cell != 0 {
            guest_ld64(class_cell)
        } else {
            0
        };
        if class != 0 {
            prestate::snapshot(class.wrapping_add(field::CLASS_INITIALIZED), 4, false);
        }
        let flag_cell = guest_ld64(cells::STATIC_FLAG_POINTER);
        if flag_cell != 0 {
            prestate::snapshot(flag_cell, 1, true);
        }
        prestate::snapshot(this.wrapping_add(field::TMP_VALUE), 4, true);
        for offset in [field::FLAG_44, field::FLAG_45, field::FLAG_46] {
            prestate::snapshot(this.wrapping_add(offset), 1, true);
        }
        let obj18 = guest_ld64(this.wrapping_add(field::OBJ18));
        if obj18 != 0 {
            prestate::snapshot(obj18.wrapping_add(field::OBJ18_FLAG), 1, true);
            prestate::snapshot(obj18.wrapping_add(field::OBJ18_A), 4, true);
            prestate::snapshot(obj18.wrapping_add(field::OBJ18_B), 4, true);
        }
        let target = guest_ld64(this.wrapping_add(field::TARGET));
        if target != 0 {
            let logger = guest_ld64(target.wrapping_add(field::RESULT_LOGGER));
            if logger != 0 {
                prestate::snapshot(logger.wrapping_add(field::LOGGER_A), 4, true);
                prestate::snapshot(logger.wrapping_add(field::LOGGER_B), 4, true);
                prestate::snapshot(logger.wrapping_add(field::LOGGER_FLAG), 1, true);
            }
        }
        if class != 0 {
            let statics = guest_ld64(class.wrapping_add(field::CLASS_STATIC_FIELDS));
            if statics != 0 {
                let instance = guest_ld64(statics.wrapping_add(field::STATICS_INSTANCE));
                if instance != 0 {
                    let object = guest_ld64(instance.wrapping_add(field::INSTANCE_OBJECT));
                    if object != 0 {
                        prestate::snapshot(object.wrapping_add(field::COUNTER), 4, true);
                    }
                }
            }
        }
        // the nested stat getters the call will make: their hooks (the defence getters have none)
        let skill_obj = guest_ld64(this.wrapping_add(field::SKILL_OBJ));
        if skill_obj != 0 {
            let acter = guest_ld64(this.wrapping_add(field::ACTER));
            let calc_type = guest_ld32(skill_obj.wrapping_add(field::CALC_TYPE));
            let mut nested = [0u64; 32];
            nested[0] = acter;
            match calc_type {
                1 | 8 | 9 | 10 if acter != 0 => {
                    if smrpg_param_calc_attack_pre(nested.as_mut_ptr()) != 0 {
                        return 1;
                    }
                }
                2 | 3 if acter != 0 => {
                    if smrpg_param_calc_magic_attack_pre(nested.as_mut_ptr()) != 0 {
                        return 1;
                    }
                }
                _ => {}
            }
        }
        0
    }
}

#[cfg(test)]
mod tests;
