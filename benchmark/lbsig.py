#!/usr/bin/env python3
"""Paired significance test for lbsweep logs.

Each round runs every config under the same conditions, so rounds pair.  Uses
an exact two-sided sign test (binomial) plus a paired permutation test on the
per-round deltas -- no scipy dependency, no normality assumption, and both are
appropriate for n=10 with outlier-prone latency maxima.
"""
import os
import sys
from itertools import product
from math import comb
from statistics import median

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lbstats import parse  # noqa: E402

METRICS = [
    ("Request.p99.0", "Request p99", True),
    ("RPS.p50.0", "RPS p50", False),      # higher is better
    ("Wakeup.p99.9", "Wakeup p99.9", True),
    ("Wakeup.max", "Wakeup MAX", True),
]


def sign_test(deltas, lower_better=True):
    """two-sided exact binomial on non-zero deltas"""
    nz = [d for d in deltas if d != 0]
    n = len(nz)
    if n == 0:
        return 1.0, 0, 0
    k = sum(1 for d in nz if (d < 0) == lower_better)   # improvements
    tail = min(k, n - k)
    p = 2 * sum(comb(n, i) for i in range(tail + 1)) / 2 ** n
    return min(p, 1.0), k, n


def perm_test(deltas, iters=None):
    """exact paired permutation on signs when feasible"""
    n = len(deltas)
    obs = sum(deltas)
    if n > 20:
        return None
    hits = 0
    total = 0
    for signs in product((1, -1), repeat=n):
        s = sum(sg * d for sg, d in zip(signs, deltas))
        if abs(s) >= abs(obs) - 1e-12:
            hits += 1
        total += 1
    return hits / total


def main(path, base, *cfgs):
    sch, hb, lb = parse(path)
    fps = sorted({k[1] for k in sch})
    rounds = sorted({k[2] for k in sch})

    print(f"paired tests vs '{base}'   n={len(rounds)} rounds")
    print("negative delta = improvement (except RPS, where higher is better)")
    print("=" * 84)

    for cfg in cfgs:
        print(f"\n######## {cfg} ########")
        for fp in fps:
            print(f"  -- F{fp} --")
            for key, label, lower in METRICS:
                d = []
                for r in rounds:
                    a = sch.get((base, fp, r), {}).get(key)
                    b = sch.get((cfg, fp, r), {}).get(key)
                    if a is not None and b is not None:
                        d.append(b - a)
                if not d:
                    continue
                p, k, n = sign_test(d, lower)
                pp = perm_test(d)
                med = median(d)
                base_med = median([sch[(base, fp, r)][key] for r in rounds
                                   if (base, fp, r) in sch and key in sch[(base, fp, r)]])
                pct = med / base_med * 100 if base_med else 0
                flag = "***" if p < 0.05 else ("*" if p < 0.10 else "")
                print(f"    {label:<14} med {pct:>+7.1f}%   better {k}/{n}   "
                      f"sign p={p:.3f}  perm p={pp:.3f}  {flag}")
        # hackbench
        d = []
        for r in rounds:
            if (base, r) in hb and (cfg, r) in hb:
                d.append(hb[(cfg, r)] - hb[(base, r)])
        if d:
            p, k, n = sign_test(d)
            pp = perm_test(d)
            bm = median([hb[(base, r)] for r in rounds if (base, r) in hb])
            flag = "***" if p < 0.05 else ("*" if p < 0.10 else "")
            print(f"  -- hackbench --")
            print(f"    {'time':<14} med {median(d)/bm*100:>+7.1f}%   better {k}/{n}   "
                  f"sign p={p:.3f}  perm p={pp:.3f}  {flag}")


if __name__ == "__main__":
    main(*sys.argv[1:])
