#!/bin/bash
# Replay a local .rlog in the debugger UI.
# Usage: ./host/rlog.sh logs/run_xxx.rlog
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
export PYTHONPATH="$DIR${PYTHONPATH:+:$PYTHONPATH}"

HTTP_PORT="${HTTP_PORT:-8080}"

if [ $# -lt 1 ] || [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
  echo "Usage: $0 <file.rlog> [--port PORT]"
  echo "  HTTP_PORT  default 8080"
  exit 1
fi

FILE="$1"
shift

echo "[replay] http://localhost:$HTTP_PORT  file:$FILE"
exec python3 -m rdbg replay "$FILE" --port "$HTTP_PORT" "$@"
