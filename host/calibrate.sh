#!/bin/bash
# Unix calibrate UI. Usage: ./host/calibrate.sh [--no-browser]
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
export PYTHONPATH="$DIR${PYTHONPATH:+:$PYTHONPATH}"

HTTP_PORT="${HTTP_PORT:-8090}"
DATA_PORT="${DATA_PORT:-15001}"
PEER_PORT="${PEER_PORT:-15100}"
DISCOVER_PORT="${DISCOVER_PORT:-15999}"

if command -v python3 >/dev/null 2>&1; then
  PY=python3
elif command -v python >/dev/null 2>&1; then
  PY=python
else
  echo "[calibrate] Python 3 not found" >&2
  exit 1
fi

echo "[calibrate] http://localhost:$HTTP_PORT  data:$DATA_PORT peer:$PEER_PORT"
echo "[calibrate] robot: ./build/calibrate configs/calibration.yaml"
exec "$PY" -m rdbg calibrate --port "$HTTP_PORT" --data-port "$DATA_PORT" \
  --peer-port "$PEER_PORT" --discover-port "$DISCOVER_PORT" "$@"
