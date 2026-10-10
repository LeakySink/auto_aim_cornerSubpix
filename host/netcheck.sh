#!/bin/bash
# UDP 网络通达度检测。Usage: ./host/netcheck.sh ping|echo|discover|burst ...
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=_env.sh
. "$DIR/_env.sh"

exec "$HOST_PY" "$DIR/netcheck.py" "$@"
