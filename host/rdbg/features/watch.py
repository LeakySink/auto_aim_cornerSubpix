"""Watch feature — one thread per page, one UDP port per robot.

Routes: fleet events/bind/state/select + img_subscribe（host/API.md §3）。
"""

from __future__ import annotations

from .fleet_bound import FleetBoundFeature


class WatchFeature(FleetBoundFeature):
    id = "watch"
    title = "Watch"
    description = "实时接收车上 RemoteLogger：曲线、日志、图像"

    allow_rebind = False
    use_source_push_state = True

    def attach(self, shell):
        # select：查询当前绑车；img_subscribe：Watch 独有图像话题订阅
        self.attach_fleet_routes(shell, select=True, img_subscribe=True)
