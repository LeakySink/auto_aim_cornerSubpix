import { useEffect, useState } from "react";
import { getJson, postJson, startFeature } from "../../shared/api";

type Beacon = { name: string; ip: string; control: number; from: string };
type PingResult = {
  sent: number;
  recv: number;
  loss_pct: number;
  rtt?: { min: number; avg: number; max: number; p50: number };
  lost_seq?: number[];
};

export function NetcheckPage() {
  const [beacons, setBeacons] = useState<Beacon[]>([]);
  const [discovering, setDiscovering] = useState(false);
  const [echoPort, setEchoPort] = useState(15050);
  const [echoOn, setEchoOn] = useState(false);
  const [echoStats, setEchoStats] = useState("");
  const [host, setHost] = useState("");
  const [pingPort, setPingPort] = useState(15050);
  const [size, setSize] = useState(1200);
  const [count, setCount] = useState(50);
  const [pingOut, setPingOut] = useState("");
  const [err, setErr] = useState("");

  useEffect(() => {
    startFeature("netcheck", {}).catch(() => {});
  }, []);

  useEffect(() => {
    if (!discovering) return;
    const t = setInterval(async () => {
      try {
        const r = await getJson<{ beacons: Beacon[] }>("/api/netcheck/discover/beacons");
        setBeacons(r.beacons || []);
      } catch {
        /* ignore */
      }
    }, 1000);
    return () => clearInterval(t);
  }, [discovering]);

  useEffect(() => {
    if (!echoOn) return;
    const t = setInterval(async () => {
      try {
        const r = await getJson<{ state: string; stats?: { replies: number; last: string } }>(
          "/api/netcheck/echo/status"
        );
        setEchoStats(
          `${r.state} replies=${r.stats?.replies ?? 0} last=${r.stats?.last || "-"}`
        );
      } catch {
        /* ignore */
      }
    }, 800);
    return () => clearInterval(t);
  }, [echoOn]);

  return (
    <div className="feature-page">
      <div className="feature-toolbar">
        <strong>Netcheck</strong>
        {err && <span style={{ color: "var(--err)" }}>{err}</span>}
      </div>
      <div className="feature-body">
        <h3>Discover（听 beacon）</h3>
        <div className="form-row">
          <button
            type="button"
            onClick={async () => {
              setErr("");
              try {
                await postJson("/api/netcheck/discover/start", {});
                setDiscovering(true);
              } catch (e) {
                setErr(String(e));
              }
            }}
          >
            开始
          </button>
          <button
            type="button"
            className="ghost"
            onClick={async () => {
              await postJson("/api/netcheck/discover/stop", {});
              setDiscovering(false);
            }}
          >
            停止
          </button>
          <span className="mono">{discovering ? "listening…" : "idle"}</span>
        </div>
        <div className="pre mono">
          {beacons.length === 0
            ? "(no beacons yet)"
            : beacons
                .map((b) => `${b.name}\t${b.ip}\tctrl=${b.control}\tfrom=${b.from}`)
                .join("\n")}
        </div>

        <h3 style={{ marginTop: "1.5rem" }}>Echo 服务</h3>
        <div className="form-row">
          <label>port</label>
          <input
            type="number"
            value={echoPort}
            style={{ minWidth: "6rem", flex: "0 0 auto" }}
            onChange={(e) => setEchoPort(Number(e.target.value))}
          />
          <button
            type="button"
            onClick={async () => {
              await postJson("/api/netcheck/echo/start", { port: echoPort });
              setEchoOn(true);
            }}
          >
            启动 echo
          </button>
          <button
            type="button"
            className="ghost"
            onClick={async () => {
              await postJson("/api/netcheck/echo/stop", {});
              setEchoOn(false);
            }}
          >
            停止
          </button>
          <span className="mono">{echoStats}</span>
        </div>

        <h3 style={{ marginTop: "1.5rem" }}>Ping</h3>
        <div className="form-row">
          <label>host</label>
          <input value={host} onChange={(e) => setHost(e.target.value)} placeholder="对方 IP" />
        </div>
        <div className="form-row">
          <label>port</label>
          <input type="number" value={pingPort} onChange={(e) => setPingPort(Number(e.target.value))} style={{ minWidth: "6rem", flex: "0 0 auto" }} />
          <label>size</label>
          <input type="number" value={size} onChange={(e) => setSize(Number(e.target.value))} style={{ minWidth: "6rem", flex: "0 0 auto" }} />
          <label>count</label>
          <input type="number" value={count} onChange={(e) => setCount(Number(e.target.value))} style={{ minWidth: "6rem", flex: "0 0 auto" }} />
          <button
            type="button"
            disabled={!host}
            onClick={async () => {
              setPingOut("running…");
              setErr("");
              try {
                const r = await postJson<{ ok: boolean; job_id: string }>("/api/netcheck/ping", {
                  host,
                  port: pingPort,
                  size,
                  count,
                });
                const poll = async () => {
                  const j = await getJson<{
                    state: string;
                    error?: string;
                    result?: PingResult;
                  }>(`/api/netcheck/jobs/${r.job_id}`);
                  if (j.state === "running") {
                    setTimeout(poll, 400);
                    return;
                  }
                  if (j.state === "error") {
                    setPingOut(j.error || "error");
                    return;
                  }
                  setPingOut(JSON.stringify(j.result, null, 2));
                };
                poll();
              } catch (e) {
                setErr(String(e));
              }
            }}
          >
            Ping
          </button>
        </div>
        <div className="pre mono">{pingOut || "(ping result)"}</div>
      </div>
    </div>
  );
}
