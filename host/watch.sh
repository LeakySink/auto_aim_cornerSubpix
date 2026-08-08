#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
BACKEND_BIN="$BUILD_DIR/udp_backend"

HTTP_PORT="${HTTP_PORT:-8080}"
UDP_PORT="${UDP_PORT:-9871}"

if [ ! -f "$BUILD_DIR/udp_backend" ]; then
  echo "[run] building C++ backend..."
  cmake -B "$BUILD_DIR" "$SCRIPT_DIR" -DCMAKE_BUILD_TYPE=Release
  make -C "$BUILD_DIR" -j$(nproc)
fi

if [ ! -f "$SCRIPT_DIR/chart.umd.min.js" ]; then
  echo "[run] downloading Chart.js for offline use..."
  python3 "$SCRIPT_DIR/server.py" --download-chartjs
fi

echo "[run] starting Remote Debugger (HTTP:$HTTP_PORT UDP:$UDP_PORT)"
echo "[run] open http://localhost:$HTTP_PORT in your browser"
echo ""

python3 "$SCRIPT_DIR/server.py" \
  --backend "$BACKEND_BIN" \
  --port "$HTTP_PORT" \
  --udp-port "$UDP_PORT"
