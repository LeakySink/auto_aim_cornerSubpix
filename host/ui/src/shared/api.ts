/**
 * Hub 全局 HTTP 封装（非实例业务 API）。
 * 完整契约见 `host/API.md`；实例内路径用 `useInstance().base` + getJson/postJson。
 */

async function req<T = unknown>(url: string, init?: RequestInit): Promise<T> {
  const r = await fetch(url, {
    ...init,
    headers: {
      "Content-Type": "application/json",
      ...(init?.headers || {}),
    },
  });
  if (!r.ok) {
    const t = await r.text();
    throw new Error(`${r.status} ${t.slice(0, 200)}`);
  }
  return r.json() as Promise<T>;
}

/** GET /api/features — Feature 种类（KINDS），不含 help。 */
export function listFeatures() {
  return req<{ features: import("./types").FeatureMeta[] }>("/api/features");
}

/** GET /api/instances — 当前打开的页面/线程。 */
export function listInstances() {
  return req<{ instances: Array<import("./types").FeatureStatus & { feature: string; instance: string; title: string }> }>(
    "/api/instances"
  );
}

/**
 * POST /api/open — 新建 Feature 实例并 start。
 * @returns path 供 window.open（标定可能是 /calibrate.html?i=…）
 */
export function openFeature(feature: string, config: Record<string, unknown> = {}) {
  return req<{ ok: boolean; id: string; feature: string; path: string }>("/api/open", {
    method: "POST",
    body: JSON.stringify({ feature, config }),
  });
}

/** GET /api/instances/<id>/status */
export function featureStatus(id: string) {
  return req<import("./types").FeatureStatus & { feature?: string }>(`/api/instances/${id}/status`);
}

/** POST /api/instances/<id>/start — 少用；一般 open 已 start。 */
export function startFeature(id: string, config: Record<string, unknown> = {}) {
  return req<import("./types").FeatureStatus>(`/api/instances/${id}/start`, {
    method: "POST",
    body: JSON.stringify(config),
  });
}

/** POST /api/instances/<id>/stop — 关页时通常再带 ?forget=1（sendBeacon）。 */
export function stopFeature(id: string) {
  return req<{ ok: boolean }>(`/api/instances/${id}/stop`, {
    method: "POST",
    body: JSON.stringify({}),
  });
}

export function postJson<T = unknown>(url: string, body: unknown) {
  return req<T>(url, { method: "POST", body: JSON.stringify(body) });
}

export function getJson<T = unknown>(url: string) {
  return req<T>(url);
}
