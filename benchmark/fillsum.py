#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Summarize a fillab.sh log.

lat:  pooled time-to-fill statistics per duty cycle and ticker setting.
tput: hog CPU time and work during the free phase, per free-phase length and
      ticker setting.
Both also give the per-block ratio LB_CONCEDE_CHEAPEST / NO_LB_CONCEDE_CHEAPEST;
blocks are paired, so the ratio is free of drift between blocks.  A trial in
which no hog arrived within the run's limit counts as the limit (lat) or as
"none" (tput wait)."""
import re
import statistics as st
import sys
from collections import defaultdict

ON, OFF = 'LB_CONCEDE_CHEAPEST', 'NO_LB_CONCEDE_CHEAPEST'
LAT = re.compile(r'block=(\d+) feat=(\S+) test=lat duty=(\S+) tick=(\d+) trial \d+ (\S+)')
TPUT = re.compile(r'block=(\d+) feat=(\S+) test=tput free=(\d+) tick=(\d+) cycle \d+ '
                  r'cpu=(\S+) work=(\S+) wait=(\S+)')

lat = defaultdict(lambda: defaultdict(list))   # (duty, tick, feat) -> block -> [ms]
tput = defaultdict(lambda: defaultdict(list))  # (free, tick, feat) -> block -> [(cpu, work, wait)]
for line in open(sys.argv[1] if len(sys.argv) > 1 else 'fillab.log'):
    m = LAT.match(line)
    if m:
        lat[(m[3], int(m[4]), m[2])][int(m[1])].append(float(m[5]))
        continue
    m = TPUT.match(line)
    if m:
        tput[(int(m[3]), int(m[4]), m[2])][int(m[1])].append((float(m[5]), float(m[6]), float(m[7])))


def q(v, p):
    v = sorted(v)
    return v[round(p * (len(v) - 1))]


def flat(d):
    return [x for b in sorted(d) for x in d[b]]


def ratios(on, off, f):
    ks = sorted(set(on) & set(off))
    return [f(on[k]) / f(off[k]) for k in ks]


def ratio_line(r, better_below):
    good = sum(x < 1 for x in r) if better_below else sum(x > 1 for x in r)
    return f"median {st.median(r):.3f}, range {min(r):.3f}-{max(r):.3f}, better in {good}/{len(r)} blocks"


def tick_name(t):
    return f'ticker {t} us' if t else 'no ticker'


if lat:
    print("== time until the idle CPU gets work (ms)")
    for duty, tick in sorted({k[:2] for k in lat}, key=lambda k: (k[1], -float(k[0].split('/')[0]))):
        print(f"duty {duty}, {tick_name(tick)}")
        for f, name in ((OFF, 'upstream'), (ON, 'cambyses')):
            v = flat(lat[(duty, tick, f)])
            if v:
                print(f"  {name:9s} n={len(v):4d} mean {st.mean(v):7.2f}  p10 {q(v, .1):6.2f}  p50 {q(v, .5):6.2f}"
                      f"  p90 {q(v, .9):7.2f}  p99 {q(v, .99):7.2f}")
        r = ratios(lat[(duty, tick, ON)], lat[(duty, tick, OFF)], st.mean)
        if r:
            print(f"  cambyses/upstream mean: {ratio_line(r, True)}")
    print()

if tput:
    print("== hog CPU time and work during the free phase (ms per cycle)")
    for free, tick in sorted({k[:2] for k in tput}, key=lambda k: (k[1], k[0])):
        print(f"free {free} ms, {tick_name(tick)}")
        res = {}
        for f, name in ((OFF, 'upstream'), (ON, 'cambyses')):
            v = flat(tput[(free, tick, f)])
            if v:
                waits = sorted(w if w >= 0 else float('inf') for _, _, w in v)
                res[f] = (st.mean(c for c, _, _ in v), st.mean(w for _, w, _ in v))
                p50 = waits[len(waits) // 2]
                print(f"  {name:9s} n={len(v):4d} cpu {res[f][0]:7.2f}  work {res[f][1]:7.2f}  "
                      f"wait p50 {'none' if p50 == float('inf') else f'{p50:.1f}'}  "
                      f"no arrival {sum(w < 0 for _, _, w in v)}")
        if len(res) == 2:
            print(f"  cambyses vs upstream: cpu {(res[ON][0] / res[OFF][0] - 1) * 100:+.1f}%, "
                  f"work {(res[ON][1] / res[OFF][1] - 1) * 100:+.1f}%")
            r = ratios(tput[(free, tick, ON)], tput[(free, tick, OFF)],
                       lambda v: st.mean(w for _, w, _ in v))
            print(f"  work ratio per block: {ratio_line(r, False)}")
