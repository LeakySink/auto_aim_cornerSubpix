#!/bin/bash
# Multi-robot field control. Usage: ./host/field.sh [--download-assets] [...]
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
export PYTHONPATH="$DIR${PYTHONPATH:+:$PYTHONPATH}"

HTTP_PORT="${HTTP_PORT:-8888}"
DATA_PORT="${DATA_PORT:-20000}"
CTRL_PORT="${CTRL_PORT:-15000}"

echo "[field] http://localhost:$HTTP_PORT  data:$DATA_PORT  ctrl:$CTRL_PORT"
exec python3 -m rdbg field --port "$HTTP_PORT" --data-port "$DATA_PORT" --ctrl-port "$CTRL_PORT" "$@"
