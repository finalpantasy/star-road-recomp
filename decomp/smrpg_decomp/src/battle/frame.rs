//! The register state of a lifted function that a readable port has to reproduce (task-57).
//!
//! The lifted C keeps a function's registers in C locals and writes them to the context (`c->x[n] = xn`) only at a
//! call, at a garbage-collection safepoint and at the return, and only the registers that changed since the last
//! write. A replay or A/B comparison looks at the whole context, so a port must write the same registers with the
//! same values at the same places. `Frame` holds the locals; the port assigns them where the compiled code does and
//! names what each value is, and `call` / `call_out` / `leave` write the lists the lifted code writes (read off the
//! lifted C by script, see docs/DECOMP.md "Readable ports").

use crate::bridge::{GuestException, Regs};

pub struct Frame {
    pub regs: Regs,
    /// The lifted locals x0 to x30 and sp (index 31).
    pub x: [u64; 32],
    /// The values at entry (a callee-saved register is restored to its entry value by the epilogue).
    pub entry: [u64; 32],
    /// The lifted vector locals as (low, high) words (task-58: the float ports keep scalars here, upper half zero).
    pub v: [(u64, u64); 32],
    /// The vector values at entry (v8 is callee-saved: the epilogue reloads its low half and zeroes the rest).
    pub v_entry: [(u64, u64); 32],
}

impl Frame {
    pub fn new(regs: Regs) -> Self {
        let mut x = [0u64; 32];
        for (n, slot) in x.iter_mut().enumerate() {
            *slot = regs.x(n);
        }
        let v = core::array::from_fn(|n| (regs.v_low(n), regs.v_high(n)));
        Self {
            regs,
            entry: x,
            x,
            v,
            v_entry: v,
        }
    }

    /// Write the listed locals to the context; `with_sp` when the call site also writes `c->x[31] = sp`.
    pub fn flush(&mut self, list: &[usize], with_sp: bool) {
        for &n in list {
            self.regs.set_x(n, self.x[n]);
        }
        if with_sp {
            self.regs.set_x(31, self.x[31]);
        }
    }

    /// Write the listed vector locals to the context.
    pub fn flush_v(&mut self, list: &[usize]) {
        for &n in list {
            self.regs.set_v(n, self.v[n].0, self.v[n].1);
        }
    }

    /// A float scalar written the way `fmov s<n>, w` or an `fp2` result does: the upper half is cleared.
    pub fn set_s(&mut self, n: usize, value: f32) {
        self.v[n] = (u64::from(value.to_bits()), 0);
    }

    pub fn s(&self, n: usize) -> f32 {
        f32::from_bits(self.v[n].0 as u32)
    }

    /// `x30 = return_address`, flush, call, then reload x0 and x1 (the only registers the lifted code reloads).
    pub fn call(
        &mut self,
        target: u64,
        return_address: u64,
        list: &[usize],
        with_sp: bool,
    ) -> Result<(), GuestException> {
        self.x[30] = return_address;
        self.flush(list, with_sp);
        self.regs.set_x(30, self.x[30]);
        let outcome = self.regs.invoke(target);
        self.reload();
        outcome.map(|_| ())
    }

    /// The same for a callee that writes `area` (see `Regs::invoke_out`).
    pub fn call_out(
        &mut self,
        target: u64,
        return_address: u64,
        list: &[usize],
        with_sp: bool,
        area: &mut [u8],
    ) -> Result<(), GuestException> {
        self.x[30] = return_address;
        self.flush(list, with_sp);
        let outcome = self.regs.invoke_out(target, area);
        self.reload();
        outcome.map(|_| ())
    }

    /// `call` for a call site that also flushes vector registers (`vlist`).
    pub fn call_v(
        &mut self,
        target: u64,
        return_address: u64,
        list: &[usize],
        vlist: &[usize],
        with_sp: bool,
    ) -> Result<(), GuestException> {
        self.x[30] = return_address;
        self.flush(list, with_sp);
        self.flush_v(vlist);
        let outcome = self.regs.invoke(target);
        self.reload();
        outcome.map(|_| ())
    }

    /// `call_out` for a call site that also flushes vector registers.
    pub fn call_out_v(
        &mut self,
        target: u64,
        return_address: u64,
        list: &[usize],
        vlist: &[usize],
        with_sp: bool,
        area: &mut [u8],
    ) -> Result<(), GuestException> {
        self.x[30] = return_address;
        self.flush(list, with_sp);
        self.flush_v(vlist);
        let outcome = self.regs.invoke_out(target, area);
        self.reload();
        outcome.map(|_| ())
    }

    fn reload(&mut self) {
        self.x[0] = self.regs.x(0);
        self.x[1] = self.regs.x(1);
        self.v[0] = (self.regs.v_low(0), self.regs.v_high(0));
        self.v[1] = (self.regs.v_low(1), self.regs.v_high(1));
    }

    /// The epilogue: x19 to x22 and x29 come back from the stack frame (their entry values), sp is the entry sp, and
    /// the lifted code writes `list` (which holds those registers and any other register it changed) and sp. x30 is
    /// popped but never written back to the context (a lifter artifact the port copies).
    pub fn leave(&mut self, list: &[usize]) {
        for n in [19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29] {
            self.x[n] = self.entry[n];
        }
        self.x[31] = self.entry[31];
        self.flush(list, true);
    }

    /// `leave` for a function that saves d8 (`str d8, [sp, #-0x70]!` ... `ldr d8, [sp], #0x70`): the epilogue reloads
    /// the low half of v8 and clears the upper half, and the lifted code writes v8 back.
    pub fn leave_v8(&mut self, list: &[usize]) {
        self.v[8] = (self.v_entry[8].0, 0);
        self.leave(list);
        self.flush_v(&[8]);
    }
}
