#!/bin/bash
# Live debugger. Usage: ./host/watch.sh [--download-assets] [...]
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
export PYTHONPATH="$DIR${PYTHONPATH:+:$PYTHONPATH}"

HTTP_PORT="${HTTP_PORT:-8080}"
CTRL_PORT="${CTRL_PORT:-15000}"

echo "[watch] http://localhost:$HTTP_PORT  control:$CTRL_PORT"
exec python3 -m rdbg watch --port "$HTTP_PORT" --control-port "$CTRL_PORT" "$@"
