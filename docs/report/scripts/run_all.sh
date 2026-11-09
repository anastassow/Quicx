#!/usr/bin/env bash
#
# run_all.sh — every test and benchmark of the report on this machine.
#
#   run_all.sh MACHINE            (after build.sh MACHINE)
#
# writes raw output to results/MACHINE/. repeated runs are interleaved
# (1.0.4, v1, v2, 1.0.4, v1, v2, ...) so slow drift hits every version alike.
# REPS (default 5), PORT (default 16391), RATE (open-loop tasks/s, default:
# 40000, or half the slowest closed-loop median when that is below 80k).
#
set -uo pipefail
MACHINE=${1:?usage: run_all.sh MACHINE}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPORT="$(cd "$HERE/.." && pwd)"
B="$REPORT/build/$MACHINE"
R="$REPORT/results/$MACHINE"
REPS=${REPS:-5}
PORT=${PORT:-16391}
VERSIONS=(1.0.4 v1 v2)
mkdir -p "$R"
log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$R/run.log"; }

if pgrep -x quicxd >/dev/null 2>&1 || pgrep -f "quicxd start" >/dev/null 2>&1; then
    log "another quicxd is running on this machine; refusing to start (run.sh would stop it)"
    pgrep -fl quicxd | tee -a "$R/run.log"
    exit 1
fi

# ---- the machine -------------------------------------------------------
{
    echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "uname: $(uname -a)"
    if [ "$(uname)" = "Darwin" ]; then
        echo "cpu: $(sysctl -n machdep.cpu.brand_string)"
        echo "cores: $(sysctl -n hw.ncpu) ($(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null) performance + $(sysctl -n hw.perflevel1.physicalcpu 2>/dev/null) efficiency)"
        echo "ram_bytes: $(sysctl -n hw.memsize)"
        echo "os: $(sw_vers -productName) $(sw_vers -productVersion)"
        echo "model: $(sysctl -n hw.model)"
    else
        echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
        lscpu 2>/dev/null | grep -E '^(Architecture|CPU\(s\)|Model name|Thread|Core|Socket|CPU max MHz)' | sed 's/^/lscpu: /'
        echo "ram_kb: $(grep MemTotal /proc/meminfo | awk '{print $2}')"
        echo "os: $(. /etc/os-release; echo "$PRETTY_NAME")"
        echo "kernel: $(uname -r)"
        echo "cpu_governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
        echo "load_at_start: $(cut -d' ' -f1-3 /proc/loadavg)"
    fi
    echo "os_page_size: $(getconf PAGESIZE)"
    echo "compiler: $(${CC:-cc} --version | head -1)"
    echo "python: $(python3 --version 2>&1)"
} > "$R/machine.txt"
log "machine info written"

# ---- unit tests --------------------------------------------------------
"$B/test_apool" > "$R/test_apool.txt" 2>&1; log "test_apool exit $?: $(tail -1 "$R/test_apool.txt")"
"$B/test_wire"  > "$R/test_wire.txt"  2>&1; log "test_wire exit $?: $(tail -1 "$R/test_wire.txt")"

# ---- end-to-end tests --------------------------------------------------
for v in "${VERSIONS[@]}"; do
    python3 "$HERE/e2e_tests.py" "$B/quicxd-$v" "$PORT" > "$R/e2e_$v.txt" 2>&1
    log "e2e $v: $(tail -1 "$R/e2e_$v.txt")"
done

# every allocator feature on together (fullest-first added to the defaults)
python3 "$HERE/e2e_tests.py" "$B/quicxd-v2-all" "$PORT" > "$R/e2e_v2-all.txt" 2>&1
log "e2e v2-all: $(tail -1 "$R/e2e_v2-all.txt")"
for i in 1 2 3; do
    python3 "$HERE/capacity.py" "$B/quicxd-v2-all" "$B/fill" "$PORT" 100 900 40 300 nginx \
        > "$R/capacity_v2-all_$i.txt" 2>&1
done
log "capacity v2-all: $(grep -m1 'phase=100' "$R/capacity_v2-all_1.txt" | cut -c1-60)"

# ---- allocator benchmarks ----------------------------------------------
for i in $(seq 1 "$REPS"); do
    "$B/bench_apool" > "$R/bench_apool_$i.txt" 2>&1; log "bench_apool run $i done"
    "$B/bench_purge" > "$R/bench_purge_$i.txt" 2>&1; log "bench_purge run $i done"
done
for i in $(seq 1 3); do
    for ps in 4 8 16 32 64; do
        "$B/bench_apool_ps$ps" > "$R/bench_ps${ps}_$i.txt" 2>&1
    done
    log "page-size sweep run $i done"
done

# ---- capacity and memory, whole daemon ---------------------------------
for i in $(seq 1 3); do
    for v in "${VERSIONS[@]}"; do
        python3 "$HERE/capacity.py" "$B/quicxd-$v" "$B/fill" "$PORT" 100 900 40 300 nginx \
            > "$R/capacity_${v}_$i.txt" 2>&1
        log "capacity $v run $i: $(grep -m1 'phase=100' "$R/capacity_${v}_$i.txt" | cut -c1-60)"
    done
done

# ---- throughput (closed loop) ------------------------------------------
cd "$HERE/bench"
for i in $(seq 1 "$REPS"); do
    for v in "${VERSIONS[@]}"; do
        QUIET_RUN=1 DAEMON="$B/quicxd-$v" ./run.sh -w 8 -p 2 -c 4 -i 32 -d 10 -u 2 -P "$PORT" \
            > "$R/closed_${v}_$i.txt" 2>&1
        log "closed $v run $i: $(tail -1 "$R/closed_${v}_$i.txt")"
        sleep 1
    done
done

# ---- latency (open loop at a fixed rate) -------------------------------
if [ -z "${RATE:-}" ]; then
    RATE=$(python3 - "$R" <<'EOF'
import glob, re, statistics, sys
meds = []
for v in ("1.0.4", "v1", "v2"):
    xs = []
    for f in glob.glob(f"{sys.argv[1]}/closed_{v}_*.txt"):
        m = re.search(r"thr=\[([^\]]*)\]", open(f).read())
        if m:
            xs.append(sum(float(t[:-1]) * 1000 if t.endswith("k") else float(t) for t in m.group(1).split()))
    if xs:
        meds.append(statistics.median(xs))
slow = min(meds) if meds else 80000
print(40000 if slow >= 80000 else int(slow / 2 // 1000 * 1000))
EOF
)
fi
echo "open_loop_rate: $RATE" >> "$R/machine.txt"
log "open-loop rate $RATE tasks/s"
for i in $(seq 1 "$REPS"); do
    for v in "${VERSIONS[@]}"; do
        QUIET_RUN=1 DAEMON="$B/quicxd-$v" ./run.sh -w 8 -p 2 -c 4 -r "$RATE" -d 10 -u 2 -P "$PORT" \
            > "$R/open_${v}_$i.txt" 2>&1
        log "open $v run $i: $(tail -1 "$R/open_${v}_$i.txt")"
        sleep 1
    done
done

log "all done"
