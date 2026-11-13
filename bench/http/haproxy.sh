#!/usr/bin/env bash
# haproxy for bench/run.sh ONLY=http (plans/http.md): `http-request return` for the two paths of the common
# contract, nbthread 1, no logs, keep-alive. The 4096 B body comes from a file that haproxy loads into memory at
# startup (a config line is limited to 2 KiB). usage: bench/http/haproxy.sh port [workers [tls]]; execs haproxy in
# the foreground (apt: haproxy). The config is generated into build/$(uname -m)/bench-http/haproxy-$port/. tls: HTTPS
# with the certificate of bench/http/cert.sh, TLS 1.2+, TLS 1.3 suite AES-256-GCM only, group X25519 only, ALPN http/1.1 only
# (alpn http/1.1: haproxy offers h2 by default): the policy of every TLS server of the bench.
set -euo pipefail
PORT=${1:?usage: $0 port [workers]}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
D=$ROOT/build/$(uname -m)/bench-http/haproxy-$PORT
mkdir -p "$D"
bind=''
if [[ ${3:-} == tls ]]; then
    bind=" ssl crt $("$ROOT/bench/http/cert.sh")/bundle.pem ssl-min-ver TLSv1.2 ciphersuites TLS_AES_256_GCM_SHA384 curves X25519 alpn http/1.1"
elif [[ -n ${3:-} ]]; then echo "usage: $0 port [workers [tls]]" >&2; exit 2; fi
head -c 4096 /dev/zero | tr '\0' x >"$D/4k.txt"
cat >"$D/haproxy.cfg" <<CONF
global
    nbthread ${2:-1}
    maxconn 20000
defaults
    mode http
    timeout client 3600s
    timeout server 3600s
    timeout connect 5s
frontend f
    bind 127.0.0.1:$PORT$bind
    http-request return status 200 content-type text/plain string "Hello, World!" if { path / }
    http-request return status 200 content-type text/plain file $D/4k.txt if { path /4k }
    http-request return status 404
CONF
exec haproxy -db -f "$D/haproxy.cfg"
