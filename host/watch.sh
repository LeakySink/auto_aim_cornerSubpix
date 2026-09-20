#!/bin/bash
# Unix watch. Usage: ./host/watch.sh [--download-assets] [...]
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=_env.sh
. "$DIR/_env.sh"

HTTP_PORT="${HTTP_PORT:-8080}"
DATA_PORT="${DATA_PORT:-15001}"
PEER_PORT="${PEER_PORT:-15100}"
DISCOVER_PORT="${DISCOVER_PORT:-15999}"

echo "[watch] http://localhost:$HTTP_PORT  data:$DATA_PORT peer:$PEER_PORT"
exec "$HOST_PY" -m rdbg watch --port "$HTTP_PORT" --data-port "$DATA_PORT" \
  --peer-port "$PEER_PORT" --discover-port "$DISCOVER_PORT" "$@"
