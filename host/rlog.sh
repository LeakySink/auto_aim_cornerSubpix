#!/bin/bash
# Replay a local .rlog in the debugger UI (paced; images via /img/N).
# Usage: ./host/rlog.sh logs/run_xxx.rlog [--speed 2]
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
export PYTHONPATH="$DIR${PYTHONPATH:+:$PYTHONPATH}"

HTTP_PORT="${HTTP_PORT:-8080}"
SPEED="${SPEED:-1}"

if [ $# -lt 1 ] || [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
  echo "Usage: $0 <file.rlog> [--speed N] [--port PORT]"
  echo "  HTTP_PORT  default 8080"
  echo "  SPEED      default 1 (realtime); e.g. SPEED=2 or --speed 2"
  exit 1
fi

FILE="$1"
shift

echo "[replay] http://localhost:$HTTP_PORT  file:$FILE  speed=${SPEED}x"
exec python3 -m rdbg replay "$FILE" --port "$HTTP_PORT" --speed "$SPEED" "$@"