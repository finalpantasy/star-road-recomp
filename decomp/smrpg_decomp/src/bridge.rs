//! Guest-call bridge (task-52): lets a readable Rust port call a function that is still lifted C.
//!
//! A port that replaces a lifted function receives the guest register file (`Ctx.x`: x0 to x30, then sp) and edits
//! it in place, exactly as the lifted body does. A call to another guest function does what the lifted `bl` does:
//! stage x0 to x7, write the return address to x30, and run the callee on the same register file. The callee's
//! stack frame therefore sits below the caller's sp, the same place the lifted code's frame would be, and the
//! registers the callee leaves behind (x8, x30, v0 to v7 and so on) stay in the file as residue, which is what the
//! `ab` comparison of the whole register file expects.
//!
//! Policy for `ab` mode lives in `runtime/decomp/swap.c` (`smrpg_guest_bl`): a callee that is not on the pure
//! allowlist stops the run instead of executing twice on live memory (see docs/DECOMP.md, "Guest-call bridge").

/// A guest exception was left pending by the callee. The lifted caller returns at once in that case, and so must
/// the port, with the register file untouched.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct GuestException;

/// The guest register file of the call being replaced: 32 words (x0 to x30, sp), followed in memory by v0 to v31
/// (two words each; `swap.c` asserts the layout).
pub struct Regs {
    x: *mut u64,
}

#[cfg(not(test))]
unsafe extern "C" {
    /// `runtime/decomp/swap.c`: runs the lifted function at `target` on the register file `x`; 1 if an exception
    /// is pending afterwards.
    fn smrpg_guest_bl(x: *mut u64, target: u64) -> i32;
}

#[cfg(not(test))]
unsafe extern "C" {
    /// `runtime/decomp/swap.c`: the loop back-edge safepoint of the lifted code (task-53): with a collection
    /// pending it records `pc` in the context and polls; otherwise nothing.
    fn smrpg_guest_safepoint(x: *mut u64, pc: u64, reg: u32, value: u64);
}

#[cfg(test)]
pub(crate) use tests::fake_guest_bl as smrpg_guest_bl;
#[cfg(test)]
pub(crate) use tests::fake_safepoint as smrpg_guest_safepoint;

use crate::fp::FpEnv;

const SP: usize = 31;
const V0: usize = 32;
/// Byte offsets of `Ctx.fpcr` and `Ctx.fpsr` from the start of `Ctx.x` (`swap.c` asserts them).
const FPCR_BYTE: usize = 780;
const FPSR_BYTE: usize = 784;

impl Regs {
    /// # Safety
    /// `x` must point at the 32 general registers of a live guest context, followed by its vector registers.
    pub unsafe fn from_raw(x: *mut u64) -> Self {
        Self { x }
    }

    pub fn x(&self, n: usize) -> u64 {
        assert!(n < 32);
        unsafe { self.x.add(n).read() }
    }

    pub fn set_x(&mut self, n: usize, value: u64) {
        assert!(n < 32);
        unsafe { self.x.add(n).write(value) }
    }

    pub fn sp(&self) -> u64 {
        self.x(SP)
    }

    /// Low 64 bits of vector register `n` (a float result is in the low 32 bits).
    pub fn v_low(&self, n: usize) -> u64 {
        assert!(n < 32);
        unsafe { self.x.add(V0 + 2 * n).read() }
    }

    /// Writes a float argument the way `fmov s<n>, ...` does: the other bits of the register become zero.
    pub fn set_f32(&mut self, n: usize, value: f32) {
        assert!(n < 32);
        unsafe {
            self.x.add(V0 + 2 * n).write(u64::from(value.to_bits()));
            self.x.add(V0 + 2 * n + 1).write(0);
        }
    }

    /// Writes the two halves of vector register `n` (a `ldr d` load is `{value, 0}`).
    pub fn set_v(&mut self, n: usize, low: u64, high: u64) {
        assert!(n < 32);
        unsafe {
            self.x.add(V0 + 2 * n).write(low);
            self.x.add(V0 + 2 * n + 1).write(high);
        }
    }

    /// The float in the low 32 bits of vector register `n`.
    pub fn v_f32(&self, n: usize) -> f32 {
        f32::from_bits(self.v_low(n) as u32)
    }

    /// FPCR and FPSR of the context. Float arithmetic reads them when it runs, because a callee can change the
    /// cumulative flags; it writes the flags back with `set_fpsr`. The register file must be a whole `Ctx`.
    pub fn fp_env(&self) -> FpEnv {
        let bytes = self.x as *const u8;
        unsafe {
            FpEnv::new(
                bytes.add(FPCR_BYTE).cast::<u32>().read(),
                bytes.add(FPSR_BYTE).cast::<u32>().read(),
            )
        }
    }

    pub fn set_fpsr(&mut self, fpsr: u32) {
        unsafe { (self.x as *mut u8).add(FPSR_BYTE).cast::<u32>().write(fpsr) }
    }

    /// The lifted loop back-edge poll at `pc`. The lifted code flushes one register only when the poll fires
    /// (`x<reg> = value`), so the register is passed along and written only then; the port keeps every other
    /// register in the file already.
    pub fn safepoint(&mut self, pc: u64, reg: u32, value: u64) {
        unsafe { smrpg_guest_safepoint(self.x, pc, reg, value) }
    }

    pub fn f32_result(&self) -> f32 {
        f32::from_bits(self.v_low(0) as u32)
    }

    /// The lifted `bl target`: arguments to x0 to x7 (registers past `args` keep what the port left in them),
    /// x30 = `return_address` (the address after the `bl`, as the lifted caller writes it), then the callee runs.
    /// Returns x0.
    pub fn call(
        &mut self,
        target: u64,
        return_address: u64,
        args: &[u64],
    ) -> Result<u64, GuestException> {
        assert!(args.len() <= 8);
        for (n, &value) in args.iter().enumerate() {
            self.set_x(n, value);
        }
        self.set_x(30, return_address);
        self.invoke(target)
    }

    /// Runs the callee on the register file exactly as it stands: the port has staged every register itself, the
    /// way the lifted code does at that call site (it writes only the registers it changed since the last call).
    /// A tail call (`b target`) is the same with x30 left alone. Returns x0.
    pub fn invoke(&mut self, target: u64) -> Result<u64, GuestException> {
        if unsafe { smrpg_guest_bl(self.x, target) } != 0 {
            return Err(GuestException);
        }
        Ok(self.x(0))
    }
}

/// Selftest row (task-52): 0x7101b8abd0 is a thunk, `b 0x7100049adc`, so the whole function is one tail call. It
/// exists to compare the bridge against the lifted tail call over a fake object graph (swap.c, `prepare_caller`).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smrpg_bridge_selftest_thunk(x: *mut u64) {
    let mut regs = unsafe { Regs::from_raw(x) };
    // An exception stays pending in the register file, which is all the lifted tail call leaves too.
    let _ = regs.invoke(0x7100049adc);
}

#[cfg(test)]
pub(crate) mod tests {
    extern crate std;
    use super::*;
    use std::cell::RefCell;
    use std::vec::Vec;

    /// A fake lifted function: edits the 32 + 64 words of the register file.
    pub type Fake = fn(&mut [u64]);

    std::thread_local! {
        static CALLEES: RefCell<Vec<(u64, Fake)>> = const { RefCell::new(Vec::new()) };
        pub static CALLS: RefCell<Vec<u64>> = const { RefCell::new(Vec::new()) };
        pub static SAFEPOINTS: RefCell<Vec<u64>> = const { RefCell::new(Vec::new()) };
    }

    pub unsafe fn fake_safepoint(_x: *mut u64, pc: u64, _reg: u32, _value: u64) {
        SAFEPOINTS.with(|s| s.borrow_mut().push(pc));
    }

    pub fn install(target: u64, body: Fake) {
        CALLEES.with(|c| c.borrow_mut().push((target, body)));
    }

    pub fn clear() {
        CALLEES.with(|c| c.borrow_mut().clear());
        CALLS.with(|c| c.borrow_mut().clear());
        SAFEPOINTS.with(|c| c.borrow_mut().clear());
    }

    /// Stands in for `smrpg_guest_bl` in unit tests: runs the installed fake on the register words.
    pub unsafe fn fake_guest_bl(x: *mut u64, target: u64) -> i32 {
        CALLS.with(|c| c.borrow_mut().push(target));
        let body = CALLEES.with(|c| {
            c.borrow()
                .iter()
                .find(|(t, _)| *t == target)
                .map(|(_, b)| *b)
        });
        let regs = unsafe { std::slice::from_raw_parts_mut(x, 32 + 64) };
        body.expect("call to an uninstalled fake callee")(regs);
        i32::from(regs[0] == u64::MAX && regs[1] == u64::MAX) // a fake raises "exception" by setting x0 = x1 = all ones
    }

    #[test]
    fn call_stages_arguments_and_return_address_and_reads_x0() {
        clear();
        install(0x1234, |r| {
            assert_eq!((r[0], r[1], r[2], r[30]), (7, 8, 9, 0x5555));
            r[0] = r[0] + r[1] + r[2];
            r[8] = 0xdead; // residue stays in the file
            r[V0_WORD] = f32::to_bits(2.5) as u64;
        });
        let mut file = [0u64; 96];
        file[31] = 0x7000;
        let mut regs = unsafe { Regs::from_raw(file.as_mut_ptr()) };
        assert_eq!(regs.call(0x1234, 0x5555, &[7, 8, 9]), Ok(24));
        assert_eq!((file[8], file[30], file[31]), (0xdead, 0x5555, 0x7000));
        let regs = unsafe { Regs::from_raw(file.as_mut_ptr()) };
        assert_eq!(regs.f32_result(), 2.5);
        assert_eq!(regs.sp(), 0x7000);
    }

    const V0_WORD: usize = 32;

    #[test]
    fn pending_exception_is_reported() {
        clear();
        install(0x99, |r| {
            r[0] = u64::MAX;
            r[1] = u64::MAX;
        });
        let mut file = [0u64; 96];
        let mut regs = unsafe { Regs::from_raw(file.as_mut_ptr()) };
        assert_eq!(regs.call(0x99, 1, &[]), Err(GuestException));
    }

    #[test]
    fn invoke_keeps_x30_and_the_selftest_thunk_goes_to_the_callee() {
        clear();
        install(0x7100049adc, |r| {
            assert_eq!(r[30], 0xabc);
            r[0] += 1;
        });
        let mut file = [0u64; 96];
        file[30] = 0xabc;
        file[0] = 41;
        unsafe { smrpg_bridge_selftest_thunk(file.as_mut_ptr()) };
        assert_eq!((file[0], file[30]), (42, 0xabc));
    }

    #[test]
    fn float_argument_zeroes_the_upper_lane() {
        let mut file = [0xffff_ffff_ffff_ffffu64; 96];
        let mut regs = unsafe { Regs::from_raw(file.as_mut_ptr()) };
        regs.set_f32(1, -0.0);
        assert_eq!((file[34], file[35]), (0x8000_0000, 0));
    }
}
