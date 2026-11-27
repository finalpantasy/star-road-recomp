//! Typed views of IL2CPP objects in guest memory (task-51). The readable ports in the class modules
//! (`battle.rs` and later ones) are built on this layer and contain no raw address arithmetic.
//!
//! A class is a one-field wrapper around the object's guest address (identity mapped, so a guest address is a
//! host address). Each class module names its fields once, at the offset the IL2CPP field table gives, and the
//! port reads them through those names. Field reads are relaxed atomic loads like the lifted code's.

use crate::{guest_ld8, guest_ld32, guest_ld64};

/// A guest object: a class wrapper around its address.
pub trait GuestObject: Copy {
    /// Wrap a guest address.
    ///
    /// # Safety
    /// The address must be null or point at an object of this class in mapped guest memory, and stay mapped
    /// for the call. Reading a field of a null or stale object faults exactly as the original would.
    unsafe fn at(address: u64) -> Self;

    fn address(self) -> u64;

    fn is_null(self) -> bool {
        self.address() == 0
    }

    /// `bool` field: any non-zero byte is true, like the compiled C#.
    fn bool_field(self, offset: u64) -> bool {
        self.u8_field(offset) != 0
    }

    fn u8_field(self, offset: u64) -> u8 {
        unsafe { guest_ld8(self.address().wrapping_add(offset)) }
    }

    fn i32_field(self, offset: u64) -> i32 {
        self.u32_field(offset) as i32
    }

    /// `float` field: the 32 bits as they are, a NaN payload included.
    fn f32_field(self, offset: u64) -> f32 {
        f32::from_bits(self.u32_field(offset))
    }

    fn u32_field(self, offset: u64) -> u32 {
        unsafe { guest_ld32(self.address().wrapping_add(offset)) }
    }

    /// Reference field the original dereferences without a null check.
    fn object_field<T: GuestObject>(self, offset: u64) -> T {
        unsafe { T::at(guest_ld64(self.address().wrapping_add(offset))) }
    }

    /// Reference field the original tests against null first.
    fn optional_field<T: GuestObject>(self, offset: u64) -> Option<T> {
        let object: T = self.object_field(offset);
        (!object.is_null()).then_some(object)
    }
}

/// Declares a class wrapper: `guest_class!(BattleCharacterParam);`
macro_rules! guest_class {
    ($(#[$meta:meta])* $name:ident) => {
        $(#[$meta])*
        #[derive(Clone, Copy, Debug, PartialEq, Eq)]
        pub struct $name(u64);

        impl $crate::object::GuestObject for $name {
            unsafe fn at(address: u64) -> Self {
                Self(address)
            }

            fn address(self) -> u64 {
                self.0
            }
        }
    };
}
pub(crate) use guest_class;

guest_class!(
    /// A managed `int[]`: the length at +0x18, the elements from +0x20 (task-54).
    Int32Array
);

impl Int32Array {
    const LENGTH: u64 = 0x18;
    const ELEMENTS: u64 = 0x20;

    /// The `Length` word. The compiled code compares it as an unsigned 32-bit value.
    pub fn len(self) -> u32 {
        self.u32_field(Self::LENGTH)
    }

    pub fn is_empty(self) -> bool {
        self.len() == 0
    }

    /// Element `index`, read without a bounds test: the caller has compared `index` with `len()` itself.
    pub fn element(self, index: u64) -> i32 {
        self.i32_field(Self::ELEMENTS + 4 * index)
    }
}
