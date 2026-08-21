#!/bin/bash
# Unix watch. Usage: ./host/watch.sh [--download-assets] [...]
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
export PYTHONPATH="$DIR${PYTHONPATH:+:$PYTHONPATH}"

HTTP_PORT="${HTTP_PORT:-8080}"
CTRL_PORT="${CTRL_PORT:-15000}"

if command -v python3 >/dev/null 2>&1; then
  PY=python3
elif command -v python >/dev/null 2>&1; then
  PY=python
else
  echo "[watch] Python 3 not found" >&2
  exit 1
fi

echo "[watch] http://localhost:$HTTP_PORT  control:$CTRL_PORT"
exec "$PY" -m rdbg watch --port "$HTTP_PORT" --control-port "$CTRL_PORT" "$@"
