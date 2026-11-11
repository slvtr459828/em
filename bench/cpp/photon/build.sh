#!/usr/bin/env bash
# Builds the Photon (alibaba/PhotonLibOS) echo servers of bench/run.sh (plans/bench-cpp.md) into OUTDIR:
#   photon-uring photon-epoll photon-tls-uring photon-tls-epoll   (usage: binary port [workers])
# usage: bench/cpp/photon/build.sh OUTDIR       env: TLS_PREFIX (OpenSSL 3.x or AWS-LC prefix, like run.sh)
# apt (Ubuntu 24.04, x86 and arm64): build-essential cmake curl ca-certificates libssl-dev zlib1g-dev
#   libssl-dev is the TLS library when TLS_PREFIX is unset. Photon's other dependencies are built from
#   pinned source below: liburing 2.3 (the version Photon's CMake insists on, Ubuntu ships 2.5) and
#   libaio 0.3.113 (Photon links it unconditionally; libaio-dev is not needed).
# Downloads and builds go to build/$(uname -m)/cpp-deps/photon/. With TLS_PREFIX, Photon and the TLS
# servers are built against the prefix headers and the servers carry its lib dir as rpath.
set -euo pipefail
OUT=$(mkdir -p "${1:?usage: $0 OUTDIR}" && cd "$1" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE/../../.."
ROOT=$PWD
ARCH=$(uname -m)
D=$ROOT/build/$ARCH/cpp-deps/photon
JOBS=$(nproc)

# name url sha256-of-uncompressed-tar (the gzip bytes of a GitHub archive are not stable)
PHOTON_URL=https://github.com/alibaba/PhotonLibOS/archive/dd236e97866fd3d4cbff64b7ada7b69ef9464695.tar.gz # tag v0.9.6
PHOTON_SHA=41b3e14d15e39c3022a70594eb1bf588e1eb69254a634ce3db867abe019d4ce7
URING_URL=https://github.com/axboe/liburing/archive/refs/tags/liburing-2.3.tar.gz
URING_SHA=6b0eeb3d826f8d5dc057e6e2c84c0874331fa50dac2120b02c2ee6b52bcf9a18
# pagure.io, where Photon fetches libaio, serves 404 for it today: Debian's orig tarball is the same release
AIO_URL=https://ftp.debian.org/debian/pool/main/liba/libaio/libaio_0.3.113.orig.tar.gz
AIO_SHA=1feaada6e59e8b58c3d585a9608cedd8a48057863a346a3ea7db5ae1554ea4d5

fetch() { # url sha dir: unpacks into dir unless dir/.done exists
    [[ -e $3/.done ]] && return 0
    rm -rf "$3" && mkdir -p "$3"
    curl -fsSL -o "$3.tgz" "$1"
    [[ $(gzip -dc "$3.tgz" | sha256sum) == "$2  -" ]] || { echo "$1: sha256 mismatch" >&2; return 1; }
    tar xzf "$3.tgz" -C "$3" --strip-components=1 && rm "$3.tgz"
}

# liburing 2.3 and libaio: static libs and headers in $D/prefix
if [[ ! -e $D/prefix/.done ]]; then
    fetch "$URING_URL" "$URING_SHA" "$D/liburing-src" >&2
    fetch "$AIO_URL" "$AIO_SHA" "$D/libaio-src" >&2
    mkdir -p "$D/prefix"
    # liburing 2.3's compat.h defines struct open_how, which newer kernel headers (Fedora 44) also define
    (cd "$D/liburing-src" && ./configure --prefix="$D/prefix" >&2 &&
        make -C src -j"$JOBS" CFLAGS="-fPIC -O3 -Wall -Wextra -fno-stack-protector -include linux/openat2.h" >&2 &&
        make install >&2)
    (cd "$D/libaio-src" && make -j"$JOBS" prefix="$D/prefix" install >&2)
    touch "$D/prefix/.done"
fi

# OpenSSL flavour
if [[ -n ${TLS_PREFIX:-} ]]; then
    SSL_INC=$TLS_PREFIX/include
    SSL_LIB=
    for d in lib64 lib; do [[ -e $TLS_PREFIX/$d/libssl.so ]] && { SSL_LIB=$TLS_PREFIX/$d; break; }; done
    [[ -n $SSL_LIB ]] || { echo "TLS_PREFIX=$TLS_PREFIX: no lib64/libssl.so or lib/libssl.so" >&2; exit 1; }
    FLAVOUR=$(basename "$TLS_PREFIX")
    RPATH=(-Wl,-rpath,"$SSL_LIB")
else
    SSL_INC=/usr/include
    SSL_LIB=$(dirname "$(cc -print-file-name=libssl.so)")
    FLAVOUR=system
    RPATH=()
fi
# Photon's tls-stream.cpp against AWS-LC: ERR_R_DISABLED is missing (only used as a `case` label that
# maps to errno EIO; any value AWS-LC never produces does the same) and deprecated/no-op OpenSSL init
# calls trip Photon's -Werror. Compiler flags only: Photon's source stays as downloaded.
PCXXF=-fno-strict-aliasing
if grep -qs OPENSSL_IS_AWSLC "$SSL_INC/openssl/base.h"; then
    PCXXF="$PCXXF -DERR_R_DISABLED=4095 -Wno-error=deprecated-declarations -Wno-error=unused-value"
fi

# Photon, once per OpenSSL flavour (tls-stream.cpp is compiled against its headers). The default
# event engine list covers epoll and io_uring: the binaries pick one at run time. libcurl is off
# (the HTTP client is not used); Photon's CMake finds liburing and libaio through CMAKE_PREFIX_PATH.
SRC=$D/photon-src
PB=$D/photon-build-$FLAVOUR
fetch "$PHOTON_URL" "$PHOTON_SHA" "$SRC" >&2 && touch "$SRC/.done"
if [[ ! -e $PB/.done ]]; then
    rm -rf "$PB"
    # -fno-strict-aliasing: common/PMF.h type-puns member function pointers, GCC 16 rejects it under
    # Photon's -Werror and -O2 may exploit it; the flag is correctness, not tuning
    cmake -S "$SRC" -B "$PB" -DCMAKE_CXX_FLAGS="$PCXXF" -DCMAKE_BUILD_TYPE=Release -DPHOTON_ENABLE_URING=ON -DPHOTON_ENABLE_LIBCURL=OFF \
        -DCMAKE_PREFIX_PATH="$D/prefix" -DOPENSSL_INCLUDE_DIRS="$SSL_INC" \
        -DOPENSSL_SSL_LIBRARIES="$SSL_LIB/libssl.so" -DOPENSSL_CRYPTO_LIBRARIES="$SSL_LIB/libcrypto.so" >&2
    cmake --build "$PB" -j"$JOBS" >&2
    touch "$PB/.done"
fi

# Photon's own architecture flags (its CMakeLists.txt): the headers use these intrinsics
case $ARCH in
    x86_64) AF=(-msse3 -mssse3 -msse4.1 -msse4.2 -mpopcnt -mcx16) ;;
    *) AF=(-fsigned-char -mcpu=generic+crc+crypto+lse) ;;
esac
CXXF=(-std=c++14 -O2 -Wall "${AF[@]}" -faligned-new -fno-strict-aliasing -fno-stack-protector -I"$PB/include" -I"$SSL_INC")
LIBS=("$PB/output/libphoton.a" "$D/prefix/lib/liburing.a" "$D/prefix/lib/libaio.a" -L"$SSL_LIB")
SYS=(-lz -lpthread -lrt -ldl)

T=$D/obj-$FLAVOUR
mkdir -p "$T"
cc -std=gnu11 -O2 -Wall -Wextra -I"$SSL_INC" -c "$HERE/cert.c" -o "$T/cert.o" >&2
for be in uring epoll; do
    D_BE=(); [[ $be == uring ]] && D_BE=(-DURING)
    c++ "${CXXF[@]}" "${D_BE[@]}" "$HERE/echo.cpp" "${LIBS[@]}" "${SYS[@]}" -o "$OUT/photon-$be" >&2
    c++ "${CXXF[@]}" "${D_BE[@]}" -DTLS "$HERE/echo.cpp" "$T/cert.o" "${LIBS[@]}" -lssl -lcrypto "${SYS[@]}" "${RPATH[@]}" \
        -o "$OUT/photon-tls-$be" >&2
done
