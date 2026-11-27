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

#[cfg(not(test))]
unsafe extern "C" {
    /// `runtime/decomp/swap.c` (task-55): the same safepoint where the lifted code flushes several registers.
    fn smrpg_guest_safepoint_n(
        x: *mut u64,
        pc: u64,
        count: u32,
        regs: *const u32,
        values: *const u64,
    );
    /// Pre-state hook only: remember the cell's value before the lifted body runs (`watch` = the port stores to it).
    fn smrpg_guest_snapshot(addr: u64, size: u32, watch: u32);
    /// The value the cell had before the lifted body ran when a replay trial is running, otherwise its live value.
    fn smrpg_guest_pre_read(addr: u64, size: u32) -> u64;
    /// Store `size` bytes; in a replay trial it is only journaled and checked against memory afterwards.
    fn smrpg_guest_store(addr: u64, size: u32, value: u64);
}

#[cfg(not(test))]
unsafe extern "C" {
    /// `runtime/decomp/fp_selftest.c`: in `ab` mode, runs the same float operation on the lifted C helper and logs a
    /// mismatch of result or FPSR (task-54); otherwise returns at once.
    fn smrpg_fp_shadow(
        op: u32,
        fpcr: u32,
        fpsr_before: u32,
        a: u64,
        b: u64,
        extra: u32,
        result: u64,
        fpsr_after: u32,
    );
}

#[cfg(test)]
unsafe fn smrpg_fp_shadow(_: u32, _: u32, _: u32, _: u64, _: u64, _: u32, _: u64, _: u32) {}

#[cfg(test)]
pub(crate) use tests::fake_guest_bl as smrpg_guest_bl;
#[cfg(test)]
pub(crate) use tests::fake_safepoint as smrpg_guest_safepoint;
#[cfg(test)]
pub(crate) use tests::fake_safepoint_n as smrpg_guest_safepoint_n;
#[cfg(test)]
pub(crate) use tests::fake_store as smrpg_guest_store;
#[cfg(test)]
pub(crate) use tests::{
    fake_pre_read as smrpg_guest_pre_read, fake_snapshot as smrpg_guest_snapshot,
};

/// Pre-state of a replay row (task-55, `swap.c`): the value of a guest cell before the lifted body of the row ran,
/// while a replay trial runs; the live value otherwise. A port reads a cell the lifted body or its callees may have
/// changed (a lazy-initialization flag, a class-initialized word) through these.
pub mod prestate {
    use super::*;

    /// Pre-state hook: remember a cell. `watch` marks one the port stores to itself (see `store`).
    pub fn snapshot(addr: u64, size: u32, watch: bool) {
        unsafe { smrpg_guest_snapshot(addr, size, u32::from(watch)) }
    }

    pub fn read_u8(addr: u64) -> u8 {
        unsafe { smrpg_guest_pre_read(addr, 1) as u8 }
    }

    pub fn read_u32(addr: u64) -> u32 {
        unsafe { smrpg_guest_pre_read(addr, 4) as u32 }
    }

    /// A one-byte store the port makes (a lazy-initialization flag): done for real outside a replay trial, checked
    /// against the memory the lifted body left inside one.
    pub fn store_u8(addr: u64, value: u8) {
        unsafe { smrpg_guest_store(addr, 1, u64::from(value)) }
    }

    /// A four-byte store (a float or int field), journaled the same way.
    pub fn store_u32(addr: u64, value: u32) {
        unsafe { smrpg_guest_store(addr, 4, u64::from(value)) }
    }
}

use crate::fp::{Float32Convert, FpEnv};

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

    /// One single-precision float operation on the guest's FPCR and FPSR: `run` computes it with the Rust float
    /// layer, the flags go back to the context, and in `ab` mode the lifted C helper runs the same operation
    /// (`op`, see `smrpg_fp_shadow`) and must agree on result and flags (task-54). Returns `result`.
    pub fn fp_op(
        &mut self,
        op: u32,
        a: u64,
        b: u64,
        extra: u32,
        run: impl FnOnce(&mut FpEnv) -> u64,
    ) -> u64 {
        let mut env = self.fp_env();
        let before = env.fpsr;
        let result = run(&mut env);
        self.set_fpsr(env.fpsr);
        unsafe { smrpg_fp_shadow(op, env.fpcr, before, a, b, extra, result, env.fpsr) };
        result
    }

    /// `fmul` (op 0), `fadd` (2) or `fsub` (3) on single-precision values, through `fp_op`.
    pub fn f32_arith(&mut self, op: u32, a: f32, b: f32) -> f32 {
        let (x, y) = (u64::from(a.to_bits()), u64::from(b.to_bits()));
        f32::from_bits(self.fp_op(op, x, y, 0, |env| {
            u64::from(
                match op {
                    0 => env.fmul32(a, b),
                    2 => env.fadd32(a, b),
                    _ => env.fsub32(a, b),
                }
                .to_bits(),
            )
        }) as u32)
    }

    /// `scvtf s, w`.
    pub fn f32_from_i32(&mut self, value: i32) -> f32 {
        let bits = u64::from(value as u32);
        f32::from_bits(self.fp_op(64, bits, 0, 0, |env| {
            u64::from(env.float_from_i32(value).to_bits())
        }) as u32)
    }

    /// `fcvtzs w, s`: toward zero, 32-bit signed (the `extra` of the shadow check: rounding mode 3 in bits 2 to 4).
    pub fn i32_from_f32_trunc(&mut self, value: f32) -> i32 {
        let bits = u64::from(value.to_bits());
        self.fp_op(48, bits, 0, 3 << 2, |env| {
            u64::from(env.trunc_to_i32(value) as u32)
        }) as u32 as i32
    }

    /// `fcmp s, s`: the NZCV flags.
    pub fn f32_compare(&mut self, a: f32, b: f32) -> crate::fp::Nzcv {
        let (x, y) = (u64::from(a.to_bits()), u64::from(b.to_bits()));
        let mut flags = None;
        self.fp_op(32, x, y, 0, |env| {
            let f = env.compare(a, b);
            flags = Some(f);
            u64::from(f.n) << 3 | u64::from(f.z) << 2 | u64::from(f.c) << 1 | u64::from(f.v)
        });
        flags.expect("fcmp ran")
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

    /// The lifted safepoint where the code flushes several registers when a collection is pending.
    pub fn safepoint_n(&mut self, pc: u64, flushed: &[(u32, u64)]) {
        let regs: [u32; 4] = core::array::from_fn(|i| flushed.get(i).map_or(0, |f| f.0));
        let values: [u64; 4] = core::array::from_fn(|i| flushed.get(i).map_or(0, |f| f.1));
        assert!(flushed.len() <= 4);
        unsafe {
            smrpg_guest_safepoint_n(
                self.x,
                pc,
                flushed.len() as u32,
                regs.as_ptr(),
                values.as_ptr(),
            )
        }
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

    std::thread_local! {
        pub static STORES: RefCell<Vec<(u64, u32, u64)>> = const { RefCell::new(Vec::new()) };
        /// Pre-state a test installs: (address, size, value) answered by `fake_pre_read` before live memory.
        pub static PRE: RefCell<Vec<(u64, u32, u64)>> = const { RefCell::new(Vec::new()) };
    }

    pub unsafe fn fake_safepoint_n(
        _x: *mut u64,
        pc: u64,
        _count: u32,
        _regs: *const u32,
        _values: *const u64,
    ) {
        SAFEPOINTS.with(|s| s.borrow_mut().push(pc));
    }

    pub unsafe fn fake_snapshot(_addr: u64, _size: u32, _watch: u32) {}

    pub unsafe fn fake_pre_read(addr: u64, size: u32) -> u64 {
        if let Some(v) = PRE.with(|p| {
            p.borrow()
                .iter()
                .find(|e| e.0 == addr && e.1 == size)
                .map(|e| e.2)
        }) {
            return v;
        }
        unsafe {
            match size {
                1 => u64::from((addr as *const u8).read()),
                4 => u64::from((addr as *const u32).read()),
                _ => (addr as *const u64).read(),
            }
        }
    }

    pub unsafe fn fake_store(addr: u64, size: u32, value: u64) {
        // Recorded only: the addresses tests use for lazy-initialization flags are game globals.
        STORES.with(|s| s.borrow_mut().push((addr, size, value)));
    }

    pub fn install(target: u64, body: Fake) {
        CALLEES.with(|c| c.borrow_mut().push((target, body)));
    }

    pub fn clear() {
        CALLEES.with(|c| c.borrow_mut().clear());
        CALLS.with(|c| c.borrow_mut().clear());
        SAFEPOINTS.with(|c| c.borrow_mut().clear());
        STORES.with(|c| c.borrow_mut().clear());
        PRE.with(|c| c.borrow_mut().clear());
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
