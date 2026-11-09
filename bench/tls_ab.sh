#!/usr/bin/env bash
# Is the em_tls vs tokio-rustls churn gap the library, or the bench setup (plans/tls-perf.md, H1 + N1)?
# em-tls-uring, em-tls-epoll and tokio-rustls, interleaved over ROUNDS, on churntls (every connection a
# full handshake) and churntlsres (resumed), 64 x 128, under each ticket mode of both servers
# (TLS_TICKETS = default 0 stateless stateful), plus one Nagle cell: churntls with NODELAY=0.
# Writes results.csv and table.md to OUT; the table goes to $GITHUB_STEP_SUMMARY when set.
# env: DURATION (10), ROUNDS (5), OUT (bench/out/tls-ab), TLS_PREFIX (see run.sh)
set -euo pipefail
cd "$(dirname "$0")/.."
DURATION=${DURATION:-10}
ROUNDS=${ROUNDS:-5}
OUT=${OUT:-bench/out/tls-ab}
source bench/run.sh

build
mkdir -p "$OUT"
echo "round,variant,mode,server,conns,size,rps,client_cpu,server_cpu,us_per_rt,p50_us,p99_us,p999_us,resumed" >"$OUT/results.csv"
: >"$OUT/tls_info.txt"
# variants: tickets=<mode> or nodelay=0; "default" leaves the server's own default
VARIANTS=(tickets=default tickets=0 tickets=stateless tickets=stateful nodelay=0)
port=22000
for ((r = 1; r <= ROUNDS; r++)); do
    for v in "${VARIANTS[@]}"; do
        unset TLS_TICKETS NODELAY
        case $v in
            tickets=default) ;;
            tickets=*) export TLS_TICKETS=${v#tickets=} ;;
            nodelay=0) export NODELAY=0 ;;
        esac
        modes=(churntls churntlsres); [[ $v == nodelay=0 ]] && modes=(churntls)
        for mode in "${modes[@]}"; do
            arg=(churn tls); [[ $mode == churntlsres ]] && arg=(churn tlsres)
            for srv in "${TLS_SERVERS[@]}"; do
                port=$((port + 1))
                start "$srv" "$port"
                run_cell "$srv" "$port" "$mode" 64 128 "${arg[@]}"
                stop
                echo "$r,$v,$mode,$srv,64,128,$CELL_ROW" | tee -a "$OUT/results.csv" >&2
            done
        done
    done
done
rm -f "$OUT/rps.tmp"

python3 -I - "$OUT/results.csv" "${TLS_SERVERS[@]}" <<'PY' | tee "$OUT/table.md"
import csv, statistics, sys
from collections import defaultdict
servers = sys.argv[2:]
g = defaultdict(list)
order = []
for r in csv.DictReader(open(sys.argv[1])):
    k = (r["variant"], r["mode"])
    if k not in order: order.append(k)
    g[(k, r["server"])].append(r)
med = lambda rows, c: statistics.median(float(x[c]) for x in rows)
print("### em_tls vs tokio-rustls, churn 64 x 128 (median over rounds)\n")
print("| variant | mode | server | conn/s | server us/conn | p50 us | p99 us | resumed |\n|---|---|---|---|---|---|---|---|")
for k in order:
    for s in servers:
        rows = g[(k, s)]
        res = rows[0]["resumed"]
        res = "-" if res == "-" else f"{med(rows, 'resumed'):.2f}"
        print(f"| {k[0]} | {k[1]} | {s} | {med(rows, 'rps'):.0f} | {med(rows, 'us_per_rt'):.1f} | "
              f"{med(rows, 'p50_us'):.0f} | {med(rows, 'p99_us'):.0f} | {res} |")
PY
[[ -z ${GITHUB_STEP_SUMMARY:-} ]] || { echo "### tls ab ($ARCH, ${DURATION}s x $ROUNDS rounds)"; cat "$OUT/table.md"; } >>"$GITHUB_STEP_SUMMARY"
