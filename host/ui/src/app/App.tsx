import { Link, useLocation } from "react-router-dom";
import { FEATURE_MODULES } from "../features/registry";
import { KeepAlive } from "./KeepAlive";
import { HomePage } from "./HomePage";
import { useFeatureStatuses } from "./useFeatureStatuses";

export function App() {
  const loc = useLocation();
  const statuses = useFeatureStatuses(4000);
  const activeId = loc.pathname.replace(/^\//, "").split("/")[0] || "";
  const isHome = loc.pathname === "/" || loc.pathname === "";

  const panes = FEATURE_MODULES.map((m) => ({
    id: m.id,
    element: <m.Component />,
  }));

  const running = FEATURE_MODULES.filter(
    (m) => statuses[m.id]?.state === "running"
  );

  return (
    <div className="app-shell">
      <header className="topbar">
        <Link to="/" className="brand">
          rdbg
        </Link>
        {!isHome && (
          <Link to="/">
            <button type="button" className="ghost">
              首页
            </button>
          </Link>
        )}
        <div className="spacer" />
        <div className="pills">
          {running.map((m) => (
            <span key={m.id} className="pill on">
              {m.title} 运行中
            </span>
          ))}
        </div>
      </header>
      <div className="main">
        {isHome ? (
          <HomePage statuses={statuses} />
        ) : (
          <KeepAlive activeId={activeId} panes={panes} />
        )}
      </div>
    </div>
  );
}
