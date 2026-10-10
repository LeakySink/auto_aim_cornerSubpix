import { useEffect, useRef, useState, type ReactNode } from "react";

type Pane = { id: string; element: ReactNode };

/** Keep previously visited feature panes mounted (hidden) so state survives navigation. */
export function KeepAlive({
  activeId,
  panes,
}: {
  activeId: string;
  panes: Pane[];
}) {
  const [seen, setSeen] = useState<Set<string>>(() => new Set());
  const known = useRef(new Map<string, ReactNode>());

  useEffect(() => {
    if (!activeId) return;
    setSeen((prev) => {
      if (prev.has(activeId)) return prev;
      const n = new Set(prev);
      n.add(activeId);
      return n;
    });
  }, [activeId]);

  for (const p of panes) {
    known.current.set(p.id, p.element);
  }

  const ids = [...seen];
  if (activeId && !ids.includes(activeId)) ids.push(activeId);

  return (
    <div className="keepalive-host">
      {ids.map((id) => (
        <div key={id} className="ka-pane" hidden={id !== activeId}>
          {known.current.get(id)}
        </div>
      ))}
    </div>
  );
}
