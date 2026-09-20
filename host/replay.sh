#!/bin/bash
# Unix replay. Usage: ./host/replay.sh logs/run.rlog
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=_env.sh
. "$DIR/_env.sh"

if [ $# -lt 1 ] || [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
  echo "Usage: $0 <file.rlog> [--port P] [--host IP] [--max-mb N] [--no-browser]"
  exit 1
fi

echo "[replay] $1"
exec "$HOST_PY" -m rdbg replay "$@"
