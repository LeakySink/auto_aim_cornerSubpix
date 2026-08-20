"""Single-robot debugger — live source on the HTTP shell."""

import sys
import time

from ..httputil import open_browser
from ..shell import Shell
from ..sources.live import LiveSource


def run(http_port, control_port, no_browser=False):
    source = LiveSource(control_port)
    shell = Shell(name="debugger")
    source.attach(shell)
    source.start()

    httpd, bound = shell.bind("0.0.0.0", http_port, tries=1)
    if httpd is None:
        print(
            f"[debugger] cannot bind port {http_port}: {shell.last_bind_error}",
            file=sys.stderr,
        )
        source.stop()
        return 1

    url = f"http://localhost:{bound}"
    print(f"[debugger] {url}", file=sys.stderr)
    print(f"[debugger] control port {control_port}", file=sys.stderr)
    if not no_browser:
        time.sleep(0.15)
        open_browser(url)
    return shell.serve(httpd, threaded=False, on_stop=source.stop)
