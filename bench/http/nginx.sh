#!/usr/bin/env bash
# nginx for bench/run.sh ONLY=http (plans/http.md): `return 200` for the two paths of the common contract,
# from memory (no file, no upstream), 1 worker, no logs, keep-alive without a request limit.
# usage: bench/http/nginx.sh port [workers]; execs nginx in the foreground (apt: nginx). The config is
# generated into build/$(uname -m)/bench-http/nginx-$port/.
set -euo pipefail
PORT=${1:?usage: $0 port [workers]}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
D=$ROOT/build/$(uname -m)/bench-http/nginx-$PORT
mkdir -p "$D"
# a config token is limited to the 4 KiB parse buffer: the 4096 B body is 4 x 1024 through a variable
k=$(head -c 1024 /dev/zero | tr '\0' x)
cat >"$D/nginx.conf" <<CONF
worker_processes ${2:-1};
daemon off;
pid $D/nginx.pid;
error_log /dev/null crit;
events { worker_connections 8192; }
http {
    access_log off;
    default_type text/plain;
    keepalive_requests 1000000000;
    keepalive_timeout 3600s;
    client_body_temp_path $D/body; proxy_temp_path $D/proxy; fastcgi_temp_path $D/fastcgi;
    uwsgi_temp_path $D/uwsgi; scgi_temp_path $D/scgi;
    server {
        listen 127.0.0.1:$PORT backlog=4096;
        set \$k "$k";
        location = / { return 200 "Hello, World!"; }
        location = /4k { return 200 "\$k\$k\$k\$k"; }
    }
}
CONF
exec nginx -p "$D" -e /dev/null -c "$D/nginx.conf"
