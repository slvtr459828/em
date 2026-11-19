#!/usr/bin/env bash
# Echo benchmark: em and its Rust crate (io_uring and epoll backends) vs tokio, monoio, compio, glommio.
# See plans/bench.md. Then the TLS cells (plans/v0.2.md, plans/tls-win.md): em-tls-* vs every Rust runtime
# on rustls/aws-lc-rs, then the memory per idle TLS connection (bench/rss_tls.sh).
# env: DURATION (s per run, 10), ROUNDS (5), OUT (output dir, bench/out)
#      ONLY=tls: run only the TLS cells (the verdict table needs ROUNDS >= 9)
#      ONLY=http: only the HTTP cells (plans/http.md), plain and TLS: em-http-* vs nginx, haproxy, h2o (bench/http/), hyper
#      (bench/rust/hyper_http), Go net/http (bench/go/http); RIVALS does not apply; TLS_PREFIX builds em_http's TLS against it
#      ONLY=h2: only the HTTP/2 cells (plans/http2.md), h2c and h2 over TLS: em-h2-* vs nginx and h2o, driven by h2load
#      (nghttp2-client); the reference cells em-h1-* are the same binary driven by `h2load --h1` (per-request h1 cost)
#      TLS_PREFIX (an OpenSSL prefix with include/ and lib[64]/: em-tls-* servers are built against it
#      and run with it on LD_LIBRARY_PATH; bench/tls_libs.sh builds such prefixes)
#      TLS_TICKETS (0|stateless|stateful), NODELAY (0): passed through to the TLS servers
#      RIVALS: rust (default: the servers above), cpp (em, tokio as the ratio baseline, and the C/C++/Go
#      servers of plans/bench-cpp.md: bench/cpp/*, bench/go; seastar uses GnuTLS, TLS_PREFIX does not apply to it), all (both). TLS_PREFIX reaches their build.sh too.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD

DURATION=${DURATION:-10}
ROUNDS=${ROUNDS:-5}
OUT=${OUT:-bench/out}
ONLY=${ONLY:-}
[[ -z $ONLY || $ONLY == tls || $ONLY == http || $ONLY == h2 ]] || { echo "ONLY=$ONLY: want tls, http, h2 or unset" >&2; exit 2; }
ARCH=$(uname -m)
BIN=build/$ARCH/bench
RIVALS=${RIVALS:-rust}
RUST_SERVERS=(em-uring emrs-uring monoio-uring compio-uring glommio-uring tokio-epoll monoio-epoll compio-epoll em-epoll emrs-epoll)
RUST_TLS_SERVERS=(em-tls-uring em-tls-epoll tokio-rustls tokio-rustls-ct monoio-rustls-uring monoio-rustls-epoll compio-rustls-uring compio-rustls-epoll glommio-rustls-uring)
# The rivals outside Rust: build scripts (build.sh OUTDIR) and server names. The -uring/-epoll suffix is
# what table.py judges by. A new runtime is its names here plus its directory in NATIVE_BUILDS.
NATIVE_BUILDS=(bench/cpp/photon bench/cpp/asio bench/cpp/libuv bench/cpp/seastar bench/go)
NATIVE_SERVERS=(photon-uring asio-uring seastar-uring photon-epoll asio-epoll seastar-epoll libuv-epoll go-epoll)
NATIVE_TLS_SERVERS=(photon-tls-uring asio-tls-uring seastar-tls-uring photon-tls-epoll asio-tls-epoll seastar-tls-epoll libuv-tls-epoll go-tls-epoll)
# go-tls-epoll: reference only (AES-128, 1 ticket: plans/bench-cpp.md): printed, never judged, no churntlsres cell
case $RIVALS in
    rust) SERVERS=("${RUST_SERVERS[@]}"); TLS_SERVERS=("${RUST_TLS_SERVERS[@]}"); REF_SERVERS=() ;;
    cpp|all) REF_SERVERS=(go-tls-epoll)
        if [[ $RIVALS == cpp ]]; then
            SERVERS=(em-uring em-epoll tokio-epoll "${NATIVE_SERVERS[@]}")
            TLS_SERVERS=(em-tls-uring em-tls-epoll tokio-rustls tokio-rustls-ct "${NATIVE_TLS_SERVERS[@]}")
        else
            SERVERS=("${RUST_SERVERS[@]}" "${NATIVE_SERVERS[@]}")
            TLS_SERVERS=("${RUST_TLS_SERVERS[@]}" "${NATIVE_TLS_SERVERS[@]}")
        fi ;;
    *) echo "RIVALS=$RIVALS: want rust, cpp or all" >&2; exit 2 ;;
esac
# ONLY=http: em_http (-uring/-epoll: the same binary, backend forced) against the servers that bench/http/ launches
# or builds (nginx, haproxy: apt; h2o: bench/http/h2o/build.sh), hyper and Go net/http. Every rival is epoll and is
# judged against both em-http-*. Each has a TLS variant (the -tls- names, for the httptls / httpchurntls cells).
# gohttp-tls-epoll: reference only (crypto/tls picks AES-128 in TLS 1.3 and the suite is not configurable), like go-tls-epoll.
HTTP_SERVERS=(em-http-uring em-http-epoll nginx-epoll haproxy-epoll h2o-epoll hyper-epoll gohttp-epoll)
HTTP_TLS_SERVERS=(em-http-tls-uring em-http-tls-epoll nginx-tls-epoll haproxy-tls-epoll h2o-tls-epoll hyper-tls-epoll gohttp-tls-epoll)
if [[ $ONLY == http ]]; then SERVERS=("${HTTP_SERVERS[@]}"); TLS_SERVERS=("${HTTP_TLS_SERVERS[@]}"); REF_SERVERS=(gohttp-tls-epoll); fi
# ONLY=h2: em_http linked with libem_http2 (bench/em_http.c, make bench-em-http2) against nginx (listen ... http2) and h2o
# (bench/http/h2o), both epoll and judged against both em-h2-*. The plain servers speak h2c (prior knowledge), the -tls- ones
# h2 through ALPN. em-h1-*: the same binary, driven by h2load --h1 (ALPN http/1.1 over TLS): reference only, never judged;
# its server us per request is the denominator of the per-stream cost (table.py (k)).
H2_SERVERS=(em-h2-uring em-h2-epoll nginx-h2-epoll h2o-h2-epoll em-h1-uring em-h1-epoll)
H2_TLS_SERVERS=(em-h2-tls-uring em-h2-tls-epoll nginx-h2-tls-epoll h2o-h2-tls-epoll em-h1-tls-uring em-h1-tls-epoll)
if [[ $ONLY == h2 ]]; then SERVERS=("${H2_SERVERS[@]}"); TLS_SERVERS=("${H2_TLS_SERVERS[@]}"); REF_SERVERS=(em-h1-uring em-h1-epoll em-h1-tls-uring em-h1-tls-epoll); fi
H2_STREAMS=10 # h2load -m: concurrent streams per connection
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

build_http() {
    # em_http with its TLS mode, built in the dir of the run's TLS_PREFIX (the exported variable reaches make);
    # the dir is asked of make itself (--eval runs before the Makefile is read, the recipe is expanded after)
    local d
    d=$(make -s HTTP_SSE42=1 --eval='em-http-dir:; @echo $(B_REL)' em-http-dir)
    make HTTP_SSE42=1 bench-em-http >&2
    mkdir -p "$BIN"
    cp "$d/em_http" "$BIN/em-http-uring"
    cp "$d/em_http" "$BIN/em-http-epoll"
    (cd bench/rust && cargo build --release --locked -p client -p hyper_http >&2 &&
        cp target/release/client ../../$BIN/client && cp target/release/hyper_http ../../$BIN/hyper-http)
    bench/go/build.sh "$ROOT/$BIN" http >&2
    bench/http/cert.sh >/dev/null # once, before the servers: h2o takes the files as arguments
    bench/http/h2o/build.sh "$ROOT/$BIN" >&2
    command -v nginx >/dev/null && command -v haproxy >/dev/null || { echo "ONLY=http needs nginx and haproxy (apt install nginx haproxy)" >&2; return 1; }
}

build_h2() {
    local d
    d=$(make -s HTTP_SSE42=1 --eval='em-http-dir:; @echo $(B_REL)' em-http-dir)
    make HTTP_SSE42=1 bench-em-http2 >&2
    mkdir -p "$BIN"
    cp "$d/em_http_h2" "$BIN/em-http-h2"
    bench/http/cert.sh >/dev/null
    bench/http/h2o/build.sh "$ROOT/$BIN" >&2
    command -v nginx >/dev/null && command -v h2load >/dev/null || { echo "ONLY=h2 needs nginx and h2load (apt install nginx nghttp2-client)" >&2; return 1; }
}

build() {
    [[ $ONLY != http ]] || { build_http; return; }
    [[ $ONLY != h2 ]] || { build_h2; return; }
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
        if [[ $RIVALS == cpp ]]; then # only the baselines and the client; the other runtimes are not built
            cargo build --release --locked -p client -p tokio_echo -p tokio_rustls_echo >&2
            cp target/release/tokio_echo ../../$BIN/tokio-epoll
            cp target/release/tokio_rustls_echo ../../$BIN/tokio-rustls
            cp target/release/client ../../$BIN/client
            return 0
        fi
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
    local d
    if [[ $RIVALS != rust ]]; then
        for d in "${NATIVE_BUILDS[@]}"; do "$d/build.sh" "$ROOT/$BIN" >&2; done
    fi
}

start() { # server port: launches pinned in the background, waits until it accepts
    local cmd=("$BIN/$1" "$2" "$WORKERS") certdir
    case $1 in em-tls-uring|em-tls-epoll) cmd=(env "EM_BACKEND=${1#em-tls-}" "${cmd[@]}") # same binary, backend forced
               [[ -z ${TLS_PREFIX:-} ]] || cmd=(env "LD_LIBRARY_PATH=$(tls_prefix_lib)" "${cmd[@]}") ;;
               em-uring|em-epoll) cmd=(env "EM_BACKEND=${1#em-}" "${cmd[@]}") ;; esac
    case $1 in emrs-uring|emrs-epoll) cmd=(env "EM_BACKEND=${1#emrs-}" "$BIN/emrs" "$2" "$WORKERS") ;; esac
    # exact names: monoio-* would also match monoio-rustls-*
    case $1 in monoio-uring|monoio-epoll) cmd=("$BIN/monoio" "$2" "${1#monoio-}" "$WORKERS") ;;
               monoio-rustls-uring|monoio-rustls-epoll) cmd=("$BIN/monoio-rustls" "$2" "${1#monoio-rustls-}" "$WORKERS") ;;
               tokio-rustls-ct) cmd=(env TOKIO_RT=current_thread "$BIN/tokio-rustls" "$2" "$WORKERS") ;; esac
    # the HTTP servers: the TLS variant is the same binary with a last argument "tls" (h2o: the cert and key files).
    # The launchers exec the server: $pid is the server.
    case $1 in em-http-uring|em-http-epoll) cmd=(env "EM_BACKEND=${1#em-http-}" "${cmd[@]}") ;;
               em-http-tls-uring|em-http-tls-epoll) cmd=(env "EM_BACKEND=${1#em-http-tls-}" "$BIN/em-http-${1#em-http-tls-}" "$2" "$WORKERS" tls) ;;
               nginx-epoll) cmd=(bench/http/nginx.sh "$2" "$WORKERS") ;;
               nginx-tls-epoll) cmd=(bench/http/nginx.sh "$2" "$WORKERS" tls) ;;
               haproxy-epoll) cmd=(bench/http/haproxy.sh "$2" "$WORKERS") ;;
               haproxy-tls-epoll) cmd=(bench/http/haproxy.sh "$2" "$WORKERS" tls) ;;
               h2o-epoll) cmd=("$BIN/h2o-epoll" "$2" "$WORKERS") ;;
               h2o-tls-epoll) certdir=$(bench/http/cert.sh); cmd=("$BIN/h2o-epoll" "$2" "$WORKERS" "$certdir/cert.pem" "$certdir/key.pem") ;;
               hyper-epoll) cmd=("$BIN/hyper-http" "$2" "$WORKERS") ;;
               hyper-tls-epoll) cmd=("$BIN/hyper-http" "$2" "$WORKERS" tls) ;;
               gohttp-epoll) cmd=("$BIN/gohttp" "$2" "$WORKERS") ;;
               gohttp-tls-epoll) cmd=("$BIN/gohttp" "$2" "$WORKERS" tls) ;; esac
    # ONLY=h2: em-h2-* and em-h1-* are one binary (backend forced, "tls" last); the nginx and h2o launchers take "h2"
    case $1 in em-h2-*|em-h1-*) local rest=${1#em-h?-} tlsarg=()
                   [[ $rest != tls-* ]] || { tlsarg=(tls); rest=${rest#tls-}; }
                   cmd=(env "EM_BACKEND=$rest" "$BIN/em-http-h2" "$2" "$WORKERS" "${tlsarg[@]}") ;;
               nginx-h2-epoll) cmd=(bench/http/nginx.sh "$2" "$WORKERS" h2) ;;
               nginx-h2-tls-epoll) cmd=(bench/http/nginx.sh "$2" "$WORKERS" tls h2) ;;
               h2o-h2-epoll) cmd=("$BIN/h2o-epoll" "$2" "$WORKERS") ;;
               h2o-h2-tls-epoll) certdir=$(bench/http/cert.sh); cmd=("$BIN/h2o-epoll" "$2" "$WORKERS" "$certdir/cert.pem" "$certdir/key.pem" h2) ;; esac
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
    # HTTP cells: the client's second line is "http response_bytes=N" (header + body of the first response)
    if tls=$(sed -n 's/^http response_bytes=//p' "$OUT/rps.tmp") && [[ -n $tls ]]; then
        echo "$srv $mode: $tls B" >>"$OUT/http_info.txt"
    fi
    CELL_ROW="$rps,$cols,$p50,$p99,$p999,$res"
}

# run_cell_h2 srv port mode conns size: the h2load counterpart of run_cell, same CELL_ROW. h2load runs -D $DURATION seconds with
# $H2_STREAMS streams per connection (em-h1-* servers: --h1, one request in flight per connection). Requests = "succeeded"
# of h2load's summary; latency percentiles from --log-file (3rd column: us from request to end of response; the summary of
# nghttp2 1.59 has no percentiles). Server CPU as in run_cell. The negotiated TLS parameters (and the ALPN h2load saw) go to
# $OUT/tls_info.txt for the table's one-negotiated-set check.
run_cell_h2() {
    local srv=$1 port=$2 mode=$3 c=$4
    local TIMEFORMAT='%U %S %R' t t0 t1 u sy wall total rps p50 p99 p999 cols tls=http url=http://127.0.0.1:$port/ log=$OUT/h2load.log
    local a=(-c "$c" -m "$H2_STREAMS" -t "$CLI_THREADS" -D "$DURATION" --log-file="$OUT/h2req.log")
    # one negotiated set for every server: em prefers AES_128, nginx and h2o AES_256 (tls_info.txt check in table.py)
    [[ $mode != h2tls* ]] || { tls=https; url=https://127.0.0.1:$port/; a+=(--tls13-ciphers=TLS_AES_256_GCM_SHA384); }
    [[ $mode != *4k ]] || url+=4k
    [[ $srv != em-h1-* ]] || a+=(--h1)
    rm -f "$OUT/h2req.log"
    t0=$(cpu_ticks)
    t=$( { time taskset -c $CLI_CPUS h2load "${a[@]}" "$url" >"$log"; } 2>&1 ) || { echo "h2load failed for $srv c=$c $mode: $t" >&2; exit 1; }
    t1=$(cpu_ticks)
    read -r u sy wall <<<"$(tail -n1 <<<"$t")"
    total=$(sed -n 's/^requests: .* \([0-9]*\) succeeded, .*/\1/p' "$log")
    rps=$(sed -n 's/^finished in [^,]*, \([0-9.]*\) req\/s.*/\1/p' "$log")
    [[ ${total:-0} -gt 0 && -n $rps ]] || { echo "h2load: no succeeded requests for $srv c=$c $mode:" >&2; cat "$log" >&2; exit 1; }
    grep -q '^requests: .* 0 failed, 0 errored, 0 timeout' "$log" || echo "warn: $srv $mode c=$c: $(grep '^requests:' "$log")" >&2
    cols=$(awk -v u="$u" -v s="$sy" -v w="$wall" -v t0="$t0" -v t1="$t1" -v hz="$TICKS" -v n="$total" -v nc="$CLI_THREADS" \
        'BEGIN { sc = (t1 - t0) / hz; printf "%.2f,%.2f,%.2f", (u + s) / (w * nc), sc / w, sc * 1e6 / n }')
    read -r p50 p99 p999 < <(cut -f3 "$OUT/h2req.log" | sort -n | awk '{ v[NR] = $1 } END { if (!NR) exit 1;
        for (i = 1; i <= 3; i++) { k = int(NR * (i == 1 ? .5 : i == 2 ? .99 : .999)); printf "%s%d", (i > 1 ? " " : ""), v[k < 1 ? 1 : k] } print "" }') ||
        { echo "h2load: empty --log-file for $srv c=$c $mode" >&2; exit 1; }
    if [[ $tls == https ]]; then # "TLS Protocol: TLSv1.3", "Cipher: ...", "Server Temp Key: ECDH X25519 253 bits", "Application protocol: h2"
        echo "$srv $mode: $(sed -n 's/^\(TLS Protocol\|Cipher\|Server Temp Key\|Application protocol\): //p' "$log" | tr '\n' ' ')" >>"$OUT/tls_info.txt"
    fi
    CELL_ROW="$rps,$cols,$p50,$p99,$p999,-"
}

# bench/profile.sh and tls_ab.sh source this file for build/start/stop/run_cell and the settings above.
[[ ${BASH_SOURCE[0]} == "$0" ]] || return 0

build
mkdir -p "$OUT"
# echo cells, then one churn cell (connect, one 128 B round-trip, RST; 64 concurrent: plans/m5.md)
CELLS=()
if [[ $ONLY == h2 ]]; then
    # size = the expected body length. h2c / h2tls: GET / (13 B), h2c4k / h2tls4k: GET /4k, $H2_STREAMS streams per connection.
    # No churn cell yet (plans/http2.md M3).
    for c in 16 256; do CELLS+=("h2c $c 13" "h2c4k $c 4096" "h2tls $c 13" "h2tls4k $c 4096"); done
elif [[ $ONLY == http ]]; then
    # size = the expected body length. http: GET / keep-alive; httppipe: 16 requests per write; http4k: GET /4k;
    # httpchurn: connect + one GET / + RST, 64 concurrent like churn. Each round-trip is one request.
    for c in "${CONNS[@]}"; do CELLS+=("http $c 13"); done
    for c in 16 256; do CELLS+=("httppipe $c 13" "http4k $c 4096"); done
    CELLS+=("httpchurn 64 13")
    # TLS: httptls = GET / keep-alive over TLS (one handshake per connection); httpchurntls = connect + full handshake
    # (client resumption off) + one GET / + RST, 64 concurrent like churntls. Run by the -tls- servers.
    for c in 16 256; do CELLS+=("httptls $c 13"); done
    CELLS+=("httpchurntls 64 13")
elif [[ $ONLY != tls ]]; then
    for c in "${CONNS[@]}"; do for s in "${SIZES[@]}"; do CELLS+=("echo $c $s"); done; done
    CELLS+=("churn 64 128")
fi
# TLS cells: server us/rt of em-tls-* minus em-* in the same cell (same conns x size) is the cost
# of TLS (table.py). The mode names echotls/churntls keep them apart from the plain cells.
# churntls: client resumption off (every connection a full handshake); churntlsres: client keeps a
# session store and resumes (resumed = fraction of resumed handshakes, must be ~1 for both servers).
[[ $ONLY == http || $ONLY == h2 ]] || CELLS+=("echotls 256 128" "echotls 256 4096" "churntls 64 128" "churntlsres 64 128")
echo "round,mode,server,conns,size,rps,client_cpu,server_cpu,us_per_rt,p50_us,p99_us,p999_us,resumed" >"$OUT/results.csv"
: >"$OUT/tls_info.txt" # what each TLS server negotiated, as the client saw it
: >"$OUT/http_info.txt"
port=20000
for ((r = 1; r <= ROUNDS; r++)); do
    for cell in "${CELLS[@]}"; do
        read -r mode c s <<<"$cell"
        case $mode in echo) arg=() ;; churn) arg=(churn) ;; echotls) arg=(tls) ;; churntls) arg=(churn tls) ;; churntlsres) arg=(churn tlsres) ;;
                  http) arg=(http) ;; httppipe) arg=(http pipe=16) ;; http4k) arg=(http path=/4k) ;; httpchurn) arg=(churn http) ;;
                  httptls) arg=(http tls) ;; httpchurntls) arg=(churn http tls) ;; h2*) arg=() ;; esac
        srvs=("${SERVERS[@]}"); [[ $mode == *tls* ]] && srvs=("${TLS_SERVERS[@]}")
        # the order rotates with the round (round r starts at server r), so no server always runs right
        # after the same other server
        k=$(((r - 1) % ${#srvs[@]})); srvs=("${srvs[@]:k}" "${srvs[@]:0:k}")
        for srv in "${srvs[@]}"; do
            [[ $mode == churntlsres && " ${REF_SERVERS[*]:-} " == *" $srv "* ]] && continue
            port=$((port + 1))
            start "$srv" "$port"
            if [[ $ONLY == h2 ]]; then run_cell_h2 "$srv" "$port" "$mode" "$c"; else run_cell "$srv" "$port" "$mode" "$c" "$s" "${arg[@]}"; fi
            stop
            echo "$r,$mode,$srv,$c,$s,$CELL_ROW" | tee -a "$OUT/results.csv" >&2
        done
    done
done
rm -f "$OUT/rps.tmp" "$OUT/h2req.log" "$OUT/h2load.log"

# memory per idle TLS connection (own table), after the cells so a failure here keeps the cell results
if [[ $ONLY == http || $ONLY == h2 ]]; then : >"$OUT/rss.md"; else bench/rss_tls.sh | tee "$OUT/rss.md" >&2; fi

# table.py exits 1 when a check fails (missing rows, mixed negotiation, resumption < 0.95): the summary is
# still written, then the run fails
rc=0
if [[ $ONLY == http || $ONLY == h2 ]]; then
    python3 -I bench/table.py "$OUT/results.csv" "${SERVERS[@]}" --tls "${TLS_SERVERS[@]}" --ref "${REF_SERVERS[@]}" | tee "$OUT/table.md" || rc=$?
else
    python3 -I bench/table.py "$OUT/results.csv" "${SERVERS[@]}" --tls "${TLS_SERVERS[@]}" ${REF_SERVERS[@]:+--ref "${REF_SERVERS[@]}"} | tee "$OUT/table.md" || rc=$?
fi
[[ -z ${GITHUB_STEP_SUMMARY:-} ]] || { echo "### $([[ $ONLY == http || $ONLY == h2 ]] && echo "$ONLY" || echo echo) bench ($ARCH, ${DURATION}s x $ROUNDS rounds)"; cat "$OUT/table.md" "$OUT/rss.md"; } >>"$GITHUB_STEP_SUMMARY"
exit "$rc"
