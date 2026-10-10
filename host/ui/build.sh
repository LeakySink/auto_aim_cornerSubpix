#!/bin/bash
# Build SPA into host/rdbg/static_ui
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
NODE_BIN=""
if [ -x "$DIR/.tools/node/bin/node" ]; then
  export PATH="$DIR/.tools/node/bin:$PATH"
  NODE_BIN="$DIR/.tools/node/bin/node"
elif command -v node >/dev/null 2>&1; then
  NODE_BIN="$(command -v node)"
else
  echo "[ui] Node.js not found. Install Node 18+ or place portable node in host/ui/.tools/node" >&2
  exit 1
fi
VER="$("$NODE_BIN" -v | sed 's/^v//' | cut -d. -f1)"
if [ "$VER" -lt 18 ]; then
  echo "[ui] Node >= 18 required (got $("$NODE_BIN" -v))" >&2
  exit 1
fi
cd "$DIR"
if [ ! -d node_modules ]; then
  npm install
fi
npm run build
echo "[ui] built -> $DIR/../rdbg/static_ui"
