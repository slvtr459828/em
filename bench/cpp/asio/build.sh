#!/usr/bin/env bash
# Builds the Asio echo servers (plans/bench-cpp.md): asio-{epoll,uring}, asio-tls-{epoll,uring} into OUTDIR.
# Standalone Asio 1.38.2 (header-only; the same code as Boost.Asio), downloaded to
# build/$(uname -m)/cpp-deps/asio-1.38.2 and pinned by the sha256 of the uncompressed tar (the gzip bytes
# of a GitHub archive are not stable). Boost is not needed. -O2, C++20 coroutines.
# env: TLS_PREFIX: an OpenSSL/AWS-LC prefix (include/, lib[64]/): the TLS servers compile and link against it
#      (rpath baked in, so they never fall back to the system libssl); unset = the system libssl.
# Ubuntu 24.04 apt: g++ (13, C++20 coroutines), gcc, curl, ca-certificates, libssl-dev, liburing-dev, pkg-config
set -euo pipefail
OUTDIR=${1:?usage: build.sh OUTDIR}
cd "$(dirname "$0")/../../.."
ROOT=$PWD
HERE=$ROOT/bench/cpp/asio
VER=1.38.2
SHA=c4663275983f145b2302ac15648bbcefb8c87e4715e2df787e5e9ad2e5f46fa1
DEPS=$ROOT/build/$(uname -m)/cpp-deps/asio-$VER
mkdir -p "$OUTDIR"
OUTDIR=$(cd "$OUTDIR" && pwd)

if [[ ! -f $DEPS/.done ]]; then
    echo "== fetching asio-$VER" >&2
    rm -rf "$DEPS"; mkdir -p "$DEPS"
    curl -fsSL -o "$DEPS/src.tar.gz" "https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-${VER//./-}.tar.gz" 2>/dev/null ||
        curl -fsSL -o "$DEPS/src.tar.gz" "https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-${VER//./-}.tar.gz"
    [[ $(gzip -dc "$DEPS/src.tar.gz" | sha256sum) == "$SHA  -" ]] || { echo "asio-$VER: sha256 mismatch" >&2; exit 1; }
    tar xzf "$DEPS/src.tar.gz" -C "$DEPS" --strip-components=1
    rm "$DEPS/src.tar.gz"
    touch "$DEPS/.done"
fi

if [[ -n ${TLS_PREFIX:-} ]]; then
    lib=
    for d in lib64 lib; do [[ -e $TLS_PREFIX/$d/libssl.so ]] && { lib=$TLS_PREFIX/$d; break; }; done
    [[ -n $lib ]] || { echo "TLS_PREFIX=$TLS_PREFIX: no lib64/libssl.so or lib/libssl.so" >&2; exit 1; }
    TLS_CF=(-I"$TLS_PREFIX/include")
    TLS_LD=(-L"$lib" -Wl,-rpath,"$lib" -lssl -lcrypto)
else
    read -ra TLS_CF <<<"$(pkg-config --cflags openssl)"
    read -ra TLS_LD <<<"$(pkg-config --libs openssl)"
fi

CXXF=(-std=c++20 -O2 -g -Wall -Wextra -Wno-mismatched-new-delete -I"$DEPS/asio/include" -pthread)
URING=(-DASIO_HAS_IO_URING -DASIO_DISABLE_EPOLL)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
echo "== building asio $VER servers" >&2
cc -std=gnu11 -O2 -g -Wall -Wextra "${TLS_CF[@]}" -c "$HERE/tls_ctx.c" -o "$T/tls_ctx.o"
pids=()
c++ "${CXXF[@]}" "$HERE/echo.cpp" -o "$OUTDIR/asio-epoll" & pids+=($!)
c++ "${CXXF[@]}" "${URING[@]}" "$HERE/echo.cpp" -o "$OUTDIR/asio-uring" -luring & pids+=($!)
c++ "${CXXF[@]}" -DTLS "${TLS_CF[@]}" "$HERE/echo.cpp" "$T/tls_ctx.o" -o "$OUTDIR/asio-tls-epoll" "${TLS_LD[@]}" & pids+=($!)
c++ "${CXXF[@]}" "${URING[@]}" -DTLS "${TLS_CF[@]}" "$HERE/echo.cpp" "$T/tls_ctx.o" -o "$OUTDIR/asio-tls-uring" -luring "${TLS_LD[@]}" & pids+=($!)
rc=0; for p in "${pids[@]}"; do wait "$p" || rc=1; done
exit $rc
