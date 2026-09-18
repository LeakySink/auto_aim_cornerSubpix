#!/bin/bash
# UDP 网络通达度检测。Usage: ./host/netcheck.sh ping|echo|discover|burst ...
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"

if command -v python3 >/dev/null 2>&1; then
  PY=python3
elif command -v python >/dev/null 2>&1; then
  PY=python
else
  echo "[netcheck] Python 3 not found" >&2
  exit 1
fi

exec "$PY" "$DIR/netcheck.py" "$@"
