#!/usr/bin/env python3
"""Refresh the proc-count table in docs/NVN.md from the sources.

Reads runtime/nvn/nvn_sigs.inc (every proc name, 535) and the NVN_API(...) registrations in runtime/nvn/nvn_api_*.cpp, groups the
procs by object class and rewrites the block between the proc-table markers in docs/NVN.md. Names are never listed: only counts.
Run: python -I tools/nvngen/gen_doc.py [--check]      (--check exits 1 if the doc is out of date instead of writing)
"""
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
NVN = os.path.join(ROOT, "runtime", "nvn")
DOC = os.path.join(ROOT, "docs", "NVN.md")
BEGIN, END = "<!-- proc-table:begin -->", "<!-- proc-table:end -->"
CLASSES = ["CommandBufferBuilder", "CommandBuffer", "DeviceBuilder", "Device", "EventBuilder", "Event", "MemoryPoolBuilder", "MemoryPool",
           "BufferBuilder", "Buffer", "TextureBuilder", "TextureView", "TexturePool", "Texture", "SamplerBuilder", "SamplerPool", "Sampler",
           "QueueBuilder", "Queue", "WindowBuilder", "Window", "Sync", "Program", "BlendState", "ColorState", "ChannelMaskState",
           "DepthStencilState", "MultisampleState", "PolygonState", "VertexAttribState", "VertexStreamState", "CounterData", "Bootstrap"]


def klass(name):
    rest = name[3:]
    for c in CLASSES:
        if rest.startswith(c):
            return c
    return "Other"


def main():
    names = []
    with open(os.path.join(NVN, "nvn_sigs.inc"), encoding="utf-8") as f:
        for line in f:
            m = re.match(r'\s*NVN_SIG\((\w+)\s*,', line)
            if m:
                names.append(m.group(1))
    impl = set()
    for fn in os.listdir(NVN):
        if fn.startswith("nvn_api_") and fn.endswith(".cpp"):
            with open(os.path.join(NVN, fn), encoding="utf-8") as f:
                impl.update(re.findall(r'NVN_API\((\w+)\s*,', f.read()))
    impl &= set(names)
    rows = {}
    for n in names:
        r = rows.setdefault(klass(n), [0, 0])
        r[0] += 1
        r[1] += n in impl
    lines = ["| Object class | Procs | Implemented | Logged stubs |", "|---|---|---|---|"]
    for c in sorted(rows, key=lambda k: (k == "Other", k)):
        t, i = rows[c]
        lines.append(f"| {c} | {t} | {i} | {t - i} |")
    lines.append(f"| **Total** | **{len(names)}** | **{len(impl)}** | **{len(names) - len(impl)}** |")
    block = BEGIN + "\n" + "\n".join(lines) + "\n" + END
    with open(DOC, encoding="utf-8") as f:
        doc = f.read()
    new = re.sub(re.escape(BEGIN) + r".*?" + re.escape(END), lambda m: block, doc, flags=re.S)
    if new == doc:
        print("docs/NVN.md proc table is current:", len(names), "procs,", len(impl), "implemented")
        return 0
    if "--check" in sys.argv:
        print("docs/NVN.md proc table is out of date")
        return 1
    with open(DOC, "w", encoding="utf-8", newline="\n") as f:
        f.write(new)
    print("docs/NVN.md updated:", len(names), "procs,", len(impl), "implemented")
    return 0


if __name__ == "__main__":
    sys.exit(main())
