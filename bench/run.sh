#!/usr/bin/env bash
#
# run.sh — one full quicx benchmark run.
#
# starts the daemon, attaches workers, drives producers, merges the
# latency histograms and prints the result. everything it starts, it
# stops, including on ctrl-c.
#
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

WORKERS=${WORKERS:-8}
PRODUCERS=${PRODUCERS:-4}
CONNS=${CONNS:-4}
RATE=${RATE:-0}
INFLIGHT=${INFLIGHT:-32}
DURATION=${DURATION:-10}
WARMUP=${WARMUP:-2}
SPIN_US=${SPIN_US:-0}
PORT=${PORT:-16381}
CONFIG=${CONFIG:-}
DAEMON=${DAEMON:-$ROOT/quicxd}

RESULTS="$HERE/results"
PIDS=()

usage() {
    cat <<EOF
usage: $0 [options]

  -w N     workers            (default $WORKERS)
  -p N     producer processes  (default $PRODUCERS)
  -c N     connections each    (default $CONNS)
  -r R     total tasks/sec, open loop. 0 = closed loop (default $RATE)
  -i N     closed-loop inflight per conn (default $INFLIGHT)
  -d S     duration seconds    (default $DURATION)
  -u S     warmup seconds      (default $WARMUP)
  -s N     worker spin us/task (default $SPIN_US)
  -P N     daemon port         (default $PORT)
  -f FILE  daemon config file

examples:
  $0                       peak throughput, closed loop
  $0 -r 50000 -d 30        50k tasks/sec for 30s, honest latency
  $0 -s 200 -w 16          simulate 200us of work per task
EOF
    exit 1
}

while getopts "w:p:c:r:i:d:u:s:P:f:h" opt; do
    case "$opt" in
        w) WORKERS=$OPTARG ;;
        p) PRODUCERS=$OPTARG ;;
        c) CONNS=$OPTARG ;;
        r) RATE=$OPTARG ;;
        i) INFLIGHT=$OPTARG ;;
        d) DURATION=$OPTARG ;;
        u) WARMUP=$OPTARG ;;
        s) SPIN_US=$OPTARG ;;
        P) PORT=$OPTARG ;;
        f) CONFIG=$OPTARG ;;
        *) usage ;;
    esac
done

cleanup() {
    for pid in "${PIDS[@]:-}"; do
        [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null
    done
    sleep 0.3
    for pid in "${PIDS[@]:-}"; do
        [ -n "${pid:-}" ] && kill -9 "$pid" 2>/dev/null
    done
    "$DAEMON" stop >/dev/null 2>&1
    wait 2>/dev/null
}
trap cleanup EXIT INT TERM

# ---- build ------------------------------------------------------------

if [ ! -x "$DAEMON" ]; then
    echo "building quicxd..."
    ( cd "$ROOT" && make quicxd ) || { echo "daemon build failed"; exit 1; }
fi
make -C "$HERE" --silent || { echo "bench build failed"; exit 1; }

rm -rf "$RESULTS"
mkdir -p "$RESULTS"

# ---- daemon -----------------------------------------------------------

"$DAEMON" stop >/dev/null 2>&1
sleep 0.2

DAEMON_ARGS=(start --port "$PORT")
[ -n "$CONFIG" ] && DAEMON_ARGS+=(--config "$CONFIG")

"$DAEMON" "${DAEMON_ARGS[@]}" > "$RESULTS/daemon.log" 2>&1 &
DAEMON_PID=$!
PIDS+=("$DAEMON_PID")

# the daemon's stdout is block buffered when redirected, so poll the port
# itself rather than waiting for a banner that may not be flushed yet
READY=0
for _ in $(seq 1 100); do
    if ! kill -0 "$DAEMON_PID" 2>/dev/null; then break; fi
    if (exec 3<>/dev/tcp/127.0.0.1/"$PORT") 2>/dev/null; then READY=1; break; fi
    sleep 0.1
done

if [ "$READY" != "1" ]; then
    echo "daemon never accepted connections on port $PORT"
    cat "$RESULTS/daemon.log"
    exit 1
fi

# ---- workers ----------------------------------------------------------

WORKER_PIDS=()
for i in $(seq 1 "$WORKERS"); do
    "$HERE/bench_worker" --port "$PORT" --id "$i" --spin-us "$SPIN_US" \
        2>> "$RESULTS/workers.log" &
    WORKER_PIDS+=($!)
    PIDS+=($!)
done

sleep 0.5   # let every worker register before load starts

# ---- producers --------------------------------------------------------

if [ "$RATE" != "0" ]; then
    PER_PROC=$(awk "BEGIN{printf \"%.4f\", $RATE / $PRODUCERS}")
    MODE="open loop @ ${RATE} tasks/sec"
    RATE_ARGS=(--rate "$PER_PROC")
else
    MODE="closed loop, ${INFLIGHT} in flight per conn"
    RATE_ARGS=(--inflight "$INFLIGHT")
fi

if [ "${QUIET_RUN:-0}" != "1" ]; then
cat <<EOF

quicx benchmark
  workload    HTTP access-log parsing (nginx combined format)
  daemon      $DAEMON  port $PORT
  workers     $WORKERS  (spin ${SPIN_US}us/task)
  producers   $PRODUCERS process(es) x $CONNS conn(s) = $((PRODUCERS * CONNS)) connections
  mode        $MODE
  duration    ${DURATION}s  (first ${WARMUP}s discarded)

EOF
fi

PROD_PIDS=()
for i in $(seq 1 "$PRODUCERS"); do
    "$HERE/bench_producer" \
        --port "$PORT" --conns "$CONNS" "${RATE_ARGS[@]}" \
        --duration "$DURATION" --warmup "$WARMUP" --seed "$i" \
        --out "$RESULTS/hist.$i" 2>> "$RESULTS/producers.log" &
    PROD_PIDS+=($!)
    PIDS+=($!)
done

PROD_FAIL=0
for pid in "${PROD_PIDS[@]}"; do
    wait "$pid" || PROD_FAIL=1
done

# ---- daemon-side view before we tear anything down --------------------

"$DAEMON" status > "$RESULTS/status.txt" 2>&1

# workers only report their totals on exit, so stop them before reading
for pid in "${WORKER_PIDS[@]}"; do kill -TERM "$pid" 2>/dev/null; done
for pid in "${WORKER_PIDS[@]}"; do wait "$pid" 2>/dev/null; done

# ---- report -----------------------------------------------------------

shopt -s nullglob
HISTS=("$RESULTS"/hist.*)
shopt -u nullglob

if [ ${#HISTS[@]} -eq 0 ]; then
    echo "no histograms produced — see $RESULTS/producers.log"
    exit 1
fi

if [ "${QUIET_RUN:-0}" = "1" ]; then
    THR=$(awk '{ for (i=1;i<=NF;i++) if ($i ~ /^thr=/) { sub(/^thr=/,"",$i); sub(/\/s$/,"",$i); print $i } }' \
          "$RESULTS/producers.log" | paste -sd' ' -)
    ERRS=$(awk '{ for (i=1;i<=NF;i++) if ($i ~ /^err=/) { sub(/^err=/,"",$i); s+=$i } } END { print s+0 }' \
          "$RESULTS/producers.log")
    DROPS=$(awk '{ for (i=1;i<=NF;i++) if ($i ~ /^dropped_conns=/) { sub(/^dropped_conns=/,"",$i); s+=$i } } END { print s+0 }' \
          "$RESULTS/producers.log")
    echo "$("$HERE/bench_report" --summary "${HISTS[@]}") thr=[$THR] err=$ERRS drops=$DROPS"
    exit 0
fi

"$HERE/bench_report" "${HISTS[@]}"

echo "per-producer"
sed 's/^/  /' "$RESULTS/producers.log"

echo
echo "workers"
sort -t= -k2 -n "$RESULTS/workers.log" 2>/dev/null | sed 's/^/  /' | head -20

echo
echo "daemon view"
sed 's/^/  /' "$RESULTS/status.txt" | head -30

if [ "$PROD_FAIL" != "0" ]; then
    echo
    echo "WARNING: a producer reported dropped connections or protocol errors."
    echo "         the numbers above are not a clean run. see $RESULTS/producers.log"
fi

echo
echo "artifacts in $RESULTS/"
