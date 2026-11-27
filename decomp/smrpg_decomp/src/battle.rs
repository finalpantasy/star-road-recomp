//! Battle classes (task-51): the first readable game logic. Names and offsets are IL2CPP metadata facts
//! (class `Battle.*`, field table of the 1.0.1 build), the same kind of fact `leaves.def` already carries.
//!
//! The readable part is the `impl` blocks: field accessors and the predicates the game computes from them.
//! The `smrpg_*` functions at the bottom are the shim: they map guest registers to those arguments and back,
//! and reproduce the register residue the lifted code leaves behind (see docs/DECOMP.md, "Readable ports").

use crate::bridge::{GuestException, Regs};
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
    const MOTION: u64 = 0x18;
    const PARAM: u64 = 0x20;

    pub fn motion(self) -> Option<BattleCharacterMotion> {
        self.optional_field(Self::MOTION)
    }

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

guest_class!(
    /// `Battle.BattleCharacterMotion`
    BattleCharacterMotion
);

guest_class!(
    /// `Battle.BattleSkillParams`
    BattleSkillParams
);

/// `Battle.BattleCharacterMotion.MotionSpecialTiming`, the values `exec_hit` passes to `calc_motion`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MotionSpecialTiming(pub u32);

impl MotionSpecialTiming {
    pub const HIT_BEFORE: Self = Self(4);
    pub const HIT: Self = Self(5);
}

guest_class!(
    /// `Battle.BattleHit`
    BattleHit
);
impl BattleHit {
    const TARGET: u64 = 0x20;
    const SKILL_PARAMS: u64 = 0x28;
    const IS_ONLY_SE_HIT: u64 = 0x81;

    /// The character being hit. The callees test it for null themselves, so it is passed on as it is.
    pub fn target(self) -> BattleCharacter {
        self.object_field(Self::TARGET)
    }

    pub fn skill_params(self) -> Option<BattleSkillParams> {
        self.optional_field(Self::SKILL_PARAMS)
    }

    pub fn is_only_se_hit(self) -> bool {
        self.bool_field(Self::IS_ONLY_SE_HIT)
    }

    /// The target's motion, when there is a target and it has one.
    fn target_motion(self) -> Option<BattleCharacterMotion> {
        let target = self.target();
        if target.is_null() {
            None
        } else {
            target.motion()
        }
    }

    /// `exec_hit`: run one hit of a skill. With no skill parameters there is nothing to do. A sound-effect-only
    /// hit plays its sound and stops; so does the third reserve hit of a combo (it only lands its damage).
    /// Otherwise the target's motion is told the hit is coming (`HIT_BEFORE`), the damage, weak or resist mark and
    /// hit count are shown, the combo display is set, the hit lands, and the motion is told it landed (`HIT`).
    /// The game re-reads the fields before each call, so this does too.
    pub fn exec_hit<C: HitCalls>(self, calls: &mut C) -> Result<(), GuestException> {
        if self.skill_params().is_none() {
            return calls.leave(HitExit::NoSkillParams);
        }
        if self.is_only_se_hit() {
            return calls.only_se_hit_tail(self, self.target());
        }
        if calls.is_renkei_3_reserve_hit(self)? {
            return calls.hit_tail(self, self.target());
        }
        if let Some(motion) = self.target_motion() {
            calls.calc_motion(motion, self, MotionSpecialTiming::HIT_BEFORE)?;
        }
        calls.disp_damage(self, self.target())?;
        calls.disp_weak_resist(self, self.target())?;
        calls.disp_hit_count(self, self.target())?;
        calls.set_disp_combo(self)?;
        calls.hit(self, self.target())?;
        if self.target().is_null() {
            return calls.leave(HitExit::TargetGone);
        }
        match self.target().motion() {
            Some(motion) => calls.calc_motion_tail(motion, self, MotionSpecialTiming::HIT),
            None => calls.leave(HitExit::NoMotion),
        }
    }
}

/// How `exec_hit` ends without a tail call; the shim needs it to leave x0 as the lifted code does.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HitExit {
    NoSkillParams,
    /// The target was null after the hit: x0 stays what `hit` returned.
    TargetGone,
    /// The target has no motion: x0 is that null motion.
    NoMotion,
}

/// The game functions `exec_hit` calls, by their C# names. The `_tail` forms are the ones the game ends the
/// method with (a jump, not a call). The real implementation calls the lifted functions through the guest-call
/// bridge; the unit tests use a recorder.
pub trait HitCalls {
    fn only_se_hit_tail(
        &mut self,
        hit: BattleHit,
        target: BattleCharacter,
    ) -> Result<(), GuestException>;
    fn is_renkei_3_reserve_hit(&mut self, hit: BattleHit) -> Result<bool, GuestException>;
    fn hit_tail(&mut self, hit: BattleHit, target: BattleCharacter) -> Result<(), GuestException>;
    fn calc_motion(
        &mut self,
        motion: BattleCharacterMotion,
        hit: BattleHit,
        timing: MotionSpecialTiming,
    ) -> Result<(), GuestException>;
    fn disp_damage(
        &mut self,
        hit: BattleHit,
        target: BattleCharacter,
    ) -> Result<(), GuestException>;
    fn disp_weak_resist(
        &mut self,
        hit: BattleHit,
        target: BattleCharacter,
    ) -> Result<(), GuestException>;
    fn disp_hit_count(
        &mut self,
        hit: BattleHit,
        target: BattleCharacter,
    ) -> Result<(), GuestException>;
    fn set_disp_combo(&mut self, hit: BattleHit) -> Result<(), GuestException>;
    fn hit(&mut self, hit: BattleHit, target: BattleCharacter) -> Result<(), GuestException>;
    fn calc_motion_tail(
        &mut self,
        motion: BattleCharacterMotion,
        hit: BattleHit,
        timing: MotionSpecialTiming,
    ) -> Result<(), GuestException>;
    fn leave(&mut self, how: HitExit) -> Result<(), GuestException>;
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

// ---- exec_hit shim: the lifted body's calls, with the registers it writes at each call site ----

mod hit_site {
    //! Callee addresses, and the address after each `bl` in `exec_hit` (the x30 the lifted code writes).
    pub const ONLY_SE_HIT: u64 = 0x7101cf77f0;
    pub const IS_RENKEI_3_RESERVE_HIT: u64 = 0x7101cf79a0;
    pub const HIT: u64 = 0x7101cf7aa0;
    pub const DISP_DAMAGE: u64 = 0x7101cf7f90;
    pub const DISP_WEAK_RESIST: u64 = 0x7101cf8310;
    pub const DISP_HIT_COUNT: u64 = 0x7101cf8570;
    pub const SET_DISP_COMBO: u64 = 0x7101cf87a0;
    pub const CALC_MOTION: u64 = 0x7101e67090;
    pub const RET_IS_RENKEI: u64 = 0x7101cf768c;
    pub const RET_CALC_MOTION: u64 = 0x7101cf76c4;
    pub const RET_DISP_DAMAGE: u64 = 0x7101cf76d0;
    pub const RET_DISP_WEAK_RESIST: u64 = 0x7101cf76dc;
    pub const RET_DISP_HIT_COUNT: u64 = 0x7101cf76e8;
    pub const RET_SET_DISP_COMBO: u64 = 0x7101cf76f0;
    pub const RET_HIT: u64 = 0x7101cf76fc;
}

/// `exec_hit` pushes a 32-byte frame (x29, x30, x19), so its callees see sp and x29 32 bytes lower, and x19 =
/// this. The lifted code writes a register to the context only at the first call after it changes (a "flush"),
/// so each method here writes just the registers the lifted code flushes at that site; every other register
/// keeps what the previous callee left. The entry values are what the epilogue restores before a tail call or
/// a return.
struct LiftedHit {
    regs: Regs,
    entry_x19: u64,
    entry_x29: u64,
    entry_x30: u64,
    entry_sp: u64,
}

impl LiftedHit {
    fn new(regs: Regs) -> Self {
        Self {
            entry_x19: regs.x(19),
            entry_x29: regs.x(29),
            entry_x30: regs.x(30),
            entry_sp: regs.sp(),
            regs,
        }
    }

    /// The epilogue before a tail call: the frame is popped and the saved registers are back.
    fn pop_frame(&mut self) {
        self.regs.set_x(19, self.entry_x19);
        self.regs.set_x(29, self.entry_x29);
        self.regs.set_x(30, self.entry_x30);
        self.regs.set_x(31, self.entry_sp);
    }

    /// The lifted `bl`: x30 is the address after it, then the callee runs on the register file.
    fn call(&mut self, target: u64, return_address: u64) -> Result<(), GuestException> {
        self.regs.set_x(30, return_address);
        self.regs.invoke(target).map(|_| ())
    }

    /// `callee(this, target)` for the methods that take the hit and a character.
    fn call_with_target(
        &mut self,
        target: u64,
        return_address: u64,
        hit: BattleHit,
        character: BattleCharacter,
    ) -> Result<(), GuestException> {
        self.regs.set_x(0, hit.address());
        self.regs.set_x(1, character.address());
        self.call(target, return_address)
    }

    /// `motion.calc_motion(skill_params, timing, hit)` with the zero method pointer in x4.
    fn stage_calc_motion(
        &mut self,
        motion: BattleCharacterMotion,
        hit: BattleHit,
        timing: MotionSpecialTiming,
    ) {
        let skill_params = hit.object_field::<BattleSkillParams>(BattleHit::SKILL_PARAMS);
        self.regs.set_x(0, motion.address());
        self.regs.set_x(1, skill_params.address());
        self.regs.set_x(2, u64::from(timing.0));
        self.regs.set_x(3, hit.address());
        self.regs.set_x(4, 0);
    }
}

impl HitCalls for LiftedHit {
    fn only_se_hit_tail(
        &mut self,
        hit: BattleHit,
        target: BattleCharacter,
    ) -> Result<(), GuestException> {
        self.regs.set_x(0, hit.address());
        self.regs.set_x(1, target.address());
        self.regs
            .set_x(8, u64::from(hit.u8_field(BattleHit::IS_ONLY_SE_HIT)));
        self.pop_frame();
        self.regs.invoke(hit_site::ONLY_SE_HIT).map(|_| ())
    }

    fn is_renkei_3_reserve_hit(&mut self, hit: BattleHit) -> Result<bool, GuestException> {
        // The first call: the frame is pushed, x19 = this, x8 = the flag byte that was just tested (zero).
        let new_sp = self.entry_sp.wrapping_sub(32);
        self.regs.set_x(0, hit.address());
        self.regs.set_x(8, 0);
        self.regs.set_x(19, hit.address());
        self.regs.set_x(29, new_sp);
        self.regs.set_x(31, new_sp);
        self.call(hit_site::IS_RENKEI_3_RESERVE_HIT, hit_site::RET_IS_RENKEI)?;
        Ok(self.regs.x(0) & 1 != 0)
    }

    fn hit_tail(&mut self, hit: BattleHit, target: BattleCharacter) -> Result<(), GuestException> {
        self.regs.set_x(0, hit.address());
        self.regs.set_x(1, target.address());
        self.pop_frame();
        self.regs.invoke(hit_site::HIT).map(|_| ())
    }

    fn calc_motion(
        &mut self,
        motion: BattleCharacterMotion,
        hit: BattleHit,
        timing: MotionSpecialTiming,
    ) -> Result<(), GuestException> {
        self.stage_calc_motion(motion, hit, timing);
        self.call(hit_site::CALC_MOTION, hit_site::RET_CALC_MOTION)
    }

    fn disp_damage(
        &mut self,
        hit: BattleHit,
        target: BattleCharacter,
    ) -> Result<(), GuestException> {
        self.call_with_target(
            hit_site::DISP_DAMAGE,
            hit_site::RET_DISP_DAMAGE,
            hit,
            target,
        )
    }

    fn disp_weak_resist(
        &mut self,
        hit: BattleHit,
        target: BattleCharacter,
    ) -> Result<(), GuestException> {
        self.call_with_target(
            hit_site::DISP_WEAK_RESIST,
            hit_site::RET_DISP_WEAK_RESIST,
            hit,
            target,
        )
    }

    fn disp_hit_count(
        &mut self,
        hit: BattleHit,
        target: BattleCharacter,
    ) -> Result<(), GuestException> {
        self.call_with_target(
            hit_site::DISP_HIT_COUNT,
            hit_site::RET_DISP_HIT_COUNT,
            hit,
            target,
        )
    }

    fn set_disp_combo(&mut self, hit: BattleHit) -> Result<(), GuestException> {
        self.regs.set_x(0, hit.address());
        self.call(hit_site::SET_DISP_COMBO, hit_site::RET_SET_DISP_COMBO)
    }

    fn hit(&mut self, hit: BattleHit, target: BattleCharacter) -> Result<(), GuestException> {
        self.call_with_target(hit_site::HIT, hit_site::RET_HIT, hit, target)
    }

    fn calc_motion_tail(
        &mut self,
        motion: BattleCharacterMotion,
        hit: BattleHit,
        timing: MotionSpecialTiming,
    ) -> Result<(), GuestException> {
        self.stage_calc_motion(motion, hit, timing);
        self.regs.set_x(8, hit.target().address());
        self.pop_frame();
        self.regs.invoke(hit_site::CALC_MOTION).map(|_| ())
    }

    fn leave(&mut self, how: HitExit) -> Result<(), GuestException> {
        // The lifted return flushes x0, x19, x29 and sp (not x30): the frame is popped, nothing else changes.
        if how == HitExit::NoMotion {
            self.regs.set_x(0, 0);
        }
        self.regs.set_x(19, self.entry_x19);
        self.regs.set_x(29, self.entry_x29);
        self.regs.set_x(31, self.entry_sp);
        Ok(())
    }
}

/// 0x7101cf7650 `BattleHit.exec_hit`. A pending exception from a callee stays in the register file, which is
/// all the lifted body leaves too.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_hit_exec_hit(x: *mut u64) {
    unsafe {
        let regs = Regs::from_raw(x);
        let hit = BattleHit::at(regs.x(0));
        let _ = hit.exec_hit(&mut LiftedHit::new(regs));
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

    extern crate std;
    use std::vec;
    use std::vec::Vec;

    /// A fake `BattleHit` graph: hit at word 0 (+0x20 target, +0x28 skill params, +0x81 flag byte), target at
    /// word 20 (+0x18 motion).
    struct HitGraph(std::boxed::Box<[u64; 64]>);

    impl HitGraph {
        fn new(skill: bool, only_se: u8, target: bool, motion: bool) -> Self {
            let mut graph = Self(std::boxed::Box::new([0u64; 64])); // on the heap: its address is stable
            let base = graph.0.as_ptr() as u64;
            let words = &mut graph.0;
            if skill {
                words[5] = base + 8 * 40;
            }
            if target {
                words[4] = base + 8 * 20;
                if motion {
                    words[23] = base + 8 * 44;
                }
            }
            words[16] = u64::from(only_se) << 8;
            graph
        }

        fn hit(&self) -> BattleHit {
            unsafe { BattleHit::at(self.0.as_ptr() as u64) }
        }
    }

    /// Records the calls `exec_hit` makes, by the game's method names.
    struct Recorder {
        calls: Vec<&'static str>,
        renkei: bool,
    }

    impl Recorder {
        fn new(renkei: bool) -> Self {
            Self {
                calls: Vec::new(),
                renkei,
            }
        }

        fn note(&mut self, name: &'static str) -> Result<(), GuestException> {
            self.calls.push(name);
            Ok(())
        }
    }

    impl HitCalls for Recorder {
        fn only_se_hit_tail(
            &mut self,
            _: BattleHit,
            _: BattleCharacter,
        ) -> Result<(), GuestException> {
            self.note("only_se_hit")
        }
        fn is_renkei_3_reserve_hit(&mut self, _: BattleHit) -> Result<bool, GuestException> {
            self.calls.push("is_renkei_3_reserve_hit");
            Ok(self.renkei)
        }
        fn hit_tail(&mut self, _: BattleHit, _: BattleCharacter) -> Result<(), GuestException> {
            self.note("hit (tail)")
        }
        fn calc_motion(
            &mut self,
            _: BattleCharacterMotion,
            _: BattleHit,
            timing: MotionSpecialTiming,
        ) -> Result<(), GuestException> {
            assert_eq!(timing, MotionSpecialTiming::HIT_BEFORE);
            self.note("calc_motion HIT_BEFORE")
        }
        fn disp_damage(&mut self, _: BattleHit, _: BattleCharacter) -> Result<(), GuestException> {
            self.note("disp_damage")
        }
        fn disp_weak_resist(
            &mut self,
            _: BattleHit,
            _: BattleCharacter,
        ) -> Result<(), GuestException> {
            self.note("disp_weak_resist")
        }
        fn disp_hit_count(
            &mut self,
            _: BattleHit,
            _: BattleCharacter,
        ) -> Result<(), GuestException> {
            self.note("disp_hit_count")
        }
        fn set_disp_combo(&mut self, _: BattleHit) -> Result<(), GuestException> {
            self.note("set_disp_combo")
        }
        fn hit(&mut self, _: BattleHit, _: BattleCharacter) -> Result<(), GuestException> {
            self.note("hit")
        }
        fn calc_motion_tail(
            &mut self,
            _: BattleCharacterMotion,
            _: BattleHit,
            timing: MotionSpecialTiming,
        ) -> Result<(), GuestException> {
            assert_eq!(timing, MotionSpecialTiming::HIT);
            self.note("calc_motion HIT (tail)")
        }
        fn leave(&mut self, how: HitExit) -> Result<(), GuestException> {
            self.note(match how {
                HitExit::NoSkillParams => "leave: no skill params",
                HitExit::TargetGone => "leave: target gone",
                HitExit::NoMotion => "leave: no motion",
            })
        }
    }

    #[test]
    fn exec_hit_calls_follow_the_game_order() {
        for skill in [false, true] {
            for only_se in [0u8, 1, 2, 0x80, 0xff] {
                for renkei in [false, true] {
                    // 0 no target, 1 target without a motion, 2 target with a motion
                    for target in 0..3 {
                        let g = HitGraph::new(skill, only_se, target > 0, target == 2);
                        let mut r = Recorder::new(renkei);
                        assert_eq!(g.hit().exec_hit(&mut r), Ok(()));
                        let mut want: Vec<&str> = Vec::new();
                        if !skill {
                            want.push("leave: no skill params");
                        } else if only_se != 0 {
                            want.push("only_se_hit");
                        } else {
                            want.push("is_renkei_3_reserve_hit");
                            if renkei {
                                want.push("hit (tail)");
                            } else {
                                if target == 2 {
                                    want.push("calc_motion HIT_BEFORE");
                                }
                                want.extend([
                                    "disp_damage",
                                    "disp_weak_resist",
                                    "disp_hit_count",
                                    "set_disp_combo",
                                    "hit",
                                ]);
                                want.push(match target {
                                    0 => "leave: target gone",
                                    1 => "leave: no motion",
                                    _ => "calc_motion HIT (tail)",
                                });
                            }
                        }
                        assert_eq!(r.calls, want, "{skill} {only_se:#x} {renkei} {target}");
                    }
                }
            }
        }
    }

    #[test]
    fn exec_hit_shim_stages_the_lifted_registers() {
        use crate::bridge::tests::{CALLS, clear, install};
        clear();
        for target in [
            hit_site::ONLY_SE_HIT,
            hit_site::IS_RENKEI_3_RESERVE_HIT,
            hit_site::HIT,
            hit_site::DISP_DAMAGE,
            hit_site::DISP_WEAK_RESIST,
            hit_site::DISP_HIT_COUNT,
            hit_site::SET_DISP_COMBO,
            hit_site::CALC_MOTION,
        ] {
            install(target, |r| r[8] = 0x1234); // every fake callee leaves x8 behind
        }
        let g = HitGraph::new(true, 0, true, true);
        let base = g.0.as_ptr() as u64;
        let mut file = [0u64; 96];
        file[0] = base;
        file[19] = 0x1919;
        file[29] = 0x2929;
        file[30] = 0x3030;
        file[31] = 0x8000;
        unsafe { smrpg_hit_exec_hit(file.as_mut_ptr()) };
        let calls: Vec<u64> = CALLS.with(|c| c.borrow().clone());
        assert_eq!(
            calls,
            vec![
                hit_site::IS_RENKEI_3_RESERVE_HIT,
                hit_site::CALC_MOTION,
                hit_site::DISP_DAMAGE,
                hit_site::DISP_WEAK_RESIST,
                hit_site::DISP_HIT_COUNT,
                hit_site::SET_DISP_COMBO,
                hit_site::HIT,
                hit_site::CALC_MOTION,
            ]
        );
        // The tail call to calc_motion(motion, skill params, HIT, hit, null) after the frame is popped.
        assert_eq!(
            (file[0], file[1], file[2], file[3], file[4]),
            (base + 8 * 44, base + 8 * 40, 5, base, 0)
        );
        // The frame is popped: the registers it saved are back.
        assert_eq!(
            (file[19], file[29], file[30], file[31]),
            (0x1919, 0x2929, 0x3030, 0x8000)
        );
    }
}
