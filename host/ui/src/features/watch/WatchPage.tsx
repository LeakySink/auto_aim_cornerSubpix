import { useCallback, useEffect, useRef, useState } from "react";
import { featureStatus, getJson, postJson, startFeature, stopFeature } from "../../shared/api";
import { useSSE } from "../../shared/useSSE";
import { useInstance } from "../../shared/instance";
import { DebugWorkbench, type DataBus } from "../../shared/DebugWorkbench";
import { markersFromFlatEkf, parseMarkerArray } from "./markerTypes";

const NESTED_SKIP = new Set(["markers", "frames", "tf", "ts", "_from", "type", "hb"]);

function handleEvent(bus: DataBus | null, msg: Record<string, unknown>, t0Ref: { current: number | null }) {
  if (!bus) return;
  const type = msg.type as string | undefined;
  if (type === "plot" || (!type && msg.data && typeof msg.data === "object")) {
    const data = (msg.data || msg) as Record<string, unknown>;
    const ts = Number(msg.ts || data.ts || 0);
    if (!t0Ref.current && ts) t0Ref.current = ts;
    const t = t0Ref.current ? (ts - t0Ref.current) / 1e9 : 0;

    if (data.markers_reset === true) bus.resetMarkers();

    const parsed = parseMarkerArray(data.markers);
    if (parsed) {
      bus.setMarkers(parsed);
    } else if (!data.markers) {
      const fallback = markersFromFlatEkf(data);
      if (fallback) bus.setMarkers(fallback);
    }

    if (data.frames) bus.setFrames(data.frames);
    if (data.tf) bus.setTf(data.tf);

    for (const [k, v] of Object.entries(data)) {
      if (NESTED_SKIP.has(k) || k === "markers_reset") continue;
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
  const inst = useInstance();
  const [state, setState] = useState("idle");
  const [err, setErr] = useState("");
  const [selected, setSelected] = useState("");
  const [port, setPort] = useState(0);
  const [robots, setRobots] = useState<{ name: string; ip: string; data_port: number }[]>([]);
  const [manual, setManual] = useState("");
  const busRef = useRef<DataBus | null>(null);
  const t0Ref = useRef<number | null>(null);
  const [running, setRunning] = useState(false);

  const refresh = useCallback(async () => {
    try {
      const st = await featureStatus(inst.id);
      setState(st.state);
      setErr(st.error || "");
      setRunning(st.state === "running" && !!st.sender);
      setSelected(st.sender || "");
      setPort(st.data_port || 0);
    } catch (e) {
      setErr(String(e));
    }
  }, [inst.id]);

  useEffect(() => {
    refresh();
    const t = setInterval(refresh, 2000);
    return () => clearInterval(t);
  }, [inst.id]);

  const onBus = useCallback((b: DataBus) => {
    busRef.current = b;
  }, []);

  const imageSubscribe = useCallback((names: string[]) => {
    const q = names.map(encodeURIComponent).join(",");
    fetch(`${inst.base}/img_subscribe?streams=${q}`).catch(() => {});
  }, [inst.base]);

  useSSE(
    running ? `${inst.base}/events` : null,
    (msg) => {
      if (msg.type === "state") {
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

  useEffect(() => {
    if (selected) return;
    let dead = false;
    const tick = async () => {
      try {
        const r = await getJson<{ robots: { name: string; ip: string; data_port: number }[] }>("/api/robots");
        if (!dead) setRobots(r.robots || []);
      } catch {
        /* ignore */
      }
    };
    tick();
    const t = setInterval(tick, 1000);
    return () => {
      dead = true;
      clearInterval(t);
    };
  }, [selected]);

  const bind = async (name: string) => {
    setErr("");
    try {
      await postJson(`${inst.base}/bind`, { sender: name });
      await refresh();
    } catch (e) {
      setErr(String(e));
    }
  };

  return (
    <div className="feature-page">
      <div className="feature-toolbar">
        <strong>Watch</strong>
        <span className="mono">{selected || "未选择车辆"}{port ? ` · UDP ${port}` : ""} · {state}</span>
        {err && <span className="err-text">{err}</span>}
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
            await stopFeature(inst.id);
            await refresh();
          }}
        >
          停止
        </button>
        <button
          type="button"
          onClick={async () => {
            await startFeature(inst.id, { sender: selected });
            await refresh();
          }}
        >
          启动
        </button>
      </div>
      <div className="feature-body fill">
        {!selected && (
          <div className="pick-pane">
            <div className="sec-label">选择车辆</div>
            <p className="sub">选中后才会占用一个空闲 UDP 口，并只接收这辆车的数据。</p>
            {robots.length > 0 && (
              <div className="robot-list">
                {robots.map((r) => (
                  <button key={r.name} type="button" className="robot" onClick={() => bind(r.name)}>
                    <span className="robot-name">{r.name}</span>
                    <span className="robot-meta mono">
                      {r.ip}
                      {r.data_port ? ` · 已在 UDP ${r.data_port}` : " · 将分配新端口"}
                    </span>
                  </button>
                ))}
              </div>
            )}
            {robots.length === 0 && <p className="empty">还没有发现车辆。</p>}
            <div className="form-row">
              <input value={manual} placeholder="车名 sender_name" onChange={(e) => setManual(e.target.value)} />
              <button type="button" disabled={!manual.trim()} onClick={() => bind(manual.trim())}>
                查看这辆车
              </button>
            </div>
          </div>
        )}
        <DebugWorkbench busOut={onBus} imageSubscribe={imageSubscribe} />
      </div>
    </div>
  );
}
