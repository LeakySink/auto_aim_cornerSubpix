"""Feature registry and control-plane HTTP routes."""

from __future__ import annotations

import json

from .base import Feature, json_body, send_json


class FeatureRegistry:
    def __init__(self):
        self._features = {}

    def register(self, feature: Feature):
        if not feature.id:
            raise ValueError("feature.id required")
        self._features[feature.id] = feature

    def get(self, fid):
        return self._features.get(fid)

    def list(self):
        return [f.meta() for f in self._features.values()]

    def all(self):
        return list(self._features.values())

    def attach(self, shell):
        for feat in self._features.values():
            feat.attach(shell)

        shell.route("/api/features", self._handle_list, methods=("GET",))
        shell.route("/api/features/", self._handle_feature, methods=("GET", "POST"), prefix=True)

    def stop_all(self):
        for feat in self._features.values():
            try:
                feat.stop()
            except Exception:
                pass

    def _handle_list(self, handler):
        send_json(handler, {"features": self.list()})

    def _handle_feature(self, handler):
        # /api/features/<id>/(start|stop|status)
        parts = handler.route_path.strip("/").split("/")
        # ["api", "features", "<id>", ...]
        if len(parts) < 3:
            handler.send_error(404)
            return
        fid = parts[2]
        action = parts[3] if len(parts) > 3 else "status"
        feat = self.get(fid)
        if feat is None:
            send_json(handler, {"error": "unknown feature", "id": fid}, code=404)
            return

        method = handler.command.upper()
        if action == "status" and method == "GET":
            send_json(handler, feat.status())
            return
        if action == "start" and method == "POST":
            cfg = json_body(handler)
            feat.start(cfg)
            send_json(handler, feat.status())
            return
        if action == "stop" and method == "POST":
            feat.stop()
            send_json(handler, feat.status())
            return
        handler.send_error(404)
