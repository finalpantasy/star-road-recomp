"""Cross-check our IL2CPP map against Il2CppDumper output (dump.cs, script.json, stringliteral.json).

Il2CppDumper is only a second opinion; our parsers stay the source of truth. Run the dumper first:
  Il2CppDumper.exe <main NSO> <global-metadata.dat> <outdir>   (outdir = <analysis>/crosscheck/dumper)
Then: python -I tools/il2cpp/crosscheck_dumper.py
Writes <analysis>/crosscheck/report.json and prints it. Nothing from the dumper output enters Git.
"""
import collections
import csv
import json
import os
import re

OUT = r"A:\AI\Recomps\smrpg-work\analysis\il2cpp"
DUMP = os.path.join(OUT, "crosscheck", "dumper")
MAP = {"t": "\t", "n": "\n", "r": "\r", "\\": "\\"}


def tsv(name):
    with open(os.path.join(OUT, name), encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f, delimiter="\t", quoting=csv.QUOTE_NONE))


def unesc(s):
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            out.append(MAP.get(s[i + 1], "\\" + s[i + 1]))
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def methods(rep):
    script = json.load(open(os.path.join(DUMP, "script.json"), encoding="utf-8"))
    theirs = {m["Address"]: m["Name"] for m in script["ScriptMethod"]}
    ours = {}
    for d in tsv("methods.tsv"):
        if d["rva"]:
            ours[int(d["rva"], 16)] = d["type"].replace("/", ".") + "$$" + d["name"]
    gen = {int(d["rva"], 16) for d in tsv("generic_methods.tsv") if d["rva"]}
    oa, ta = set(ours), set(theirs)
    rep["methods"] = {
        "ours": len(oa), "dumper": len(ta), "common_addresses": len(oa & ta),
        "ours_only": len(oa - ta), "dumper_only": len(ta - oa),
        "dumper_only_that_are_our_generic_instances": len((ta - oa) & gen),
        "dumper_only_unexplained": len(ta - oa - gen),
        "name_equal_on_common": sum(1 for a in oa & ta if ours[a] == theirs[a]),
        "dumper_names_with_fully_shared_type": sum(1 for n in theirs.values() if "__Il2CppFullySharedGenericType" in n),
    }
    mm = script["ScriptMetadataMethod"]
    known = set(ours) | gen
    nz = [x for x in mm if x["MethodAddress"]]
    rep["metadata_method_usages"] = {"total": len(mm), "with_address": len(nz), "zero_address": len(mm) - len(nz),
                                     "address_in_our_map": sum(1 for x in nz if x["MethodAddress"] in known)}


def fields(rep):
    types = tsv("types.tsv")
    vt = {int(t["type_index"]): t["is_valuetype"] == "1" for t in types}
    gen = {int(t["type_index"]): t["generic_container"] != "-1" for t in types}
    ours = collections.defaultdict(list)
    for f in tsv("fields.tsv"):
        if f["is_static"] == "0" and f["is_literal"] == "0":
            ours[int(f["type_index"])].append(int(f["offset"]))
    theirs = collections.defaultdict(list)
    cur = None
    fre = re.compile(r"^\t(?!//)(.*);\s*// 0x([0-9A-Fa-f]+)\s*$")
    with open(os.path.join(DUMP, "dump.cs"), encoding="utf-8") as f:
        for line in f:
            m = re.search(r"// TypeDefIndex: (\d+)", line)
            if m and not line.startswith("\t"):
                cur = int(m.group(1))
                continue
            m = fre.match(line.rstrip("\n"))
            if cur is not None and m and " static " not in " " + m.group(1) and "(" not in m.group(1).split("=")[0]:
                theirs[cur].append(int(m.group(2), 16))
    c = collections.Counter()
    for ti in range(len(types)):
        o = [x - 16 if vt[ti] else x for x in ours.get(ti, [])]
        t = theirs.get(ti, [])
        if o == t:
            c["identical"] += 1
        elif gen[ti] and vt[ti] and all(x == 0 for x in t) and all(x == 0 for x in ours.get(ti, [])):
            c["generic_value_type_definition_all_zero_both"] += 1
        else:
            c["real_difference"] += 1
    rep["field_offsets"] = dict(c)
    rep["field_offsets"]["types"] = len(types)
    rep["field_offsets"]["instance_fields_ours"] = sum(len(v) for v in ours.values())
    rep["field_offsets"]["instance_fields_dumper"] = sum(len(v) for v in theirs.values())
    ndump = 0
    with open(os.path.join(DUMP, "dump.cs"), encoding="utf-8") as f:
        for line in f:
            if "// TypeDefIndex:" in line and not line.startswith("\t"):
                ndump += 1
    rep["types_in_dump_cs"] = ndump


def strings(rep):
    with open(os.path.join(OUT, "strings.tsv"), encoding="utf-8", newline="") as f:
        rows = f.read().split("\n")[1:-1]
    ours = []
    for r in rows:
        p = r.split("\t", 2)
        ours.append(unesc(p[2]) if len(p) > 2 else "")
    theirs = [x["value"] for x in json.load(open(os.path.join(DUMP, "stringliteral.json"), encoding="utf-8"))]
    co, ct = collections.Counter(ours), collections.Counter(theirs)
    rep["strings"] = {"ours": len(ours), "dumper": len(theirs), "ours_only": sum((co - ct).values()),
                      "dumper_only": sum((ct - co).values())}


def generic_sharing(rep):
    g = tsv("generic_methods.tsv")
    has = collections.Counter()
    for d in g:
        if d["rva"]:
            has[d["method_def_index"]] += 1
    nop = [d for d in g if not d["rva"]]
    cov = sum(1 for d in nop if has[d["method_def_index"]] > 0)
    rep["generic_sharing"] = {"specs": len(g), "specs_with_pointer": len(g) - len(nop), "specs_without_pointer": len(nop),
                              "without_pointer_but_def_has_a_shared_instance": cov,
                              "without_pointer_and_no_shared_instance": len(nop) - cov,
                              "pointer_specs_with_fully_shared_arg": sum(1 for d in g if d["rva"] and "__Il2CppFullySharedGenericType" in d["class_args"] + d["method_args"])}


def main():
    rep = {}
    methods(rep)
    fields(rep)
    strings(rep)
    generic_sharing(rep)
    with open(os.path.join(OUT, "crosscheck", "report.json"), "w") as f:
        json.dump(rep, f, indent=1)
    print(json.dumps(rep, indent=1))


if __name__ == "__main__":
    main()
