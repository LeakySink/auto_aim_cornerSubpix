"""Built-in features package."""

from .dump import DumpFeature
from .netcheck import NetcheckFeature
from .replay import ReplayFeature
from .watch import WatchFeature


def builtin_features():
    return [
        WatchFeature(),
        ReplayFeature(),
        DumpFeature(),
        NetcheckFeature(),
    ]
