"""Prove every entry of pure.def from the lifted C (task-52).

  python -I runtime/decomp/check_pure.py [--work A:/AI/Recomps/smrpg-work]

A function the guest-call bridge may run twice on live memory (`ab` mode) must not change memory that anyone else
sees. For each address in pure.def this checks, on the lifted chunk:
  1. every store is a stack store (base register 31): a pre-index push with a negative immediate (the frame
     allocation) or a store at a non-negative offset inside the frame the function itself allocated (the sum of its
     pushes and `sub sp, sp, #imm`). Any other base register, post-index or NEON structure store fails;
  2. no indirect call or jump (rt_icall, rt_dispatch), no system call, no exception landing (eh_*), no exclusive,
     acquire or release instruction, and no operation outside the known load, arithmetic, floating point and SIMD
     arithmetic set;
  3. every direct call (`f_<addr>(c)`, tail calls included) goes to another pure.def entry or to a ported load row
     (LEAF, GLEAF, NLEAF, DLEAF, VLEAF that is not a store).
Exit 0 only if every entry passes. The garbage-collector poll (gc_poll) is allowed: it can suspend the thread but
writes no guest memory.
"""
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.environ.get("SMRPG_WORK", r"A:/AI/Recomps/smrpg-work")
if "--work" in sys.argv:
    WORK = sys.argv[sys.argv.index("--work") + 1]

SAFE_OPS = {
    "ld", "ldp", "ldv", "ldpv", "ld_wb", "ldp_wb", "ldv_wb", "ldpv_wb", "log_reg", "log_imm", "addsub_imm",
    "addsub_reg", "addsub_ext", "addsubs_imm", "addsubs_reg", "addsubs_ext", "movi", "movk", "csel", "bfm", "dp1",
    "dp2", "dp3", "fp1", "fp2", "fcmp", "fcsel", "fmov_fx", "fmov_tx", "fmovi", "fcvti", "cvtf", "cvtf_v", "f3same",
    "felem", "fccmp", "fpair", "ccmp_imm", "ccmp_reg", "extr", "ands_imm", "ands_reg", "adc", "nop", "crc32",
    "dup_s", "addp_s", "nv_3fp", "nv_mov", "nv_elemf", "nv_logic", "nv_dupe", "nv_inse", "nv_ext", "nv_movi",
    "nv_2fp", "nv_3int", "nv_dupg", "nv_2misc", "nv_perm", "nv_umov", "nv_shll", "nv_insg", "nv_tbl", "nv_bici",
    "nv_3diff", "nv_across", "nv_shn", "nv_fixcvt", "nv_elem", "nv_shift", "nv_ldl", "nv_ldr", "nv_ldm",
}
STACK_STORES = {  # operation: (index of the base register, index of the immediate, has size pair, writeback)
    "st": (2, 3, 1, False), "stv": (2, 3, 1, False), "stp": (3, 4, 2, False), "stpv": (3, 4, 2, False),
    "st_wb": (3, 4, 1, True), "stv_wb": (3, 4, 1, True), "stp_wb": (4, 5, 2, True), "stpv_wb": (4, 5, 2, True),
}
FORBIDDEN_CALLS = ("rt_icall", "rt_dispatch", "rt_svc", "rt_brk", "rt_fatal", "eh_land", "eh_no_site")


def parse_def(path, kind):
    rows = []
    for line in open(path, encoding="utf8"):
        m = re.match(rf"{kind}\(([0-9a-f]+)\)", line)
        if m:
            rows.append(int(m.group(1), 16))
    return rows


def load_rows():
    """Addresses of ported rows that only read memory."""
    loads = set()
    sources = [os.path.join(HERE, "leaves.def"), os.path.join(WORK, "decompgen", "gen.def")]
    for path in sources:
        if not os.path.exists(path):
            continue
        for line in open(path, encoding="utf8"):
            m = re.match(r"([SCGNVDPE]?LEAF)\((\w+), ([0-9a-f]+)((?:, \w+)*)\)", line)
            if not m:
                continue
            kind, address, rest = m.group(1), int(m.group(3), 16), [x.strip() for x in m.group(4).split(",")[1:]]
            if kind in ("LEAF", "GLEAF", "NLEAF", "DLEAF"):
                loads.add(address)
            elif kind == "VLEAF" and rest[1] != "2":   # rest = rust, vkind, ...
                loads.add(address)
    return loads


def find_functions(wanted):
    found = {}
    pattern = re.compile(r"^void f_([0-9a-f]+)\(Ctx \*restrict c\) \{$", re.M)
    for path in sorted(glob.glob(os.path.join(WORK, "build", "c", "main_*.c"))):
        text = open(path, encoding="utf8").read()
        if not any(f"f_{a:x}(Ctx *restrict c) {{" in text for a in wanted):
            continue
        for m in pattern.finditer(text):
            address = int(m.group(1), 16)
            if address in wanted:
                end = text.find("\n}\n", m.end())
                found[address] = (os.path.basename(path), text[m.end():end])
    return found


def check_body(body, allowed_calls):
    problems = []
    for name in FORBIDDEN_CALLS:
        if re.search(rf"\b{name}\b", body):
            problems.append(f"uses {name}")
    ops = re.findall(r"^\s*(?:\{ \w+ _t = \w+; \w+ = [^;]*;)?\s*OP_(\w+)\(([^)]*)\)", body, re.M)
    frame = 0
    stores = []
    for name, raw in ops:
        args = [a.strip() for a in raw.split(",")]
        if name in STACK_STORES:
            stores.append((name, args))
        elif name not in SAFE_OPS:
            problems.append(f"operation OP_{name} is not in the safe set")
    for m in re.finditer(r"OP_addsub_imm\(1, 1, 31, 31, (0x[0-9a-f]+|\d+)\)", body):   # sub sp, sp, #imm
        frame += int(m.group(1), 0)
    for name, args in stores:
        base_index, imm_index, pair, writeback = STACK_STORES[name]
        if args[base_index] != "31":
            problems.append(f"OP_{name} stores through x{args[base_index]}, not sp")
            continue
        imm = int(args[imm_index].rstrip("ll"), 0)
        if writeback:
            if args[1] != "2" or imm >= 0:
                problems.append(f"OP_{name} is not a pre-index push")
            else:
                frame += -imm
    for name, args in stores:
        base_index, imm_index, pair, writeback = STACK_STORES[name]
        if writeback or args[base_index] != "31":
            continue
        imm = int(args[imm_index].rstrip("ll"), 0)
        width = int(args[0]) * pair
        if imm < 0 or imm + width > frame:
            problems.append(f"OP_{name} at sp+{imm} width {width} is outside the {frame}-byte frame")
    for target in re.findall(r"\bf_([0-9a-f]+)\(c\)", body):
        if int(target, 16) not in allowed_calls:
            problems.append(f"calls 0x{target}, which is neither a pure.def entry nor a ported load row")
    return sorted(set(problems))


def main():
    entries = parse_def(os.path.join(HERE, "pure.def"), "PURE_FN")
    if not entries:
        print("pure.def has no entries")
        return 1
    allowed = set(entries) | load_rows()
    found = find_functions(set(entries))
    failed = 0
    for address in entries:
        if address not in found:
            print(f"0x{address:x} FAIL: no lifted function found")
            failed += 1
            continue
        chunk, body = found[address]
        problems = check_body(body, allowed)
        print(f"0x{address:x} {'FAIL ' + '; '.join(problems) if problems else 'PASS'} ({chunk})")
        failed += bool(problems)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
