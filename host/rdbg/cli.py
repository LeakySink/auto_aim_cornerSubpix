"""CLI: invoked by host/*.sh via python -m rdbg"""

import argparse
import sys


def _parser():
    p = argparse.ArgumentParser(
        prog="rdbg",
        description="Remote Debugger host (stdlib only)",
    )
    sub = p.add_subparsers(dest="cmd")

    serve = sub.add_parser("serve", aliases=["hub"], help="unified portal (default)")
    serve.add_argument("--host", default="0.0.0.0", help="HTTP bind address")
    serve.add_argument("--port", type=int, default=8080, help="HTTP port")
    serve.add_argument("--no-browser", action="store_true")
    serve.add_argument("--open", default="/", dest="open_path",
                       help="path to open in browser (e.g. /watch)")

    watch = sub.add_parser("watch", aliases=["debugger"], help="live UDP debugger (legacy)")
    watch.add_argument("--port", type=int, default=8080, help="HTTP port")
    watch.add_argument("--data-port", type=int, default=15001, help="UDP data port")
    watch.add_argument("--peer-port", type=int, default=15100, help="host-to-host peer port")
    watch.add_argument("--discover-port", type=int, default=15999, help="LAN beacon port")
    watch.add_argument("--control-port", type=int, default=15000,
                       help="ignored (robot listens here; kept for old scripts)")
    watch.add_argument("--download-assets", action="store_true",
                       help="download Chart.js / Hammer / zoom for offline use")
    watch.add_argument("--no-browser", action="store_true", help="do not open a browser")

    rpl = sub.add_parser("replay", help="play local .rlog file (legacy)")
    rpl.add_argument("rlog", help="path to .rlog")
    rpl.add_argument("--host", default="127.0.0.1", help="HTTP bind address")
    rpl.add_argument("--port", type=int, default=8765, help="HTTP port (auto +1 if busy)")
    rpl.add_argument("--max-mb", type=int, default=1024, help="preload memory cap in MiB")
    rpl.add_argument("--no-browser", action="store_true", help="do not open a browser")

    dump = sub.add_parser("dump", help="export .rlog to a folder (log/plot/video)")
    dump.add_argument("rlog", help="path to .rlog")
    dump.add_argument("-o", "--output", default=None,
                      help="output directory (default: <stem>_dump next to rlog)")
    dump.add_argument("--fps", type=float, default=10.0,
                      help="images.mp4 frame rate (default 10)")
    return p


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv:
        argv = ["serve"]
    elif argv[0].startswith("-") and argv[0] not in ("-h", "--help"):
        argv = ["serve"] + argv

    args = _parser().parse_args(argv)
    if getattr(args, "download_assets", False):
        from .http.httputil import download_vendor
        download_vendor()
        return 0

    if args.cmd in ("serve", "hub"):
        from .apps.hub_app import run
        return run(args.host, args.port, args.no_browser, args.open_path)

    if args.cmd in ("watch", "debugger"):
        from .apps.watch import run
        return run(args.port, args.data_port, args.peer_port, args.discover_port,
                   args.no_browser)

    if args.cmd == "replay":
        from .apps.replay import run
        return run(args.rlog, args.host, args.port, args.max_mb, args.no_browser)

    if args.cmd == "dump":
        from .log.dump import dump_rlog_to_path
        return dump_rlog_to_path(args.rlog, output=args.output, fps=args.fps)

    _parser().print_help()
    return 0
