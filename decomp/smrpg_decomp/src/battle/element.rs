//! `BattleCharacterParam.calc_element(ElementKind)` (task-55): the elemental damage multiplier of a character.
//!
//! In C# terms: `_element._elements_state[kind]` is the character's state for that element (an `ElementState` int); a
//! state of 1 to 4 selects one of four multipliers from a constant table, any other state means 1.0. The damage code
//! compares the result with 1.0 and 2.0 (see `SkillExecter.damage_calc`). An index past the end of the array throws
//! through 0x71000c4510.

use super::{BattleCharacterParam, GuestException, GuestObject, Regs};
#[cfg(not(test))]
use crate::guest_ld32;
use crate::object::{Int32Array, guest_class};

guest_class!(
    /// `Battle.ElementManager`
    ElementManager
);
impl ElementManager {
    /// `_elements_state`: one `ElementState` int per element kind.
    const ELEMENTS_STATE: u64 = 0x10;

    fn elements_state(self) -> Int32Array {
        self.object_field(Self::ELEMENTS_STATE)
    }
}

impl BattleCharacterParam {
    const ELEMENT: u64 = 0xa8;

    fn element(self) -> ElementManager {
        self.object_field(Self::ELEMENT)
    }
}

/// The four multipliers (read-only data of the image), indexed by `state - 1`.
#[cfg(not(test))]
const ELEMENT_RATIO_TABLE: u64 = 0x7102af4000 + 0xfa0;

/// Entry `slot` of the table, as bits (unit tests substitute their own table).
fn table_entry(slot: u64) -> u32 {
    #[cfg(test)]
    {
        [0x3f00_0000, 0x4000_0000, 0, 0x3fc0_0000][slot as usize]
    }
    #[cfg(not(test))]
    unsafe {
        guest_ld32(ELEMENT_RATIO_TABLE + 4 * slot)
    }
}

/// The throw and the result, which the compiled code leaves in different registers.
pub trait ElementCalls {
    /// The index throw (0x71000c4510): `kind` is not below the array length. Always an exception.
    fn throw_index_out_of_range(&mut self, array: Int32Array, length: u32) -> GuestException;
    /// Return `ratio` in s0.
    fn leave(&mut self, ratio: f32);
}

impl BattleCharacterParam {
    /// `calc_element(kind)`: the multiplier for the character's state of element `kind`.
    pub fn calc_element<C: ElementCalls>(
        self,
        kind: i32,
        calls: &mut C,
    ) -> Result<f32, GuestException> {
        let states = self.element().elements_state();
        let length = states.len();
        // The compiled code compares the length with `kind` as unsigned 32-bit values.
        if u64::from(length) <= u64::from(kind as u32) {
            return Err(calls.throw_index_out_of_range(states, length));
        }
        let state = states.element(i64::from(kind) as u64);
        // `state - 1` as unsigned: states 1 to 4 index the table.
        let ratio = match (state as u32).wrapping_sub(1) {
            slot @ 0..=3 => f32::from_bits(table_entry(u64::from(slot))),
            _ => 1.0,
        };
        calls.leave(ratio);
        Ok(ratio)
    }
}

/// The method pushes 16 bytes (x29, x30), keeps x29 = sp, and flushes x8, x9, x29, x30 and sp only before the throw;
/// on a normal return only v0 changes (x29 and sp come back to their entry values).
struct LiftedElement {
    regs: Regs,
    entry_sp: u64,
}

impl ElementCalls for LiftedElement {
    fn throw_index_out_of_range(&mut self, array: Int32Array, length: u32) -> GuestException {
        let sp = self.entry_sp.wrapping_sub(16);
        self.regs.set_x(8, array.address());
        self.regs.set_x(9, u64::from(length));
        self.regs.set_x(29, sp);
        self.regs.set_x(30, 0x71021be414);
        self.regs.set_x(31, sp);
        // If the thrower returns without an exception the compiled function simply ends: no epilogue.
        let _ = self.regs.invoke(0x71000c4510);
        GuestException
    }

    fn leave(&mut self, ratio: f32) {
        self.regs.set_f32(0, ratio);
    }
}

/// 0x71021be3c0 `BattleCharacterParam.calc_element`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_param_calc_element(x: *mut u64) {
    unsafe {
        let regs = Regs::from_raw(x);
        let param = BattleCharacterParam::at(regs.x(0));
        let kind = regs.x(1) as i32;
        let entry_sp = regs.sp();
        let _ = param.calc_element(kind, &mut LiftedElement { regs, entry_sp });
    }
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use std::boxed::Box;
    use std::vec::Vec;

    struct Recorder(Vec<std::string::String>);

    impl ElementCalls for Recorder {
        fn throw_index_out_of_range(&mut self, _: Int32Array, length: u32) -> GuestException {
            self.0.push(std::format!("throw {length}"));
            GuestException
        }
        fn leave(&mut self, ratio: f32) {
            self.0.push(std::format!("leave {:#x}", ratio.to_bits()));
        }
    }

    #[test]
    fn element_state_selects_a_table_entry_or_one() {
        let mut graph = Box::new([0u64; 48]);
        let base = graph.as_ptr() as u64;
        graph[0xa8 / 8] = base + 8 * 24;
        graph[24 + 2] = base + 8 * 32;
        let bytes = graph.as_mut_ptr() as *mut u8;
        let param = unsafe { BattleCharacterParam::at(base) };
        for length in [0u32, 1, 3, 4, 9] {
            for state in [0u32, 1, 2, 3, 4, 5, 0xffff_ffff] {
                for kind in [-1i32, 0, 1, 2, 3, 8, 9] {
                    unsafe {
                        bytes
                            .add(8 * 32 + 0x18)
                            .cast::<u32>()
                            .write_unaligned(length);
                        for i in 0..12 {
                            bytes
                                .add(8 * 32 + 0x20 + 4 * i)
                                .cast::<u32>()
                                .write_unaligned(state);
                        }
                    }
                    let mut r = Recorder(Vec::new());
                    let result = param.calc_element(kind, &mut r);
                    if length as u64 <= u64::from(kind as u32) {
                        assert_eq!(result, Err(GuestException));
                        assert_eq!(r.0, [std::format!("throw {length}")]);
                    } else {
                        let expected = match state {
                            1 => 0x3f00_0000u32,
                            2 => 0x4000_0000,
                            3 => 0,
                            4 => 0x3fc0_0000,
                            _ => 0x3f80_0000,
                        };
                        assert_eq!(result.map(f32::to_bits), Ok(expected));
                        assert_eq!(r.0, [std::format!("leave {expected:#x}")]);
                    }
                }
            }
        }
    }
}
