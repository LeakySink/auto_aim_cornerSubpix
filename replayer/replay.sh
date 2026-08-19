#!/bin/bash
# Replay a local .rlog by impersonating RemoteLogger → Host (UDP).
# Usage: ./replayer/replay.sh logs/run_xxx.rlog [--host IP] [--speed 1]
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"

HOST="${HOST:-127.0.0.1}"
CTRL_PORT="${CTRL_PORT:-15000}"
SPEED="${SPEED:-1}"

if [ $# -lt 1 ] || [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
  echo "Usage: $0 <file.rlog> [--host IP] [--ctrl-port P] [--name NAME] [--speed N] [--loop]"
  echo "  HOST / --host       default 127.0.0.1"
  echo "  CTRL_PORT           default 15000"
  echo "  SPEED               default 1 (realtime)"
  echo "First start Host: ./host/watch.sh"
  exit 1
fi

FILE="$1"
shift

ARGS=(--host "$HOST" --ctrl-port "$CTRL_PORT")
has_speed=0
for a in "$@"; do
  if [ "$a" = "--speed" ]; then has_speed=1; break; fi
done
if [ "$has_speed" = 0 ]; then
  ARGS+=(--speed "$SPEED")
fi

echo "[replayer] $FILE  -> $HOST:$CTRL_PORT"
cd "$DIR"
exec python3 replay.py "$FILE" "${ARGS[@]}" "$@"
