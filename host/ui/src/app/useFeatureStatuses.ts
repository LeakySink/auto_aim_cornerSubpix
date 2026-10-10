import { useEffect, useState } from "react";
import { featureStatus } from "../shared/api";
import { FEATURE_MODULES } from "../features/registry";
import type { FeatureStatus } from "../features/types";

export function useFeatureStatuses(intervalMs = 4000) {
  const [map, setMap] = useState<Record<string, FeatureStatus>>({});

  useEffect(() => {
    let dead = false;
    const tick = async () => {
      const next: Record<string, FeatureStatus> = {};
      await Promise.all(
        FEATURE_MODULES.map(async (m) => {
          try {
            next[m.id] = await featureStatus(m.id);
          } catch {
            /* hub down */
          }
        })
      );
      if (!dead) setMap(next);
    };
    tick();
    const t = setInterval(tick, intervalMs);
    return () => {
      dead = true;
      clearInterval(t);
    };
  }, [intervalMs]);

  return map;
}
