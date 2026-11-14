#!/usr/bin/env bash
# Did em_http get slower between two commits? bench/em_http built at git ref BASE and at the checked-out
# HEAD, run interleaved in the same job (same machine) on the plain HTTP cells of run.sh, both backends.
# Writes results.csv and table.md to OUT (median and min-max of per-round new/base ratios, with a verdict);
# the table goes to $GITHUB_STEP_SUMMARY when set. Needs the history of BASE (worktree).
# env: BASE (git ref, required), DURATION (10), ROUNDS (9), OUT (bench/out/http-ab), TLS_PREFIX (see run.sh)
set -euo pipefail
cd "$(dirname "$0")/.."
: "${BASE:?BASE: git ref to compare HEAD against}"
DURATION=${DURATION:-10}
ROUNDS=${ROUNDS:-9}
OUT=${OUT:-bench/out/http-ab}
source bench/run.sh
git rev-parse --verify -q "$BASE^{commit}" >/dev/null || { echo "BASE=$BASE: not a commit here (fetch-depth 0?)" >&2; exit 2; }

# one dir per version: run.sh's start() and run_cell() take $BIN/em-http-{uring,epoll} and $BIN/client
AB=build/$ARCH/bench-http-ab
rm -rf "$AB"; mkdir -p "$AB/base" "$AB/new"
bin_dir() { make -s -C "$1" HTTP_SSE42=1 --eval='em-http-dir:; @echo $(B_REL)' em-http-dir; }
install_bin() { # srcdir version
    cp "$1/$(bin_dir "$1")/em_http" "$AB/$2/em-http-uring"
    cp "$AB/$2/em-http-uring" "$AB/$2/em-http-epoll"
}
wt=$ROOT/build/http-ab-wt-$$
trap 'git worktree remove --force "$wt" 2>/dev/null || true' EXIT
git worktree add --detach "$wt" "$BASE" >&2
make -C "$wt" HTTP_SSE42=1 bench-em-http >&2
install_bin "$wt" base
git worktree remove --force "$wt"
make HTTP_SSE42=1 bench-em-http >&2
install_bin . new
(cd bench/rust && cargo build --release --locked -p client >&2)
cp bench/rust/target/release/client "$AB/base/client"
cp "$AB/base/client" "$AB/new/client"

mkdir -p "$OUT"
echo "round,cell,backend,version,conns,size,rps,client_cpu,server_cpu,us_per_rt,p50_us,p99_us,p999_us,resumed" >"$OUT/results.csv"
: >"$OUT/http_info.txt"
CELLS=("http 16 13" "http 256 13" "http 1024 13" "http4k 16 4096" "http4k 256 4096" "httpchurn 64 13" "httppipe 16 13" "httppipe 256 13")
port=23000
for ((r = 1; r <= ROUNDS; r++)); do
    vers=(base new); ((r % 2 == 0)) && vers=(new base)
    for cell in "${CELLS[@]}"; do
        read -r mode c s <<<"$cell"
        case $mode in http) arg=(http) ;; httppipe) arg=(http pipe=16) ;; http4k) arg=(http path=/4k) ;; httpchurn) arg=(churn http) ;; esac
        for be in uring epoll; do
            for v in "${vers[@]}"; do
                BIN=$AB/$v
                port=$((port + 1))
                start em-http-$be "$port"
                run_cell em-http-$be "$port" "$mode" "$c" "$s" "${arg[@]}"
                stop
                echo "$r,$mode,$be,$v,$c,$s,$CELL_ROW" | tee -a "$OUT/results.csv" >&2
            done
        done
    done
done
rm -f "$OUT/rps.tmp"

python3 -I - "$OUT/results.csv" <<'PY' | tee "$OUT/table.md"
import csv, statistics, sys
from collections import defaultdict
d = defaultdict(dict)  # (cell, conns, backend) -> round -> version -> row
order = []
for r in csv.DictReader(open(sys.argv[1])):
    k = (r["cell"], r["conns"], r["backend"])
    if k not in order: order.append(k)
    d[k].setdefault(r["round"], {})[r["version"]] = r
def verdict(rs):  # WORSE: median > 1.05 and every round > 1; BETTER: median < 0.95 and every round < 1
    m = statistics.median(rs)
    return "WORSE" if m > 1.05 and all(x > 1 for x in rs) else "BETTER" if m < 0.95 and all(x < 1 for x in rs) else "SAME"
def cell(rs):
    return f"{statistics.median(rs):.3f} [{min(rs):.3f}-{max(rs):.3f}] {verdict(rs)}"
print("### em_http new/base (per-round ratios: median [min-max]; > 1 = new is slower)\n")
print("| cell | conns | backend | server us/rt | p99/p50 |\n|---|---|---|---|---|")
for k in order:
    us, tail = [], []
    for rnd in d[k].values():
        if "base" not in rnd or "new" not in rnd: continue
        n, b = rnd["new"], rnd["base"]
        us.append(float(n["us_per_rt"]) / float(b["us_per_rt"]))
        tail.append((float(n["p99_us"]) / float(n["p50_us"])) / (float(b["p99_us"]) / float(b["p50_us"])))
    print(f"| {k[0]} | {k[1]} | {k[2]} | {cell(us)} | {cell(tail)} |")
PY
[[ -z ${GITHUB_STEP_SUMMARY:-} ]] || { echo "### http ab: HEAD vs $BASE ($ARCH, ${DURATION}s x $ROUNDS rounds)"; cat "$OUT/table.md"; } >>"$GITHUB_STEP_SUMMARY"
