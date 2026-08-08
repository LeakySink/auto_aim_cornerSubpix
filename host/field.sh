#!/bin/bash
# Field Control — multi-robot monitoring grid
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"

if [ ! -f "$DIR/build/udp_backend" ]; then
  echo "[field] building C++ backend..."
  cmake -B "$DIR/build" "$DIR" -DCMAKE_BUILD_TYPE=Release
  make -C "$DIR/build" -j$(nproc)
fi

HTTP_PORT="${HTTP_PORT:-8888}"
DATA_PORT="${DATA_PORT:-20000}"

echo "[field] http://localhost:$HTTP_PORT"
echo "[field] data port $DATA_PORT"
echo "[field] Run senders: ./build/remote_logger_test --ctrl-port=0 --port=$DATA_PORT --name=robot_N"

python3 "$DIR/field.py" --port "$HTTP_PORT" --data-port "$DATA_PORT"
