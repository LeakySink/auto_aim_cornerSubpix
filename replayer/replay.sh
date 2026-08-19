#!/bin/bash
# Standalone .rlog player — left image, right charts, draggable scrubber.
# Usage: ./replayer/replay.sh logs/run_xxx.rlog
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"

PORT="${PORT:-8765}"
HOST="${HOST:-127.0.0.1}"

if [ $# -lt 1 ] || [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
  echo "Usage: $0 <file.rlog> [--port P] [--host IP] [--max-mb N] [--no-browser]"
  echo "  Preloads log into memory (<1G by default) and opens preview window."
  exit 1
fi

FILE="$1"
shift

ARGS=(--host "$HOST" --port "$PORT")
has_port=0
for a in "$@"; do
  if [ "$a" = "--port" ]; then has_port=1; break; fi
done
if [ "$has_port" = 0 ]; then
  ARGS+=(--port "$PORT")
fi

echo "[replayer] $FILE"
cd "$DIR"
exec python3 replay.py "$FILE" "${ARGS[@]}" "$@"
