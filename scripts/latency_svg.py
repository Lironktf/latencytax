#!/usr/bin/env python3
"""Renders the tick to trade budget as a stacked bar figure, light and dark.

Reads the ladder that build/ticktotrade --all prints, so the picture cannot
drift from the measurement: rerun the benchmark, rerun this, and the figure is
whatever the machine just said.

Two SVGs come out, one per colour scheme, because GitHub serves an SVG in a
README as an image and an image cannot follow a media query. The README pairs
them with <picture>.
"""
import argparse
import os
import re
import sys

# The stages, in the order they happen. Colours are categorical slots 1 to 4 of
# the validated default palette, assigned in fixed order and never cycled.
STAGES = [
    ("kernel in", "send to the packet being readable"),
    ("engine", "decode, book, decide"),
    ("kernel out", "the send call itself"),
    ("reply in flight", "after send returns, until the reply is in hand"),
]

# The tool prints a sentence per transport; a chart needs a label. Anything not
# listed is used as it comes, so a new transport shows up rather than vanishing.
SHORT = {
    "udp on loopback": "UDP loopback",
    "udp on loopback, connected sockets, recvmmsg": "UDP, connected + recvmmsg",
    "af_unix datagrams": "AF_UNIX datagrams",
    "spsc ring, no kernel in the path": "SPSC ring, no kernel",
}

THEME = {
    "light": dict(
        surface="#fcfcfb", panel="#f4f4f2", ink="#0b0b0b", ink2="#52514e",
        ink3="#7a7974", grid="#e3e3df",
        series=["#2a78d6", "#eb6834", "#1baf7a", "#eda100"],
    ),
    "dark": dict(
        surface="#1a1a19", panel="#232322", ink="#ffffff", ink2="#c3c2b7",
        ink3="#8f8e86", grid="#343432",
        series=["#3987e5", "#d95926", "#199e70", "#c98500"],
    ),
}


def parse_ladder(path):
    """Pulls the medians table out of the ticktotrade output."""
    rows = []
    with open(path) as fh:
        text = fh.read()
    started = False
    for line in text.splitlines():
        if line.startswith("transport") and "TOTAL" in line:
            started = True
            continue
        if not started:
            continue
        if not line.strip():
            if rows:
                break
            continue
        m = re.match(r"^(.*?)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s*$",
                     line)
        if not m:
            if rows:
                break
            continue
        name = m.group(1).strip()
        vals = [int(m.group(i)) for i in range(2, 9)]
        kin, decode, book, decide, kout, engine, total = vals
        # The stages are measured separately and the total is measured end to
        # end, so the difference is the part of the return leg after the send
        # call returns. It is shown rather than absorbed into a neighbour.
        inflight = max(0, total - (kin + engine + kout))
        rows.append(dict(name=name, total=total,
                         parts=[kin, engine, kout, inflight],
                         detail=dict(decode=decode, book=book, decide=decide)))
    return rows


def esc(s):
    return (s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))


def fmt_ns(v):
    if v >= 1000:
        return f"{v/1000:.1f} µs"
    return f"{v} ns"


def rounded_right(x, y, w, h, r):
    """A bar segment with only its outer end rounded, anchored to the baseline."""
    r = min(r, w, h / 2)
    if r <= 0.5:
        return f'<rect x="{x:.1f}" y="{y:.1f}" width="{max(w,0.6):.1f}" height="{h:.1f}"/>'
    return (f'<path d="M{x:.1f},{y:.1f} H{x+w-r:.1f} A{r:.1f},{r:.1f} 0 0 1 {x+w:.1f},{y+r:.1f}'
            f' V{y+h-r:.1f} A{r:.1f},{r:.1f} 0 0 1 {x+w-r:.1f},{y+h:.1f} H{x:.1f} Z"/>')


def panel(rows, x0, y0, width, bar_h, gap, xmax, t, label_w, title, sub):
    """One stacked bar panel. Returns svg fragment and its height."""
    out = []
    plot_x = x0 + label_w
    plot_w = width - label_w - 86           # room for the total at the right
    out.append(f'<text x="{x0}" y="{y0}" class="h2">{esc(title)}</text>')
    out.append(f'<text x="{x0}" y="{y0+15}" class="cap">{esc(sub)}</text>')
    top = y0 + 32

    # Gridlines, recessive, behind the marks.
    step = 2000 if xmax > 4000 else 200
    v = 0
    while v <= xmax:
        gx = plot_x + plot_w * v / xmax
        out.append(f'<line x1="{gx:.1f}" y1="{top}" x2="{gx:.1f}" '
                   f'y2="{top + len(rows)*(bar_h+gap)+4:.1f}" class="grid"/>')
        out.append(f'<text x="{gx:.1f}" y="{top + len(rows)*(bar_h+gap)+18:.1f}" '
                   f'class="ax" text-anchor="middle">{fmt_ns(v) if v else "0"}</text>')
        v += step

    for i, r in enumerate(rows):
        by = top + i * (bar_h + gap)
        name = SHORT.get(r["name"], r["name"])
        out.append(f'<text x="{plot_x-10}" y="{by+bar_h/2+4:.1f}" class="lab" '
                   f'text-anchor="end">{esc(name)}</text>')
        cx = plot_x
        for si, val in enumerate(r["parts"]):
            w = plot_w * val / xmax
            if w <= 0:
                continue
            last = si == len(r["parts"]) - 1 or sum(r["parts"][si+1:]) == 0
            shape = rounded_right(cx, by, w, bar_h, 4 if last else 0)
            out.append(f'<g fill="{t["series"][si]}"><title>{esc(STAGES[si][0])}: '
                       f'{val} ns</title>{shape}</g>')
            # A direct label only where it fits, never a number on every segment.
            if w > 52:
                out.append(f'<text x="{cx+w/2:.1f}" y="{by+bar_h/2+4:.1f}" class="seg" '
                           f'text-anchor="middle">{fmt_ns(val)}</text>')
            cx += w + 2                      # 2px surface gap between fills
        out.append(f'<text x="{plot_x+plot_w+10:.1f}" y="{by+bar_h/2+4:.1f}" '
                   f'class="tot">{fmt_ns(r["total"])}</text>')
    return "\n".join(out), top + len(rows) * (bar_h + gap) + 30


def render(rows, mode, out_path):
    t = THEME[mode]
    # Computed, not asserted, so the sentence cannot drift from the picture.
    first, last = rows[0], rows[-1]
    subtitle = (f"the engine is {100.0*first['parts'][1]/first['total']:.0f}% of a "
                f"{SHORT.get(first['name'], first['name'])} path and "
                f"{100.0*last['parts'][1]/last['total']:.0f}% of one with no kernel in it")
    W = 900
    label_w = 176
    ring = [r for r in rows if "ring" in r["name"]]
    top_rows = rows
    xmax_a = max(r["total"] for r in rows)
    xmax_a = int((xmax_a * 1.02) // 1000 + 1) * 1000
    xmax_b = 1200 if ring else 1000

    body = []
    frag_a, next_y = panel(top_rows, 40, 74, W - 80, 30, 14, xmax_a, t, label_w,
                           "the whole path", "median nanoseconds, one packet in and the "
                           "order it provoked out")
    body.append(frag_a)
    if ring:
        frag_b, next_y = panel(ring, 40, next_y + 30, W - 80, 30, 14, xmax_b, t, label_w,
                               "the same bottom row, to scale",
                               "the kernel is gone, so the engine is a third of what is left")
        body.append(frag_b)

    # Legend, always present for more than one series.
    ly = next_y + 16
    lx = 40 + label_w
    leg = []
    for si, (name, _) in enumerate(STAGES):
        leg.append(f'<rect x="{lx}" y="{ly-9}" width="11" height="11" rx="2.5" '
                   f'fill="{t["series"][si]}"/>')
        leg.append(f'<text x="{lx+17}" y="{ly}" class="leg">{esc(name)}</text>')
        lx += 19 + 8.0 * len(name) + 20
    body.append("\n".join(leg))

    H = int(ly + 34)
    css = f"""
    text {{ font-family: ui-sans-serif, -apple-system, "Segoe UI", Roboto, Helvetica, sans-serif; }}
    .h1 {{ font-size: 19px; font-weight: 640; fill: {t['ink']}; }}
    .h2 {{ font-size: 13.5px; font-weight: 600; fill: {t['ink']}; }}
    .cap {{ font-size: 11.5px; fill: {t['ink3']}; }}
    .lab {{ font-size: 12px; fill: {t['ink2']}; }}
    .ax  {{ font-size: 10.5px; fill: {t['ink3']}; }}
    .leg {{ font-size: 11.5px; fill: {t['ink2']}; }}
    .tot {{ font-size: 12.5px; font-weight: 640; fill: {t['ink']}; }}
    .seg {{ font-size: 10.5px; fill: {t['surface']}; font-weight: 600; }}
    .grid {{ stroke: {t['grid']}; stroke-width: 1; }}
    """
    svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
           f'viewBox="0 0 {W} {H}" role="img" '
           f'aria-label="Tick to trade latency budget by transport">',
           f'<style>{css}</style>',
           f'<rect width="{W}" height="{H}" fill="{t["surface"]}"/>',
           f'<text x="40" y="36" class="h1">Tick to trade: where the time goes</text>',
           f'<text x="40" y="55" class="cap">{esc(subtitle)}</text>']
    svg += body
    svg.append("</svg>")
    with open(out_path, "w") as fh:
        fh.write("\n".join(svg) + "\n")
    return out_path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", default="results/ticktotrade.txt")
    ap.add_argument("--outdir", default="docs")
    args = ap.parse_args()
    rows = parse_ladder(args.input)
    if not rows:
        sys.exit(f"no ladder table found in {args.input}")
    os.makedirs(args.outdir, exist_ok=True)
    for mode, name in (("light", "latency_budget.svg"), ("dark", "latency_budget_dark.svg")):
        p = render(rows, mode, os.path.join(args.outdir, name))
        print(f"wrote {p}")
    print("\nthe same numbers, as a table:")
    print(f"  {'transport':<40} {'kernel in':>10} {'engine':>8} {'kernel out':>11} "
          f"{'in flight':>10} {'total':>9}")
    for r in rows:
        print(f"  {r['name']:<40} {r['parts'][0]:>10} {r['parts'][1]:>8} "
              f"{r['parts'][2]:>11} {r['parts'][3]:>10} {r['total']:>9}")


if __name__ == "__main__":
    main()
