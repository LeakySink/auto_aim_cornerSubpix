#!/bin/bash
# Local .rlog player. Usage: ./host/replay.sh logs/run_xxx.rlog
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
export PYTHONPATH="$DIR${PYTHONPATH:+:$PYTHONPATH}"

if [ $# -lt 1 ] || [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
  echo "Usage: $0 <file.rlog> [--port P] [--host IP] [--max-mb N] [--no-browser]"
  echo "  Preloads log into memory (<1GiB by default) and opens preview window."
  exit 1
fi

echo "[replay] $1"
exec python3 -m rdbg replay "$@"
