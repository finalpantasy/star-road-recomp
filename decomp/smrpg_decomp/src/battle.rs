//! Battle classes (task-51): the first readable game logic. Names and offsets are IL2CPP metadata facts
//! (class `Battle.*`, field table of the 1.0.1 build), the same kind of fact `leaves.def` already carries.
//!
//! The readable part is the `impl` blocks: field accessors and the predicates the game computes from them.
//! The `smrpg_*` functions at the bottom are the shim: they map guest registers to those arguments and back,
//! and reproduce the register residue the lifted code leaves behind (see docs/DECOMP.md, "Readable ports").

use crate::object::{GuestObject, guest_class};

/// `Battle.BadStatusManager.BadStatusKind`, the raw 32-bit value (the game stores any int, so no Rust enum).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BadStatusKind(pub u32);

impl BadStatusKind {
    pub const NORMAL: Self = Self(0);
    pub const SLEEP: Self = Self(3);
    pub const MUSHROOM: Self = Self(6);
    pub const GONE: Self = Self(8);
    pub const DEAD: Self = Self(9);
}

guest_class!(
    /// `Battle.BadStatusManager`
    BadStatusManager
);
impl BadStatusManager {
    const STATUS: u64 = 0x10;

    pub fn status(self) -> BadStatusKind {
        BadStatusKind(self.u32_field(Self::STATUS))
    }

    /// A character can take a command unless it is asleep, a mushroom, dead or gone.
    pub fn can_command(self) -> bool {
        !matches!(
            self.status(),
            BadStatusKind::SLEEP
                | BadStatusKind::MUSHROOM
                | BadStatusKind::GONE
                | BadStatusKind::DEAD
        )
    }
}

guest_class!(
    /// `Battle.BuffManager`
    BuffManager
);
impl BuffManager {
    const INVINCIBLE_TURN: u64 = 0x28;

    /// Turns of invincibility left (signed, like the C# int).
    pub fn invincible_turn(self) -> i32 {
        self.i32_field(Self::INVINCIBLE_TURN)
    }
}

guest_class!(
    /// `Battle.BattleCharacterParam`
    BattleCharacterParam
);
impl BattleCharacterParam {
    const ACT_NUM: u64 = 0x5c;
    const IS_JOIN: u64 = 0x6e;
    const IS_BUNNAGE: u64 = 0x71;
    const CAN_TARGETTING: u64 = 0x78;
    const BADSTATUS_MNG: u64 = 0x98;
    const BUFF_MNG: u64 = 0xa0;

    pub fn act_num(self) -> i32 {
        self.i32_field(Self::ACT_NUM)
    }

    pub fn is_join(self) -> bool {
        self.bool_field(Self::IS_JOIN)
    }

    pub fn is_bunnage(self) -> bool {
        self.bool_field(Self::IS_BUNNAGE)
    }

    pub fn targetting_flag(self) -> bool {
        self.bool_field(Self::CAN_TARGETTING)
    }

    pub fn badstatus_mng(self) -> BadStatusManager {
        self.object_field(Self::BADSTATUS_MNG)
    }

    pub fn buff_mng(self) -> BuffManager {
        self.object_field(Self::BUFF_MNG)
    }

    pub fn status(self) -> BadStatusKind {
        self.badstatus_mng().status()
    }

    pub fn is_dead(self) -> bool {
        self.status() == BadStatusKind::DEAD
    }

    pub fn is_gone(self) -> bool {
        self.status() == BadStatusKind::GONE
    }

    pub fn is_invincible(self) -> bool {
        self.buff_mng().invincible_turn() > 0
    }

    /// In the party, not bunnaged, and neither dead nor gone.
    pub fn can_battle(self) -> bool {
        self.is_join() && !self.is_bunnage() && !self.is_dead() && !self.is_gone()
    }

    pub fn can_targetting(self) -> bool {
        self.targetting_flag() && self.can_battle()
    }

    /// In the party with at least one action left.
    pub fn can_act(self) -> bool {
        self.is_join() && self.act_num() > 0
    }

    /// Which test of `can_use_any_command` decided the answer. The variant also says how far the game's own
    /// evaluation got, which the shim needs to reproduce the registers the lifted code leaves behind.
    pub fn command_check(self) -> CommandCheck {
        if !self.is_join() {
            CommandCheck::NotJoined
        } else if self.is_bunnage() {
            CommandCheck::Bunnage
        } else if !self.badstatus_mng().can_command() {
            CommandCheck::BadStatus
        } else if self.act_num() <= 0 {
            CommandCheck::NoActionLeft
        } else {
            CommandCheck::Yes
        }
    }

    pub fn can_use_any_command(self) -> bool {
        self.command_check() == CommandCheck::Yes
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CommandCheck {
    NotJoined,
    Bunnage,
    BadStatus,
    NoActionLeft,
    Yes,
}

guest_class!(
    /// `Battle.BattleCharacter`
    BattleCharacter
);
impl BattleCharacter {
    const PARAM: u64 = 0x20;

    pub fn param(self) -> BattleCharacterParam {
        self.object_field(Self::PARAM)
    }
}

guest_class!(
    /// `Battle.BattleCharacterMotion.MotionSpecial_Mario` (its `_parent` field is declared on `MotionSpecial`)
    MotionSpecialMario
);
impl MotionSpecialMario {
    const PARENT: u64 = 0x10;

    pub fn parent(self) -> Option<BattleCharacter> {
        self.optional_field(Self::PARENT)
    }

    /// `can_bunnage_globe`: Mario's character is in the party, not bunnaged, has a normal status and is not
    /// invincible. The game tests dead before the status; the status test after it makes that one redundant.
    pub fn can_bunnage_globe(self) -> GlobeCheck {
        let Some(character) = self.parent() else {
            return GlobeCheck::NoCharacter;
        };
        let param = character.param();
        if !param.is_join() {
            GlobeCheck::NotJoined
        } else if param.is_bunnage() {
            GlobeCheck::Bunnage
        } else if param.is_dead() {
            GlobeCheck::Dead
        } else if param.status() != BadStatusKind::NORMAL {
            GlobeCheck::NotNormal
        } else if param.is_invincible() {
            GlobeCheck::Invincible
        } else {
            GlobeCheck::Yes
        }
    }
}

/// The step of `can_bunnage_globe` that decided the answer; only `Yes` means true.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum GlobeCheck {
    NoCharacter,
    NotJoined,
    Bunnage,
    Dead,
    NotNormal,
    Invincible,
    Yes,
}

// ---- Shim: guest registers to arguments (x0 is `this`, x1 the hidden method pointer the game passes) ----

/// Leaf shim: wrap x0 as a class and return the bool as the new x0.
macro_rules! leaf_shim {
    ($symbol:ident, $class:ty, $method:ident) => {
        #[unsafe(no_mangle)]
        pub unsafe extern "C" fn $symbol(this: u64, _: u64, _: u64) -> u64 {
            u64::from(unsafe { <$class>::at(this) }.$method())
        }
    };
}

leaf_shim!(smrpg_badstatus_can_command, BadStatusManager, can_command);
leaf_shim!(smrpg_param_is_dead, BattleCharacterParam, is_dead);
leaf_shim!(smrpg_param_is_gone, BattleCharacterParam, is_gone);
leaf_shim!(
    smrpg_param_is_invincible,
    BattleCharacterParam,
    is_invincible
);
leaf_shim!(smrpg_param_can_battle, BattleCharacterParam, can_battle);
leaf_shim!(
    smrpg_param_can_targetting,
    BattleCharacterParam,
    can_targetting
);
leaf_shim!(smrpg_param_can_act, BattleCharacterParam, can_act);

/// Registers the lifted code leaves after `bl callee(x0, x1 = 0)`: it flushes x1, x8 and x30 (the return
/// address) before the call and never restores them. x8 is the zero the caller had just tested.
#[inline(always)]
unsafe fn after_call(x: *mut u64, return_address: u64) {
    unsafe {
        x.add(1).write(0);
        x.add(8).write(0);
        x.add(30).write(return_address);
    }
}

/// 0x71021c35a0 `BattleCharacterParam.can_use_any_command`; calls `BadStatusManager.can_command` at 0x7101e2fa80.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_param_can_use_any_command(x: *mut u64) {
    unsafe {
        let check = BattleCharacterParam::at(x.read()).command_check();
        if !matches!(check, CommandCheck::NotJoined | CommandCheck::Bunnage) {
            after_call(x, 0x71021c35cc); // reached the call
        }
        x.write(u64::from(check == CommandCheck::Yes));
    }
}

/// 0x7101e73c40 `MotionSpecial_Mario.can_bunnage_globe`; calls `is_dead` (0x71021bdec0) and
/// `is_invincible` (0x71021bcb30).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_mario_can_bunnage_globe(x: *mut u64) {
    unsafe {
        let check = MotionSpecialMario::at(x.read()).can_bunnage_globe();
        match check {
            GlobeCheck::NoCharacter | GlobeCheck::NotJoined | GlobeCheck::Bunnage => {}
            GlobeCheck::Dead | GlobeCheck::NotNormal => after_call(x, 0x7101e73c74),
            GlobeCheck::Invincible | GlobeCheck::Yes => after_call(x, 0x7101e73ca4),
        }
        x.write(u64::from(check == GlobeCheck::Yes));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const STATUSES: [u32; 17] = [
        0,
        1,
        2,
        3,
        4,
        5,
        6,
        7,
        8,
        9,
        10,
        11,
        12,
        0x7fff_ffff,
        0x8000_0000,
        0xffff_fff7,
        0xffff_ffff,
    ];
    const COUNTS: [u32; 6] = [0, 1, 2, 0x7fff_ffff, 0x8000_0000, 0xffff_ffff];

    /// The compiled `can_command` as the disassembly reads: bit mask 0x248 for statuses up to 9, then not 8.
    fn reference_can_command(status: u32) -> bool {
        let masked = status <= 9 && (1u32 << status) & 0x248 != 0;
        !masked && status != 8
    }

    /// One fake object graph: param at word 0, status manager at word 24, buff manager at word 32,
    /// character at word 40, motion special at word 48.
    struct Graph([u64; 64]);

    impl Graph {
        fn new(join: u8, bunnage: u8, targetting: u8, status: u32, act: u32, turn: u32) -> Self {
            let mut words = [0u64; 64];
            let base = words.as_ptr() as u64;
            words[13] |= u64::from(join) << 48;
            words[14] |= u64::from(bunnage) << 8;
            words[15] |= u64::from(targetting);
            words[11] |= u64::from(act) << 32;
            words[19] = base + 8 * 24;
            words[20] = base + 8 * 32;
            words[26] = u64::from(status);
            words[37] = u64::from(turn);
            words[44] = base;
            words[50] = base + 8 * 40;
            Self(words)
        }

        fn address(&self, word: usize) -> u64 {
            self.0.as_ptr() as u64 + 8 * word as u64
        }
    }

    #[test]
    fn param_predicates_match_the_disassembly() {
        for join in [0u8, 1, 2, 0xff] {
            for bunnage in [0u8, 1, 0x80] {
                for targetting in [0u8, 1, 0xff] {
                    for status in STATUSES {
                        for act in COUNTS {
                            let g = Graph::new(join, bunnage, targetting, status, act, 0);
                            let param = unsafe { BattleCharacterParam::at(g.address(0)) };
                            let usable = join != 0 && bunnage == 0 && status != 9 && status != 8;
                            assert_eq!(param.can_battle(), usable);
                            assert_eq!(param.can_targetting(), targetting != 0 && usable);
                            assert_eq!(param.can_act(), join != 0 && (act as i32) > 0);
                            assert_eq!(param.is_dead(), status == 9);
                            assert_eq!(param.is_gone(), status == 8);
                            assert_eq!(
                                param.can_use_any_command(),
                                join != 0
                                    && bunnage == 0
                                    && reference_can_command(status)
                                    && (act as i32) > 0
                            );
                        }
                    }
                }
            }
        }
    }

    #[test]
    fn can_command_matches_the_bit_mask_form() {
        for status in STATUSES {
            let g = Graph::new(1, 0, 0, status, 0, 0);
            let manager = unsafe { BadStatusManager::at(g.address(24)) };
            assert_eq!(manager.can_command(), reference_can_command(status));
        }
    }

    #[test]
    fn globe_check_follows_the_game_order() {
        for join in [0u8, 1] {
            for bunnage in [0u8, 1] {
                for status in STATUSES {
                    for turn in COUNTS {
                        let g = Graph::new(join, bunnage, 0, status, 0, turn);
                        let motion = unsafe { MotionSpecialMario::at(g.address(48)) };
                        let want = if join == 0 {
                            GlobeCheck::NotJoined
                        } else if bunnage != 0 {
                            GlobeCheck::Bunnage
                        } else if status == 9 {
                            GlobeCheck::Dead
                        } else if status != 0 {
                            GlobeCheck::NotNormal
                        } else if (turn as i32) > 0 {
                            GlobeCheck::Invincible
                        } else {
                            GlobeCheck::Yes
                        };
                        assert_eq!(motion.can_bunnage_globe(), want);
                    }
                }
            }
        }
        let mut g = Graph::new(1, 0, 0, 0, 0, 0);
        g.0[50] = 0;
        let motion = unsafe { MotionSpecialMario::at(g.address(48)) };
        assert_eq!(motion.can_bunnage_globe(), GlobeCheck::NoCharacter);
    }

    #[test]
    fn shims_leave_the_register_residue_of_the_lifted_code() {
        let g = Graph::new(1, 0, 0, 0, 5, 0);
        let mut x = [0xaau64; 32];
        x[0] = g.address(48);
        unsafe { smrpg_mario_can_bunnage_globe(x.as_mut_ptr()) };
        assert_eq!((x[0], x[1], x[8], x[30]), (1, 0, 0, 0x7101e73ca4));
        x = [0xaa; 32];
        x[0] = g.address(0);
        unsafe { smrpg_param_can_use_any_command(x.as_mut_ptr()) };
        assert_eq!((x[0], x[1], x[8], x[30]), (1, 0, 0, 0x71021c35cc));
        let off = Graph::new(0, 0, 0, 0, 5, 0);
        x = [0xaa; 32];
        x[0] = off.address(0);
        unsafe { smrpg_param_can_use_any_command(x.as_mut_ptr()) };
        assert_eq!((x[0], x[1], x[8], x[30]), (0, 0xaa, 0xaa, 0xaa));
    }
}
