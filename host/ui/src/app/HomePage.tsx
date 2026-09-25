import { useEffect, useState } from "react";
import { useParams } from "react-router-dom";
import { FEATURE_MODULES } from "../features/registry";
import { featureStatus, getJson, listInstances, openFeature } from "../shared/api";
import { InstanceProvider } from "../shared/instance";
import { featureFromApp } from "../shared/robotFeature";

type Row = { instance: string; feature: string; title: string; state: string; sender?: string; data_port?: number; path?: string };
type Robot = {
  name: string;
  ip: string;
  data_port: number;
  watches: number;
  app?: string;
  feature?: string;
};

const HOME_TOOLS = FEATURE_MODULES.filter((m) => m.id !== "watch" && m.id !== "calibrate");

export function HomePage() {
  const [rows, setRows] = useState<Row[]>([]);
  const [robots, setRobots] = useState<Robot[]>([]);
  const [err, setErr] = useState("");
  const [manual, setManual] = useState("");

  useEffect(() => {
    let dead = false;
    const tick = async () => {
      try {
        const r = await listInstances();
        if (!dead) setRows(r.instances || []);
        const bots = await getJson<{ robots: Robot[] }>("/api/robots");
        if (!dead) setRobots(bots.robots || []);
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

  const open = async (feature: string, config: Record<string, unknown> = {}) => {
    setErr("");
    try {
      const r = await openFeature(feature, config);
      const w = window.open(r.path, "_blank");
      if (!w) setErr("浏览器拦截了新窗口，请允许弹窗后重试");
    } catch (e) {
      setErr(String(e));
    }
  };

  const openRobot = (r: Robot) => {
    const feat =
      r.feature === "calibrate" || r.feature === "watch"
        ? r.feature
        : featureFromApp(r.app);
    open(feat, { sender: r.name });
  };

  const openManual = () => {
    const name = manual.trim();
    if (!name) return;
    const known = robots.find((r) => r.name === name);
    if (known) openRobot(known);
    else open("watch", { sender: name });
  };

  return (
    <div className="app-shell">
      <header className="topbar">
        <span className="brand">rdbg</span>
        <span className="topbar-sep" />
        <span className="topbar-title">Host</span>
        <div className="spacer" />
        <span className="pill">{rows.length ? `${rows.length} 个页面` : "无打开页面"}</span>
      </header>
      <div className="main">
        <div className="home">
          <header className="page-head">
            <h1>调试入口</h1>
            <p className="sub">每次打开都是独立页面和线程，可以多开。关掉页面就会停掉对应线程。</p>
          </header>
          {err && <p className="err-text">{err}</p>}
          <section className="block">
            <div className="sec-label">功能</div>
            <div className="launcher">
              {HOME_TOOLS.map((m) => (
                <button key={m.id} type="button" className="tool" onClick={() => open(m.id)}>
                  <span className="tool-id">{m.id}</span>
                  <span className="tool-body">
                    <span className="tool-title">{m.title}</span>
                    <span className="tool-desc">{m.description}</span>
                  </span>
                  <span className="tool-go">新窗口</span>
                </button>
              ))}
            </div>
          </section>
          <section className="block">
            <div className="sec-label">车辆</div>
            <div className="launcher">
              {robots.map((r) => {
                const feat =
                  r.feature === "calibrate" || r.feature === "watch"
                    ? r.feature
                    : featureFromApp(r.app);
                const meta = [
                  r.app && r.app !== "normal" ? `app=${r.app}` : "",
                  r.ip,
                  r.data_port ? `UDP ${r.data_port}` : "",
                ]
                  .filter(Boolean)
                  .join(" · ");
                return (
                  <button
                    key={r.name}
                    type="button"
                    className="tool"
                    onClick={() => openRobot(r)}
                  >
                    <span className="tool-id">{feat}</span>
                    <span className="tool-body">
                      <span className="tool-title">{r.name}</span>
                      {meta && <span className="tool-desc mono">{meta}</span>}
                    </span>
                    <span className="tool-go">新窗口</span>
                  </button>
                );
              })}
              {robots.length === 0 && (
                <p className="empty launcher-empty">
                  还没有发现车辆，可以直接填车名。标定程序 beacon 带 app=calibrate。
                </p>
              )}
              <form
                className="launcher-manual"
                onSubmit={(e) => {
                  e.preventDefault();
                  openManual();
                }}
              >
                <input
                  value={manual}
                  placeholder="车名 sender_name"
                  onChange={(e) => setManual(e.target.value)}
                />
                <button type="submit" disabled={!manual.trim()}>
                  打开
                </button>
              </form>
            </div>
          </section>
          {rows.length > 0 && (
            <section className="block">
              <div className="sec-label">已打开</div>
              <ul className="inst-list">
                {rows.map((r) => (
                  <li key={r.instance}>
                    <a href={r.path || `/i/${r.instance}`} target="_blank" rel="noreferrer">
                      <span className="inst-title">
                        {r.title}
                        {r.sender ? ` · ${r.sender}` : ""}
                      </span>
                      <span className="inst-meta mono">{r.data_port ? `UDP ${r.data_port}` : r.instance}</span>
                      <span className={`state-tag s-${r.state}`}>{r.state}</span>
                    </a>
                  </li>
                ))}
              </ul>
            </section>
          )}
        </div>
      </div>
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

  useEffect(() => {
    if (feature === "calibrate" && iid) {
      window.location.replace(`/calibrate.html?i=${iid}`);
    }
  }, [feature, iid]);

  if (feature === "calibrate") {
    return <p className="page-pad muted">正在打开标定页…</p>;
  }

  return (
    <div className="app-shell">
      <header className="topbar">
        <a href="/" className="brand" target="_blank" rel="noreferrer">
          rdbg
        </a>
        <span className="topbar-sep" />
        {mod && <span className="topbar-title">{mod.title}</span>}
        <div className="spacer" />
        {mod && <span className="pill on">{mod.id}</span>}
        <button type="button" className="ghost" onClick={close}>
          关闭
        </button>
      </header>
      <div className="main">
        {err && <p className="err-text page-pad">{err}</p>}
        {mod && iid && mod.Component && (
          <InstanceProvider value={{ id: iid, feature: mod.id, base: `/api/i/${iid}` }}>
            <mod.Component />
          </InstanceProvider>
        )}
      </div>
    </div>
  );
}
