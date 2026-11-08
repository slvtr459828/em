#!/usr/bin/env bash
# Echo benchmark: em vs tokio, monoio, compio, glommio. See plans/bench.md.
# env: DURATION (s per run, 10), ROUNDS (3), OUT (output dir, bench/out)
set -euo pipefail
cd "$(dirname "$0")/.."

DURATION=${DURATION:-10}
ROUNDS=${ROUNDS:-3}
OUT=${OUT:-bench/out}
ARCH=$(uname -m)
BIN=build/$ARCH/bench
SERVERS=(em-uring monoio-uring compio-uring glommio-uring tokio-epoll monoio-epoll compio-epoll)
CONNS=(16 256 1024)
SIZES=(128 4096)
SRV_CPUS=0-1
CLI_CPUS=2-3

pid=
stop() { # kill the running server and reap it
    [[ -n $pid ]] || return 0
    kill "$pid" 2>/dev/null || true
    for _ in {1..50}; do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
    kill -9 "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    pid=
}
trap stop EXIT
trap 'exit 130' INT TERM

# fds for ~1k connections per server and client; memlock for glommio's io_uring buffers
ulimit -n "$(ulimit -Hn)" 2>/dev/null || true
ulimit -l unlimited 2>/dev/null || echo "warn: cannot raise RLIMIT_MEMLOCK; glommio may fail" >&2

build() {
    make bench-em >&2
    mkdir -p "$BIN"
    cp "build/$ARCH/rel/em_echo" "$BIN/em-uring"
    (
        cd bench/rust
        local pkgs=(-p tokio_echo -p monoio_echo -p glommio_echo -p client)
        # glommio's bundled liburing misses struct open_how with some headers (Fedora 44);
        # on others (Ubuntu 24.04) forcing it in is a redefinition. Only add it when needed.
        cargo build --release --locked "${pkgs[@]}" >&2 ||
            CFLAGS="${CFLAGS:-} -include linux/openat2.h" cargo build --release --locked "${pkgs[@]}" >&2
        cp target/release/tokio_echo ../../$BIN/tokio-epoll
        cp target/release/monoio_echo ../../$BIN/monoio
        cp target/release/glommio_echo ../../$BIN/glommio-uring
        cp target/release/client ../../$BIN/client
        # the compio driver is a build-time feature: one binary per driver
        for d in uring epoll; do
            cargo build --release --locked -p compio_echo --features $d >&2
            cp target/release/compio_echo ../../$BIN/compio-$d
        done
    )
}

start() { # server port: launches pinned in the background, waits until it accepts
    local cmd=("$BIN/$1" "$2")
    case $1 in monoio-*) cmd=("$BIN/monoio" "$2" "${1#monoio-}") ;; esac
    taskset -c $SRV_CPUS "${cmd[@]}" >/dev/null &
    pid=$!
    for _ in {1..100}; do
        kill -0 "$pid" 2>/dev/null || { echo "$1 exited during startup" >&2; pid=; return 1; }
        (exec 3<>"/dev/tcp/127.0.0.1/$2") 2>/dev/null && return 0
        sleep 0.1
    done
    echo "$1 not listening on $2" >&2
}

build
mkdir -p "$OUT"
echo "round,server,conns,size,rps,client_cpu" >"$OUT/results.csv"
port=20000
TIMEFORMAT='%U %S %R'
for ((r = 1; r <= ROUNDS; r++)); do
    for c in "${CONNS[@]}"; do for s in "${SIZES[@]}"; do for srv in "${SERVERS[@]}"; do
        port=$((port + 1))
        start "$srv" "$port"
        # bash `time` reports the client's own user+sys; client stderr is kept for errors
        t=$( { time taskset -c $CLI_CPUS "$BIN/client" "127.0.0.1:$port" "$c" "$s" "$DURATION" 2 \
                 >"$OUT/rps.tmp"; } 2>&1 ) || { echo "client failed for $srv c=$c s=$s: $t" >&2; exit 1; }
        stop
        read -r u sy wall <<<"$(tail -n1 <<<"$t")"
        cpu=$(awk -v u="$u" -v s="$sy" -v w="$wall" 'BEGIN { printf "%.2f", (u + s) / (w * 2) }')
        echo "$r,$srv,$c,$s,$(<"$OUT/rps.tmp"),$cpu" | tee -a "$OUT/results.csv" >&2
    done; done; done
done
rm -f "$OUT/rps.tmp"

python3 -I bench/table.py "$OUT/results.csv" "${SERVERS[@]}" | tee "$OUT/table.md"
[[ -z ${GITHUB_STEP_SUMMARY:-} ]] || { echo "### echo bench ($ARCH, ${DURATION}s x $ROUNDS rounds)"; cat "$OUT/table.md"; } >>"$GITHUB_STEP_SUMMARY"
