#!/bin/bash
# Compatibility: open hub portal (Calibrate is a feature on the home page)
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=_env.sh
. "$DIR/_env.sh"

HTTP_PORT="${HTTP_PORT:-8080}"
echo "[calibrate] redirecting to hub portal (use ./host/start.sh, then Calibrate)"
exec "$HOST_PY" -m rdbg serve --port "$HTTP_PORT" --open / "$@"
