"""Feature kinds and per-page instances. Each open page is its own thread.

HTTP surface (see host/API.md):
  GET  /api/features
  POST /api/open
  GET  /api/instances
  GET|POST /api/instances/<id>/{status,start,stop}

Instance business routes are registered in Feature.attach under /api/i/<id>/….
Help is frontend-only and is NOT listed in KINDS.
"""

from __future__ import annotations

import uuid

from .base import Feature, json_body, send_json
from .calibrate import CalibrateFeature
from .dump import DumpFeature
from .netcheck import NetcheckFeature
from .replay import ReplayFeature
from .tfviz import TfVizFeature
from .watch import WatchFeature

# kind id → Feature subclass（与 UI FEATURE_MODULES 对齐，不含 help）
KINDS = {
    "watch": WatchFeature,
    "calibrate": CalibrateFeature,
    "replay": ReplayFeature,
    "dump": DumpFeature,
    "netcheck": NetcheckFeature,
    "tfviz": TfVizFeature,
}


class FeatureRegistry:
    """Owns running Feature instances for one Hub process."""

    def __init__(self):
        self._shell = None
        self._instances = {}

    def attach(self, shell):
        """Mount Hub-level control routes on the HTTP shell."""
        self._shell = shell
        shell.route("/api/features", self._handle_kinds, methods=("GET",))
        shell.route("/api/open", self._handle_open, methods=("POST",))
        shell.route("/api/instances", self._handle_list, methods=("GET",))
        shell.route("/api/instances/", self._handle_instance, methods=("GET", "POST"), prefix=True)

    def stop_all(self):
        for inst in list(self._instances.values()):
            try:
                inst.stop()
            except Exception:
                pass
        self._instances.clear()

    def open(self, kind, config=None):
        """Construct, attach routes, start thread; return the Feature."""
        cls = KINDS.get(kind)
        if cls is None:
            raise KeyError(kind)
        feat = cls()
        iid = uuid.uuid4().hex[:10]
        feat.instance_id = iid
        feat.api_prefix = f"/api/i/{iid}"
        if self._shell is not None:
            feat.attach(self._shell)
        feat.start(config or {})
        self._instances[iid] = feat
        return feat

    def _public(self, feat: Feature):
        """JSON-safe status blob for list/open/status responses."""
        st = feat.status()
        st["feature"] = feat.id
        st["instance"] = getattr(feat, "instance_id", "")
        st["title"] = feat.title
        st["path"] = feat.ui_path
        return st

    def _handle_kinds(self, handler):
        features = []
        for kind, cls in KINDS.items():
            proto = cls()
            features.append({
                "id": proto.id,
                "title": proto.title,
                "description": proto.description,
            })
        send_json(handler, {"features": features})

    def _handle_open(self, handler):
        body = json_body(handler)
        kind = body.get("feature") or body.get("id") or ""
        if kind not in KINDS:
            send_json(handler, {"ok": False, "error": "unknown feature"}, code=404)
            return
        feat = self.open(kind, body.get("config") or {})
        iid = feat.instance_id
        send_json(handler, {
            "ok": True,
            "id": iid,
            "feature": feat.id,
            "path": feat.ui_path,
            "status": self._public(feat),
        })

    def _handle_list(self, handler):
        send_json(handler, {
            "instances": [self._public(f) for f in self._instances.values()],
        })

    def _handle_instance(self, handler):
        # /api/instances/<id>/{status|start|stop}
        parts = handler.route_path.strip("/").split("/")
        # api instances <id> [start|stop|status]
        if len(parts) < 3:
            handler.send_error(404)
            return
        iid = parts[2]
        action = parts[3] if len(parts) > 3 else "status"
        feat = self._instances.get(iid)
        if feat is None:
            send_json(handler, {"error": "unknown instance", "id": iid}, code=404)
            return
        method = handler.command.upper()
        if action == "status" and method == "GET":
            send_json(handler, self._public(feat))
            return
        if action == "stop" and method == "POST":
            feat.stop()
            qs = handler.query.get("forget") if handler.query else None
            forget = qs[0] if isinstance(qs, list) and qs else qs
            if forget == "1":
                self._instances.pop(iid, None)
            send_json(handler, {"ok": True, "id": iid, "state": "idle"})
            return
        if action == "start" and method == "POST":
            feat.start(json_body(handler))
            send_json(handler, self._public(feat))
            return
        handler.send_error(404)
