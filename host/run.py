#!/usr/bin/env python3
"""Pick the platform launcher: *.bat on Windows, *.sh elsewhere.

  python3 host/run.py              # watch
  python3 host/run.py watch --no-browser
  python3 host/run.py replay logs/run.rlog
"""

import os
import subprocess
import sys
from pathlib import Path

HOST = Path(__file__).resolve().parent


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0].startswith("-"):
        name, rest = "watch", argv
    elif argv[0] in ("watch", "replay", "calibrate"):
        name, rest = argv[0], argv[1:]
    else:
        print("Usage: run.py [watch|replay|calibrate] [args...]", file=sys.stderr)
        return 2

    win = sys.platform == "win32"
    script = HOST / (name + (".bat" if win else ".sh"))
    if not script.is_file():
        print("[run] missing " + str(script), file=sys.stderr)
        return 1

    if win:
        cmd = ["cmd.exe", "/c", str(script)] + rest
    else:
        bash = "/bin/bash" if os.path.isfile("/bin/bash") else "bash"
        cmd = [bash, str(script)] + rest
    return subprocess.call(cmd)


if __name__ == "__main__":
    raise SystemExit(main())
