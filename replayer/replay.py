#!/usr/bin/env python3
"""Standalone .rlog player — preload to memory, local preview window."""

import argparse
import shutil
import subprocess
import sys
import time
import webbrowser
from pathlib import Path

from session import load_session_or_exit
from server import run as run_server

ROOT = Path(__file__).resolve().parent
DEFAULT_PORT = 8765
PORT_TRY = 20


def _open_window(url):
    for browser in ("google-chrome", "chromium-browser", "chromium", "microsoft-edge"):
        exe = shutil.which(browser)
        if exe:
            subprocess.Popen([exe, f"--app={url}"])
            return True
    webbrowser.open(url)
    return True


def _bind_server(session, host, port):
    for p in range(port, port + PORT_TRY):
        httpd, bound = run_server(session, host=host, port=p)
        if httpd is not None:
            if p != port:
                print(f"[replayer] port {port} busy, using {p}", file=sys.stderr)
            return httpd, bound
    print(
        f"[replayer] ports {port}-{port + PORT_TRY - 1} all busy",
        file=sys.stderr,
    )
    return None, 0


def main():
    p = argparse.ArgumentParser(description="Standalone .rlog player (local GUI)")
    p.add_argument("rlog", type=Path, help="path to .rlog")
    p.add_argument("--host", default="127.0.0.1", help="HTTP bind address")
    p.add_argument("--port", type=int, default=DEFAULT_PORT, help="HTTP port (auto +1 if busy)")
    p.add_argument("--max-mb", type=int, default=1024,
                   help="max preload size in MiB (default 1024)")
    p.add_argument("--no-browser", action="store_true", help="do not open browser")
    args = p.parse_args()

    max_bytes = max(64, args.max_mb) << 20
    rlog = args.rlog.expanduser()
    if not rlog.is_file():
        rlog = (ROOT / rlog).resolve()
    if not rlog.is_file():
        print(f"[replayer] file not found: {args.rlog}", file=sys.stderr)
        return 1
    session = load_session_or_exit(str(rlog), max_bytes=max_bytes)

    httpd, port = _bind_server(session, host=args.host, port=args.port)
    if httpd is None:
        return 1

    url = f"http://{args.host}:{port}/"
    print(f"[replayer] {url}", file=sys.stderr)
    if not args.no_browser:
        time.sleep(0.15)
        _open_window(url)

    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("\n[replayer] exit", file=sys.stderr)
    finally:
        httpd.shutdown()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
