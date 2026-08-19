#!/bin/bash
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HTTP_PORT="${HTTP_PORT:-8080}"
CTRL_PORT="${CTRL_PORT:-15000}"

echo "[run] starting Remote Debugger (HTTP:$HTTP_PORT control:$CTRL_PORT)"
echo "[run] open http://localhost:$HTTP_PORT in your browser"
echo ""

cd "$ROOT"
python3 -m host debugger --port "$HTTP_PORT" --control-port "$CTRL_PORT"
