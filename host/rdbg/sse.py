"""Thread-safe SSE message queue and HTTP writer."""

import queue


class SSEQueue:
    def __init__(self, maxsize=4096):
        self._q = queue.Queue(maxsize=maxsize)

    def put(self, line):
        try:
            self._q.put_nowait(line)
        except queue.Full:
            try:
                self._q.get_nowait()
            except queue.Empty:
                pass
            self._q.put_nowait(line)

    def get(self, timeout=0.5):
        try:
            return self._q.get(timeout=timeout)
        except queue.Empty:
            return None


def write_sse(handler, sse_queue, on_connect=None):
    handler.send_response(200)
    handler.send_header("Content-Type", "text/event-stream")
    handler.send_header("Cache-Control", "no-cache")
    handler.send_header("Connection", "keep-alive")
    handler.send_header("Access-Control-Allow-Origin", "*")
    handler.end_headers()
    if on_connect:
        on_connect()
    try:
        while True:
            line = sse_queue.get(timeout=1.0)
            if line:
                handler.wfile.write(f"data: {line}\n\n".encode())
                handler.wfile.flush()
    except Exception:
        pass
