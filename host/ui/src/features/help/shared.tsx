/**
 * Help 手册共用图示与排版原子。
 * 各章 pages/* 只拼内容；动效 class 定义在 help.css（hp-* / help-*）。
 */
import type { ReactNode } from "react";

/** 章标题区：kicker 为「01 · 总览」类小节号。 */
export function PageHead({
  kicker,
  title,
  lead,
}: {
  kicker: string;
  title: string;
  lead: string;
}) {
  return (
    <header className="hp-head">
      <span className="hp-kicker">{kicker}</span>
      <h1>{title}</h1>
      <p>{lead}</p>
    </header>
  );
}

/** 等宽代码块（API 示例）。 */
export function CodeBlock({ children }: { children: string }) {
  return (
    <pre className="hp-code">
      <code>{children}</code>
    </pre>
  );
}

/** 仓库相对路径样式（非可点击链接；文档在 git 树内）。 */
export function DocPath({ children }: { children: ReactNode }) {
  return <code className="hp-path">{children}</code>;
}

/** 带序号的步骤条；每项可带短说明。 */
export function Steps({ items }: { items: { title: string; body?: string }[] }) {
  return (
    <ol className="hp-steps">
      {items.map((it, i) => (
        <li key={it.title} style={{ animationDelay: `${i * 60}ms` }}>
          <span className="hp-step-n">{i + 1}</span>
          <div>
            <strong>{it.title}</strong>
            {it.body ? <p>{it.body}</p> : null}
          </div>
        </li>
      ))}
    </ol>
  );
}

export function Callout({
  tone = "info",
  title,
  children,
}: {
  tone?: "info" | "warn" | "ok";
  title: string;
  children: ReactNode;
}) {
  return (
    <aside className={`hp-callout hp-callout-${tone}`}>
      <strong>{title}</strong>
      <div>{children}</div>
    </aside>
  );
}

export function CardGrid({
  items,
}: {
  items: { title: string; body: string; tag?: string; meta?: string }[];
}) {
  return (
    <div className="hp-cards">
      {items.map((it, i) => (
        <article key={it.title} className="hp-card" style={{ animationDelay: `${i * 50}ms` }}>
          <div className="hp-card-top">
            {it.tag ? <span className="hp-tag">{it.tag}</span> : null}
            {it.meta ? <span className="hp-meta">{it.meta}</span> : null}
          </div>
          <h3>{it.title}</h3>
          <p>{it.body}</p>
        </article>
      ))}
    </div>
  );
}

export function MiniTable({ headers, rows }: { headers: string[]; rows: string[][] }) {
  return (
    <div className="hp-table-wrap">
      <table className="hp-table">
        <thead>
          <tr>
            {headers.map((h) => (
              <th key={h}>{h}</th>
            ))}
          </tr>
        </thead>
        <tbody>
          {rows.map((r, i) => (
            <tr key={i}>
              {r.map((c, j) => (
                <td key={j}>
                  {c.includes("`") ? (
                    <span dangerouslySetInnerHTML={{ __html: c.replace(/`([^`]+)`/g, "<code>$1</code>") }} />
                  ) : (
                    c
                  )}
                </td>
              ))}
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

export function Section({
  id,
  title,
  children,
}: {
  id?: string;
  title: string;
  children: ReactNode;
}) {
  return (
    <section className="hp-block" id={id} data-help-sec={id || undefined}>
      <h2>{title}</h2>
      {children}
    </section>
  );
}

export function FlowRow({ children }: { children: ReactNode }) {
  return <div className="hp-flow">{children}</div>;
}

export function Node({
  title,
  sub,
  accent = "muted",
  pulse,
}: {
  title: string;
  sub?: string;
  accent?: "host" | "car" | "ui" | "muted";
  pulse?: boolean;
}) {
  return (
    <div className={`hp-node hp-node-${accent}${pulse ? " pulse" : ""}`}>
      <strong>{title}</strong>
      {sub ? <span>{sub}</span> : null}
    </div>
  );
}

export function FlowArrow({ label, animated }: { label?: string; animated?: boolean }) {
  return (
    <div className={`hp-arrow${animated ? " anim" : ""}`}>
      <span className="hp-arrow-track">
        <span className="hp-arrow-dot" />
      </span>
      {label ? <span className="hp-arrow-label">{label}</span> : null}
    </div>
  );
}

export function SeqDiagram({
  rows,
}: {
  rows: { from: string; fromKind: "car" | "host" | "ui"; msg: string; to: string; toKind: "car" | "host" | "ui" }[];
}) {
  return (
    <div className="hp-seq">
      {rows.map((r, i) => (
        <div key={i} className="hp-seq-row" style={{ animationDelay: `${i * 80}ms` }}>
          <span className={`hp-who ${r.fromKind}`}>{r.from}</span>
          <div className="hp-seq-mid">
            <span className="hp-seq-line" />
            <span className="hp-seq-bubble">{r.msg}</span>
          </div>
          <span className={`hp-who ${r.toKind}`}>{r.to}</span>
        </div>
      ))}
    </div>
  );
}

export function Pipeline({ stages }: { stages: string[] }) {
  return (
    <div className="hp-pipeline" aria-hidden={false}>
      {stages.map((s, i) => (
        <div key={s} className="hp-pipe-stage" style={{ animationDelay: `${i * 90}ms` }}>
          {i > 0 ? <span className="hp-pipe-link" /> : null}
          <span className="hp-pipe-chip">{s}</span>
        </div>
      ))}
    </div>
  );
}

/** 发现示意：中心车 beacon 波纹 + 多 Host 接收脉冲。 */
export function BeaconViz() {
  return (
    <div className="hp-beacon-viz" aria-hidden>
      <div className="hp-beacon-car">
        <span className="hp-beacon-core" />
        <span className="hp-beacon-ring r1" />
        <span className="hp-beacon-ring r2" />
        <span className="hp-beacon-ring r3" />
        <span className="hp-beacon-label">车 beacon</span>
      </div>
      <div className="hp-beacon-hosts">
        {["Host A", "Host B", "Host C"].map((h, i) => (
          <div key={h} className="hp-beacon-host" style={{ animationDelay: `${0.4 + i * 0.35}s` }}>
            <span className="hp-beacon-recv" />
            {h}
          </div>
        ))}
      </div>
      <p className="hp-beacon-cap">广播 → 局域网所有 Host :15999（连上后仍持续）</p>
    </div>
  );
}

/** 车上 host FIFO：队首收数据面，follower 跟队首。 */
export function QueueViz() {
  return (
    <div className="hp-queue" aria-hidden>
      <div className="hp-queue-car">
        <strong>车 host 队列</strong>
        <div className="hp-queue-slots">
          <div className="hp-queue-slot head">
            <span>队首</span>
            <em>Host A</em>
            <small>收 plot / 发 head_alive</small>
          </div>
          <div className="hp-queue-slot">
            <span>#1</span>
            <em>Host B</em>
            <small>follower · 跟队首拉流</small>
          </div>
          <div className="hp-queue-slot">
            <span>#2</span>
            <em>Host C</em>
            <small>follower</small>
          </div>
        </div>
      </div>
      <div className="hp-queue-note">数据面只向队首单播一份；列表「离线」不清此队列</div>
    </div>
  );
}
