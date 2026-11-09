#!/usr/bin/env bash
# TLS library microbench (plans/tls-perf.md, step 2): builds OpenSSL 3.0, OpenSSL 3.5 and AWS-LC from
# pinned release tarballs, links bench/tls_lib.c against each (and the system OpenSSL), runs it and the
# rustls-ring twin (bench/rust/tls_lib_rs) over every op x ticket mode x variant, and compares the
# primitives with `openssl speed` / `bssl speed`.
#   $OUT/tls_lib.csv  lib,op,tickets,variant,ns_per_op,instr_per_op,cycles_per_op
#   $OUT/tls_lib.md   the same with ratios vs rustls-ring (also appended to $GITHUB_STEP_SUMMARY)
#   $OUT/speed.txt    openssl speed / bssl speed per library
# Prefixes: build/$(uname -m)/tlslibs/<name>/{include,lib,bin} (other tools rely on this layout).
# env: ITERS (5000, per op and variant), OUT (bench/out/tls-lib), LIBS (default: all of $ALL)
set -euo pipefail
cd "$(dirname "$0")/.."
ITERS=${ITERS:-5000}
OUT=${OUT:-bench/out/tls-lib}
ROOT=$PWD/build/$(uname -m)/tlslibs
ALL="openssl-3.0.13 openssl-3.5.9 aws-lc-5.12.0 system"
LIBS=(${LIBS:-$ALL})

# name url sha256. OpenSSL: sha256 of the .tar.gz, same as upstream's .sha256 asset. AWS-LC publishes no
# checksum and no asset: the source is the archive of tag v5.12.0 (latest release at least 2 weeks old
# on 2026-11-09), pinned by the sha256 of the uncompressed tar (the gzip bytes of an archive are not stable).
declare -A URL=(
    [openssl-3.0.13]=https://github.com/openssl/openssl/releases/download/openssl-3.0.13/openssl-3.0.13.tar.gz
    [openssl-3.5.9]=https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz
    [aws-lc-5.12.0]=https://github.com/aws/aws-lc/archive/refs/tags/v5.12.0.tar.gz
)
declare -A SHA=(
    [openssl-3.0.13]=88525753f79d3bec27d2fa7c66aa0b92b3aa9498dafd93d7cfa4b3780cdae313
    [openssl-3.5.9]=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
    [aws-lc-5.12.0]=3a813ae8e62ea1cd4cb4682bf1492f3aaeb34c306ac8ba3ff3d895a002bf6ed2
)

build_lib() { # name
    local n=$1 p=$ROOT/$1 t=$ROOT/.tmp/$1
    [[ -f $p/.done ]] && return
    echo "== building $n" >&2
    [[ $n != openssl-3.5* ]] || perl -MTime::Piece -e1 2>/dev/null ||
        { echo "OpenSSL 3.5 Configure needs perl Time::Piece (Fedora: dnf install perl-Time-Piece)" >&2; exit 1; }
    rm -rf "$t" "$p"; mkdir -p "$t"
    curl -fsSL -o "$t/src.tar.gz" "${URL[$n]}"
    if [[ $n == openssl-* ]]; then
        echo "${SHA[$n]}  $t/src.tar.gz" | sha256sum -c - >&2
    else
        [[ $(gzip -dc "$t/src.tar.gz" | sha256sum) == "${SHA[$n]}  -" ]] || { echo "$n: sha256 mismatch" >&2; exit 1; }
    fi
    tar xzf "$t/src.tar.gz" -C "$t" --strip-components=1
    if [[ $n == openssl-* ]]; then
        (cd "$t" && ./Configure --prefix="$p" --libdir=lib shared -g >&2 && make -j"$(nproc)" build_sw >&2 && make install_sw >&2)
    else
        cmake -S "$t" -B "$t/b" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF \
            -DDISABLE_WERROR=ON -DCMAKE_C_FLAGS=-g -DCMAKE_CXX_FLAGS=-g \
            -DCMAKE_INSTALL_PREFIX="$p" -DCMAKE_INSTALL_LIBDIR=lib >&2
        cmake --build "$t/b" -j"$(nproc)" >&2
        cmake --install "$t/b" >&2
        # the speed tool is only built, not installed, by some versions
        [[ -x $p/bin/bssl ]] || { mkdir -p "$p/bin"; cp "$t/b/tool/bssl" "$p/bin/" 2>/dev/null || true; }
    fi
    rm -rf "$t"
    touch "$p/.done"
}

mkdir -p "$OUT" "$ROOT/bin"
BIN=$ROOT/bin
CFLAGS="-O2 -g -Wall -Wno-deprecated-declarations"
for n in "${LIBS[@]}"; do
    if [[ $n == system ]]; then
        cc $CFLAGS $(pkg-config --cflags openssl) bench/tls_lib.c -o "$BIN/tls_lib-system" $(pkg-config --libs openssl)
    else
        build_lib "$n"
        cc $CFLAGS -I"$ROOT/$n/include" bench/tls_lib.c -o "$BIN/tls_lib-$n" \
            -L"$ROOT/$n/lib" -lssl -lcrypto -Wl,-rpath,"$ROOT/$n/lib"
    fi
done
(cd bench/rust && cargo build --release --locked -p tls_lib_rs >&2)

CSV=$OUT/tls_lib.csv
echo "lib,op,tickets,variant,ns_per_op,instr_per_op,cycles_per_op" >"$CSV"
for t in 0 stateless stateful; do
    for n in "${LIBS[@]}"; do
        "$BIN/tls_lib-$n" "$n" $t "$ITERS" >>"$CSV"
    done
    bench/rust/target/release/tls_lib_rs $t "$ITERS" >>"$CSV"
done

python3 -I - "$CSV" >"$OUT/tls_lib.md" <<'EOF'
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1])))
def num(s): return None if s == "n/a" else float(s)
rs = {(r["op"], r["tickets"], r["variant"]): r for r in rows if r["lib"] == "rustls-ring"}
def ratio(r, k, ref):
    a, b = num(r[k]), num(ref[k]) if ref else None
    return "n/a" if a is None or not b else f"{a / b:.2f}"
print("Server-side TLS work only (no sockets), per op. ratio = this lib / rustls-ring, same row.\n")
print("| op | tickets | variant | lib | ns | instr:u | cycles:u | ns ratio | instr ratio | cycles ratio |")
print("|---|---|---|---|---:|---:|---:|---:|---:|---:|")
for r in sorted(rows, key=lambda r: (r["op"], r["tickets"], r["variant"], r["lib"] != "rustls-ring")):
    ref = rs.get((r["op"], r["tickets"], r["variant"]))
    print(f'| {r["op"]} | {r["tickets"]} | {r["variant"]} | {r["lib"]} | {r["ns_per_op"]} | {r["instr_per_op"]} | '
          f'{r["cycles_per_op"]} | {ratio(r, "ns_per_op", ref)} | {ratio(r, "instr_per_op", ref)} | {ratio(r, "cycles_per_op", ref)} |')
EOF

SPEED=$OUT/speed.txt
: >"$SPEED"
for n in "${LIBS[@]}"; do
    p=$ROOT/$n
    {
        echo "=== $n"
        if [[ $n == system ]]; then
            openssl speed -elapsed -seconds 3 ecdhx25519 ecdsap256
            openssl speed -elapsed -seconds 3 -evp aes-256-gcm
        elif [[ $n == openssl-* ]]; then
            LD_LIBRARY_PATH=$p/lib "$p/bin/openssl" speed -elapsed -seconds 3 ecdhx25519 ecdsap256
            LD_LIBRARY_PATH=$p/lib "$p/bin/openssl" speed -elapsed -seconds 3 -evp aes-256-gcm
        elif [[ -x $p/bin/bssl ]]; then
            LD_LIBRARY_PATH=$p/lib "$p/bin/bssl" speed -timeout 3 -filter "ECDH X25519,ECDSA P-256,EVP-AES-256-GCM"
        else
            echo "bssl not built for $n: speed skipped"
        fi
    } >>"$SPEED" 2>&1
done

cat "$OUT/tls_lib.md"
[[ -z ${GITHUB_STEP_SUMMARY:-} ]] || cat "$OUT/tls_lib.md" >>"$GITHUB_STEP_SUMMARY"
echo "wrote $CSV $OUT/tls_lib.md $SPEED" >&2
