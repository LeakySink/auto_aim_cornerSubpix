#!/bin/bash
# Compatibility: open hub portal at /replay
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=_env.sh
. "$DIR/_env.sh"

if [ $# -lt 1 ] || [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
  echo "Usage: $0 <file.rlog>   # opens hub /replay (prefer ./host/start.sh)"
  exit 1
fi

RLOG="$1"
shift || true
echo "[replay] hub portal; load rlog in UI: $RLOG"
# pass path via query for future; UI still needs manual path for now
exec "$HOST_PY" -m rdbg serve --open "/replay" "$@"
