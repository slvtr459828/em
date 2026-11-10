#!/usr/bin/env bash
# Echo benchmark: em and its Rust crate (io_uring and epoll backends) vs tokio, monoio, compio, glommio.
# See plans/bench.md. Then the TLS cells (plans/v0.2.md, plans/tls-win.md): em-tls-* vs every Rust runtime
# on rustls/aws-lc-rs, then the memory per idle TLS connection (bench/rss_tls.sh).
# env: DURATION (s per run, 10), ROUNDS (5), OUT (output dir, bench/out)
#      ONLY=tls: run only the TLS cells (the verdict table needs ROUNDS >= 9)
#      TLS_PREFIX (an OpenSSL prefix with include/ and lib[64]/: em-tls-* servers are built against it
#      and run with it on LD_LIBRARY_PATH; bench/tls_libs.sh builds such prefixes)
#      TLS_TICKETS (0|stateless|stateful), NODELAY (0): passed through to the TLS servers
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD

DURATION=${DURATION:-10}
ROUNDS=${ROUNDS:-5}
OUT=${OUT:-bench/out}
ONLY=${ONLY:-}
[[ -z $ONLY || $ONLY == tls ]] || { echo "ONLY=$ONLY: want tls or unset" >&2; exit 2; }
ARCH=$(uname -m)
BIN=build/$ARCH/bench
SERVERS=(em-uring emrs-uring monoio-uring compio-uring glommio-uring tokio-epoll monoio-epoll compio-epoll em-epoll emrs-epoll)
TLS_SERVERS=(em-tls-uring em-tls-epoll tokio-rustls tokio-rustls-ct monoio-rustls-uring monoio-rustls-epoll compio-rustls-uring compio-rustls-epoll glommio-rustls-uring)
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

# TLS_PREFIX: tls.c and em_tls_echo.c compiled against the prefix's headers (same flags as the
# Makefile's rel build) into their own dir; the default build is left alone.
tls_prefix_lib() { for d in lib64 lib; do [[ -e $TLS_PREFIX/$d/libssl.so ]] && { echo "$TLS_PREFIX/$d"; return; }; done
                   echo "TLS_PREFIX=$TLS_PREFIX: no lib64/libssl.so or lib/libssl.so" >&2; return 1; }
build_tls_prefix() {
    local lib d=build/$ARCH/bench-tlsprefix rel=build/$ARCH/rel
    lib=$(tls_prefix_lib) || return 1
    local cf=(-std=gnu11 -O2 -g -D_FORTIFY_SOURCE=3 -Wall -Wextra -Werror -Iinclude -I"$TLS_PREFIX/include")
    mkdir -p "$d"
    { cc "${cf[@]}" -c src/tls.c -o "$d/tls.o" && rm -f "$d/libem_tls.a" && ar rcs "$d/libem_tls.a" "$d/tls.o" &&
      cc "${cf[@]}" -Isrc bench/em_tls_echo.c "$d/libem_tls.a" "$rel/libem.a" -L"$lib" -lssl -lcrypto -luring -lm -lpthread \
          -o "$d/em_tls_echo"; } >&2 ||
        { echo "TLS_PREFIX=$TLS_PREFIX: em_tls does not build against it (OpenSSL 3.x or AWS-LC)" >&2; return 1; }
}

build() {
    make bench-em bench-em-tls TLS_PREFIX= >&2 # the default build: TLS_PREFIX is built separately below
    mkdir -p "$BIN"
    cp "build/$ARCH/rel/em_echo" "$BIN/em-uring"
    cp "build/$ARCH/rel/em_echo" "$BIN/em-epoll"
    local tls_echo=build/$ARCH/rel/em_tls_echo
    if [[ -n ${TLS_PREFIX:-} ]]; then build_tls_prefix; tls_echo=build/$ARCH/bench-tlsprefix/em_tls_echo; fi
    cp "$tls_echo" "$BIN/em-tls-uring"
    cp "$tls_echo" "$BIN/em-tls-epoll"
    (
        cd bench/rust
        local pkgs=(-p tokio_echo -p monoio_echo -p glommio_echo -p glommio_rustls_echo)
        # glommio's bundled liburing misses struct open_how with some headers (Fedora 44);
        # on others (Ubuntu 24.04) forcing it in is a redefinition. Only add it when needed.
        # em_rs_echo, client and the other TLS servers on their own: the retry below changes CC, which
        # reruns the build.rs of every crate in its graph. Not CFLAGS: it would also reach aws-lc-sys
        # (asm files do not take `-include`), em (asm file) and ring. The wrapper adds the include
        # to glommio's liburing sources only.
        cargo build --release --locked -p em_rs_echo -p client -p tokio_rustls_echo -p monoio_rustls_echo >&2
        cargo build --release --locked "${pkgs[@]}" >&2 || {
            local cc=$ROOT/build/$ARCH/glommio-cc
            mkdir -p "$(dirname "$cc")"
            printf '#!/bin/sh\ncase "$*" in *glommio-*/liburing/*) exec %s -include linux/openat2.h "$@" ;; esac\nexec %s "$@"\n' \
                "${CC:-cc}" "${CC:-cc}" >"$cc"
            chmod +x "$cc"
            CC=$cc cargo build --release --locked "${pkgs[@]}" >&2
        }
        cp target/release/em_rs_echo ../../$BIN/emrs
        cp target/release/tokio_echo ../../$BIN/tokio-epoll
        cp target/release/tokio_rustls_echo ../../$BIN/tokio-rustls # tokio-rustls-ct: the same binary, TOKIO_RT=current_thread
        cp target/release/monoio_rustls_echo ../../$BIN/monoio-rustls
        cp target/release/glommio_rustls_echo ../../$BIN/glommio-rustls-uring
        cp target/release/monoio_echo ../../$BIN/monoio
        cp target/release/glommio_echo ../../$BIN/glommio-uring
        cp target/release/client ../../$BIN/client
        # the compio driver is a build-time feature: one binary per driver
        for d in uring epoll; do
            cargo build --release --locked -p compio_echo --features $d >&2
            cp target/release/compio_echo ../../$BIN/compio-$d
            cargo build --release --locked -p compio_rustls_echo --features $d >&2
            cp target/release/compio_rustls_echo ../../$BIN/compio-rustls-$d
        done
    )
}

start() { # server port: launches pinned in the background, waits until it accepts
    local cmd=("$BIN/$1" "$2" "$WORKERS")
    case $1 in em-tls-uring|em-tls-epoll) cmd=(env "EM_BACKEND=${1#em-tls-}" "${cmd[@]}") # same binary, backend forced
               [[ -z ${TLS_PREFIX:-} ]] || cmd=(env "LD_LIBRARY_PATH=$(tls_prefix_lib)" "${cmd[@]}") ;;
               em-uring|em-epoll) cmd=(env "EM_BACKEND=${1#em-}" "${cmd[@]}") ;; esac
    case $1 in emrs-uring|emrs-epoll) cmd=(env "EM_BACKEND=${1#emrs-}" "$BIN/emrs" "$2" "$WORKERS") ;; esac
    # exact names: monoio-* would also match monoio-rustls-*
    case $1 in monoio-uring|monoio-epoll) cmd=("$BIN/monoio" "$2" "${1#monoio-}" "$WORKERS") ;;
               monoio-rustls-uring|monoio-rustls-epoll) cmd=("$BIN/monoio-rustls" "$2" "${1#monoio-rustls-}" "$WORKERS") ;;
               tokio-rustls-ct) cmd=(env TOKIO_RT=current_thread "$BIN/tokio-rustls" "$2" "$WORKERS") ;; esac
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

# run_cell srv port mode conns size [client args...]: one client run against the server the caller
# started (the caller stops it, so it can attach perf meanwhile). Sets CELL_ROW to
# "rps,client_cpu,server_cpu,us_per_rt,p50_us,p99_us,p999_us,resumed" (resumed: "-" for non-TLS cells)
# and total (round-trips, or connections for churn); appends the negotiated TLS parameters to
# $OUT/tls_info.txt. Needs $OUT to exist.
run_cell() {
    local srv=$1 port=$2 mode=$3 c=$4 s=$5; shift 5
    local TIMEFORMAT='%U %S %R' t t0 t1 u sy wall rps p50 p99 p999 cols tls res
    # bash `time` reports the client's own user+sys; client stderr is kept for errors
    t0=$(cpu_ticks)
    t=$( { time taskset -c $CLI_CPUS "$BIN/client" "127.0.0.1:$port" "$c" "$s" "$DURATION" "$CLI_THREADS" "$@" \
             >"$OUT/rps.tmp"; } 2>&1 ) || { echo "client failed for $srv c=$c s=$s: $t" >&2; exit 1; }
    t1=$(cpu_ticks)
    read -r u sy wall <<<"$(tail -n1 <<<"$t")"
    read -r total rps p50 p99 p999 <"$OUT/rps.tmp"
    # client prints "total_round_trips rps p50 p99 p999"; server CPU is over the same client wall time
    cols=$(awk -v u="$u" -v s="$sy" -v w="$wall" -v t0="$t0" -v t1="$t1" -v hz="$TICKS" -v n="$total" -v nc="$CLI_THREADS" \
        'BEGIN { sc = (t1 - t0) / hz; printf "%.2f,%.2f,%.2f", (u + s) / (w * nc), sc / w, sc * 1e6 / n }')
    # TLS cells: the client's second line is "tls <version> <suite> <group> <cert> full=N resumed=M"
    res=-
    if tls=$(sed -n 's/^tls //p' "$OUT/rps.tmp") && [[ -n $tls ]]; then
        echo "$srv $mode: $tls" >>"$OUT/tls_info.txt"
        res=$(sed 's/.*full=\([0-9]*\) resumed=\([0-9]*\)/\1 \2/' <<<"$tls" | awk '{ printf "%.3f", $2 / ($1 + $2) }')
    fi
    CELL_ROW="$rps,$cols,$p50,$p99,$p999,$res"
}

# bench/profile.sh and tls_ab.sh source this file for build/start/stop/run_cell and the settings above.
[[ ${BASH_SOURCE[0]} == "$0" ]] || return 0

build
mkdir -p "$OUT"
# echo cells, then one churn cell (connect, one 128 B round-trip, RST; 64 concurrent: plans/m5.md)
CELLS=()
if [[ $ONLY != tls ]]; then
    for c in "${CONNS[@]}"; do for s in "${SIZES[@]}"; do CELLS+=("echo $c $s"); done; done
    CELLS+=("churn 64 128")
fi
# TLS cells: server us/rt of em-tls-* minus em-* in the same cell (same conns x size) is the cost
# of TLS (table.py). The mode names echotls/churntls keep them apart from the plain cells.
# churntls: client resumption off (every connection a full handshake); churntlsres: client keeps a
# session store and resumes (resumed = fraction of resumed handshakes, must be ~1 for both servers).
CELLS+=("echotls 256 128" "echotls 256 4096" "churntls 64 128" "churntlsres 64 128")
echo "round,mode,server,conns,size,rps,client_cpu,server_cpu,us_per_rt,p50_us,p99_us,p999_us,resumed" >"$OUT/results.csv"
: >"$OUT/tls_info.txt" # what each TLS server negotiated, as the client saw it
port=20000
for ((r = 1; r <= ROUNDS; r++)); do
    for cell in "${CELLS[@]}"; do
        read -r mode c s <<<"$cell"
        case $mode in echo) arg=() ;; churn) arg=(churn) ;; echotls) arg=(tls) ;; churntls) arg=(churn tls) ;; churntlsres) arg=(churn tlsres) ;; esac
        srvs=("${SERVERS[@]}"); [[ $mode == *tls* ]] && srvs=("${TLS_SERVERS[@]}")
        # the order rotates with the round (round r starts at server r), so no server always runs right
        # after the same other server
        k=$(((r - 1) % ${#srvs[@]})); srvs=("${srvs[@]:k}" "${srvs[@]:0:k}")
        for srv in "${srvs[@]}"; do
            port=$((port + 1))
            start "$srv" "$port"
            run_cell "$srv" "$port" "$mode" "$c" "$s" "${arg[@]}"
            stop
            echo "$r,$mode,$srv,$c,$s,$CELL_ROW" | tee -a "$OUT/results.csv" >&2
        done
    done
done
rm -f "$OUT/rps.tmp"

# memory per idle TLS connection (own table), after the cells so a failure here keeps the cell results
bench/rss_tls.sh | tee "$OUT/rss.md" >&2

# table.py exits 1 when a check fails (missing rows, mixed negotiation, resumption < 0.95): the summary is
# still written, then the run fails
rc=0
python3 -I bench/table.py "$OUT/results.csv" "${SERVERS[@]}" --tls "${TLS_SERVERS[@]}" | tee "$OUT/table.md" || rc=$?
[[ -z ${GITHUB_STEP_SUMMARY:-} ]] || { echo "### echo bench ($ARCH, ${DURATION}s x $ROUNDS rounds)"; cat "$OUT/table.md" "$OUT/rss.md"; } >>"$GITHUB_STEP_SUMMARY"
exit "$rc"
