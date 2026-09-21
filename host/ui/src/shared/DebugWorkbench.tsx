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
// chartjs-plugin-zoom looks up Hammer on window for pinch/pan. It binds the
// canvas only; the plot host is clipped so this cannot cover the toolbar.
(window as unknown as { Hammer: typeof Hammer }).Hammer = Hammer;

export type DataBus = {
  addPoint: (field: string, t: number, v: number) => void;
  addLog: (level: string, msg: string) => void;
  setImage: (name: string, objectUrl: string) => void;
  clear: () => void;
};

type PanelKind = "plot" | "log" | "image";
type LogLine = { level: string; msg: string };

const COLORS = ["#3d8bfd", "#3ecf8e", "#e6a23c", "#f07178", "#c792ea", "#89ddff"];

export function DebugWorkbench({
  busOut,
}: {
  busOut: (bus: DataBus) => void;
}) {
  const [kinds, setKinds] = useState<PanelKind[]>(["plot", "log", "image"]);
  const [logs, setLogs] = useState<LogLine[]>([]);
  const [images, setImages] = useState<Record<string, string>>({});
  const [imgSel, setImgSel] = useState("");
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const hostRef = useRef<HTMLDivElement>(null);
  const chartRef = useRef<Chart | null>(null);
  const seriesRef = useRef<Record<string, { x: number; y: number }[]>>({});
  const logBuf = useRef<LogLine[]>([]);
  const imgBuf = useRef<Record<string, string>>({});
  const chartDirty = useRef(false);
  const rafRef = useRef(0);

  const syncChart = useCallback(() => {
    const chart = chartRef.current;
    if (!chart) return;
    const labels = Object.keys(seriesRef.current);
    chart.data.datasets = labels.map((label, i) => ({
      label,
      data: seriesRef.current[label],
      borderColor: COLORS[i % COLORS.length],
      backgroundColor: COLORS[i % COLORS.length],
      pointRadius: 0,
      borderWidth: 1.5,
    }));
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
      setImgSel(names[names.length - 1]);
    }
  }, [syncChart]);

  const schedule = useCallback(() => {
    if (rafRef.current) return;
    rafRef.current = requestAnimationFrame(flush);
  }, [flush]);

  useEffect(() => {
    return () => {
      if (rafRef.current) cancelAnimationFrame(rafRef.current);
    };
  }, []);

  const plotIndex = kinds.indexOf("plot");

  // responsive:false + our own ResizeObserver. Chart.js responsive mode plus
  // CSS width/height 100% !important on the canvas resizes every frame and
  // freezes the main thread (clicks never run).
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
        scales: {
          x: { type: "linear" },
          y: { type: "linear" },
        },
        plugins: {
          legend: { display: true },
          zoom: {
            pan: { enabled: true, mode: "x" },
            zoom: {
              wheel: { enabled: true },
              pinch: { enabled: true },
              mode: "x",
            },
          },
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
      // Ignore 1px jitter (scrollbar / border) so the observer cannot loop.
      if (Math.abs(w - lastW) <= 1 && Math.abs(h - lastH) <= 1) return;
      lastW = w;
      lastH = h;
      chart.resize(w, h);
    };
    const ro = new ResizeObserver(() => fit());
    ro.observe(host);
    fit();
    if (Object.keys(seriesRef.current).length) syncChart();
    return () => {
      ro.disconnect();
      chart.destroy();
      chartRef.current = null;
    };
  }, [plotIndex, syncChart]);

  const bus = useMemo<DataBus>(() => {
    return {
      addPoint(field, t, v) {
        if (!seriesRef.current[field]) seriesRef.current[field] = [];
        const arr = seriesRef.current[field];
        arr.push({ x: t, y: v });
        if (arr.length > 4000) arr.splice(0, arr.length - 3500);
        chartDirty.current = true;
        schedule();
      },
      addLog(level, msg) {
        logBuf.current.push({ level, msg });
        if (logBuf.current.length > 2000) logBuf.current.splice(0, logBuf.current.length - 1600);
        schedule();
      },
      setImage(name, objectUrl) {
        const queued = imgBuf.current[name];
        if (queued && queued.startsWith("blob:") && queued !== objectUrl) {
          URL.revokeObjectURL(queued);
        }
        imgBuf.current[name] = objectUrl;
        schedule();
      },
      clear() {
        seriesRef.current = {};
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
        setImgSel("");
      },
    };
  }, [schedule]);

  useEffect(() => {
    busOut(bus);
  }, [bus, busOut]);

  const names = Object.keys(images);
  const activeImg = images[imgSel] || images[names[0]];

  return (
    <div className="workbench-root">
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
              {k === "log" && (
                <div className="log-view">
                  {logs.length === 0 && <div className="L-DEBUG">(no log)</div>}
                  {logs.map((l, idx) => (
                    <div key={idx} className={`L-${l.level}`}>
                      [{l.level}] {l.msg}
                    </div>
                  ))}
                </div>
              )}
              {k === "image" && (
                <div className="img-view">
                  <select
                    value={imgSel || names[0] || ""}
                    onChange={(e) => setImgSel(e.target.value)}
                  >
                    {names.length === 0 && <option value="">(no image)</option>}
                    {names.map((n) => (
                      <option key={n} value={n}>
                        {n}
                      </option>
                    ))}
                  </select>
                  {activeImg ? <img src={activeImg} alt="" /> : <span className="mono">waiting…</span>}
                </div>
              )}
            </div>
          </div>
        ))}
      </div>
    </div>
  );
}
