"""Small shared helpers for host features / live source."""

from __future__ import annotations

import json


def parse_stream_list(raw):
    """Split comma-separated image stream names; preserve order, drop empties."""
    streams = []
    seen = set()
    for part in (raw or "").split(","):
        name = part.strip()
        if name and name not in seen:
            seen.add(name)
            streams.append(name)
    return streams


def sse_state(active_sender, senders):
    """Serialize the standard SSE state event."""
    return json.dumps({
        "type": "state",
        "active_sender": active_sender or "",
        "senders": list(senders or []),
    })
