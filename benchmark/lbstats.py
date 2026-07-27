#!/usr/bin/env python3
"""Summarise lbsweep.sh logs: paired per-round comparison, medians, consistency.

Per RESULTS.md: primary = schbench Request p99 + RPS; Wakeup max is read
separately because p99.9 alone has hidden deep tails before.  Report medians and
per-round win/loss counts, not means -- single-run maxima are noisy.
"""
import re
import sys
from collections import defaultdict
from statistics import median

BLOCK = re.compile(r"^(Wakeup|Request|RPS) .*percentiles")
PCT = re.compile(r"^\s+\*?\s*([\d.]+)th:\s+(\d+)")
MINMAX = re.compile(r"^\s+min=(\d+), max=(\d+)")
BEGIN = re.compile(r"^SCHB_BEGIN (\S+)\.F(\d+)\.r(\d+)")
HB = re.compile(r"^HB (\S+)\.r(\d+) socket=([\d.]+)")
LB = re.compile(r"^\s+(busy|idle|newidle)\s+calls=(\d+)\s+balanced=(\d+)\s+"
                r"failed=(\d+)\s+moved=(\d+)\s+hot_moved=(\d+)")
DOM = re.compile(r"^-- domain(\d+)")
ACT = re.compile(r"^\s+active\s+alb_count=(\d+) alb_pushed=(\d+)")


def parse(path):
    sch = defaultdict(dict)          # (cfg, fp, r) -> metrics
    hb = {}                          # (cfg, r) -> seconds
    lb = defaultdict(lambda: defaultdict(int))   # (cfg, r) -> counters
    key = blk = None
    dom = None
    for line in open(path):
        m = BEGIN.match(line)
        if m:
            key = (m.group(1), int(m.group(2)), int(m.group(3)))
            blk = None
            continue
        m = HB.match(line)
        if m:
            hb[(m.group(1), int(m.group(2)))] = float(m.group(3))
            continue
        m = BLOCK.match(line)
        if m and key:
            blk = m.group(1)
            continue
        m = DOM.match(line)
        if m:
            dom = int(m.group(1))
            continue
        m = ACT.match(line)
        if m and key:
            lb[(key[0], key[2])][f"d{dom}.alb"] += int(m.group(2))
            continue
        m = LB.match(line)
        if m and key:
            it, calls, bal, failed, moved, hot = m.groups()
            c = lb[(key[0], key[2])]
            c[f"d{dom}.{it}.calls"] += int(calls)
            c[f"d{dom}.{it}.failed"] += int(failed)
            c[f"d{dom}.{it}.moved"] += int(moved)
            c[f"d{dom}.{it}.hot"] += int(hot)
            continue
        m = PCT.match(line)
        if m and key and blk:
            sch[key][f"{blk}.p{m.group(1)}"] = int(m.group(2))
            continue
        m = MINMAX.match(line)
        if m and key and blk:
            sch[key][f"{blk}.max"] = int(m.group(2))
    return sch, hb, lb


def cmp_metric(sch, cfgs, fp, metric, lower_better=True):
    """returns (per-cfg medians, per-round wins for cfgs[1] vs cfgs[0])"""
    rounds = sorted({k[2] for k in sch if k[1] == fp})
    series = {c: [] for c in cfgs}
    wins = 0
    paired = 0
    for r in rounds:
        vals = {}
        for c in cfgs:
            v = sch.get((c, fp, r), {}).get(metric)
            if v is None:
                break
            vals[c] = v
            series[c].append(v)
        if len(vals) == len(cfgs):
            paired += 1
            a, b = vals[cfgs[0]], vals[cfgs[1]]
            if (b < a) if lower_better else (b > a):
                wins += 1
    med = {c: median(v) if v else None for c, v in series.items()}
    return med, wins, paired


def main(path, cfgs):
    sch, hb, lb = parse(path)
    fps = sorted({k[1] for k in sch})

    print(f"log: {path}   configs: {' vs '.join(cfgs)}")
    print("=" * 78)

    metrics = [
        ("Request.p99.0", True,  "Request p99  (usec, primary)"),
        ("Request.p50.0", True,  "Request p50  (usec)"),
        ("RPS.p50.0",     False, "RPS p50      (higher better)"),
        ("Wakeup.p99.0",  True,  "Wakeup p99   (usec)"),
        ("Wakeup.p99.9",  True,  "Wakeup p99.9 (usec)"),
        ("Wakeup.max",    True,  "Wakeup MAX   (usec, deep tail)"),
    ]
    for fp in fps:
        print(f"\n---- footprint {fp} KB ----")
        print(f"  {'metric':<30}{cfgs[0]:>12}{cfgs[1]:>12}   {'delta':>9}  rounds")
        for metric, lower, label in metrics:
            med, wins, paired = cmp_metric(sch, cfgs, fp, metric, lower)
            a, b = med[cfgs[0]], med[cfgs[1]]
            if a is None or b is None or not paired:
                continue
            d = (b - a) / a * 100 if a else 0
            better = "better" if ((d < 0) == lower and d != 0) else \
                     ("worse" if d != 0 else "tie")
            print(f"  {label:<30}{a:>12}{b:>12}   {d:>+8.1f}%  "
                  f"{wins}/{paired} {better}")

    print("\n---- hackbench (socket, seconds, lower better) ----")
    rounds = sorted({k[1] for k in hb})
    series = {c: [hb[(c, r)] for r in rounds if (c, r) in hb] for c in cfgs}
    wins = sum(1 for r in rounds
               if (cfgs[0], r) in hb and (cfgs[1], r) in hb
               and hb[(cfgs[1], r)] < hb[(cfgs[0], r)])
    paired = sum(1 for r in rounds if (cfgs[0], r) in hb and (cfgs[1], r) in hb)
    if all(series.values()):
        a, b = median(series[cfgs[0]]), median(series[cfgs[1]])
        print(f"  median  {cfgs[0]}={a:.3f}  {cfgs[1]}={b:.3f}  "
              f"delta={(b-a)/a*100:+.1f}%  {wins}/{paired} rounds better")
        for c in cfgs:
            print(f"    {c:<8} {[f'{x:.3f}' for x in series[c]]}")

    print("\n---- load-balance mechanism (summed per round, median) ----")
    keys = ["d1.newidle.calls", "d1.newidle.moved", "d1.newidle.failed",
            "d1.idle.moved", "d1.busy.moved", "d1.alb",
            "d0.newidle.moved", "d1.newidle.hot"]
    print(f"  {'counter':<22}{cfgs[0]:>12}{cfgs[1]:>12}   {'delta':>9}")
    for k in keys:
        vals = {}
        for c in cfgs:
            s = [lb[(c, r)][k] for r in sorted({kk[1] for kk in lb})
                 if (c, r) in lb]
            if s:
                vals[c] = median(s)
        if len(vals) == 2 and vals[cfgs[0]]:
            d = (vals[cfgs[1]] - vals[cfgs[0]]) / vals[cfgs[0]] * 100
            print(f"  {k:<22}{vals[cfgs[0]]:>12.0f}{vals[cfgs[1]]:>12.0f}"
                  f"   {d:>+8.1f}%")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2:])
