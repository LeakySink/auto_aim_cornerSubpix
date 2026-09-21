import { useEffect, useState } from "react";
import { useParams } from "react-router-dom";
import { FEATURE_MODULES } from "../features/registry";
import { featureStatus, listInstances, openFeature } from "../shared/api";
import { InstanceProvider } from "../shared/instance";

type Row = { instance: string; feature: string; title: string; state: string };

export function HomePage() {
  const [rows, setRows] = useState<Row[]>([]);
  const [err, setErr] = useState("");

  useEffect(() => {
    let dead = false;
    const tick = async () => {
      try {
        const r = await listInstances();
        if (!dead) setRows(r.instances || []);
      } catch {
        /* hub down */
      }
    };
    tick();
    const t = setInterval(tick, 2000);
    return () => {
      dead = true;
      clearInterval(t);
    };
  }, []);

  const open = async (feature: string) => {
    setErr("");
    try {
      const r = await openFeature(feature);
      const w = window.open(r.path, "_blank");
      if (!w) setErr("浏览器拦截了新窗口，请允许弹窗后重试");
    } catch (e) {
      setErr(String(e));
    }
  };

  return (
    <div className="home">
      <h1>rdbg Host</h1>
      <p className="sub">每次打开都是独立页面和线程，可以多开。关掉页面就会停掉对应线程。</p>
      {err && <p style={{ color: "var(--err)" }}>{err}</p>}
      <div className="grid">
        {FEATURE_MODULES.map((m) => (
          <button key={m.id} type="button" className="card" onClick={() => open(m.id)}>
            <h2>{m.title}</h2>
            <p>{m.description}</p>
            <div className="state">新开页面</div>
          </button>
        ))}
      </div>
      {rows.length > 0 && (
        <div style={{ marginTop: "1.5rem" }}>
          <h3>已打开</h3>
          <ul>
            {rows.map((r) => (
              <li key={r.instance}>
                <a href={`/i/${r.instance}`} target="_blank" rel="noreferrer">
                  {r.title} · {r.state}
                </a>
              </li>
            ))}
          </ul>
        </div>
      )}
    </div>
  );
}

export function InstancePage() {
  const { iid } = useParams();
  const [feature, setFeature] = useState("");
  const [err, setErr] = useState("");

  useEffect(() => {
    if (!iid) return;
    featureStatus(iid)
      .then((st) => setFeature(st.feature || ""))
      .catch(() => setErr("这个页面对应的线程已经关闭"));
  }, [iid]);

  const mod = FEATURE_MODULES.find((m) => m.id === feature);
  const close = () => {
    if (!iid) return;
    const url = `/api/instances/${iid}/stop?forget=1`;
    if (navigator.sendBeacon) navigator.sendBeacon(url, "");
    window.close();
    window.location.href = "/";
  };

  return (
    <div className="app-shell">
      <header className="topbar">
        <a href="/" className="brand" target="_blank" rel="noreferrer">
          rdbg
        </a>
        <button type="button" className="ghost" onClick={close}>
          关闭
        </button>
        <div className="spacer" />
        {mod && <span className="pill on">{mod.title}</span>}
      </header>
      <div className="main">
        {err && <p style={{ padding: "1rem" }}>{err}</p>}
        {mod && iid && (
          <InstanceProvider value={{ id: iid, feature: mod.id, base: `/api/i/${iid}` }}>
            <mod.Component />
          </InstanceProvider>
        )}
      </div>
    </div>
  );
}
