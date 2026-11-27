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

// ---- Helpers for the generated functions (task-45) ----
// The 8 and 32 bit loads and stores above are shared; these are the 16 bit and 8 bit load forms and the 16 bit
// store that the generated module (tools/decompgen) needs. All are relaxed atomics like ir_ld and ir_st.

/// Relaxed 16-bit guest load, the same access as ir_ld(2, ...).
///
/// # Safety
/// `addr` must be a mapped, 2-byte aligned guest address.
#[inline(always)]
#[allow(dead_code)]
unsafe fn guest_ld16(addr: u64) -> u16 {
    unsafe {
        core::sync::atomic::AtomicU16::from_ptr(addr as *mut u16)
            .load(core::sync::atomic::Ordering::Relaxed)
    }
}

/// Relaxed 8-bit guest load, the same access as ir_ld(1, ...).
///
/// # Safety
/// `addr` must be a mapped guest address.
#[inline(always)]
#[allow(dead_code)]
unsafe fn guest_ld8(addr: u64) -> u8 {
    unsafe {
        core::sync::atomic::AtomicU8::from_ptr(addr as *mut u8)
            .load(core::sync::atomic::Ordering::Relaxed)
    }
}

/// Relaxed 16-bit guest store, the same access as ir_st(2, ...).
///
/// # Safety
/// `addr` must be a mapped, 2-byte aligned guest address.
#[inline(always)]
#[allow(dead_code)]
unsafe fn guest_st16(addr: u64, value: u16) {
    unsafe {
        core::sync::atomic::AtomicU16::from_ptr(addr as *mut u16)
            .store(value, core::sync::atomic::Ordering::Relaxed)
    }
}

// The bulk-generated functions (python -I tools/decompgen/generate.py). They derive from the game binary, so
// the file lives on A: and build.rs finds it (SMRPG_DECOMPGEN_DIR). Without it the crate builds without them.
#[cfg(smrpg_gen)]
#[allow(dead_code, clippy::all)]
mod generated {
    include!(env!("SMRPG_GEN_RS"));
}

// ---- Functions that call ported leaves (task-44) ----
// These take a pointer to the guest register file (Ctx.x, 32 u64 with x[31] = sp) and edit it in place, so
// every register the lifted body changes is reproduced, not only x0. Callees are the Rust leaves above,
// called directly (never twice on live memory). There is no guest stack: the lifted prologue and epilogue
// save and restore x29 and x30 below sp, which leaves sp, x29 and the callee-saved registers unchanged, so
// Rust leaves them alone. The one stack artifact it copies is the lifter's stale x30: before a `bl` the
// lifted body stores the return address into the context and does not restore it afterwards.

/// Write register `index` of the guest register file.
#[inline(always)]
unsafe fn set_x(x: *mut u64, index: usize, value: u64) {
    unsafe { x.add(index).write(value) }
}

#[inline(always)]
unsafe fn get_x(x: *const u64, index: usize) -> u64 {
    unsafe { x.add(index).read() }
}

/// 0x71005b3d50: x2 = 8, then tail call to the array element address leaf (8-byte elements).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_call_array_elem_addr_8(x: *mut u64) {
    unsafe {
        set_x(x, 2, 8);
        set_x(x, 0, smrpg_array_elem_addr(get_x(x, 0), get_x(x, 1), 8));
    }
}

/// 0x71005b3d70: x2 = 8, x0 = *(u64 *)element_address(x0, x1, 8), with a stack frame and a `bl`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_call_array_load_8(x: *mut u64) {
    unsafe {
        set_x(x, 2, 8);
        set_x(x, 30, 0x71005b3d80);
        let address = smrpg_array_elem_addr(get_x(x, 0), get_x(x, 1), 8);
        set_x(x, 0, guest_ld64(address));
    }
}

/// 0x71005ae5a0: x0 = object == 0 ? 0 : (field = *(u64 *)(object + 0x10), field == 0 ? 0 : *(u32 *)(field + 8)).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_call_nested_u32_8(x: *mut u64) {
    unsafe {
        let object = get_x(x, 0);
        if object == 0 {
            return;
        }
        set_x(x, 30, 0x71005ae5b0);
        let field = null_safe_ld64_0x10(object);
        set_x(x, 0, field);
        if field != 0 {
            set_x(x, 0, u64::from(guest_ld32(field.wrapping_add(8))));
        }
    }
}

/// 0x7101cccaf0: w1 == 0x81d ? 1 : is_in(w1, 0x7f8, 0x7fb), with x1, x2 and x3 set for the tail call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_call_in_range_7f8(x: *mut u64) {
    unsafe {
        let value = get_x(x, 1) as u32;
        if value == 0x81d {
            set_x(x, 0, 1);
            return;
        }
        set_x(x, 2, 0x7fb);
        set_x(x, 1, 0x7f8);
        set_x(x, 3, 0);
        set_x(x, 0, smrpg_in_range_i32(u64::from(value), 0x7f8, 0x7fb));
    }
}

/// 0x71001919f0: x2 = 0, x0 = (*(u64 *)(x0 + 0x10) == x1), a tail call to the pointer equality leaf.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_call_field_ptr_eq(x: *mut u64) {
    unsafe {
        let field = guest_ld64(get_x(x, 0).wrapping_add(0x10));
        set_x(x, 2, 0);
        set_x(x, 0, smrpg_ptr_eq(field, get_x(x, 1), 0));
    }
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

    #[test]
    fn caller_functions_match_reference() {
        let mut m = region(7);
        let base = m.as_ptr() as u64;
        let mut x = [0u64; 32];
        unsafe {
            // 0x71005b3d70: element address base + 8 * index + 0x20, then load
            x[0] = base;
            x[1] = 3;
            smrpg_call_array_load_8(x.as_mut_ptr());
            assert_eq!((x[0], x[2], x[30]), (m[7], 8, 0x71005b3d80));
            x[0] = base;
            smrpg_call_array_elem_addr_8(x.as_mut_ptr());
            assert_eq!(x[0], base + 24 + 0x20);
            // 0x71001919f0: compare the field at +0x10 with x1
            m[2] = 0x1234;
            x[0] = base;
            x[1] = 0x1234;
            smrpg_call_field_ptr_eq(x.as_mut_ptr());
            assert_eq!((x[0], x[2]), (1, 0));
            // 0x71005ae5a0: object -> field -> u32 at +8
            m[2] = base + 8 * 10;
            m[11] = 0xdead_beef_0bad_f00d;
            x[0] = base;
            smrpg_call_nested_u32_8(x.as_mut_ptr());
            assert_eq!(x[0], 0x0bad_f00d);
            x[0] = 0;
            smrpg_call_nested_u32_8(x.as_mut_ptr());
            assert_eq!(x[0], 0);
            // 0x7101cccaf0
            x[1] = 0x81d;
            smrpg_call_in_range_7f8(x.as_mut_ptr());
            assert_eq!(x[0], 1);
            x[1] = 0x7fb;
            smrpg_call_in_range_7f8(x.as_mut_ptr());
            assert_eq!((x[0], x[1], x[2], x[3]), (1, 0x7f8, 0x7fb, 0));
            x[1] = 0x7fc;
            smrpg_call_in_range_7f8(x.as_mut_ptr());
            assert_eq!(x[0], 0);
        }
    }
}
