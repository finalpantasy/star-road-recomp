#![no_std]

#[cfg(not(test))]
#[panic_handler]
fn panic(_: &core::panic::PanicInfo<'_>) -> ! {
    unsafe extern "C" {
        fn abort() -> !;
    }
    unsafe { abort() }
}

#[unsafe(no_mangle)]
pub extern "C" fn smrpg_min_i32(a: i32, b: i32, _: i32) -> i32 {
    a.min(b)
}

#[unsafe(no_mangle)]
pub extern "C" fn smrpg_max_i32(a: i32, b: i32, _: i32) -> i32 {
    a.max(b)
}

#[unsafe(no_mangle)]
pub extern "C" fn smrpg_in_range_i32(value: i32, low: i32, high: i32) -> i32 {
    i32::from(low <= value && value <= high)
}
