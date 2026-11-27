//! ARM64 scalar floating point with the exact results and FPSR flags of the lifted C helpers (task-53).
//!
//! A readable port of float code must leave the guest context bit for bit as the lifted body does, and the
//! lifted body gets its numbers from `runtime/ir_simd_a.h` (`sa_fp2`, `sa_fp3`, `sa_fcmp`, `sa_fcvt`, `sa_fcvti`,
//! `sa_cvtf`): host SSE/FMA for ordinary operands when FPCR is zero, an integer implementation of the ARM
//! pseudocode otherwise. This module is the integer implementation, ported line by line from the `sa_s_*`
//! functions (same structure, same names where they fit), for single and double precision. It never uses a host
//! float operation, so the host MXCSR cannot change a result and NaN payloads are never touched by the compiler.
//! The runtime selftest (`runtime/decomp/swap.c`, `fp_selftest`) compares it with the C helpers, fast paths
//! included, over edge and random operands in every FPCR mode; `cargo test` holds hand-computed cases.
//!
//! State: FPCR (read: FZ bit 24, DN bit 25, RMode bits 23:22) and FPSR (cumulative flags, OR-ed in). Same
//! limits as the C: no half precision, no AHP, AH, FIZ or NEP, no trapped exceptions.
//!
//! What the lifted C ignores, so this ignores it too: flush-to-zero of a result sets UFC only when the rounding
//! step sees a tiny result (that is the ARM rule, kept); `fcmp` of a quiet NaN with the quiet form does not set
//! IOC (ARM rule); half precision and the 16-bit FZ16 bit do not exist here.

pub const IOC: u32 = 0x01;
pub const DZC: u32 = 0x02;
pub const OFC: u32 = 0x04;
pub const UFC: u32 = 0x08;
pub const IXC: u32 = 0x10;
pub const IDC: u32 = 0x80;

/// Floating point format (`ftype` of the C helpers: 0 single, 1 double).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Fmt {
    Single,
    Double,
}

impl Fmt {
    const fn nbits(self) -> u32 {
        match self {
            Fmt::Single => 32,
            Fmt::Double => 64,
        }
    }
    const fn mbits(self) -> u32 {
        match self {
            Fmt::Single => 23,
            Fmt::Double => 52,
        }
    }
    const fn ebits(self) -> u32 {
        match self {
            Fmt::Single => 8,
            Fmt::Double => 11,
        }
    }
    const fn signbit(self) -> u64 {
        1u64 << (self.nbits() - 1)
    }
    const fn dnan(self) -> u64 {
        match self {
            Fmt::Single => 0x7FC0_0000,
            Fmt::Double => 0x7FF8_0000_0000_0000,
        }
    }
    const fn inf(self, sign: u32) -> u64 {
        ((sign as u64) << (self.nbits() - 1)) | (((1u64 << self.ebits()) - 1) << self.mbits())
    }
    const fn zero(self, sign: u32) -> u64 {
        (sign as u64) << (self.nbits() - 1)
    }
    const fn maxfin(self, sign: u32) -> u64 {
        ((sign as u64) << (self.nbits() - 1))
            | (((1u64 << self.ebits()) - 2) << self.mbits())
            | ((1u64 << self.mbits()) - 1)
    }
}

/// Operand class, ordered so that `>= QNan` means "is a NaN".
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
enum Cls {
    Num,
    Zero,
    Inf,
    QNan,
    SNan,
}

/// An unpacked operand: value = m * 2^e.
#[derive(Clone, Copy)]
struct Unpacked {
    cls: Cls,
    sign: u32,
    e: i32,
    m: u64,
}

/// Result of `fcmp`: the NZCV flags the instruction writes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Nzcv {
    pub n: bool,
    pub z: bool,
    pub c: bool,
    pub v: bool,
}

impl Nzcv {
    const fn from_bits(bits: u32) -> Self {
        Self {
            n: bits & 8 != 0,
            z: bits & 4 != 0,
            c: bits & 2 != 0,
            v: bits & 1 != 0,
        }
    }
}

/// FPCR and FPSR of the guest context. Operations read FPCR and OR cumulative flags into FPSR.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FpEnv {
    pub fpcr: u32,
    pub fpsr: u32,
}

fn msb128(m: u128) -> i32 {
    127 - m.leading_zeros() as i32
}

impl FpEnv {
    pub const fn new(fpcr: u32, fpsr: u32) -> Self {
        Self { fpcr, fpsr }
    }

    fn rmode(&self) -> u32 {
        (self.fpcr >> 22) & 3
    }
    fn fz(&self) -> bool {
        (self.fpcr >> 24) & 1 != 0
    }
    fn dn(&self) -> bool {
        (self.fpcr >> 25) & 1 != 0
    }

    // ---- unpack, round (FPRound), NaN rules ----

    fn unpack(&mut self, ft: Fmt, x: u64) -> Unpacked {
        let (mb, eb, nb) = (ft.mbits(), ft.ebits(), ft.nbits());
        let sign = ((x >> (nb - 1)) & 1) as u32;
        let fr = x & ((1u64 << mb) - 1);
        let ex = ((x >> mb) & ((1u64 << eb) - 1)) as u32;
        let mut u = Unpacked {
            cls: Cls::Zero,
            sign,
            e: 0,
            m: 0,
        };
        if ex == (1u32 << eb) - 1 {
            u.cls = if fr == 0 {
                Cls::Inf
            } else if (fr >> (mb - 1)) & 1 != 0 {
                Cls::QNan
            } else {
                Cls::SNan
            };
            return u;
        }
        let bias = (1i32 << (eb - 1)) - 1;
        if ex == 0 {
            if fr == 0 {
                return u;
            }
            if self.fz() {
                self.fpsr |= IDC;
                return u;
            }
            u.cls = Cls::Num;
            u.m = fr;
            u.e = 1 - bias - mb as i32;
            return u;
        }
        u.cls = Cls::Num;
        u.m = fr | (1u64 << mb);
        u.e = ex as i32 - bias - mb as i32;
        u
    }

    /// value = (m + eps) * 2^e with eps in (0,1) when `sticky`; m != 0. Uses RMode and FZ.
    fn round(&mut self, ft: Fmt, sign: u32, m: u128, e: i32, sticky: bool) -> u64 {
        let (mb, eb, nb) = (ft.mbits() as i32, ft.ebits(), ft.nbits());
        let bias = (1i32 << (eb - 1)) - 1;
        let rm = self.rmode();
        let sgn = (sign as u64) << (nb - 1);
        let msb = msb128(m);
        let big_e = e + msb;
        let emin = 1 - bias;
        let tiny = big_e < emin;
        if tiny && self.fz() {
            self.fpsr |= UFC;
            return sgn;
        }
        let l = if tiny { emin - mb } else { big_e - mb };
        let sh = l - e;
        let kept: u128;
        let mut g = false;
        let mut low = sticky;
        if sh <= 0 {
            kept = m.checked_shl((-sh) as u32).unwrap_or(0);
        } else if sh >= 129 {
            kept = 0;
            low |= m != 0;
        } else {
            kept = if sh == 128 { 0 } else { m >> sh };
            g = (m >> (sh - 1)) & 1 != 0;
            low |= (m & ((1u128 << (sh - 1)) - 1)) != 0;
        }
        let inexact = g | low;
        let up = match rm {
            0 => g && (low || (kept as u64) & 1 != 0),
            1 => sign == 0 && inexact,
            2 => sign != 0 && inexact,
            _ => false,
        };
        if tiny && inexact {
            self.fpsr |= UFC;
        }
        let k = (kept as u64).wrapping_add(u64::from(up));
        let mut ovf = false;
        let mut bits = 0u64;
        if !tiny {
            if big_e > bias {
                ovf = true;
            } else {
                bits = (((big_e + bias) as u64) << mb).wrapping_add(k.wrapping_sub(1u64 << mb));
                if bits >= (((1u64 << eb) - 1) << mb) {
                    ovf = true;
                }
            }
        } else {
            bits = k;
        }
        if ovf {
            self.fpsr |= OFC | IXC;
            let to_inf = rm == 0 || (rm == 1 && sign == 0) || (rm == 2 && sign != 0);
            return if to_inf {
                ft.inf(sign)
            } else {
                ft.maxfin(sign)
            };
        }
        if inexact {
            self.fpsr |= IXC;
        }
        sgn | bits
    }

    /// Propagated NaN: a signaling NaN raises IOC and is quieted; DN replaces any NaN with the default NaN.
    fn pnan(&mut self, ft: Fmt, mut x: u64, cls: Cls) -> u64 {
        if cls == Cls::SNan {
            self.fpsr |= IOC;
            x |= 1u64 << (ft.mbits() - 1);
        }
        if self.dn() {
            x = ft.dnan();
        }
        x
    }

    fn nan2(&mut self, ft: Fmt, a: &Unpacked, b: &Unpacked, ax: u64, bx: u64) -> Option<u64> {
        if a.cls == Cls::SNan {
            Some(self.pnan(ft, ax, Cls::SNan))
        } else if b.cls == Cls::SNan {
            Some(self.pnan(ft, bx, Cls::SNan))
        } else if a.cls == Cls::QNan {
            Some(self.pnan(ft, ax, Cls::QNan))
        } else if b.cls == Cls::QNan {
            Some(self.pnan(ft, bx, Cls::QNan))
        } else {
            None
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn nan3(
        &mut self,
        ft: Fmt,
        a: &Unpacked,
        b: &Unpacked,
        c: &Unpacked,
        ax: u64,
        bx: u64,
        cx: u64,
    ) -> Option<u64> {
        if a.cls == Cls::SNan {
            Some(self.pnan(ft, ax, Cls::SNan))
        } else if b.cls == Cls::SNan {
            Some(self.pnan(ft, bx, Cls::SNan))
        } else if c.cls == Cls::SNan {
            Some(self.pnan(ft, cx, Cls::SNan))
        } else if a.cls == Cls::QNan {
            Some(self.pnan(ft, ax, Cls::QNan))
        } else if b.cls == Cls::QNan {
            Some(self.pnan(ft, bx, Cls::QNan))
        } else if c.cls == Cls::QNan {
            Some(self.pnan(ft, cx, Cls::QNan))
        } else {
            None
        }
    }

    fn invalid(&mut self, ft: Fmt) -> u64 {
        self.fpsr |= IOC;
        ft.dnan()
    }

    /// Exact sum of two terms (value = m * 2^e), rounded once.
    #[allow(clippy::too_many_arguments)]
    fn sum(
        &mut self,
        ft: Fmt,
        mut sa: u32,
        mut ma: u128,
        mut ea: i32,
        mut sb: u32,
        mut mb: u128,
        mut eb: i32,
    ) -> u64 {
        if ma == 0 && mb == 0 {
            let sign = if sa == sb {
                sa
            } else {
                u32::from(self.rmode() == 2)
            };
            return ft.zero(sign);
        }
        if ma == 0 {
            return self.round(ft, sb, mb, eb, false);
        }
        if mb == 0 {
            return self.round(ft, sa, ma, ea, false);
        }
        let mut la = msb128(ma);
        let mut lb = msb128(mb);
        if eb + lb > ea + la {
            core::mem::swap(&mut ma, &mut mb);
            core::mem::swap(&mut ea, &mut eb);
            core::mem::swap(&mut sa, &mut sb);
            core::mem::swap(&mut la, &mut lb);
        }
        let up = 125 - la;
        let a = ma << up;
        let e0 = ea - up;
        let d = eb - e0;
        let b: u128;
        let mut st = false;
        if d >= 0 {
            b = mb << d;
        } else {
            let r = -d;
            if r >= 128 {
                b = 0;
                st = true;
            } else {
                b = mb >> r;
                st = (mb & ((1u128 << r) - 1)) != 0;
            }
        }
        let m: u128;
        let mut sign = sa;
        if sa == sb {
            m = a + b;
        } else if a >= b {
            m = a - b - u128::from(st);
        } else {
            m = b - a;
            sign = sb;
        }
        if m == 0 && !st {
            return ft.zero(u32::from(self.rmode() == 2));
        }
        self.round(ft, sign, m, e0, st)
    }

    // ---- arithmetic on raw bits ----

    pub fn addsub_bits(&mut self, ft: Fmt, a: u64, b: u64, sub: bool) -> u64 {
        let ua = self.unpack(ft, a);
        let mut ub = self.unpack(ft, b);
        if let Some(r) = self.nan2(ft, &ua, &ub, a, b) {
            return r;
        }
        if sub {
            ub.sign ^= 1;
        }
        let ia = ua.cls == Cls::Inf;
        let ib = ub.cls == Cls::Inf;
        if ia && ib && ua.sign != ub.sign {
            return self.invalid(ft);
        }
        if ia {
            return ft.inf(ua.sign);
        }
        if ib {
            return ft.inf(ub.sign);
        }
        self.sum(ft, ua.sign, ua.m as u128, ua.e, ub.sign, ub.m as u128, ub.e)
    }

    pub fn mul_bits(&mut self, ft: Fmt, a: u64, b: u64) -> u64 {
        let ua = self.unpack(ft, a);
        let ub = self.unpack(ft, b);
        if let Some(r) = self.nan2(ft, &ua, &ub, a, b) {
            return r;
        }
        let (ia, ib) = (ua.cls == Cls::Inf, ub.cls == Cls::Inf);
        let (za, zb) = (ua.cls == Cls::Zero, ub.cls == Cls::Zero);
        let sg = ua.sign ^ ub.sign;
        if (ia && zb) || (za && ib) {
            return self.invalid(ft);
        }
        if ia || ib {
            return ft.inf(sg);
        }
        if za || zb {
            return ft.zero(sg);
        }
        self.round(
            ft,
            sg,
            u128::from(ua.m) * u128::from(ub.m),
            ua.e + ub.e,
            false,
        )
    }

    pub fn div_bits(&mut self, ft: Fmt, a: u64, b: u64) -> u64 {
        let ua = self.unpack(ft, a);
        let ub = self.unpack(ft, b);
        if let Some(r) = self.nan2(ft, &ua, &ub, a, b) {
            return r;
        }
        let (ia, ib) = (ua.cls == Cls::Inf, ub.cls == Cls::Inf);
        let (za, zb) = (ua.cls == Cls::Zero, ub.cls == Cls::Zero);
        let sg = ua.sign ^ ub.sign;
        if (ia && ib) || (za && zb) {
            return self.invalid(ft);
        }
        if ia || zb {
            if !ia {
                self.fpsr |= DZC;
            }
            return ft.inf(sg);
        }
        if za || ib {
            return ft.zero(sg);
        }
        let la = 63 - ua.m.leading_zeros() as i32;
        let lb = 63 - ub.m.leading_zeros() as i32;
        let am = ua.m << (52 - la);
        let bm = ub.m << (52 - lb);
        let ea = ua.e - (52 - la);
        let eb = ub.e - (52 - lb);
        let n = u128::from(am) << 62;
        let q = n / u128::from(bm);
        let st = (n - q * u128::from(bm)) != 0;
        self.round(ft, sg, q, ea - 62 - eb, st)
    }

    /// FPMulAdd: `a + x*y`, one rounding.
    pub fn fma_bits(&mut self, ft: Fmt, a: u64, x: u64, y: u64) -> u64 {
        let ua = self.unpack(ft, a);
        let ux = self.unpack(ft, x);
        let uy = self.unpack(ft, y);
        let (ix, iy) = (ux.cls == Cls::Inf, uy.cls == Cls::Inf);
        let (zx, zy) = (ux.cls == Cls::Zero, uy.cls == Cls::Zero);
        if ua.cls == Cls::QNan && ((ix && zy) || (zx && iy)) {
            return self.invalid(ft);
        }
        if let Some(r) = self.nan3(ft, &ua, &ux, &uy, a, x, y) {
            return r;
        }
        let ia = ua.cls == Cls::Inf;
        let ip = ix || iy;
        let za = ua.cls == Cls::Zero;
        let zp = zx || zy;
        let sp = ux.sign ^ uy.sign;
        if (ix && zy) || (zx && iy) || (ia && ip && ua.sign != sp) {
            return self.invalid(ft);
        }
        if (ia && ua.sign == 0) || (ip && sp == 0) {
            return ft.inf(0);
        }
        if (ia && ua.sign != 0) || (ip && sp != 0) {
            return ft.inf(1);
        }
        if za && zp && ua.sign == sp {
            return ft.zero(ua.sign);
        }
        let pm = if zp {
            0
        } else {
            u128::from(ux.m) * u128::from(uy.m)
        };
        self.sum(ft, ua.sign, ua.m as u128, ua.e, sp, pm, ux.e + uy.e)
    }

    // ---- compare, min and max ----

    fn sgn(u: &Unpacked) -> i32 {
        if u.cls == Cls::Zero {
            0
        } else if u.sign != 0 {
            -1
        } else {
            1
        }
    }

    fn cmpv(a: &Unpacked, b: &Unpacked) -> i32 {
        let (sa, sb) = (Self::sgn(a), Self::sgn(b));
        if sa != sb {
            return if sa < sb { -1 } else { 1 };
        }
        if sa == 0 {
            return 0;
        }
        let mag = if a.cls == Cls::Inf || b.cls == Cls::Inf {
            i32::from(a.cls == Cls::Inf) - i32::from(b.cls == Cls::Inf)
        } else {
            let la = 63 - a.m.leading_zeros() as i32;
            let lb = 63 - b.m.leading_zeros() as i32;
            let ta = a.e + la;
            let tb = b.e + lb;
            if ta != tb {
                if ta < tb { -1 } else { 1 }
            } else {
                let ma = a.m << (63 - la);
                let mb = b.m << (63 - lb);
                if ma == mb {
                    0
                } else if ma < mb {
                    -1
                } else {
                    1
                }
            }
        };
        if sa < 0 { -mag } else { mag }
    }

    /// `fcmp` (`signaling` false) or `fcmpe` (true).
    pub fn fcmp_bits(&mut self, ft: Fmt, a: u64, b: u64, signaling: bool) -> Nzcv {
        let ua = self.unpack(ft, a);
        let ub = self.unpack(ft, b);
        if ua.cls >= Cls::QNan || ub.cls >= Cls::QNan {
            if signaling || ua.cls == Cls::SNan || ub.cls == Cls::SNan {
                self.fpsr |= IOC;
            }
            return Nzcv::from_bits(3);
        }
        let k = Self::cmpv(&ua, &ub);
        Nzcv::from_bits(if k == 0 {
            6
        } else if k < 0 {
            8
        } else {
            2
        })
    }

    /// fmax, fmin (`is_num` false) and fmaxnm, fminnm (`is_num` true).
    pub fn minmax_bits(
        &mut self,
        ft: Fmt,
        mut a: u64,
        mut b: u64,
        is_min: bool,
        is_num: bool,
    ) -> u64 {
        let mut ua = self.unpack(ft, a);
        let mut ub = self.unpack(ft, b);
        if is_num {
            let an = ua.cls >= Cls::QNan;
            let bn = ub.cls >= Cls::QNan;
            if !(an && bn) {
                if ua.cls == Cls::QNan {
                    a = ft.inf(u32::from(!is_min));
                    ua = self.unpack(ft, a);
                } else if ub.cls == Cls::QNan {
                    b = ft.inf(u32::from(!is_min));
                    ub = self.unpack(ft, b);
                }
            }
        }
        if let Some(r) = self.nan2(ft, &ua, &ub, a, b) {
            return r;
        }
        let k = Self::cmpv(&ua, &ub);
        let pick_first = if is_min { k < 0 } else { k > 0 };
        let (s, sx) = if pick_first { (&ua, a) } else { (&ub, b) };
        if s.cls == Cls::Zero {
            return ft.zero(if is_min {
                ua.sign | ub.sign
            } else {
                ua.sign & ub.sign
            });
        }
        sx
    }

    // ---- conversions ----

    /// fcvt between single and double.
    pub fn cvt_bits(&mut self, sft: Fmt, dft: Fmt, a: u64) -> u64 {
        let ua = self.unpack(sft, a);
        if ua.cls >= Cls::QNan {
            if ua.cls == Cls::SNan {
                self.fpsr |= IOC;
            }
            if self.dn() {
                return dft.dnan();
            }
            let (smb, dmb) = (sft.mbits(), dft.mbits());
            let mut fr = a & ((1u64 << smb) - 1);
            fr = if dmb > smb {
                fr << (dmb - smb)
            } else {
                fr >> (smb - dmb)
            };
            fr |= 1u64 << (dmb - 1);
            return dft.inf(ua.sign) | fr;
        }
        if ua.cls == Cls::Inf {
            return dft.inf(ua.sign);
        }
        if ua.cls == Cls::Zero {
            return dft.zero(ua.sign);
        }
        self.round(dft, ua.sign, u128::from(ua.m), ua.e, false)
    }

    /// Float to integer (`fcvtz*`, `fcvtn*`, `fcvtp*`, `fcvtm*`, `fcvta*`): `sf` 64-bit result, `uns` unsigned,
    /// `fbits` fixed-point fraction bits, `rm` 0 nearest even, 1 up, 2 down, 3 zero, 4 away.
    pub fn tofix_bits(&mut self, ft: Fmt, x: u64, sf: bool, uns: bool, fbits: i32, rm: u32) -> u64 {
        let u = self.unpack(ft, x);
        let nb: u32 = if sf { 64 } else { 32 };
        let mask: u64 = if sf { !0 } else { 0xFFFF_FFFF };
        if u.cls == Cls::QNan || u.cls == Cls::SNan {
            self.fpsr |= IOC;
            return 0;
        }
        if u.cls == Cls::Zero {
            return 0;
        }
        let mut ip: u128 = 0;
        let mut g = false;
        let mut low = false;
        let mut ovf = false;
        if u.cls == Cls::Inf {
            ovf = true;
        } else {
            let e = u.e + fbits;
            if e >= 0 {
                if e > 70 {
                    ovf = true;
                } else {
                    ip = u128::from(u.m) << e;
                }
            } else {
                let sh = -e;
                if sh >= 64 {
                    low = true;
                } else {
                    ip = u128::from(u.m >> sh);
                    g = (u.m >> (sh - 1)) & 1 != 0;
                    low = (u.m & ((1u64 << (sh - 1)) - 1)) != 0;
                }
            }
        }
        let inex = g | low;
        if !ovf {
            let up = match rm {
                0 => g && (low || (ip as u64) & 1 != 0),
                1 => u.sign == 0 && inex,
                2 => u.sign != 0 && inex,
                4 => g,
                _ => false,
            };
            ip += u128::from(up);
            if uns {
                if if u.sign != 0 {
                    ip != 0
                } else {
                    ip > u128::from(mask)
                } {
                    ovf = true;
                }
            } else {
                let lim = 1u128 << (nb - 1);
                if if u.sign != 0 { ip > lim } else { ip > lim - 1 } {
                    ovf = true;
                }
            }
        }
        if ovf {
            self.fpsr |= IOC;
            if uns {
                return if u.sign != 0 { 0 } else { mask };
            }
            return if u.sign != 0 {
                1u64 << (nb - 1)
            } else {
                (1u64 << (nb - 1)) - 1
            };
        }
        if inex {
            self.fpsr |= IXC;
        }
        let mut r = ip as u64;
        if u.sign != 0 {
            r = 0u64.wrapping_sub(r);
        }
        r & mask
    }

    /// Integer (or fixed point) to float (`scvtf`, `ucvtf`).
    pub fn fromfix_bits(&mut self, ft: Fmt, v: u64, sf: bool, uns: bool, fbits: i32) -> u64 {
        let mut sign = 0u32;
        let mut mag = if sf { v } else { u64::from(v as u32) };
        if !uns {
            let negative = if sf { (v as i64) < 0 } else { (v as i32) < 0 };
            if negative {
                sign = 1;
                mag = if sf {
                    0u64.wrapping_sub(v)
                } else {
                    u64::from(0u32.wrapping_sub(v as u32))
                };
            }
        }
        if mag == 0 {
            return 0;
        }
        self.round(ft, sign, u128::from(mag), -fbits, false)
    }

    // ---- typed wrappers (single and double) ----

    pub fn fadd32(&mut self, a: f32, b: f32) -> f32 {
        f32::from_bits(self.addsub_bits(
            Fmt::Single,
            u64::from(a.to_bits()),
            u64::from(b.to_bits()),
            false,
        ) as u32)
    }
    pub fn fsub32(&mut self, a: f32, b: f32) -> f32 {
        f32::from_bits(self.addsub_bits(
            Fmt::Single,
            u64::from(a.to_bits()),
            u64::from(b.to_bits()),
            true,
        ) as u32)
    }
    pub fn fmul32(&mut self, a: f32, b: f32) -> f32 {
        f32::from_bits(
            self.mul_bits(Fmt::Single, u64::from(a.to_bits()), u64::from(b.to_bits())) as u32,
        )
    }
    pub fn fdiv32(&mut self, a: f32, b: f32) -> f32 {
        f32::from_bits(
            self.div_bits(Fmt::Single, u64::from(a.to_bits()), u64::from(b.to_bits())) as u32,
        )
    }
    /// `fmadd d, n, m, a`: a + n*m.
    pub fn fmadd32(&mut self, n: f32, m: f32, a: f32) -> f32 {
        self.fp3_32(0, n, m, a)
    }
    /// `fmsub d, n, m, a`: a - n*m.
    pub fn fmsub32(&mut self, n: f32, m: f32, a: f32) -> f32 {
        self.fp3_32(1, n, m, a)
    }
    /// `fnmadd d, n, m, a`: -a - n*m.
    pub fn fnmadd32(&mut self, n: f32, m: f32, a: f32) -> f32 {
        self.fp3_32(2, n, m, a)
    }
    /// `fnmsub d, n, m, a`: -a + n*m.
    pub fn fnmsub32(&mut self, n: f32, m: f32, a: f32) -> f32 {
        self.fp3_32(3, n, m, a)
    }
    fn fp3_32(&mut self, kind: u32, n: f32, m: f32, a: f32) -> f32 {
        f32::from_bits(self.fp3_bits(
            Fmt::Single,
            kind,
            u64::from(n.to_bits()),
            u64::from(m.to_bits()),
            u64::from(a.to_bits()),
        ) as u32)
    }
    pub fn fmax32(&mut self, a: f32, b: f32) -> f32 {
        f32::from_bits(self.minmax_bits(
            Fmt::Single,
            u64::from(a.to_bits()),
            u64::from(b.to_bits()),
            false,
            false,
        ) as u32)
    }
    pub fn fmin32(&mut self, a: f32, b: f32) -> f32 {
        f32::from_bits(self.minmax_bits(
            Fmt::Single,
            u64::from(a.to_bits()),
            u64::from(b.to_bits()),
            true,
            false,
        ) as u32)
    }
    pub fn fcmp32(&mut self, a: f32, b: f32) -> Nzcv {
        self.fcmp_bits(
            Fmt::Single,
            u64::from(a.to_bits()),
            u64::from(b.to_bits()),
            false,
        )
    }
    pub fn fcvt_s_to_d(&mut self, a: f32) -> f64 {
        f64::from_bits(self.cvt_bits(Fmt::Single, Fmt::Double, u64::from(a.to_bits())))
    }
    pub fn fcvt_d_to_s(&mut self, a: f64) -> f32 {
        f32::from_bits(self.cvt_bits(Fmt::Double, Fmt::Single, a.to_bits()) as u32)
    }

    pub fn fadd64(&mut self, a: f64, b: f64) -> f64 {
        f64::from_bits(self.addsub_bits(Fmt::Double, a.to_bits(), b.to_bits(), false))
    }
    pub fn fsub64(&mut self, a: f64, b: f64) -> f64 {
        f64::from_bits(self.addsub_bits(Fmt::Double, a.to_bits(), b.to_bits(), true))
    }
    pub fn fmul64(&mut self, a: f64, b: f64) -> f64 {
        f64::from_bits(self.mul_bits(Fmt::Double, a.to_bits(), b.to_bits()))
    }
    pub fn fdiv64(&mut self, a: f64, b: f64) -> f64 {
        f64::from_bits(self.div_bits(Fmt::Double, a.to_bits(), b.to_bits()))
    }
    pub fn fcmp64(&mut self, a: f64, b: f64) -> Nzcv {
        self.fcmp_bits(Fmt::Double, a.to_bits(), b.to_bits(), false)
    }
    pub fn fmax64(&mut self, a: f64, b: f64) -> f64 {
        f64::from_bits(self.minmax_bits(Fmt::Double, a.to_bits(), b.to_bits(), false, false))
    }
    pub fn fmin64(&mut self, a: f64, b: f64) -> f64 {
        f64::from_bits(self.minmax_bits(Fmt::Double, a.to_bits(), b.to_bits(), true, false))
    }

    /// Three-operand fused ops by kind (0 fmadd, 1 fmsub, 2 fnmadd, 3 fnmsub): the sign flips of `sa_fp3`.
    pub fn fp3_bits(&mut self, ft: Fmt, kind: u32, n: u64, m: u64, a: u64) -> u64 {
        let sb = ft.signbit();
        let mut x = n;
        let mut acc = a;
        if kind == 1 || kind == 2 {
            x ^= sb;
        }
        if kind >= 2 {
            acc ^= sb;
        }
        self.fma_bits(ft, acc, x, m)
    }
}

/// Single-precision arithmetic as a readable port uses it. The guest implementation reads FPCR and FPSR from the
/// context each time (a callee may have changed the flags) and writes the flags back; tests use a plain `FpEnv`.
pub trait Float32Ops {
    fn fadd(&mut self, a: f32, b: f32) -> f32;
    fn fsub(&mut self, a: f32, b: f32) -> f32;
    fn fmul(&mut self, a: f32, b: f32) -> f32;
}

/// Single-precision conversions and compares a port needs beyond `Float32Ops` (task-54): `scvtf s, w`, `fcvtzs w, s`
/// and `fcmp`. Like `Float32Ops`, the guest implementation reads and writes the context's FPSR on every call.
pub trait Float32Convert: Float32Ops {
    /// `scvtf`: 32-bit signed integer to single.
    fn float_from_i32(&mut self, value: i32) -> f32;
    /// `fcvtzs`: single to 32-bit signed integer, rounding toward zero, saturating (NaN gives 0).
    fn trunc_to_i32(&mut self, value: f32) -> i32;
    /// `fcmp`: the NZCV flags.
    fn compare(&mut self, a: f32, b: f32) -> Nzcv;
}

impl Float32Convert for FpEnv {
    fn float_from_i32(&mut self, value: i32) -> f32 {
        f32::from_bits(
            self.fromfix_bits(Fmt::Single, u64::from(value as u32), false, false, 0) as u32,
        )
    }
    fn trunc_to_i32(&mut self, value: f32) -> i32 {
        self.tofix_bits(Fmt::Single, u64::from(value.to_bits()), false, false, 0, 3) as u32 as i32
    }
    fn compare(&mut self, a: f32, b: f32) -> Nzcv {
        self.fcmp32(a, b)
    }
}

impl Float32Ops for FpEnv {
    fn fadd(&mut self, a: f32, b: f32) -> f32 {
        self.fadd32(a, b)
    }
    fn fsub(&mut self, a: f32, b: f32) -> f32 {
        self.fsub32(a, b)
    }
    fn fmul(&mut self, a: f32, b: f32) -> f32 {
        self.fmul32(a, b)
    }
}

// ---- entry point for the runtime selftest (`fp_selftest` in swap.c compares these with the C helpers) ----

/// `op`: 0 fmul, 1 fdiv, 2 fadd, 3 fsub, 4 fmax, 5 fmin, 6 fmaxnm, 7 fminnm (the `kind` of `sa_fp2`), 16 to 19
/// fmadd, fmsub, fnmadd, fnmsub (kind + 16; the operands are n = a, m = b, addend = c), 32 fcmp, 33 fcmpe (the
/// result is the NZCV nibble), 40 fcvt single to double, 41 fcvt double to single, 48 to 63 fcvtzs/fcvtzu/
/// fcvt{n,p,m,a} (see `swap.c`), 64 to 67 scvtf/ucvtf. `ft` 0 single, 1 double. `fpsr` is in and out.
#[unsafe(no_mangle)]
pub extern "C" fn smrpg_fp_selftest_op(
    op: u32,
    ft: u32,
    fpcr: u32,
    fpsr: *mut u32,
    a: u64,
    b: u64,
    c: u64,
    extra: u32,
) -> u64 {
    let fmt = if ft == 0 { Fmt::Single } else { Fmt::Double };
    let mut env = FpEnv::new(fpcr, unsafe { fpsr.read() });
    let result = match op {
        0 => env.mul_bits(fmt, a, b),
        1 => env.div_bits(fmt, a, b),
        2 => env.addsub_bits(fmt, a, b, false),
        3 => env.addsub_bits(fmt, a, b, true),
        4..=7 => env.minmax_bits(fmt, a, b, op & 1 != 0, op >= 6),
        16..=19 => env.fp3_bits(fmt, op - 16, a, b, c),
        32 | 33 => {
            let f = env.fcmp_bits(fmt, a, b, op == 33);
            u64::from(f.n) << 3 | u64::from(f.z) << 2 | u64::from(f.c) << 1 | u64::from(f.v)
        }
        40 => env.cvt_bits(Fmt::Single, Fmt::Double, a),
        41 => env.cvt_bits(Fmt::Double, Fmt::Single, a),
        // extra: bit 0 sf, bit 1 unsigned, bits 2..4 rm, bits 8.. fbits
        48 => env.tofix_bits(
            fmt,
            a,
            extra & 1 != 0,
            extra & 2 != 0,
            (extra >> 8) as i32,
            (extra >> 2) & 7,
        ),
        64 => env.fromfix_bits(fmt, a, extra & 1 != 0, extra & 2 != 0, (extra >> 8) as i32),
        _ => 0,
    };
    unsafe { fpsr.write(env.fpsr) };
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    const ONE: u32 = 0x3F80_0000;
    const TWO: u32 = 0x4000_0000;
    const QNAN: u32 = 0x7FC0_0001;
    const SNAN: u32 = 0x7F80_0001;
    const INF: u32 = 0x7F80_0000;
    const DENORM: u32 = 0x0000_0001;
    const MIN_NORMAL: u32 = 0x0080_0000;
    const MAX: u32 = 0x7F7F_FFFF;
    const FZ: u32 = 1 << 24;
    const DN: u32 = 1 << 25;
    const RM_UP: u32 = 1 << 22;
    const RM_DOWN: u32 = 2 << 22;
    const RM_ZERO: u32 = 3 << 22;

    fn run2(op: &str, fpcr: u32, a: u32, b: u32) -> (u32, u32) {
        let mut env = FpEnv::new(fpcr, 0);
        let (a, b) = (u64::from(a), u64::from(b));
        let r = match op {
            "add" => env.addsub_bits(Fmt::Single, a, b, false),
            "sub" => env.addsub_bits(Fmt::Single, a, b, true),
            "mul" => env.mul_bits(Fmt::Single, a, b),
            "div" => env.div_bits(Fmt::Single, a, b),
            _ => unreachable!(),
        };
        (r as u32, env.fpsr)
    }

    #[test]
    fn exact_results_set_no_flags() {
        assert_eq!(run2("add", 0, ONE, ONE), (TWO, 0));
        assert_eq!(run2("mul", 0, TWO, TWO), (0x4080_0000, 0));
        assert_eq!(run2("div", 0, ONE, TWO), (0x3F00_0000, 0));
        assert_eq!(run2("sub", 0, ONE, ONE), (0, 0));
    }

    #[test]
    fn inexact_sets_ixc() {
        // 1/3 rounds
        assert_eq!(run2("div", 0, ONE, 0x4040_0000), (0x3EAA_AAAB, IXC));
        // 1 + 2^-24 is a tie, rounds to even (1.0)
        assert_eq!(run2("add", 0, ONE, 0x3380_0000), (ONE, IXC));
    }

    #[test]
    fn rounding_modes() {
        let tiny = 0x3380_0000; // 2^-24: 1 + tiny is half way between 1.0 and 1.0 + 2^-23
        assert_eq!(run2("add", RM_UP, ONE, tiny), (0x3F80_0001, IXC));
        assert_eq!(run2("add", RM_DOWN, ONE, tiny), (ONE, IXC));
        assert_eq!(run2("add", RM_ZERO, ONE, tiny), (ONE, IXC));
        // -1 - tiny rounded toward minus infinity grows in magnitude
        assert_eq!(run2("sub", RM_DOWN, 0xBF80_0000, tiny), (0xBF80_0001, IXC));
    }

    #[test]
    fn overflow_and_infinity_rules() {
        assert_eq!(run2("add", 0, MAX, MAX), (INF, OFC | IXC));
        // toward zero returns the largest finite number instead
        assert_eq!(run2("add", RM_ZERO, MAX, MAX), (MAX, OFC | IXC));
        assert_eq!(run2("mul", 0, MAX, TWO), (INF, OFC | IXC));
        assert_eq!(run2("sub", 0, INF, INF), (0x7FC0_0000, IOC));
        assert_eq!(run2("mul", 0, INF, 0), (0x7FC0_0000, IOC));
        assert_eq!(run2("div", 0, 0, 0), (0x7FC0_0000, IOC));
    }

    #[test]
    fn divide_by_zero() {
        assert_eq!(run2("div", 0, ONE, 0), (INF, DZC));
        assert_eq!(run2("div", 0, ONE, 0x8000_0000), (0xFF80_0000, DZC));
        // infinity divided by zero is exact, no flag
        assert_eq!(run2("div", 0, INF, 0), (INF, 0));
    }

    #[test]
    fn nan_propagation() {
        // a signaling NaN wins over a quiet one, is quieted, raises IOC
        assert_eq!(run2("add", 0, QNAN, SNAN), (0x7FC0_0001, IOC));
        assert_eq!(run2("add", 0, SNAN, ONE), (0x7FC0_0001, IOC));
        // quiet NaN: first operand wins, no flag
        assert_eq!(run2("mul", 0, QNAN, 0x7FC0_0002), (QNAN, 0));
        assert_eq!(run2("mul", 0, ONE, 0x7FC0_0002), (0x7FC0_0002, 0));
        // default NaN mode drops the payload, still raises IOC for a signaling NaN
        assert_eq!(run2("add", DN, QNAN, ONE), (0x7FC0_0000, 0));
        assert_eq!(run2("add", DN, SNAN, ONE), (0x7FC0_0000, IOC));
    }

    #[test]
    fn denormals_and_flush_to_zero() {
        // exact denormal sum is not tiny-inexact: no flags
        assert_eq!(run2("add", 0, DENORM, DENORM), (2, 0));
        // an input denormal flushes to zero under FZ and sets IDC
        assert_eq!(run2("add", FZ, DENORM, ONE), (ONE, IDC));
        assert_eq!(run2("mul", FZ, DENORM, TWO), (0, IDC));
        // a tiny result flushes to zero under FZ and sets UFC (no IXC)
        assert_eq!(run2("mul", FZ, MIN_NORMAL, 0x3F00_0000), (0, UFC));
        // without FZ the same product is an exact denormal
        assert_eq!(run2("mul", 0, MIN_NORMAL, 0x3F00_0000), (0x0040_0000, 0));
        // an inexact tiny result sets UFC and IXC
        assert_eq!(
            run2("mul", 0, MIN_NORMAL | 1, 0x3F00_0000),
            (0x0040_0000, UFC | IXC)
        );
    }

    #[test]
    fn signed_zero() {
        assert_eq!(run2("add", 0, 0x8000_0000, 0x8000_0000), (0x8000_0000, 0));
        assert_eq!(run2("add", 0, 0x8000_0000, 0), (0, 0));
        assert_eq!(run2("add", RM_DOWN, 0x8000_0000, 0), (0x8000_0000, 0));
        assert_eq!(run2("sub", 0, ONE, ONE), (0, 0));
        assert_eq!(run2("sub", RM_DOWN, ONE, ONE), (0x8000_0000, 0));
        assert_eq!(run2("mul", 0, 0x8000_0000, ONE), (0x8000_0000, 0));
        assert_eq!(run2("mul", 0, 0x8000_0000, 0xBF80_0000), (0, 0));
    }

    #[test]
    fn flags_accumulate_and_are_never_cleared() {
        let mut env = FpEnv::new(0, DZC);
        assert_eq!(env.fadd32(1.0, 2.0), 3.0);
        assert_eq!(env.fpsr, DZC);
        env.fdiv32(1.0, 3.0);
        assert_eq!(env.fpsr, DZC | IXC);
    }

    #[test]
    fn fused_multiply_add_rounds_once() {
        let mut env = FpEnv::new(0, 0);
        // (1 + 2^-12)^2 = 1 + 2^-11 + 2^-24: the 2^-24 survives only when the add is fused
        let x = f32::from_bits(0x3F80_0800);
        let fused = env.fmadd32(x, x, -1.0);
        let split = {
            let mut e = FpEnv::new(0, 0);
            let p = e.fmul32(x, x);
            e.fadd32(p, -1.0)
        };
        assert_eq!(fused.to_bits(), 0x3A00_0400);
        assert_eq!(split.to_bits(), 0x3A00_0000);
        // fmsub: a - n*m, fnmadd: -a - n*m, fnmsub: -a + n*m
        let mut env = FpEnv::new(0, 0);
        assert_eq!(env.fmsub32(2.0, 3.0, 10.0), 4.0);
        assert_eq!(env.fnmadd32(2.0, 3.0, 10.0), -16.0);
        assert_eq!(env.fnmsub32(2.0, 3.0, 10.0), -4.0);
        assert_eq!(env.fpsr, 0);
        // infinity times zero plus a quiet NaN is invalid, not the NaN
        let mut env = FpEnv::new(0, 0);
        assert_eq!(
            env.fma_bits(Fmt::Single, u64::from(QNAN), u64::from(INF), 0),
            0x7FC0_0000
        );
        assert_eq!(env.fpsr, IOC);
    }

    fn cmp(a: u32, b: u32) -> (u32, u32) {
        let mut env = FpEnv::new(0, 0);
        let f = env.fcmp_bits(Fmt::Single, u64::from(a), u64::from(b), false);
        (
            u32::from(f.n) << 3 | u32::from(f.z) << 2 | u32::from(f.c) << 1 | u32::from(f.v),
            env.fpsr,
        )
    }

    #[test]
    fn compare_flags() {
        assert_eq!(cmp(ONE, TWO), (0b1000, 0)); // less: N
        assert_eq!(cmp(TWO, ONE), (0b0010, 0)); // greater: C
        assert_eq!(cmp(ONE, ONE), (0b0110, 0)); // equal: Z C
        assert_eq!(cmp(0x8000_0000, 0), (0b0110, 0)); // -0 == +0
        assert_eq!(cmp(QNAN, ONE), (0b0011, 0)); // unordered: C V, quiet NaN raises nothing in fcmp
        assert_eq!(cmp(SNAN, ONE), (0b0011, IOC));
        let mut env = FpEnv::new(0, 0);
        env.fcmp_bits(Fmt::Single, u64::from(QNAN), u64::from(ONE), true);
        assert_eq!(env.fpsr, IOC); // fcmpe raises it
        assert_eq!(cmp(0xFF80_0000, INF), (0b1000, 0));
    }

    fn minmax(a: u32, b: u32, min: bool, num: bool) -> (u32, u32) {
        let mut env = FpEnv::new(0, 0);
        let r = env.minmax_bits(Fmt::Single, u64::from(a), u64::from(b), min, num);
        (r as u32, env.fpsr)
    }

    #[test]
    fn min_max() {
        assert_eq!(minmax(ONE, TWO, false, false), (TWO, 0));
        assert_eq!(minmax(ONE, TWO, true, false), (ONE, 0));
        // signed zeros: max is +0, min is -0
        assert_eq!(minmax(0x8000_0000, 0, false, false), (0, 0));
        assert_eq!(minmax(0x8000_0000, 0, true, false), (0x8000_0000, 0));
        // fmax with a quiet NaN returns it; fmaxnm returns the number
        assert_eq!(minmax(QNAN, ONE, false, false), (QNAN, 0));
        assert_eq!(minmax(QNAN, ONE, false, true), (ONE, 0));
        assert_eq!(minmax(ONE, QNAN, true, true), (ONE, 0));
        // a signaling NaN is still an error for the nm forms
        assert_eq!(minmax(SNAN, ONE, false, true), (0x7FC0_0001, IOC));
    }

    #[test]
    fn conversions() {
        let mut env = FpEnv::new(0, 0);
        assert_eq!(env.fcvt_s_to_d(1.5), 1.5);
        assert_eq!(env.fpsr, 0);
        assert_eq!(env.fcvt_d_to_s(0.1).to_bits(), 0x3DCC_CCCD);
        assert_eq!(env.fpsr, IXC);
        // double too large for single: overflow
        let mut env = FpEnv::new(0, 0);
        assert_eq!(env.fcvt_d_to_s(1e300), f32::INFINITY);
        assert_eq!(env.fpsr, OFC | IXC);
        // a signaling NaN is quieted with its payload kept, and raises IOC
        let mut env = FpEnv::new(0, 0);
        let r = env.cvt_bits(Fmt::Single, Fmt::Double, u64::from(SNAN));
        assert_eq!(r, 0x7FF8_0000_2000_0000);
        assert_eq!(env.fpsr, IOC);
        // float to int: truncation, saturation, NaN
        let mut env = FpEnv::new(0, 0);
        assert_eq!(
            env.tofix_bits(
                Fmt::Single,
                u64::from((-2.7f32).to_bits()),
                false,
                false,
                0,
                3
            ),
            0xFFFF_FFFE
        );
        assert_eq!(env.fpsr, IXC);
        let mut env = FpEnv::new(0, 0);
        assert_eq!(
            env.tofix_bits(
                Fmt::Single,
                u64::from(1e20f32.to_bits()),
                false,
                false,
                0,
                3
            ),
            0x7FFF_FFFF
        );
        assert_eq!(env.fpsr, IOC);
        let mut env = FpEnv::new(0, 0);
        assert_eq!(
            env.tofix_bits(Fmt::Single, u64::from(QNAN), true, false, 0, 3),
            0
        );
        assert_eq!(env.fpsr, IOC);
        // int to float: 2^24 + 1 is inexact in single
        let mut env = FpEnv::new(0, 0);
        assert_eq!(
            env.fromfix_bits(Fmt::Single, 16_777_217, false, false, 0),
            0x4B80_0000
        );
        assert_eq!(env.fpsr, IXC);
    }

    #[test]
    fn double_precision() {
        let mut env = FpEnv::new(0, 0);
        assert_eq!(env.fadd64(0.1, 0.2).to_bits(), 0x3FD3_3333_3333_3334);
        assert_eq!(env.fpsr, IXC);
        let mut env = FpEnv::new(0, 0);
        assert_eq!(env.fmul64(1e200, 1e200), f64::INFINITY);
        assert_eq!(env.fpsr, OFC | IXC);
        let mut env = FpEnv::new(0, 0);
        assert_eq!(env.fdiv64(1.0, 3.0).to_bits(), 0x3FD5_5555_5555_5555);
        assert_eq!(env.fpsr, IXC);
    }
}
