#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Draw a fillab.sh log as an SVG that follows the viewer's light/dark
preference: time to fill the idle CPU (top) and hog throughput during the free
phase (bottom), upstream vs Cambyses.  PANEL lat or tput draws only that part;
PANEL runs draws a many-run log (one "duty=.. tick=.. R<n> <feature> FILL ..."
line per run) as one dot per run.
usage: mkfig.py LOG OUT.svg [both|lat|tput|runs] [NOTE] [ticks=T1,T2]
ticks= keeps only those ticker settings (us, 0 = none)."""
import math
import re
import statistics as st
import sys
from collections import defaultdict

ON, OFF = 'LB_CONCEDE_CHEAPEST', 'NO_LB_CONCEDE_CHEAPEST'
LAT = re.compile(r'block=\d+ feat=(\S+) test=lat duty=(\S+) tick=(\d+) trial \d+ (\S+)')
TPUT = re.compile(r'block=\d+ feat=(\S+) test=tput free=(\d+) tick=(\d+) cycle \d+ cpu=\S+ work=(\S+)')
lat, tput = defaultdict(list), defaultdict(list)
for line in open(sys.argv[1]):
    m = LAT.match(line)
    if m:
        lat[(m[2], int(m[3]), m[1])].append(float(m[4]))
        continue
    m = TPUT.match(line)
    if m:
        tput[(int(m[2]), int(m[3]), m[1])].append(float(m[4]))
panel = sys.argv[3] if len(sys.argv) > 3 else 'both'
note = sys.argv[4] if len(sys.argv) > 4 else ''
keep = None
for arg in sys.argv[5:]:
    if arg.startswith('ticks='):
        keep = {int(t) for t in arg[6:].split(',')}
if keep is not None:
    for d in (lat, tput):
        for k in [k for k in d if k[1] not in keep]:
            del d[k]
LAT_HEAD = 'Time until the idle CPU gets work — lower is better'
TPUT_HEAD = 'Hog work in the window after each release — higher is better'
if panel == 'lat':
    tput.clear()
elif panel == 'tput':
    lat.clear()
RUNS_HEAD = 'Mean wait of each independent run — lower is better'
TITLE = {'lat': 'Distribution of the time until the idle CPU gets work — lower is better',
         'tput': TPUT_HEAD, 'runs': RUNS_HEAD}.get(panel, 'Idle-fill test, upstream vs Cambyses')
single = panel in ('lat', 'tput', 'runs')


def q(v, p):
    v = sorted(v)
    return v[round(p * (len(v) - 1))]


def tick_name(t):
    return f'ticker {t} µs' if t else 'no ticker'


SER = [(OFF, 'Upstream', '1'), (ON, 'Cambyses', '2')]
LIGHT = dict(bg='#fcfcfb', ink='#0b0b0b', ink2='#52514e', muted='#8a8985', grid='#e6e5e0', s1='#2a78d6', s2='#eb6834')
DARK = dict(bg='#1a1a19', ink='#f0efec', ink2='#c3c2b7', muted='#8f8e88', grid='#33332f', s1='#3987e5', s2='#d95926')
FONT = 'font-family:-apple-system,Segoe UI,Helvetica,Arial,sans-serif;'
TEXT = dict(title=('ink', 'font-size:16px;font-weight:600;'), head=('ink', 'font-size:14px;font-weight:600;'),
            sub=('ink2', 'font-size:12px;'), leg=('ink2', 'font-size:12px;'), lab=('ink', 'font-size:13px;'),
            val=('ink', 'font-size:11.5px;'), ax=('muted', 'font-size:11px;'))


def rules(c, full):
    out = [f".{k}{{{FONT + extra if full else ''}fill:{c[col]}}}" for k, (col, extra) in TEXT.items()]
    out += [f".bg{{fill:{c['bg']}}}", f".grid{{stroke:{c['grid']}}}",
            f".f1{{fill:{c['s1']}}}", f".f2{{fill:{c['s2']}}}",
            f".l1{{stroke:{c['s1']}}}", f".l2{{stroke:{c['s2']}}}",
            f".d1{{fill:{c['s1']};stroke:{c['bg']}}}", f".d2{{fill:{c['s2']};stroke:{c['bg']}}}",
            f".k{{fill:{c['ink2']}}}", f".kd{{fill:{c['ink2']};stroke:{c['bg']}}}", f".kl{{stroke:{c['ink2']}}}"]
    return '\n'.join(out)


W, L, R = 760, 170, 640
LANE, ROWH = 22, 66
o = []
y = 32
o.append(f'<text x="20" y="{y}" class="title">{TITLE}</text>')
y += 20
o.append(f'<text x="20" y="{y}" class="sub">{note}</text>')

# legend
y += 28
lx = 20
for _, name, i in SER:
    o.append(f'<rect x="{lx}" y="{y - 6}" width="22" height="8" rx="4" class="f{i}"/>')
    o.append(f'<text x="{lx + 30}" y="{y + 2}" class="lab">{name}</text>')
    lx += 30 + len(name) * 7.6 + 26
o.append(f'<circle cx="{lx + 6}" cy="{y - 2}" r="4.5" stroke-width="2" class="kd"/>'
         f'<text x="{lx + 16}" y="{y + 2}" class="leg">{"mean" if panel == "tput" else "one run" if panel == "runs" else "median" if single else "median / mean"}</text>')
lx += 110 if not single else 80
if panel == 'runs':
    o.append(f'<line x1="{lx + 6}" y1="{y - 9}" x2="{lx + 6}" y2="{y + 5}" stroke-width="3" class="kl"/>'
             f'<text x="{lx + 16}" y="{y + 2}" class="leg">mean of the runs</text>')
elif panel != 'tput':
    o.append(f'<rect x="{lx}" y="{y - 6}" width="22" height="8" rx="4" class="k"/>'
         f'<text x="{lx + 28}" y="{y + 2}" class="leg">p10–p90</text>')
lx += 92
if panel not in ('tput', 'runs'):
    o.append(f'<line x1="{lx}" y1="{y - 2}" x2="{lx + 22}" y2="{y - 2}" stroke-width="2" class="kl"/>'
         f'<text x="{lx + 28}" y="{y + 2}" class="leg">to p99</text>')


def axis(top, bottom, lo, hi, step, fmt, label):
    def x(v):
        return L + (min(max(v, lo), hi) - lo) / (hi - lo) * (R - L)
    v = lo
    while v <= hi + 1e-9:
        o.append(f'<line x1="{x(v):.1f}" y1="{top}" x2="{x(v):.1f}" y2="{bottom}" stroke-width="1" class="grid"/>')
        o.append(f'<text x="{x(v):.1f}" y="{bottom + 16}" text-anchor="middle" class="ax">{fmt(v)}</text>')
        v += step
    o.append(f'<text x="{(L + R) / 2:.0f}" y="{bottom + 34}" text-anchor="middle" class="ax">{label}</text>')
    return x


STEPS = [m * 10 ** e for e in range(-2, 5) for m in (1, 2, 5)]


def log_axis(top, bottom, values, label, floor=None):
    """Log axis on 1-2-5 ticks spanning values; values below floor are drawn at
    floor and the label says so."""
    lo = max(s_ for s_ in STEPS if s_ <= max(min(values), floor or 0))
    hi = min(s_ for s_ in STEPS if s_ >= max(values))
    a, b = math.log10(lo), math.log10(hi)

    def x(v):
        return L + (math.log10(min(max(v, lo), hi)) - a) / (b - a) * (R - L)
    for v in (s_ for s_ in STEPS if lo <= s_ <= hi):
        o.append(f'<line x1="{x(v):.1f}" y1="{top}" x2="{x(v):.1f}" y2="{bottom}" stroke-width="1" class="grid"/>')
        o.append(f'<text x="{x(v):.1f}" y="{bottom + 16}" text-anchor="middle" class="ax">{v:g}</text>')
    if min(values) < lo:
        label += f'; values below {lo:g} drawn at {lo:g}'
    o.append(f'<text x="{(L + R) / 2:.0f}" y="{bottom + 34}" text-anchor="middle" class="ax">{label}</text>')
    return x


# top panel: time to fill
lat_rows = sorted({k[:2] for k in lat}, key=lambda k: (-k[1], -float(k[0].split('/')[0])))
if lat_rows:
    y += 26 if single else 40
    if not single:
        o.append(f'<text x="20" y="{y}" class="head">{LAT_HEAD}</text>')
    if panel != 'lat':  # a lat-only figure shows the distribution; means are given elsewhere
        o.append(f'<text x="{R + 20}" y="{y}" class="ax">mean</text>')
    top = y + 12
    bottom = top + ROWH * len(lat_rows)
    shown = [q(lat[k], p) for k in lat for p in (.1, .5, .9, .99)]
    x = log_axis(top, bottom, shown, 'ms, log scale', floor=0.1)
    for r, (duty, tick) in enumerate(lat_rows):
        y0 = top + r * ROWH + 16
        o.append(f'<text x="20" y="{y0 + LANE / 2 + 4}" class="lab">duty {duty.replace("/", " / ")} ms</text>')
        o.append(f'<text x="20" y="{y0 + LANE / 2 + 20}" class="ax">{tick_name(tick)}</text>')
        for j, (feat, name, i) in enumerate(SER):
            v = lat[(duty, tick, feat)]
            if not v:
                continue
            p10, p50, p90, p99 = (q(v, a) for a in (.1, .5, .9, .99))
            yc = y0 + j * LANE
            mean = f'mean {st.mean(v):.1f}, ' if panel != 'lat' else ''
            o.append(f'<g><title>{name}, duty {duty}, {tick_name(tick)}: {mean}p10 {p10:.1f}, '
                     f'median {p50:.1f}, p90 {p90:.1f}, p99 {p99:.1f} ms (n={len(v)})</title>')
            o.append(f'<line x1="{x(p90):.1f}" y1="{yc}" x2="{x(p99):.1f}" y2="{yc}" stroke-width="2" class="l{i}"/>')
            o.append(f'<rect x="{x(p10):.1f}" y="{yc - 4}" width="{max(x(p90) - x(p10), 8):.1f}" height="8" '
                     f'rx="4" class="f{i}"/>')
            o.append(f'<circle cx="{x(p50):.1f}" cy="{yc}" r="5" stroke-width="2" class="d{i}"/>')
            if panel != 'lat':
                o.append(f'<text x="{R + 20}" y="{yc + 4}" class="val">{st.mean(v):.1f}</text>')
            o.append('</g>')
    y = bottom + 34

# bottom panel: throughput, as CPUs' worth of hog work during the free phase
tput_rows = sorted({k[:2] for k in tput}, key=lambda k: (-k[1], k[0]))
if tput_rows:
    y += 26 if single else 46
    if not single:
        o.append(f'<text x="20" y="{y}" class="head">{TPUT_HEAD}</text>')
    o.append(f'<text x="{R + 20}" y="{y}" class="ax">gain</text>')
    top = y + 12
    vals = [st.mean(tput[(f, t, s)]) / f for f, t in tput_rows for s, _, _ in SER if tput[(f, t, s)]]
    lo = int(min(vals) * 10) / 10 - 0.1
    hi = int(max(vals) * 10) / 10 + 0.2
    rowh = 40
    bottom = top + rowh * len(tput_rows)
    x = axis(top, bottom, lo, hi, 0.1 if hi - lo < 1.2 else 0.2, lambda v: f'{v:.1f}', 'CPUs of hog work')
    for r, (free, tick) in enumerate(tput_rows):
        yc = top + r * rowh + rowh / 2
        o.append(f'<text x="20" y="{yc - 2}" class="lab">window {free} ms</text>')
        o.append(f'<text x="20" y="{yc + 14}" class="ax">{tick_name(tick)}</text>')
        a, b = (st.mean(tput[(free, tick, s)]) / free for s in (OFF, ON))
        o.append(f'<line x1="{x(a):.1f}" y1="{yc}" x2="{x(b):.1f}" y2="{yc}" stroke-width="2" class="kl"/>')
        for v, (_, name, i) in zip((a, b), SER):
            o.append(f'<g><title>{name}, window {free} ms, {tick_name(tick)}: {v:.3f} CPUs of hog work</title>'
                     f'<circle cx="{x(v):.1f}" cy="{yc}" r="6" stroke-width="2" class="d{i}"/></g>')
        o.append(f'<text x="{R + 20}" y="{yc + 4}" class="val">{(b / a - 1) * 100:+.1f}%</text>')
    y = bottom + 34

if panel == 'runs':
    RUNS = re.compile(r'duty=(\S+) tick=(\d+) R\d+ (\S+) FILL .* mean=(\S+)')
    runs = defaultdict(list)
    for line in open(sys.argv[1]):
        m = RUNS.match(line)
        if m:
            runs[(m[1], int(m[2]), m[3])].append(float(m[4]))
    rows = sorted({k[:2] for k in runs}, key=lambda k: (-k[1], -float(k[0].split('/')[0])))
    y += 26
    o.append(f'<text x="{R + 20}" y="{y}" class="ax">mean</text>')
    top = y + 12
    bottom = top + ROWH * len(rows)
    x = log_axis(top, bottom, [v for vs in runs.values() for v in vs], 'ms, log scale')
    for r, (duty, tick) in enumerate(rows):
        y0 = top + r * ROWH + 16
        o.append(f'<text x="20" y="{y0 + LANE / 2 + 4}" class="lab">duty {duty.replace("/", " / ")} ms</text>')
        o.append(f'<text x="20" y="{y0 + LANE / 2 + 20}" class="ax">{tick_name(tick)}</text>')
        for j, (feat, name, i) in enumerate(SER):
            v = runs[(duty, tick, feat)]
            if not v:
                continue
            yc = y0 + j * LANE
            mean = st.mean(v)
            o.append(f'<g><title>{name}, duty {duty}, {tick_name(tick)}: {len(v)} runs, mean of run means '
                     f'{mean:.1f} ms, range {min(v):.1f}-{max(v):.1f}</title>')
            for k, val in enumerate(v):
                o.append(f'<circle cx="{x(val):.1f}" cy="{yc + (k % 3 - 1) * 3}" r="3.5" stroke-width="1" '
                         f'class="d{i}"/>')
            o.append(f'<line x1="{x(mean):.1f}" y1="{yc - 9}" x2="{x(mean):.1f}" y2="{yc + 9}" stroke-width="3" '
                     f'class="kl"/>')
            o.append(f'<text x="{R + 20}" y="{yc + 4}" class="val">{mean:.1f}</text></g>')
    y = bottom + 34

H = int(y + 20)
head = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" role="img" '
        'aria-labelledby="t d">',
        f'<title id="t">{TITLE}</title>',
        '<desc id="d">Time until an idle CPU in another LLC gets work, and the hog work done during the free '
        'phase, upstream vs Cambyses, with and without a ticker on the idle CPU.</desc>',
        '<style>', rules(LIGHT, True), '@media (prefers-color-scheme:dark){\n' + rules(DARK, False) + '\n}',
        '</style>', f'<rect width="{W}" height="{H}" rx="8" class="bg"/>']
open(sys.argv[2], 'w').write('\n'.join(head + o + ['</svg>']) + '\n')
