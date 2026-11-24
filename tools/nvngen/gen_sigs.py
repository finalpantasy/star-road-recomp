#!/usr/bin/env python3
"""Generate runtime/nvn/nvn_sigs.inc: one line per NVN proc, name plus a compact signature.

Inputs (read only, nothing copied):
  - the list of NVN procedure names found in the owner's `main` (docs: platform-survey.md; file on A:)
  - the parameter types of the public NVN C API, read from a community header (MIT) as facts.
Output signature letters (AAPCS64 classes): ret then args. v void, p pointer, i 32-bit int or enum,
l 64-bit int, f float, b NVNboolean. Example: "v:pif" = void f(ptr, int, float).
Run: python tools/nvngen/gen_sigs.py [--names FILE] [--header FILE] [--out FILE]
"""
import argparse, re, sys, os

NAMES = r"A:\AI\Recomps\smrpg-work\analysis\survey\nvn_procs_main.txt"
HDR = r"A:\AI\Recomps\new-pokemon-snap\tools\ref\totk\runtime\nvn.h"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "runtime", "nvn", "nvn_sigs.inc")

# Procs missing from the reference header (newer NVN) that we implement; the rest are logged stubs (no signature).
MANUAL = {
    "nvnDeviceGetProcAddress": "p:pp", "nvnCommandBufferSetMemoryCallback": "v:pp", "nvnCommandBufferGetMemoryCallback": "p:p",
    "nvnMemoryPoolBuilderGetMemory": "p:p", "nvnBufferBuilderGetMemoryPool": "p:p",
    "nvnBufferBuilderGetDevice": "p:p", "nvnMemoryPoolBuilderGetDevice": "p:p", "nvnQueueBuilderGetDevice": "p:p",
    "nvnSamplerBuilderGetDevice": "p:p", "nvnTextureBuilderGetDevice": "p:p", "nvnWindowBuilderGetDevice": "p:p",
    "nvnQueueBuilderGetFlags": "i:p", "nvnDeviceBuilderGetFlags": "i:p",
    "nvnQueueBuilderGetCommandMemorySize": "l:p", "nvnQueueBuilderGetComputeMemorySize": "l:p",
    "nvnQueueBuilderGetControlMemorySize": "l:p", "nvnQueueBuilderGetMemorySize": "l:p",
    "nvnQueueBuilderGetCommandFlushThreshold": "l:p", "nvnQueueBuilderGetMemory": "p:p",
    "nvnQueueBuilderGetQueuePriority": "i:p", "nvnQueueBuilderSetQueuePriority": "v:pi",
    "nvnWindowBuilderGetNumActiveTextures": "i:p", "nvnWindowBuilderGetNumTextures": "i:p",
    "nvnWindowBuilderGetTexture": "p:pi", "nvnWindowBuilderSetNumActiveTextures": "v:pi",
    "nvnWindowGetNumActiveTextures": "i:p", "nvnWindowGetNumTextures": "i:p", "nvnWindowSetNumActiveTextures": "v:pi",
    "nvnTextureBuilderGetViewOffset": "l:p", "nvnTextureGetRawStorageClass": "i:p", "nvnTextureBuilderGetRawStorageClass": "i:p",
    "nvnEventBuilderGetMemoryOffset": "l:p", "nvnEventBuilderGetMemoryPool": "p:p", "nvnEventBuilderGetStorage": "p:p",
    "nvnEventGetMemoryOffset": "l:p", "nvnEventGetMemoryPool": "p:p",
    "nvnCommandBufferSetColorReductionEnable": "v:pb", "nvnCommandBufferSetColorReductionThresholds": "v:pii",
    "nvnCommandBufferSetProvokingVertexMode": "v:pi", "nvnCommandBufferSetStencilCullCriteria": "v:pii",
    "nvnCommandBufferSetZCullZF32CompressionEnable": "v:pb", "nvnCommandBufferOverrideSubtileSize": "v:pii",
    "nvnCommandBufferSetCommandMemoryCallbackEnabled": "v:pb", "nvnCommandBufferSetControlMemoryCallbackEnabled": "v:pb",
    "nvnWindowBuilderGetNativeWindow": "p:p", "nvnWindowGetNativeWindow": "p:p",
    "nvnProgramGetSubtileSize": "v:ppp", "nvnProgramSetSampleShading": "v:pi",
}

def classify(t):
    t = re.sub(r"\s+", " ", t.strip())
    if t.endswith("*") or "*" in t or "NVNdevice" in t and "*" in t: return "p"
    t2 = t.replace("const ", "").strip()
    if t2 in ("NVNnativeWindow", "NVNsubroutineLinkageMapPtr"): return "p"
    if t2 == "float": return "f"
    if t2 == "double": return "d"
    if t2 in ("NVNboolean", "bool"): return "b"
    if t2 in ("size_t", "ptrdiff_t", "uint64_t", "int64_t", "NVNbufferAddress", "NVNtextureHandle", "NVNimageHandle",
              "NVNcommandHandle", "NVNtextureAddress", "uintptr_t") or t2.endswith("Address") or t2.endswith("Handle"):
        return "l"
    return "i"

def parse_params(a):
    a = a.strip()
    if not a or a == "void": return ""
    out = ""
    depth = 0; cur = ""; parts = []
    for ch in a:
        if ch == "(": depth += 1
        if ch == ")": depth -= 1
        if ch == "," and depth == 0: parts.append(cur); cur = ""
        else: cur += ch
    parts.append(cur)
    for p in parts:
        p = p.strip()
        if "(*" in p: out += "p"; continue
        # drop a trailing parameter name if present (an identifier preceded by a type token or '*')
        m = re.match(r"^(.*[\s\*])(\w+)$", p)
        ty = p
        if m and m.group(1).strip() and m.group(2) not in ("int", "float", "double", "char", "void") and not m.group(2).startswith("NVN") and "uint" not in m.group(2) and m.group(2) not in ("size_t", "ptrdiff_t"):
            ty = m.group(1)
        out += classify(ty)
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--names", default=NAMES); ap.add_argument("--header", default=HDR); ap.add_argument("--out", default=OUT)
    a = ap.parse_args()
    names = [l.strip() for l in open(a.names) if l.strip()]
    t = open(a.header, encoding="utf8").read()
    t = re.sub(r"/\*.*?\*/", "", t, flags=re.S); t = re.sub(r"//[^\n]*", "", t)
    defs = re.findall(r"typedef\s+([^;(]+?)\s*\(\*\s*(PFNNVN\w+PROC)\)\s*\(([^;]*?)\)\s*;", t, flags=re.S)
    by = {n[6:-4].lower(): (r, p) for r, n, p in defs}
    lines = []; nosig = 0
    for n in names:
        k = n[3:].lower()
        if n in MANUAL: sig = MANUAL[n]
        elif k in by:
            r, p = by[k]
            r = r.strip()
            sig = ("v" if r == "void" else classify(r)) + ":" + parse_params(p)
        else: sig = "?"; nosig += 1
        if re.search(r"(GetMemoryPool|GetDevice|GetNativeWindow|GetStorage)$", n) and sig[0] in "iv": sig = "p" + sig[1:]
        if n == "nvnTextureGetStorageSize": sig = "l:p"
        lines.append('NVN_SIG(%s, "%s")' % (n, sig))
    with open(a.out, "w", newline="\n") as f:
        f.write("// Generated by tools/nvngen/gen_sigs.py. Do not edit. Signature letters: v void, p ptr, i int, l int64, f float, b bool.\n")
        f.write("\n".join(lines) + "\n")
    print("procs", len(names), "without signature", nosig, "->", a.out)

main()
