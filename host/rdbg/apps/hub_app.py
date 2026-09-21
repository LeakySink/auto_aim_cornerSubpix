"""Unified Hub app — one HTTP port, per-page feature threads, SPA."""

from __future__ import annotations

import sys
import time

from ..features.registry import KINDS, FeatureRegistry
from ..http.httputil import STATIC_UI_DIR, open_browser
from ..http.shell import Shell
from ..sources.fleet import fleet


def run(host="0.0.0.0", port=8080, no_browser=False, open_path="/"):
    registry = FeatureRegistry()
    shell = Shell(name="hub")
    registry.attach(shell)
    fleet.attach(shell)
    fleet.start()

    if not (STATIC_UI_DIR / "index.html").is_file():
        print(
            f"[hub] SPA not built: missing {STATIC_UI_DIR}/index.html\n"
            f"      cd host/ui && npm install && npm run build",
            file=sys.stderr,
        )

    httpd, bound = shell.bind(host, port, tries=20)
    if httpd is None:
        print(f"[hub] cannot bind {host}:{port}: {shell.last_bind_error}", file=sys.stderr)
        return 1

    url = f"http://127.0.0.1:{bound}{open_path}"
    print(f"[hub] {url}", file=sys.stderr)
    print(f"[hub] features: {', '.join(KINDS)}", file=sys.stderr)
    if not no_browser:
        time.sleep(0.15)
        open_browser(url)

    def _stop():
        registry.stop_all()
        fleet.stop()

    return shell.serve(httpd, threaded=False, on_stop=_stop)
