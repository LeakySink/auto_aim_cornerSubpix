"""Shared calibrate control-plane helpers (hub feature + legacy standalone app)."""

from __future__ import annotations

import threading
import time

ALLOWED_CMDS = {"add", "calibrate", "drop", "reset", "quit", "done"}


def parse_calib_cmd(handler, json_body_fn):
    """Read cmd from POST JSON body or ?cmd= query. Empty if missing."""
    cmd = ""
    if getattr(handler, "command", "").upper() == "POST":
        body = json_body_fn(handler)
        cmd = (body.get("cmd") or "").strip()
    if not cmd:
        cmd = (handler.query.get("cmd") or [""])[0].strip()
    return cmd


def calib_payload(cmd):
    """Build application JSON for RobotClient.send_json."""
    payload = {"cmd": cmd}
    if cmd == "calibrate":
        payload["host_time"] = time.strftime("%Y-%m-%d %H:%M:%S")
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
