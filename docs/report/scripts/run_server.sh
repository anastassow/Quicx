#!/usr/bin/env bash
#
# run_server.sh — the whole suite on the Linux server, then results back here.
#
#   run_server.sh [user@host]          default: anastassow@da (Tailscale)
#
# needs key-based ssh once:  ssh-copy-id anastassow@da
# or an open shared connection: SSH_OPTS="-o ControlPath=~/.ssh/cm/%C" run_server.sh
# on the server it only uses ~/quicx-report/, never sudo, and refuses to run
# while another quicxd is running there.
#
set -euo pipefail
HOST=${1:-anastassow@da}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPORT="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$REPORT/../.." && pwd)"
SSH_OPTS=${SSH_OPTS:-"-o BatchMode=yes"}
SSH=(ssh $SSH_OPTS -o ConnectTimeout=15 "$HOST")

"${SSH[@]}" true || { echo "ssh to $HOST failed; run once: ssh-copy-id $HOST"; exit 1; }
if "${SSH[@]}" "pgrep -x quicxd || pgrep -fl '[q]uicxd start'" ; then
    echo "a quicxd is running on $HOST; not touching it"; exit 1
fi
"${SSH[@]}" 'command -v cc gcc python3 make rsync' || true

echo "syncing source to $HOST:~/quicx-report/"
rsync -az --delete -e "ssh $SSH_OPTS" \
    --exclude .git --exclude '*.dSYM' \
    --exclude '/docs/report/build/mac' --exclude '/docs/report/build/venv' --exclude '/docs/report/build/look' \
    --exclude '/docs/report/results' --exclude '/docs/report/figures' --exclude '/docs/report/fonts' \
    --exclude '/quicxd' --exclude '/test_*' --exclude '/bench_apool' \
    --exclude '/docs/report/scripts/bench/bench_worker' --exclude '/docs/report/scripts/bench/bench_producer' \
    --exclude '/docs/report/scripts/bench/bench_report' --exclude '/docs/report/scripts/bench/results' \
    "$ROOT/" "$HOST:quicx-report/"

echo "building and starting the runs (detached, survives a dropped connection)"
"${SSH[@]}" 'cd ~/quicx-report/docs/report && mkdir -p results/server &&
    ./scripts/build.sh server > results/server/build.log 2>&1 &&
    (nohup ./scripts/run_all.sh server > results/server/run_all.out 2>&1 < /dev/null &) ; tail -3 results/server/build.log'

echo "waiting for the server to finish"
while sleep 60; do
    last=$("${SSH[@]}" 'tail -1 ~/quicx-report/docs/report/results/server/run.log 2>/dev/null' || echo "(ssh failed, retrying)")
    echo "  $last"
    case "$last" in *"all done"*|*"refusing"*) break ;; esac
done

mkdir -p "$REPORT/results/server"
rsync -az -e "ssh $SSH_OPTS" "$HOST:quicx-report/docs/report/results/server/" "$REPORT/results/server/"
cd "$REPORT"
python3 scripts/make_tables.py
tectonic -X compile main.tex --keep-logs >/dev/null && echo "main.pdf rebuilt with the server results"
