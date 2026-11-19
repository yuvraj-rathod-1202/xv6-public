#!/usr/bin/env python3
"""Render an xv6 scheduling trace as an SVG Gantt chart.

Inside xv6, record a trace as CSV:
    $ schedtrace -c schedbench cpu fifo
and save the console output to a file (the test harness does this). Then:
    python3 tools/gantt.py console.log -o gantt.svg --title "FIFO, cpu workload"

Each row is a process; bars show when it ran, coloured by CPU. Dashed
vertical lines mark scheduler switches. The right-hand column gives each
process's number of runs and average run length (its effective time slice).
Uses only the Python standard library.
"""
import argparse
import html
import sys

CPU_COLORS = ["#4e79a7", "#f28e2b", "#59a14f", "#e15759",
              "#76b7b2", "#edc948", "#b07aa1", "#9c755f"]
POLICY = {0: "RR", 1: "FIFO", 2: "CFS"}
TR_RUN, TR_STOP, TR_SCHED = 1, 2, 3


def parse(path, index=0):
    """Return the events of the index-th TRACE-BEGIN..TRACE-END block."""
    blocks, cur = [], None
    with open(path, errors="replace") as f:
        for line in f:
            line = line.strip()
            if line.startswith("TRACE-BEGIN"):
                cur = []
            elif line.startswith("TRACE-END"):
                if cur is not None:
                    blocks.append(cur)
                cur = None
            elif cur is not None and line.startswith("E,"):
                parts = line.split(",", 6)
                if len(parts) < 7:
                    continue
                _, t, cpu, pid, typ, arg, name = parts
                cur.append(dict(t=int(t), cpu=int(cpu), pid=int(pid),
                                type=int(typ), arg=int(arg), name=name))
    if index >= len(blocks):
        sys.exit(f"gantt.py: {path} has {len(blocks)} trace(s); no #{index}")
    return blocks[index]


def intervals(events):
    """Turn RUN/STOP events into per-process run intervals."""
    end = events[-1]["t"] if events else 0
    running = {}            # cpu -> (pid, start)
    rows = {}               # pid -> dict(name, spans[(a, b, cpu)], runs)
    switches = []           # (t, policy)
    for e in events:
        if e["type"] == TR_SCHED:
            switches.append((e["t"], e["arg"]))
            continue
        row = rows.setdefault(e["pid"], dict(name=e["name"], spans=[], runs=0))
        if e["type"] == TR_RUN:
            running[e["cpu"]] = (e["pid"], e["t"])
            row["runs"] += 1
        elif e["type"] == TR_STOP:
            r = running.pop(e["cpu"], None)
            if r and r[0] == e["pid"] and e["t"] > r[1]:
                row["spans"].append((r[1], e["t"], e["cpu"]))
    for cpu, (pid, start) in running.items():
        if end > start:
            rows[pid]["spans"].append((start, end, cpu))
    rows = {p: r for p, r in rows.items() if r["runs"]}
    return rows, switches, end


def render(events, title):
    rows, switches, end = intervals(events)
    end_ticks = max(end / 100.0, 0.01)
    left, right, top, row_h = 170, 150, 60, 24
    width = 1100
    plot_w = width - left - right
    height = top + row_h * len(rows) + 50
    x = lambda t: left + plot_w * (t / 100.0) / end_ticks

    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" '
           f'height="{height}" font-family="monospace" font-size="12">',
           f'<rect width="{width}" height="{height}" fill="white"/>',
           f'<text x="{left}" y="22" font-size="15" font-weight="bold">'
           f'{html.escape(title)}</text>']

    # time axis
    step = 1
    for s in (1, 2, 5, 10, 20, 50, 100, 200, 500, 1000):
        if end_ticks / s <= 12:
            step = s
            break
    axis_y = top + row_h * len(rows) + 8
    t = 0
    while t <= end_ticks:
        px = left + plot_w * t / end_ticks
        out.append(f'<line x1="{px:.1f}" y1="{top - 6}" x2="{px:.1f}" '
                   f'y2="{axis_y}" stroke="#ddd"/>')
        out.append(f'<text x="{px:.1f}" y="{axis_y + 14}" '
                   f'text-anchor="middle">{t}</text>')
        t += step
    out.append(f'<text x="{left + plot_w / 2}" y="{axis_y + 32}" '
               f'text-anchor="middle">time (ticks)</text>')

    for i, (pid, r) in enumerate(sorted(rows.items())):
        y = top + i * row_h
        out.append(f'<text x="8" y="{y + 16}">{pid:>3} '
                   f'{html.escape(r["name"][:14])}</text>')
        for a, b, cpu in r["spans"]:
            out.append(f'<rect x="{x(a):.2f}" y="{y + 4}" '
                       f'width="{max(x(b) - x(a), 0.6):.2f}" height="{row_h - 8}" '
                       f'fill="{CPU_COLORS[cpu % len(CPU_COLORS)]}"/>')
        total = sum(b - a for a, b, _ in r["spans"]) / 100.0
        avg = total / r["runs"] if r["runs"] else 0
        out.append(f'<text x="{left + plot_w + 8}" y="{y + 16}">'
                   f'{r["runs"]:>3} runs, avg {avg:.1f}</text>')

    for t, pol in switches:
        px = x(t)
        out.append(f'<line x1="{px:.1f}" y1="{top - 14}" x2="{px:.1f}" '
                   f'y2="{axis_y}" stroke="black" stroke-dasharray="4,3"/>')
        out.append(f'<text x="{px + 3:.1f}" y="{top - 16}">'
                   f'{POLICY.get(pol, pol)}</text>')

    cpus = sorted({c for r in rows.values() for _, _, c in r["spans"]})
    lx = left
    for c in cpus:
        out.append(f'<rect x="{lx}" y="32" width="12" height="12" '
                   f'fill="{CPU_COLORS[c % len(CPU_COLORS)]}"/>')
        out.append(f'<text x="{lx + 16}" y="43">cpu{c}</text>')
        lx += 70
    out.append("</svg>")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", help="console output containing TRACE-BEGIN/END")
    ap.add_argument("-o", "--output", required=True, help="SVG file to write")
    ap.add_argument("--index", type=int, default=0,
                    help="which trace in the log (default: first)")
    ap.add_argument("--title", default="xv6 scheduling trace")
    args = ap.parse_args()
    events = parse(args.log, args.index)
    with open(args.output, "w") as f:
        f.write(render(events, args.title))
    print(f"gantt.py: wrote {args.output} ({len(events)} events)")


if __name__ == "__main__":
    main()
