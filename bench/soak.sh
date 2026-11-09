#!/usr/bin/env bash
# Long-run memory check (plans/m5.md, step 4): each em server under connection churn for SOAK s.
# Every 10 s: VmRSS and the line count of /proc/<pid>/maps (each fiber stack is one mapping).
# Pass when the last sample is within 5% (RSS) and 2% (maps) of the sample at the half-way point:
# a leak grows with connections, so it keeps growing through the second half (millions of them).
# Not the sample at 60 s: the fiber stack pool may reach a new high-water mark once, at any time,
# when a burst of fibers overlaps (seen at 30 s and 110 s, flat afterwards); that is not a leak.
# Only em is measured: the question is whether em leaks, not how it compares.
# env: SOAK (s, 600), OUT (bench/out/soak), SOAK_SERVERS
set -euo pipefail
cd "$(dirname "$0")/.."
SOAK=${SOAK:-600}
OUT=${OUT:-bench/out/soak}
source bench/run.sh
SOAK_SERVERS=(${SOAK_SERVERS:-em-uring em-epoll})
((SOAK >= 120)) || { echo "SOAK must be >= 120" >&2; exit 2; }
HALF=$((SOAK / 20 * 10)) # sample times are multiples of 10

build
mkdir -p "$OUT"
echo "server,t,rss_kb,maps" >"$OUT/soak.csv"
: >"$OUT/soak.md"
fail=0
port=22000
for srv in "${SOAK_SERVERS[@]}"; do
    port=$((port + 1))
    start "$srv" "$port"
    taskset -c $CLI_CPUS "$BIN/client" "127.0.0.1:$port" 64 128 "$SOAK" "$CLI_THREADS" churn >"$OUT/$srv.client" &
    cp=$!
    for ((t = 10; t <= SOAK; t += 10)); do
        sleep 10
        rss=$(awk '/^VmRSS/ { print $2 }' "/proc/$pid/status")
        echo "$srv,$t,$rss,$(wc -l <"/proc/$pid/maps")" >>"$OUT/soak.csv"
    done
    wait $cp || { echo "client failed for $srv" >&2; fail=1; }
    stop
    read -r conns cps _ <"$OUT/$srv.client"
    # base: the sample at HALF; last: the final sample
    awk -F, -v s="$srv" -v n="$conns" -v cps="$cps" -v h="$HALF" '
        $1 == s && $2 == h { r0 = $3; m0 = $4 }
        $1 == s { r1 = $3; m1 = $4 }
        END {
            ok = r1 <= r0 * 1.05 && m1 <= m0 * 1.02
            printf "| %s | %d | %d | %d -> %d (%+.1f%%) | %d -> %d (%+.1f%%) | %s |\n", s, n, cps,
                r0, r1, (r1 - r0) * 100 / r0, m0, m1, (m1 - m0) * 100 / m0, ok ? "pass" : "FAIL"
            exit !ok
        }' "$OUT/soak.csv" >>"$OUT/soak.md" || fail=1
done
{
    echo "### soak ($ARCH, ${SOAK}s churn, 64 concurrent connections)"
    echo
    echo "| server | connections | conn/s | RSS KiB, ${HALF} s -> end | maps, ${HALF} s -> end | |"
    echo "|---|---|---|---|---|---|"
    cat "$OUT/soak.md"
} | tee "$OUT/soak.md.tmp"
mv "$OUT/soak.md.tmp" "$OUT/soak.md"
[[ -z ${GITHUB_STEP_SUMMARY:-} ]] || cat "$OUT/soak.md" >>"$GITHUB_STEP_SUMMARY"
exit $fail
