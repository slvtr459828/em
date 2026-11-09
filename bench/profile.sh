#!/usr/bin/env bash
# Where does a server spend its round-trips (plans/m5.md, step 1)? For each server and cell:
#   syscalls.csv  syscalls per round-trip, by syscall (perf stat on syscalls:sys_enter_*)
#   <srv>-<conns>x<size>.txt  flat CPU profile of the server CPU (perf record -e cpu-clock)
# Needs perf with tracepoint access (root, or perf_event_paranoid=-1 and readable tracefs).
# env: DURATION (5), OUT (bench/out/profile), PROF_SERVERS, PROF_CELLS ("conns:size ..."),
#      PERF (perf command, e.g. "sudo perf" where tracefs is root-only)
set -euo pipefail
cd "$(dirname "$0")/.."
DURATION=${DURATION:-5}
OUT=${OUT:-bench/out/profile}
source bench/run.sh
PERF=(${PERF:-perf})
PROF_SERVERS=(${PROF_SERVERS:-em-epoll tokio-epoll em-uring})
PROF_CELLS=(${PROF_CELLS:-16:4096 1024:4096})

build
mkdir -p "$OUT"
echo "server,conns,size,syscall,per_rt" >"$OUT/syscalls.csv"
port=21000
for cell in "${PROF_CELLS[@]}"; do
    c=${cell%:*} s=${cell#*:}
    for srv in "${PROF_SERVERS[@]}"; do
        # pass 1: syscall counts (tracepoints cost a little; counts, not time, are what we read)
        port=$((port + 1)); start "$srv" "$port"
        "${PERF[@]}" stat -x, -e 'syscalls:sys_enter_*' -p "$pid" -o "$OUT/stat.tmp" &
        sp=$!; sleep 0.5
        taskset -c $CLI_CPUS "$BIN/client" "127.0.0.1:$port" "$c" "$s" "$DURATION" "$CLI_THREADS" >"$OUT/rps.tmp"
        kill -INT $sp; wait $sp || true
        stop
        read -r total _ <"$OUT/rps.tmp"
        awk -F, -v srv="$srv" -v c="$c" -v s="$s" -v n="$total" \
            '$1 ~ /^[0-9]+$/ && $1 > 0 { sub("syscalls:sys_enter_", "", $3); printf "%s,%s,%s,%s,%.3f\n", srv, c, s, $3, $1 / n }' \
            "$OUT/stat.tmp" | sort -t, -k5 -gr | tee -a "$OUT/syscalls.csv" >&2
        # pass 2: where the server CPU goes (kernel TCP stack included: softirq runs on that CPU)
        port=$((port + 1)); start "$srv" "$port"
        # record the server CPU while the client runs (the client itself is not run under perf/sudo)
        "${PERF[@]}" record -q -e cpu-clock -C "$SRV_CPUS" -o "$OUT/perf.data" -- sleep "$DURATION" &
        sp=$!
        taskset -c $CLI_CPUS "$BIN/client" "127.0.0.1:$port" "$c" "$s" "$DURATION" "$CLI_THREADS" >/dev/null
        wait $sp
        stop
        "${PERF[@]}" report -q -i "$OUT/perf.data" --no-children --sort comm,dso,sym --percent-limit 0.5 \
            >"$OUT/$srv-${c}x$s.txt" 2>/dev/null
        head -n 25 "$OUT/$srv-${c}x$s.txt" >&2
    done
done
rm -f "$OUT/stat.tmp" "$OUT/rps.tmp" "$OUT/perf.data" "$OUT/perf.data.old" # our directory: removable whoever owns them
python3 -I - "$OUT/syscalls.csv" <<'PY' | tee "$OUT/syscalls.md"
import csv, sys
from collections import defaultdict
t = defaultdict(float); rows = defaultdict(dict)
for r in csv.DictReader(open(sys.argv[1])):
    k = (r["server"], r["conns"], r["size"]); t[k] += float(r["per_rt"]); rows[k][r["syscall"]] = float(r["per_rt"])
print("### syscalls per round-trip\n\n| server | conns x size | total | top |\n|---|---|---|---|")
for k in sorted(t, key=lambda k: (int(k[1]), int(k[2]), k[0])):
    top = ", ".join(f"{n} {v:.2f}" for n, v in sorted(rows[k].items(), key=lambda x: -x[1])[:5])
    print(f"| {k[0]} | {k[1]}x{k[2]} | {t[k]:.2f} | {top} |")
PY
[[ -z ${GITHUB_STEP_SUMMARY:-} ]] || { echo "### profile ($ARCH)"; cat "$OUT/syscalls.md"; } >>"$GITHUB_STEP_SUMMARY"
