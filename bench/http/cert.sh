#!/usr/bin/env bash
# The certificate of the TLS HTTP servers launched by bench/run.sh ONLY=http that read files (nginx, haproxy, h2o):
# self-signed ECDSA P-256 for "localhost" (SAN DNS:localhost), like tests/tls_util.h and the other servers
# generate in memory. Writes cert.pem, key.pem and bundle.pem (cert + key, for haproxy) into
# build/$(uname -m)/bench-http/ unless they exist, and prints that directory. Needs the openssl CLI.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
D=$ROOT/build/$(uname -m)/bench-http
if [[ ! -f $D/bundle.pem ]]; then
    mkdir -p "$D"
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -sha256 -nodes -days 3650 -subj /CN=localhost \
        -addext subjectAltName=DNS:localhost -keyout "$D/key.pem" -out "$D/cert.pem" 2>/dev/null
    cat "$D/cert.pem" "$D/key.pem" >"$D/bundle.pem.tmp" && mv "$D/bundle.pem.tmp" "$D/bundle.pem"
fi
echo "$D"
