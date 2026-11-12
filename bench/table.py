# usage: table.py results.csv server... [--tls tlsserver...] [--ref tlsserver...]
#        table.py --rss rss.csv [--ref server...]     (the memory table of bench/rss_tls.sh)
# --ref: reference servers (go-tls-epoll: AES-128, 1 ticket, plans/bench-cpp.md). Printed in the tables, but never
# judged and never part of a check (negotiated set, resumption, missing rows).
# markdown: (a) median rps per echo cell, (b) median server us per round-trip, (c) p99 and p99/p50,
# (d) connection churn. Ratios are vs tokio-epoll, with the min-max of the per-round ratio
# (round r of a server over round r of tokio, run minutes apart).
# With --tls: the same tables for the TLS cells (ratios vs tokio-rustls) and (h) the cost of TLS:
# server us per round-trip (or connection) of em-tls-X minus em-X in the same cell.
# TLS churn comes in two modes: churntls (client resumption off: every connection a full handshake) and
# churntlsres (client keeps a session store; "resumed" is the measured fraction of resumed handshakes
# and must be ~100% for every server or the cell is not a resumption cell).
# With --tls: the "lost cells" table (plans/tls-win.md 1.3): em-tls-uring against every *-uring server,
# em-tls-epoll against tokio-rustls, tokio-rustls-ct and every *-epoll server; ratios per round (round r of
# em over round r of the rival); THUA = median worse than 5% AND every round on the worse side, THẮNG
# symmetric, else NGANG. Needs ROUNDS >= 9 (fewer: a warning instead of verdicts). Judged axes: server CPU
# per round-trip (connection), p99/p50. rps and p999 are only printed. Exit status 1 when a check fails:
# a TLS server without a row in a TLS cell, more than one negotiated (version suite group cert), a
# resumption ratio < 0.95 in the churn cell with resumption.
# (j2): the same rule for the plain cells: em-uring / em-epoll against the other plain servers of the same backend.
# ONLY=http (plans/http.md): the CSV has only http* modes and the servers are em-http-* and nginx-epoll,
# haproxy-epoll, h2o-epoll. Tables (a)-(c) with ratios vs nginx-epoll and (j3): em-http-uring and em-http-epoll
# each against EVERY other server (the rivals are epoll-only, so the backend does not pair them), same rule.
import csv, re, statistics, sys
from collections import defaultdict

REF = set()  # --ref servers
REF_NOTE = {"go-tls-epoll": "AES-128, 1 ticket"}


def opponents(em, srvs):  # the servers em[-tls]-<backend> is judged against: same backend (emrs-* is not a rival)
    srvs = [s for s in srvs if not s.startswith(("em-", "emrs-")) and s not in REF]
    if em.endswith("-uring"):
        return [s for s in srvs if s.endswith("-uring")]
    return [s for s in srvs if s.endswith("-epoll") or (em.startswith("em-tls-") and s.startswith("tokio-rustls"))]


def verdict(rs):  # rs: per-round em / rival of a lower-is-better number
    m = statistics.median(rs)
    if m > 1.05 and all(x > 1 for x in rs):
        return "THUA"
    if m < 0.95 and all(x < 1 for x in rs):
        return "THẮNG"
    return "NGANG"


def rss_report(path):  # bench/rss_tls.sh: KiB per idle connection per repetition
    v = defaultdict(list)
    for r in csv.DictReader(open(path)):
        v[r["server"]].append(float(r["kib_per_conn"]))
    print("### (i) memory per idle TLS connection, KiB (median [min–max] of the repetitions)\n")
    print("Slope (RSS at 1000 connections - RSS at 500) / 500; each connection did a handshake and one 16 KiB echo, then idled.\n")
    print("| server | KiB per connection | em-tls (same backend) vs this server |")
    print("|---|---|---|")
    for srv, xs in v.items():
        lo, hi, m = min(xs), max(xs), statistics.median(xs)
        vs = []
        for em in ("em-tls-uring", "em-tls-epoll"):
            if em in v and srv in opponents(em, list(v)):
                e = v[em]
                em_m = statistics.median(e)
                w = "THUA" if em_m > 1.05 * m and min(e) > hi else "THẮNG" if em_m < 0.95 * m and max(e) < lo else "NGANG"
                vs.append(f"{em}: {w} ({em_m / m:.2f}x)")
        print(f"| {srv} | {m:.1f} [{lo:.1f}–{hi:.1f}] | {'; '.join(vs)} |")
    print("\nTHUA = em more than 5% above and the ranges of the repetitions do not overlap; THẮNG the mirror.")


if "--ref" in sys.argv:
    REF = set(sys.argv[sys.argv.index("--ref") + 1 :])
    del sys.argv[sys.argv.index("--ref") :]
if sys.argv[1] == "--rss":
    rss_report(sys.argv[2])
    sys.exit(0)
args = sys.argv[2:]
servers = args[: args.index("--tls")] if "--tls" in args else args
tls_servers = args[args.index("--tls") + 1 :] if "--tls" in args else []
judged_tls = [v for v in tls_servers if v not in REF]
col = defaultdict(lambda: defaultdict(dict))  # field -> (mode, conns, size, server) -> round -> value
for r in csv.DictReader(open(sys.argv[1])):
    k = (r["mode"], int(r["conns"]), int(r["size"]), r["server"])
    for f in ("rps", "client_cpu", "server_cpu", "us_per_rt", "p50_us", "p99_us", "p999_us"):
        if f in r:
            col[f][k][r["round"]] = float(r[f])
    if r.get("resumed", "-") != "-":
        col["resumed"][k][r["round"]] = float(r["resumed"])
cells = sorted({k[:3] for k in col["rps"]})
echo = [c for c in cells if c[0] == "echo"]
churn = [c for c in cells if c[0] == "churn"]
echotls = [c for c in cells if c[0] == "echotls"]
churntls = [c for c in cells if c[0] in ("churntls", "churntlsres")]
httpc = [c for c in cells if c[0].startswith("http")]
HTTP_LABEL = {"http": "GET / keep-alive", "httppipe": "GET / pipeline 16", "http4k": "GET /4k keep-alive", "httpchurn": "churn GET / (connect + 1 request + RST)"}
hlab = lambda c: f"{HTTP_LABEL[c[0]]}, {c[1]} conns"
mode_name = {"churntls": "full handshake, client resumption off", "churntlsres": "resumption on, session store in the client"}
med = lambda f, cl, srv: statistics.median(col[f][(*cl, srv)].values())


def ratio(f, cl, srv, fmt):
    base = "tokio-rustls" if "tls" in cl[0] else "nginx-epoll" if cl[0].startswith("http") else "tokio-epoll"
    m, v, b = med(f, cl, srv), col[f][(*cl, srv)], col[f][(*cl, base)]
    rs = [v[r] / b[r] for r in v if r in b]
    rng = f" [{min(rs):.2f}–{max(rs):.2f}]" if len(rs) > 1 and srv != base else ""
    return f"{fmt(m)} ({m / med(f, cl, base):.2f}x{rng})"


def tail(cl, srv):  # p99 (p99/p50) / p999: p999 is only printed, never judged
    p99 = med("p99_us", cl, srv)
    p999 = f" / {med('p999_us', cl, srv):,.0f}" if (*cl, srv) in col["p999_us"] else ""
    return f"{p99:,.0f} ({p99 / med('p50_us', cl, srv):.1f}){p999}"


def cell(fn, v):  # a server without a row in this cell (a reference server has no churntlsres): "-"
    try:
        return fn(v)
    except (KeyError, statistics.StatisticsError):
        return "-"


def table(title, first, rows, srvs=None):  # rows: (label, cell function of server)
    srvs = srvs or servers
    print(f"### {title}\n")
    print(f"| {first} | " + " | ".join(f"{v} (ref: {REF_NOTE.get(v, 'reference')})" if v in REF else v for v in srvs) + " |")
    print("|---|" + "---|" * len(srvs))
    for label, fn in rows:
        print(f"| {label} | " + " | ".join(cell(fn, v) for v in srvs) + " |")
    print()


rps = lambda m: f"{m:,.0f}"
us = lambda m: f"{m:.2f}"
lab = lambda cl: f"{cl[1]} x {cl[2]}"
if echo:
    table("(a) round-trips/s, higher is better", "conns x size", [(lab(c), lambda v, c=c: ratio("rps", c, v, rps)) for c in echo])
    table("(b) server CPU us per round-trip, lower is better", "conns x size", [(lab(c), lambda v, c=c: ratio("us_per_rt", c, v, us)) for c in echo])
    table("(c) p99 latency us (p99/p50) / p999, lower is better", "conns x size", [(lab(c), lambda v, c=c: tail(c, v)) for c in echo])
    print("Closed loop at saturation: the mean is queueing (~conns/rps); p99/p50 shows how evenly connections are served.\n")
for c in churn:
    table(f"(d) churn: {c[1]} concurrent, each connect + one {c[2]} B round-trip + RST", "", [
        ("connections/s", lambda v: ratio("rps", c, v, rps)),
        ("server CPU us per connection", lambda v: ratio("us_per_rt", c, v, us)),
        ("p99 us incl. connect (p99/p50) / p999", lambda v: tail(c, v)),
    ])

if httpc:
    table("(a) HTTP: requests/s (churn: connections/s), higher is better", "cell", [(hlab(c), lambda v, c=c: ratio("rps", c, v, rps)) for c in httpc])
    table("(b) HTTP: server CPU us per request (churn: per connection), lower is better", "cell", [(hlab(c), lambda v, c=c: ratio("us_per_rt", c, v, us)) for c in httpc])
    table("(c) HTTP: p99 latency us (p99/p50) / p999, lower is better (pipeline: latency of the batch of 16)", "cell", [(hlab(c), lambda v, c=c: tail(c, v)) for c in httpc])
    try:  # bytes of the first response (header + body) per server and path, as the client counted them
        seen = sorted({l.strip() for l in open(sys.argv[1].rsplit("/", 1)[0] + "/http_info.txt") if l.strip()})
        print("Response bytes (header + body), as the client counted them:\n" + "".join(f"- {l}\n" for l in seen))
    except FileNotFoundError:
        pass


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
    table("(g) TLS: p99 latency us (p99/p50) / p999, lower is better", "conns x size", [(lab(c), lambda v, c=c: tail(c, v)) for c in echotls], tls_servers)
    for c in churntls:
        table(f"(g2) TLS churn, {mode_name[c[0]]} [{c[0]}]: {c[1]} concurrent, each connect + handshake + one {c[2]} B round-trip + RST", "", [
            ("resumed handshakes", lambda v: f"{100 * statistics.median(col['resumed'][(*c, v)].values()):.1f}%"),
            ("connections/s", lambda v: ratio("rps", c, v, rps)),
            ("server CPU us per connection", lambda v: ratio("us_per_rt", c, v, us)),
            ("p99 us incl. connect+handshake (p99/p50) / p999", lambda v: tail(c, v)),
        ], tls_servers)
    print("### (h) cost of TLS: server CPU us per round-trip (churn: per connection), em-tls minus plain em, same cell\n")
    print("| cell | backend | em-tls us | em us (rest of the round-trip) | TLS cost us [lo–hi] | cost / rest |")
    print("|---|---|---|---|---|---|")
    for tl in echotls + churntls:
        plain = ("churn" if tl[0].startswith("churn") else "echo", tl[1], tl[2])
        for b in ("uring", "epoll"):
            if plain in cells:
                print(f"| {tl[0].replace('tls', ' tls ').strip()} {lab(tl)} | {b} | {cost_row(tl, plain, b)} |")
    print()
    print("kTLS is worth a look only if the cost is larger than the rest of the round-trip in the 4096 cell AND em has users that send files.\n")
info = sys.argv[1].rsplit("/", 1)[0] + "/tls_info.txt" if "/" in sys.argv[1] else "tls_info.txt"
try:  # what the client saw negotiated: version suite group cert-key, per server (full=/resumed= counts are summed away)
    seen = sorted({re.sub(r" full=\d+ resumed=\d+", "", l.strip()) for l in open(info) if l.strip()})
    if seen:
        print("Negotiated (as seen by the client):\n" + "".join(f"- {l}\n" for l in seen))
except FileNotFoundError:
    pass
print(f"Server pinned to 1 CPU, client to 3. (Nx [lo–hi]) = median ratio vs {'nginx-epoll' if httpc else 'tokio-epoll'} [min–max of per-round ratios]; uring servers first, then epoll.")
print("Two servers differ in a cell only if their [lo–hi] ranges do not overlap; otherwise read it as a tie.")
name = lambda cl: f"{cl[0]} c={cl[1]} s={cl[2]}"
srvs_of = lambda c: tls_servers if "tls" in c[0] else servers
ran = [(c, v) for c in cells for v in srvs_of(c) if col["client_cpu"].get((*c, v))]  # skip cells a server did not run
cli = [f"{v} {name(c)}" for c, v in ran if med("client_cpu", c, v) >= 0.9]
srv = [f"{v} {name(c)}" for c, v in ran if med("server_cpu", c, v) < 0.9]
if cli:
    print(f"\nWARNING: client CPU >= 90% in {len(cli)} cells (ratios compressed): " + "; ".join(cli))
if srv:
    print(f"\nNOTE: server CPU < 90% in {len(srv)} cells (server not saturated, rps not meaningful there; us/rt still is): " + "; ".join(srv))

# ---- (j) lost cells and the automatic checks (plans/tls-win.md 1.3)
JUDGED = [("echotls", 256, 128), ("echotls", 256, 4096), ("churntls", 64, 128), ("churntlsres", 64, 128)]
fails = []
all_rounds = {r for d in col["rps"].values() for r in d}
have = lambda f, cl, v: col[f].get((*cl, v), {})
if judged_tls:
    for cl in JUDGED:
        for v in judged_tls:
            if missing := all_rounds - set(have("rps", cl, v)):
                fails.append(f"missing rows: {v} {name(cl)} rounds {sorted(missing)}")
    # one negotiated set (version suite group cert) over every server and cell
    try:
        neg = [re.match(r"(\S+) \S+: (.*?) full=\d+ resumed=\d+$", l.strip()) for l in open(info) if l.strip()]
    except FileNotFoundError:
        neg = []
    neg = [m for m in neg if m and m.group(1) not in REF]
    if len({m.group(2) for m in neg}) != 1:
        fails.append("tls_info.txt: not exactly one negotiated set: " + "; ".join(sorted({m.group(2) for m in neg})))
    for v in judged_tls:
        if not any(m.group(1) == v for m in neg):
            fails.append(f"tls_info.txt: no line for {v}")
    for v in judged_tls:
        rs = have("resumed", JUDGED[3], v)
        if not rs or min(rs.values()) < 0.95:
            fails.append(f"resumption < 0.95 or missing: {v} churntlsres: {dict(sorted(rs.items()))}")


def lost_cells(heading, who, judged, ems, rivals, opp=opponents, name=name):  # rivals: the servers that ran in these cells
    print(f"\n### {heading}\n")
    nr = len(all_rounds)
    if nr < 9:
        print(f"WARNING: ROUNDS={nr} < 9: no verdicts (the rule 'median worse than 5% and every round on the worse side' needs 9 rounds).")
        return
    print(f"Ratio {who} / rival per round (lower is better for both axes), median [min–max]. THUA = median > 1.05 and every round > 1;")
    print("THẮNG = median < 0.95 and every round < 1; else NGANG. rps only reported: it is the inverse of us/rt when the server is saturated (CPU >= 95%).\n")
    print(f"| cell | {who} | rival | server us/rt | verdict | p99/p50 | verdict | rps em/rival |")
    print("|---|---|---|---|---|---|---|---|")
    lost = []
    ratios = lambda a, b: [a[r] / b[r] for r in a if r in b]
    tailr = lambda cl, v: {r: p99 / have("p50_us", cl, v)[r] for r, p99 in have("p99_us", cl, v).items()}
    fmt = lambda rs: f"{statistics.median(rs):.2f} [{min(rs):.2f}–{max(rs):.2f}]"
    for cl in judged:
        for em in ems:
            for rv in opp(em, rivals):
                if not (have("us_per_rt", cl, em) and have("us_per_rt", cl, rv)):
                    continue  # reported as missing rows above
                if max(med("client_cpu", cl, em), med("client_cpu", cl, rv)) >= 0.9:
                    print(f"| {name(cl)} | {em} | {rv} | không phán (client CPU >= 90%) | | | | |")
                    continue
                u, t = ratios(have("us_per_rt", cl, em), have("us_per_rt", cl, rv)), ratios(tailr(cl, em), tailr(cl, rv))
                uv, tv = verdict(u), verdict(t)
                lost += [f"{em} vs {rv} {name(cl)} {ax}: {fmt(x)}" for ax, vd, x in (("us/rt", uv, u), ("p99/p50", tv, t)) if vd == "THUA"]
                rr = ratios(have("rps", cl, em), have("rps", cl, rv))
                sat = min(med("server_cpu", cl, em), med("server_cpu", cl, rv)) >= 0.95
                print(f"| {name(cl)} | {em} | {rv} | {fmt(u)} | {uv} | {fmt(t)} | {tv} | {statistics.median(rr):.2f}x {'(= 1/us)' if sat else 'không phán (chưa bão hoà)'} |")
    print("\nTHUA: " + ("none" if not lost else "") + "".join(f"\n- {l}" for l in lost))


if judged_tls:
    lost_cells("(j) lost cells: em-tls against same-backend rivals", "em-tls", JUDGED, ("em-tls-uring", "em-tls-epoll"), judged_tls)
# plain cells: em-uring / em-epoll against the plain servers of the same backend (not emrs-*, not tokio-rustls)
if echo + churn:
    lost_cells("(j2) lost cells, plain: em against same-backend rivals", "em", echo + churn, ("em-uring", "em-epoll"), servers)
if httpc:
    http_rivals = lambda em, srvs: [v for v in srvs if not v.startswith("em-http-")]  # every rival, whatever the backend
    for v in servers:
        for cl in httpc:
            if missing := all_rounds - set(have("rps", cl, v)):
                fails.append(f"missing rows: {v} {name(cl)} rounds {sorted(missing)}")
    lost_cells("(j3) lost cells, HTTP: em-http against every rival", "em-http", httpc, ("em-http-uring", "em-http-epoll"), servers, http_rivals, hlab)
if ref_seen := [v for v in tls_servers if v in REF]:
    print("\nReference only, not judged and not checked: " + "; ".join(f"{v} ({REF_NOTE.get(v, 'reference')})" for v in ref_seen))
if fails:
    print("\nCHECKS FAILED:" + "".join(f"\n- {f}" for f in fails))
    print("CHECKS FAILED: " + "; ".join(fails), file=sys.stderr)
    sys.exit(1)
