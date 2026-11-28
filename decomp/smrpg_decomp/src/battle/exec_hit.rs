//! `Battle.SkillExecter.is_hit_calc_succeed` (0x71021fe9d0), the third callee of `exec_each_targets` ported (task-65).
//!
//! In C# terms (names are the IL2CPP ones; the cells and callees the metadata dump does not name are named by what
//! they do):
//!
//! ```text
//! bool SkillExecter.is_hit_calc_succeed() {
//!     float cell_18 = 0, cell_1c;                           // two stack floats the debug lines pass by reference
//!     <lazy metadata initialization: 11 slots>
//!     if (in_range(_skill_obj._skill_id, 0x2c3, 0x2c5) && _target.<+0x8c> < 1) return false;
//!     <GlobalWork class init>;
//!     if (GlobalWork.get_inst().<+0x61>) return true;
//!     if (<singleton>().<+0x5c>) return true;
//!     BattleManager <class init, static field metadata>;    // lazy
//!     var manager = BattleManager._instance;
//!     if (manager.<+0x168> == 0x7db && manager.<+0x60>.<+0x30> && _skill_obj._skill_id == 0x2ba) return true;
//!     // the hit rate in percent: 100 for skill 1, the skill's own integer otherwise
//!     cell_1c = _skill_obj._skill_id == 1 ? 100.0f : (float)_skill_obj.<+0x34>;
//!     _monitor.write(<literal> + cell_1c.ToString());
//!     if (_skill_obj._skill_id == 1 && !_target.<+0x6c>) {
//!         cell_18 = _target.<+0x58 float>;
//!         _monitor.write(<literal> + cell_18.ToString());
//!         _monitor.write(<literal>);
//!         cell_1c = cell_1c + cell_18 * -0.5f;
//!         _monitor.write(<literal> + cell_1c.ToString());
//!     }
//!     var mode = <singleton>().<+0x68>;
//!     if (mode == 1 || mode == 2) {                          // forced rate: 100 for 1, 0 for 2, with a debug line
//!         cell_1c = mode == 1 ? 100.0f : 0.0f;
//!         <debug float>(_monitor, 0, cell_1c); _monitor.write(<literal for the mode>);
//!     }
//!     if (cell_1c >= 100.0f) { _monitor.write(<literal>); return true; }
//!     bool roll = <random check>(_monitor, cell_1c, 100.0f);
//!     if (_target._character_id == 0x434 && _skill_obj._skill_id == 1) _monitor.write(<literal>);   // forces a miss
//!     else if (roll) return true;
//!     _monitor.write(<literal>);
//!     _target._result_logger.<+0x40> = true;                 // the miss flag
//!     return false;
//! }
//! ```
//!
//! The function keeps the lifted register residue (see `Frame`). The first run of a process (the lazy-initialization flag
//! byte clear: the metadata slots are still tokens) is deferred in `ab` mode like its siblings'; the port runs it in `on` mode.
//! Every float operation goes through `Regs` (`f32_from_i32`, `f32_arith`, `f32_compare`), so results and FPSR flags are the
//! lifted helpers' (checked per operation in `ab` mode by the fp shadow).

use super::damage::SkillExecter;
use super::frame::Frame;
use super::{GuestException, GuestObject, Regs};
use crate::bridge::prestate;
use crate::{guest_ld8, guest_ld32, guest_ld64};

const FLAG_PAGE: u64 = 0x7103cba000;
const LITERAL_PAGE: u64 = 0x7103a83000;

mod callee {
    pub const INITIALIZE_METADATA: u64 = 0x71000c4150;
    pub const CLASS_INITIALIZER: u64 = 0x71000c4330;
    pub const GET_INST: u64 = 0x7101886a00;
    pub const IN_RANGE: u64 = 0x71018ba2c0;
    pub const SINGLETON: u64 = 0x7101892aa0;
    pub const FLOAT_TO_STRING: u64 = 0x7101d6a050;
    pub const STRING_CONCAT_2: u64 = 0x71020147d0;
    pub const DEBUG_WRITE: u64 = 0x71018bebc0;
    pub const DEBUG_FLOAT: u64 = 0x71018bebf0;
    pub const RANDOM_CHECK: u64 = 0x71018bfce0;
}

mod cells {
    pub const FLAG: u64 = super::FLAG_PAGE + 0xd70;
    /// Slot (data page + offset) holding the `BattleManager` class cell.
    pub const CLASS_SLOT: u64 = 0x7103a57000 + 0xe48;
    /// Slot holding the pointer to the `GlobalWork` class cell.
    pub const GLOBAL_SLOT: u64 = 0x7103a58000 + 0x48;
    /// Pointer to the byte that says the static-field metadata of this method has been initialized.
    pub const STATIC_FLAG_POINTER: u64 = 0x7103a58000 + 0x1d0;
    /// The nine literal slots, lowest first; the lazy initialization resolves them after the two class slots.
    pub const LITERALS: [u64; 9] = [
        super::LITERAL_PAGE + 0xaf8,
        super::LITERAL_PAGE + 0xb00,
        super::LITERAL_PAGE + 0xb08,
        super::LITERAL_PAGE + 0xb10,
        super::LITERAL_PAGE + 0xb18,
        super::LITERAL_PAGE + 0xb20,
        super::LITERAL_PAGE + 0xb28,
        super::LITERAL_PAGE + 0xb30,
        super::LITERAL_PAGE + 0xb38,
    ];
    pub const LIT_MISS: u64 = super::LITERAL_PAGE + 0xaf8;
    pub const LIT_RATE: u64 = super::LITERAL_PAGE + 0xb00;
    pub const LIT_LINE: u64 = super::LITERAL_PAGE + 0xb08;
    pub const LIT_RATE_SUM: u64 = super::LITERAL_PAGE + 0xb10;
    pub const LIT_CERTAIN: u64 = super::LITERAL_PAGE + 0xb18;
    pub const LIT_MODE_ONE: u64 = super::LITERAL_PAGE + 0xb20;
    pub const LIT_MODIFIER: u64 = super::LITERAL_PAGE + 0xb28;
    pub const LIT_FORCED_MISS: u64 = super::LITERAL_PAGE + 0xb30;
    pub const LIT_MODE_TWO: u64 = super::LITERAL_PAGE + 0xb38;
}

mod field {
    pub const MONITOR: u64 = 0x10;
    pub const TARGET: u64 = 0x28;
    pub const SKILL_OBJ: u64 = 0x30;
    pub const SKILL_ID: u64 = 0x14;
    pub const SKILL_RATE: u64 = 0x34;
    pub const TARGET_CHARACTER_ID: u64 = 0x20;
    pub const TARGET_COUNTER: u64 = 0x8c;
    pub const TARGET_NO_MODIFIER: u64 = 0x6c;
    pub const TARGET_MODIFIER: u64 = 0x58;
    pub const RESULT_LOGGER: u64 = 0xb8;
    pub const LOGGER_MISS: u64 = 0x40;
    pub const CLASS_INITIALIZED: u64 = 0xe0;
    pub const CLASS_STATIC_FIELDS: u64 = 0xb8;
    pub const STATICS_INSTANCE: u64 = 0x10;
    pub const INSTANCE_STATE: u64 = 0x168;
    pub const INSTANCE_OBJECT: u64 = 0x60;
    pub const OBJECT_FLAG: u64 = 0x30;
    pub const GLOBAL_FLAG: u64 = 0x61;
    pub const SINGLETON_FLAG: u64 = 0x5c;
    pub const SINGLETON_MODE: u64 = 0x68;
}

mod value {
    pub const RANGE_LOW: u64 = 0x2c3;
    pub const RANGE_HIGH: u64 = 0x2c5;
    pub const MANAGER_STATE: u32 = 0x7db;
    pub const SKILL_CERTAIN: u32 = 0x2ba;
    pub const SKILL_BASIC: u32 = 1;
    pub const TARGET_ID: u32 = 0x434;
    pub const HUNDRED: u64 = 0x42c8_0000;
    pub const MINUS_HALF: u64 = 0xbf00_0000;
}

mod site {
    /// The eleven metadata initializations.
    pub const INIT: [u64; 11] = [
        0x71021fe9fc,
        0x71021fea08,
        0x71021fea14,
        0x71021fea20,
        0x71021fea2c,
        0x71021fea38,
        0x71021fea44,
        0x71021fea50,
        0x71021fea5c,
        0x71021fea68,
        0x71021fea74,
    ];
    pub const IN_RANGE: u64 = 0x71021fea98;
    pub const GET_INST_A: u64 = 0x71021feac8;
    pub const CLASS_INIT_GLOBAL: u64 = 0x71021fead8;
    pub const GET_INST_B: u64 = 0x71021feae0;
    pub const SINGLETON_A: u64 = 0x71021feaf0;
    pub const CLASS_INIT_A: u64 = 0x71021feb54;
    pub const CERTAIN_POLL: u64 = 0x71021feba8;
    pub const TO_STRING_RATE: u64 = 0x71021febe0;
    pub const CONCAT_RATE: u64 = 0x71021febf8;
    pub const DEBUG_RATE: u64 = 0x71021fec08;
    pub const TO_STRING_MODIFIER: u64 = 0x71021fec3c;
    pub const CONCAT_MODIFIER: u64 = 0x71021fec54;
    pub const DEBUG_MODIFIER: u64 = 0x71021fec64;
    pub const DEBUG_LINE: u64 = 0x71021fec7c;
    pub const TO_STRING_SUM: u64 = 0x71021feca0;
    pub const CONCAT_SUM: u64 = 0x71021fecb8;
    pub const DEBUG_SUM: u64 = 0x71021fecc8;
    pub const SINGLETON_B: u64 = 0x71021fecd0;
    pub const CLASS_INIT_B: u64 = 0x71021fecf8;
    pub const CLASS_INIT_B_POLL: u64 = 0x71021fed04;
    pub const METADATA_D: u64 = 0x71021fed14;
    pub const METADATA_D_POLL: u64 = 0x71021fed24;
    pub const METADATA_D_POLL_2: u64 = 0x71021fed28;
    pub const DEBUG_FLOAT: u64 = 0x71021fed4c;
    pub const DEBUG_MODE: u64 = 0x71021fed5c;
    pub const RANDOM_CHECK: u64 = 0x71021fed7c;
    pub const DEBUG_FORCED_MISS: u64 = 0x71021fedb4;
    pub const DEBUG_CERTAIN: u64 = 0x71021fedcc;
    pub const FORCED_MISS_POLL: u64 = 0x71021fedd0;
    pub const DEBUG_MISS: u64 = 0x71021fedec;
    pub const CLASS_INIT_I: u64 = 0x71021fee14;
    pub const CLASS_INIT_I_POLL: u64 = 0x71021fee18;
    pub const METADATA_K: u64 = 0x71021fee28;
    pub const METADATA_K_POLL: u64 = 0x71021fee38;
    pub const CLASS_INIT_M: u64 = 0x71021fee40;
    pub const CLASS_INIT_M_POLL: u64 = 0x71021fee44;
}

/// Where the code goes in the first group of `BattleManager` class checks (labels are the lifted block addresses).
enum Gate {
    /// 0xfeb34: the static-field flag byte.
    StaticFlag,
    /// 0xfeb44: the class word again.
    ClassWord,
    /// 0xfeb50: the class word was zero, run the class initializer.
    ClassInit,
    /// 0xfecf4: the first class word was zero, run the class initializer first, then test the flag byte.
    ClassInitFirst,
    /// 0xfed08: the static-field metadata is not resolved yet.
    Metadata,
}

/// The second group (the manager state is 0x7db).
enum Recheck {
    /// 0xfeb74: the flag byte.
    StaticFlag,
    /// 0xfeb7c: the class word.
    ClassWord,
    /// 0xfee10: the class word was zero.
    ClassInit,
    /// 0xfee1c: the flag byte was zero.
    Metadata,
    /// 0xfee3c: the class word is still zero.
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

fn live_u8(address: u64) -> u8 {
    if cfg!(test) {
        prestate::read_u8(address)
    } else {
        unsafe { guest_ld8(address) }
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

/// Which way the function returns.
enum Verdict {
    Hit,
    Miss,
}

impl SkillExecter {
    /// `is_hit_calc_succeed()`. `f` is the register file at entry; the result is in x0 (1 hit, 0 miss).
    pub fn is_hit_calc_succeed(self, f: &mut Frame) -> Result<(), GuestException> {
        use field::*;
        // prologue: `stp x29, x30, [sp, #-0x30]!`, saves x21, x20, x19, `mov x29, sp`
        f.x[31] = f.entry[31].wrapping_sub(0x30);
        f.x[29] = f.x[31];
        let cell_18 = f.x[29].wrapping_add(0x18);
        let cell_1c = f.x[29].wrapping_add(0x1c);
        f.x[20] = FLAG_PAGE;
        f.x[8] = u64::from(prestate::read_u8(cells::FLAG));
        f.x[19] = f.x[0];
        if f.x[8] & 1 == 0 {
            let slots = [cells::CLASS_SLOT, cells::GLOBAL_SLOT]
                .into_iter()
                .chain(cells::LITERALS);
            for (n, slot) in slots.enumerate() {
                f.x[0] = prestate::read_u64(slot);
                if n == 0 {
                    f.call(
                        callee::INITIALIZE_METADATA,
                        site::INIT[n],
                        &[0, 8, 19, 20, 29],
                        true,
                    )?;
                } else {
                    f.call(callee::INITIALIZE_METADATA, site::INIT[n], &[0], false)?;
                }
            }
            f.x[8] = 1;
            prestate::store_u8(cells::FLAG, 1);
        }
        // `str xzr, [x29, #0x18]`: the journal matches cells by address and size, so it is two four-byte zeros
        prestate::store_u32(cell_18, 0);
        prestate::store_u32(cell_18.wrapping_add(4), 0);
        f.x[8] = ld64(f.x[19].wrapping_add(SKILL_OBJ));
        f.x[0] = w(ld32(f.x[8].wrapping_add(SKILL_ID)));
        f.x[1] = value::RANGE_LOW;
        f.x[2] = value::RANGE_HIGH;
        f.x[3] = 0;
        f.call(
            callee::IN_RANGE,
            site::IN_RANGE,
            &[0, 1, 2, 3, 8, 19, 20, 29],
            true,
        )?;
        f.x[8] = f.regs.x(8);
        f.x[9] = f.regs.x(9);
        if f.x[0] & 1 != 0 {
            f.x[8] = ld64(f.x[19].wrapping_add(TARGET));
            f.x[8] = w(ld32(f.x[8].wrapping_add(TARGET_COUNTER)));
            if (f.x[8] as u32 as i32) < 1 {
                return self.leave(f, Verdict::Miss);
            }
        }
        // the flags of GlobalWork and the singleton: a set one is a sure hit
        f.x[8] = 0x7103a58000;
        f.x[8] = live_u64(f.x[8].wrapping_add(0x48));
        f.x[0] = ld64(f.x[8]);
        f.x[8] = w(prestate::read_u32(f.x[0].wrapping_add(CLASS_INITIALIZED)));
        if f.x[8] as u32 == 0 {
            f.call(
                callee::CLASS_INITIALIZER,
                site::CLASS_INIT_GLOBAL,
                &[0, 8],
                false,
            )?;
            f.x[0] = 0;
            f.call(callee::GET_INST, site::GET_INST_B, &[0], false)?;
        } else {
            f.x[0] = 0;
            f.call(callee::GET_INST, site::GET_INST_A, &[0, 8], false)?;
        }
        f.x[8] = u64::from(live_u8(f.x[0].wrapping_add(GLOBAL_FLAG)));
        if f.x[8] as u32 != 0 {
            return self.leave(f, Verdict::Hit);
        }
        f.x[0] = 0;
        f.call(callee::SINGLETON, site::SINGLETON_A, &[0, 8], false)?;
        f.x[8] = u64::from(live_u8(f.x[0].wrapping_add(SINGLETON_FLAG)));
        if f.x[8] as u32 != 0 {
            return self.leave(f, Verdict::Hit);
        }
        // BattleManager: class word, static-field flag byte, class word
        f.x[20] = 0x7103a57000;
        f.x[20] = live_u64(f.x[20].wrapping_add(0xe48));
        f.x[0] = live_u64(f.x[20]);
        f.x[8] = w(prestate::read_u32(f.x[0].wrapping_add(CLASS_INITIALIZED)));
        let mut gate = if f.x[8] as u32 == 0 {
            Gate::ClassInitFirst
        } else {
            Gate::StaticFlag
        };
        loop {
            match gate {
                Gate::StaticFlag => {
                    f.x[21] = 0x7103a58000;
                    f.x[21] = live_u64(f.x[21].wrapping_add(0x1d0));
                    f.x[8] = u64::from(prestate::read_u8(f.x[21]));
                    gate = if f.x[8] as u32 == 0 {
                        Gate::Metadata
                    } else {
                        Gate::ClassWord
                    };
                }
                Gate::ClassWord => {
                    f.x[0] = live_u64(f.x[20]);
                    f.x[8] = w(live_u32(f.x[0].wrapping_add(CLASS_INITIALIZED)));
                    if f.x[8] as u32 != 0 {
                        break;
                    }
                    gate = Gate::ClassInit;
                }
                Gate::ClassInit => {
                    f.call(
                        callee::CLASS_INITIALIZER,
                        site::CLASS_INIT_A,
                        &[0, 8, 20, 21],
                        false,
                    )?;
                    f.x[0] = live_u64(f.x[20]);
                    break;
                }
                Gate::ClassInitFirst => {
                    f.call(
                        callee::CLASS_INITIALIZER,
                        site::CLASS_INIT_B,
                        &[0, 8, 20],
                        false,
                    )?;
                    f.x[21] = 0x7103a58000;
                    f.x[21] = live_u64(f.x[21].wrapping_add(0x1d0));
                    f.x[8] = u64::from(prestate::read_u8(f.x[21]));
                    f.regs
                        .safepoint_n(site::CLASS_INIT_B_POLL, &[(8, f.x[8]), (21, f.x[21])]);
                    gate = if f.x[8] as u32 != 0 {
                        Gate::ClassWord
                    } else {
                        Gate::Metadata
                    };
                }
                Gate::Metadata => {
                    f.x[0] = prestate::read_u64(cells::CLASS_SLOT);
                    f.call(
                        callee::INITIALIZE_METADATA,
                        site::METADATA_D,
                        &[0, 8, 20, 21],
                        false,
                    )?;
                    f.x[8] = 1;
                    prestate::store_u8(f.x[21], 1);
                    f.x[0] = live_u64(f.x[20]);
                    f.x[8] = w(live_u32(f.x[0].wrapping_add(CLASS_INITIALIZED)));
                    f.regs
                        .safepoint_n(site::METADATA_D_POLL, &[(0, f.x[0]), (8, f.x[8])]);
                    if f.x[8] as u32 != 0 {
                        break;
                    }
                    f.regs
                        .safepoint_n(site::METADATA_D_POLL_2, &[(0, f.x[0]), (8, f.x[8])]);
                    gate = Gate::ClassInit;
                }
            }
        }
        // the manager's state: 0x7db with a flag on its object makes skill 0x2ba a sure hit
        f.x[8] = ld64(f.x[0].wrapping_add(CLASS_STATIC_FIELDS));
        f.x[8] = ld64(f.x[8].wrapping_add(STATICS_INSTANCE));
        f.x[8] = w(ld32(f.x[8].wrapping_add(INSTANCE_STATE)));
        if f.x[8] as u32 == value::MANAGER_STATE {
            f.x[8] = w(live_u32(f.x[0].wrapping_add(CLASS_INITIALIZED)));
            let mut recheck = if f.x[8] as u32 == 0 {
                Recheck::ClassInit
            } else {
                Recheck::StaticFlag
            };
            loop {
                match recheck {
                    Recheck::StaticFlag => {
                        f.x[8] = u64::from(live_u8(f.x[21]));
                        recheck = if f.x[8] as u32 == 0 {
                            Recheck::Metadata
                        } else {
                            Recheck::ClassWord
                        };
                    }
                    Recheck::ClassWord => {
                        f.x[0] = live_u64(f.x[20]);
                        f.x[8] = w(live_u32(f.x[0].wrapping_add(CLASS_INITIALIZED)));
                        if f.x[8] as u32 != 0 {
                            break;
                        }
                        recheck = Recheck::ClassInitAgain;
                    }
                    Recheck::ClassInit => {
                        f.call(
                            callee::CLASS_INITIALIZER,
                            site::CLASS_INIT_I,
                            &[0, 8, 20, 21],
                            false,
                        )?;
                        f.x[8] = u64::from(live_u8(f.x[21]));
                        f.regs.safepoint_n(site::CLASS_INIT_I_POLL, &[(8, f.x[8])]);
                        recheck = if f.x[8] as u32 != 0 {
                            Recheck::ClassWord
                        } else {
                            Recheck::Metadata
                        };
                    }
                    Recheck::Metadata => {
                        f.x[0] = prestate::read_u64(cells::CLASS_SLOT);
                        f.call(
                            callee::INITIALIZE_METADATA,
                            site::METADATA_K,
                            &[0, 8, 20, 21],
                            false,
                        )?;
                        f.x[8] = 1;
                        prestate::store_u8(f.x[21], 1);
                        f.x[0] = live_u64(f.x[20]);
                        f.x[8] = w(live_u32(f.x[0].wrapping_add(CLASS_INITIALIZED)));
                        f.regs
                            .safepoint_n(site::METADATA_K_POLL, &[(0, f.x[0]), (8, f.x[8])]);
                        if f.x[8] as u32 != 0 {
                            break;
                        }
                        recheck = Recheck::ClassInitAgain;
                    }
                    Recheck::ClassInitAgain => {
                        f.call(
                            callee::CLASS_INITIALIZER,
                            site::CLASS_INIT_M,
                            &[0, 8, 20, 21],
                            false,
                        )?;
                        f.x[0] = live_u64(f.x[20]);
                        f.regs.safepoint_n(site::CLASS_INIT_M_POLL, &[(0, f.x[0])]);
                        break;
                    }
                }
            }
            f.x[8] = ld64(f.x[0].wrapping_add(CLASS_STATIC_FIELDS));
            f.x[8] = ld64(f.x[8].wrapping_add(STATICS_INSTANCE));
            f.x[8] = ld64(f.x[8].wrapping_add(INSTANCE_OBJECT));
            f.x[8] = u64::from(unsafe { guest_ld8(f.x[8].wrapping_add(OBJECT_FLAG)) });
            if f.x[8] as u32 != 0 {
                f.x[8] = ld64(f.x[19].wrapping_add(SKILL_OBJ));
                f.x[8] = w(ld32(f.x[8].wrapping_add(SKILL_ID)));
                f.regs.safepoint_n(
                    site::CERTAIN_POLL,
                    &[(8, f.x[8]), (21, f.x[21]), (0, 0), (20, 0)],
                );
                if f.x[8] as u32 == value::SKILL_CERTAIN {
                    return self.leave(f, Verdict::Hit);
                }
            }
        }
        self.rate_and_roll(f, cell_18, cell_1c)
    }

    /// The hit rate in percent with its debug lines, the forced modes, and the roll (everything after the sure-hit tests).
    fn rate_and_roll(
        self,
        f: &mut Frame,
        cell_18: u64,
        cell_1c: u64,
    ) -> Result<(), GuestException> {
        use field::*;
        // the rate: 100 for skill 1, otherwise the skill's integer field converted to float
        f.x[8] = ld64(f.x[19].wrapping_add(SKILL_OBJ));
        f.x[0] = cell_1c;
        f.x[1] = 0;
        let raw = ld32(f.x[8].wrapping_add(SKILL_RATE));
        let skill_id = ld32(f.x[8].wrapping_add(SKILL_ID));
        f.x[8] = w(skill_id);
        let rate = f.regs.f32_from_i32(raw as i32);
        f.set_s(0, rate);
        f.x[8] = value::HUNDRED;
        f.v[1] = (value::HUNDRED, 0);
        // `fcsel s0, s1, s0, eq` on the compare of the skill id with 1
        if skill_id == value::SKILL_BASIC {
            f.v[0] = f.v[1];
        }
        prestate::store_u32(cell_1c, f.v[0].0 as u32);
        f.x[20] = ld64(f.x[19].wrapping_add(MONITOR));
        let mut area = prestate::read_u32(cell_1c).to_le_bytes();
        f.call_out_v(
            callee::FLOAT_TO_STRING,
            site::TO_STRING_RATE,
            &[0, 1, 8, 20, 21],
            &[0, 1],
            false,
            &mut area,
        )?;
        self.literal_line(f, cells::LIT_RATE, site::CONCAT_RATE, site::DEBUG_RATE)?;
        // skill 1 against a target without the flag: the target's modifier lowers the rate by half of itself
        f.x[8] = ld64(f.x[19].wrapping_add(SKILL_OBJ));
        f.x[8] = w(ld32(f.x[8].wrapping_add(SKILL_ID)));
        if f.x[8] as u32 == value::SKILL_BASIC {
            f.x[8] = ld64(f.x[19].wrapping_add(TARGET));
            f.x[9] = u64::from(unsafe { guest_ld8(f.x[8].wrapping_add(TARGET_NO_MODIFIER)) });
            if f.x[9] as u32 == 0 {
                f.x[8] = w(ld32(f.x[8].wrapping_add(TARGET_MODIFIER)));
                prestate::store_u32(cell_18, f.x[8] as u32);
                f.x[20] = ld64(f.x[19].wrapping_add(MONITOR));
                f.x[0] = cell_18;
                f.x[1] = 0;
                let mut area = prestate::read_u32(cell_18).to_le_bytes();
                f.call_out(
                    callee::FLOAT_TO_STRING,
                    site::TO_STRING_MODIFIER,
                    &[0, 1, 8, 9, 20],
                    false,
                    &mut area,
                )?;
                self.literal_line(
                    f,
                    cells::LIT_MODIFIER,
                    site::CONCAT_MODIFIER,
                    site::DEBUG_MODIFIER,
                )?;
                f.x[8] = LITERAL_PAGE;
                f.x[0] = ld64(f.x[19].wrapping_add(MONITOR));
                f.x[2] = 0;
                f.x[8] = live_u64(f.x[8].wrapping_add(cells::LIT_LINE - LITERAL_PAGE));
                f.x[1] = ld64(f.x[8]);
                f.call(callee::DEBUG_WRITE, site::DEBUG_LINE, &[0, 1, 2, 8], false)?;
                f.v[1] = (u64::from(prestate::read_u32(cell_18)), 0);
                f.v[0] = (u64::from(prestate::read_u32(cell_1c)), 0);
                f.v[2] = (value::MINUS_HALF, 0);
                let product = f.regs.f32_arith(0, f.s(1), f.s(2));
                f.set_s(1, product);
                let sum = f.regs.f32_arith(2, f.s(0), f.s(1));
                f.set_s(0, sum);
                prestate::store_u32(cell_1c, f.v[0].0 as u32);
                f.x[20] = ld64(f.x[19].wrapping_add(MONITOR));
                f.x[0] = cell_1c;
                f.x[1] = 0;
                let mut area = prestate::read_u32(cell_1c).to_le_bytes();
                f.call_out_v(
                    callee::FLOAT_TO_STRING,
                    site::TO_STRING_SUM,
                    &[0, 1, 20],
                    &[0, 1, 2],
                    false,
                    &mut area,
                )?;
                self.literal_line(f, cells::LIT_RATE_SUM, site::CONCAT_SUM, site::DEBUG_SUM)?;
            }
        }
        // the singleton's mode 1 forces 100 and mode 2 forces 0, each with a debug line
        f.x[0] = 0;
        f.call(callee::SINGLETON, site::SINGLETON_B, &[0, 8, 9], false)?;
        f.x[8] = w(ld32(f.x[0].wrapping_add(SINGLETON_MODE)));
        let mode = f.x[8] as u32;
        if mode == 1 || mode == 2 {
            f.x[20] = LITERAL_PAGE;
            if mode == 1 {
                f.x[20] = live_u64(f.x[20].wrapping_add(cells::LIT_MODE_ONE - LITERAL_PAGE));
                f.x[8] = value::HUNDRED;
                f.v[0] = (value::HUNDRED, 0);
            } else {
                f.x[20] = live_u64(f.x[20].wrapping_add(cells::LIT_MODE_TWO - LITERAL_PAGE));
                f.v[0] = (0, 0);
            }
            prestate::store_u32(cell_1c, f.v[0].0 as u32);
            f.x[0] = ld64(f.x[19].wrapping_add(MONITOR));
            f.x[1] = 0;
            f.call_v(
                callee::DEBUG_FLOAT,
                site::DEBUG_FLOAT,
                &[0, 1, 8, 20],
                &[0],
                false,
            )?;
            f.x[0] = ld64(f.x[19].wrapping_add(MONITOR));
            f.x[2] = 0;
            f.x[1] = ld64(f.x[20]);
            f.call(callee::DEBUG_WRITE, site::DEBUG_MODE, &[0, 1, 2], false)?;
        }
        // the rate against 100
        f.x[8] = value::HUNDRED;
        f.v[0] = (u64::from(prestate::read_u32(cell_1c)), 0);
        f.x[0] = ld64(f.x[19].wrapping_add(MONITOR));
        f.v[1] = (value::HUNDRED, 0);
        let flags = f.regs.f32_compare(f.s(0), f.s(1));
        if flags.n == flags.v {
            // 100 or more: a sure hit
            f.x[8] = LITERAL_PAGE;
            f.x[8] = live_u64(f.x[8].wrapping_add(cells::LIT_CERTAIN - LITERAL_PAGE));
            f.x[1] = ld64(f.x[8]);
            f.x[2] = 0;
            f.call_v(
                callee::DEBUG_WRITE,
                site::DEBUG_CERTAIN,
                &[0, 1, 2, 8],
                &[0, 1],
                false,
            )?;
            f.regs.safepoint_n(site::DEBUG_CERTAIN, &[]);
            return self.leave(f, Verdict::Hit);
        }
        f.x[1] = 0;
        f.call_v(
            callee::RANDOM_CHECK,
            site::RANDOM_CHECK,
            &[0, 1, 8],
            &[0, 1],
            false,
        )?;
        f.x[8] = ld64(f.x[19].wrapping_add(TARGET));
        f.x[8] = w(ld32(f.x[8].wrapping_add(TARGET_CHARACTER_ID)));
        let mut forced = false;
        if f.x[8] as u32 == value::TARGET_ID {
            f.x[8] = ld64(f.x[19].wrapping_add(SKILL_OBJ));
            f.x[8] = w(ld32(f.x[8].wrapping_add(SKILL_ID)));
            forced = f.x[8] as u32 == value::SKILL_BASIC;
        }
        if forced {
            f.x[8] = LITERAL_PAGE;
            f.x[0] = ld64(f.x[19].wrapping_add(MONITOR));
            f.x[2] = 0;
            f.x[8] = live_u64(f.x[8].wrapping_add(cells::LIT_FORCED_MISS - LITERAL_PAGE));
            f.x[1] = ld64(f.x[8]);
            f.call(
                callee::DEBUG_WRITE,
                site::DEBUG_FORCED_MISS,
                &[0, 1, 2, 8],
                false,
            )?;
        } else {
            f.regs.safepoint_n(site::FORCED_MISS_POLL, &[(8, f.x[8])]);
            if f.x[0] & 1 != 0 {
                return self.leave(f, Verdict::Hit);
            }
        }
        // a miss: a debug line and the miss flag in the target's result logger
        f.x[8] = LITERAL_PAGE;
        f.x[0] = ld64(f.x[19].wrapping_add(MONITOR));
        f.x[2] = 0;
        f.x[8] = live_u64(f.x[8].wrapping_add(cells::LIT_MISS - LITERAL_PAGE));
        f.x[1] = ld64(f.x[8]);
        f.call(callee::DEBUG_WRITE, site::DEBUG_MISS, &[0, 1, 2, 8], false)?;
        f.x[8] = ld64(f.x[19].wrapping_add(TARGET));
        f.x[9] = 1;
        f.x[0] = 0;
        f.x[8] = ld64(f.x[8].wrapping_add(RESULT_LOGGER));
        prestate::store_u8(f.x[8].wrapping_add(LOGGER_MISS), f.x[9] as u8);
        self.leave(f, Verdict::Miss)
    }

    /// `_monitor.write(<literal> + <string in x0>)`: the literal's slot, the concat, the debug call. x20 is the monitor.
    fn literal_line(
        self,
        f: &mut Frame,
        slot: u64,
        concat_site: u64,
        debug_site: u64,
    ) -> Result<(), GuestException> {
        f.x[8] = LITERAL_PAGE;
        f.x[8] = live_u64(f.x[8].wrapping_add(slot - LITERAL_PAGE));
        f.x[1] = f.x[0];
        f.x[0] = ld64(f.x[8]);
        f.x[2] = 0;
        f.call(callee::STRING_CONCAT_2, concat_site, &[0, 1, 2, 8], false)?;
        f.x[1] = f.x[0];
        f.x[0] = f.x[20];
        f.x[2] = 0;
        f.call(callee::DEBUG_WRITE, debug_site, &[0, 1, 2], false)
    }

    /// The epilogue shared by every exit: x19 to x21 and x29 and the sp come back; x0 is the result, x30 is not written.
    fn leave(self, f: &mut Frame, verdict: Verdict) -> Result<(), GuestException> {
        f.x[0] = match verdict {
            Verdict::Hit => 1,
            Verdict::Miss => 0,
        };
        f.leave(&[0, 19, 20, 21, 29]);
        Ok(())
    }
}

/// 0x71021fe9d0 `SkillExecter.is_hit_calc_succeed` (replay row with a store journal).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_skill_is_hit_calc_succeed(x: *mut u64) {
    unsafe {
        let regs = Regs::from_raw(x);
        let this = regs.x(0);
        let mut frame = Frame::new(regs);
        let _ = SkillExecter::at(this).is_hit_calc_succeed(&mut frame);
    }
}

/// Pre-state hook. Returns 1 for the first run in a process (the flag byte clear: the metadata slots are still tokens).
/// Otherwise the cells the port reads after callees may have changed (the two class words, the static-field flag byte)
/// and the cell it stores to (the miss flag).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_skill_is_hit_calc_succeed_pre(x: *mut u64) -> i32 {
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
        prestate::snapshot(class.wrapping_add(field::CLASS_INITIALIZED), 4, false);
        let flag_cell = guest_ld64(cells::STATIC_FLAG_POINTER);
        prestate::snapshot(flag_cell, 1, true);
        let target = guest_ld64(this.wrapping_add(field::TARGET));
        if target != 0 {
            let logger = guest_ld64(target.wrapping_add(field::RESULT_LOGGER));
            if logger != 0 {
                prestate::snapshot(logger.wrapping_add(field::LOGGER_MISS), 1, true);
            }
        }
        0
    }
}

#[cfg(test)]
mod tests;
