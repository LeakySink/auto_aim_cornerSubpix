import { useEffect, useState } from "react";
import { getJson, postJson, startFeature } from "../../shared/api";

type Job = {
  id: string;
  state: string;
  error?: string;
  output?: string;
  result?: Record<string, unknown>;
};

export function DumpPage() {
  const [path, setPath] = useState("");
  const [output, setOutput] = useState("");
  const [fps, setFps] = useState(10);
  const [job, setJob] = useState<Job | null>(null);
  const [err, setErr] = useState("");

  useEffect(() => {
    startFeature("dump", {}).catch(() => {});
  }, []);

  useEffect(() => {
    if (!job || job.state === "done" || job.state === "error") return;
    const t = setInterval(async () => {
      try {
        const j = await getJson<Job>(`/api/dump/jobs/${job.id}`);
        setJob(j);
      } catch (e) {
        setErr(String(e));
      }
    }, 500);
    return () => clearInterval(t);
  }, [job?.id, job?.state]);

  const run = async () => {
    setErr("");
    setJob(null);
    try {
      const res = await postJson<{ ok: boolean; job_id?: string; error?: string }>(
        "/api/dump/run",
        { path, output: output || undefined, fps }
      );
      if (!res.ok || !res.job_id) throw new Error(res.error || "failed");
      setJob({ id: res.job_id, state: "running" });
    } catch (e) {
      setErr(String(e));
    }
  };

  return (
    <div className="feature-page">
      <div className="feature-toolbar">
        <strong>Dump</strong>
      </div>
      <div className="feature-body">
        <div className="form-row">
          <label>.rlog</label>
          <input value={path} onChange={(e) => setPath(e.target.value)} placeholder="logs/run_xxx.rlog" />
        </div>
        <div className="form-row">
          <label>输出目录</label>
          <input value={output} onChange={(e) => setOutput(e.target.value)} placeholder="默认 <stem>_dump" />
        </div>
        <div className="form-row">
          <label>fps</label>
          <input
            type="number"
            value={fps}
            min={1}
            max={60}
            style={{ minWidth: "6rem", flex: "0 0 auto" }}
            onChange={(e) => setFps(Number(e.target.value))}
          />
          <button type="button" onClick={run} disabled={!path}>
            开始导出
          </button>
        </div>
        {err && <p style={{ color: "var(--err)" }}>{err}</p>}
        {job && (
          <div className="pre mono">
            job={job.id}  state={job.state}
            {job.output ? `\noutput=${job.output}` : ""}
            {job.error ? `\nerror=${job.error}` : ""}
            {job.result ? `\n${JSON.stringify(job.result, null, 2)}` : ""}
          </div>
        )}
        <p style={{ color: "var(--muted)", marginTop: "1.5rem" }}>
          导出结果：log.txt / plot.txt / images.mp4，行格式 [t][LOG|PLOT]-----…
        </p>
      </div>
    </div>
  );
}
