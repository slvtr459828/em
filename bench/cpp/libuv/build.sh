#!/usr/bin/env bash
# libuv echo servers for bench/run.sh (plans/bench-cpp.md): OUTDIR/libuv-epoll and OUTDIR/libuv-tls-epoll.
# libuv v1.53.0 (pinned tarball, sha256-checked) is built static into build/$(uname -m)/cpp-deps/;
# the TLS server uses OpenSSL memory BIOs (bench/cpp/libuv/echo.c) against the system libssl, or against
# $TLS_PREFIX (an OpenSSL / AWS-LC prefix as built by bench/tls_libs.sh; its lib dir goes in the rpath).
# apt (Ubuntu 24.04): build-essential cmake curl libssl-dev
# usage: bench/cpp/libuv/build.sh OUTDIR
set -euo pipefail
[[ $# -eq 1 ]] || { echo "usage: $0 OUTDIR" >&2; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
mkdir -p "$1"
OUT=$(cd "$1" && pwd)
VER=1.53.0
SHA=cb0d6dd2128d5a95bd242c6cc982a24fe608fa93da57b6b4ec763b0018c53e64
D=$ROOT/build/$(uname -m)/cpp-deps/libuv-$VER

if [[ ! -f $D/.done ]]; then
    echo "== building libuv $VER" >&2
    rm -rf "$D"; mkdir -p "$D/src"
    curl -fsSL -o "$D/src.tar.gz" "https://dist.libuv.org/dist/v$VER/libuv-v$VER.tar.gz"
    echo "$SHA  $D/src.tar.gz" | sha256sum -c - >&2
    tar xzf "$D/src.tar.gz" -C "$D/src" --strip-components=1
    cmake -S "$D/src" -B "$D/b" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" \
        -DLIBUV_BUILD_SHARED=OFF -DLIBUV_BUILD_TESTS=OFF -DLIBUV_BUILD_BENCH=OFF -DBUILD_TESTING=OFF \
        -DCMAKE_INSTALL_PREFIX="$D/inst" -DCMAKE_INSTALL_LIBDIR=lib >&2
    cmake --build "$D/b" -j"$(nproc)" >&2
    cmake --install "$D/b" >&2
    rm -rf "$D/src" "$D/b" "$D/src.tar.gz"
    touch "$D/.done"
fi

cf=(-std=gnu11 -O2 -g -Wall -Wextra -I"$D/inst/include")
ssl=(-lssl -lcrypto)
if [[ -n ${TLS_PREFIX:-} ]]; then
    lib=
    for d in lib64 lib; do [[ -e $TLS_PREFIX/$d/libssl.so ]] && { lib=$TLS_PREFIX/$d; break; }; done
    [[ -n $lib ]] || { echo "TLS_PREFIX=$TLS_PREFIX: no lib64/libssl.so or lib/libssl.so" >&2; exit 1; }
    tcf=("${cf[@]}" -I"$TLS_PREFIX/include")
    ssl=(-L"$lib" -Wl,-rpath,"$lib" -lssl -lcrypto)
else
    tcf=("${cf[@]}")
fi
cc "${cf[@]}" "$HERE/echo.c" "$D/inst/lib/libuv.a" -lpthread -ldl -lrt -o "$OUT/libuv-epoll"
cc "${tcf[@]}" -DWITH_TLS "$HERE/echo.c" "$D/inst/lib/libuv.a" "${ssl[@]}" -lpthread -ldl -lrt -o "$OUT/libuv-tls-epoll"
