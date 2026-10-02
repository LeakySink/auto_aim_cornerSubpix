import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useRef,
  useState,
  type ReactNode,
} from "react";
import { createPortal } from "react-dom";

export type AlertKind = "error" | "warn" | "info";

export type AlertRequest = {
  kind?: AlertKind;
  title: string;
  message?: string;
  /** 主按钮文案，默认「知道了」 */
  confirmLabel?: string;
  /** 点遮罩 / Esc 是否可关；error 默认 false，其余 true */
  dismissible?: boolean;
};

type AlertItem = AlertRequest & { id: number; kind: AlertKind };

type AlertApi = {
  show: (req: AlertRequest) => void;
  error: (title: string, message?: string) => void;
  warn: (title: string, message?: string) => void;
  info: (title: string, message?: string) => void;
};

const AlertCtx = createContext<AlertApi | null>(null);

let _id = 1;
let _external: AlertApi | null = null;

/** 非 React 上下文也可调用（模块级）。Provider 挂载前会排队，挂载后刷出。 */
const _pending: AlertRequest[] = [];

export const alertModal: AlertApi = {
  show(req) {
    if (_external) _external.show(req);
    else _pending.push(req);
  },
  error(title, message) {
    alertModal.show({ kind: "error", title, message, dismissible: false });
  },
  warn(title, message) {
    alertModal.show({ kind: "warn", title, message });
  },
  info(title, message) {
    alertModal.show({ kind: "info", title, message });
  },
};

export function useAlert(): AlertApi {
  const api = useContext(AlertCtx);
  if (!api) throw new Error("useAlert must be used under AlertProvider");
  return api;
}

function DialogCard({
  item,
  onClose,
}: {
  item: AlertItem;
  onClose: () => void;
}) {
  const confirmRef = useRef<HTMLButtonElement | null>(null);
  const dismissible = item.dismissible ?? item.kind !== "error";

  useEffect(() => {
    confirmRef.current?.focus();
  }, [item.id]);

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape" && dismissible) onClose();
      if (e.key === "Enter") onClose();
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [dismissible, onClose]);

  const kindLabel =
    item.kind === "error" ? "错误" : item.kind === "warn" ? "警告" : "提示";

  return (
    <div
      className="dialog-backdrop"
      role="presentation"
      onMouseDown={(e) => {
        if (e.target === e.currentTarget && dismissible) onClose();
      }}
    >
      <div
        className={`dialog-card dialog-${item.kind}`}
        role="alertdialog"
        aria-modal="true"
        aria-labelledby={`dialog-title-${item.id}`}
        aria-describedby={item.message ? `dialog-body-${item.id}` : undefined}
      >
        <div className="dialog-head">
          <span className={`dialog-kind k-${item.kind}`}>{kindLabel}</span>
          <h2 id={`dialog-title-${item.id}`} className="dialog-title">
            {item.title}
          </h2>
        </div>
        {item.message && (
          <p id={`dialog-body-${item.id}`} className="dialog-body mono">
            {item.message}
          </p>
        )}
        <div className="dialog-actions">
          <button type="button" ref={confirmRef} onClick={onClose}>
            {item.confirmLabel || "知道了"}
          </button>
        </div>
      </div>
    </div>
  );
}

export function AlertProvider({ children }: { children: ReactNode }) {
  const [queue, setQueue] = useState<AlertItem[]>([]);
  const current = queue[0] || null;

  const show = useCallback((req: AlertRequest) => {
    const kind = req.kind || "info";
    const item: AlertItem = {
      ...req,
      id: _id++,
      kind,
      dismissible: req.dismissible ?? kind !== "error",
    };
    setQueue((q) => [...q, item]);
  }, []);

  const api = useMemo<AlertApi>(
    () => ({
      show,
      error: (title, message) => show({ kind: "error", title, message, dismissible: false }),
      warn: (title, message) => show({ kind: "warn", title, message }),
      info: (title, message) => show({ kind: "info", title, message }),
    }),
    [show]
  );

  useEffect(() => {
    _external = api;
    if (_pending.length) {
      const batch = _pending.splice(0, _pending.length);
      batch.forEach((r) => api.show(r));
    }
    return () => {
      if (_external === api) _external = null;
    };
  }, [api]);

  const close = useCallback(() => {
    setQueue((q) => q.slice(1));
  }, []);

  return (
    <AlertCtx.Provider value={api}>
      {children}
      {current &&
        createPortal(<DialogCard item={current} onClose={close} />, document.body)}
    </AlertCtx.Provider>
  );
}
