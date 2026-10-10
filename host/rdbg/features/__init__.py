"""Built-in features package."""

from .calibrate import CalibrateFeature
from .dump import DumpFeature
from .fleet_bound import FleetBoundFeature
from .netcheck import NetcheckFeature
from .replay import ReplayFeature
from .watch import WatchFeature


def builtin_features():
    return [
        WatchFeature(),
        CalibrateFeature(),
        ReplayFeature(),
        DumpFeature(),
        NetcheckFeature(),
    ]


__all__ = [
    "CalibrateFeature",
    "DumpFeature",
    "FleetBoundFeature",
    "NetcheckFeature",
    "ReplayFeature",
    "WatchFeature",
    "builtin_features",
]
