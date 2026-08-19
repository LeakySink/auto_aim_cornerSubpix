"""CLI: invoked by host/*.sh via python -m rdbg"""

import argparse
import sys


def _parser():
    p = argparse.ArgumentParser(
        prog="rdbg",
        description="Remote Debugger host (stdlib only)",
    )
    sub = p.add_subparsers(dest="cmd")

    dbg = sub.add_parser("debugger", help="single-robot debugger (default)")
    dbg.add_argument("--port", type=int, default=8080, help="HTTP port")
    dbg.add_argument("--control-port", type=int, default=15000, help="UDP control port")
    dbg.add_argument("--download-assets", action="store_true",
                     help="download Chart.js / Hammer / zoom for offline use")

    fld = sub.add_parser("field", help="multi-robot field control")
    fld.add_argument("--port", type=int, default=8888, help="HTTP port")
    fld.add_argument("--data-port", type=int, default=20000, help="shared UDP data port")
    fld.add_argument("--ctrl-port", type=int, default=15000, help="UDP control port")
    fld.add_argument("--download-assets", action="store_true",
                     help="download Chart.js / Hammer / zoom for offline use")

    rep = sub.add_parser("replay", help="replay a local .rlog file")
    rep.add_argument("rlog", help="path to .rlog")
    rep.add_argument("--port", type=int, default=8080, help="HTTP port")
    rep.add_argument(
        "--speed",
        type=float,
        default=1.0,
        help="playback speed (1=realtime, 2=2x; idle gaps capped)",
    )
    return p


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv:
        argv = ["debugger"]
    elif argv[0].startswith("-") and argv[0] not in ("-h", "--help"):
        argv = ["debugger"] + argv

    args = _parser().parse_args(argv)
    if getattr(args, "download_assets", False):
        from .httputil import download_vendor
        download_vendor()
        return 0

    if args.cmd == "field":
        from .apps.field import run
        return run(args.port, args.data_port, args.ctrl_port)

    if args.cmd == "debugger":
        from .apps.debugger import run
        return run(args.port, args.control_port)

    if args.cmd == "replay":
        from .apps.replay import run
        return run(args.rlog, args.port, speed=args.speed)

    _parser().print_help()
    return 0
