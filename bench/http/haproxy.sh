#!/usr/bin/env bash
# haproxy for bench/run.sh ONLY=http (plans/http.md): `http-request return` for the two paths of the common
# contract, nbthread 1, no logs, keep-alive. The 4096 B body comes from a file that haproxy loads into memory at
# startup (a config line is limited to 2 KiB). usage: bench/http/haproxy.sh port [workers]; execs haproxy in
# the foreground (apt: haproxy). The config is generated into build/$(uname -m)/bench-http/haproxy-$port/.
set -euo pipefail
PORT=${1:?usage: $0 port [workers]}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
D=$ROOT/build/$(uname -m)/bench-http/haproxy-$PORT
mkdir -p "$D"
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
    bind 127.0.0.1:$PORT
    http-request return status 200 content-type text/plain string "Hello, World!" if { path / }
    http-request return status 200 content-type text/plain file $D/4k.txt if { path /4k }
    http-request return status 404
CONF
exec haproxy -db -f "$D/haproxy.cfg"
