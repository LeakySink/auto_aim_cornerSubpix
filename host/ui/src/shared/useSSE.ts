import { useEffect, useRef } from "react";

/** Subscribe to an SSE endpoint; calls onEvent for each JSON message. */
export function useSSE(
  url: string | null,
  onEvent: (data: Record<string, unknown>) => void,
  enabled = true
) {
  const cb = useRef(onEvent);
  cb.current = onEvent;

  useEffect(() => {
    if (!url || !enabled) return;
    const es = new EventSource(url);
    es.onmessage = (ev) => {
      try {
        const data = JSON.parse(ev.data);
        if (data && typeof data === "object") cb.current(data);
      } catch {
        /* ignore */
      }
    };
    return () => es.close();
  }, [url, enabled]);
}
