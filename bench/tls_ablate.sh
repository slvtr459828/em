#!/usr/bin/env bash
# How much of em_tls's CPU per round-trip is our glue (plans/tls-perf.md, step 4)? Each patch in
# bench/ablate/ is applied to a COPY of the sources, em_tls_echo is rebuilt from it, and all
# variants are measured interleaved against the unchanged one ("base"): echotls 256x128 and
# 256x4096 on both backends. Per row: round-trips/s, server CPU us/rt (as run.sh), and with a PMU
# `perf stat -e instructions:u,cycles:u -p <server>` per round-trip. src/ itself is never touched.
# Output: $OUT/results.csv, $OUT/ablate.md (also appended to $GITHUB_STEP_SUMMARY).
# env: DURATION (5), ROUNDS (5), OUT (bench/out/tls-ablate), PERF (perf command), VARIANTS (patch names, plus base)
set -euo pipefail
cd "$(dirname "$0")/.."
DURATION=${DURATION:-5}
ROUNDS=${ROUNDS:-5}
OUT=${OUT:-bench/out/tls-ablate}
PERF=(${PERF:-perf})
source bench/run.sh

VARIANTS=(${VARIANTS:-base G1-errclear G14 G124 ALL})
CELLS=("256 128" "256 4096")
BACKENDS=(uring epoll)

build
mkdir -p "$OUT"
REL=build/$ARCH/rel
root=$PWD
ABL=build/$ARCH/ablate
for v in "${VARIANTS[@]}"; do
    d=$ABL/$v
    rm -rf "$d"; mkdir -p "$d/tests" "$d/bench"
    cp -r src include "$d/"
    cp tests/tls_util.h tests/util.h "$d/tests/"
    cp bench/em_tls_echo.c "$d/bench/"
    [[ $v == base ]] || (cd "$d" && git apply "$root/bench/ablate/$v.patch")
    # same flags as the Makefile's rel build (CF_REL, TLS_CF, TLS_LIBS); libem.a is the normal build's
    ${CC:-gcc} -std=gnu11 -O2 -g -D_FORTIFY_SOURCE=3 -Wall -Wextra -Werror -I"$d/include" $(pkg-config --cflags openssl) \
        -I"$d/src" "$d/src/tls.c" "$d/bench/em_tls_echo.c" "$REL/libem.a" $(pkg-config --libs openssl) \
        -luring -lm -lpthread -o "$d/em_tls_echo"
    # run.sh's start/run_cell find the server and the client under $BIN/<name>
    ln -s em_tls_echo "$d/em-tls-uring"; ln -s em_tls_echo "$d/em-tls-epoll"
    ln -s "$PWD/$BIN/client" "$d/client"
done

# PMU: counted for our own processes (perf_event_paranoid <= 2); a CI VM gives "<not supported>"
pmu=0
if "${PERF[@]}" stat -x, -e instructions:u,cycles:u -o "$OUT/stat.tmp" -- true 2>/dev/null &&
   awk -F, '$1 ~ /^[0-9]+$/ { n++ } END { exit n != 2 }' "$OUT/stat.tmp"; then pmu=1
else echo "note: no PMU counters here; instructions/rt and cycles/rt are left empty" >&2; fi

echo "round,cell,backend,variant,rps,us_per_rt,instr_per_rt,cycles_per_rt" >"$OUT/results.csv"
port=22000
for ((r = 1; r <= ROUNDS; r++)); do
    for cell in "${CELLS[@]}"; do
        read -r c s <<<"$cell"
        for be in "${BACKENDS[@]}"; do
            for v in "${VARIANTS[@]}"; do
                BIN=$ABL/$v
                port=$((port + 1)); start "em-tls-$be" "$port"
                sp=
                if ((pmu)); then
                    "${PERF[@]}" stat -x, -e instructions:u,cycles:u -p "$pid" -o "$OUT/stat.tmp" &
                    sp=$!; sleep 0.3
                fi
                run_cell "$v" "$port" echotls "$c" "$s" tls
                [[ -z $sp ]] || { kill -INT $sp; wait $sp || true; }
                stop
                ic=,
                ((pmu)) && ic=$(awk -F, -v n="$total" '$3 ~ /^instructions/ { i = $1 } $3 ~ /^cycles/ { y = $1 }
                    END { printf "%.0f,%.0f", i / n, y / n }' "$OUT/stat.tmp")
                IFS=, read -r rps _ _ us _ <<<"$CELL_ROW"
                echo "$r,${c}x$s,$be,$v,$rps,$us,$ic" | tee -a "$OUT/results.csv" >&2
            done
        done
    done
done
rm -f "$OUT/stat.tmp" "$OUT/rps.tmp"

python3 -I - "$OUT/results.csv" "${VARIANTS[@]}" <<'PY' | tee "$OUT/ablate.md"
import csv, statistics as st, sys
from collections import defaultdict
variants = sys.argv[2:]
d = defaultdict(dict)  # (cell, backend, variant) -> round -> row
for r in csv.DictReader(open(sys.argv[1])):
    d[(r["cell"], r["backend"], r["variant"])][int(r["round"])] = r
def val(row, k):
    return float(row[k]) if row[k] else None
def ratios(cb, v, k):  # per-round ratio vs base, same round
    out = []
    for rd, row in d[cb + (v,)].items():
        a, b = val(row, k), val(d[cb + ("base",)][rd], k)
        if a and b: out.append(a / b)
    return out
def cellfmt(rs):
    return "-" if not rs else f"{st.median(rs):.3f} [{min(rs):.3f}-{max(rs):.3f}]"
K = ("rps", "us_per_rt", "instr_per_rt", "cycles_per_rt")
print("### em_tls glue ablation: base absolute, others ratio vs base (median [min-max] over rounds; < 1 is better, rps > 1)\n")
print("| cell | backend | variant | rps | server us/rt | instructions:u/rt | cycles:u/rt |\n|---|---|---|---|---|---|---|")
for cell, be in sorted({k[:2] for k in d}, key=lambda k: (int(k[0].split("x")[1]), k[1])):
    for v in variants:
        if v == "base":
            cols = []
            for k in K:
                xs = [val(x, k) for x in d[(cell, be, v)].values() if val(x, k)]
                cols.append("-" if not xs else f"{st.median(xs):.2f}" if k == "us_per_rt" else f"{st.median(xs):.0f}")
        else:
            cols = [cellfmt(ratios((cell, be), v, k)) for k in K]
        print(f"| {cell} | {be} | {v} | " + " | ".join(cols) + " |")
PY
[[ -z ${GITHUB_STEP_SUMMARY:-} ]] || { echo "### tls glue ablation ($ARCH, ${DURATION}s x $ROUNDS rounds)"; cat "$OUT/ablate.md"; } >>"$GITHUB_STEP_SUMMARY"
