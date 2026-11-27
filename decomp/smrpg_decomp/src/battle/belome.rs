//! `Battle.SkillExecter.calc_belome_barrior_ratio(SkillObject skill, ref float ratio)` (0x71021f5c50, task-57), one of
//! the formula callees of `damage_calc`.
//!
//! In C# terms (names are IL2CPP's; the id 1609 is the character `_belomu_barrior_list` is about):
//!
//! ```text
//! void calc_belome_barrior_ratio(SkillObject skill, ref float ratio) {
//!     if (_target._character_id != 1609) return;
//!     bool[] list = GlobalWork.get_inst()._belomu_barrior_list;      // after the GlobalWork class initializer
//!     bool barriered = false;
//!     switch (skill._element) {
//!         case NORMAL:  if (skill._calc_type == SP_ATTACK) barriered = list[4]; break;
//!         case JUMP:    if (_acter._is_equip_kaiten_shoes) return; barriered = list[1]; break;
//!         case FIRE:    barriered = list[3]; break;
//!         case THUNDER: barriered = list[2]; break;
//!         case ICE:     barriered = list[5]; break;
//!     }
//!     int id = skill._skill_id;
//!     bool absorbed;
//!     if (id > 0x9f)      absorbed = (uint)(id - 0x2ba) <= 15 && ((1 << (id - 0x2ba)) & 0x8201) != 0 ? list[4] : barriered;
//!     else if (id == 1)   absorbed = list[0];
//!     else                absorbed = (id & ~1) == 0x9e ? list[4] : barriered;
//!     if (absorbed) {
//!         ratio = 0f;
//!         List<int> numbers = _target._result_logger._multi_number;
//!         if (numbers == null) return;
//!         int count = numbers.Count;
//!         numbers.Clear();                                             // version++, size = 0, Array.Clear
//!         for (int i = 0; i < count; i++) numbers.Add(0);              // every hit number becomes 0
//!     }
//! }
//! ```
//!
//! (`list[i]` throws IndexOutOfRange when the array is shorter. `numbers.Clear()` is the inlined `List<T>.Clear`:
//! it clears the array only when `T` holds references.) The compiled function begins with IL2CPP's lazy
//! initialization (a flag byte, four metadata slots) and the GlobalWork class-initialized test.
//!
//! The port is a replay row with a pre-state hook (`QLEAF`): the lazy-initialization flag, the class-initialized
//! word and the list header are read through `prestate`, and its stores (`ratio`, the list's version and size) go
//! through the store journal. The `numbers.Add(0)` callee changes the list header too, so it is an out footprint
//! row (`OLEAF` 0x710015b4c0, the 16 bytes at list+0x10): the port carries the header from call to call and the
//! comparison checks it at each call.

use super::damage::{CharaSkillResultLogger, SkillExecter, SkillObject};
use super::frame::Frame;
use super::{BattleCharacterParam, GuestException, GuestObject, Regs};
use crate::bridge::prestate;
use crate::guest_ld8;
use crate::guest_ld64;
use crate::object::guest_class;

/// `_character_id` of the target the barrier list applies to.
pub(crate) const BELOME_CHARACTER_ID: u32 = 0x649;
/// `SkillObject.CalcType.SP_ATTACK`.
const CALC_TYPE_SP_ATTACK: u32 = 2;

/// `ElementKind` values the compiled jump table covers (0 to 4); anything above takes the default.
mod element {
    pub const NORMAL: u32 = 0;
    pub const JUMP: u32 = 1;
    pub const FIRE: u32 = 2;
    pub const THUNDER: u32 = 3;
    pub const ICE: u32 = 4;
}

/// The compiled switch is `br x10` with `x10 = 0x71021f5d04 + 4 * table[element]`; the byte table is in the image.
const JUMP_TABLE: u64 = 0x7102b04eeb;
const JUMP_TABLE_BYTES: [u8; 5] = [0, 43, 31, 37, 25];
const JUMP_BASE: u64 = 0x71021f5d04;

/// Indexes of `_belomu_barrior_list` (named by who reads them).
mod barrier_index {
    /// Skill 1.
    pub const SKILL_ONE: u64 = 0;
    pub const JUMP: u64 = 1;
    pub const THUNDER: u64 = 2;
    pub const FIRE: u64 = 3;
    /// Normal-element special attacks and the skill ids 0x9e, 0x9f, 0x2ba, 0x2c3 and 0x2c9.
    pub const SPECIAL: u64 = 4;
    pub const ICE: u64 = 5;
}

/// The lazy-initialization flag byte of this method, and the metadata slots it resolves (data page + offset).
pub mod slots {
    pub const INIT_FLAG: u64 = 0x7103cba000 + 0xd5d;
    /// Class `GlobalWork` (its static constructor must have run before `get_inst`).
    pub const GLOBAL_WORK_CLASS: u64 = 0x7103a58000 + 0x48;
    /// The generic method info of `List<int>.Add`, passed to the generic call (its first word is the argument).
    pub const LIST_ADD_METHOD: u64 = 0x7103a4d000 + 0x788;
    /// Second slot resolved at start; unused by the body.
    pub const UNUSED_790: u64 = 0x7103a4d000 + 0x790;
    /// Generic context chain that ends at the `IsReferenceOrContainsReferences<int>` argument.
    pub const LIST_CONTEXT: u64 = 0x7103a4e000 + 0x8c8;
}

/// The lifted functions the body calls.
mod callee {
    pub const INITIALIZE_METADATA: u64 = 0x71000c4150;
    pub const CLASS_INITIALIZER: u64 = 0x71000c4330;
    pub const GLOBAL_WORK_GET_INST: u64 = 0x7101886a00;
    pub const INDEX_THROWER: u64 = 0x71000c4510;
    pub const IS_REFERENCE_OR_CONTAINS_REFERENCES: u64 = 0x71000c5060;
    pub const ARRAY_CLEAR: u64 = 0x7101d7c0c0;
    pub const LIST_INT_ADD: u64 = 0x710015b4c0;
}

/// The address after each call (what the lifted code writes to x30) and the registers written before it.
mod site {
    pub const INIT_RETURNS: [u64; 4] = [0x71021f5c84, 0x71021f5c90, 0x71021f5c9c, 0x71021f5ca8];
    pub const CLASS_INIT_RETURN: u64 = 0x71021f5cd8;
    pub const GET_INST_RETURN: u64 = 0x71021f5ce0;
    pub const THROW_RETURN: u64 = 0x71021f5ed0;
    pub const REFERENCES_RETURN: u64 = 0x71021f5e60;
    pub const CLEAR_RETURN: u64 = 0x71021f5e84;
    pub const ADD_RETURN: u64 = 0x71021f5eb4;
    /// The two back-edge safepoints: after the barrier test (flushes x8 to x11) and after the Add loop's decrement.
    pub const POLL_AFTER_TEST: u64 = 0x71021f5de0;
    pub const POLL_IN_LOOP: u64 = 0x71021f5eb8;

    pub const FIRST_CALL: &[usize] = &[0, 8, 19, 20, 21, 22, 29, 30];
    pub const LATER_INIT_CALL: &[usize] = &[0, 30];
    pub const THROW: &[usize] = &[8, 9, 10, 11, 30];
    pub const REFERENCES: &[usize] = &[0, 8, 9, 10, 11, 20, 21, 30];
    pub const CLEAR: &[usize] = &[0, 1, 2, 3, 30];
    pub const ADD: &[usize] = &[0, 1, 2, 8, 20, 21, 30];
    pub const RETURN: &[usize] = &[2, 19, 20, 21, 22, 29];
}

guest_class!(
    /// `GlobalWork`: only `_belomu_barrior_list`.
    GlobalWork
);
impl GlobalWork {
    const BELOMU_BARRIOR_LIST: u64 = 0xe8;

    fn belomu_barrior_list(self) -> BoolArray {
        self.object_field(Self::BELOMU_BARRIOR_LIST)
    }
}

guest_class!(
    /// A managed `bool[]`: the length at +0x18, the elements from +0x20.
    BoolArray
);
impl BoolArray {
    const LENGTH: u64 = 0x18;
    const ELEMENTS: u64 = 0x20;

    fn len(self) -> u32 {
        self.u32_field(Self::LENGTH)
    }

    fn element(self, index: u64) -> u8 {
        self.u8_field(Self::ELEMENTS + index)
    }
}

guest_class!(
    /// `List<int>`: the header the inlined `Clear` and `Add` edit.
    ListInt
);
impl ListInt {
    const ITEMS: u64 = 0x10;
    const SIZE: u64 = 0x18;
    const VERSION: u64 = 0x1c;
    /// Bytes of the header the `Add` callee edits (items pointer, size, version): its out footprint.
    pub(crate) const HEADER_BYTES: usize = 16;

    pub(crate) fn size_address(self) -> u64 {
        self.address().wrapping_add(Self::SIZE)
    }

    pub(crate) fn version_address(self) -> u64 {
        self.address().wrapping_add(Self::VERSION)
    }

    pub(crate) fn items_address(self) -> u64 {
        self.address().wrapping_add(Self::ITEMS)
    }
}

impl CharaSkillResultLogger {
    pub(crate) const MULTI_NUMBER: u64 = 0x50;

    pub(crate) fn multi_number(self) -> Option<ListInt> {
        self.optional_field(Self::MULTI_NUMBER)
    }
}

impl SkillObject {
    const CALC_TYPE: u64 = 0x24;
    const ELEMENT: u64 = 0x38;

    pub(crate) fn calc_type(self) -> u32 {
        self.u32_field(Self::CALC_TYPE)
    }

    pub(crate) fn element(self) -> u32 {
        self.u32_field(Self::ELEMENT)
    }
}

impl BattleCharacterParam {
    const CHARACTER_ID: u64 = 0x20;
    const IS_EQUIP_KAITEN_SHOES: u64 = 0x7d;

    pub(crate) fn character_id(self) -> u32 {
        self.u32_field(Self::CHARACTER_ID)
    }

    pub(crate) fn is_equip_kaiten_shoes_byte(self) -> u8 {
        self.u8_field(Self::IS_EQUIP_KAITEN_SHOES)
    }
}

impl SkillExecter {
    const TARGET: u64 = 0x28;

    pub(crate) fn target(self) -> BattleCharacterParam {
        self.object_field(Self::TARGET)
    }
}

/// What the element `switch` decides.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ElementRoute {
    /// Read `list[index]`.
    List(u64),
    /// `NORMAL` with a calc type other than SP_ATTACK, or an element outside 0 to 4: no entry, `barriered = false`.
    None,
    /// `JUMP`: the function returns when `_acter` wears the kaiten shoes, otherwise `list[1]` is read.
    JumpUnlessKaitenShoes,
}

impl ElementRoute {
    pub fn of(element: u32, calc_type: u32) -> Self {
        match element {
            element::NORMAL if calc_type == CALC_TYPE_SP_ATTACK => {
                Self::List(barrier_index::SPECIAL)
            }
            element::JUMP => Self::JumpUnlessKaitenShoes,
            element::FIRE => Self::List(barrier_index::FIRE),
            element::THUNDER => Self::List(barrier_index::THUNDER),
            element::ICE => Self::List(barrier_index::ICE),
            _ => Self::None,
        }
    }
}

/// What the skill id adds to the element route (the second part of the compiled body).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SkillRoute {
    /// Skill 1 reads `list[0]` and ignores the element result.
    SkillOne,
    /// Skills 0x9e and 0x9f, and the ids 0x2ba, 0x2c3 and 0x2c9: `list[4]`.
    Special,
    /// Any other id: the element route's result.
    Element,
}

impl SkillRoute {
    pub fn of(skill_id: i32) -> Self {
        if skill_id > 0x9f {
            let bit = (skill_id as u32).wrapping_sub(0x2ba);
            if bit <= 0xf && (1u32 << bit) & 0x8201 != 0 {
                Self::Special
            } else {
                Self::Element
            }
        } else if skill_id == 1 {
            Self::SkillOne
        } else if (skill_id as u32) & !1 == 0x9e {
            Self::Special
        } else {
            Self::Element
        }
    }
}

/// The skill id test as the compiled code leaves registers: x9 = the id, then `id - 0x2ba` and the bit mask test.
fn skill_id_registers(f: &mut Frame, skill_id: i32) {
    if skill_id > 0x9f {
        let bit = (skill_id as u32).wrapping_sub(0x2ba);
        f.x[9] = u64::from(bit);
        if bit <= 0xf {
            // `mov w10, #1`, `lsl w9, w10, w9`, `mov w10, #0x8201`
            f.x[10] = 1;
            f.x[9] = u64::from(1u32 << bit);
            f.x[10] = 0x8201;
        }
    } else if skill_id != 1 {
        f.x[9] = u64::from(skill_id as u32 & !1);
    }
}

impl SkillExecter {
    /// `calc_belome_barrior_ratio(skill, ref ratio)`: `ratio` is the guest address of the float.
    pub fn calc_belome_barrior_ratio(
        self,
        skill: SkillObject,
        ratio: u64,
        f: &mut Frame,
    ) -> Result<(), GuestException> {
        let flag = prestate::read_u8(slots::INIT_FLAG);
        // prologue: `stp x29, x30, [sp, #-48]!`, x22 = the flag page, x8 = the flag byte, x19 to x21 = the arguments
        f.x[31] = f.entry[31].wrapping_sub(48);
        f.x[29] = f.x[31];
        f.x[22] = slots::INIT_FLAG & !0xfff;
        f.x[8] = u64::from(flag);
        f.x[19] = self.address();
        f.x[20] = ratio;
        f.x[21] = skill.address();
        if flag & 1 == 0 {
            self.initialize_metadata(f)?;
            prestate::store_u8(slots::INIT_FLAG, 1);
        }
        let character_id = self.target().character_id();
        f.x[8] = u64::from(character_id);
        if character_id != BELOME_CHARACTER_ID {
            f.leave(site::RETURN);
            return Ok(());
        }
        let work = self.global_work(f)?;
        if !self.barrier_absorbs(skill, work, f)? {
            f.leave(site::RETURN);
            return Ok(());
        }
        self.zero_hit_numbers(ratio, f)
    }

    /// The four `initialize_runtime_metadata` calls of the first run, then `x8 = 1` (the flag store is the caller's).
    fn initialize_metadata(self, f: &mut Frame) -> Result<(), GuestException> {
        let slot_list = [
            slots::GLOBAL_WORK_CLASS,
            slots::LIST_ADD_METHOD,
            slots::LIST_CONTEXT,
            slots::UNUSED_790,
        ];
        for (n, slot) in slot_list.into_iter().enumerate() {
            f.x[0] = unsafe { guest_ld64(slot) };
            let list = if n == 0 {
                site::FIRST_CALL
            } else {
                site::LATER_INIT_CALL
            };
            f.call(
                callee::INITIALIZE_METADATA,
                site::INIT_RETURNS[n],
                list,
                n == 0,
            )?;
        }
        f.x[8] = 1;
        Ok(())
    }

    /// The `GlobalWork` class initializer check and `GlobalWork.get_inst()`.
    fn global_work(self, f: &mut Frame) -> Result<GlobalWork, GuestException> {
        let class = unsafe { guest_ld64(guest_ld64(slots::GLOBAL_WORK_CLASS)) };
        let initialized = prestate::read_u32(class.wrapping_add(0xe0));
        f.x[0] = class;
        f.x[8] = u64::from(initialized);
        if initialized == 0 {
            f.call(
                callee::CLASS_INITIALIZER,
                site::CLASS_INIT_RETURN,
                site::FIRST_CALL,
                true,
            )?;
        }
        f.x[0] = 0;
        f.call(
            callee::GLOBAL_WORK_GET_INST,
            site::GET_INST_RETURN,
            site::FIRST_CALL,
            true,
        )?;
        Ok(unsafe { GlobalWork::at(f.x[0]) })
    }

    /// `_belomu_barrior_list[index]` as the compiled code reads it: x8 = the array, x9 = its length, then the byte in
    /// x8. A shorter array calls the index thrower.
    fn barrier_entry(
        self,
        f: &mut Frame,
        work: GlobalWork,
        index: u64,
    ) -> Result<bool, GuestException> {
        let list = work.belomu_barrior_list();
        let length = list.len();
        f.x[8] = list.address();
        f.x[9] = u64::from(length);
        if u64::from(length) <= index {
            // The thrower raises; if it returned normally the compiled function would simply end.
            let _ = f.call(
                callee::INDEX_THROWER,
                site::THROW_RETURN,
                site::THROW,
                false,
            );
            return Err(GuestException);
        }
        let entry = list.element(index);
        f.x[8] = u64::from(entry);
        Ok(entry != 0)
    }

    /// `x8 = barriered`, `x9 = skill id`, and the loop back-edge safepoint that follows.
    fn poll_after_test(f: &mut Frame, barriered: bool, skill_id: i32) {
        f.x[8] = u64::from(barriered);
        f.x[9] = u64::from(skill_id as u32);
        let flushed = [(8, f.x[8]), (9, f.x[9]), (10, f.x[10]), (11, f.x[11])];
        f.regs.safepoint_n(site::POLL_AFTER_TEST, &flushed);
    }

    /// The element and skill id tests: does a barrier absorb this hit?
    fn barrier_absorbs(
        self,
        skill: SkillObject,
        work: GlobalWork,
        f: &mut Frame,
    ) -> Result<bool, GuestException> {
        let element = skill.element();
        let skill_id = skill.skill_id();
        f.x[8] = u64::from(element);
        let mut barriered = false;
        if element <= element::ICE {
            // `adrp/add x9`, `adr x10`, `ldrb w11, [x9, x8]`, `add x10, x10, x11, lsl #2`, `br x10`
            let entry = JUMP_TABLE_BYTES[element as usize];
            f.x[9] = JUMP_TABLE;
            f.x[11] = u64::from(entry);
            f.x[10] = JUMP_BASE + 4 * u64::from(entry);
            let calc_type = skill.calc_type();
            if element == element::NORMAL {
                f.x[8] = u64::from(calc_type);
            }
            match ElementRoute::of(element, calc_type) {
                ElementRoute::List(index) => {
                    barriered = self.barrier_entry(f, work, index)?;
                    Self::poll_after_test(f, barriered, skill_id);
                }
                ElementRoute::JumpUnlessKaitenShoes => {
                    let acter = self.acter();
                    f.x[8] = acter.address();
                    let shoes = acter.is_equip_kaiten_shoes_byte();
                    f.x[8] = u64::from(shoes);
                    if shoes != 0 {
                        return Ok(false);
                    }
                    barriered = self.barrier_entry(f, work, barrier_index::JUMP)?;
                    Self::poll_after_test(f, barriered, skill_id);
                }
                ElementRoute::None => {
                    f.x[8] = 0;
                    f.x[9] = u64::from(skill_id as u32);
                }
            }
        } else {
            f.x[8] = 0;
            f.x[9] = u64::from(skill_id as u32);
        }
        // From here x8 is `barriered` (0 or 1) and x9 the skill id.
        match SkillRoute::of(skill_id) {
            SkillRoute::SkillOne => self.barrier_entry(f, work, barrier_index::SKILL_ONE),
            SkillRoute::Special => {
                skill_id_registers(f, skill_id);
                self.barrier_entry(f, work, barrier_index::SPECIAL)
            }
            SkillRoute::Element => {
                skill_id_registers(f, skill_id);
                Ok(barriered)
            }
        }
    }

    /// A barrier absorbed the hit: `ratio = 0`, and every hit number of the target's result logger becomes 0.
    fn zero_hit_numbers(self, ratio: u64, f: &mut Frame) -> Result<(), GuestException> {
        prestate::store_u32(ratio, 0);
        let target = self.target();
        f.x[8] = target.address();
        let logger = target.result_logger();
        f.x[8] = logger.address();
        let Some(numbers) = logger.multi_number() else {
            f.x[21] = 0;
            f.leave(site::RETURN);
            return Ok(());
        };
        f.x[21] = numbers.address();
        // the generic context of `IsReferenceOrContainsReferences<int>`
        let context = unsafe { guest_ld64(guest_ld64(slots::LIST_CONTEXT)) };
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
            callee::IS_REFERENCE_OR_CONTAINS_REFERENCES,
            site::REFERENCES_RETURN,
            site::REFERENCES,
            false,
        )?;
        if f.x[0] & 1 == 0 {
            prestate::store_u32(numbers.size_address(), 0);
        } else {
            f.x[2] = u64::from(count);
            prestate::store_u32(numbers.size_address(), 0);
            if (count as i32) >= 1 {
                f.x[0] = unsafe { guest_ld64(numbers.items_address()) };
                f.x[1] = 0;
                f.x[3] = 0;
                f.call(callee::ARRAY_CLEAR, site::CLEAR_RETURN, site::CLEAR, false)?;
            }
        }
        if (count as i32) < 1 {
            f.leave(site::RETURN);
            return Ok(());
        }
        f.x[21] = unsafe { guest_ld64(slots::LIST_ADD_METHOD) };
        // The list header as it stands after the stores above: the `Add` callee edits it (and the comparison gives back
        // what it left), so the port carries it from call to call.
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
                callee::LIST_INT_ADD,
                site::ADD_RETURN,
                site::ADD,
                false,
                &mut header,
            )?;
            remaining = remaining.wrapping_sub(1);
            f.x[20] = u64::from(remaining);
            f.regs.safepoint(site::POLL_IN_LOOP, 20, f.x[20]);
            if remaining == 0 {
                break;
            }
        }
        f.leave(site::RETURN);
        Ok(())
    }
}

// ---- Shim ----

/// 0x71021f5c50 `SkillExecter.calc_belome_barrior_ratio` (replay row with a store journal and an out footprint callee).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_skill_calc_belome_barrior_ratio(x: *mut u64) {
    unsafe {
        let regs = Regs::from_raw(x);
        let (this, skill, ratio) = (regs.x(0), regs.x(1), regs.x(2));
        let mut frame = Frame::new(regs);
        let _ = SkillExecter::at(this).calc_belome_barrior_ratio(
            SkillObject::at(skill),
            ratio,
            &mut frame,
        );
    }
}

/// Pre-state hook: the cells the port reads after the lifted body may have changed them, and the ones it stores to.
/// Returns 1 for the first run in a process (the lazy-initialization flag clear, the metadata slots unresolved).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_skill_calc_belome_barrior_ratio_pre(x: *mut u64) -> i32 {
    unsafe {
        let flag = guest_ld8(slots::INIT_FLAG);
        prestate::snapshot(slots::INIT_FLAG, 1, true);
        if flag & 1 == 0 {
            return 1;
        }
        let regs = Regs::from_raw(x);
        let this = SkillExecter::at(regs.x(0));
        prestate::snapshot(regs.x(2), 4, true);
        let target = this.target();
        if target.is_null() || target.character_id() != BELOME_CHARACTER_ID {
            return 0;
        }
        let class = guest_ld64(guest_ld64(slots::GLOBAL_WORK_CLASS));
        prestate::snapshot(class.wrapping_add(0xe0), 4, false);
        let logger = target.result_logger();
        if logger.is_null() {
            return 0;
        }
        if let Some(numbers) = logger.multi_number() {
            prestate::snapshot(numbers.items_address(), 8, false);
            prestate::snapshot(numbers.size_address(), 4, true);
            prestate::snapshot(numbers.version_address(), 4, true);
        }
        0
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn element_route_follows_the_compiled_jump_table() {
        assert_eq!(ElementRoute::of(0, 2), ElementRoute::List(4));
        assert_eq!(ElementRoute::of(0, 1), ElementRoute::None);
        assert_eq!(ElementRoute::of(1, 2), ElementRoute::JumpUnlessKaitenShoes);
        assert_eq!(ElementRoute::of(2, 0), ElementRoute::List(3));
        assert_eq!(ElementRoute::of(3, 0), ElementRoute::List(2));
        assert_eq!(ElementRoute::of(4, 0), ElementRoute::List(5));
        assert_eq!(ElementRoute::of(5, 2), ElementRoute::None);
        assert_eq!(ElementRoute::of(u32::MAX, 2), ElementRoute::None);
    }

    #[test]
    fn skill_route_covers_the_signed_split_at_0x9f_and_the_bit_mask() {
        assert_eq!(SkillRoute::of(1), SkillRoute::SkillOne);
        assert_eq!(SkillRoute::of(0x9e), SkillRoute::Special);
        assert_eq!(SkillRoute::of(0x9f), SkillRoute::Special);
        assert_eq!(SkillRoute::of(0x9d), SkillRoute::Element);
        assert_eq!(SkillRoute::of(0x2ba), SkillRoute::Special);
        assert_eq!(SkillRoute::of(0x2c3), SkillRoute::Special);
        assert_eq!(SkillRoute::of(0x2c9), SkillRoute::Special);
        assert_eq!(SkillRoute::of(0x2bb), SkillRoute::Element);
        assert_eq!(SkillRoute::of(0x2ca), SkillRoute::Element);
        assert_eq!(SkillRoute::of(i32::MIN), SkillRoute::Element);
        assert_eq!(SkillRoute::of(-1), SkillRoute::Element);
        assert_eq!(SkillRoute::of(0x9f + 1), SkillRoute::Element);
    }

    #[test]
    fn jump_table_matches_the_dispatch_addresses() {
        let targets: [u64; 5] =
            core::array::from_fn(|i| JUMP_BASE + 4 * u64::from(JUMP_TABLE_BYTES[i]));
        assert_eq!(
            targets,
            [
                0x71021f5d04,
                0x71021f5db0,
                0x71021f5d80,
                0x71021f5d98,
                0x71021f5d68
            ]
        );
    }
}
