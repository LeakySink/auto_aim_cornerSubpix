import { Link } from "react-router-dom";
import { FEATURE_MODULES } from "../features/registry";
import type { FeatureStatus } from "../features/types";

export function HomePage({
  statuses,
}: {
  statuses: Record<string, FeatureStatus>;
}) {
  return (
    <div className="home">
      <h1>rdbg Host</h1>
      <p className="sub">选择功能进入。各功能在独立线程中运行，切换页面不会自动停止。</p>
      <div className="grid">
        {FEATURE_MODULES.map((m) => {
          const st = statuses[m.id]?.state || "idle";
          return (
            <Link key={m.id} to={m.route} className="card">
              <h2>{m.title}</h2>
              <p>{m.description}</p>
              <div className={`state ${st === "running" ? "running" : ""}`}>
                {st}
              </div>
            </Link>
          );
        })}
      </div>
    </div>
  );
}
