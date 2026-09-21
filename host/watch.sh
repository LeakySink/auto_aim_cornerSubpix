#!/bin/bash
# Compatibility: open hub portal at /watch
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=_env.sh
. "$DIR/_env.sh"

HTTP_PORT="${HTTP_PORT:-8080}"
echo "[watch] redirecting to hub portal /watch (use ./host/start.sh)"
exec "$HOST_PY" -m rdbg serve --port "$HTTP_PORT" --open /watch "$@"
