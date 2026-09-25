"""Shared calibrate control-plane helpers (hub feature + legacy standalone app)."""

from __future__ import annotations

import threading
import time

ALLOWED_CMDS = {"add", "calibrate", "drop", "reset", "quit", "done", "set_exposure"}


def parse_calib_request(handler, json_body_fn):
    """Return (cmd, body_dict) from POST JSON or ?cmd= / ?exposure_ms=."""
    body = {}
    if getattr(handler, "command", "").upper() == "POST":
        body = json_body_fn(handler) or {}
        if not isinstance(body, dict):
            body = {}
    cmd = (body.get("cmd") or "").strip()
    if not cmd:
        cmd = (handler.query.get("cmd") or [""])[0].strip()
    return cmd, body


def parse_calib_cmd(handler, json_body_fn):
    """Read cmd from POST JSON body or ?cmd= query. Empty if missing."""
    cmd, _ = parse_calib_request(handler, json_body_fn)
    return cmd


def calib_payload(cmd, body=None):
    """Build application JSON for RobotClient.send_json."""
    body = body or {}
    payload = {"cmd": cmd}
    if cmd == "calibrate":
        payload["host_time"] = time.strftime("%Y-%m-%d %H:%M:%S")
    if cmd == "set_exposure":
        ms = body.get("exposure_ms")
        if ms is None:
            ms = body.get("exposure")
        try:
            payload["exposure_ms"] = float(ms)
        except (TypeError, ValueError):
            payload["exposure_ms"] = 10.0
    return payload


def quit_robot_burst(client, ip, control, times=4, gap_s=0.03):
    """Fire-and-forget UDP quit bursts (daemon thread)."""

    def _burst():
        for _ in range(times):
            try:
                client.send_json(ip, control, {"cmd": "quit"})
            except Exception:
                pass
            time.sleep(gap_s)

    threading.Thread(target=_burst, name="calib-quit", daemon=True).start()
