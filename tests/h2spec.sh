#!/usr/bin/env bash
# h2spec (HTTP/2 conformance) against tests/h2_server: h2c, or with "tls" h2 through ALPN (h2spec -t -k).
# usage: H2SPEC=/path/to/h2spec tests/h2spec.sh [tls] [h2spec args, e.g. section ids 3.5 4.1]
# BACKEND=epoll|uring|auto (default auto). Fails on any failed test except the two in KNOWN, which
# cannot pass here whatever the code does (so this is the gate, not "h2spec exits 0"):
#  - 3.5.2 "Sends invalid connection preface" (h2c only): the port also speaks HTTP/1, so garbage in
#    place of the preface is an h1 request and gets an h1 400; h2spec wants a GOAWAY or a bare close.
#  - 5.1.1.2 "stream identifier numerically smaller than previous": nghttp2 does not track closed
#    stream ids, so it neither sends GOAWAY nor closes (the test times out).
set -euo pipefail
cd "$(dirname "$0")/.."
H2SPEC=${H2SPEC:-h2spec}
mode=plain
if [[ ${1:-} == tls ]]; then mode=tls; shift; fi
make -s h2-server
SRV=$(make -s print-brel)/h2_server
port=$((20000 + RANDOM % 12000)) # below the ephemeral range: no clash with a client socket
EM_BACKEND=${BACKEND:-auto} "$SRV" "$port" 1 $([[ $mode == tls ]] && echo tls) &
pid=$!
for _ in {1..50}; do
    kill -0 "$pid" 2>/dev/null || { echo "server exited during startup" >&2; exit 1; }
    (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null && break
    sleep 0.1
done
tlsargs=()
[[ $mode == tls ]] && tlsargs=(-t -k)
KNOWN=("Sends invalid connection preface" "Sends stream identifier that is numerically smaller than previous")
log=$(mktemp)
trap 'kill $pid 2>/dev/null || true; rm -f "$log"' EXIT
"$H2SPEC" -h 127.0.0.1 -p "$port" "${tlsargs[@]}" --strict "$@" | tee "$log" | tail -n 1 || true
rc=0
while IFS= read -r t; do
    for k in "${KNOWN[@]}"; do [[ $t == "$k" ]] && continue 2; done
    echo "h2spec FAILED: $t" >&2; rc=1
done < <(grep -a '^ *×' "$log" | sed 's/^ *× [0-9]*: //' | sort -u)
grep -aq ' tests, ' "$log" || { echo "h2spec did not finish" >&2; rc=1; }
[[ $rc == 0 ]] && echo "h2spec: only the known failures ($mode)"
exit $rc
