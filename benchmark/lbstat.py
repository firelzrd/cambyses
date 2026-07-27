#!/usr/bin/env python3
"""Summarise /proc/schedstat load-balance counters, summed over all CPUs.

schedstat v16 domain line:
  domain<N> <mask> [8 counters] x CPU_MAX_IDLE_TYPES  then 12 more

  per idle type: lb_count lb_balanced lb_failed lb_imbalance
                 lb_gained lb_hot_gained lb_nobusyq lb_nobusyg
  idle types:    0 = NOT_IDLE (periodic, busy)
                 1 = IDLE     (periodic, cpu idle)
                 2 = NEWLY_IDLE
  tail: alb_count alb_failed alb_pushed sbe_* x3 sbf_* x3
        ttwu_wake_remote ttwu_move_affine ttwu_move_balance

Usage:  lbstat.py snap > before ; ...workload... ; lbstat.py snap > after
        lbstat.py diff before after
"""
import sys
from collections import defaultdict

PER = ["lb_count", "lb_balanced", "lb_failed", "lb_imbalance",
       "lb_gained", "lb_hot_gained", "lb_nobusyq", "lb_nobusyg"]
ITYPE = {0: "busy", 1: "idle", 2: "newidle"}
TAIL = ["alb_count", "alb_failed", "alb_pushed",
        "sbe_count", "sbe_balanced", "sbe_pushed",
        "sbf_count", "sbf_balanced", "sbf_pushed",
        "ttwu_wake_remote", "ttwu_move_affine", "ttwu_move_balance"]


def snap():
    """{(domain_idx, key): total} summed across CPUs."""
    out = defaultdict(int)
    dom = -1
    for line in open("/proc/schedstat"):
        f = line.split()
        if not f:
            continue
        if f[0].startswith("cpu"):
            dom = -1
            continue
        if not f[0].startswith("domain"):
            continue
        dom = int(f[0][6:])
        v = [int(x) for x in f[2:]]
        for it in range(3):
            base = it * 8
            for j, name in enumerate(PER):
                out[(dom, f"{ITYPE[it]}.{name}")] += v[base + j]
        for j, name in enumerate(TAIL):
            out[(dom, name)] += v[24 + j]
    return out


def fmt(d):
    return "\n".join(f"{dom} {k} {v}" for (dom, k), v in sorted(d.items()))


def load(path):
    d = {}
    for line in open(path):
        dom, k, v = line.split()
        d[(int(dom), k)] = int(v)
    return d


def diff(a, b, label=""):
    keys = sorted(set(a) | set(b))
    doms = sorted({k[0] for k in keys})
    print(f"=== load-balance activity {label} ===")
    for dom in doms:
        rows = [(k[1], b.get(k, 0) - a.get(k, 0)) for k in keys if k[0] == dom]
        rows = [(k, v) for k, v in rows if v]
        if not rows:
            continue
        span = "SMT" if dom == 0 else "LLC"
        print(f"\n-- domain{dom} ({span}) --")
        for it in ("busy", "idle", "newidle"):
            sub = {k.split(".", 1)[1]: v for k, v in rows if k.startswith(it + ".")}
            if not sub:
                continue
            cnt = sub.get("lb_count", 0)
            gained = sub.get("lb_gained", 0)
            failed = sub.get("lb_failed", 0)
            bal = sub.get("lb_balanced", 0)
            hot = sub.get("lb_hot_gained", 0)
            print(f"  {it:<8} calls={cnt:<7} balanced={bal:<7} failed={failed:<7} "
                  f"moved={gained:<7} hot_moved={hot}")
        tail = {k: v for k, v in rows if "." not in k}
        alb = [f"{k}={v}" for k, v in tail.items() if k.startswith("alb")]
        if alb:
            print(f"  active   {' '.join(alb)}")


if __name__ == "__main__":
    if sys.argv[1] == "snap":
        print(fmt(snap()))
    else:
        diff(load(sys.argv[2]), load(sys.argv[3]),
             sys.argv[4] if len(sys.argv) > 4 else "")
