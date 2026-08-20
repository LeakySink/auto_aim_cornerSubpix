#!/bin/bash
# Trampoline → replay.py (Win/macOS/Linux 同一份入口)
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
if [ $# -lt 1 ] || [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
  echo "Usage: $0 <file.rlog> [--port P] [--host IP] [--max-mb N] [--no-browser]"
  exit 1
fi
if command -v python3 >/dev/null 2>&1; then
  PY=python3
elif command -v python >/dev/null 2>&1; then
  PY=python
else
  echo "[replay] Python 3 not found (install python3 / python)" >&2
  exit 1
fi
exec "$PY" "$DIR/replay.py" "$@"
