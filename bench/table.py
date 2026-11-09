# usage: table.py results.csv server... [--tls tlsserver...]
# markdown: (a) median rps per echo cell, (b) median server us per round-trip, (c) p99 and p99/p50,
# (d) connection churn. Ratios are vs tokio-epoll, with the min-max of the per-round ratio
# (round r of a server over round r of tokio, run minutes apart).
# With --tls: the same tables for the TLS cells (ratios vs tokio-rustls) and (h) the cost of TLS:
# server us per round-trip (or connection) of em-tls-X minus em-X in the same cell.
import csv, statistics, sys
from collections import defaultdict

args = sys.argv[2:]
servers = args[: args.index("--tls")] if "--tls" in args else args
tls_servers = args[args.index("--tls") + 1 :] if "--tls" in args else []
col = defaultdict(lambda: defaultdict(dict))  # field -> (mode, conns, size, server) -> round -> value
for r in csv.DictReader(open(sys.argv[1])):
    k = (r["mode"], int(r["conns"]), int(r["size"]), r["server"])
    for f in ("rps", "client_cpu", "server_cpu", "us_per_rt", "p50_us", "p99_us"):
        col[f][k][r["round"]] = float(r[f])
cells = sorted({k[:3] for k in col["rps"]})
echo = [c for c in cells if c[0] == "echo"]
churn = [c for c in cells if c[0] == "churn"]
echotls = [c for c in cells if c[0] == "echotls"]
churntls = [c for c in cells if c[0] == "churntls"]
med = lambda f, cl, srv: statistics.median(col[f][(*cl, srv)].values())


def ratio(f, cl, srv, fmt):
    base = "tokio-rustls" if cl[0].endswith("tls") else "tokio-epoll"
    m, v, b = med(f, cl, srv), col[f][(*cl, srv)], col[f][(*cl, base)]
    rs = [v[r] / b[r] for r in v if r in b]
    rng = f" [{min(rs):.2f}–{max(rs):.2f}]" if len(rs) > 1 and srv != base else ""
    return f"{fmt(m)} ({m / med(f, cl, base):.2f}x{rng})"


def tail(cl, srv):
    p99 = med("p99_us", cl, srv)
    return f"{p99:,.0f} ({p99 / med('p50_us', cl, srv):.1f})"


def table(title, first, rows, srvs=None):  # rows: (label, cell function of server)
    srvs = srvs or servers
    print(f"### {title}\n")
    print(f"| {first} | " + " | ".join(srvs) + " |")
    print("|---|" + "---|" * len(srvs))
    for label, fn in rows:
        print(f"| {label} | " + " | ".join(fn(v) for v in srvs) + " |")
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

def cost_row(tl, plain, backend):  # server us of em-tls-X minus em-X, per round (same round: run minutes apart)
    t, p = col["us_per_rt"][(*tl, f"em-tls-{backend}")], col["us_per_rt"][(*plain, f"em-{backend}")]
    d = [t[r] - p[r] for r in t if r in p]
    rest = statistics.median(p.values())
    c = statistics.median(d)
    rng = f" [{min(d):.2f}–{max(d):.2f}]" if len(d) > 1 else ""
    return f"{statistics.median(t.values()):.2f} | {rest:.2f} | {c:.2f}{rng} | {c / rest:.2f}x"


if tls_servers and (echotls or churntls):
    table("(e) TLS echo: round-trips/s, higher is better", "conns x size", [(lab(c), lambda v, c=c: ratio("rps", c, v, rps)) for c in echotls], tls_servers)
    table("(f) TLS echo: server CPU us per round-trip, lower is better", "conns x size", [(lab(c), lambda v, c=c: ratio("us_per_rt", c, v, us)) for c in echotls], tls_servers)
    table("(g) TLS: p99 latency us (p99/p50), lower is better", "conns x size", [(lab(c), lambda v, c=c: tail(c, v)) for c in echotls], tls_servers)
    for c in churntls:
        table(f"(g2) TLS churn: {c[1]} concurrent, each connect + handshake + one {c[2]} B round-trip + RST", "", [
            ("connections/s", lambda v: ratio("rps", c, v, rps)),
            ("server CPU us per connection", lambda v: ratio("us_per_rt", c, v, us)),
            ("p99 us incl. connect+handshake (p99/p50)", lambda v: tail(c, v)),
        ], tls_servers)
    print("### (h) cost of TLS: server CPU us per round-trip (churn: per connection), em-tls minus plain em, same cell\n")
    print("| cell | backend | em-tls us | em us (rest of the round-trip) | TLS cost us [lo–hi] | cost / rest |")
    print("|---|---|---|---|---|---|")
    for tl in echotls + churntls:
        plain = ("churn" if tl[0] == "churntls" else "echo", tl[1], tl[2])
        for b in ("uring", "epoll"):
            if plain in cells:
                print(f"| {tl[0][:-3]} {lab(tl)} | {b} | {cost_row(tl, plain, b)} |")
    print()
    print("kTLS is worth a look only if the cost is larger than the rest of the round-trip in the 4096 cell AND em has users that send files.\n")
print("Server pinned to 1 CPU, client to 3. (Nx [lo–hi]) = median ratio vs tokio-epoll [min–max of per-round ratios]; uring servers first, then epoll.")
print("Two servers differ in a cell only if their [lo–hi] ranges do not overlap; otherwise read it as a tie.")
name = lambda cl: f"{cl[0]} c={cl[1]} s={cl[2]}"
srvs_of = lambda c: tls_servers if c[0].endswith("tls") else servers
cli = [f"{v} {name(c)}" for c in cells for v in srvs_of(c) if med("client_cpu", c, v) >= 0.9]
srv = [f"{v} {name(c)}" for c in cells for v in srvs_of(c) if med("server_cpu", c, v) < 0.9]
if cli:
    print(f"\nWARNING: client CPU >= 90% in {len(cli)} cells (ratios compressed): " + "; ".join(cli))
if srv:
    print(f"\nNOTE: server CPU < 90% in {len(srv)} cells (server not saturated, rps not meaningful there; us/rt still is): " + "; ".join(srv))
