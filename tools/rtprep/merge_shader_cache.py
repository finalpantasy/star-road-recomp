"""Build the runtime's merged shader cache root: our own translator (v1) first, the reused
Ryubing cache (reuse-v1, D-009) for the stages v1 cannot translate yet.

The NVN layer loads one root (nvn::ShaderCacheLoad). The merged root holds only an index and the
reuse-v1 files the fallback entries need; v1 entries point at the v1 tree with a relative path.
Everything is written under A:, nothing from the game goes into Git.

Usage: python -I tools/rtprep/merge_shader_cache.py [--cache-dir DIR]
"""
import argparse, json, os, shutil, sys

DEF = r"A:\AI\Recomps\smrpg-work\shaders\cache"


def read_index(p):
    out = []
    with open(p, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                out.append(json.loads(line))
    return out


def rewrite_files(o):
    """Meta 'file' values (program constants) are relative to the root: keep them, we copy const/ over."""
    return o


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cache-dir", default=DEF)
    a = ap.parse_args()
    base = a.cache_dir
    v1 = read_index(os.path.join(base, "v1", "index.jsonl"))
    rv = {e["key"]: e for e in read_index(os.path.join(base, "reuse-v1", "index.jsonl"))}
    out = os.path.join(base, "merged-v1")
    os.makedirs(out, exist_ok=True)
    nown = nfall = nmiss = 0
    lines = []
    for e in v1:
        if e["status"] == "ok":
            e = dict(e)
            e["spv"] = "../v1/" + e["spv"]
            e["meta"] = "../v1/" + e["meta"]
            nown += 1
        else:
            r = rv.get(e["key"])
            if r is None or r.get("status") != "ok":
                nmiss += 1          # neither cache has it: stays failed
            else:
                e = dict(r)
                e["spv"] = "../reuse-v1/" + r["spv"]
                e["meta"] = "../reuse-v1/" + r["meta"]
                # constants files are addressed from the root: copy them next to the index
                with open(os.path.join(base, "reuse-v1", r["meta"]), "r", encoding="utf-8") as f:
                    meta = json.load(f)
                stack = [meta]
                while stack:
                    o = stack.pop()
                    if isinstance(o, dict):
                        fn = o.get("file")
                        if isinstance(fn, str):
                            dst = os.path.join(out, fn)
                            os.makedirs(os.path.dirname(dst), exist_ok=True)
                            shutil.copyfile(os.path.join(base, "reuse-v1", fn), dst)
                        stack.extend(o.values())
                    elif isinstance(o, list):
                        stack.extend(o)
                nfall += 1
        lines.append(json.dumps(e))
    tmp = os.path.join(out, "index.jsonl.tmp")
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    os.replace(tmp, os.path.join(out, "index.jsonl"))
    print(f"merged-v1: {nown} from v1, {nfall} from reuse-v1, {nmiss} missing in both -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
