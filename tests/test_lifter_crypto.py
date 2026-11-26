"""Execute C emitted by the lifter on the published crypto and CRC vectors."""
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools" / "lifter"))
import gen
import lift
import lower


def vector_adapter(vector_path):
    vectors = vector_path.read_text()
    words = {int(w, 16) for w in re.findall(r"(0x[0-9a-f]{8})u\b", vectors)}
    words.update((0x0E22E020, 0x4E22E020, 0x4EE2E020))
    lines = ['#define main run_published_vectors', vectors, '#undef main',
             '#include "mode_lift.h"', '#include "ops_gen.h"',
             'int a64_crypto_exec(v128 *v, uint32_t w) {',
             '    switch (w) {']
    for word in sorted(words):
        name, fields = lower.decode_any(word, 0)
        reads, _ = lift.op_regs(name, fields)
        lines.append(f'    case 0x{word:08x}: {{')
        registers = {fields[field] for field, kind, _, reg_type in lift.OPS.OPS[name]["fields"]
                     if kind == "reg" and reg_type == "v"}
        for reg in sorted(registers):
            value = f' = v[{reg}]' if reads & lift.bit_v(reg) else ''
            lines.append(f'        v128 v{reg}{value};')
        rd = fields["rd"]
        lines += [f'        {lift.emit_op(name, fields)}', f'        v[{rd}] = v{rd};',
                  '        return 1;', '    }']
    lines += ['    default: return 0;', '    }', '}']
    return "\n".join(lines)


def crc_tests():
    lines = ['static void test_crc(void) {', '    uint64_t x0, x1, x2;']
    wants = (0xcbf43926, 0xe3069283)
    data = b"123456789"
    for polynomial, expected in enumerate(wants):
        for width in range(4):
            lines.append('    x1 = UINT64_MAX;')
            chunk = 1 << width
            for i in range(0, 8, chunk):
                word = 0x1AC24020 | (polynomial << 12) | (width << 10) | ((width == 3) << 31)
                name, fields = lower.decode_any(word, 0)
                value = int.from_bytes(data[i:i + chunk], "little")
                if width != 3:
                    value |= (0xFFFFFFFFFFFFFFFF << (chunk * 8)) & 0xFFFFFFFFFFFFFFFF
                lines.append(f'    x2 = UINT64_C(0x{value:x}); {lift.emit_op(name, fields)} x1 = x0;')
            name, fields = lower.decode_any(0x1AC24020 | (polynomial << 12), 0)
            lines.append(f'    x2 = 0x39; {lift.emit_op(name, fields)}')
            lines.append(f'    CHECK((x0 ^ 0xffffffffu) == 0x{expected:08x}u, "CRC variant {polynomial}/{chunk}");')
    return "\n".join(lines + ['}'])


MAIN = """
int main(void) {
    run_published_vectors();
    test_crc();
    v128 v[32] = {0};
    uint8_t a[16] = {3, 0x80, 0, 0, 0, 0, 0, 0, 7, 9};
    uint8_t b[16] = {3, 0x80, 0, 0, 0, 0, 0, 0, 3, 7};
    const uint16_t low[8] = {5, 0x4000};
    const uint16_t high[8] = {9, 63};
    memcpy(&v[1], a, sizeof a);
    memcpy(&v[2], b, sizeof b);
    run(v, 0x0e22e020u);
    CHECK(!memcmp(&v[0], low, sizeof low), "pmull .8b");
    run(v, 0x4e22e020u);
    CHECK(!memcmp(&v[0], high, sizeof high), "pmull2 .16b");
    const uint64_t n[2] = {1, UINT64_C(0x8000000000000000)};
    const uint64_t product[2] = {0, UINT64_C(0x4000000000000000)};
    memcpy(&v[1], n, sizeof n);
    memcpy(&v[2], n, sizeof n);
    run(v, 0x4ee2e020u);
    CHECK(!memcmp(&v[0], product, sizeof product), "pmull2 .2d");
    if (!fails) puts("PASS emitted AES, SHA-1, SHA-256, PMULL and CRC vectors");
    return fails != 0;
}
"""


def main():
    clang = shutil.which("clang")
    if clang is None:
        raise SystemExit("clang not on PATH")
    with tempfile.TemporaryDirectory(prefix="lifter-crypto-") as tmp:
        work = Path(tmp)
        gen.write_all(work)
        src = work / "vectors.c"
        exe = work / "vectors.exe"
        adapter = vector_adapter(ROOT / "tests" / "runtime" / "test_crypto_a64.c")
        src.write_text(adapter + "\n" + crc_tests() + MAIN, encoding="ascii")
        subprocess.run([clang, "-std=c11", "-O2", "-I", str(ROOT / "runtime"), str(src), "-o", str(exe)],
                       check=True, timeout=45)
        subprocess.run([str(exe)], check=True, timeout=15)


if __name__ == "__main__":
    main()
