#!/bin/bash
# Trampoline → watch.py (Win/macOS/Linux 同一份入口)
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
if command -v python3 >/dev/null 2>&1; then
  PY=python3
elif command -v python >/dev/null 2>&1; then
  PY=python
else
  echo "[watch] Python 3 not found (install python3 / python)" >&2
  exit 1
fi
exec "$PY" "$DIR/watch.py" "$@"
