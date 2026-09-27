"""Watch feature — one thread per page, one UDP port per robot."""

from __future__ import annotations

from .fleet_bound import FleetBoundFeature


class WatchFeature(FleetBoundFeature):
    id = "watch"
    title = "Watch"
    description = "实时接收车上 RemoteLogger：曲线、日志、图像"

    allow_rebind = False
    use_source_push_state = True

    def attach(self, shell):
        self.attach_fleet_routes(shell, select=True, img_subscribe=True)
