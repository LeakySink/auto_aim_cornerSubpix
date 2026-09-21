import { useCallback, useEffect, useRef, useState } from "react";
import { startFeature, stopFeature, featureStatus } from "../../shared/api";
import { useSSE } from "../../shared/useSSE";
import { DebugWorkbench, type DataBus } from "../../shared/DebugWorkbench";

function handleEvent(bus: DataBus | null, msg: Record<string, unknown>, t0Ref: { current: number | null }) {
  if (!bus) return;
  const type = msg.type as string | undefined;
  if (type === "plot" || (!type && msg.data && typeof msg.data === "object")) {
    const data = (msg.data || msg) as Record<string, unknown>;
    const ts = Number(msg.ts || data.ts || 0);
    if (!t0Ref.current && ts) t0Ref.current = ts;
    const t = t0Ref.current ? (ts - t0Ref.current) / 1e9 : 0;
    for (const [k, v] of Object.entries(data)) {
      if (k === "ts" || k === "_from" || k === "type" || k === "hb") continue;
      if (typeof v === "number") bus.addPoint(k, t, v);
    }
    return;
  }
  if (type === "log") {
    bus.addLog(String(msg.level || "INFO"), String(msg.msg || ""), Number(msg.ts) || undefined);
    return;
  }
  if (type === "img_streams") {
    const list = (msg.streams as string[]) || [];
    bus.setStreams(list.filter((s) => typeof s === "string"));
    return;
  }
  if (type === "image") {
    const meta = (msg.meta || {}) as Record<string, unknown>;
    const name = String(meta.name || "image");
    const b64 = String(msg.jpg_b64 || "");
    if (!b64) return;
    const bin = atob(b64);
    const arr = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) arr[i] = bin.charCodeAt(i);
    const url = URL.createObjectURL(new Blob([arr], { type: "image/jpeg" }));
    bus.setImage(name, url);
  }
}

export function WatchPage() {
  const [state, setState] = useState("idle");
  const [err, setErr] = useState("");
  const [senders, setSenders] = useState<string[]>([]);
  const [selected, setSelected] = useState("");
  const busRef = useRef<DataBus | null>(null);
  const t0Ref = useRef<number | null>(null);
  const [running, setRunning] = useState(false);

  const refresh = useCallback(async () => {
    try {
      const st = await featureStatus("watch");
      setState(st.state);
      setErr(st.error || "");
      setRunning(st.state === "running");
    } catch (e) {
      setErr(String(e));
    }
  }, []);

  useEffect(() => {
    refresh();
    const t = setInterval(refresh, 2000);
    return () => clearInterval(t);
  }, [refresh]);

  useEffect(() => {
    if (state === "idle" || state === "error") {
      startFeature("watch", {}).then(refresh).catch((e) => setErr(String(e)));
    }
  }, []);

  const onBus = useCallback((b: DataBus) => {
    busRef.current = b;
  }, []);

  const imageSubscribe = useCallback((names: string[]) => {
    const q = names.map(encodeURIComponent).join(",");
    fetch(`/api/watch/img_subscribe?streams=${q}`).catch(() => {});
  }, []);

  useSSE(
    running ? "/api/watch/events" : null,
    (msg) => {
      if (msg.type === "state") {
        const list = (msg.senders as string[]) || [];
        setSenders((prev) =>
          prev.length === list.length && prev.every((s, i) => s === list[i]) ? prev : list
        );
        if (msg.active_sender) {
          const next = String(msg.active_sender);
          setSelected((prev) => (prev === next ? prev : next));
        }
        return;
      }
      handleEvent(busRef.current, msg, t0Ref);
    },
    running
  );

  const onSelect = async (name: string) => {
    setSelected(name);
    await fetch(`/api/watch/select?sender=${encodeURIComponent(name)}`);
  };

  return (
    <div className="feature-page">
      <div className="feature-toolbar">
        <strong>Watch</strong>
        <span className="mono">{state}</span>
        {err && <span style={{ color: "var(--err)" }}>{err}</span>}
        <select value={selected} onChange={(e) => onSelect(e.target.value)}>
          {senders.length === 0 && <option value="">(no robot)</option>}
          {senders.map((s) => (
            <option key={s} value={s}>
              {s}
            </option>
          ))}
        </select>
        <button type="button" className="ghost" onClick={() => busRef.current?.clear()}>
          清除
        </button>
        <button type="button" className="ghost" onClick={() => busRef.current?.resetView()}>
          重置
        </button>
        <button
          type="button"
          className="ghost"
          onClick={async () => {
            await stopFeature("watch");
            await refresh();
          }}
        >
          停止
        </button>
        <button
          type="button"
          onClick={async () => {
            await startFeature("watch", {});
            await refresh();
          }}
        >
          启动
        </button>
      </div>
      <div className="feature-body fill">
        <DebugWorkbench busOut={onBus} imageSubscribe={imageSubscribe} />
      </div>
    </div>
  );
}
