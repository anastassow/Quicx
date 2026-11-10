#!/usr/bin/env bash
#
# run_several.sh N - N Quicx daemons on one machine at the same time, each with
# its own workers and producers (the closed-loop load of bench/run.sh), and the
# throughput of each. Shows how independent daemons share a multi-core machine.
#
# env: WORKERS (8) PRODUCERS (2) CONNS (4) INFLIGHT (32) DURATION (10)
#      WARMUP (2) PORT0 (16500) DAEMON (../../../../quicxd) OUT (results dir)
#      PIN=1 on Linux pins daemon i to CPU i with taskset (DAEMON_CPUS="0 1"
#      picks the CPUs); CLIENT_CPUS="2,3" pins every producer and worker there
set -uo pipefail
N=${1:?usage: run_several.sh N}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH="$HERE/../bench"
DAEMON=${DAEMON:-$HERE/../../../../quicxd}
WORKERS=${WORKERS:-8}; PRODUCERS=${PRODUCERS:-2}; CONNS=${CONNS:-4}; INFLIGHT=${INFLIGHT:-32}
DURATION=${DURATION:-10}; WARMUP=${WARMUP:-2}; PORT0=${PORT0:-16500}
OUT=${OUT:-$HERE/results/n$N}
PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null; done; sleep 0.3
            for p in "${PIDS[@]:-}"; do kill -9 "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT INT TERM
make -C "$BENCH" --silent || exit 1
CL=(); [ -n "${CLIENT_CPUS:-}" ] && CL=(taskset -c "$CLIENT_CPUS")
rm -rf "$OUT"; mkdir -p "$OUT"

for i in $(seq 0 $((N - 1))); do
    port=$((PORT0 + i)); d="$OUT/d$i"; mkdir -p "$d"
    if [ "${PIN:-0}" = "1" ]; then
        read -r -a DC <<< "${DAEMON_CPUS:-0 1 2 3 4 5 6 7}"
        taskset -c "${DC[$i]}" "$DAEMON" start --port "$port" > "$d/daemon.log" 2>&1 &
    else "$DAEMON" start --port "$port" > "$d/daemon.log" 2>&1 & fi
    PIDS+=($!)
    for _ in $(seq 1 100); do (exec 3<>/dev/tcp/127.0.0.1/"$port") 2>/dev/null && break; sleep 0.1; done
    for w in $(seq 1 "$WORKERS"); do
        ${CL[@]+"${CL[@]}"} "$BENCH/bench_worker" --port "$port" --id "$w" --spin-us 0 2>> "$d/workers.log" & PIDS+=($!)
    done
done
sleep 0.8

PROD=()
for i in $(seq 0 $((N - 1))); do
    port=$((PORT0 + i)); d="$OUT/d$i"
    for p in $(seq 1 "$PRODUCERS"); do
        ${CL[@]+"${CL[@]}"} "$BENCH/bench_producer" --port "$port" --conns "$CONNS" --inflight "$INFLIGHT" \
            --duration "$DURATION" --warmup "$WARMUP" --seed "$p" --out "$d/hist.$p" 2>> "$d/producers.log" &
        PROD+=($!); PIDS+=($!)
    done
done
for p in "${PROD[@]}"; do wait "$p"; done

total=0; per=()
for i in $(seq 0 $((N - 1))); do
    t=$(awk '{ for (k=1;k<=NF;k++) if ($k ~ /^thr=/) { v=$k; sub(/^thr=/,"",v); sub(/\/s$/,"",v);
               if (v ~ /k$/) { sub(/k$/,"",v); v*=1000 } s+=v } } END { printf "%d", s }' "$OUT/d$i/producers.log")
    per+=("$t"); total=$((total + t))
done
echo "daemons=$N total=$total per=[${per[*]}]" | tee "$OUT/summary.txt"
