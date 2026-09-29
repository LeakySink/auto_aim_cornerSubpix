import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import {
  Chart,
  LineController,
  LineElement,
  PointElement,
  LinearScale,
  Legend,
} from "chart.js";
import zoomPlugin from "chartjs-plugin-zoom";
import Hammer from "hammerjs";

Chart.register(LineController, LineElement, PointElement, LinearScale, Legend, zoomPlugin);
(window as unknown as { Hammer: typeof Hammer }).Hammer = Hammer;

export type DataBus = {
  addPoint: (field: string, t: number, v: number) => void;
  addLog: (level: string, msg: string, ts?: number) => void;
  setImage: (name: string, objectUrl: string) => void;
  setStreams: (names: string[]) => void;
  loadReplay: (
    series: Record<string, [number, number][]>,
    logs: { t: number; ts?: number; level: string; msg: string }[],
    duration: number
  ) => void;
  clear: () => void;
  resetView: () => void;
};

type PanelKind = "plot" | "log" | "image";
type PanelState = { kind: PanelKind; imgSel: string };
type LogLine = { level: string; msg: string; ts?: number; t?: number };
type ViewMode = "sliding" | "centered" | "paused";

const COLORS = ["#4fc3f7", "#ffb74d", "#81c784", "#e57373", "#ba68c8", "#4dd0e1", "#fff176", "#a1887f"];

function fillEmptyImgSels(panels: PanelState[], fallback: string): PanelState[] {
  if (!fallback) return panels;
  let changed = false;
  const next = panels.map((p) => {
    if (p.kind !== "image" || p.imgSel) return p;
    changed = true;
    return { ...p, imgSel: fallback };
  });
  return changed ? next : panels;
}

function normLevel(lv: string) {
  const u = (lv || "INFO").toUpperCase();
  if (u === "WARNING") return "WARN";
  if (u === "FATAL" || u === "CRITICAL") return "ERROR";
  if (u === "DEBUG" || u === "INFO" || u === "WARN" || u === "ERROR") return u;
  return "INFO";
}

function fmtTs(ts?: number) {
  if (!ts) return "";
  const d = new Date(ts / 1e6);
  if (Number.isNaN(d.getTime())) return "";
  return d.toTimeString().slice(0, 8) + "." + String(d.getMilliseconds()).padStart(3, "0");
}

export function DebugWorkbench({
  busOut,
  imageSubscribe,
  replay = false,
  cursorT = null,
  onSeek,
}: {
  busOut: (bus: DataBus) => void;
  imageSubscribe?: (names: string[]) => void;
  replay?: boolean;
  cursorT?: number | null;
  onSeek?: (t: number) => void;
}) {
  const [panels, setPanels] = useState<PanelState[]>([
    { kind: "plot", imgSel: "" },
    { kind: "log", imgSel: "" },
    { kind: "image", imgSel: "" },
  ]);
  const [sidebar, setSidebar] = useState(true);
  const [mode, setMode] = useState<ViewMode>("sliding");
  const [win, setWin] = useState(10);
  const [history, setHistory] = useState(60);
  const [fields, setFields] = useState<Record<string, { color: string; on: boolean }>>({});
  const [logs, setLogs] = useState<LogLine[]>([]);
  const [images, setImages] = useState<Record<string, string>>({});
  const [streams, setStreams] = useState<string[]>([]);
  const [manual, setManual] = useState(false);
  const kinds = panels.map((p) => p.kind);

  const canvasRef = useRef<HTMLCanvasElement>(null);
  const hostRef = useRef<HTMLDivElement>(null);
  const chartRef = useRef<Chart | null>(null);
  const seriesRef = useRef<Record<string, { x: number; y: number }[]>>({});
  const fieldsRef = useRef(fields);
  const viewRef = useRef({ mode, win, history, manual });
  const lastX = useRef(0);
  const colorN = useRef(0);
  const logBuf = useRef<LogLine[]>([]);
  const imgBuf = useRef<Record<string, string>>({});
  const streamBuf = useRef<string[]>([]);
  const chartDirty = useRef(false);
  const rafRef = useRef(0);
  const replayRef = useRef(replay);
  const cursorRef = useRef<number | null>(cursorT);
  const seekRef = useRef(onSeek);
  const dragCursor = useRef(false);
  const mouseX = useRef(0);
  replayRef.current = replay;
  cursorRef.current = cursorT;
  seekRef.current = onSeek;

  fieldsRef.current = fields;
  // manual 由滚轮/拖动立刻写入。不能在这里用 state 覆盖，否则下一帧数据会把缩放清掉。
  viewRef.current = { ...viewRef.current, mode, win, history };

  const syncChart = useCallback(() => {
    const chart = chartRef.current;
    if (!chart) return;
    const meta = fieldsRef.current;
    const labels = Object.keys(seriesRef.current).filter((k) => meta[k]?.on !== false);
    chart.data.datasets = labels.map((label) => ({
      label,
      data: seriesRef.current[label],
      borderColor: meta[label]?.color || COLORS[0],
      backgroundColor: meta[label]?.color || COLORS[0],
      pointRadius: 0,
      borderWidth: 1.6,
    }));
    const v = viewRef.current;
    const follow = replayRef.current && cursorRef.current != null ? cursorRef.current : lastX.current;
    if (!v.manual && v.mode !== "paused") {
      const w = v.win || 10;
      if (v.mode === "centered") {
        chart.options.scales!.x!.min = follow - w / 2;
        chart.options.scales!.x!.max = follow + w / 2;
      } else {
        chart.options.scales!.x!.min = Math.max(0, follow - w);
        chart.options.scales!.x!.max = Math.max(w, follow);
      }
    }
    chart.update("none");
  }, []);

  const flush = useCallback(() => {
    rafRef.current = 0;
    if (chartDirty.current) {
      chartDirty.current = false;
      syncChart();
    }
    if (logBuf.current.length) {
      const batch = logBuf.current;
      logBuf.current = [];
      setLogs((prev) => {
        const next = prev.concat(batch);
        return next.length > 2000 ? next.slice(-1600) : next;
      });
    }
    const pending = imgBuf.current;
    const pendingNames = Object.keys(pending);
    if (pendingNames.length) {
      imgBuf.current = {};
      setImages((prev) => {
        const next = { ...prev };
        for (const name of pendingNames) {
          const url = pending[name];
          const old = next[name];
          if (old && old.startsWith("blob:") && old !== url) URL.revokeObjectURL(old);
          next[name] = url;
        }
        return next;
      });
      setPanels((prev) => fillEmptyImgSels(prev, pendingNames[pendingNames.length - 1]));
    }
    if (streamBuf.current.length) {
      const list = streamBuf.current;
      streamBuf.current = [];
      setStreams((prev) => {
        const s = new Set(prev);
        list.forEach((n) => s.add(n));
        return [...s].sort();
      });
      setPanels((prev) => fillEmptyImgSels(prev, list[0] || ""));
    }
  }, [syncChart]);

  const schedule = useCallback(() => {
    if (rafRef.current) return;
    rafRef.current = requestAnimationFrame(flush);
  }, [flush]);

  useEffect(() => () => {
    if (rafRef.current) cancelAnimationFrame(rafRef.current);
  }, []);

  useEffect(() => {
    chartDirty.current = true;
    schedule();
  }, [mode, win, history, manual, fields, cursorT, schedule]);

  const plotIndex = kinds.indexOf("plot");

  useEffect(() => {
    const canvas = canvasRef.current;
    const host = hostRef.current;
    if (plotIndex < 0 || !canvas || !host) return;
    const chart = new Chart(canvas, {
      type: "line",
      plugins: [
        {
          id: "replayCursor",
          afterDraw(ch) {
            const t = cursorRef.current;
            if (!replayRef.current || t == null || !Number.isFinite(t)) return;
            const xScale = ch.scales.x;
            if (!xScale) return;
            const x = xScale.getPixelForValue(t);
            const area = ch.chartArea;
            if (x < area.left - 2 || x > area.right + 2) return;
            const ctx = ch.ctx;
            ctx.save();
            ctx.beginPath();
            ctx.moveTo(x, area.top);
            ctx.lineTo(x, area.bottom);
            ctx.lineWidth = dragCursor.current ? 2.5 : 1.5;
            ctx.strokeStyle = "#ff7043";
            ctx.stroke();
            ctx.beginPath();
            ctx.moveTo(x, area.top);
            ctx.lineTo(x - 6, area.top - 7);
            ctx.lineTo(x + 6, area.top - 7);
            ctx.closePath();
            ctx.fillStyle = "#ff7043";
            ctx.fill();
            ctx.restore();
          },
        },
      ],
      data: { datasets: [] },
      options: {
        animation: false,
        responsive: false,
        maintainAspectRatio: false,
        onClick(evt, _els, ch) {
          if (!replayRef.current || !seekRef.current || dragCursor.current) return;
          const xScale = ch.scales.x;
          if (!xScale || evt.x == null) return;
          const cur = cursorRef.current;
          if (cur != null) {
            const cx = xScale.getPixelForValue(cur);
            if (Math.abs(evt.x - cx) <= 12) return;
          }
          const t = xScale.getValueForPixel(evt.x);
          if (t != null && Number.isFinite(t)) seekRef.current(t);
        },
        plugins: {
          legend: { display: false },
          zoom: {
            pan: {
              enabled: true,
              mode: "x" as const,
              overScaleMode: "y" as const,
              onPanStart: () => {
                if (dragCursor.current) return false;
                viewRef.current = { ...viewRef.current, manual: true };
                setManual(true);
              },
            },
            zoom: {
              wheel: { enabled: true },
              pinch: { enabled: true },
              mode: () => (mouseX.current < 50 ? "y" : "x"),
              overScaleMode: "y" as const,
              onZoomStart: () => {
                if (dragCursor.current) return false;
                viewRef.current = { ...viewRef.current, manual: true };
                setManual(true);
              },
            },
            limits: { x: { min: 0 } },
          },
        },
        scales: {
          x: {
            type: "linear",
            min: 0,
            max: 10,
            ticks: { color: "#8e8e9c", font: { size: 11, family: "ui-monospace, monospace" } },
            grid: { color: "rgba(255,255,255,0.06)" },
            border: { color: "rgba(255,255,255,0.08)" },
          },
          y: {
            type: "linear",
            ticks: { color: "#8e8e9c", font: { size: 11, family: "ui-monospace, monospace" } },
            grid: { color: "rgba(255,255,255,0.06)" },
            border: { color: "rgba(255,255,255,0.08)" },
          },
        },
      },
    });
    chartRef.current = chart;
    const onMove = (e: MouseEvent) => {
      const r = canvas.getBoundingClientRect();
      mouseX.current = e.clientX - r.left;
    };
    canvas.addEventListener("mousemove", onMove);
    let lastW = 0;
    let lastH = 0;
    const fit = () => {
      const w = Math.floor(host.clientWidth);
      const h = Math.floor(host.clientHeight);
      if (w < 2 || h < 2) return;
      if (Math.abs(w - lastW) <= 1 && Math.abs(h - lastH) <= 1) return;
      lastW = w;
      lastH = h;
      chart.resize(w, h);
    };
    const ro = new ResizeObserver(() => fit());
    ro.observe(host);
    fit();
    syncChart();
    return () => {
      canvas.removeEventListener("mousemove", onMove);
      ro.disconnect();
      chart.destroy();
      chartRef.current = null;
    };
  }, [plotIndex, syncChart]);

  const names = [...new Set([...streams, ...Object.keys(images)])].sort();
  const wantedImgs = [
    ...new Set(
      panels
        .filter((p) => p.kind === "image")
        .map((p) => p.imgSel || names[0] || "")
        .filter(Boolean)
    ),
  ].sort();

  const wantedKey = wantedImgs.join("\0");
  useEffect(() => {
    if (!imageSubscribe) return;
    imageSubscribe(wantedKey ? wantedKey.split("\0") : []);
  }, [wantedKey, imageSubscribe]);

  const bus = useMemo<DataBus>(() => {
    return {
      addPoint(field, t, v) {
        if (!seriesRef.current[field]) seriesRef.current[field] = [];
        const arr = seriesRef.current[field];
        arr.push({ x: t, y: v });
        lastX.current = t;
        if (!replayRef.current) {
          const hist = viewRef.current.history || 60;
          const cut = t - hist;
          while (arr.length > 1 && arr[0].x < cut) arr.shift();
          if (arr.length > 8000) arr.splice(0, arr.length - 6000);
        }
        if (!fieldsRef.current[field]) {
          const color = COLORS[colorN.current % COLORS.length];
          colorN.current += 1;
          const on = Object.keys(fieldsRef.current).length < 6;
          fieldsRef.current = { ...fieldsRef.current, [field]: { color, on } };
          setFields(fieldsRef.current);
        }
        chartDirty.current = true;
        schedule();
      },
      addLog(level, msg, ts) {
        logBuf.current.push({ level, msg, ts });
        if (logBuf.current.length > 2000) logBuf.current.splice(0, logBuf.current.length - 1600);
        schedule();
      },
      setImage(name, objectUrl) {
        const queued = imgBuf.current[name];
        if (queued && queued.startsWith("blob:") && queued !== objectUrl) URL.revokeObjectURL(queued);
        imgBuf.current[name] = objectUrl;
        schedule();
      },
      setStreams(names) {
        streamBuf.current.push(...names);
        schedule();
      },
      loadReplay(series, logs, duration) {
        const nextSeries: Record<string, { x: number; y: number }[]> = {};
        const nextFields = { ...fieldsRef.current };
        for (const [field, pts] of Object.entries(series || {})) {
          nextSeries[field] = pts.map(([x, y]) => ({ x, y }));
          if (!nextFields[field]) {
            nextFields[field] = {
              color: COLORS[colorN.current % COLORS.length],
              on: Object.keys(nextFields).length < 6,
            };
            colorN.current += 1;
          }
        }
        seriesRef.current = nextSeries;
        fieldsRef.current = nextFields;
        lastX.current = duration;
        setFields(nextFields);
        logBuf.current = [];
        setLogs(
          (logs || []).map((l) => ({
            level: l.level,
            msg: l.msg,
            ts: l.ts,
            t: l.t,
          }))
        );
        chartDirty.current = true;
        schedule();
      },
      clear() {
        seriesRef.current = {};
        lastX.current = 0;
        logBuf.current = [];
        chartDirty.current = false;
        Object.values(imgBuf.current).forEach((u) => {
          if (u.startsWith("blob:")) URL.revokeObjectURL(u);
        });
        imgBuf.current = {};
        if (chartRef.current) {
          chartRef.current.data.datasets = [];
          chartRef.current.update("none");
        }
        setLogs([]);
        setImages((prev) => {
          Object.values(prev).forEach((u) => {
            if (u.startsWith("blob:")) URL.revokeObjectURL(u);
          });
          return {};
        });
      },
      resetView() {
        viewRef.current = { ...viewRef.current, manual: false };
        setManual(false);
        const chart = chartRef.current as (Chart & { resetZoom?: () => void }) | null;
        chart?.resetZoom?.();
        chartDirty.current = true;
        schedule();
      },
    };
  }, [schedule]);

  useEffect(() => {
    busOut(bus);
  }, [bus, busOut]);

  return (
    <div className="workbench-root">
      <aside className={sidebar ? "dbg-side" : "dbg-side collapsed"}>
        <button type="button" className="side-toggle" onClick={() => setSidebar((s) => !s)}>
          {sidebar ? "◀" : "▶"}
        </button>
        {sidebar && (
          <div className="side-body">
            <div className="side-title">设置</div>
            <div className="side-sec">
              <div className="side-h">显示模式</div>
              {(["sliding", "centered", "paused"] as ViewMode[]).map((m) => (
                <label key={m}>
                  <input type="radio" name={replay ? "view-mode-replay" : "view-mode-watch"} checked={mode === m} onChange={() => setMode(m)} />
                  {m === "sliding" ? "滑动" : m === "centered" ? "居中" : "暂停"}
                </label>
              ))}
            </div>
            <div className="side-sec">
              <div className="side-h">窗口 (秒)</div>
              <input type="number" min={1} max={120} value={win} onChange={(e) => setWin(Number(e.target.value) || 10)} />
            </div>
            <div className="side-sec">
              <div className="side-h">历史</div>
              <select value={history} onChange={(e) => setHistory(Number(e.target.value))}>
                <option value={30}>30 秒</option>
                <option value={60}>1 分钟</option>
                <option value={120}>2 分钟</option>
                <option value={300}>5 分钟</option>
              </select>
            </div>
            <div className="side-sec">
              <div className="side-h">数据字段</div>
              {Object.keys(fields).length === 0 && <span className="muted">等待数据…</span>}
              {Object.entries(fields).map(([name, meta]) => (
                <label key={name} className="field-row">
                  <input
                    type="checkbox"
                    checked={meta.on}
                    onChange={(e) =>
                      setFields((prev) => ({ ...prev, [name]: { ...prev[name], on: e.target.checked } }))
                    }
                  />
                  <span className="swatch" style={{ background: meta.color }} />
                  {name}
                </label>
              ))}
            </div>
          </div>
        )}
      </aside>
      <div className="tile-root">
        {panels.map((panel, i) => {
          const k = panel.kind;
          const imgSel = panel.imgSel || names[0] || "";
          return (
            <div className="tile-pane" key={i}>
              <div className="tile-head">
                <select
                  value={k}
                  onChange={(e) => {
                    const kind = e.target.value as PanelKind;
                    setPanels((prev) => {
                      const n = [...prev];
                      const cur = n[i];
                      n[i] = {
                        ...cur,
                        kind,
                        imgSel:
                          kind === "image" && !cur.imgSel ? names[0] || "" : cur.imgSel,
                      };
                      return n;
                    });
                  }}
                >
                  <option value="plot">绘图</option>
                  <option value="log">日志</option>
                  <option value="image">图像</option>
                </select>
                <button
                  type="button"
                  className="ghost"
                  title="向右拆分"
                  onClick={() => {
                    setPanels((prev) => {
                      const n = [...prev];
                      n.splice(i + 1, 0, { kind: "plot", imgSel: "" });
                      return n.slice(0, 6);
                    });
                  }}
                >
                  拆分
                </button>
                {panels.length > 1 && (
                  <button
                    type="button"
                    className="ghost"
                    title="关闭"
                    onClick={() => setPanels((prev) => prev.filter((_, j) => j !== i))}
                  >
                    ×
                  </button>
                )}
              </div>
              <div className="tile-content">
                {k === "plot" &&
                  (i === plotIndex ? (
                    <div
                      className="plot-host"
                      ref={hostRef}
                      onPointerDown={(e) => {
                        if (!replayRef.current || !seekRef.current) return;
                        const chart = chartRef.current;
                        if (!chart || cursorRef.current == null) return;
                        const rect = chart.canvas.getBoundingClientRect();
                        const px = e.clientX - rect.left;
                        const cx = chart.scales.x.getPixelForValue(cursorRef.current);
                        if (Math.abs(px - cx) > 12) return;
                        dragCursor.current = true;
                        (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
                        e.preventDefault();
                      }}
                      onPointerMove={(e) => {
                        if (!dragCursor.current || !seekRef.current) return;
                        const chart = chartRef.current;
                        if (!chart) return;
                        const rect = chart.canvas.getBoundingClientRect();
                        const t = chart.scales.x.getValueForPixel(e.clientX - rect.left);
                        if (t != null && Number.isFinite(t)) seekRef.current(t);
                      }}
                      onPointerUp={() => {
                        dragCursor.current = false;
                      }}
                    >
                      <canvas ref={canvasRef} />
                    </div>
                  ) : (
                    <div className="mono muted" style={{ padding: 8 }}>
                      绘图已在其他面板
                    </div>
                  ))}
                {k === "log" && (
                  <LogPane
                    lines={logs}
                    cursorT={replay ? cursorT : null}
                    onSeek={replay ? onSeek : undefined}
                  />
                )}
                {k === "image" && (
                  <ImagePane
                    names={names}
                    sel={imgSel}
                    url={images[imgSel]}
                    onSel={(n) =>
                      setPanels((prev) => {
                        const next = [...prev];
                        next[i] = { ...next[i], imgSel: n };
                        return next;
                      })
                    }
                  />
                )}
              </div>
            </div>
          );
        })}
      </div>
    </div>
  );
}

function LogPane({
  lines,
  cursorT = null,
  onSeek,
}: {
  lines: LogLine[];
  cursorT?: number | null;
  onSeek?: (t: number) => void;
}) {
  const ref = useRef<HTMLDivElement>(null);
  const stick = useRef(true);
  const [lv, setLv] = useState({ DEBUG: true, INFO: true, WARN: true, ERROR: true });
  const [mark, setMark] = useState<{ y: number; kind: "line" | "gap" } | null>(null);
  const visible = lines.filter((l) => lv[normLevel(l.level) as keyof typeof lv]);

  let current = -1;
  if (cursorT != null) {
    for (let i = 0; i < visible.length; i++) {
      const tt = visible[i].t;
      if (tt == null) continue;
      if (tt <= cursorT) current = i;
      else break;
    }
  }

  useEffect(() => {
    const el = ref.current;
    if (!el) return;
    if (cursorT == null) {
      if (stick.current) el.scrollTop = el.scrollHeight;
      return;
    }
    const row = el.querySelector(".log-current") as HTMLElement | null;
    if (row) {
      const top = row.offsetTop;
      const bot = top + row.offsetHeight;
      if (top < el.scrollTop || bot > el.scrollTop + el.clientHeight) {
        el.scrollTop = Math.max(0, top - el.clientHeight / 3);
      }
      const next = visible[current + 1];
      const cur = visible[current];
      if (cur && next && cur.t != null && next.t != null && next.t > cur.t) {
        const frac = (cursorT - cur.t) / (next.t - cur.t);
        if (frac > 0.08 && frac < 0.92) {
          const nEl = row.nextElementSibling as HTMLElement | null;
          const y1 = nEl ? nEl.offsetTop : bot;
          setMark({ y: top + (y1 - top) * frac - el.scrollTop, kind: "gap" });
          return;
        }
      }
      setMark({ y: top + row.offsetHeight / 2 - el.scrollTop, kind: "line" });
    } else {
      setMark(null);
    }
  }, [visible.length, lines.length, cursorT, current]);

  return (
    <div className="log-pane">
      <div className="log-toolbar">
        {(["DEBUG", "INFO", "WARN", "ERROR"] as const).map((k) => (
          <label key={k}>
            <input
              type="checkbox"
              checked={lv[k]}
              onChange={(e) => setLv((p) => ({ ...p, [k]: e.target.checked }))}
            />
            {k}
          </label>
        ))}
        <span className="log-count">{visible.length}</span>
      </div>
      <div className="log-stream-wrap">
        {mark?.kind === "gap" && <div className="log-tri" style={{ top: mark.y }} />}
        <div
          className="log-stream"
          ref={ref}
          onScroll={() => {
            const el = ref.current;
            if (!el) return;
            stick.current = el.scrollTop + el.clientHeight >= el.scrollHeight - 24;
          }}
        >
          {visible.length === 0 && <div className="log-line L-DEBUG"><span className="log-msg">无日志</span></div>}
          {visible.map((l, idx) => (
            <div
              key={idx}
              className={`log-line L-${normLevel(l.level)}${idx === current ? " log-current" : ""}${onSeek ? " log-click" : ""}`}
              onClick={() => {
                if (l.t != null) onSeek?.(l.t);
              }}
            >
              {l.t != null ? (
                <span className="log-ts">{l.t.toFixed(3)}</span>
              ) : l.ts ? (
                <span className="log-ts">{fmtTs(l.ts)}</span>
              ) : null}
              <span className="log-lv">{normLevel(l.level)}</span>
              <span className="log-msg">{l.msg}</span>
            </div>
          ))}
        </div>
      </div>
    </div>
  );
}

function ImagePane({
  names,
  sel,
  url,
  onSel,
}: {
  names: string[];
  sel: string;
  url?: string;
  onSel: (n: string) => void;
}) {
  const [scale, setScale] = useState(1);
  const [pan, setPan] = useState({ x: 0, y: 0 });
  const drag = useRef<{ x: number; y: number; px: number; py: number } | null>(null);

  useEffect(() => {
    setScale(1);
    setPan({ x: 0, y: 0 });
  }, [sel]);

  return (
    <div className="img-pane">
      <div className="img-bar">
        <select value={sel} onChange={(e) => onSel(e.target.value)}>
          {names.length === 0 && <option value="">无图像</option>}
          {names.map((n) => (
            <option key={n} value={n}>
              {n}
            </option>
          ))}
        </select>
      </div>
      <div
        className="img-view"
        onWheel={(e) => {
          e.preventDefault();
          setScale((s) => Math.min(15, Math.max(0.1, s * (e.deltaY > 0 ? 0.9 : 1.1))));
        }}
        onMouseDown={(e) => {
          if (e.button !== 0) return;
          drag.current = { x: e.clientX, y: e.clientY, px: pan.x, py: pan.y };
        }}
        onMouseMove={(e) => {
          if (!drag.current) return;
          setPan({
            x: drag.current.px + e.clientX - drag.current.x,
            y: drag.current.py + e.clientY - drag.current.y,
          });
        }}
        onMouseUp={() => {
          drag.current = null;
        }}
        onMouseLeave={() => {
          drag.current = null;
        }}
      >
        {url ? (
          <img
            src={url}
            alt=""
            draggable={false}
            style={{ transform: `translate(${pan.x}px, ${pan.y}px) scale(${scale})` }}
          />
        ) : (
          <span className="mono muted">{sel ? `等待 ${sel}…` : "等待图像…"}</span>
        )}
      </div>
    </div>
  );
}
