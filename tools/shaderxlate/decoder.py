"""Maxwell (SM 5.x) instruction decoder: opcode matching and field helpers.

Encodings are hardware facts; this table is written for this project in its own format:
(leading opcode bits, name, operand form). Forms: R register b operand, I 20-bit immediate,
C constant buffer b operand, RC register b with constant c operand (c and b swapped), I32 32-bit
immediate, '-' no b operand form.
Code is a list of u64 words; every word at index % 4 == 0 is a scheduling control word.
"""

_TABLE_SRC = """
1110111110100 al2p -
1110111111011 ald -
1110111111110 ast -
11101101 atom -
111011101111 atomcas -
11101100 atoms -
111011100 atomscas -
1111000010111 b2r -
1111000010101 bar -
0101110000000 bfe R
0011100x00000 bfe I
0100110000000 bfe C
0101101111110 bfi R
0011011x11110 bfi I
0100101111110 bfi C
0101001111110 bfi RC
111000111010 bpt -
111000100100 bra -
111000110100 brk -
111000100101 brx -
111000100110 cal -
11101111011 cctl -
1110111110000 cctll -
111000110101 cont -
0101000010011 cset -
0101000010100 csetp -
0101000011001 cs2r -
0101110001110 dadd R
0011100x01110 dadd I
0100110001110 dadd C
010110110111 dfma R
0011011x0111 dfma I
010010110111 dfma C
010100110111 dfma RC
0101110001010 dmnmx R
0011100x01010 dmnmx I
0100110001010 dmnmx C
0101110010000 dmul R
0011100x10000 dmul I
0100110010000 dmul C
010110010 dset R
0011001x0 dset I
010010010 dset C
010110111000 dsetp R
0011011x1000 dsetp I
010010111000 dsetp C
1111000011110 depbar -
111000110000 exit -
0101110010101 f2f R
0011100x10101 f2f I
0100110010101 f2f C
0101110010110 f2i R
0011100x10110 f2i I
0100110010110 f2i C
0101110001011 fadd R
0011100x01011 fadd I
0100110001011 fadd C
000010 fadd32i I32
0101110010001 fchk R
0011100x10001 fchk I
0100110010001 fchk C
010110111010 fcmp R
0011011x1010 fcmp I
010010111010 fcmp C
010100111010 fcmp RC
010110011 ffma R
0011001x1 ffma I
010010011 ffma C
010100011 ffma RC
000011 ffma32i I32
0101110000110 flo R
0011100x00110 flo I
0100110000110 flo C
0101110001100 fmnmx R
0011100x01100 fmnmx I
0100110001100 fmnmx C
0101110001101 fmul R
0011100x01101 fmul I
0100110001101 fmul C
00011110 fmul32i I32
01011000 fset R
0011000 fset I
01001000 fset C
010110111011 fsetp R
0011011x1011 fsetp I
010010111011 fsetp C
0101000011111 fswzadd -
111000101100 getcrsptr -
111000101101 getlmembase -
0101110100010 hadd2 R
0111101x0 hadd2 I
0111101x1 hadd2 C
0010110 hadd2_32i I32
0101110100000 hfma2 R
01110xxx0 hfma2 I
01110xxx1 hfma2 C
01100xxx1 hfma2 RC
0010100 hfma2_32i I32
0101110100001 hmul2 R
0111100x0 hmul2 I
0111100x1 hmul2 C
0010101 hmul2_32i I32
0101110100011 hset2 R
0111110x0 hset2 I
0111110x1 hset2 C
0101110100100 hsetp2 R
0111111x0 hsetp2 I
0111111x1 hsetp2 C
0101110010111 i2f R
0011100x10111 i2f I
0100110010111 i2f C
0101110011100 i2i R
0011100x11100 i2i I
0100110011100 i2i C
0101110000010 iadd R
0011100x00010 iadd I
0100110000010 iadd C
0001110 iadd32i I32
010111001100 iadd3 R
0011100x1100 iadd3 I
010011001100 iadd3 C
010110110100 icmp R
0011011x0100 icmp I
010010110100 icmp C
010100110100 icmp RC
111000111001 ide -
0101001111111 idp R
0101001111011 idp I
010110100 imad R
0011010x0 imad I
010010100 imad C
010100100 imad RC
000100 imad32i I32
010110101 imadsp R
0011010x1 imadsp I
010010101 imadsp C
010100101 imadsp RC
0101110000100 imnmx R
0011100x00100 imnmx I
0100110000100 imnmx C
0101110000111 imul R
0011100x00111 imul I
0100110000111 imul C
00011111 imul32i I32
11100000 ipa -
1110111111010 isberd -
0101110000011 iscadd R
0011100x00011 iscadd I
0100110000011 iscadd C
000101 iscadd32i I32
010110110101 iset R
0011011x0101 iset I
010010110101 iset C
010110110110 isetp R
0011011x0110 isetp I
010010110110 isetp C
111000100010 jcal -
111000100001 jmp -
111000100000 jmx -
111000110011 kil -
100 ld -
1110111110010 ldc -
1110111011010 ldg -
1110111101000 ldl -
1110111101001 lds -
0101101111010 lea R
0011011x11010 lea I
0100101111010 lea C
0101101111011 lea_hi R
000110 lea_hi C
0101000011010 lepc -
111000110001 longjmp -
0101110001000 lop R
0011100x01000 lop I
0100110001000 lop C
0101101111100 lop3 R
001111 lop3 I
0000001 lop3 C
000001 lop32i I32
1110111110011 membar -
0101110010011 mov R
0011100x10011 mov I
0100110010011 mov C
000000010000 mov32i I32
0101000010000 mufu -
0101000010110 nop -
1111101111100 out R
1111011x11100 out I
1110101111100 out C
0101110011101 p2r R
0011100x11101 p2r I
0100110011101 p2r C
111000101010 pbk -
111000101011 pcnt -
111000100011 pexit -
1110111111101 pixld -
111000101000 plongjmp -
0101110000001 popc R
0011100x00001 popc I
0100110000001 popc C
111000100111 pret -
010110111100 prmt R
0011011x1100 prmt I
010010111100 prmt C
010100111100 prmt RC
0101000010001 pset -
0101000010010 psetp -
1111000011000 r2b -
0101110011110 r2p R
0011100x11110 r2p I
0100110011110 r2p C
111000111000 ram -
1110101111111 red -
111000110010 ret -
0101110010010 rro R
0011100x10010 rro I
0100110010010 rro C
111000110110 rtt -
1111000011001 s2r -
111000110111 sam -
0101110010100 sel R
0011100x10100 sel I
0100110010100 sel C
111000101110 setcrsptr -
111000101111 setlmembase -
0101101111111 shf_l R
0101110011111 shf_r R
0011011x11111 shf_l I
0011100x11111 shf_r I
1110111100010 shfl -
0101110001001 shl R
0011100x01001 shl I
0100110001001 shl C
0101110000101 shr R
0011100x00101 shr I
0100110000101 shr C
111000101001 ssy -
101 st -
1110111011011 stg -
1110111101010 stl -
1110111010100 stp -
1110111101011 sts -
1110101001110 suatom_b -
11101010x0 suatom -
1110101110 suatom_b2 -
1110101011010 suatomcas_b -
1110101x1 suatomcas -
1110101100010 suld_d_b -
1110101100011 suld_d -
1110101100000 suld_b -
1110101100001 suld -
1110101101010 sured_b -
1110101101011 sured -
1110101100110 sust_d_b -
1110101100111 sust_d -
1110101100100 sust_b -
1110101100101 sust -
1111000011111 sync -
11000 tex -
1101111010 tex_b -
1101100 texs -
1101000 texs_f16 -
11011100 tld -
11011101 tld_b -
1101101 tlds -
1101001 tlds_f16 -
110010 tld4 -
1101111011 tld4_b -
1101111100 tld4s -
1101111110 tld4s_f16 -
1101111101011 tmml -
1101111101100 tmml_b -
1101111101000 txa -
110111100 txd -
1101111001 txd_b -
1101111101001 txq -
1101111101010 txq_b -
01010100 vabsdiff -
010100000 vabsdiff4 -
001000 vadd -
01011111 vmad -
0011101 vmnmx -
0101000011011 vote -
0101000011100 votevtg -
0100000 vset -
0101000011110 vsetp -
01010111 vshl -
01010110 vshr -
0101101100 xmad R
0011011x00 xmad I
0100111 xmad C
010100010 xmad RC
"""


def _build():
    rows = []
    for line in _TABLE_SRC.strip().splitlines():
        bits, name, form = line.split()
        mask = val = 0
        for i, ch in enumerate(bits):
            shift = 63 - i
            if ch != 'x':
                mask |= 1 << shift
                if ch == '1':
                    val |= 1 << shift
        rows.append((bin(mask).count('1'), mask, val, name, form))
    # Most specific pattern first so prefixes such as "ld" (100) never shadow longer ones.
    rows.sort(key=lambda r: -r[0])
    return rows


ROWS = _build()
_LUT = {}
_TOP = 0xFFFF << 48


def match(word):
    """Return (name, form) for an instruction word, or (None, None)."""
    top = word >> 48
    cand = _LUT.get(top)
    if cand is None:
        w = top << 48
        cand = _LUT[top] = [(m, v, n, f) for (_, m, v, n, f) in ROWS if (w & m & _TOP) == (v & _TOP)]
    for m, v, n, f in cand:
        if word & m == v:
            return n, f
    return None, None


def bits(w, lo, n):
    return (w >> lo) & ((1 << n) - 1)


def sext(v, n):
    return v - (1 << n) if v & (1 << (n - 1)) else v


class Inst:
    """One decoded instruction. Field helpers follow the common Maxwell layout."""
    __slots__ = ('addr', 'w', 'name', 'form', 'sched')

    def __init__(self, addr, w, name, form, sched=0):
        self.addr = addr      # byte address relative to code start (after SPH)
        self.w = w
        self.name = name
        self.form = form
        self.sched = sched

    def f(self, lo, n):
        return (self.w >> lo) & ((1 << n) - 1)

    def b(self, lo):
        return (self.w >> lo) & 1

    @property
    def rd(self):
        return self.w & 0xFF

    @property
    def ra(self):
        return (self.w >> 8) & 0xFF

    @property
    def rb(self):
        return (self.w >> 20) & 0xFF

    @property
    def rc(self):
        return (self.w >> 39) & 0xFF

    @property
    def pred(self):
        return (self.w >> 16) & 7

    @property
    def pred_neg(self):
        return (self.w >> 19) & 1

    # constant buffer operand of C / RC forms
    @property
    def cb_slot(self):
        return (self.w >> 34) & 0x1F

    @property
    def cb_off(self):
        return ((self.w >> 20) & 0x3FFF) << 2

    @property
    def imm20i(self):
        """Signed 20-bit integer immediate (bits 20..38, sign at 56)."""
        v = (self.w >> 20) & 0x7FFFF
        if self.w >> 56 & 1:
            v |= 0xFFF80000
        return v & 0xFFFFFFFF

    @property
    def imm20f(self):
        """Float immediate: 19 bits at 20 plus sign at 56 form the top 20 bits of an f32."""
        v = ((self.w >> 20) & 0x7FFFF) | (((self.w >> 56) & 1) << 19)
        return (v << 12) & 0xFFFFFFFF

    @property
    def imm32(self):
        return (self.w >> 20) & 0xFFFFFFFF

    def __repr__(self):
        return '%05x %-8s %016x' % (self.addr, self.name, self.w)


def decode_code(code, base=0):
    """Decode a whole code section (bytes). Returns a list of Inst, skipping sched words.
    Unknown words decode with name None."""
    import struct
    n = len(code) // 8
    words = struct.unpack_from('<%dQ' % n, code, 0)
    out = []
    sched = 0
    for i in range(n):
        w = words[i]
        if i % 4 == 0:
            sched = w
            continue
        name, form = match(w)
        out.append(Inst(base + i * 8, w, name, form, (sched >> (21 * (i % 4 - 1))) & 0x1FFFFF))
    return out
