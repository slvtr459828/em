#!/usr/bin/env bash
# RSS of em_tls_echo with N idle TLS connections (plans/v0.2.md): the number for the README.
# usage: bench/rss_tls.sh [N=1000]   (needs build/$ARCH/rel/em_tls_echo: make bench-em-tls)
set -euo pipefail
cd "$(dirname "$0")/.."
N=${1:-1000}
BIN=build/$(uname -m)/rel/em_tls_echo
ulimit -n "$(ulimit -Hn)" 2>/dev/null || true
port=$((24000 + RANDOM % 2000))
EM_BACKEND=${EM_BACKEND:-uring} "$BIN" "$port" 1 &
pid=$!
trap 'kill $pid 2>/dev/null || true' EXIT
for _ in {1..50}; do (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null && break; sleep 0.1; done
rss() { awk '/VmRSS/ {print $2}' /proc/$pid/status; }
before=$(rss)
python3 -I - "$port" "$N" "$pid" "$before" <<'PY'
import socket, ssl, sys
port, n, pid, before = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3], int(sys.argv[4])
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE
conns = []
for _ in range(n):
    s = ctx.wrap_socket(socket.create_connection(("127.0.0.1", port)))
    s.sendall(b"x"); s.recv(1)  # handshake done and the server fiber is in em_tls_recv
    conns.append(s)
rss = int([l for l in open(f"/proc/{pid}/status") if l.startswith("VmRSS")][0].split()[1])
print(f"em_tls_echo RSS: {before} KiB idle, {rss} KiB with {n} idle TLS connections = {(rss - before) / n:.1f} KiB per connection")
PY
