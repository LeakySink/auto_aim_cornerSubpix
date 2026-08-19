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


def _open_window(url):
    for browser in ("google-chrome", "chromium-browser", "chromium", "microsoft-edge"):
        exe = shutil.which(browser)
        if exe:
            subprocess.Popen([exe, f"--app={url}"])
            return True
    webbrowser.open(url)
    return True


def main():
    p = argparse.ArgumentParser(description="Standalone .rlog player (local GUI)")
    p.add_argument("rlog", help="path to .rlog")
    p.add_argument("--host", default="127.0.0.1", help="HTTP bind address")
    p.add_argument("--port", type=int, default=8765, help="HTTP port")
    p.add_argument("--max-mb", type=int, default=1024,
                   help="max preload size in MiB (default 1024)")
    p.add_argument("--no-browser", action="store_true", help="do not open browser")
    args = p.parse_args()

    max_bytes = max(64, args.max_mb) << 20
    session = load_session_or_exit(args.rlog, max_bytes=max_bytes)

    httpd, port = run_server(session, host=args.host, port=args.port)
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
