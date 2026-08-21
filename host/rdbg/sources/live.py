"""Live UDP source — control port + data port, push onto the shell SSE queue."""

import json
import threading
import time

from ..net.control import ControlServer
from ..net.udp import UdpBackend


class LiveSource:
    id = "live"

    def __init__(self, control_port):
        self.control = ControlServer(port=control_port)
        self.udp = UdpBackend()
        self.udp.on_state = self.push_state
        self.shell = None
        self._last_version = -1
        self._last_senders = []

    def attach(self, shell):
        self.shell = shell
        self.udp.on_output = lambda line: shell.sse.put(line)
        shell.page("/", "watch.html")
        shell.sse_route("/events", on_connect=self.push_state)
        shell.route("/select", self._handle_select)

    def start(self):
        self.control.start()
        threading.Thread(target=self.poller, daemon=True).start()
        self.push_state()

    def stop(self):
        self.udp.stop()
        self.control.stop()

    def push_state(self):
        if not self.shell:
            return
        state = {
            "type": "state",
            "active_sender": self.udp.active_sender,
            "senders": self.control.get_senders(),
        }
        self.shell.sse.put(json.dumps(state))

    def select(self, name):
        if not name:
            return
        info = self.control.get_sender_info(name)
        if info:
            self.udp.switch(name, info["data_port"])
        self.push_state()

    def _handle_select(self, handler):
        self.select(handler.query.get("sender", [""])[0])
        handler.send(200, b'{"ok":true}', "application/json")

    def poller(self):
        while self.control._running:
            time.sleep(0.5)
            try:
                version = self.control.version()
                senders = self.control.get_senders()
                if version != self._last_version or senders != self._last_senders:
                    self._last_version = version
                    self._last_senders = senders
                    self.push_state()
                if senders:
                    info = self.control.get_sender_info(senders[0])
                    if info and (not self.udp.running or
                                 self.udp.active_sender != senders[0] or
                                 self.udp.active_port != info["data_port"]):
                        self.udp.switch(senders[0], info["data_port"])
                elif self.udp.running:
                    self.udp.stop()
            except Exception:
                pass
