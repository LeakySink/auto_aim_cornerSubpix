import { createContext, useContext, useEffect } from "react";

export type InstanceInfo = {
  id: string;
  feature: string;
  base: string;
};

const Ctx = createContext<InstanceInfo | null>(null);

export function InstanceProvider({
  value,
  children,
}: {
  value: InstanceInfo;
  children: React.ReactNode;
}) {
  useEffect(() => {
    const stop = () => {
      const url = `/api/instances/${value.id}/stop?forget=1`;
      if (navigator.sendBeacon) navigator.sendBeacon(url, "");
      else fetch(url, { method: "POST", keepalive: true }).catch(() => {});
    };
    window.addEventListener("pagehide", stop);
    return () => window.removeEventListener("pagehide", stop);
  }, [value.id]);

  return <Ctx.Provider value={value}>{children}</Ctx.Provider>;
}

export function useInstance(): InstanceInfo {
  const v = useContext(Ctx);
  if (!v) throw new Error("not in a feature page");
  return v;
}
