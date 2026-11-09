#!/usr/bin/env bash
#
# build.sh — builds everything the report runs, into build/<machine>/
#
#   build.sh MACHINE
#
# - quicxd-v2      the adaptive-pool worktree, release flags (-O2)
# - quicxd-1.0.4   tag v1.0.4 (static PMAD pool), release flags (-O2)
# - test_apool, test_wire                   as the Makefile builds them (-g)
# - bench_apool, bench_purge                -O2
# - bench_apool_ps<N>  bench_apool with APOOL_PAGE_SIZE = 4, 8, 16, 32, 64 KiB
# - bench clients (worker, producer, report, fill) from scripts/bench
#
set -euo pipefail
MACHINE=${1:?usage: build.sh MACHINE}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPORT="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$REPORT/../.." && pwd)"          # the adaptive-pool worktree
BASE="$REPORT/build/src-1.0.4"               # git archive v1.0.4
OUT="$REPORT/build/$MACHINE"
CC=${CC:-cc}
mkdir -p "$OUT"

INC="-Ipmad/include -Ipmad/include/stuctures -Iquicx/include"
COMMON="-std=c17 -D_GNU_SOURCE"
PMAD_SRC="pmad/src/PMAD.c pmad/src/incPMAD.c pmad/src/MemoryPool.c pmad/src/BlockHeader.c"
CORE_V2="quicx/src/queue.c quicx/src/workers.c quicx/src/dispacher.c quicx/src/protocol.c
         quicx/src/event_loop.c quicx/src/server.c quicx/src/daemon.c quicx/src/stats.c
         quicx/src/cli.c quicx/src/config.c quicx/src/pool_config.c quicx/src/quicx.c quicx/src/apool.c"

echo "compiler: $($CC --version | head -1)"

# the two older versions, from git (the server gets these folders by rsync)
for pair in "v1.0.4:$BASE" "e7d4cdf:$REPORT/build/src-v1"; do
    rev=${pair%%:*}; dir=${pair#*:}
    if [ ! -d "$dir" ]; then
        mkdir -p "$dir"
        git -C "$ROOT" archive "$rev" | tar -x -C "$dir"
    fi
done

cd "$ROOT"
$CC $COMMON $INC -O2 quicx/src/main.c $CORE_V2 -o "$OUT/quicxd-v2" -lpthread
# the same daemon with every allocator feature on (adds fullest-first to the defaults)
$CC $COMMON $INC -O2 -DQUICX_POOL_FLAGS=APOOL_F_ALL quicx/src/main.c $CORE_V2 -o "$OUT/quicxd-v2-all" -lpthread
$CC $COMMON $INC -g quicx/src/tests/test_apool.c quicx/src/apool.c -o "$OUT/test_apool"
$CC $COMMON $INC -g quicx/src/tests/test_wire.c quicx/src/protocol.c -o "$OUT/test_wire"
$CC $COMMON $INC -g quicx/src/tests/test_config.c quicx/src/config.c quicx/src/pool_config.c quicx/src/apool.c -o "$OUT/test_config"
$CC $COMMON $INC -O2 quicx/src/tests/bench_apool.c quicx/src/tests/pmad_layout.c quicx/src/apool.c $PMAD_SRC -o "$OUT/bench_apool"
$CC $COMMON $INC -O2 quicx/src/tests/bench_purge.c quicx/src/apool.c -o "$OUT/bench_purge"
for ps in 4 8 16 32 64; do
    $CC $COMMON $INC -O2 -DAPOOL_PAGE_SIZE=$((ps * 1024)) \
        quicx/src/tests/bench_apool.c quicx/src/tests/pmad_layout.c quicx/src/apool.c $PMAD_SRC -o "$OUT/bench_apool_ps$ps"
done

# the baseline: v1.0.4 with its own Makefile's source list, release flags
cd "$BASE"
CORE_104=$(make -s -n quicxd 2>/dev/null | tr -s ' \t\\' '\n' | grep -E '^(quicx|pmad)/src/.*\.c$' | tr '\n' ' ')
$CC $COMMON $INC -O2 $CORE_104 -o "$OUT/quicxd-1.0.4" -lpthread

# the first adaptive pool (v1, before the papers): commit e7d4cdf, same flags.
# git archive output is prepared on the Mac in build/src-v1 (the server gets it via rsync)
cd "$REPORT/build/src-v1"
CORE_V1=$(make -s -n quicxd 2>/dev/null | tr -s ' \t\\' '\n' | grep -E '^(quicx|pmad)/src/.*\.c$' | tr '\n' ' ')
$CC $COMMON $INC -O2 $CORE_V1 -o "$OUT/quicxd-v1" -lpthread

# bench clients: run.sh builds its own three with scripts/bench/Makefile;
# fill (capacity client) is built here
cd "$HERE/bench"
make -s clean >/dev/null 2>&1 || true
make -s CC="$CC"
$CC -std=c17 -O2 -D_GNU_SOURCE fill.c -o "$OUT/fill"

echo "built into $OUT:"
ls "$OUT"
"$OUT/quicxd-v2" version 2>/dev/null || true
"$OUT/quicxd-1.0.4" version 2>/dev/null || true
