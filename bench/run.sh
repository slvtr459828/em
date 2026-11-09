#!/usr/bin/env bash
# Echo benchmark: em and its Rust crate (io_uring and epoll backends) vs tokio, monoio, compio, glommio. See plans/bench.md.
# env: DURATION (s per run, 10), ROUNDS (3), OUT (output dir, bench/out)
set -euo pipefail
cd "$(dirname "$0")/.."

DURATION=${DURATION:-10}
ROUNDS=${ROUNDS:-3}
OUT=${OUT:-bench/out}
ARCH=$(uname -m)
BIN=build/$ARCH/bench
SERVERS=(em-uring emrs-uring monoio-uring compio-uring glommio-uring tokio-epoll monoio-epoll compio-epoll em-epoll emrs-epoll)
CONNS=(16 256 1024)
SIZES=(128 4096)
SRV_CPUS=0 # one server CPU (a single number: see cpu_ticks), 1 worker; client gets 3 CPUs: plans/bench.md
CLI_CPUS=1-3
CLI_THREADS=3
WORKERS=1
TICKS=$(getconf CLK_TCK)

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
    cp "build/$ARCH/rel/em_echo" "$BIN/em-epoll"
    (
        cd bench/rust
        local pkgs=(-p tokio_echo -p monoio_echo -p glommio_echo -p client)
        # glommio's bundled liburing misses struct open_how with some headers (Fedora 44);
        # on others (Ubuntu 24.04) forcing it in is a redefinition. Only add it when needed.
        # em_rs_echo on its own: the CFLAGS retry below would also reach em's build.rs (asm file)
        cargo build --release --locked -p em_rs_echo >&2
        cargo build --release --locked "${pkgs[@]}" >&2 ||
            CFLAGS="${CFLAGS:-} -include linux/openat2.h" cargo build --release --locked "${pkgs[@]}" >&2
        cp target/release/em_rs_echo ../../$BIN/emrs
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
    local cmd=("$BIN/$1" "$2" "$WORKERS")
    case $1 in em-*) cmd=(env "EM_BACKEND=${1#em-}" "${cmd[@]}") ;; esac # same binary, backend forced
    case $1 in emrs-*) cmd=(env "EM_BACKEND=${1#emrs-}" "$BIN/emrs" "$2" "$WORKERS") ;; esac
    case $1 in monoio-*) cmd=("$BIN/monoio" "$2" "${1#monoio-}" "$WORKERS") ;; esac
    taskset -c $SRV_CPUS "${cmd[@]}" >/dev/null &
    pid=$!
    for _ in {1..100}; do
        kill -0 "$pid" 2>/dev/null || { echo "$1 exited during startup" >&2; pid=; return 1; }
        (exec 3<>"/dev/tcp/127.0.0.1/$2") 2>/dev/null && return 0
        sleep 0.1
    done
    echo "$1 not listening on $2" >&2
}

# Busy ticks of the server's CPU from /proc/stat (user+nice+system+irq+softirq+steal). Not the
# process's utime+stime: on loopback the TCP stack runs in softirq, which tick accounting does not
# charge to the task (~40% of CPU 0 here), so /proc/<pid>/stat shows ~0.55 for a saturated server.
# SRV_CPUS is a single CPU that only the server runs on, so its busy time is the server's cost.
cpu_ticks() {
    local l u n sy i io irq si st
    while read -r l u n sy i io irq si st _; do
        [[ $l == "cpu$SRV_CPUS" ]] && { echo $((u + n + sy + irq + si + st)); return; }
    done </proc/stat
}

build
mkdir -p "$OUT"
echo "round,server,conns,size,rps,client_cpu,server_cpu,us_per_rt" >"$OUT/results.csv"
port=20000
TIMEFORMAT='%U %S %R'
for ((r = 1; r <= ROUNDS; r++)); do
    for c in "${CONNS[@]}"; do for s in "${SIZES[@]}"; do for srv in "${SERVERS[@]}"; do
        port=$((port + 1))
        start "$srv" "$port"
        # bash `time` reports the client's own user+sys; client stderr is kept for errors
        t0=$(cpu_ticks)
        t=$( { time taskset -c $CLI_CPUS "$BIN/client" "127.0.0.1:$port" "$c" "$s" "$DURATION" "$CLI_THREADS" \
                 >"$OUT/rps.tmp"; } 2>&1 ) || { echo "client failed for $srv c=$c s=$s: $t" >&2; exit 1; }
        t1=$(cpu_ticks)
        stop
        read -r u sy wall <<<"$(tail -n1 <<<"$t")"
        read -r total rps <"$OUT/rps.tmp"
        # client prints "total_round_trips rps"; server CPU is over the same client wall time
        cols=$(awk -v u="$u" -v s="$sy" -v w="$wall" -v t0="$t0" -v t1="$t1" -v hz="$TICKS" -v n="$total" -v nc="$CLI_THREADS" \
            'BEGIN { sc = (t1 - t0) / hz; printf "%.2f,%.2f,%.2f", (u + s) / (w * nc), sc / w, sc * 1e6 / n }')
        echo "$r,$srv,$c,$s,$rps,$cols" | tee -a "$OUT/results.csv" >&2
    done; done; done
done
rm -f "$OUT/rps.tmp"

python3 -I bench/table.py "$OUT/results.csv" "${SERVERS[@]}" | tee "$OUT/table.md"
[[ -z ${GITHUB_STEP_SUMMARY:-} ]] || { echo "### echo bench ($ARCH, ${DURATION}s x $ROUNDS rounds)"; cat "$OUT/table.md"; } >>"$GITHUB_STEP_SUMMARY"
