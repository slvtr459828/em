# usage: table.py results.csv server...   -> markdown: median rps per cell, ratio vs tokio-epoll
import csv, statistics, sys
from collections import defaultdict

servers = sys.argv[2:]
rps, cpu = defaultdict(list), defaultdict(list)
for r in csv.DictReader(open(sys.argv[1])):
    k = (int(r["conns"]), int(r["size"]), r["server"])
    rps[k].append(float(r["rps"]))
    cpu[k].append(float(r["client_cpu"]))
cells = sorted({k[:2] for k in rps})

print("| conns | size | " + " | ".join(servers) + " |")
print("|---|---|" + "---|" * len(servers))
warn = []
for c, s in cells:
    base = statistics.median(rps[(c, s, "tokio-epoll")])
    row = []
    for srv in servers:
        m = statistics.median(rps[(c, s, srv)])
        row.append(f"{m:,.0f} ({m / base:.2f}x)")
        if statistics.median(cpu[(c, s, srv)]) >= 0.9:
            warn.append(f"{srv} c={c} s={s}")
    print(f"| {c} | {s} | " + " | ".join(row) + " |")
print("\nrps = median round-trips/s; (Nx) = ratio vs tokio-epoll. uring servers first, then epoll.")
if warn:
    print(f"\nWARNING: client CPU >= 90% in {len(warn)} runs (ratios compressed): " + "; ".join(warn))
