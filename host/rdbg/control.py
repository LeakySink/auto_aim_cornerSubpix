"""
UDP Control Server — handles sender registration/deregistration protocol.

Protocol:
  Register:   {"type":"register","name":"robot"}
  Ack ok:     {"type":"register_ack","status":"ok","port":15001}
  Ack err:    {"type":"register_ack","status":"error","message":"..."}
  Deregister: {"type":"deregister","name":"robot"}
  Dereg ack:  {"type":"deregister_ack","status":"ok"}
  Host down:  {"type":"host_shutdown"}
"""

import json
import socket
import threading
import time


class ControlServer:
    def __init__(self, host="0.0.0.0", port=15000, data_port_start=15001, data_port_end=15099, reuse_ports=False):
        self.host = host
        self.port = port
        self.data_port_start = data_port_start
        self.data_port_end = data_port_end
        self.reuse_ports = reuse_ports
        self.senders = {}
        self.used_ports = set()
        self._lock = threading.Lock()
        self._running = False
        self._sock = None
        self._thread = None
        self._version = 0    # incremented on every register/deregister

    def version(self):
        with self._lock:
            return self._version

    def start(self):
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind((self.host, self.port))
        self._running = True
        self._thread = threading.Thread(target=self._worker, daemon=True)
        self._thread.start()
        print(f"[control] listening on UDP {self.host}:{self.port}")

    def stop(self):
        if not self._running:
            return
        self._running = False
        with self._lock:
            for name, info in list(self.senders.items()):
                self._send(info["addr"], {"type": "host_shutdown"})
                self.senders.pop(name, None)
                self._release_port(info["data_port"])
        time.sleep(0.2)
        if self._sock:
            self._sock.close()
            self._sock = None
        if self._thread:
            self._thread.join(timeout=2)
        print("[control] stopped")

    def get_senders(self):
        with self._lock:
            return list(self.senders.keys())

    def get_sender_info(self, name):
        with self._lock:
            return self.senders.get(name)

    def _next_port(self):
        if self.reuse_ports:
            return self.data_port_start
        for p in range(self.data_port_start, self.data_port_end + 1):
            if p not in self.used_ports:
                self.used_ports.add(p)
                return p
        return 0

    def _release_port(self, p):
        if not self.reuse_ports:
            self.used_ports.discard(p)

    def _send(self, addr, obj):
        try:
            data = json.dumps(obj).encode()
            self._sock.sendto(data, addr)
        except Exception as e:
            print(f"[control] send error to {addr}: {e}")

    def _worker(self):
        buf_size = 65536
        while self._running:
            try:
                self._sock.settimeout(1.0)
                data, addr = self._sock.recvfrom(buf_size)
            except socket.timeout:
                continue
            except Exception:
                break

            try:
                msg = json.loads(data.decode())
            except Exception:
                continue

            t = msg.get("type", "")
            name = msg.get("name", "").strip()

            if t == "register" and name:
                with self._lock:
                    if name in self.senders:
                        port = 0
                    else:
                        port = self._next_port()
                    if port == 0:
                        err = (f"name '{name}' already registered" if name in self.senders
                               else "no available data port")
                        ack = {"type": "register_ack", "status": "error", "message": err}
                    else:
                        self.senders[name] = {"addr": addr, "data_port": port}
                        self._version += 1
                        ack = {"type": "register_ack", "status": "ok", "port": port}
                if ack.get("status") == "ok":
                    print(f"[control] registered '{name}' -> {addr[0]}:{addr[1]}, data port {ack['port']}")
                self._send(addr, ack)

            elif t == "deregister" and name:
                with self._lock:
                    if name not in self.senders:
                        continue
                    info = self.senders.pop(name)
                    self._release_port(info["data_port"])
                    self._version += 1
                print(f"[control] deregistered '{name}'")
                self._send(addr, {"type": "deregister_ack", "status": "ok"})

