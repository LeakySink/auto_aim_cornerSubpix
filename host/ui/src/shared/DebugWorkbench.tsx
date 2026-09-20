import { useEffect, useMemo, useRef, useState } from "react";
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
  addLog: (level: string, msg: string) => void;
  setImage: (name: string, objectUrl: string) => void;
  clear: () => void;
};

type PanelKind = "plot" | "image" | "log";

const COLORS = ["#3d8bfd", "#3ecf8e", "#e6a23c", "#f07178", "#c792ea", "#89ddff"];

export function DebugWorkbench({
  busOut,
}: {
  busOut: (bus: DataBus) => void;
}) {
  const [kinds, setKinds] = useState<PanelKind[]>(["plot", "log", "image"]);
  const [logs, setLogs] = useState<{ level: string; msg: string }[]>([]);
  const [images, setImages] = useState<Record<string, string>>({});
  const [imgSel, setImgSel] = useState("");
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const chartRef = useRef<Chart | null>(null);
  const seriesRef = useRef<Record<string, { x: number; y: number }[]>>({});

  useEffect(() => {
    if (!canvasRef.current) return;
    if (chartRef.current) return;
    const chart = new Chart(canvasRef.current, {
      type: "line",
      data: { datasets: [] },
      options: {
        animation: false,
        responsive: true,
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
    return () => {
      chart.destroy();
      chartRef.current = null;
    };
  }, [kinds.includes("plot")]);

  const bus = useMemo<DataBus>(() => {
    const syncChart = () => {
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
    };
    return {
      addPoint(field, t, v) {
        if (!seriesRef.current[field]) seriesRef.current[field] = [];
        const arr = seriesRef.current[field];
        arr.push({ x: t, y: v });
        if (arr.length > 4000) arr.splice(0, arr.length - 3500);
        syncChart();
      },
      addLog(level, msg) {
        setLogs((prev) => {
          const next = [...prev, { level, msg }];
          return next.length > 2000 ? next.slice(-1600) : next;
        });
      },
      setImage(name, objectUrl) {
        setImages((prev) => {
          const old = prev[name];
          if (old && old.startsWith("blob:")) URL.revokeObjectURL(old);
          return { ...prev, [name]: objectUrl };
        });
        setImgSel(name);
      },
      clear() {
        seriesRef.current = {};
        if (chartRef.current) {
          chartRef.current.data.datasets = [];
          chartRef.current.update();
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
  }, []);

  useEffect(() => {
    busOut(bus);
  }, [bus, busOut]);

  const names = Object.keys(images);
  const activeImg = images[imgSel] || images[names[0]];
  const plotIndex = kinds.indexOf("plot");

  return (
    <div style={{ display: "flex", flexDirection: "column", height: "100%", minHeight: 0 }}>
      <div className="tile-root" style={{ flex: 1 }}>
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
              {k === "plot" && (
                <div style={{ height: "100%", padding: 4 }}>
                  {i === plotIndex ? (
                    <canvas ref={canvasRef} />
                  ) : (
                    <div className="mono" style={{ padding: 8, color: "var(--muted)" }}>
                      绘图已在其他面板
                    </div>
                  )}
                </div>
              )}
              {k === "log" && (
                <div className="log-view">
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
                  {activeImg ? (
                    <img src={activeImg} alt="" />
                  ) : (
                    <span className="mono">waiting…</span>
                  )}
                </div>
              )}
            </div>
          </div>
        ))}
      </div>
    </div>
  );
}
