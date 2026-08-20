"""CLI: invoked by host/*.sh via python -m rdbg"""

import argparse
import sys


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
