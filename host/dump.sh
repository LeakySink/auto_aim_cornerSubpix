#!/bin/bash
# Unix dump. Usage: ./host/dump.sh logs/run.rlog [-o DIR] [--fps N]
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=_env.sh
. "$DIR/_env.sh"

if [ $# -lt 1 ] || [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
  echo "Usage: $0 <file.rlog> [-o DIR] [--fps N]"
  echo "  Writes DIR/log.txt, DIR/plot.txt, DIR/images.mp4"
  echo "  Default DIR: <stem>_dump next to the .rlog"
  exit 1
fi

echo "[dump] $1"
exec "$HOST_PY" -m rdbg dump "$@"
