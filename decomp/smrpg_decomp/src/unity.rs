//! UnityEngine value types and classes the readable ports touch (task-53). Names are Unity's.

use crate::fp::Float32Ops;
use crate::object::guest_class;

guest_class!(
    /// `UnityEngine.Camera`
    Camera
);
guest_class!(
    /// `UnityEngine.Transform`
    Transform
);

/// `UnityEngine.Mathf.Deg2Rad` as the game was built with it: 0x3c8efa35, pi / 180 rounded to single.
pub const DEG2RAD: f32 = f32::from_bits(0x3c8e_fa35);

/// `UnityEngine.Vector3` (three floats). Arithmetic goes through `Float32Ops` so every operation sets the FPSR
/// flags the compiled code sets, in the compiled operand order (`a.sub(b)` is `a - b`, first operand first).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Vector3 {
    pub x: f32,
    pub y: f32,
    pub z: f32,
}

impl Vector3 {
    pub const fn new(x: f32, y: f32, z: f32) -> Self {
        Self { x, y, z }
    }

    /// Component-wise `self * k` (the game scales an angle vector by `Deg2Rad`).
    pub fn scaled<F: Float32Ops>(self, k: f32, fp: &mut F) -> Self {
        Self::new(fp.fmul(self.x, k), fp.fmul(self.y, k), fp.fmul(self.z, k))
    }

    pub fn add<F: Float32Ops>(self, other: Self, fp: &mut F) -> Self {
        Self::new(
            fp.fadd(self.x, other.x),
            fp.fadd(self.y, other.y),
            fp.fadd(self.z, other.z),
        )
    }

    pub fn sub<F: Float32Ops>(self, other: Self, fp: &mut F) -> Self {
        Self::new(
            fp.fsub(self.x, other.x),
            fp.fsub(self.y, other.y),
            fp.fsub(self.z, other.z),
        )
    }

    /// Same bits, for tests and the compare of two results that may hold NaNs.
    pub fn bits(self) -> [u32; 3] {
        [self.x.to_bits(), self.y.to_bits(), self.z.to_bits()]
    }
}

/// `UnityEngine.Quaternion` (x, y, z, w).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Quaternion {
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub w: f32,
}
