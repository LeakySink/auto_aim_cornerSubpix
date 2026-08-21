#!/bin/bash
# Unix replay. Usage: ./host/replay.sh logs/run.rlog
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
export PYTHONPATH="$DIR${PYTHONPATH:+:$PYTHONPATH}"

if [ $# -lt 1 ] || [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
  echo "Usage: $0 <file.rlog> [--port P] [--host IP] [--max-mb N] [--no-browser]"
  exit 1
fi

if command -v python3 >/dev/null 2>&1; then
  PY=python3
elif command -v python >/dev/null 2>&1; then
  PY=python
else
  echo "[replay] Python 3 not found" >&2
  exit 1
fi

echo "[replay] $1"
exec "$PY" -m rdbg replay "$@"
