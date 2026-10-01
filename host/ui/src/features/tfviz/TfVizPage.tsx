import { useCallback, useEffect, useRef, useState } from "react";
import { featureStatus, getJson, postJson, startFeature, stopFeature } from "../../shared/api";
import { useSSE } from "../../shared/useSSE";
import { useInstance } from "../../shared/instance";
import { TfScene } from "./TfScene";
import { fmtMat3, fmtVec, parseTf, resolveFrame, type TfFrame } from "./tfMath";

type LivePose = {
  yaw: number;
  pitch: number;
  roll: number;
};

function radFmt(rad: number): string {
  return `${rad.toFixed(4)} rad (${((rad * 180) / Math.PI).toFixed(2)}°)`;
}

export function TfVizPage() {
  const inst = useInstance();
  const [state, setState] = useState("idle");
  const [err, setErr] = useState("");
  const [selected, setSelected] = useState("");
  const [port, setPort] = useState(0);
  const [robots, setRobots] = useState<{ name: string; ip: string; data_port: number }[]>([]);
  const [manual, setManual] = useState("");
  const [running, setRunning] = useState(false);
  const [frame, setFrame] = useState<TfFrame | null>(null);
  const [live, setLive] = useState<LivePose | null>(null);
  const [carCheck, setCarCheck] = useState("");
  const [hz, setHz] = useState(0);
  const hzRef = useRef({ n: 0, window: performance.now() });

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
    const t = setInterval(refresh, 1000);
    return () => clearInterval(t);
  }, [refresh]);

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
      if (msg.type !== "plot") return;
      const data = (msg.data || {}) as Record<string, unknown>;
      const tf = parseTf(data.tf);
      if (!tf) return;

      const resolved = resolveFrame(tf);
      setFrame(resolved);
      setLive({
        yaw: typeof data.gimbal_yaw === "number" ? data.gimbal_yaw : 0,
        pitch: typeof data.gimbal_pitch === "number" ? data.gimbal_pitch : 0,
        roll: typeof data.gimbal_roll === "number" ? data.gimbal_roll : 0,
      });

      const now = performance.now();
      hzRef.current.n += 1;
      if (now - hzRef.current.window >= 1000) {
        setHz(hzRef.current.n / ((now - hzRef.current.window) / 1000));
        hzRef.current.n = 0;
        hzRef.current.window = now;
      }

      if (tf.t_camera2world && tf.R_camera2world) {
        const dx = Math.hypot(
          resolved.t_camera2world[0] - tf.t_camera2world[0],
          resolved.t_camera2world[1] - tf.t_camera2world[1],
          resolved.t_camera2world[2] - tf.t_camera2world[2]
        );
        setCarCheck(dx < 1e-6 ? "与车上计算一致" : `与车上差 ${dx.toExponential(2)} m`);
      } else {
        setCarCheck("车上未发派生量");
      }
    },
    running
  );

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
        <strong>TF Viz</strong>
        <span className="mono">
          {selected || "未选择车辆"}
          {port ? ` · UDP ${port}` : ""} · {state}
          {hz > 0 ? ` · ${hz.toFixed(0)} Hz` : ""}
        </span>
        {err && <span className="err-text">{err}</span>}
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
            await startFeature(inst.id, { sender: selected, spawn: !selected });
            await refresh();
          }}
        >
          启动
        </button>
      </div>
      <div className="feature-body fill tfviz-layout">
        {!selected && (
          <div className="pick-pane">
            <div className="sec-label">选择车辆</div>
            <p className="sub">
              首页「TF Viz」会在无发布者时本机拉起 <span className="mono">build/tf_pub_test</span>
              （默认 <span className="mono">configs/tf_pub.yaml</span>）。也可手动选已有 feature=tfviz 的车。
            </p>
            {state === "error" && err && <p className="err-text">{err}</p>}
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
            {robots.length === 0 && state === "running" && (
              <p className="empty">等待 tf_pub beacon（feature/app=tfviz）…</p>
            )}
            {robots.length === 0 && state !== "running" && state !== "error" && (
              <p className="empty">还没有发现车辆。</p>
            )}
            <div className="form-row">
              <input value={manual} placeholder="车名 sender_name" onChange={(e) => setManual(e.target.value)} />
              <button type="button" disabled={!manual.trim()} onClick={() => bind(manual.trim())}>
                查看这辆车
              </button>
            </div>
          </div>
        )}
        {selected && (
          <>
            <div className="tfviz-stage">
              <TfScene frame={frame} />
            </div>
            <div className="tfviz-bottom">
              <section className="tfviz-col tfviz-col-fixed">
                <div className="sec-label">固定信息</div>
                {!frame && <p className="empty">等待 plot.tf …</p>}
                {frame && (
                  <>
                    <div className="tfviz-nums mono">
                      <div className="tfviz-cfg-name">{frame.config_name || "(unknown)"}</div>
                      {frame.config_path && <div className="muted tfviz-cfg-path">{frame.config_path}</div>}
                    </div>
                    <div className="sec-label">外参（yaml）</div>
                    <div className="tfviz-nums mono">
                      <div className="tfviz-mat-label">R_gimbal2imubody</div>
                      <pre className="tfviz-mat">{fmtMat3(frame.R_gimbal2imubody)}</pre>
                      <div className="tfviz-mat-label">R_camera2gimbal</div>
                      <pre className="tfviz-mat">{fmtMat3(frame.R_camera2gimbal)}</pre>
                      <div className="tfviz-mat-label">t_camera2gimbal (m)</div>
                      <div>{fmtVec(frame.t_camera2gimbal)}</div>
                    </div>
                    <div className="sec-label">定义</div>
                    <p className="sub mono">
                      R_g2w = R_g2imuᵀ · R_imuabs · R_g2imu
                      <br />
                      p_cam = R_g2w · t_c2g
                      <br />
                      R_c2w = R_g2w · R_c2g
                    </p>
                    <div className="sec-label">图例</div>
                    <ul className="tfviz-legend">
                      <li>
                        <span className="sw r" />X 红 · <span className="sw g" />Y 绿 ·{" "}
                        <span className="sw b" />Z 蓝
                      </li>
                      <li>灰圆柱 = 枪管（原点，云台 +X，电控姿态）</li>
                      <li>黄长方体 + 橙圆锥 = 相机（尖端贴机身，底朝光轴 +Z）</li>
                    </ul>
                  </>
                )}
              </section>
              <section className="tfviz-col tfviz-col-live">
                <div className="sec-label">电控云台位姿（实时）</div>
                {!frame && <p className="empty">等待 plot.tf …</p>}
                {frame && (
                  <div className="tfviz-nums mono">
                    {live && (
                      <>
                        <div className="tfviz-mat-label">yaw</div>
                        <div>{radFmt(live.yaw)}</div>
                        <div className="tfviz-mat-label">pitch</div>
                        <div>{radFmt(live.pitch)}</div>
                        <div className="tfviz-mat-label">roll</div>
                        <div>{radFmt(live.roll)}</div>
                      </>
                    )}
                    <div className="tfviz-mat-label">q (wxyz)</div>
                    <div>{fmtVec(frame.q, 4)}</div>
                    <div className="tfviz-mat-label">R_gimbal2world</div>
                    <pre className="tfviz-mat">{fmtMat3(frame.R_gimbal2world)}</pre>
                    <div className="tfviz-mat-label">R_camera2world</div>
                    <pre className="tfviz-mat">{fmtMat3(frame.R_camera2world)}</pre>
                    <div className="tfviz-mat-label">t_camera2world (m)</div>
                    <div>{fmtVec(frame.t_camera2world)}</div>
                    <div className="muted">{carCheck}</div>
                  </div>
                )}
              </section>
            </div>
          </>
        )}
      </div>
    </div>
  );
}
