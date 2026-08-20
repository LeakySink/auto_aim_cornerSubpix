"""OS-agnostic process entry used by host/watch.py and host/replay.py."""

from .cli import expand_watch_env, main


def watch(argv=None):
    import sys
    argv = list(sys.argv[1:] if argv is None else argv)
    return main(["watch"] + expand_watch_env(argv))


def replay(argv=None):
    import sys
    argv = list(sys.argv[1:] if argv is None else argv)
    return main(["replay"] + argv)
