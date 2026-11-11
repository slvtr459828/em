#!/usr/bin/env bash
# Go echo servers for bench/run.sh (plans/bench-cpp.md): OUTDIR/go-epoll and OUTDIR/go-tls-epoll.
# Go 1.27.3 (pinned official tarball, sha256-checked, picked by uname -m) is unpacked into
# build/$(uname -m)/cpp-deps/go-1.27.3; the distro's Go is never used. Stdlib only, default flags.
# TLS_PREFIX does not apply: Go uses its own crypto/tls (no libssl).
# apt (Ubuntu 24.04): curl ca-certificates tar gzip
# usage: bench/go/build.sh OUTDIR
set -euo pipefail
[[ $# -eq 1 ]] || { echo "usage: $0 OUTDIR" >&2; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
mkdir -p "$1"
OUT=$(cd "$1" && pwd)
VER=1.27.3
case $(uname -m) in
    x86_64)  A=amd64 SHA=65efbc80fdc1fa2cd2631cad5b481777a68e8725f34e89f50fc6ac714e379b40 ;;
    aarch64) A=arm64 SHA=a06e99309dbcbdca1a8ccd2282c3ee146e55930886e26b4f1193269587165f49 ;;
    *) echo "unsupported arch $(uname -m)" >&2; exit 1 ;;
esac
D=$ROOT/build/$(uname -m)/cpp-deps/go-$VER

if [[ ! -f $D/.done ]]; then
    echo "== fetching go $VER" >&2
    rm -rf "$D"; mkdir -p "$D"
    curl -fsSL -o "$D/go.tar.gz" "https://go.dev/dl/go$VER.linux-$A.tar.gz"
    echo "$SHA  $D/go.tar.gz" | sha256sum -c - >&2
    tar xzf "$D/go.tar.gz" -C "$D"
    rm "$D/go.tar.gz"
    touch "$D/.done"
fi

export GOROOT=$D/go GOTOOLCHAIN=local GOFLAGS=-mod=readonly GOCACHE=$D/cache CGO_ENABLED=0
cd "$HERE"
"$GOROOT/bin/go" build -trimpath -o "$OUT/go-epoll" .
"$GOROOT/bin/go" build -trimpath -tags tls -o "$OUT/go-tls-epoll" .
