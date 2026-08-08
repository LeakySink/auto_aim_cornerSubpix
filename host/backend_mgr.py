"""Backend process manager — manages udp_backend lifecycle."""
import subprocess
import threading


class BackendManager:
    def __init__(self, binary_path):
        self._binary = binary_path
        self._proc = None
        self._reader = None
        self._lock = threading.Lock()
        self._sender = ""        # currently active sender name
        self._port = 0
        self.on_output = None    # callback(line) for each stdout line
        self.on_state = None     # callback() when state changes

    @property
    def active_sender(self):
        return self._sender

    @property
    def active_port(self):
        return self._port

    @property
    def running(self):
        return self._proc is not None and self._proc.poll() is None

    def start(self, port, sender_name="default"):
        """Start backend on given port for given sender."""
        with self._lock:
            self._stop_locked()
            self._sender = sender_name
            self._port = port
            self._proc = subprocess.Popen(
                [self._binary, "--port", str(port)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, bufsize=1,
            )
            self._reader = threading.Thread(
                target=self._read_stdout, daemon=True
            )
            self._reader.start()
            # Log stderr
            threading.Thread(target=self._read_stderr, daemon=True).start()
        if self.on_state:
            self.on_state()

    def stop(self):
        with self._lock:
            self._stop_locked()
        if self.on_state:
            self.on_state()

    def switch(self, sender_name, port):
        """Stop current backend and start new one for sender."""
        with self._lock:
            if self._sender == sender_name and self._port == port and self.running:
                return
            self._stop_locked()
        self.start(port, sender_name)

    def _stop_locked(self):
        if self._proc:
            try:
                self._proc.terminate()
                self._proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self._proc.kill()
                self._proc.wait(timeout=2)
            except Exception:
                pass
            self._proc = None
        self._sender = ""
        self._port = 0

    def _read_stdout(self):
        """Read lines from backend stdout and call on_output."""
        try:
            for line in self._proc.stdout:
                line = line.strip()
                if line and self.on_output:
                    self.on_output(line)
        except Exception:
            pass
        if self.on_state:
            self.on_state()

    def _read_stderr(self):
        """Read and print backend stderr."""
        try:
            for line in self._proc.stderr:
                print(f"[backend] {line.rstrip()}")
        except Exception:
            pass
