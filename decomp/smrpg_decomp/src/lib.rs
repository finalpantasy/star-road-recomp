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
