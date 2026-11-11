#!/usr/bin/env bash
# Memory per idle TLS connection of every TLS server of bench/run.sh (plans/tls-win.md 1.4).
# Each server is started by run.sh's own start() (same binary, same LD_LIBRARY_PATH, same pinning), then
# N/2 connections are opened (handshake, one 16 KiB echo, then idle), RSS read, N/2 more, RSS read again:
# KiB per connection = (RSS at N - RSS at N/2) / (N/2). The slope leaves the runtime's fixed footprint out.
# Repeated REPS times with a fresh server each time; table.py --rss prints the median and range and the verdict.
# usage: bench/rss_tls.sh   env: CONNS_N (1000), REPS (3), OUT (bench/out), TLS_PREFIX (see run.sh)
set -euo pipefail
cd "$(dirname "$0")/.."
N=${CONNS_N:-1000}
REPS=${REPS:-3}
OUT=${OUT:-bench/out}
source bench/run.sh
[[ -x $BIN/em-tls-uring && -x $BIN/${TLS_SERVERS[-1]} ]] || build
mkdir -p "$OUT"

echo "server,rep,kib_per_conn" >"$OUT/rss.csv"
port=24000
for ((rep = 1; rep <= REPS; rep++)); do
    for srv in "${TLS_SERVERS[@]}"; do
        port=$((port + 1))
        start "$srv" "$port"
        taskset -c $CLI_CPUS python3 -I - "$port" "$pid" "$N" "$srv" "$rep" >>"$OUT/rss.csv" <<'PY'
import socket, ssl, sys, time
port, pid, n, srv, rep = int(sys.argv[1]), sys.argv[2], int(sys.argv[3]), sys.argv[4], sys.argv[5]
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE
msg = b"x" * 16384
conns = []
def open_more(k):
    for _ in range(k):
        s = ctx.wrap_socket(socket.create_connection(("127.0.0.1", port)))
        s.sendall(msg)
        got = 0
        while got < len(msg):
            got += len(s.recv(len(msg) - got))
        conns.append(s)  # idle from here on
def rss():
    time.sleep(0.5)  # let the server settle (deferred frees, pending tickets)
    return int(next(l for l in open(f"/proc/{pid}/status") if l.startswith("VmRSS")).split()[1])
open_more(n // 2); half = rss()
open_more(n - n // 2); full = rss()
print(f"{srv},{rep},{(full - half) / (n - n // 2):.2f}")
PY
        stop
    done
done
python3 -I bench/table.py --rss "$OUT/rss.csv" ${REF_SERVERS[@]:+--ref "${REF_SERVERS[@]}"}
