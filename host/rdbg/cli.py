"""CLI: python -m rdbg, or host/watch.py / host/replay.py"""

import argparse
import os
import sys


def _has_flag(argv, flag):
    return any(a == flag or a.startswith(flag + "=") for a in argv)


def expand_watch_env(argv):
    """Insert HTTP_PORT / CTRL_PORT unless the flag is already present."""
    out = list(argv)
    if os.environ.get("HTTP_PORT") and not _has_flag(out, "--port"):
        out = ["--port", os.environ["HTTP_PORT"]] + out
    if os.environ.get("CTRL_PORT") and not _has_flag(out, "--control-port"):
        out = ["--control-port", os.environ["CTRL_PORT"]] + out
    return out


def _parser():
    p = argparse.ArgumentParser(
        prog="rdbg",
        description="Remote Debugger host (stdlib only)",
    )
    sub = p.add_subparsers(dest="cmd")

    watch = sub.add_parser("watch", aliases=["debugger"], help="live UDP debugger")
    watch.add_argument("--port", type=int, default=8080, help="HTTP port")
    watch.add_argument("--control-port", type=int, default=15000, help="UDP control port")
    watch.add_argument("--download-assets", action="store_true",
                       help="download Chart.js / Hammer / zoom for offline use")
    watch.add_argument("--no-browser", action="store_true", help="do not open a browser")

    rpl = sub.add_parser("replay", help="play local .rlog file")
    rpl.add_argument("rlog", help="path to .rlog")
    rpl.add_argument("--host", default="127.0.0.1", help="HTTP bind address")
    rpl.add_argument("--port", type=int, default=8765, help="HTTP port (auto +1 if busy)")
    rpl.add_argument("--max-mb", type=int, default=1024, help="preload memory cap in MiB")
    rpl.add_argument("--no-browser", action="store_true", help="do not open a browser")
    return p


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv:
        argv = ["watch"]
    elif argv[0].startswith("-") and argv[0] not in ("-h", "--help"):
        argv = ["watch"] + argv

    if argv and argv[0] in ("watch", "debugger"):
        argv = [argv[0]] + expand_watch_env(argv[1:])

    args = _parser().parse_args(argv)
    if getattr(args, "download_assets", False):
        from .http.httputil import download_vendor
        download_vendor()
        return 0

    if args.cmd in ("watch", "debugger"):
        from .apps.watch import run
        return run(args.port, args.control_port, args.no_browser)

    if args.cmd == "replay":
        from .apps.replay import run
        return run(args.rlog, args.host, args.port, args.max_mb, args.no_browser)

    _parser().print_help()
    return 0
