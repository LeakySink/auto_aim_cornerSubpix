# Shared by host/*.sh — sets HOST_PY to host/.venv or system Python.
# Usage:  DIR=...; . "$DIR/_env.sh"
#
# Policy:
#   - If host/.venv looks usable → use it
#   - Else if network reachable → create .venv, seed pip, install requirements.txt
#   - Else (or create fails) → system python3/python
#
# Notes: Debian without ensurepip → `venv --without-pip`, then seed pip via
# the base interpreter's `python -m pip install --target ...`.

_HOST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

_host_base_py() {
  if command -v python3 >/dev/null 2>&1; then
    echo python3
  elif command -v python >/dev/null 2>&1; then
    echo python
  else
    echo ""
  fi
}

_host_online() {
  local py="$1"
  [ -n "$py" ] || return 1
  "$py" - <<'PY' 2>/dev/null
import socket
hosts = (
    ("pypi.org", 443),
    ("files.pythonhosted.org", 443),
    ("pypi.tuna.tsinghua.edu.cn", 443),
)
for host, port in hosts:
    try:
        socket.create_connection((host, port), timeout=3).close()
        raise SystemExit(0)
    except OSError:
        pass
raise SystemExit(1)
PY
}

_host_venv_ok() {
  local venv="$1"
  [ -x "$venv/bin/python" ] && "$venv/bin/python" -c "import pip" 2>/dev/null
}

_host_rm_venv() {
  rm -rf "$1" 2>/dev/null || true
}

_host_site_packages() {
  local venv="$1"
  "$venv/bin/python" -c 'import sysconfig; print(sysconfig.get_path("purelib"))'
}

_host_write_pip_launcher() {
  local venv="$1"
  # thin wrappers so `pip` / `pip3` invoke venv python -m pip
  for name in pip pip3; do
    cat > "$venv/bin/$name" <<'EOF'
#!/bin/sh
exec "$(dirname "$0")/python" -m pip "$@"
EOF
    chmod +x "$venv/bin/$name"
  done
}

# Seed pip into a --without-pip venv using the base interpreter's pip module.
_host_bootstrap_pip() {
  local venv="$1" base="$2" site
  if "$venv/bin/python" -c "import pip" 2>/dev/null; then
    _host_write_pip_launcher "$venv"
    return 0
  fi
  if ! "$base" -m pip --version >/dev/null 2>&1; then
    echo "[host] base Python has no pip module" >&2
    return 1
  fi
  site="$(_host_site_packages "$venv")"
  [ -n "$site" ] || return 1
  mkdir -p "$site"
  if ! "$base" -m pip install -q --target "$site" pip; then
    return 1
  fi
  _host_write_pip_launcher "$venv"
  "$venv/bin/python" -c "import pip" 2>/dev/null
}

_host_create_venv() {
  local base="$1" venv="$2"
  _host_rm_venv "$venv"

  if ! "$base" -m venv --without-pip "$venv" >/dev/null 2>&1; then
    _host_rm_venv "$venv"
    return 1
  fi
  if [ ! -x "$venv/bin/python" ]; then
    _host_rm_venv "$venv"
    return 1
  fi
  if ! _host_bootstrap_pip "$venv" "$base"; then
    echo "[host] could not seed pip into .venv" >&2
    _host_rm_venv "$venv"
    return 1
  fi
  return 0
}

_host_ensure_py() {
  local base venv
  base="$(_host_base_py)"
  if [ -z "$base" ]; then
    echo "[host] Python 3 not found" >&2
    return 1
  fi

  venv="$_HOST_DIR/.venv"
  if _host_venv_ok "$venv"; then
    HOST_PY="$venv/bin/python"
    return 0
  fi
  if [ -e "$venv" ]; then
    echo "[host] removing broken .venv" >&2
    _host_rm_venv "$venv"
  fi

  if ! _host_online "$base"; then
    echo "[host] .venv missing and offline; using system Python ($base)" >&2
    HOST_PY="$base"
    return 0
  fi

  echo "[host] creating $_HOST_DIR/.venv ..." >&2
  if ! _host_create_venv "$base" "$venv"; then
    echo "[host] venv create failed; using system Python" >&2
    HOST_PY="$base"
    return 0
  fi

  if [ -f "$_HOST_DIR/requirements.txt" ]; then
    echo "[host] pip install -r requirements.txt ..." >&2
    if ! "$venv/bin/python" -m pip install -q -r "$_HOST_DIR/requirements.txt"; then
      echo "[host] pip install failed; removing incomplete .venv, using system Python" >&2
      _host_rm_venv "$venv"
      HOST_PY="$base"
      return 0
    fi
  fi

  HOST_PY="$venv/bin/python"
  echo "[host] using $HOST_PY" >&2
  return 0
}

_host_ensure_py || return 1
export HOST_PY
export PYTHONPATH="${_HOST_DIR}${PYTHONPATH:+:$PYTHONPATH}"
