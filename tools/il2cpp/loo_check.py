"""Leave-one-out check of the neighbour inference used for tier C labels.

For every band anchor (tier A/B, excluding point-only evidence) hide its label and
predict it from the previous and next anchors. When both neighbours agree the prediction
is the shared class (this is exactly the rule that produces tier C labels). Reports how
often that prediction equals the true anchor class.
"""
import collections
import csv
import json
import os
import sys

OUT_DEFAULT = r"A:\AI\Recomps\smrpg-work\analysis\il2cpp"
POINT_EVIDENCE = ("dynsym", "import_thunk")


def main(outdir=OUT_DEFAULT):
    rows = list(csv.reader(open(os.path.join(outdir, "functions.tsv"), encoding="utf-8"), delimiter="\t"))[1:]
    anchors = [r for r in rows if r[7] in ("A", "B") and r[8] not in POINT_EVIDENCE]
    n = len(anchors)
    decisive = ok = boundary = 0
    per = collections.defaultdict(lambda: [0, 0, 0])
    wrong = collections.Counter()
    for i in range(1, n - 1):
        c = anchors[i][5]
        p, q = anchors[i - 1][5], anchors[i + 1][5]
        if p == q:
            per[c][0] += 1
            decisive += 1
            if p == c:
                ok += 1
                per[c][1] += 1
            else:
                wrong[(c, p)] += 1
        else:
            boundary += 1
            per[c][2] += 1
    res = {"anchors": n, "decisive": decisive, "correct": ok, "accuracy": round(ok / decisive, 5),
           "boundary_cases": boundary,
           "per_class": {c: {"decisive": v[0], "correct": v[1], "boundary": v[2]} for c, v in per.items()},
           "wrong_pairs": {"%s_predicted_%s" % k: v for k, v in wrong.most_common(10)}}
    with open(os.path.join(outdir, "loo_check.json"), "w") as f:
        json.dump(res, f, indent=1)
    print(json.dumps(res, indent=1))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else OUT_DEFAULT)
