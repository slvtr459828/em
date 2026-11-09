# usage: table.py results.csv server...
# markdown: (a) median rps per echo cell, (b) median server us per round-trip, (c) p99 and p99/p50,
# (d) connection churn. Ratios are vs tokio-epoll, with the min-max of the per-round ratio
# (round r of a server over round r of tokio, run minutes apart).
import csv, statistics, sys
from collections import defaultdict

servers = sys.argv[2:]
col = defaultdict(lambda: defaultdict(dict))  # field -> (mode, conns, size, server) -> round -> value
for r in csv.DictReader(open(sys.argv[1])):
    k = (r["mode"], int(r["conns"]), int(r["size"]), r["server"])
    for f in ("rps", "client_cpu", "server_cpu", "us_per_rt", "p50_us", "p99_us"):
        col[f][k][r["round"]] = float(r[f])
cells = sorted({k[:3] for k in col["rps"]})
echo = [c for c in cells if c[0] == "echo"]
churn = [c for c in cells if c[0] == "churn"]
med = lambda f, cl, srv: statistics.median(col[f][(*cl, srv)].values())


def ratio(f, cl, srv, fmt):
    m, v, b = med(f, cl, srv), col[f][(*cl, srv)], col[f][(*cl, "tokio-epoll")]
    rs = [v[r] / b[r] for r in v if r in b]
    rng = f" [{min(rs):.2f}–{max(rs):.2f}]" if len(rs) > 1 and srv != "tokio-epoll" else ""
    return f"{fmt(m)} ({m / med(f, cl, 'tokio-epoll'):.2f}x{rng})"


def tail(cl, srv):
    p99 = med("p99_us", cl, srv)
    return f"{p99:,.0f} ({p99 / med('p50_us', cl, srv):.1f})"


def table(title, first, rows):  # rows: (label, cell function of server)
    print(f"### {title}\n")
    print(f"| {first} | " + " | ".join(servers) + " |")
    print("|---|" + "---|" * len(servers))
    for label, fn in rows:
        print(f"| {label} | " + " | ".join(fn(v) for v in servers) + " |")
    print()


rps = lambda m: f"{m:,.0f}"
us = lambda m: f"{m:.2f}"
lab = lambda cl: f"{cl[1]} x {cl[2]}"
table("(a) round-trips/s, higher is better", "conns x size", [(lab(c), lambda v, c=c: ratio("rps", c, v, rps)) for c in echo])
table("(b) server CPU us per round-trip, lower is better", "conns x size", [(lab(c), lambda v, c=c: ratio("us_per_rt", c, v, us)) for c in echo])
table("(c) p99 latency us (p99/p50), lower is better", "conns x size", [(lab(c), lambda v, c=c: tail(c, v)) for c in echo])
print("Closed loop at saturation: the mean is queueing (~conns/rps); p99/p50 shows how evenly connections are served.\n")
for c in churn:
    table(f"(d) churn: {c[1]} concurrent, each connect + one {c[2]} B round-trip + RST", "", [
        ("connections/s", lambda v: ratio("rps", c, v, rps)),
        ("server CPU us per connection", lambda v: ratio("us_per_rt", c, v, us)),
        ("p99 us incl. connect (p99/p50)", lambda v: tail(c, v)),
    ])
print("Server pinned to 1 CPU, client to 3. (Nx [lo–hi]) = median ratio vs tokio-epoll [min–max of per-round ratios]; uring servers first, then epoll.")
print("Two servers differ in a cell only if their [lo–hi] ranges do not overlap; otherwise read it as a tie.")
name = lambda cl: f"{cl[0]} c={cl[1]} s={cl[2]}"
cli = [f"{v} {name(c)}" for c in cells for v in servers if med("client_cpu", c, v) >= 0.9]
srv = [f"{v} {name(c)}" for c in cells for v in servers if med("server_cpu", c, v) < 0.9]
if cli:
    print(f"\nWARNING: client CPU >= 90% in {len(cli)} cells (ratios compressed): " + "; ".join(cli))
if srv:
    print(f"\nNOTE: server CPU < 90% in {len(srv)} cells (server not saturated, rps not meaningful there; us/rt still is): " + "; ".join(srv))
