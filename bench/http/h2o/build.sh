#!/usr/bin/env bash
# Builds OUTDIR/h2o-epoll for bench/run.sh ONLY=http (plans/http.md): bench/http/h2o/hello.c linked with libh2o
# built from source. h2o master at the pinned commit (2.3.0-DEV: the last tag, v2.3.0-beta2, is from 2020);
# its evloop is epoll on Linux. mruby, libuv and the h2o binary are not built.
# apt (Ubuntu 24.04, x86 and arm64): build-essential cmake git pkg-config libssl-dev zlib1g-dev
# The library and the headers hello.c needs go to build/$(uname -m)/http-deps/h2o-$COMMIT/ (cached by
# .github/workflows/bench.yml); the source and build trees are deleted after the build.
# usage: bench/http/h2o/build.sh OUTDIR
set -euo pipefail
[[ $# -eq 1 ]] || { echo "usage: $0 OUTDIR" >&2; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
mkdir -p "$1"
OUT=$(cd "$1" && pwd)
COMMIT=d19f742b00a4bb71207524f2eed99464854d24cd # 2026-11-11
D=$ROOT/build/$(uname -m)/http-deps/h2o-$COMMIT

if [[ ! -f $D/.done ]]; then
    echo "== building libh2o $COMMIT" >&2
    rm -rf "$D"; mkdir -p "$D/src"
    git -C "$D/src" init -q
    git -C "$D/src" fetch -q --depth 1 https://github.com/h2o/h2o.git "$COMMIT"
    git -C "$D/src" checkout -q FETCH_HEAD
    [[ $(git -C "$D/src" rev-parse HEAD) == "$COMMIT" ]] || { echo "h2o: fetched commit differs from $COMMIT" >&2; exit 1; }
    git -C "$D/src" submodule update --init --recursive --depth 1 -q >&2
    cmake -S "$D/src" -B "$D/b" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DWITH_MRUBY=OFF -DDISABLE_LIBUV=ON >&2
    cmake --build "$D/b" -j"$(nproc)" --target libh2o-evloop >&2
    mkdir -p "$D/inst"
    cp "$D/b/libh2o-evloop.a" "$D/inst/"
    for i in include deps/picohttpparser deps/cloexec deps/klib deps/quicly/include deps/picotls/include deps/golombset; do
        mkdir -p "$D/inst/$i" && cp -r "$D/src/$i/." "$D/inst/$i/"
    done
    rm -rf "$D/src" "$D/b"
    touch "$D/.done"
fi

I=$D/inst
cc -std=gnu11 -O2 -g -Wall -Wextra -D_GNU_SOURCE -DH2O_USE_LIBUV=0 \
    -I"$I/include" -I"$I/deps/picohttpparser" -I"$I/deps/cloexec" -I"$I/deps/klib" -I"$I/deps/quicly/include" \
    -I"$I/deps/picotls/include" -I"$I/deps/golombset" \
    "$HERE/hello.c" "$I/libh2o-evloop.a" -lssl -lcrypto -lz -lpthread -lm -ldl -o "$OUT/h2o-epoll"
