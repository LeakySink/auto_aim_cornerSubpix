"""TF Viz feature — receive plot.tf; optionally spawn local tf_pub_test.

Fleet routes only（host/API.md §3 TF Viz）。spawn 配置见 resolve_tf_pub_config。
"""

from __future__ import annotations

import os
import subprocess
import time
from pathlib import Path

from ..sources.fleet import fleet
from .fleet_bound import FleetBoundFeature

# host/rdbg/features/tfviz.py → repo root (features → rdbg → host → repo)
REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_CONFIG = REPO_ROOT / "configs" / "tf_pub.yaml"
BINARY = REPO_ROOT / "build" / "tf_pub_test"


def resolve_tf_pub_config(config: dict) -> Path:
    raw = (config.get("config_path") or "").strip()
    if not raw:
        raw = (os.environ.get("RDBG_TF_PUB_CONFIG") or "").strip()
    if raw:
        p = Path(raw).expanduser()
        if not p.is_absolute():
            p = REPO_ROOT / p
        return p.resolve()
    return DEFAULT_CONFIG.resolve()


class TfVizFeature(FleetBoundFeature):
    id = "tfviz"
    title = "TF Viz"
    description = "可视化相机相对世界系位姿（接收 tf_pub_test 的 plot.tf）"

    allow_rebind = True
    use_source_push_state = False

    def __init__(self):
        super().__init__()
        self._child = None
        self._spawned = False
        self._want_spawn = False
        self._config_path = ""

    def attach(self, shell):
        self.attach_fleet_routes(shell, select=True, img_subscribe=False)

    def status(self):
        st = super().status()
        st["spawned"] = self._spawned
        st["config_path"] = self._config_path
        if self._child is not None:
            st["child_alive"] = self._child.poll() is None
        return st

    def on_start(self, config):
        self._want_spawn = bool(config.get("spawn"))
        sender = (config.get("sender") or config.get("robot") or "").strip()
        if sender:
            self._bind(sender)
            return

        prefer = self._find_tfviz_sender()
        if prefer:
            self._bind(prefer)
            return

        if self._want_spawn:
            self._spawn_local(config)

    def on_stop(self):
        self._kill_child()
        super().on_stop()

    def list_senders(self):
        return [row["name"] for row in fleet.snapshot()]

    def after_bind(self):
        self.push_page_state()

    def on_tick(self):
        if self._spawned and self._child is not None and self._child.poll() is not None:
            code = self._child.returncode
            self._child = None
            with self._lock:
                self._error = "tf_pub_test exited (code=%s)" % code
                self._state = "error"
            return

        rows = fleet.snapshot()
        names = [row["name"] for row in rows]
        if not self._sender and rows:
            prefer = next(
                (
                    row["name"]
                    for row in rows
                    if row.get("feature") == "tfviz" or row.get("app") == "tfviz"
                ),
                None,
            )
            if prefer:
                try:
                    self._bind(prefer)
                except Exception:
                    pass
        self.push_page_state(names)

    def _find_tfviz_sender(self):
        for row in fleet.snapshot():
            if row.get("feature") == "tfviz" or row.get("app") == "tfviz":
                return row["name"]
        return ""

    def _spawn_local(self, config):
        cfg = resolve_tf_pub_config(config)
        self._config_path = str(cfg)
        if not cfg.is_file():
            raise RuntimeError("tf_pub config missing: %s" % cfg)
        if not BINARY.is_file():
            raise RuntimeError("tf_pub_test not built: %s (build the target first)" % BINARY)

        self._child = subprocess.Popen(
            [str(BINARY), str(cfg)],
            cwd=str(REPO_ROOT),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        self._spawned = True
        time.sleep(0.4)
        if self._child.poll() is not None:
            code = self._child.returncode
            self._child = None
            self._spawned = False
            raise RuntimeError(
                "tf_pub_test exited immediately (code=%s), config=%s" % (code, cfg)
            )

    def _kill_child(self):
        if not self._spawned or self._child is None:
            self._child = None
            self._spawned = False
            return
        proc = self._child
        self._child = None
        self._spawned = False
        if proc.poll() is not None:
            return
        try:
            proc.terminate()
            proc.wait(timeout=2.0)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass
