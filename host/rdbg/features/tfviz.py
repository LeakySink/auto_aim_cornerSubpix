"""TF Viz feature — receive RemoteLogger plot.tf and visualize camera vs world."""

from __future__ import annotations

from .fleet_bound import FleetBoundFeature


class TfVizFeature(FleetBoundFeature):
    id = "tfviz"
    title = "TF Viz"
    description = "可视化相机相对世界系位姿（接收 tf_pub_test 的 plot.tf）"

    allow_rebind = True
    use_source_push_state = True

    def attach(self, shell):
        self.attach_fleet_routes(shell, select=True, img_subscribe=False)
