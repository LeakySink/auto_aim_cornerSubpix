import { useCallback, useEffect, useRef, useState } from "react";
import { postJson, stopFeature } from "../../shared/api";
import { DebugWorkbench, type DataBus } from "../../shared/DebugWorkbench";
import { useInstance } from "../../shared/instance";

type Frame = { i: number; t: number; meta: { name?: string } };
type LogRec = { t: number; ts?: number; level: string; msg: string };
type Meta = {
  duration: number;
  t0_ns?: number;
  series: Record<string, [number, number][]>;
  logs: LogRec[];
  frames: Frame[];
  file?: string;
};

export function ReplayPage() {
  const inst = useInstance();
  const [path, setPath] = useState("");
  const [meta, setMeta] = useState<Meta | null>(null);
  const [err, setErr] = useState("");
  const [playing, setPlaying] = useState(false);
  const [t, setT] = useState(0);
  const [rate, setRate] = useState(1);
  const busRef = useRef<DataBus | null>(null);
  const metaRef = useRef<Meta | null>(null);
  const tRef = useRef(0);
  const rateRef = useRef(1);
  const playRef = useRef({ wall: 0, t0: 0 });
  const lastImg = useRef<Record<string, number>>({});
  const rafRef = useRef(0);

  tRef.current = t;
  rateRef.current = rate;

  const applyImages = useCallback(async (time: number) => {
    const m = metaRef.current;
    const bus = busRef.current;
    if (!m || !bus) return;
    const latest: Record<string, Frame> = {};
    for (const fr of m.frames || []) {
      if (fr.t > time) break;
      const n = fr.meta?.name || "default";
      latest[n] = fr;
    }
    const names = Object.keys(latest);
    if (names.length) bus.setStreams(names);
    await Promise.all(
      names.map(async (name) => {
        const fr = latest[name];
        if (lastImg.current[name] === fr.i) return;
        lastImg.current[name] = fr.i;
        try {
          const r = await fetch(`${inst.base}/frame/${fr.i}`);
          if (!r.ok) return;
          if (lastImg.current[name] !== fr.i) return;
          const blob = await r.blob();
          bus.setImage(name, URL.createObjectURL(blob));
        } catch {
          /* ignore */
        }
      })
    );
  }, [inst.base]);

  const seek = useCallback(
    (nt: number, keepPlay = false) => {
      const dur = metaRef.current?.duration || 0;
      const next = Math.max(0, Math.min(dur, nt));
      setT(next);
      tRef.current = next;
      if (playing || keepPlay) {
        playRef.current = { wall: performance.now(), t0: next };
      }
      void applyImages(next);
    },
    [applyImages, playing]
  );

  const onBus = useCallback((b: DataBus) => {
    busRef.current = b;
  }, []);

  const load = async () => {
    setErr("");
    setPlaying(false);
    try {
      const res = await postJson<{ ok: boolean; meta?: Meta; error?: string }>(`${inst.base}/load`, { path });
      if (!res.ok || !res.meta) throw new Error(res.error || "load failed");
      setMeta(res.meta);
      metaRef.current = res.meta;
      lastImg.current = {};
      busRef.current?.clear();
      busRef.current?.loadReplay(res.meta.series || {}, res.meta.logs || [], res.meta.duration || 0);
      const names = [
        ...new Set((res.meta.frames || []).map((f) => f.meta?.name || "default")),
      ];
      if (names.length) busRef.current?.setStreams(names);
      setT(0);
      tRef.current = 0;
      void applyImages(0);
    } catch (e) {
      setErr(String(e));
    }
  };

  useEffect(() => {
    if (!playing || !meta) return;
    playRef.current = { wall: performance.now(), t0: tRef.current };
    const loop = (now: number) => {
      const dur = metaRef.current?.duration || 0;
      const elapsed = ((now - playRef.current.wall) / 1000) * rateRef.current;
      const next = playRef.current.t0 + elapsed;
      if (next >= dur) {
        setT(dur);
        tRef.current = dur;
        setPlaying(false);
        void applyImages(dur);
        return;
      }
      setT(next);
      tRef.current = next;
      void applyImages(next);
      rafRef.current = requestAnimationFrame(loop);
    };
    rafRef.current = requestAnimationFrame(loop);
    return () => cancelAnimationFrame(rafRef.current);
  }, [playing, meta, applyImages]);

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const tag = (e.target as HTMLElement | null)?.tagName || "";
      if (tag === "INPUT" || tag === "SELECT" || tag === "TEXTAREA") return;
      if (!metaRef.current) return;
      if (e.code === "Space") {
        e.preventDefault();
        setPlaying((p) => {
          if (!p && tRef.current >= (metaRef.current?.duration || 0) - 1e-4) {
            tRef.current = 0;
            setT(0);
          }
          return !p;
        });
      } else if (e.code === "ArrowLeft") {
        setPlaying(false);
        seek(tRef.current - 0.05);
      } else if (e.code === "ArrowRight") {
        setPlaying(false);
        seek(tRef.current + 0.05);
      }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [seek]);

  const onSeek = useCallback(
    (nt: number) => {
      setPlaying(false);
      seek(nt);
    },
    [seek]
  );

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
        <button
          type="button"
          className="ghost"
          disabled={!meta}
          title="空格 播放/暂停"
          onClick={() => {
            if (!playing && t >= (meta?.duration || 0) - 1e-4) {
              setT(0);
              tRef.current = 0;
            }
            setPlaying((p) => !p);
          }}
        >
          {playing ? "暂停" : "播放"}
        </button>
        <select
          value={rate}
          onChange={(e) => {
            const r = Number(e.target.value);
            setRate(r);
            rateRef.current = r;
            playRef.current = { wall: performance.now(), t0: tRef.current };
          }}
        >
          {[0.25, 0.5, 1, 2, 4].map((r) => (
            <option key={r} value={r}>
              {r}×
            </option>
          ))}
        </select>
        <button type="button" className="ghost" onClick={() => busRef.current?.resetView()}>
          重置
        </button>
        <button type="button" className="ghost" onClick={() => stopFeature(inst.id)}>
          停止线程
        </button>
        {err && <span className="err-text">{err}</span>}
        {meta && <span className="mono">{meta.file}</span>}
      </div>
      {meta && (
        <div className="feature-toolbar scrub">
          <input
            type="range"
            min={0}
            max={meta.duration || 0}
            step={0.01}
            value={t}
            style={{ flex: 1 }}
            onPointerDown={() => setPlaying(false)}
            onChange={(e) => seek(Number(e.target.value))}
          />
          <span className="mono">
            {t.toFixed(2)} / {(meta.duration || 0).toFixed(2)} s
          </span>
          <span className="mono muted">
            全量曲线/日志 · 拖橙线或点日志跳转 · ← → 0.05s · 空格播放
          </span>
        </div>
      )}
      <div className="feature-body fill">
        <DebugWorkbench replay cursorT={meta ? t : null} onSeek={onSeek} busOut={onBus} />
      </div>
    </div>
  );
}
