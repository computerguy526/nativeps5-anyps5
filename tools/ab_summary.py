#!/usr/bin/env python3
"""Summarise the "[ab] mode=A|B ..." lines printed by APS5_AB_PERIOD / APS5_AB_FLAGS.

usage: ab_summary.py LOGFILE [LOGFILE...]     (use - for stdin)

The first period after each mode switch (since_switch=0) is warm-up (caches, spin
state, pipelines settle) and is excluded. Run with APS5_AB_MODE_PERIODS>=2 so each
mode visit has at least one measured period. Per mode the output has the number of used periods and frames, the
frame-time p50/p90 (mean of the per-period values weighted by frames), the draw-commit
and qworker CPU ms per frame, and the B-vs-A delta.
"""
import re
import sys

LINE = re.compile(r"\[ab\] mode=([AB]) period=(\d+) since_switch=(\d+) frames=(\d+) ft_p50=([\d.]+) ft_p90=([\d.]+) ft_mean=([\d.]+) commit_ms=([\d.]+) qworker_ms=([\d.]+)")
KEYS = ("ft_p50", "ft_p90", "ft_mean", "commit_ms", "qworker_ms")


def parse(stream):
    rows = []
    for text in stream:
        m = LINE.search(text)
        if m:
            rows.append(dict(mode=m.group(1), period=int(m.group(2)), since=int(m.group(3)), frames=int(m.group(4)),
                             **{k: float(v) for k, v in zip(KEYS, m.groups()[4:])}))
    return rows


def summarise(rows):
    kept = [r for r in rows if r["since"] >= 1]  # since_switch=0 is the first period after a switch: warm-up
    out = {}
    for mode in "AB":
        sel = [r for r in kept if r["mode"] == mode]
        n = sum(r["frames"] for r in sel)
        if not sel:
            continue
        out[mode] = dict(periods=len(sel), frames=n, **{k: sum(r[k] * r["frames"] for r in sel) / n for k in KEYS})
    return out


def main(argv):
    paths = argv[1:] or ["-"]
    rows = []
    for p in paths:
        rows += parse(sys.stdin if p == "-" else open(p, errors="replace"))
    if not rows:
        print("no [ab] mode= lines found")
        return 1
    res = summarise(rows)
    print("mode periods frames  ft_p50  ft_p90 ft_mean commit_ms qworker_ms   (first period after each switch excluded as warm-up)")
    for mode in "AB":
        r = res.get(mode)
        if r:
            print(f"{mode:>4} {r['periods']:7d} {r['frames']:6d} {r['ft_p50']:7.2f} {r['ft_p90']:7.2f} {r['ft_mean']:7.2f} {r['commit_ms']:9.2f} {r['qworker_ms']:10.2f}")
    if "A" in res and "B" in res:
        a, b = res["A"], res["B"]
        print("B-A  " + " " * 14 + "".join(f"{b[k] - a[k]:+8.2f}" for k in ("ft_p50", "ft_p90", "ft_mean")) + f"{b['commit_ms'] - a['commit_ms']:+10.2f}{b['qworker_ms'] - a['qworker_ms']:+11.2f}")
        pct = (b["ft_p50"] - a["ft_p50"]) / a["ft_p50"] * 100 if a["ft_p50"] else 0
        print(f"B vs A frame time p50: {pct:+.1f} %")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
