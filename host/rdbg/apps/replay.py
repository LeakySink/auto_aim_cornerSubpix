"""Local .rlog player — replay source on the HTTP shell."""

import sys
import time
from pathlib import Path

from ..http.httputil import open_browser
from ..http.shell import Shell
from ..sources.replay import ReplaySource, load_or_exit

PORT_TRY = 20


def run(rlog, host="127.0.0.1", port=8765, max_mb=1024, no_browser=False):
    path = Path(rlog).expanduser()
    if not path.is_file():
        print(f"[replay] file not found: {rlog}", file=sys.stderr)
        return 1

    session = load_or_exit(str(path), max_mb=max_mb)
    source = ReplaySource(session)
    shell = Shell(name="replay")
    source.attach(shell)
    source.start()

    httpd, bound = shell.bind(host, port, tries=PORT_TRY)
    if httpd is None:
        print(
            f"[replay] ports {port}-{port + PORT_TRY - 1} all busy",
            file=sys.stderr,
        )
        source.stop()
        return 1

    url = f"http://{host}:{bound}/"
    print(f"[replay] {url}", file=sys.stderr)
    if bound != port:
        print(
            f"[replay] 请打开上面的地址（不要用已占用的 :{port} 旧窗口）",
            file=sys.stderr,
        )

    if not no_browser:
        time.sleep(0.05)
        open_browser(url)
    return shell.serve(httpd, threaded=True, on_stop=source.stop)
