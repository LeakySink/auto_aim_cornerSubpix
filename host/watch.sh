#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

HTTP_PORT="${HTTP_PORT:-8080}"
CTRL_PORT="${CTRL_PORT:-15000}"

echo "[run] starting Remote Debugger (HTTP:$HTTP_PORT control:$CTRL_PORT)"
echo "[run] open http://localhost:$HTTP_PORT in your browser"
echo ""

python3 "$SCRIPT_DIR/server.py" \
  --port "$HTTP_PORT" \
  --control-port "$CTRL_PORT"
