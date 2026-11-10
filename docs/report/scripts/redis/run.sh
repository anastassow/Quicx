#!/usr/bin/env bash
# Quicx next to Redis, each in a Docker container limited to 64 MB (no swap),
# both sent the same 100-byte tasks at 40,000 a second by one producer.
#
#   docs/report/scripts/redis/run.sh default      Redis with its defaults (no maxmemory), Quicx with 8 MiB
#   docs/report/scripts/redis/run.sh limit 8      both limited to 8 MiB
#   docs/report/scripts/redis/run.sh limit 56     both limited to 56 MiB
#
# Needs Docker and python3. Results go to docs/report/results/redis/.
set -euo pipefail
cd "$(dirname "$0")/../../../.."
MODE=${1:?default or limit}; BUDGET=${2:-8}
OUT=docs/report/results/redis; mkdir -p "$OUT"
SAMPLE='while :; do cat /sys/fs/cgroup/memory.current; sleep 0.05; done'

docker build -q -f docs/report/scripts/redis/Dockerfile -t quicx:local . >/dev/null
docker pull -q redis:7-alpine >/dev/null
RUN=$(date +%s); R=qxcmp-redis-$RUN; Q=qxcmp-quicx-$RUN

if [ "$MODE" = default ]; then
  REDIS_ARGS="--protected-mode no"; POOL=8; TAG=default
else
  REDIS_ARGS="--protected-mode no --maxmemory ${BUDGET}mb"; POOL=$BUDGET; TAG=limit-$BUDGET
fi
CONF=$(mktemp); printf '[server]\nport = 16381\n\n[memory]\npool_size = %sM\n' "$POOL" > "$CONF"

docker run -d --name "$R" --memory 64m --memory-swap 64m -p 16379:6379 redis:7-alpine \
  sh -c "redis-server $REDIS_ARGS & $SAMPLE" >/dev/null
docker run -d --name "$Q" --memory 64m --memory-swap 64m -p 16381:16381 -v "$CONF:/etc/quicx.conf:ro" quicx:local \
  sh -c "quicx start --config /etc/quicx.conf >/tmp/quicx.log 2>&1 & $SAMPLE" >/dev/null
sleep 2

python3 docs/report/scripts/redis/flood.py "$R" "$Q" "$OUT/$TAG.json"

# the memory each container used, sampled every 50 ms, with Docker's timestamps
docker logs -t "$R" > "$OUT/$TAG-redis-memory.txt" 2>&1 || true
docker logs -t "$Q" > "$OUT/$TAG-quicx-memory.txt" 2>&1 || true
docker inspect "$R" --format 'redis: {{.State.Status}} oomkilled={{.State.OOMKilled}}' | tee "$OUT/$TAG.txt"
docker inspect "$Q" --format 'quicx: {{.State.Status}} oomkilled={{.State.OOMKilled}}' | tee -a "$OUT/$TAG.txt"
docker stop "$R" "$Q" >/dev/null 2>&1 || true
echo "containers $R and $Q are stopped; delete them with docker container prune"
