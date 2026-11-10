#!/usr/bin/env bash
#
# run_remote.sh N - N daemons on a Linux server, daemon i pinned to CPU i with
# taskset, and all producers and workers on this machine over the network, so
# the daemons have the server's cores to themselves.
#
# env: SSH_HOST (anastassow@da)  HOST_IP (192.168.1.27)  SSH_OPTS (shared
#      ControlPath)  REMOTE_DIR (quicx-several, a built checkout on the server)
#      CPUS ("0 1 2 3": the CPU for daemon 0, 1, ...)  WORKERS (48)
#      PRODUCERS (2) CONNS (4) INFLIGHT (32) DURATION (10) WARMUP (2) PORT0 (16500)
set -uo pipefail
N=${1:?usage: run_remote.sh N}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH="$HERE/../bench"
SSH_HOST=${SSH_HOST:-anastassow@da}; HOST_IP=${HOST_IP:-192.168.1.27}
SSH_OPTS=${SSH_OPTS:-"-o ControlPath=$HOME/.ssh/cm/%C"}; REMOTE_DIR=${REMOTE_DIR:-quicx-several}
read -r -a CPU <<< "${CPUS:-0 1 2 3}"
WORKERS=${WORKERS:-48}; PRODUCERS=${PRODUCERS:-2}; CONNS=${CONNS:-4}; INFLIGHT=${INFLIGHT:-32}
DURATION=${DURATION:-10}; WARMUP=${WARMUP:-2}; PORT0=${PORT0:-16500}
OUT=${OUT:-$HERE/results/remote-n$N}
rsh() { ssh $SSH_OPTS "$SSH_HOST" "$@"; }
PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null; done; sleep 0.3
            for p in "${PIDS[@]:-}"; do kill -9 "$p" 2>/dev/null; done; wait 2>/dev/null
            rsh "pkill -f '[q]uicx-several/quicxd start --port 165' ; true" >/dev/null 2>&1; }
trap cleanup EXIT INT TERM
make -C "$BENCH" --silent || exit 1
rm -rf "$OUT"; mkdir -p "$OUT"

rsh "pkill -f '[q]uicx-several/quicxd start --port 165'; sleep 0.3; for i in \$(seq 0 $((N - 1))); do
       cpu=\$(echo '${CPU[*]}' | cut -d' ' -f\$((i + 1)))
       nohup taskset -c \$cpu \$HOME/$REMOTE_DIR/quicxd start --port \$(( $PORT0 + i )) > /tmp/qx-\$i.log 2>&1 &
     done; sleep 0.5" >/dev/null 2>&1
for i in $(seq 0 $((N - 1))); do
    for _ in $(seq 1 50); do nc -z -G 1 "$HOST_IP" $((PORT0 + i)) 2>/dev/null && break; sleep 0.1; done
done

for i in $(seq 0 $((N - 1))); do
    port=$((PORT0 + i)); d="$OUT/d$i"; mkdir -p "$d"
    for w in $(seq 1 "$WORKERS"); do
        "$BENCH/bench_worker" --host "$HOST_IP" --port "$port" --id "$w" --spin-us 0 2>> "$d/workers.log" & PIDS+=($!)
    done
done
sleep 1

# server CPU per daemon while the load runs
( sleep $((WARMUP + 3)); rsh "top -b -n 2 -d 1 | grep quicxd | tail -n $N" > "$OUT/server_top.txt" 2>&1 ) &
TOP=$!

PROD=()
for i in $(seq 0 $((N - 1))); do
    port=$((PORT0 + i)); d="$OUT/d$i"
    for p in $(seq 1 "$PRODUCERS"); do
        "$BENCH/bench_producer" --host "$HOST_IP" --port "$port" --conns "$CONNS" --inflight "$INFLIGHT" \
            --duration "$DURATION" --warmup "$WARMUP" --seed "$p" --out "$d/hist.$p" 2>> "$d/producers.log" &
        PROD+=($!); PIDS+=($!)
    done
done
for p in "${PROD[@]}"; do wait "$p"; done
wait "$TOP" 2>/dev/null

total=0; per=()
for i in $(seq 0 $((N - 1))); do
    t=$(awk '{ for (k=1;k<=NF;k++) if ($k ~ /^thr=/) { v=$k; sub(/^thr=/,"",v); sub(/\/s$/,"",v);
               if (v ~ /k$/) { sub(/k$/,"",v); v*=1000 } s+=v } } END { printf "%d", s }' "$OUT/d$i/producers.log")
    per+=("$t"); total=$((total + t))
done
cpu=$(awk '{ printf "%s%s", sep, $9; sep=" " }' "$OUT/server_top.txt" 2>/dev/null)
echo "daemons=$N total=$total per=[${per[*]}] server_cpu%=[$cpu]" | tee "$OUT/summary.txt"
