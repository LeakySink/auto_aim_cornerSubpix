"""Dump feature — export .rlog to folder (async job).

Routes: POST /run → job_id；GET /jobs/<id>（host/API.md §5）。
"""

from __future__ import annotations

import threading
import uuid

from ..log.dump import dump_rlog
from .base import Feature, json_body, send_json


class DumpFeature(Feature):
    id = "dump"
    title = "Dump"
    description = "把 .rlog 导出为 log.txt / plot.txt / images.mp4"

    def __init__(self):
        super().__init__()
        self._jobs = {}
        self._jobs_lock = threading.Lock()

    def attach(self, shell):
        p = getattr(self, "api_prefix", "/api/dump")
        shell.route(p + "/run", self._handle_run, methods=("POST",))
        shell.route(p + "/jobs/", self._handle_job, prefix=True)

    def on_start(self, config):
        pass

    def run(self, stop_event: threading.Event):
        while not stop_event.wait(0.5):
            pass

    def _handle_run(self, handler):
        body = json_body(handler)
        path = body.get("path") or body.get("rlog") or ""
        out = body.get("output") or body.get("out_dir") or None
        fps = float(body.get("fps") or 10.0)
        if not path:
            send_json(handler, {"ok": False, "error": "path required"}, code=400)
            return
        job_id = uuid.uuid4().hex[:12]
        with self._jobs_lock:
            self._jobs[job_id] = {
                "id": job_id,
                "state": "running",
                "path": path,
                "output": out,
                "error": "",
                "result": None,
            }

        def _work():
            try:
                from pathlib import Path
                src = Path(path).expanduser()
                if out:
                    out_dir = Path(out).expanduser()
                else:
                    out_dir = src.with_name(src.stem + "_dump")
                info = dump_rlog(src, out_dir, fps=fps)
                with self._jobs_lock:
                    self._jobs[job_id]["state"] = "done"
                    self._jobs[job_id]["result"] = info
                    self._jobs[job_id]["output"] = str(out_dir)
            except Exception as e:
                with self._jobs_lock:
                    self._jobs[job_id]["state"] = "error"
                    self._jobs[job_id]["error"] = str(e)

        threading.Thread(target=_work, name=f"dump-{job_id}", daemon=True).start()
        # ensure feature marked running for status UI
        if self.status().get("state") != "running":
            self.start({})
        send_json(handler, {"ok": True, "job_id": job_id})

    def _handle_job(self, handler):
        parts = handler.route_path.strip("/").split("/")
        if len(parts) < 2 or parts[-2] != "jobs":
            handler.send_error(404)
            return
        job_id = parts[-1]
        with self._jobs_lock:
            job = self._jobs.get(job_id)
        if not job:
            send_json(handler, {"error": "unknown job"}, code=404)
            return
        send_json(handler, job)
