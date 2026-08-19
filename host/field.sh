#!/bin/bash
# Field Control — multi-robot monitoring grid
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"

HTTP_PORT="${HTTP_PORT:-8888}"
DATA_PORT="${DATA_PORT:-20000}"
CTRL_PORT="${CTRL_PORT:-15000}"

echo "[field] http://localhost:$HTTP_PORT"
echo "[field] data port $DATA_PORT  ctrl port $CTRL_PORT"
echo "[field] Senders: ./build/remote_logger_test --name=robot_N"

python3 "$DIR/field.py" --port "$HTTP_PORT" --data-port "$DATA_PORT" --ctrl-port "$CTRL_PORT"
