# usage: table.py results.csv server...
# markdown: (a) median rps per cell, (b) median server us per round-trip; ratio vs tokio-epoll in both
import csv, statistics, sys
from collections import defaultdict

servers = sys.argv[2:]
col = defaultdict(lambda: defaultdict(list))
for r in csv.DictReader(open(sys.argv[1])):
    k = (int(r["conns"]), int(r["size"]), r["server"])
    for f in ("rps", "client_cpu", "server_cpu", "us_per_rt"):
        col[f][k].append(float(r[f]))
cells = sorted({k[:2] for k in col["rps"]})
med = lambda f, c, s, srv: statistics.median(col[f][(c, s, srv)])


def table(title, f, fmt):
    print(f"### {title}\n")
    print("| conns | size | " + " | ".join(servers) + " |")
    print("|---|---|" + "---|" * len(servers))
    for c, s in cells:
        base = med(f, c, s, "tokio-epoll")
        print(f"| {c} | {s} | " + " | ".join(f"{fmt(m)} ({m / base:.2f}x)" for m in (med(f, c, s, v) for v in servers)) + " |")
    print()


table("(a) round-trips/s, higher is better", "rps", lambda m: f"{m:,.0f}")
table("(b) server CPU us per round-trip, lower is better", "us_per_rt", lambda m: f"{m:.2f}")
print("Server pinned to 1 CPU, client to 3. (Nx) = ratio vs tokio-epoll; uring servers first, then epoll.")
cli = [f"{v} c={c} s={s}" for c, s in cells for v in servers if med("client_cpu", c, s, v) >= 0.9]
srv = [f"{v} c={c} s={s}" for c, s in cells for v in servers if med("server_cpu", c, s, v) < 0.9]
if cli:
    print(f"\nWARNING: client CPU >= 90% in {len(cli)} cells (ratios compressed): " + "; ".join(cli))
if srv:
    print(f"\nNOTE: server CPU < 90% in {len(srv)} cells (server not saturated, rps not meaningful there; us/rt still is): " + "; ".join(srv))
