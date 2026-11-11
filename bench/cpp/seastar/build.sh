#!/usr/bin/env bash
# Seastar echo servers for bench/run.sh (plans/bench-cpp.md). usage: build.sh OUTDIR
# Builds Seastar (pinned below) from source and bench/cpp/seastar/echo.cc against it, then puts
# seastar-{uring,epoll} (plain) and seastar-tls-{uring,epoll} (seastar::tls, GnuTLS) into OUTDIR.
# The four names are copies of two binaries; the binary reads its backend (-uring/-epoll) from argv[0].
# apt (Ubuntu 24.04; Seastar install-dependencies.sh minus apps, tests, docs, dpdk; valgrind only for a CMake check):
#   build-essential cmake ninja-build pkg-config git python3 ragel libboost-all-dev libc-ares-dev
#   libfmt-dev libgnutls28-dev libhwloc-dev liblz4-dev libnuma-dev libpciaccess-dev libsctp-dev
#   liburing-dev libyaml-cpp-dev libxml2-dev systemtap-sdt-dev libprotobuf-dev protobuf-compiler valgrind xfslibs-dev
set -euo pipefail
OUT=${1:?usage: build.sh OUTDIR}
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
HERE=$ROOT/bench/cpp/seastar
DEPS=$ROOT/build/$(uname -m)/cpp-deps
SEASTAR_REV=8df8212e53577e1d8477a5c901457cd61d88afc7 # tag seastar-25.05.0
SRC=$DEPS/seastar-src
PREFIX=$DEPS/seastar-install
mkdir -p "$DEPS" "$OUT"

if [[ ! -f $PREFIX/.rev || $(cat "$PREFIX/.rev") != "$SEASTAR_REV" ]]; then
    rm -rf "$SRC" "$DEPS/seastar-build" "$PREFIX"
    git init -q "$SRC"
    git -C "$SRC" fetch -q --depth 1 https://github.com/scylladb/seastar "$SEASTAR_REV"
    git -C "$SRC" checkout -q FETCH_HEAD # no submodules: dpdk and friends are not used
    cmake -S "$SRC" -B "$DEPS/seastar-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DSeastar_IO_URING=ON -DSeastar_APPS=OFF -DSeastar_DEMOS=OFF -DSeastar_DOCS=OFF -DSeastar_TESTING=OFF >&2
    cmake --build "$DEPS/seastar-build" >&2
    cmake --install "$DEPS/seastar-build" >&2
    echo "$SEASTAR_REV" >"$PREFIX/.rev"
fi

cmake -S "$HERE" -B "$DEPS/seastar-echo-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" -DCMAKE_PREFIX_PATH="$PREFIX" >&2
cmake --build "$DEPS/seastar-echo-build" >&2
for b in uring epoll; do
    cp "$DEPS/seastar-echo-build/seastar_echo" "$OUT/seastar-$b"
    cp "$DEPS/seastar-echo-build/seastar_tls_echo" "$OUT/seastar-tls-$b"
done
