//! Battle classes (task-51): the first readable game logic. Names and offsets are IL2CPP metadata facts
//! (class `Battle.*`, field table of the 1.0.1 build), the same kind of fact `leaves.def` already carries.
//!
//! The readable part is the `impl` blocks: field accessors and the predicates the game computes from them.
//! The `smrpg_*` functions at the bottom are the shim: they map guest registers to those arguments and back,
//! and reproduce the register residue the lifted code leaves behind (see docs/DECOMP.md, "Readable ports").

use crate::bridge::{GuestException, Regs};
use crate::fp::{Float32Convert, Float32Ops, Nzcv};
use crate::object::{GuestObject, Int32Array, guest_class};
use crate::unity::{Camera, DEG2RAD, Quaternion, Transform, Vector3};

pub mod attack;
pub mod belome;
pub mod damage;
pub mod element;
pub mod exec_exception;
pub mod exec_hit;
pub mod exec_steps;
pub(crate) mod frame;
pub mod renkei;
pub mod targets;

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

// ---- BattleCamera.adjust_camera_transform (task-53): float code, the first port that goes through `fp.rs` ----

guest_class!(
    /// `Battle.BattleCamera`
    BattleCamera
);
guest_class!(
    /// `Battle.BattleCamera.CameraShake` (a private nested class)
    CameraShake
);

impl CameraShake {
    const OFFSET: u64 = 0x34;

    /// `offset`: the shake displacement added to the camera position.
    pub fn offset(self) -> Vector3 {
        Vector3::new(
            self.f32_field(Self::OFFSET),
            self.f32_field(Self::OFFSET + 4),
            self.f32_field(Self::OFFSET + 8),
        )
    }
}

/// Which of the method's three `get_transform` calls this is. They are the same call in C#; the compiled code
/// has different registers live at each, and the shim must reproduce them.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TransformSite {
    /// First statement: `_camera.transform.eulerAngles = _euler_angle`.
    Entry,
    /// Following the look-at point (position not locked).
    Follow,
    /// Adding the shake offset; carries the shake object that was just null-tested.
    Shake(CameraShake),
}

/// The game functions `adjust_camera_transform` calls, by their Unity names, and the float arithmetic it does
/// (through `Float32Ops`, so each operation sets the FPSR flags the compiled code sets). The real implementation
/// calls the lifted functions through the bridge; the unit tests use a recorder.
pub trait CameraCalls: Float32Ops {
    /// `UnityEngine.Component.get_transform` (0x7101799d40).
    fn camera_transform(
        &mut self,
        camera: Camera,
        site: TransformSite,
    ) -> Result<Transform, GuestException>;
    /// `UnityEngine.Transform.set_eulerAngles` (0x71017a7da0).
    fn set_euler_angles(
        &mut self,
        transform: Transform,
        angles: Vector3,
    ) -> Result<(), GuestException>;
    /// `UnityEngine.Quaternion.Internal_FromEulerRad` (0x7102057610).
    fn quaternion_from_euler_rad(
        &mut self,
        euler_rad: Vector3,
    ) -> Result<Quaternion, GuestException>;
    /// `UnityEngine.Quaternion.op_Multiply(Quaternion, Vector3)` (0x7102057ae0).
    fn rotate(&mut self, rotation: Quaternion, vector: Vector3) -> Result<Vector3, GuestException>;
    /// `UnityEngine.Transform.set_position` (0x71017a7910).
    fn set_position(
        &mut self,
        transform: Transform,
        position: Vector3,
    ) -> Result<(), GuestException>;
    /// `UnityEngine.Transform.get_position` (0x71017a7860).
    fn get_position(&mut self, transform: Transform) -> Result<Vector3, GuestException>;
    /// The last statement: `set_position` as a jump. `shake` is the object whose offset was just added.
    fn set_position_tail(
        &mut self,
        transform: Transform,
        position: Vector3,
        shake: CameraShake,
    ) -> Result<(), GuestException>;
    /// The garbage-collection safepoint the compiled code polls after the follow step. `shake` is the field
    /// value it had just loaded.
    fn safepoint(&mut self, shake: Option<CameraShake>);
    /// Return without a tail call.
    fn leave(&mut self) -> Result<(), GuestException>;
}

impl BattleCamera {
    const CAMERA: u64 = 0x10;
    const LOOK_AT_POSITION: u64 = 0x3c;
    const EULER_ANGLE: u64 = 0x48;
    const DISTANCE: u64 = 0x54;
    const IS_POSITION_CHANGE_LOCKED: u64 = 0x74;
    const CAMERA_SHAKE: u64 = 0x90;

    /// The Unity camera, passed to `get_transform` without a null test (the callee throws on null).
    pub fn camera(self) -> Camera {
        self.object_field(Self::CAMERA)
    }

    fn vector_field(self, offset: u64) -> Vector3 {
        Vector3::new(
            self.f32_field(offset),
            self.f32_field(offset + 4),
            self.f32_field(offset + 8),
        )
    }

    pub fn look_at_position(self) -> Vector3 {
        self.vector_field(Self::LOOK_AT_POSITION)
    }

    pub fn euler_angle(self) -> Vector3 {
        self.vector_field(Self::EULER_ANGLE)
    }

    pub fn distance(self) -> f32 {
        self.f32_field(Self::DISTANCE)
    }

    pub fn is_position_change_locked(self) -> bool {
        self.bool_field(Self::IS_POSITION_CHANGE_LOCKED)
    }

    pub fn camera_shake(self) -> Option<CameraShake> {
        self.optional_field(Self::CAMERA_SHAKE)
    }

    /// `adjust_camera_transform`: put the camera transform where the camera state says. It always sets the
    /// Euler angles. Unless the position is locked it also places the camera behind the look-at point:
    /// `position = look_at - rotation * (0, 0, distance)`, with `rotation` from the Euler angles in radians.
    /// A camera shake, when there is one, then moves the position by its offset. The game re-reads the shake
    /// field for the offset, as `exec_hit` re-reads its fields.
    pub fn adjust_camera_transform<C: CameraCalls>(
        self,
        calls: &mut C,
    ) -> Result<(), GuestException> {
        let transform = calls.camera_transform(self.camera(), TransformSite::Entry)?;
        calls.set_euler_angles(transform, self.euler_angle())?;
        let shake = if self.is_position_change_locked() {
            self.camera_shake()
        } else {
            let transform = calls.camera_transform(self.camera(), TransformSite::Follow)?;
            let angles = self.euler_angle().scaled(DEG2RAD, calls);
            let rotation = calls.quaternion_from_euler_rad(angles)?;
            let behind = calls.rotate(rotation, Vector3::new(0.0, 0.0, self.distance()))?;
            let position = self.look_at_position().sub(behind, calls);
            calls.set_position(transform, position)?;
            let shake = self.camera_shake();
            calls.safepoint(shake);
            shake
        };
        let Some(shake) = shake else {
            return calls.leave();
        };
        let transform = calls.camera_transform(self.camera(), TransformSite::Shake(shake))?;
        let position = calls.get_position(transform)?;
        let shake: CameraShake = self.object_field(Self::CAMERA_SHAKE);
        let moved = position.add(shake.offset(), calls);
        calls.set_position_tail(transform, moved, shake)
    }
}

mod camera_site {
    //! Callee addresses and the address after each `bl` in `adjust_camera_transform`.
    pub const GET_TRANSFORM: u64 = 0x7101799d40;
    pub const SET_EULER_ANGLES: u64 = 0x71017a7da0;
    pub const GET_POSITION: u64 = 0x71017a7860;
    pub const SET_POSITION: u64 = 0x71017a7910;
    pub const FROM_EULER_RAD: u64 = 0x7102057610;
    pub const MULTIPLY: u64 = 0x7102057ae0;
    pub const RET_ENTRY_TRANSFORM: u64 = 0x7101e2cff4;
    pub const RET_SET_EULER_ANGLES: u64 = 0x7101e2d004;
    pub const RET_SHAKE_TRANSFORM: u64 = 0x7101e2d020;
    pub const RET_GET_POSITION: u64 = 0x7101e2d02c;
    pub const RET_FOLLOW_TRANSFORM: u64 = 0x7101e2d06c;
    pub const RET_FROM_EULER_RAD: u64 = 0x7101e2d09c;
    pub const RET_MULTIPLY: u64 = 0x7101e2d0b0;
    pub const RET_SET_POSITION: u64 = 0x7101e2d0c8;
    /// The poll after the follow step (the load of the shake field is the instruction before it).
    pub const SAFEPOINT: u64 = 0x7101e2d0cc;
    /// `Mathf.Deg2Rad` is built into x8 with mov and movk, then moved to s2.
    pub const DEG2RAD_BITS: u64 = 0x3c8e_fa35;
}

/// The frame is 64 bytes (d10, x29, x30, d9, d8, x20, x19) and x29 points at sp + 0x20. As in `LiftedHit`, the
/// lifted code writes a register to the context only at the first call after it changes, so each method writes
/// just the registers the lifted code flushes at that site (floats as `ldr s` leaves them: `{bits, 0}`), and the
/// entry values come back at the epilogue. The registers that are not flushed keep what the last callee left.
struct LiftedCamera {
    regs: Regs,
    camera: BattleCamera,
    /// x20 of the lifted body: the transform of the follow or shake step.
    transform: u64,
    entry_x19: u64,
    entry_x20: u64,
    entry_x29: u64,
    entry_x30: u64,
    entry_sp: u64,
    /// d8, d9 and d10 are saved with `stp d` and restored with `ldp d`: the low 64 bits, upper half zero.
    entry_d: [u64; 3],
}

impl LiftedCamera {
    fn new(regs: Regs) -> Self {
        Self {
            camera: unsafe { BattleCamera::at(regs.x(0)) },
            transform: 0,
            entry_x19: regs.x(19),
            entry_x20: regs.x(20),
            entry_x29: regs.x(29),
            entry_x30: regs.x(30),
            entry_sp: regs.sp(),
            entry_d: [regs.v_low(8), regs.v_low(9), regs.v_low(10)],
            regs,
        }
    }

    /// The epilogue: saved registers back, frame popped.
    fn pop_frame(&mut self) {
        self.regs.set_x(19, self.entry_x19);
        self.regs.set_x(20, self.entry_x20);
        self.regs.set_x(29, self.entry_x29);
        self.regs.set_x(31, self.entry_sp);
        for (n, low) in [
            (8, self.entry_d[0]),
            (9, self.entry_d[1]),
            (10, self.entry_d[2]),
        ] {
            self.regs.set_v(n, low, 0);
        }
    }

    fn set_vector(&mut self, first: usize, v: Vector3) {
        self.regs.set_f32(first, v.x);
        self.regs.set_f32(first + 1, v.y);
        self.regs.set_f32(first + 2, v.z);
    }

    fn vector_result(&self) -> Vector3 {
        Vector3::new(self.regs.v_f32(0), self.regs.v_f32(1), self.regs.v_f32(2))
    }

    /// The lifted `bl`: x30 is the address after it, then the callee runs on the register file.
    fn call(&mut self, target: u64, return_address: u64) -> Result<(), GuestException> {
        self.regs.set_x(30, return_address);
        self.regs.invoke(target).map(|_| ())
    }
}

impl Float32Ops for LiftedCamera {
    fn fadd(&mut self, a: f32, b: f32) -> f32 {
        self.regs.f32_arith(2, a, b)
    }
    fn fsub(&mut self, a: f32, b: f32) -> f32 {
        self.regs.f32_arith(3, a, b)
    }
    fn fmul(&mut self, a: f32, b: f32) -> f32 {
        self.regs.f32_arith(0, a, b)
    }
}

impl CameraCalls for LiftedCamera {
    fn camera_transform(
        &mut self,
        camera: Camera,
        site: TransformSite,
    ) -> Result<Transform, GuestException> {
        self.regs.set_x(0, camera.address());
        self.regs.set_x(1, 0);
        let return_address = match site {
            TransformSite::Entry => {
                // The first call: frame pushed, x19 = this.
                let new_sp = self.entry_sp.wrapping_sub(64);
                self.regs.set_x(19, self.camera.address());
                self.regs.set_x(29, new_sp.wrapping_add(0x20));
                self.regs.set_x(31, new_sp);
                camera_site::RET_ENTRY_TRANSFORM
            }
            // x8 is the byte just tested (zero).
            TransformSite::Follow => {
                self.regs.set_x(8, 0);
                camera_site::RET_FOLLOW_TRANSFORM
            }
            TransformSite::Shake(shake) => {
                self.regs.set_x(8, shake.address());
                camera_site::RET_SHAKE_TRANSFORM
            }
        };
        self.call(camera_site::GET_TRANSFORM, return_address)?;
        let transform = self.regs.x(0);
        self.transform = transform;
        Ok(unsafe { Transform::at(transform) })
    }

    fn set_euler_angles(
        &mut self,
        transform: Transform,
        angles: Vector3,
    ) -> Result<(), GuestException> {
        // x0 is still the transform `get_transform` returned.
        debug_assert_eq!(self.regs.x(0), transform.address());
        self.regs.set_x(1, 0);
        self.set_vector(0, angles);
        self.call(
            camera_site::SET_EULER_ANGLES,
            camera_site::RET_SET_EULER_ANGLES,
        )
    }

    fn quaternion_from_euler_rad(
        &mut self,
        euler_rad: Vector3,
    ) -> Result<Quaternion, GuestException> {
        // x8 = the Deg2Rad bits, x20 = transform, x0 = 0 (the method pointer); v0 to v2 the scaled angles, v3 the
        // unscaled z (loaded for the multiply), v8 to v10 the look-at point (kept across the calls).
        self.regs.set_x(0, 0);
        self.regs.set_x(8, camera_site::DEG2RAD_BITS);
        self.regs.set_x(20, self.transform);
        self.set_vector(0, euler_rad);
        self.regs.set_f32(3, self.camera.euler_angle().z);
        let look_at = self.camera.look_at_position();
        self.set_vector(8, look_at);
        self.call(camera_site::FROM_EULER_RAD, camera_site::RET_FROM_EULER_RAD)?;
        Ok(Quaternion {
            x: self.regs.v_f32(0),
            y: self.regs.v_f32(1),
            z: self.regs.v_f32(2),
            w: self.regs.v_f32(3),
        })
    }

    fn rotate(
        &mut self,
        _rotation: Quaternion,
        vector: Vector3,
    ) -> Result<Vector3, GuestException> {
        // The quaternion is still in v0 to v3 where `Internal_FromEulerRad` left it; the vector goes to v4 to v6.
        self.regs.set_x(0, 0);
        self.set_vector(4, vector);
        self.call(camera_site::MULTIPLY, camera_site::RET_MULTIPLY)?;
        Ok(self.vector_result())
    }

    fn set_position(
        &mut self,
        _transform: Transform,
        position: Vector3,
    ) -> Result<(), GuestException> {
        self.regs.set_x(0, self.transform);
        self.regs.set_x(1, 0);
        self.set_vector(0, position);
        self.call(camera_site::SET_POSITION, camera_site::RET_SET_POSITION)
    }

    fn get_position(&mut self, transform: Transform) -> Result<Vector3, GuestException> {
        self.regs.set_x(1, 0);
        self.regs.set_x(20, transform.address());
        self.call(camera_site::GET_POSITION, camera_site::RET_GET_POSITION)?;
        Ok(self.vector_result())
    }

    fn set_position_tail(
        &mut self,
        transform: Transform,
        position: Vector3,
        shake: CameraShake,
    ) -> Result<(), GuestException> {
        self.regs.set_x(0, transform.address());
        self.regs.set_x(1, 0);
        self.regs.set_x(8, shake.address());
        self.regs.set_x(30, self.entry_x30);
        self.pop_frame();
        self.set_vector(0, position);
        self.set_vector(3, shake.offset());
        self.regs.invoke(camera_site::SET_POSITION).map(|_| ())
    }

    fn safepoint(&mut self, shake: Option<CameraShake>) {
        let x8 = shake.map_or(0, GuestObject::address);
        self.regs.safepoint(camera_site::SAFEPOINT, 8, x8);
    }

    fn leave(&mut self) -> Result<(), GuestException> {
        // The lifted return flushes x19, x20, x29, d8 to d10 and sp (not x0, x8 or x30).
        self.pop_frame();
        Ok(())
    }
}

/// 0x7101e2cfd0 `BattleCamera.adjust_camera_transform`. A pending exception from a callee stays in the register
/// file, which is all the lifted body leaves too.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_camera_adjust_camera_transform(x: *mut u64) {
    unsafe {
        let regs = Regs::from_raw(x);
        let camera = BattleCamera::at(regs.x(0));
        let _ = camera.adjust_camera_transform(&mut LiftedCamera::new(regs));
    }
}

// ---- BattleCharacterParam.get_calc_defence and get_calc_magic_defence (task-54) ----
//
// The defence stat the damage code reads: the base stat plus the buff manager's added status, and for a player
// character scaled by the buff manager's ratio. Live in a battle (see backlog/tasks/task-54.md for the counts).

impl BuffManager {
    const ADD_STATUS: u64 = 0x18;

    /// `_add_status`: the added status values, one per stat (1 is defence, 3 is magic defence).
    pub fn add_status(self) -> Int32Array {
        self.object_field(Self::ADD_STATUS)
    }
}

impl BattleCharacterParam {
    const DEFENCE: u64 = 0x40;
    const MAGIC_DEFENCE: u64 = 0x48;
    const IS_PLAYER: u64 = 0x6c;

    /// `_is_player`, the raw byte (the compiled code passes it on as it is).
    pub fn is_player_byte(self) -> u8 {
        self.u8_field(Self::IS_PLAYER)
    }
}

/// Which of the two methods: they have the same shape and differ in the stat, the `_add_status` index, the
/// buff manager ratio method and the code addresses the shim needs.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DefenceStat {
    /// `get_calc_defence` (0x71021bc270)
    Physical,
    /// `get_calc_magic_defence` (0x71021bc440)
    Magic,
}

impl DefenceStat {
    fn base_offset(self) -> u64 {
        match self {
            Self::Physical => BattleCharacterParam::DEFENCE,
            Self::Magic => BattleCharacterParam::MAGIC_DEFENCE,
        }
    }

    /// Index into `_add_status`.
    fn add_status_index(self) -> u64 {
        match self {
            Self::Physical => 1,
            Self::Magic => 3,
        }
    }
}

/// The values the method has loaded when it calls the buff manager (the compiled code keeps them in registers).
#[derive(Clone, Copy, Debug)]
pub struct ScaleSite {
    pub buff: BuffManager,
    pub is_player_byte: u8,
    pub base: i32,
    pub added: i32,
    pub sum: i32,
}

/// The game functions the two methods call, and the float arithmetic of `(int)(sum * ratio)`.
pub trait StatCalls: Float32Convert {
    /// `BuffManager.defense_up_ratio` (0x71021c4240) or `magic_defense_up_ratio` (0x71021c4560).
    fn defense_up_ratio(
        &mut self,
        stat: DefenceStat,
        site: ScaleSite,
    ) -> Result<f32, GuestException>;
    /// The `IndexOutOfRangeException` throw (0x71000c4510): `_add_status` is too short. Always an exception.
    fn throw_index_out_of_range(
        &mut self,
        stat: DefenceStat,
        param: BattleCharacterParam,
        buff: BuffManager,
        array: Int32Array,
        length: u32,
    ) -> GuestException;
    /// Return `result`.
    fn leave(&mut self, result: i32);
}

/// C#'s `(int)(value * ratio)` as the compiled code does it: the float product, converted toward zero with
/// saturation, except that a product of exactly +infinity gives `int.MinValue` (the x86 result of the cast).
fn scale_by_ratio<F: Float32Convert>(value: i32, ratio: f32, f: &mut F) -> i32 {
    let value_as_float = f.float_from_i32(value);
    let product = f.fmul(ratio, value_as_float);
    let truncated = f.trunc_to_i32(product);
    if f.compare(product, f32::INFINITY).z {
        i32::MIN
    } else {
        truncated
    }
}

impl BattleCharacterParam {
    /// `get_calc_defence`: `_defence + _buff_mng._add_status[1]`, times the buff manager's defense-up ratio for
    /// a player character.
    pub fn calc_defence<C: StatCalls>(self, calls: &mut C) -> Result<i32, GuestException> {
        self.calc_defence_stat(DefenceStat::Physical, calls)
    }

    /// `get_calc_magic_defence`: the same with `_magic_defence`, `_add_status[3]` and the magic ratio.
    pub fn calc_magic_defence<C: StatCalls>(self, calls: &mut C) -> Result<i32, GuestException> {
        self.calc_defence_stat(DefenceStat::Magic, calls)
    }

    fn calc_defence_stat<C: StatCalls>(
        self,
        stat: DefenceStat,
        calls: &mut C,
    ) -> Result<i32, GuestException> {
        let buff = self.buff_mng();
        let added_status = buff.add_status();
        let length = added_status.len();
        if u64::from(length) <= stat.add_status_index() {
            return Err(calls.throw_index_out_of_range(stat, self, buff, added_status, length));
        }
        let base = self.i32_field(stat.base_offset());
        let added = added_status.element(stat.add_status_index());
        let sum = base.wrapping_add(added);
        let is_player_byte = self.is_player_byte();
        if is_player_byte == 0 {
            calls.leave(sum);
            return Ok(sum);
        }
        let site = ScaleSite {
            buff,
            is_player_byte,
            base,
            added,
            sum,
        };
        let ratio = calls.defense_up_ratio(stat, site)?;
        let scaled = scale_by_ratio(sum, ratio, calls);
        calls.leave(scaled);
        Ok(scaled)
    }
}

mod stat_site {
    //! Callee addresses and the address after each `bl` in the two methods.
    pub const THROW_INDEX: u64 = 0x71000c4510;
    pub const PHYSICAL_RATIO: u64 = 0x71021c4240;
    pub const MAGIC_RATIO: u64 = 0x71021c4560;
    pub const PHYSICAL_RET_RATIO: u64 = 0x71021bc2ac;
    pub const MAGIC_RET_RATIO: u64 = 0x71021bc47c;
    pub const PHYSICAL_RET_THROW: u64 = 0x71021bc2e0;
    pub const MAGIC_RET_THROW: u64 = 0x71021bc4b0;
}

/// Both methods push a 32-byte frame (x29, x30, x19) and keep x29 = sp. Like the other shims it writes a register
/// to the context only where the lifted code flushes it: before the ratio call x0, x8, x9, x10, x19, x29, x30 and
/// sp, before the throw x0, x8, x9, x10, x29, x30 and sp, at the return x0, x19, x29, sp and v0, v1 (floats as the
/// instructions leave them: `{bits, 0}`). x8 to x10 and x30 keep what the callee left.
struct LiftedStat {
    regs: Regs,
    entry_x19: u64,
    entry_x29: u64,
    entry_sp: u64,
}

impl LiftedStat {
    fn new(regs: Regs) -> Self {
        Self {
            entry_x19: regs.x(19),
            entry_x29: regs.x(29),
            entry_sp: regs.sp(),
            regs,
        }
    }

    /// The prologue's `stp x29, x30, [sp, #-32]!` and `add x29, sp, #0`, flushed at the first call.
    fn push_frame(&mut self) {
        let sp = self.entry_sp.wrapping_sub(32);
        self.regs.set_x(29, sp);
        self.regs.set_x(31, sp);
    }
}

impl Float32Ops for LiftedStat {
    fn fadd(&mut self, a: f32, b: f32) -> f32 {
        let r = self.regs.f32_arith(2, a, b);
        self.regs.set_f32(0, r);
        r
    }
    fn fsub(&mut self, a: f32, b: f32) -> f32 {
        let r = self.regs.f32_arith(3, a, b);
        self.regs.set_f32(0, r);
        r
    }
    /// `fmul s0, s0, s1`.
    fn fmul(&mut self, a: f32, b: f32) -> f32 {
        let r = self.regs.f32_arith(0, a, b);
        self.regs.set_f32(0, r);
        r
    }
}

impl Float32Convert for LiftedStat {
    /// `scvtf s1, w19`.
    fn float_from_i32(&mut self, value: i32) -> f32 {
        let r = self.regs.f32_from_i32(value);
        self.regs.set_f32(1, r);
        r
    }
    /// `fcvtzs w8, s0` (x8 is not flushed).
    fn trunc_to_i32(&mut self, value: f32) -> i32 {
        self.regs.i32_from_f32_trunc(value)
    }
    /// `fmov s1, w9` (+infinity), then `fcmp s0, s1`.
    fn compare(&mut self, a: f32, b: f32) -> Nzcv {
        self.regs.set_f32(1, b);
        self.regs.f32_compare(a, b)
    }
}

impl StatCalls for LiftedStat {
    fn defense_up_ratio(
        &mut self,
        stat: DefenceStat,
        site: ScaleSite,
    ) -> Result<f32, GuestException> {
        let (target, return_address) = match stat {
            DefenceStat::Physical => (stat_site::PHYSICAL_RATIO, stat_site::PHYSICAL_RET_RATIO),
            DefenceStat::Magic => (stat_site::MAGIC_RATIO, stat_site::MAGIC_RET_RATIO),
        };
        self.push_frame();
        self.regs.set_x(0, site.buff.address());
        self.regs.set_x(8, u64::from(site.is_player_byte));
        self.regs.set_x(9, u64::from(site.added as u32));
        self.regs.set_x(10, u64::from(site.base as u32));
        self.regs.set_x(19, u64::from(site.sum as u32));
        self.regs.set_x(30, return_address);
        self.regs.invoke(target)?;
        Ok(self.regs.f32_result())
    }

    fn throw_index_out_of_range(
        &mut self,
        stat: DefenceStat,
        param: BattleCharacterParam,
        buff: BuffManager,
        array: Int32Array,
        length: u32,
    ) -> GuestException {
        self.push_frame();
        self.regs.set_x(0, buff.address());
        self.regs.set_x(8, param.address());
        self.regs.set_x(9, array.address());
        self.regs.set_x(10, u64::from(length));
        let return_address = match stat {
            DefenceStat::Physical => stat_site::PHYSICAL_RET_THROW,
            DefenceStat::Magic => stat_site::MAGIC_RET_THROW,
        };
        self.regs.set_x(30, return_address);
        // If the thrower returns without an exception the compiled function simply ends: no epilogue.
        let _ = self.regs.invoke(stat_site::THROW_INDEX);
        GuestException
    }

    fn leave(&mut self, result: i32) {
        // `mov w0, w19`, then x19, x29 and sp come back from the frame; v0 and v1 are flushed as they are.
        self.regs.set_x(0, u64::from(result as u32));
        self.regs.set_x(19, self.entry_x19);
        self.regs.set_x(29, self.entry_x29);
        self.regs.set_x(31, self.entry_sp);
    }
}

/// 0x71021bc270 `BattleCharacterParam.get_calc_defence`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_param_calc_defence(x: *mut u64) {
    unsafe {
        let regs = Regs::from_raw(x);
        let param = BattleCharacterParam::at(regs.x(0));
        let _ = param.calc_defence(&mut LiftedStat::new(regs));
    }
}

/// 0x71021bc440 `BattleCharacterParam.get_calc_magic_defence`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_param_calc_magic_defence(x: *mut u64) {
    unsafe {
        let regs = Regs::from_raw(x);
        let param = BattleCharacterParam::at(regs.x(0));
        let _ = param.calc_magic_defence(&mut LiftedStat::new(regs));
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
    struct Graph(std::boxed::Box<[u64; 64]>);

    impl Graph {
        fn new(join: u8, bunnage: u8, targetting: u8, status: u32, act: u32, turn: u32) -> Self {
            // Boxed: the pointers inside must not move when `Self` is returned.
            let mut words = std::boxed::Box::new([0u64; 64]);
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

#[cfg(test)]
mod camera_tests {
    extern crate std;
    use super::*;
    use crate::bridge::tests::{CALLS, SAFEPOINTS, clear, fake_guest_bl, fake_safepoint, install};
    use crate::fp::FpEnv;
    use crate::{guest_ld8, guest_ld32, guest_ld64};
    use std::vec;
    use std::vec::Vec;

    /// Fake object graph: the camera at word 0 (0xa0 bytes), a shake object at word 24.
    struct Graph(std::boxed::Box<[u64; 40]>);

    const SHAKE_WORD: usize = 24;

    impl Graph {
        fn new(
            locked: u8,
            shake: bool,
            look: [u32; 3],
            euler: [u32; 3],
            distance: u32,
            offset: [u32; 3],
        ) -> Self {
            let mut g = Self(std::boxed::Box::new([0; 40])); // boxed: the pointers inside must not move
            let base = g.0.as_ptr() as u64;
            g.0[2] = 0xca4e_0000_0000_c0de; // _camera, never dereferenced
            for i in 0..3 {
                g.set32(0x3c + 4 * i, look[i]);
                g.set32(0x48 + 4 * i, euler[i]);
                g.set32(8 * SHAKE_WORD + 0x34 + 4 * i, offset[i]);
            }
            g.set32(0x54, distance);
            g.0[0x74 / 8] = u64::from(locked) << 32;
            g.0[0x90 / 8] = if shake {
                base + 8 * SHAKE_WORD as u64
            } else {
                0
            };
            g
        }

        fn set32(&mut self, byte: usize, value: u32) {
            let bytes = self.0.as_mut_ptr() as *mut u8;
            unsafe { bytes.add(byte).cast::<u32>().write_unaligned(value) };
        }

        fn address(&self) -> u64 {
            self.0.as_ptr() as u64
        }
    }

    // ---- the readable body, against a recorder ----

    #[derive(Debug, PartialEq)]
    enum Event {
        Transform(&'static str),
        SetEuler([u32; 3]),
        FromEulerRad([u32; 3]),
        Rotate([u32; 3]),
        SetPosition([u32; 3]),
        GetPosition,
        SetPositionTail([u32; 3]),
        Safepoint(bool),
        Leave,
    }

    struct Recorder {
        env: FpEnv,
        events: Vec<Event>,
    }

    impl Float32Ops for Recorder {
        fn fadd(&mut self, a: f32, b: f32) -> f32 {
            self.env.fadd32(a, b)
        }
        fn fsub(&mut self, a: f32, b: f32) -> f32 {
            self.env.fsub32(a, b)
        }
        fn fmul(&mut self, a: f32, b: f32) -> f32 {
            self.env.fmul32(a, b)
        }
    }

    impl CameraCalls for Recorder {
        fn camera_transform(
            &mut self,
            _camera: Camera,
            site: TransformSite,
        ) -> Result<Transform, GuestException> {
            self.events.push(Event::Transform(match site {
                TransformSite::Entry => "entry",
                TransformSite::Follow => "follow",
                TransformSite::Shake(_) => "shake",
            }));
            Ok(unsafe { Transform::at(0x7777) })
        }
        fn set_euler_angles(&mut self, _t: Transform, a: Vector3) -> Result<(), GuestException> {
            self.events.push(Event::SetEuler(a.bits()));
            Ok(())
        }
        fn quaternion_from_euler_rad(&mut self, e: Vector3) -> Result<Quaternion, GuestException> {
            self.events.push(Event::FromEulerRad(e.bits()));
            Ok(Quaternion {
                x: e.x,
                y: e.y,
                z: e.z,
                w: 1.0,
            })
        }
        fn rotate(&mut self, q: Quaternion, v: Vector3) -> Result<Vector3, GuestException> {
            self.events.push(Event::Rotate(v.bits()));
            Ok(Vector3::new(v.z * 0.5, q.x, q.y + v.z))
        }
        fn set_position(&mut self, _t: Transform, p: Vector3) -> Result<(), GuestException> {
            self.events.push(Event::SetPosition(p.bits()));
            Ok(())
        }
        fn get_position(&mut self, _t: Transform) -> Result<Vector3, GuestException> {
            self.events.push(Event::GetPosition);
            Ok(Vector3::new(10.0, 20.0, 30.0))
        }
        fn set_position_tail(
            &mut self,
            _t: Transform,
            p: Vector3,
            _s: CameraShake,
        ) -> Result<(), GuestException> {
            self.events.push(Event::SetPositionTail(p.bits()));
            Ok(())
        }
        fn safepoint(&mut self, shake: Option<CameraShake>) {
            self.events.push(Event::Safepoint(shake.is_some()));
        }
        fn leave(&mut self) -> Result<(), GuestException> {
            self.events.push(Event::Leave);
            Ok(())
        }
    }

    fn bits(a: [f32; 3]) -> [u32; 3] {
        [a[0].to_bits(), a[1].to_bits(), a[2].to_bits()]
    }

    fn run_recorder(g: &Graph) -> Vec<Event> {
        let mut r = Recorder {
            env: FpEnv::new(0, 0),
            events: Vec::new(),
        };
        let camera = unsafe { BattleCamera::at(g.address()) };
        camera.adjust_camera_transform(&mut r).unwrap();
        r.events
    }

    #[test]
    fn call_order_and_values_for_every_branch() {
        let look = bits([1.5, -2.25, 100.0]);
        let euler = bits([30.0, -45.0, 10.0]);
        let offset = bits([0.5, 0.25, -0.125]);
        let distance = 12.5f32.to_bits();
        let rad = [30.0 * DEG2RAD, -45.0 * DEG2RAD, 10.0 * DEG2RAD];
        // The recorder's rotate(q, v) is (v.z * 0.5, q.x, q.y + v.z) with q the angles in radians, v = (0, 0, d).
        let behind = [12.5f32 * 0.5, rad[0], rad[1] + 12.5];
        let position = bits([
            1.5f32 - behind[0],
            -2.25f32 - behind[1],
            100.0f32 - behind[2],
        ]);
        let shaken = bits([10.5, 20.25, 29.875]);
        // unlocked, no shake
        let g = Graph::new(0, false, look, euler, distance, offset);
        assert_eq!(
            run_recorder(&g),
            vec![
                Event::Transform("entry"),
                Event::SetEuler(euler),
                Event::Transform("follow"),
                Event::FromEulerRad(bits(rad)),
                Event::Rotate(bits([0.0, 0.0, 12.5])),
                Event::SetPosition(position),
                Event::Safepoint(false),
                Event::Leave,
            ]
        );
        // unlocked, shake
        let g = Graph::new(0, true, look, euler, distance, offset);
        let events = run_recorder(&g);
        assert_eq!(events[6], Event::Safepoint(true));
        assert_eq!(
            events[7..],
            [
                Event::Transform("shake"),
                Event::GetPosition,
                Event::SetPositionTail(shaken)
            ]
        );
        // locked (any non-zero byte), no shake: only the angles
        for flag in [1u8, 2, 0x80, 0xff] {
            let g = Graph::new(flag, false, look, euler, distance, offset);
            assert_eq!(
                run_recorder(&g),
                vec![
                    Event::Transform("entry"),
                    Event::SetEuler(euler),
                    Event::Leave
                ]
            );
        }
        // locked, shake: angles, then the shake step, no follow step and no safepoint
        let g = Graph::new(1, true, look, euler, distance, offset);
        assert_eq!(
            run_recorder(&g),
            vec![
                Event::Transform("entry"),
                Event::SetEuler(euler),
                Event::Transform("shake"),
                Event::GetPosition,
                Event::SetPositionTail(shaken),
            ]
        );
    }

    // ---- the shim, against a transcription of the lifted C (main_0343.c, f_7101e2cfd0) ----

    const WORDS: usize = 100;
    const FPCR_WORD: usize = 97; // fpcr is the high half of word 97, fpsr the low half of word 98

    fn fop(file: &mut [u64; WORDS], op: u8, a: u64, b: u64) -> [u64; 2] {
        let mut env = FpEnv::new((file[FPCR_WORD] >> 32) as u32, file[98] as u32);
        let (a, b) = (f32::from_bits(a as u32), f32::from_bits(b as u32));
        let r = match op {
            0 => env.fmul32(a, b),
            2 => env.fadd32(a, b),
            _ => env.fsub32(a, b),
        };
        file[98] = (file[98] & !0xffff_ffff) | u64::from(env.fpsr);
        [u64::from(r.to_bits()), 0]
    }

    fn call(file: &mut [u64; WORDS], target: u64) -> bool {
        unsafe { fake_guest_bl(file.as_mut_ptr(), target) != 0 }
    }

    /// The lifted body, line by line: locals for the registers it keeps in C variables, a flush list at every
    /// call site as the C writes it, and the reload of x0, x1 and v0 to v3 after each call.
    #[allow(unused_assignments)]
    fn lifted_reference(file: &mut [u64; WORDS]) {
        let (mut x0, mut x1, mut x8);
        let mut x19;
        let mut x20;
        let mut x29;
        let mut x30;
        let mut sp;
        x0 = file[0];
        x1 = file[1];
        x19 = file[19];
        x20 = file[20];
        x29 = file[29];
        sp = file[31];
        let mut v: [[u64; 2]; 11] = [[0; 2]; 11];
        for (n, slot) in v.iter_mut().enumerate() {
            *slot = [file[32 + 2 * n], file[33 + 2 * n]];
        }
        let s = |bits: u32| [u64::from(bits), 0];
        let (entry_x19, entry_x20, entry_x29, entry_sp) = (x19, x20, x29, sp);
        let entry_x30 = file[30];
        let entry_d = [v[8][0], v[9][0], v[10][0]];
        let ld32 = |base: u64, off: u64| unsafe { guest_ld32(base + off) };

        macro_rules! reload {
            () => {
                x0 = file[0];
                x1 = file[1];
                for n in 0..4 {
                    v[n] = [file[32 + 2 * n], file[33 + 2 * n]];
                }
            };
        }
        macro_rules! put_x {
            ($($n:expr => $val:expr),*) => { $( file[$n] = $val; )* };
        }
        macro_rules! put_v {
            ($($n:expr),*) => { $( file[32 + 2 * $n] = v[$n][0]; file[33 + 2 * $n] = v[$n][1]; )* };
        }

        // prologue: frame, x29, x19 = this, then the first call
        sp = sp.wrapping_sub(64);
        x29 = sp + 0x20;
        x19 = x0;
        x0 = unsafe { guest_ld64(x0 + 0x10) };
        x1 = 0;
        x30 = 0x7101e2cff4;
        put_x!(0 => x0, 1 => x1, 19 => x19, 29 => x29, 30 => x30, 31 => sp);
        if call(file, 0x7101799d40) {
            return;
        }
        reload!();
        v[0] = s(ld32(x19, 0x48));
        v[1] = s(ld32(x19, 0x4c));
        x1 = 0;
        v[2] = s(ld32(x19, 0x50));
        x30 = 0x7101e2d004;
        put_x!(1 => x1, 30 => x30);
        put_v!(0, 1, 2);
        if call(file, 0x71017a7da0) {
            return;
        }
        reload!();
        x8 = u64::from(unsafe { guest_ld8(x19 + 0x74) });
        let to_shake;
        if x8 as u32 == 0 {
            // d060: follow the look-at point
            x0 = unsafe { guest_ld64(x19 + 0x10) };
            x1 = 0;
            x30 = 0x7101e2d06c;
            put_x!(0 => x0, 1 => x1, 8 => x8, 30 => x30);
            if call(file, 0x7101799d40) {
                return;
            }
            reload!();
            x8 = 0xfa35;
            x8 |= 0x3c8e << 16;
            v[2] = s(x8 as u32);
            v[1] = s(ld32(x19, 0x4c));
            v[3] = s(ld32(x19, 0x50));
            v[1] = fop(file, 0, v[1][0], v[2][0]);
            v[10] = s(ld32(x19, 0x44));
            v[0] = s(ld32(x19, 0x48));
            v[0] = fop(file, 0, v[0][0], v[2][0]);
            v[2] = fop(file, 0, v[3][0], v[2][0]);
            v[8] = s(ld32(x19, 0x3c));
            v[9] = s(ld32(x19, 0x40));
            x20 = x0;
            x0 = 0;
            x30 = 0x7101e2d09c;
            put_x!(0 => x0, 8 => x8, 20 => x20, 30 => x30);
            put_v!(0, 1, 2, 3, 8, 9, 10);
            if call(file, 0x7102057610) {
                return;
            }
            reload!();
            v[4] = [0, 0];
            v[6] = s(ld32(x19, 0x54));
            x0 = 0;
            v[5] = v[4];
            x30 = 0x7101e2d0b0;
            put_x!(0 => x0, 30 => x30);
            put_v!(4, 5, 6);
            if call(file, 0x7102057ae0) {
                return;
            }
            reload!();
            v[0] = fop(file, 1, v[8][0], v[0][0]);
            v[1] = fop(file, 1, v[9][0], v[1][0]);
            x0 = x20;
            v[2] = fop(file, 1, v[10][0], v[2][0]);
            x1 = 0;
            x30 = 0x7101e2d0c8;
            put_x!(0 => x0, 1 => x1, 30 => x30);
            put_v!(0, 1, 2);
            if call(file, 0x71017a7910) {
                return;
            }
            reload!();
            x8 = unsafe { guest_ld64(x19 + 0x90) };
            unsafe { fake_safepoint(file.as_mut_ptr(), 0x7101e2d0cc, 8, x8) };
            to_shake = x8 != 0;
        } else {
            x8 = unsafe { guest_ld64(x19 + 0x90) };
            to_shake = x8 != 0;
        }
        if to_shake {
            // d014: add the shake offset, then jump to set_position
            x0 = unsafe { guest_ld64(x19 + 0x10) };
            x1 = 0;
            x30 = 0x7101e2d020;
            put_x!(0 => x0, 1 => x1, 8 => x8, 30 => x30);
            if call(file, 0x7101799d40) {
                return;
            }
            reload!();
            x1 = 0;
            x20 = x0;
            x30 = 0x7101e2d02c;
            put_x!(1 => x1, 20 => x20, 30 => x30);
            if call(file, 0x71017a7860) {
                return;
            }
            reload!();
            x8 = unsafe { guest_ld64(x19 + 0x90) };
            x0 = x20;
            x1 = 0;
            v[9] = [entry_d[1], 0];
            v[8] = [entry_d[0], 0];
            v[5] = s(ld32(x8, 0x3c));
            v[3] = s(ld32(x8, 0x34));
            v[4] = s(ld32(x8, 0x38));
            v[1] = fop(file, 2, v[1][0], v[4][0]);
            x20 = entry_x20;
            x19 = entry_x19;
            v[0] = fop(file, 2, v[0][0], v[3][0]);
            x29 = entry_x29;
            x30 = entry_x30;
            v[2] = fop(file, 2, v[2][0], v[5][0]);
            v[10] = [entry_d[2], 0];
            sp = entry_sp;
            put_x!(0 => x0, 1 => x1, 8 => x8, 19 => x19, 20 => x20, 29 => x29, 30 => x30, 31 => sp);
            put_v!(0, 1, 2, 3, 4, 5, 8, 9, 10);
            call(file, 0x71017a7910);
            return;
        }
        // d0d0: return (x0, x1, x8 and x30 are not flushed)
        v[9] = [entry_d[1], 0];
        v[8] = [entry_d[0], 0];
        x20 = entry_x20;
        x19 = entry_x19;
        x29 = entry_x29;
        v[10] = [entry_d[2], 0];
        sp = entry_sp;
        put_x!(19 => x19, 20 => x20, 29 => x29, 31 => sp);
        put_v!(8, 9, 10);
        let _ = (x0, x1, x30);
    }

    /// A fake callee that leaves a result in every register a real callee may clobber, derived from every
    /// register it is given, so a wrong staged register at any call changes the final file.
    fn scramble(r: &mut [u64]) {
        let mut h: u64 = 0xcbf2_9ce4_8422_2325;
        for (i, w) in r.iter().enumerate() {
            h = (h ^ *w ^ ((i as u64) << 56)).wrapping_mul(0x0000_0100_0000_01b3);
            h ^= h >> 29;
        }
        r[0] = h & 0xffff_ffff_ffff_fff0;
        r[1] = h.rotate_left(7);
        r[8] = h.rotate_left(13);
        for n in 0..4usize {
            r[32 + 2 * n] = 0x3f00_0000 + ((h.rotate_left(n as u32 * 11) >> 40) & 0x00ff_ffff);
            r[33 + 2 * n] = 0;
        }
        for n in 4..8usize {
            r[32 + 2 * n] = h.rotate_left(n as u32) & 0xffff_ffff;
            r[33 + 2 * n] = 0;
        }
    }

    const CALLEES: [u64; 6] = [
        camera_site::GET_TRANSFORM,
        camera_site::SET_EULER_ANGLES,
        camera_site::GET_POSITION,
        camera_site::SET_POSITION,
        camera_site::FROM_EULER_RAD,
        camera_site::MULTIPLY,
    ];

    fn random(state: &mut u64) -> u64 {
        *state ^= *state << 13;
        *state ^= *state >> 7;
        *state ^= *state << 17;
        *state
    }

    fn edge_float(state: &mut u64) -> u32 {
        const EDGES: [u32; 14] = [
            0,
            0x8000_0000,
            1,
            0x8000_0001,
            0x0080_0000,
            0x3f80_0000,
            0xbf80_0000,
            0x7f7f_ffff,
            0x7f80_0000,
            0xff80_0000,
            0x7fc0_0001,
            0x7f80_0001,
            0xffc0_1234,
            0x4348_0000,
        ];
        let r = random(state);
        if r & 3 == 0 {
            (r >> 8) as u32
        } else {
            EDGES[((r >> 8) as usize) % EDGES.len()]
        }
    }

    fn install_all() {
        clear();
        for target in CALLEES {
            install(target, scramble);
        }
    }

    #[test]
    fn shim_matches_the_lifted_transcription_in_every_branch_and_fp_mode() {
        let mut seed = 0x1234_5678_9abc_def1u64;
        let modes = [
            0u32,
            1 << 24,
            1 << 25,
            (1 << 24) | (1 << 25),
            1 << 22,
            2 << 22,
            3 << 22,
        ];
        let mut cases = 0;
        for locked in [0u8, 1, 0xff] {
            for shake in [false, true] {
                for fpcr in modes {
                    for _ in 0..40 {
                        let mut f = || edge_float(&mut seed);
                        let g = Graph::new(
                            locked,
                            shake,
                            [f(), f(), f()],
                            [f(), f(), f()],
                            f(),
                            [f(), f(), f()],
                        );
                        let mut file = [0u64; WORDS];
                        for w in file.iter_mut().take(96) {
                            *w = random(&mut seed);
                        }
                        file[0] = g.address();
                        file[31] = 0x7000_0000;
                        file[FPCR_WORD] =
                            (u64::from(fpcr) << 32) | (random(&mut seed) & 0xffff_ffff);
                        file[98] = random(&mut seed) & 0x0000_0000_0800_009f;
                        let mut expected = file;
                        install_all();
                        lifted_reference(&mut expected);
                        let expected_calls: Vec<u64> = CALLS.with(|c| c.borrow().clone());
                        let expected_safepoints: Vec<u64> = SAFEPOINTS.with(|c| c.borrow().clone());
                        install_all();
                        unsafe { smrpg_camera_adjust_camera_transform(file.as_mut_ptr()) };
                        let calls: Vec<u64> = CALLS.with(|c| c.borrow().clone());
                        let safepoints: Vec<u64> = SAFEPOINTS.with(|c| c.borrow().clone());
                        assert_eq!(calls, expected_calls, "call order");
                        assert_eq!(safepoints, expected_safepoints, "safepoints");
                        let diff: Vec<usize> =
                            (0..WORDS).filter(|&i| file[i] != expected[i]).collect();
                        assert!(
                            diff.is_empty(),
                            "words {diff:?} differ (locked {locked} shake {shake} fpcr {fpcr:#x}) calls {calls:x?}"
                        );
                        for i in 0..WORDS {
                            assert_eq!(
                                file[i], expected[i],
                                "word {i} (locked {locked} shake {shake} fpcr {fpcr:#x})"
                            );
                        }
                        cases += 1;
                    }
                }
            }
        }
        assert_eq!(cases, 3 * 2 * 7 * 40);
    }
}

#[cfg(test)]
mod stat_tests {
    extern crate std;
    use super::*;
    use crate::bridge::tests::{CALLS, clear, fake_guest_bl, install};
    use crate::fp::FpEnv;
    use crate::{guest_ld8, guest_ld32, guest_ld64};
    use std::vec::Vec;

    /// Fake object graph: the param at word 0 (0xa0 bytes), the buff manager at word 24, the `int[]` at word 32.
    struct Graph(std::boxed::Box<[u64; 48]>);

    const BUFF_WORD: usize = 24;
    const ARRAY_WORD: usize = 32;

    impl Graph {
        fn new(player: u8, length: u32, defence: i32, magic_defence: i32, added: [i32; 4]) -> Self {
            let mut g = Self(std::boxed::Box::new([0; 48])); // boxed: the pointers inside must not move
            let base = g.0.as_ptr() as u64;
            g.0[0xa0 / 8] = base + 8 * BUFF_WORD as u64;
            g.0[BUFF_WORD + 3] = base + 8 * ARRAY_WORD as u64; // buff + 0x18
            g.set32(0x40, defence as u32);
            g.set32(0x48, magic_defence as u32);
            g.0[0x6c / 8] |= u64::from(player) << ((0x6c % 8) * 8);
            g.set32(8 * ARRAY_WORD + 0x18, length);
            for (i, a) in added.iter().enumerate() {
                g.set32(8 * ARRAY_WORD + 0x20 + 4 * i, *a as u32);
            }
            g
        }

        fn set32(&mut self, byte: usize, value: u32) {
            let bytes = self.0.as_mut_ptr() as *mut u8;
            unsafe { bytes.add(byte).cast::<u32>().write_unaligned(value) };
        }

        fn address(&self) -> u64 {
            self.0.as_ptr() as u64
        }

        fn param(&self) -> BattleCharacterParam {
            unsafe { BattleCharacterParam::at(self.address()) }
        }
    }

    // ---- the readable body, against a recorder and an independent host-float reference ----

    #[derive(Debug, PartialEq)]
    enum Event {
        Ratio(DefenceStat, i32),
        Throw(DefenceStat, u32),
        Leave(i32),
    }

    struct Recorder {
        env: FpEnv,
        ratio: f32,
        events: Vec<Event>,
    }

    impl Float32Ops for Recorder {
        fn fadd(&mut self, a: f32, b: f32) -> f32 {
            self.env.fadd32(a, b)
        }
        fn fsub(&mut self, a: f32, b: f32) -> f32 {
            self.env.fsub32(a, b)
        }
        fn fmul(&mut self, a: f32, b: f32) -> f32 {
            self.env.fmul32(a, b)
        }
    }

    impl Float32Convert for Recorder {
        fn float_from_i32(&mut self, value: i32) -> f32 {
            self.env.float_from_i32(value)
        }
        fn trunc_to_i32(&mut self, value: f32) -> i32 {
            self.env.trunc_to_i32(value)
        }
        fn compare(&mut self, a: f32, b: f32) -> Nzcv {
            self.env.compare(a, b)
        }
    }

    impl StatCalls for Recorder {
        fn defense_up_ratio(
            &mut self,
            stat: DefenceStat,
            site: ScaleSite,
        ) -> Result<f32, GuestException> {
            self.events.push(Event::Ratio(stat, site.sum));
            Ok(self.ratio)
        }
        fn throw_index_out_of_range(
            &mut self,
            stat: DefenceStat,
            _: BattleCharacterParam,
            _: BuffManager,
            _: Int32Array,
            length: u32,
        ) -> GuestException {
            self.events.push(Event::Throw(stat, length));
            GuestException
        }
        fn leave(&mut self, result: i32) {
            self.events.push(Event::Leave(result));
        }
    }

    /// What the C# says, with host floats: `(int)(sum * ratio)`, +infinity giving `int.MinValue` as the x86 cast
    /// does (the ARM `fcvtzs` would saturate), NaN giving 0.
    fn host_scale(sum: i32, ratio: f32) -> i32 {
        let product = ratio * (sum as f32);
        if product == f32::INFINITY {
            i32::MIN
        } else {
            product as i32
        }
    }

    const INTS: [i32; 9] = [
        0,
        1,
        -1,
        100,
        -100,
        0x00ff_ffff,
        0x7fff_ffff,
        i32::MIN,
        0x0123_4567,
    ];
    const RATIOS: [u32; 14] = [
        0x3f80_0000, // 1.0
        0x3fc0_0000, // 1.5
        0x3f00_0000, // 0.5
        0,
        0x8000_0000,
        0xbf80_0000,
        0x7f80_0000, // +inf
        0xff80_0000, // -inf
        0x7fc0_0000, // NaN
        0x7f80_0001, // signaling NaN
        0x4f00_0000, // 2^31
        0x4f80_0000,
        0x7f7f_ffff,
        0x0000_0001,
    ];

    #[test]
    fn defence_follows_the_game_order_and_the_float_cast() {
        let mut cases = 0;
        for stat in [DefenceStat::Physical, DefenceStat::Magic] {
            let (index, physical) = if stat == DefenceStat::Physical {
                (1usize, true)
            } else {
                (3, false)
            };
            for player in [0u8, 1, 0x80, 0xff] {
                for length in [0u32, 1, 2, 3, 4, 100, u32::MAX] {
                    for &base in &INTS {
                        for &add in &INTS {
                            for &ratio in &RATIOS {
                                let added = [add ^ 5, add, add ^ 9, add];
                                let (defence, magic) = if physical {
                                    (base, !base)
                                } else {
                                    (!base, base)
                                };
                                let g = Graph::new(player, length, defence, magic, added);
                                let mut r = Recorder {
                                    env: FpEnv::new(0, 0),
                                    ratio: f32::from_bits(ratio),
                                    events: Vec::new(),
                                };
                                let result = if physical {
                                    g.param().calc_defence(&mut r)
                                } else {
                                    g.param().calc_magic_defence(&mut r)
                                };
                                if (length as usize) <= index {
                                    assert_eq!(result, Err(GuestException));
                                    assert_eq!(r.events, [Event::Throw(stat, length)]);
                                    continue;
                                }
                                let sum = base.wrapping_add(added[index]);
                                if player == 0 {
                                    assert_eq!(result, Ok(sum));
                                    assert_eq!(r.events, [Event::Leave(sum)]);
                                } else {
                                    let want = host_scale(sum, f32::from_bits(ratio));
                                    assert_eq!(result, Ok(want), "sum {sum} ratio {ratio:#x}");
                                    assert_eq!(
                                        r.events,
                                        [Event::Ratio(stat, sum), Event::Leave(want)]
                                    );
                                }
                                cases += 1;
                            }
                        }
                    }
                }
            }
        }
        assert!(cases > 10_000);
    }

    // ---- the shim, against a transcription of the lifted C (main_0376.c, f_71021bc270 and f_71021bc440) ----

    const WORDS: usize = 100;
    const FPCR_WORD: usize = 97; // fpcr is the high half of word 97, fpsr the low half of word 98

    fn call(file: &mut [u64; WORDS], target: u64) -> bool {
        unsafe { fake_guest_bl(file.as_mut_ptr(), target) != 0 }
    }

    /// The lifted body, line by line: locals for the registers it keeps in C variables, the flush list at each call
    /// as the C writes it, the reload of x0, v0 and v1 after the ratio call, and the flush at the return.
    #[allow(unused_assignments)]
    fn lifted_reference(file: &mut [u64; WORDS], magic: bool) {
        let (limit, base_offset, element_offset, ratio, ret_ratio, ret_throw) = if magic {
            (
                3u32,
                0x48u64,
                0x2cu64,
                0x71021c4560u64,
                0x71021bc47cu64,
                0x71021bc4b0u64,
            )
        } else {
            (1, 0x40, 0x24, 0x71021c4240, 0x71021bc2ac, 0x71021bc2e0)
        };
        let mut x0 = file[0];
        let (mut x8, mut x9, mut x10);
        let mut x19 = file[19];
        let mut x29 = file[29];
        let mut x30 = file[30];
        let mut sp = file[31];
        let mut v0 = [file[32], file[33]];
        let mut v1 = [file[34], file[35]];
        let entry_x19 = x19;
        let entry_x29 = x29;
        let entry_sp = sp;
        // stp x29, x30, [sp, #-32]!; str x19; add x29, sp, #0
        sp = sp.wrapping_sub(32);
        x29 = sp;
        x8 = x0;
        x0 = unsafe { guest_ld64(x0 + 0xa0) };
        x9 = unsafe { guest_ld64(x0 + 0x18) };
        x10 = u64::from(unsafe { guest_ld32(x9 + 0x18) });
        if (x10 as u32) <= limit {
            x30 = ret_throw;
            file[0] = x0;
            file[8] = x8;
            file[9] = x9;
            file[10] = x10;
            file[29] = x29;
            file[30] = x30;
            file[31] = sp;
            call(file, 0x71000c4510);
            return;
        }
        x10 = u64::from(unsafe { guest_ld32(x8 + base_offset) });
        x9 = u64::from(unsafe { guest_ld32(x9 + element_offset) });
        x19 = u64::from((x9 as u32).wrapping_add(x10 as u32));
        x8 = u64::from(unsafe { guest_ld8(x8 + 0x6c) });
        if x8 != 0 {
            x30 = ret_ratio;
            file[0] = x0;
            file[8] = x8;
            file[9] = x9;
            file[10] = x10;
            file[19] = x19;
            file[29] = x29;
            file[30] = x30;
            file[31] = sp;
            if call(file, ratio) {
                return;
            }
            x0 = file[0];
            v0 = [file[32], file[33]];
            v1 = [file[34], file[35]];
            let mut env = FpEnv::new((file[FPCR_WORD] >> 32) as u32, file[98] as u32);
            // scvtf s1, w19; fmul s0, s0, s1; fcvtzs w8, s0; fmov s1, w9; fcmp s0, s1; csel w19
            v1 = [
                u64::from(env.float_from_i32(x19 as u32 as i32).to_bits()),
                0,
            ];
            let product = env.fmul32(f32::from_bits(v0[0] as u32), f32::from_bits(v1[0] as u32));
            v0 = [u64::from(product.to_bits()), 0];
            x8 = u64::from(env.trunc_to_i32(product) as u32);
            x9 = 0x7f80_0000;
            v1 = [x9, 0];
            let flags = env.compare(product, f32::from_bits(v1[0] as u32));
            x9 = 0x8000_0000;
            x19 = if flags.z { x9 } else { x8 };
            file[98] = (file[98] & !0xffff_ffff) | u64::from(env.fpsr);
        }
        // mov w0, w19; ldr x19, [sp, #0x10]; ldp x29, x30, [sp], #32
        x0 = x19 & 0xffff_ffff;
        x19 = entry_x19;
        x29 = entry_x29;
        sp = entry_sp;
        file[0] = x0;
        file[19] = x19;
        file[29] = x29;
        file[32] = v0[0];
        file[33] = v0[1];
        file[34] = v1[0];
        file[35] = v1[1];
        file[31] = sp;
    }

    /// A fake callee that leaves a result in every register a real callee may clobber, derived from every
    /// register it is given, so a wrong staged register at any call changes the final file.
    fn scramble_with(r: &mut [u64], float_result: bool) {
        let mut h: u64 = 0xcbf2_9ce4_8422_2325;
        for (i, w) in r.iter().enumerate() {
            h = (h ^ *w ^ ((i as u64) << 56)).wrapping_mul(0x0000_0100_0000_01b3);
            h ^= h >> 29;
        }
        r[0] = h & 0xffff_ffff_ffff_fff0;
        r[1] = h.rotate_left(7);
        r[8] = h.rotate_left(13);
        r[9] = h.rotate_left(17);
        r[10] = h.rotate_left(19);
        for n in 0..8usize {
            r[32 + 2 * n] = h.rotate_left(n as u32 * 11) & 0xffff_ffff;
            r[33 + 2 * n] = 0;
        }
        if float_result {
            let pick = RATIOS[(h >> 40) as usize % RATIOS.len()];
            r[32] = if h & 3 == 0 {
                h & 0xffff_ffff
            } else {
                u64::from(pick)
            };
        }
    }

    fn ratio_callee(r: &mut [u64]) {
        scramble_with(r, true);
    }

    fn throw_callee(r: &mut [u64]) {
        scramble_with(r, false);
        r[0] = u64::MAX;
        r[1] = u64::MAX;
    }

    fn install_all() {
        clear();
        install(0x71021c4240, ratio_callee);
        install(0x71021c4560, ratio_callee);
        install(0x71000c4510, throw_callee);
    }

    fn random(state: &mut u64) -> u64 {
        *state ^= *state << 13;
        *state ^= *state >> 7;
        *state ^= *state << 17;
        *state
    }

    #[test]
    fn shim_matches_the_lifted_transcription_in_every_branch_and_fp_mode() {
        let mut seed = 0x9e37_79b9_7f4a_7c15u64;
        let modes = [
            0u32,
            1 << 24,
            1 << 25,
            (1 << 24) | (1 << 25),
            1 << 22,
            2 << 22,
            3 << 22,
        ];
        let mut cases = 0;
        for magic in [false, true] {
            for player in [0u8, 1, 0xff] {
                for length in [0u32, 1, 2, 3, 4, 9] {
                    for fpcr in modes {
                        for pick in 0..24 {
                            let base = INTS[(random(&mut seed) >> 8) as usize % INTS.len()];
                            let added = [
                                INTS[pick % INTS.len()],
                                INTS[(pick + 3) % INTS.len()],
                                0x1234,
                                INTS[(pick + 5) % INTS.len()],
                            ];
                            let g = Graph::new(player, length, base, !base, added);
                            let mut file = [0u64; WORDS];
                            for w in file.iter_mut().take(96) {
                                *w = random(&mut seed);
                            }
                            file[0] = g.address();
                            file[31] = 0x7000_0000;
                            file[FPCR_WORD] =
                                (u64::from(fpcr) << 32) | (random(&mut seed) & 0xffff_ffff);
                            file[98] = random(&mut seed) & 0x0000_0000_0800_009f;
                            let mut expected = file;
                            install_all();
                            lifted_reference(&mut expected, magic);
                            let expected_calls: Vec<u64> = CALLS.with(|c| c.borrow().clone());
                            install_all();
                            unsafe {
                                if magic {
                                    smrpg_param_calc_magic_defence(file.as_mut_ptr());
                                } else {
                                    smrpg_param_calc_defence(file.as_mut_ptr());
                                }
                            }
                            let calls: Vec<u64> = CALLS.with(|c| c.borrow().clone());
                            assert_eq!(calls, expected_calls, "call order");
                            for i in 0..WORDS {
                                assert_eq!(
                                    file[i], expected[i],
                                    "word {i} (magic {magic} player {player} length {length} fpcr {fpcr:#x})"
                                );
                            }
                            cases += 1;
                        }
                    }
                }
            }
        }
        assert_eq!(cases, 2 * 3 * 6 * 7 * 24);
    }
}
