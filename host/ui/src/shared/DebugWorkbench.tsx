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
  clear: () => void;
  resetView: () => void;
};

type PanelKind = "plot" | "log" | "image";
type LogLine = { level: string; msg: string; ts?: number };
type ViewMode = "sliding" | "centered" | "paused";

const COLORS = ["#4fc3f7", "#ffb74d", "#81c784", "#e57373", "#ba68c8", "#4dd0e1", "#fff176", "#a1887f"];

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
}: {
  busOut: (bus: DataBus) => void;
  imageSubscribe?: (names: string[]) => void;
}) {
  const [kinds, setKinds] = useState<PanelKind[]>(["plot", "log", "image"]);
  const [sidebar, setSidebar] = useState(true);
  const [mode, setMode] = useState<ViewMode>("sliding");
  const [win, setWin] = useState(10);
  const [history, setHistory] = useState(60);
  const [fields, setFields] = useState<Record<string, { color: string; on: boolean }>>({});
  const [logs, setLogs] = useState<LogLine[]>([]);
  const [images, setImages] = useState<Record<string, string>>({});
  const [streams, setStreams] = useState<string[]>([]);
  const [imgSel, setImgSel] = useState("");
  const [manual, setManual] = useState(false);

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

  fieldsRef.current = fields;
  viewRef.current = { mode, win, history, manual };

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
    const x = lastX.current;
    if (!v.manual && v.mode !== "paused") {
      const w = v.win || 10;
      if (v.mode === "centered") {
        chart.options.scales!.x!.min = x - w / 2;
        chart.options.scales!.x!.max = x + w / 2;
      } else {
        chart.options.scales!.x!.min = Math.max(0, x - w);
        chart.options.scales!.x!.max = Math.max(w, x);
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
    const names = Object.keys(pending);
    if (names.length) {
      imgBuf.current = {};
      setImages((prev) => {
        const next = { ...prev };
        for (const name of names) {
          const url = pending[name];
          const old = next[name];
          if (old && old.startsWith("blob:") && old !== url) URL.revokeObjectURL(old);
          next[name] = url;
        }
        return next;
      });
      setImgSel((cur) => cur || names[names.length - 1]);
    }
    if (streamBuf.current.length) {
      const list = streamBuf.current;
      streamBuf.current = [];
      setStreams((prev) => {
        const s = new Set(prev);
        list.forEach((n) => s.add(n));
        return [...s].sort();
      });
      setImgSel((cur) => cur || list[0] || "");
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
  }, [mode, win, history, manual, fields, schedule]);

  const plotIndex = kinds.indexOf("plot");

  useEffect(() => {
    const canvas = canvasRef.current;
    const host = hostRef.current;
    if (plotIndex < 0 || !canvas || !host) return;
    const chart = new Chart(canvas, {
      type: "line",
      data: { datasets: [] },
      options: {
        animation: false,
        responsive: false,
        maintainAspectRatio: false,
        plugins: {
          legend: { display: false },
          zoom: {
            pan: {
              enabled: true,
              mode: "x",
              onPanStart: () => {
                setManual(true);
              },
            },
            zoom: {
              wheel: { enabled: true },
              pinch: { enabled: true },
              mode: "x",
              onZoomStart: () => {
                setManual(true);
              },
            },
          },
        },
        scales: {
          x: { type: "linear", min: 0, max: 10, ticks: { color: "#8b9bb0" }, grid: { color: "#243044" } },
          y: { type: "linear", ticks: { color: "#8b9bb0" }, grid: { color: "#243044" } },
        },
      },
    });
    chartRef.current = chart;
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
      ro.disconnect();
      chart.destroy();
      chartRef.current = null;
    };
  }, [plotIndex, syncChart]);

  useEffect(() => {
    if (imgSel) imageSubscribe?.([imgSel]);
  }, [imgSel, imageSubscribe]);

  const bus = useMemo<DataBus>(() => {
    return {
      addPoint(field, t, v) {
        if (!seriesRef.current[field]) seriesRef.current[field] = [];
        const arr = seriesRef.current[field];
        arr.push({ x: t, y: v });
        lastX.current = t;
        const hist = viewRef.current.history || 60;
        const cut = t - hist;
        while (arr.length > 1 && arr[0].x < cut) arr.shift();
        if (arr.length > 8000) arr.splice(0, arr.length - 6000);
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
        setManual(false);
        chartDirty.current = true;
        schedule();
      },
    };
  }, [schedule]);

  useEffect(() => {
    busOut(bus);
  }, [bus, busOut]);

  const names = [...new Set([...streams, ...Object.keys(images)])].sort();
  const activeImg = images[imgSel];

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
                  <input type="radio" name="view-mode" checked={mode === m} onChange={() => setMode(m)} />
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
        {kinds.map((k, i) => (
          <div className="tile-pane" key={i}>
            <div className="tile-head">
              <select
                value={k}
                onChange={(e) => {
                  const n = [...kinds];
                  n[i] = e.target.value as PanelKind;
                  setKinds(n);
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
                  const n = [...kinds];
                  n.splice(i + 1, 0, "plot");
                  setKinds(n.slice(0, 6));
                }}
              >
                拆分
              </button>
              {kinds.length > 1 && (
                <button
                  type="button"
                  className="ghost"
                  title="关闭"
                  onClick={() => setKinds(kinds.filter((_, j) => j !== i))}
                >
                  ×
                </button>
              )}
            </div>
            <div className="tile-content">
              {k === "plot" &&
                (i === plotIndex ? (
                  <div className="plot-host" ref={hostRef}>
                    <canvas ref={canvasRef} />
                  </div>
                ) : (
                  <div className="mono" style={{ padding: 8, color: "var(--muted)" }}>
                    绘图已在其他面板
                  </div>
                ))}
              {k === "log" && <LogPane lines={logs} />}
              {k === "image" && (
                <ImagePane
                  names={names}
                  sel={imgSel || names[0] || ""}
                  url={activeImg}
                  onSel={setImgSel}
                />
              )}
            </div>
          </div>
        ))}
      </div>
    </div>
  );
}

function LogPane({ lines }: { lines: LogLine[] }) {
  const ref = useRef<HTMLDivElement>(null);
  const stick = useRef(true);
  const [lv, setLv] = useState({ DEBUG: true, INFO: true, WARN: true, ERROR: true });
  const visible = lines.filter((l) => lv[normLevel(l.level) as keyof typeof lv]);

  useEffect(() => {
    const el = ref.current;
    if (el && stick.current) el.scrollTop = el.scrollHeight;
  }, [visible.length, lines.length]);

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
      <div
        className="log-stream"
        ref={ref}
        onScroll={() => {
          const el = ref.current;
          if (!el) return;
          stick.current = el.scrollTop + el.clientHeight >= el.scrollHeight - 24;
        }}
      >
        {visible.length === 0 && <div className="L-DEBUG">(no log)</div>}
        {visible.map((l, idx) => (
          <div key={idx} className={`log-line L-${normLevel(l.level)}`}>
            {l.ts ? <span className="log-ts">{fmtTs(l.ts)}</span> : null}
            <span className="log-lv">{normLevel(l.level)}</span>
            <span className="log-msg">{l.msg}</span>
          </div>
        ))}
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
      <select value={sel} onChange={(e) => onSel(e.target.value)}>
        {names.length === 0 && <option value="">(no image)</option>}
        {names.map((n) => (
          <option key={n} value={n}>
            {n}
          </option>
        ))}
      </select>
      {url ? (
        <img
          src={url}
          alt=""
          draggable={false}
          style={{ transform: `translate(${pan.x}px, ${pan.y}px) scale(${scale})` }}
        />
      ) : (
        <span className="mono">{sel ? `等待 ${sel}…` : "waiting…"}</span>
      )}
    </div>
  );
}
