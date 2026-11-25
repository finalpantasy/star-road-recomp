"""Research helper (task-24): read the game's compiled field scripts (romfs script/*.bin) read-only.

Layout, from the lifted ScriptGroupClass / ScriptObjectClass code:
  file: "BIN\\0", group count (12), then 16 bytes per group at 8 + 16*g = [int start, int end, float bytes, object count].
  group ints: ints[0..n) = base index of each script object; the list ends with -1.
  object at base b: ints[b+0..b+3] = start index of the Entry, Init, Execute and Terminate stream.
  stream: records [cmd, (false_offset << 8) | nargs, args x (nargs - 1)], end at a negative word.
cmd 3 is the flag read (args: type, index, expected), cmd 4 the flag write (type, index, value); other
ids are numbered by the order of ScriptCommandClass.cmd_* in the IL2CPP dump (calibrated, see the doc).
Output of this tool stays on A:, never in Git.
"""
import struct, sys, glob, os

SCRIPT_DIR = r"A:/AI/Recomps/smrpg-work/v101/romfs/Data/StreamingAssets/script/"


def groups(path):
    b = open(path, "rb").read()
    if b[:4] != b"BIN\0":
        return
    n = struct.unpack_from("<i", b, 4)[0]
    for g in range(n):
        s, e, fb, cnt = struct.unpack_from("<4i", b, 8 + 16 * g)
        if not (s and e > s):
            continue
        yield g, list(struct.unpack_from("<%di" % ((e - s) // 4), b, s)), cnt


def parse_stream(ints, idx):
    recs = []
    while 0 <= idx < len(ints) and ints[idx] >= 0:
        cmd = ints[idx]
        w1 = ints[idx + 1]
        nargs = w1 & 0xFF if w1 >= 0x100 else w1
        false = w1 >> 8 if w1 >= 0x100 else 0
        recs.append((idx, cmd, false, ints[idx + 2: idx + 1 + nargs]))
        idx += 1 + nargs
    return recs


def objects(ints, cnt):
    bases = []
    i = 0
    while i < len(ints) and ints[i] >= 0 and len(bases) < cnt:
        bases.append(ints[i]); i += 1
    for k, b in enumerate(bases):
        yield k, b, [ints[b + t] for t in range(4)]


if __name__ == "__main__":
    p = sys.argv[1]
    for g, ints, cnt in groups(SCRIPT_DIR + p):
        if len(sys.argv) > 2 and g != int(sys.argv[2]):
            continue
        print("group", g, "objects", cnt, "ints", len(ints))
        for k, b, ent in objects(ints, cnt):
            print(" obj", k, "base", b, "entry/init/exec/term", ent)
            for t, name in ((1, "init"), (2, "exec"), (3, "term")):
                for r in parse_stream(ints, ent[t]):
                    print("   ", name, r)
