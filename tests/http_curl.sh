#!/usr/bin/env bash
# Smoke test: bench/em_http (the em HTTP server) against a real client, curl, on every backend in
# BACKENDS (default "uring epoll"), then the same server over TLS (bench/em_http "tls", curl -k; needs
# OpenSSL, as building bench/em_http does). Exits non-zero on the first failed check.
# usage: tests/http_curl.sh        BACKENDS=epoll tests/http_curl.sh
set -euo pipefail
cd "$(dirname "$0")/.."
BACKENDS=${BACKENDS:-"uring epoll"}
ARCH=$(uname -m)
make -s bench-em-http
SRV=build/$ARCH/rel/em_http
T=$(mktemp -d)
pid=
cleanup() { [[ -z $pid ]] || kill "$pid" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT

fail() { echo "FAIL [$backend]: $*" >&2; sed 's/^/  server: /' "$T/srv.log" >&2 || true; exit 1; }
check() { # check <what> <command...>: the command is the assertion
    local what=$1; shift
    "$@" >/dev/null 2>&1 || fail "$what"
    echo "ok   [$backend] $what"
}

head -c 200000 /dev/urandom > "$T/body200k"
head -c 2097152 /dev/urandom > "$T/body2m"

start() { # start [tls]: sets port, pid
    port=$((20000 + RANDOM % 12000)) # below the ephemeral range (32768+): no clash with a client socket
    EM_BACKEND=$backend "$SRV" "$port" 1 ${1:-} > "$T/srv.log" 2>&1 &
    pid=$!
    for _ in {1..50}; do
        kill -0 "$pid" 2>/dev/null || fail "server exited during startup"
        (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null && break
        sleep 0.1
    done
}

for backend in $BACKENDS; do
    start
    u=http://127.0.0.1:$port
    c="curl -sS --max-time 20"

    for v in 1.0 1.1; do
        $c --http$v -D "$T/h" -o "$T/o" "$u/" || fail "GET --http$v: curl rc=$?"
        [[ $(< "$T/o") == "Hello, World!" ]] && head -1 "$T/h" | grep -q "^HTTP/$v 200" || fail "GET --http$v: $(head -1 "$T/h")"
        echo "ok   [$backend] GET --http$v"
    done

    # two URLs in one run: curl must reuse the connection
    $c -v --http1.1 "$u/" "$u/4k" -o /dev/null -o "$T/o4k" 2> "$T/v" || fail "keep-alive: curl rc=$?"
    grep -Eqi "re-?using existing" "$T/v" || fail "keep-alive: connection not reused"
    [[ $(wc -c < "$T/o4k") == 4096 ]] || fail "keep-alive: second response is not 4096 bytes"
    echo "ok   [$backend] keep-alive (2 requests, 1 connection)"

    $c --data-binary @"$T/body200k" -o "$T/o" "$u/echo" || fail "POST Content-Length: curl rc=$?"
    check "POST Content-Length 200 KB echo identical" cmp "$T/body200k" "$T/o"

    $c -H 'Transfer-Encoding: chunked' --data-binary @"$T/body200k" -o "$T/o" "$u/echo" || fail "chunked upload: curl rc=$?"
    check "chunked upload 200 KB echo identical" cmp "$T/body200k" "$T/o"

    $c -H 'Expect: 100-continue' --data-binary @"$T/body2m" -o "$T/o" "$u/echo" || fail "100-continue: curl rc=$?"
    check "Expect: 100-continue 2 MB echo identical" cmp "$T/body2m" "$T/o"

    # HEAD twice on one connection: a body leaking after the head would break the second response
    $c -I "$u/" "$u/4k" > "$T/head" || fail "HEAD: curl rc=$?"
    [[ $(grep -ci '^Content-Length: ' "$T/head") == 2 ]] && grep -qi '^Content-Length: 4096' "$T/head" || fail "HEAD: wrong headers: $(cat "$T/head")"
    exec 3<>"/dev/tcp/127.0.0.1/$port"
    printf 'HEAD /4k HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' >&3
    cat <&3 > "$T/raw"
    exec 3<&-
    [[ $(tail -c 4 "$T/raw" | od -An -c | tr -d ' ') == '\r\n\r\n' ]] || fail "HEAD: bytes after the head"
    echo "ok   [$backend] HEAD: Content-Length, no body"

    kill "$pid"; wait "$pid" 2>/dev/null || true; pid=

    start tls
    u=https://localhost:$port
    c="curl -sS -k --max-time 20 --resolve localhost:$port:127.0.0.1"
    $c -v --http1.1 "$u/" "$u/4k" -o "$T/o1" -o "$T/o4k" 2> "$T/v" || fail "https keep-alive: curl rc=$?"
    [[ $(< "$T/o1") == "Hello, World!" && $(wc -c < "$T/o4k") == 4096 ]] || fail "https keep-alive: wrong bodies"
    grep -q "SSL connection using" "$T/v" || fail "https: not TLS"
    [[ $(grep -ci "SSL connection using" "$T/v") == 1 ]] || fail "https keep-alive: handshake done twice"
    grep -Eqi "re-?using existing" "$T/v" || fail "https keep-alive: connection not reused"
    echo "ok   [$backend] https keep-alive (2 requests, 1 handshake)"
    $c --data-binary @"$T/body200k" -o "$T/o" "$u/echo" || fail "https POST Content-Length: curl rc=$?"
    check "https POST Content-Length 200 KB echo identical" cmp "$T/body200k" "$T/o"
    $c -H 'Transfer-Encoding: chunked' --data-binary @"$T/body200k" -o "$T/o" "$u/echo" || fail "https chunked upload: curl rc=$?"
    check "https chunked upload 200 KB echo identical" cmp "$T/body200k" "$T/o"
    $c -H 'Expect: 100-continue' --data-binary @"$T/body2m" -o "$T/o" "$u/echo" || fail "https 100-continue: curl rc=$?"
    check "https Expect: 100-continue 2 MB echo identical" cmp "$T/body2m" "$T/o"
    kill "$pid"; wait "$pid" 2>/dev/null || true; pid=
done
echo "http_curl: all ok ($BACKENDS)"
