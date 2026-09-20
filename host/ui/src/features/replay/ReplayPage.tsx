import { useCallback, useEffect, useRef, useState } from "react";
import { postJson, startFeature, stopFeature } from "../../shared/api";
import { DebugWorkbench, type DataBus } from "../../shared/DebugWorkbench";

type Meta = {
  duration: number;
  series: Record<string, [number, number][]>;
  logs: { t: number; level: string; msg: string }[];
  frames: { i: number; t: number; meta: { name?: string } }[];
  file?: string;
};

export function ReplayPage() {
  const [path, setPath] = useState("");
  const [meta, setMeta] = useState<Meta | null>(null);
  const [err, setErr] = useState("");
  const [playing, setPlaying] = useState(false);
  const [t, setT] = useState(0);
  const [rate, setRate] = useState(1);
  const busRef = useRef<DataBus | null>(null);
  const metaRef = useRef<Meta | null>(null);
  const idxRef = useRef({ series: {} as Record<string, number>, log: 0, frame: 0 });
  const rafRef = useRef(0);
  const lastRef = useRef(0);

  useEffect(() => {
    startFeature("replay", {}).catch(() => {});
  }, []);

  const load = async () => {
    setErr("");
    try {
      await startFeature("replay", { path });
      const res = await postJson<{ ok: boolean; meta?: Meta; error?: string }>(
        "/api/replay/load",
        { path }
      );
      if (!res.ok) throw new Error(res.error || "load failed");
      setMeta(res.meta || null);
      metaRef.current = res.meta || null;
      idxRef.current = { series: {}, log: 0, frame: 0 };
      busRef.current?.clear();
      setT(0);
      setPlaying(false);
    } catch (e) {
      setErr(String(e));
    }
  };

  const applyUntil = useCallback(async (time: number) => {
    const m = metaRef.current;
    const bus = busRef.current;
    if (!m || !bus) return;
    for (const [field, pts] of Object.entries(m.series || {})) {
      let i = idxRef.current.series[field] || 0;
      while (i < pts.length && pts[i][0] <= time) {
        bus.addPoint(field, pts[i][0], pts[i][1]);
        i++;
      }
      idxRef.current.series[field] = i;
    }
    let li = idxRef.current.log;
    const logs = m.logs || [];
    while (li < logs.length && logs[li].t <= time) {
      bus.addLog(logs[li].level, logs[li].msg);
      li++;
    }
    idxRef.current.log = li;
    let fi = idxRef.current.frame;
    const frames = m.frames || [];
    while (fi < frames.length && frames[fi].t <= time) {
      const fr = frames[fi];
      try {
        const r = await fetch(`/api/replay/frame/${fr.i}`);
        if (r.ok) {
          const blob = await r.blob();
          const url = URL.createObjectURL(blob);
          bus.setImage(String(fr.meta?.name || "image"), url);
        }
      } catch {
        /* ignore */
      }
      fi++;
    }
    idxRef.current.frame = fi;
  }, []);

  useEffect(() => {
    if (!playing || !meta) return;
    lastRef.current = performance.now();
    const loop = (now: number) => {
      const dt = ((now - lastRef.current) / 1000) * rate;
      lastRef.current = now;
      setT((prev) => {
        const next = Math.min(meta.duration || 0, prev + dt);
        applyUntil(next);
        if (next >= (meta.duration || 0)) setPlaying(false);
        return next;
      });
      rafRef.current = requestAnimationFrame(loop);
    };
    rafRef.current = requestAnimationFrame(loop);
    return () => cancelAnimationFrame(rafRef.current);
  }, [playing, meta, rate, applyUntil]);

  const seek = async (nt: number) => {
    busRef.current?.clear();
    idxRef.current = { series: {}, log: 0, frame: 0 };
    setT(nt);
    await applyUntil(nt);
  };

  return (
    <div className="feature-page">
      <div className="feature-toolbar">
        <strong>Replay</strong>
        <input
          style={{ minWidth: "18rem", flex: 1 }}
          placeholder="/path/to/run.rlog"
          value={path}
          onChange={(e) => setPath(e.target.value)}
        />
        <button type="button" onClick={load}>
          加载
        </button>
        <button type="button" className="ghost" disabled={!meta} onClick={() => setPlaying((p) => !p)}>
          {playing ? "暂停" : "播放"}
        </button>
        <select value={rate} onChange={(e) => setRate(Number(e.target.value))}>
          {[0.25, 0.5, 1, 2, 4].map((r) => (
            <option key={r} value={r}>
              {r}x
            </option>
          ))}
        </select>
        <button type="button" className="ghost" onClick={() => stopFeature("replay")}>
          停止线程
        </button>
        {err && <span style={{ color: "var(--err)" }}>{err}</span>}
        {meta && <span className="mono">{meta.file}</span>}
      </div>
      {meta && (
        <div className="feature-toolbar">
          <input
            type="range"
            min={0}
            max={meta.duration || 0}
            step={0.01}
            value={t}
            style={{ flex: 1 }}
            onChange={(e) => seek(Number(e.target.value))}
          />
          <span className="mono">
            {t.toFixed(2)} / {(meta.duration || 0).toFixed(2)} s
          </span>
        </div>
      )}
      <div className="feature-body" style={{ padding: 0 }}>
        <DebugWorkbench busOut={(b) => { busRef.current = b; }} />
      </div>
    </div>
  );
}
