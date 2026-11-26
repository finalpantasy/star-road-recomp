#![no_std]

#[cfg(not(test))]
#[panic_handler]
fn panic(_: &core::panic::PanicInfo<'_>) -> ! {
    unsafe extern "C" {
        fn abort() -> !;
    }
    unsafe { abort() }
}

// Every leaf takes the guest x0..x2 and returns the new x0 (a 32-bit result is zero extended).
#[unsafe(no_mangle)]
pub extern "C" fn smrpg_min_i32(a: u64, b: u64, _: u64) -> u64 {
    u64::from((a as i32).min(b as i32) as u32)
}

#[unsafe(no_mangle)]
pub extern "C" fn smrpg_max_i32(a: u64, b: u64, _: u64) -> u64 {
    u64::from((a as i32).max(b as i32) as u32)
}

#[unsafe(no_mangle)]
pub extern "C" fn smrpg_in_range_i32(value: u64, low: u64, high: u64) -> u64 {
    let (value, low, high) = (value as i32, low as i32, high as i32);
    u64::from(low <= value && value <= high)
}

/// Pointer-sized equality (System.Type and IntPtr operators).
#[unsafe(no_mangle)]
pub extern "C" fn smrpg_ptr_eq(a: u64, b: u64, _: u64) -> u64 {
    u64::from(a == b)
}

#[unsafe(no_mangle)]
pub extern "C" fn smrpg_ptr_ne(a: u64, b: u64, _: u64) -> u64 {
    u64::from(a != b)
}

/// HashHelpers.Combine(int h1, int h2): ((h1 rol 5) + h1) ^ h2 on 32 bits.
#[unsafe(no_mangle)]
pub extern "C" fn smrpg_hash_combine(h1: u64, h2: u64, _: u64) -> u64 {
    let h1 = h1 as u32;
    u64::from(h1.rotate_left(5).wrapping_add(h1) ^ h2 as u32)
}

/// Array element address: base + (int32)index * element_size + 0x20 (IL2CPP array header).
#[unsafe(no_mangle)]
pub extern "C" fn smrpg_array_elem_addr(base: u64, index: u64, size: u64) -> u64 {
    base.wrapping_add((index as i32 as i64 as u64).wrapping_mul(size))
        .wrapping_add(0x20)
}

/// System.String.GetRawStringData: address of the first char, string + 0x14.
#[unsafe(no_mangle)]
pub extern "C" fn smrpg_add_0x14(a: u64, _: u64, _: u64) -> u64 {
    a.wrapping_add(0x14)
}

/// VisualElement.get_computedStyle: address of the style field, object + 0x2c8.
#[unsafe(no_mangle)]
pub extern "C" fn smrpg_add_0x2c8(a: u64, _: u64, _: u64) -> u64 {
    a.wrapping_add(0x2c8)
}

/// Byte array element address: base + (int32)index (sxtw, no shift, no header offset).
#[unsafe(no_mangle)]
pub extern "C" fn smrpg_add_sext32(base: u64, index: u64, _: u64) -> u64 {
    base.wrapping_add((index as i32 as i64) as u64)
}

/// 16.16 fixed-point multiply, rounded: ((a*b) + (a*b >> 63) + 0x8000) >> 16, all arithmetic 64-bit.
#[unsafe(no_mangle)]
pub extern "C" fn smrpg_fixmul_16(a: u64, b: u64, _: u64) -> u64 {
    let p = a.wrapping_mul(b) as i64;
    let p = p.wrapping_add(p >> 63).wrapping_add(0x8000);
    (p >> 16) as u64
}

// ---- Leaves that read guest memory (task-42) ----
// Guest memory is identity mapped: a guest address is a host address (runtime/ir_ops.h, MLD/ir_ld, which
// use relaxed atomic loads). The leaves below read through the same relaxed atomic loads and never store.

/// Relaxed 64-bit guest load, the same access as ir_ld(8, ...).
///
/// # Safety
/// `addr` must be a mapped, 8-byte aligned guest address.
#[inline(always)]
unsafe fn guest_ld64(addr: u64) -> u64 {
    unsafe {
        core::sync::atomic::AtomicU64::from_ptr(addr as *mut u64)
            .load(core::sync::atomic::Ordering::Relaxed)
    }
}

/// Relaxed 32-bit guest load, the same access as ir_ld(4, ...).
///
/// # Safety
/// `addr` must be a mapped, 4-byte aligned guest address.
#[inline(always)]
unsafe fn guest_ld32(addr: u64) -> u32 {
    unsafe {
        core::sync::atomic::AtomicU32::from_ptr(addr as *mut u32)
            .load(core::sync::atomic::Ordering::Relaxed)
    }
}

/// Unity native null-safe getter: object == 0 ? 0 : *(u64 *)(object + 0x10).
#[inline(always)]
unsafe fn null_safe_ld64_0x10(object: u64) -> u64 {
    if object == 0 {
        object
    } else {
        unsafe { guest_ld64(object.wrapping_add(0x10)) }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_ld64_nullsafe_0x10(object: u64, _: u64, _: u64) -> u64 {
    unsafe { null_safe_ld64_0x10(object) }
}

/// Static slot getter: *(u64 *)(0x7103ded000 + 0x330) (a global pointer in the data segment).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_static_7103ded330(_: u64, _: u64, _: u64) -> u64 {
    unsafe { guest_ld64(0x7103ded000 + 0x330) }
}

/// The same global slot plus 0x18 (address of a field inside the object it points to).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_static_7103ded330_add_0x18(_: u64, _: u64, _: u64) -> u64 {
    unsafe { guest_ld64(0x7103ded000 + 0x330).wrapping_add(0x18) }
}

/// Static slot getter: *(u64 *)(0x7103d82000 + 0x3f8).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_static_7103d823f8(_: u64, _: u64, _: u64) -> u64 {
    unsafe { guest_ld64(0x7103d82000 + 0x3f8) }
}

/// System.String.IsNullOrEmpty: string == 0 || *(u32 *)(string + 0x10) == 0 (the length field).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_string_is_null_or_empty(string: u64, _: u64, _: u64) -> u64 {
    u64::from(string == 0 || unsafe { guest_ld32(string.wrapping_add(0x10)) } == 0)
}

// ---- Leaves that write guest memory (task-43) ----
// Store leaves take a fourth argument, `delta`, added to every store address. The game passes 0. The `ab`
// check passes shadow minus real address so the stores land in a private shadow buffer while the lifted body
// writes live memory (runtime/decomp/swap.c). Stores are relaxed atomics like ir_st (runtime/ir_ops.h).

/// Relaxed 64-bit guest store, the same access as ir_st(8, ...).
///
/// # Safety
/// `addr` must be a mapped, 8-byte aligned guest address.
#[inline(always)]
unsafe fn guest_st64(addr: u64, value: u64) {
    unsafe {
        core::sync::atomic::AtomicU64::from_ptr(addr as *mut u64)
            .store(value, core::sync::atomic::Ordering::Relaxed)
    }
}

/// Relaxed 32-bit guest store, the same access as ir_st(4, ...).
///
/// # Safety
/// `addr` must be a mapped, 4-byte aligned guest address.
#[inline(always)]
unsafe fn guest_st32(addr: u64, value: u32) {
    unsafe {
        core::sync::atomic::AtomicU32::from_ptr(addr as *mut u32)
            .store(value, core::sync::atomic::Ordering::Relaxed)
    }
}

/// Relaxed 8-bit guest store, the same access as ir_st(1, ...).
///
/// # Safety
/// `addr` must be a mapped guest address.
#[inline(always)]
unsafe fn guest_st8(addr: u64, value: u8) {
    unsafe {
        core::sync::atomic::AtomicU8::from_ptr(addr as *mut u8)
            .store(value, core::sync::atomic::Ordering::Relaxed)
    }
}

/// Unity native, zero a 12 byte struct: *(u64 *)obj = 0; *(u32 *)(obj + 8) = 0 (unnamed).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_zero_12(obj: u64, _: u64, _: u64, delta: u64) -> u64 {
    let at = obj.wrapping_add(delta);
    unsafe {
        guest_st64(at, 0);
        guest_st32(at.wrapping_add(8), 0);
    }
    obj
}

/// CRI native, fill a struct at x2: *(u64 *)x2 = x0; *(u64 *)(x2 + 8) = x0; *(u32 *)(x2 + 0x10) = x1 (unnamed).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_store_x0_pair_x1_0x10(a: u64, b: u64, dest: u64, delta: u64) -> u64 {
    let at = dest.wrapping_add(delta);
    unsafe {
        guest_st64(at, a);
        guest_st64(at.wrapping_add(8), a);
        guest_st32(at.wrapping_add(0x10), b as u32);
    }
    a
}

/// Unity native setter: *(u32 *)(obj + 4) = value (unnamed).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_store_w1_0x4(obj: u64, value: u64, _: u64, delta: u64) -> u64 {
    unsafe { guest_st32(obj.wrapping_add(delta).wrapping_add(4), value as u32) };
    obj
}

/// Battle.BattleCharacterAnimator.set_idle_id(int, int): two int fields at +0x54 and +0x58.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_set_idle_id(obj: u64, a: u64, b: u64, delta: u64) -> u64 {
    let at = obj.wrapping_add(delta).wrapping_add(0x54);
    unsafe {
        guest_st32(at, a as u32);
        guest_st32(at.wrapping_add(4), b as u32);
    }
    obj
}

/// Flag.off(): clear the byte flag at +0x10.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_flag_off(obj: u64, _: u64, _: u64, delta: u64) -> u64 {
    unsafe { guest_st8(obj.wrapping_add(delta).wrapping_add(0x10), 0) };
    obj
}

#[cfg(test)]
mod tests {
    use super::*;

    // Fake guest memory: 64 words, addressed by their host address (identity mapping).
    fn region(seed: u64) -> [u64; 64] {
        let mut s = seed | 1;
        let mut words = [0u64; 64];
        for w in &mut words {
            s ^= s << 13;
            s ^= s >> 7;
            s ^= s << 17;
            *w = match s % 4 {
                0 => 0,
                1 => s >> 60,
                _ => s,
            };
        }
        words
    }

    #[test]
    fn nullsafe_load_matches_reference() {
        for seed in 1..200 {
            let words = region(seed);
            let base = words.as_ptr() as u64;
            assert_eq!(unsafe { smrpg_ld64_nullsafe_0x10(0, 5, 6) }, 0);
            for k in 0..60u64 {
                let got = unsafe { smrpg_ld64_nullsafe_0x10(base + 8 * k, 0, 0) };
                assert_eq!(got, words[(k + 2) as usize]);
            }
        }
    }

    #[test]
    fn string_is_null_or_empty_matches_reference() {
        for seed in 1..200 {
            let words = region(seed);
            let base = words.as_ptr() as u64;
            assert_eq!(unsafe { smrpg_string_is_null_or_empty(0, 0, 0) }, 1);
            for k in 0..60u64 {
                let length = words[(k + 2) as usize] as u32;
                let got = unsafe { smrpg_string_is_null_or_empty(base + 8 * k, 0, 0) };
                assert_eq!(got, u64::from(length == 0));
            }
        }
    }

    #[test]
    fn store_leaves_write_expected_bytes() {
        let mut m = [0xa5a5_a5a5_a5a5_a5a5u64; 8];
        let base = m.as_mut_ptr() as u64;
        unsafe {
            assert_eq!(smrpg_zero_12(base, 1, 2, 0), base);
            assert_eq!(m[0], 0);
            assert_eq!(m[1] as u32, 0);
            assert_eq!(m[1] >> 32, 0xa5a5_a5a5);
            smrpg_store_w1_0x4(base + 8, 0x1122_3344, 0, 0);
            assert_eq!(m[1] >> 32, 0x1122_3344);
            assert_eq!(smrpg_store_x0_pair_x1_0x10(7, 9, base + 16, 0), 7);
            assert_eq!((m[2], m[3], m[4] as u32), (7, 7, 9));
            smrpg_set_idle_id(base + 40 - 0x54, 5, 6, 0);
            assert_eq!(m[5], 5 | (6 << 32));
            smrpg_flag_off(base + 48 - 0x10, 0, 0, 0);
            assert_eq!(m[6], 0xa5a5_a5a5_a5a5_a500);
        }
    }
}
