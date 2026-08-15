#!/usr/bin/env bash
#
# sweep.sh — drive quicx at a range of offered rates and tabulate the
# latency at each one.
#
# this is the measurement that answers "is the daemon fast enough yet".
# a single throughput number cannot: what matters is the rate at which
# latency stops being flat. below the knee the daemon keeps up and p99
# barely moves; above it, work arrives faster than it drains, the
# backlog grows for the whole run and latency climbs without bound.
#
# run it once before changing anything, keep the output, then run it
# again after. the knee moving right is the only proof that a change
# helped.
#
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

RATES=${RATES:-"5000 10000 20000 40000 60000 80000 100000 140000"}
WORKERS=${WORKERS:-8}
PRODUCERS=${PRODUCERS:-2}
CONNS=${CONNS:-4}
DURATION=${DURATION:-8}
WARMUP=${WARMUP:-2}
SPIN_US=${SPIN_US:-0}

echo
echo "rate sweep — workers=$WORKERS producers=$PRODUCERS conns=$CONNS spin=${SPIN_US}us duration=${DURATION}s"
echo
printf "%10s  %10s  %10s  %10s  %10s  %8s\n" "offered" "p50" "p99" "p99.9" "max" "errors"
printf "%10s  %10s  %10s  %10s  %10s  %8s\n" "---------" "---------" "---------" "---------" "---------" "-------"

for rate in $RATES; do
    line=$(QUIET_RUN=1 "$HERE/run.sh" \
              -w "$WORKERS" -p "$PRODUCERS" -c "$CONNS" \
              -r "$rate" -d "$DURATION" -u "$WARMUP" -s "$SPIN_US" 2>/dev/null)

    if [ -z "$line" ]; then
        printf "%10s  %s\n" "$rate" "run failed"
        continue
    fi

    p50=$(echo  "$line" | sed -n 's/.*p50=\([0-9.]*\).*/\1/p')
    p99=$(echo  "$line" | sed -n 's/.*p99=\([0-9.]*\).*/\1/p')
    p999=$(echo "$line" | sed -n 's/.*p999=\([0-9.]*\).*/\1/p')
    mx=$(echo   "$line" | sed -n 's/.*max=\([0-9.]*\).*/\1/p')
    er=$(echo   "$line" | sed -n 's/.*err=\([0-9]*\).*/\1/p')
    dr=$(echo   "$line" | sed -n 's/.*drops=\([0-9]*\).*/\1/p')

    flag=""
    [ "${dr:-0}" != "0" ] && flag="  <- dropped connections"

    printf "%10s  %8s us  %8s us  %8s us  %8s us  %8s%s\n" \
           "$rate" "$p50" "$p99" "$p999" "$mx" "${er:-?}" "$flag"

    sleep 1   # let TIME_WAIT sockets drain between runs
done

echo
echo "the knee is the first rate where p99 stops tracking p50."
echo "that is the daemon's usable capacity for this workload."
echo
