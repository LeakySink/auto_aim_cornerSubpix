"""Feature plugin base — each feature runs in its own thread when started."""

from __future__ import annotations

import json
import threading
from abc import ABC, abstractmethod


def json_body(handler):
    raw = getattr(handler, "body", b"") or b""
    if not raw:
        return {}
    try:
        return json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return {}


def send_json(handler, obj, code=200):
    body = json.dumps(obj, ensure_ascii=False, separators=(",", ":"))
    handler.send(code, body, "application/json; charset=utf-8")


class Feature(ABC):
    id: str = ""
    title: str = ""
    description: str = ""

    def __init__(self):
        self._lock = threading.Lock()
        self._thread = None
        self._stop = threading.Event()
        self._state = "idle"  # idle | starting | running | stopping | error
        self._error = ""
        self._config = {}

    def meta(self):
        return {
            "id": self.id,
            "title": self.title,
            "description": self.description,
            "state": self.status().get("state", "idle"),
        }

    def status(self):
        with self._lock:
            return {
                "id": self.id,
                "state": self._state,
                "error": self._error,
                "config": dict(self._config),
            }

    def attach(self, shell):
        """Register /api/<id>/* business routes on the shell. Override in subclass."""

    def start(self, config=None):
        config = dict(config or {})
        with self._lock:
            if self._state == "running" and self._thread and self._thread.is_alive():
                self._config.update(config)
                return
            if self._state in ("starting", "stopping"):
                return
            self._config = config
            self._error = ""
            self._state = "starting"
            self._stop.clear()
            self._thread = threading.Thread(
                target=self._thread_main, name=f"feat-{self.id}", daemon=True)
            self._thread.start()

    def stop(self, timeout=5.0):
        with self._lock:
            if self._state in ("idle", "stopping"):
                self._state = "idle"
                return
            self._state = "stopping"
            self._stop.set()
            t = self._thread
        if t and t.is_alive():
            t.join(timeout=timeout)
        try:
            self.on_stop()
        except Exception as e:
            with self._lock:
                self._error = str(e)
        with self._lock:
            self._state = "idle"
            self._thread = None

    def _thread_main(self):
        try:
            self.on_start(dict(self._config))
            with self._lock:
                self._state = "running"
            self.run(self._stop)
        except Exception as e:
            with self._lock:
                self._state = "error"
                self._error = str(e)
        finally:
            try:
                self.on_stop()
            except Exception:
                pass
            with self._lock:
                if self._state != "error":
                    self._state = "idle"
                self._thread = None

    def on_start(self, config):
        """Called once in the feature thread before run()."""

    def on_stop(self):
        """Cleanup; may be called from stop() or after run() exits."""

    @abstractmethod
    def run(self, stop_event: threading.Event):
        """Feature main loop. Return when stop_event is set or work is done."""
