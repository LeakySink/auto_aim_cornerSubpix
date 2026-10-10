#!/bin/bash
# Unified portal. Usage: ./host/start.sh [--port 8080] [--no-browser]
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=_env.sh
. "$DIR/_env.sh"

echo "[start] hub portal"
exec "$HOST_PY" -m rdbg serve "$@"
