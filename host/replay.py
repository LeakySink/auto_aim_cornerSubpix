#!/usr/bin/env python3
"""Local .rlog player. Same file on Windows / macOS / Linux.

  python3 host/replay.py logs/run.rlog
  python host\\replay.py logs\\run.rlog
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from rdbg.launch import replay

if __name__ == "__main__":
    raise SystemExit(replay())
