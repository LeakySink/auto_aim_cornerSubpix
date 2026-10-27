"""CLI: invoked by host/*.sh via python -m rdbg"""

import argparse
import sys


def _safe_print(text):
    """打印文本；遇到终端编码表示不了的字符就替换，而不是抛 UnicodeEncodeError。"""
    try:
        print(text)
    except UnicodeEncodeError:
        enc = getattr(sys.stdout, "encoding", None) or "utf-8"
        sys.stdout.write(text.encode(enc, "replace").decode(enc, "replace"))
        sys.stdout.write(chr(10))

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
                       help="path to open in browser")

    watch = sub.add_parser("watch", aliases=["debugger"], help="open portal home")
    watch.add_argument("--port", type=int, default=8080, help="HTTP port")
    watch.add_argument("--data-port", type=int, default=15001, help="ignored (feature starts from UI)")
    watch.add_argument("--peer-port", type=int, default=15100, help="ignored")
    watch.add_argument("--discover-port", type=int, default=15999, help="ignored")
    watch.add_argument("--control-port", type=int, default=15000, help="ignored")
    watch.add_argument("--download-assets", action="store_true", help="ignored")
    watch.add_argument("--no-browser", action="store_true", help="do not open a browser")

    cal = sub.add_parser("calibrate", help="open portal home (Calibrate is a hub feature)")
    cal.add_argument("--port", type=int, default=8080, help="HTTP port")
    cal.add_argument("--data-port", type=int, default=15001, help="ignored (feature starts from UI)")
    cal.add_argument("--peer-port", type=int, default=15100, help="ignored")
    cal.add_argument("--discover-port", type=int, default=15999, help="ignored")
    cal.add_argument("--no-browser", action="store_true", help="do not open a browser")

    rpl = sub.add_parser("replay", help="open portal home")
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

    met = sub.add_parser("metrics", help="analyse a .rlog and print a per-frame quality report")
    met.add_argument("rlog", help="path to .rlog")
    met.add_argument("--title", default=None, help="report title (default: file stem)")
    met.add_argument("--derived", default=None,
                     help="also write the derived series (unwrapped phase etc.) to this CSV")

    csvp = sub.add_parser("csv", help="export a .rlog to a per-frame CSV")
    csvp.add_argument("rlog", help="path to .rlog")
    csvp.add_argument("-o", "--output", default=None,
                      help="output CSV path (default: <rlog stem>.csv)")
    return p


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv:
        argv = ["serve"]
    elif argv[0].startswith("-") and argv[0] not in ("-h", "--help"):
        argv = ["serve"] + argv

    args = _parser().parse_args(argv)

    if args.cmd in ("serve", "hub"):
        from .apps.hub_app import run
        return run(args.host, args.port, args.no_browser, args.open_path)

    if args.cmd in ("watch", "debugger"):
        from .apps.hub_app import run
        return run("0.0.0.0", args.port, args.no_browser, "/")

    if args.cmd == "calibrate":
        from .apps.hub_app import run
        return run("0.0.0.0", args.port, args.no_browser, "/")

    if args.cmd == "replay":
        print(f"[replay] open home, then Replay, and load: {args.rlog}", file=sys.stderr)
        from .apps.hub_app import run
        return run(args.host, args.port, args.no_browser, "/")

    if args.cmd == "dump":
        from .log.dump import dump_rlog_to_path
        return dump_rlog_to_path(args.rlog, output=args.output, fps=args.fps)

    if args.cmd == "metrics":
        from pathlib import Path

        from .log import metrics
        data = metrics.load(args.rlog)
        title = args.title or Path(args.rlog).stem
        _safe_print(metrics.report(data["json"], data["imgs"], title=title))
        if args.derived:
            info = metrics.to_csv(data["json"], args.derived)
            print("[metrics] 逐帧数据已导出：%s（%d 行）" % (info["file"], info["rows"]))
        return 0

    if args.cmd == "csv":
        from pathlib import Path

        from .log import metrics
        data = metrics.load(args.rlog)
        out = args.output or str(Path(args.rlog).with_suffix(".csv"))
        info = metrics.to_csv(data["json"], out)
        print("[csv] %s：%d 行 / %d 列" % (info["file"], info["rows"], len(info["cols"])))
        if info["skipped"]:
            print("[csv] 已跳过嵌套字段：%s" % ", ".join(info["skipped"]))
        return 0

    _parser().print_help()
    return 0
