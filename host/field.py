#!/usr/bin/env python3
"""Compatibility launcher. Prefer: python3 -m host field"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from host.cli import main

if __name__ == "__main__":
    raise SystemExit(main(["field", *sys.argv[1:]]))
