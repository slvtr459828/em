#!/usr/bin/env bash
# Where does a server spend its round-trips (plans/m5.md, step 1)? For each server and cell:
#   syscalls.csv  syscalls per round-trip, by syscall (perf stat on syscalls:sys_enter_*)
#   <srv>-<conns>x<size>.txt  flat CPU profile of the server CPU (perf record -e cpu-clock),
#   <srv>-<conns>x<size>.dso.txt the same by dso only (kernel / libcrypto / libssl / the binary)
#   A TLS cell ("tls:256:128") writes <srv>-tls-<conns>x<size>.* (the plain cells use other conns).
#   A TLS churn cell ("churntls:64:128", "churntlsres:64:128") writes <srv>-churntls-64x128.*; its
#   "round-trip" is one connection (client: churn tls / churn tlsres).
#   PROF_CALLGRAPH=1: <name>.callers.txt, who calls each symbol >= 0.5% (perf report -g callee; frame pointers)
#   PROF_COUNTERS=1:  counters.csv  server,cell,event,per_rt from perf stat (needs a PMU; skipped if none)
# Pass 1 needs tracepoint access (root, or perf_event_paranoid=-1 and readable tracefs), else it is
# skipped with a note. Pass 2 records the server CPU system-wide (-C); where that is not permitted
# (perf_event_paranoid > 0) it records the server process (-p) and misses the kernel softirq work
# that loopback TCP runs on that CPU.
# env: DURATION (5), OUT (bench/out/profile), PROF_SERVERS, PROF_CELLS ("conns:size ..."),
#      (a cell is "conns:size", or "tls:conns:size" for a TLS echo cell: use with the TLS servers),
#      PERF (perf command, e.g. "sudo perf" where tracefs is root-only), PROF_CALLGRAPH, PROF_COUNTERS
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
echo "server,conns,size,rps_under_perf" >"$OUT/rps.csv"
echo "server,cell,event,per_rt" >"$OUT/counters.csv"

# what this perf/kernel/VM lets us do
can_trace=1; can_cpu=1; counters=
"${PERF[@]}" stat -e syscalls:sys_enter_read -o /dev/null -- true >/dev/null 2>&1 ||
    { can_trace=0; echo "note: no syscall tracepoint access: pass 1 (syscalls per round-trip) skipped" >&2; }
"${PERF[@]}" record -q -e cpu-clock -C "$SRV_CPUS" -o "$OUT/probe.data" -- sleep 0.1 >/dev/null 2>&1 ||
    { can_cpu=0; echo "note: no system-wide perf (paranoid): recording the server process, kernel softirq on CPU $SRV_CPUS is missed" >&2; }
if [[ ${PROF_COUNTERS:-} == 1 ]]; then
    for set in instructions:u,cycles:u,branch-misses:u,instructions:k,cycles:k instructions:u,cycles:u,branch-misses:u; do
        if "${PERF[@]}" stat -x, -e "$set" -o "$OUT/probe.tmp" -- true >/dev/null 2>&1 && ! grep -q "not supported\|not counted" "$OUT/probe.tmp"; then
            counters=$set; break
        fi
    done
    [[ -n $counters ]] || echo "note: no PMU (VM?): counters pass (PROF_COUNTERS) skipped" >&2
    [[ -z $counters || $counters == *:k* ]] || echo "note: no kernel-mode counters here: user-mode only" >&2
fi
rm -f "$OUT/probe.data" "$OUT/probe.tmp"
cg=() # call graphs: user-only chains when unprivileged (perf refuses kernel ones)
if [[ ${PROF_CALLGRAPH:-} == 1 ]]; then cg=(--call-graph fp); ((can_cpu)) || cg+=(--user-callchains); fi

port=21000
for cell in "${PROF_CELLS[@]}"; do
    arg=(); tag=
    case $cell in
        tls:*) arg=(tls); tag=tls-; cell=${cell#tls:} ;;
        churntls:*) arg=(churn tls); tag=churntls-; cell=${cell#churntls:} ;;
        churntlsres:*) arg=(churn tlsres); tag=churntlsres-; cell=${cell#churntlsres:} ;;
    esac
    c=${cell%:*} s=${cell#*:}
    for srv in "${PROF_SERVERS[@]}"; do
        name=$srv-$tag${c}x$s
        sname=$srv; [[ $tag == churn* ]] && sname=$srv-${tag%-} # syscalls.md keys on server x conns x size: keep churntls and churntlsres apart
        # pass 1: syscall counts (tracepoints cost a little; counts, not time, are what we read)
        if ((can_trace)); then
            port=$((port + 1)); start "$srv" "$port"
            "${PERF[@]}" stat -x, -e 'syscalls:sys_enter_*' -p "$pid" -o "$OUT/stat.tmp" &
            sp=$!; sleep 0.5
            taskset -c $CLI_CPUS "$BIN/client" "127.0.0.1:$port" "$c" "$s" "$DURATION" "$CLI_THREADS" "${arg[@]}" >"$OUT/rps.tmp"
            kill -INT $sp; wait $sp || true
            stop
            read -r total _ <"$OUT/rps.tmp"
            awk -F, -v srv="$sname" -v c="$c" -v s="$s" -v n="$total" \
                '$1 ~ /^[0-9]+$/ && $1 > 0 { sub("syscalls:sys_enter_", "", $3); printf "%s,%s,%s,%s,%.3f\n", srv, c, s, $3, $1 / n }' \
                "$OUT/stat.tmp" | sort -t, -k5 -gr | tee -a "$OUT/syscalls.csv" >&2
        fi
        # pass 2: where the server CPU goes (kernel TCP stack included: softirq runs on that CPU)
        port=$((port + 1)); start "$srv" "$port"
        # record the server CPU while the client runs (the client itself is not run under perf/sudo)
        if ((can_cpu)); then who=(-C "$SRV_CPUS"); else who=(-p "$pid"); fi
        "${PERF[@]}" record -q -e cpu-clock "${cg[@]}" "${who[@]}" -o "$OUT/perf.data" -- sleep "$DURATION" &
        sp=$!
        taskset -c $CLI_CPUS "$BIN/client" "127.0.0.1:$port" "$c" "$s" "$DURATION" "$CLI_THREADS" "${arg[@]}" >"$OUT/rps.tmp"
        wait $sp
        # server CPU time per round-trip (idle included) = 1e6 / rps: a profile share x this = us per round-trip
        read -r _ rps _ <"$OUT/rps.tmp"; echo "$srv,$c,$s,$rps" >>"$OUT/rps.csv"
        stop
        "${PERF[@]}" report -q -i "$OUT/perf.data" --no-children --sort comm,dso,sym --percent-limit 0.1 \
            >"$OUT/$name.txt" 2>/dev/null
        "${PERF[@]}" report -q -i "$OUT/perf.data" --no-children --sort dso >"$OUT/$name.dso.txt" 2>/dev/null
        if ((${#cg[@]})); then
            "${PERF[@]}" report -q -i "$OUT/perf.data" --no-children -g callee --sort sym --percent-limit 0.5 \
                >"$OUT/$name.callers.txt" 2>/dev/null
        fi
        head -n 25 "$OUT/$name.txt" >&2
        cat "$OUT/$name.dso.txt" >&2
        # pass 3: instructions, cycles, branch misses per round-trip (a separate run: no -g, no sampling)
        if [[ -n $counters ]]; then
            port=$((port + 1)); start "$srv" "$port"
            "${PERF[@]}" stat -x, -e "$counters" -p "$pid" -o "$OUT/cnt.tmp" &
            sp=$!; sleep 0.5
            taskset -c $CLI_CPUS "$BIN/client" "127.0.0.1:$port" "$c" "$s" "$DURATION" "$CLI_THREADS" "${arg[@]}" >"$OUT/rps.tmp"
            kill -INT $sp; wait $sp || true
            stop
            read -r total _ <"$OUT/rps.tmp"
            awk -F, -v srv="$srv" -v cell="$tag${c}x$s" -v n="$total" \
                '$1 ~ /^[0-9]+$/ { printf "%s,%s,%s,%.1f\n", srv, cell, $3, $1 / n }' "$OUT/cnt.tmp" | tee -a "$OUT/counters.csv" >&2
        fi
    done
done
rm -f "$OUT/stat.tmp" "$OUT/cnt.tmp" "$OUT/rps.tmp" "$OUT/perf.data" "$OUT/perf.data.old" # our directory: removable whoever owns them
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
