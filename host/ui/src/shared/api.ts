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

export function listFeatures() {
  return req<{ features: import("./types").FeatureMeta[] }>("/api/features");
}

export function listInstances() {
  return req<{ instances: Array<import("./types").FeatureStatus & { feature: string; instance: string; title: string }> }>(
    "/api/instances"
  );
}

export function openFeature(feature: string, config: Record<string, unknown> = {}) {
  return req<{ ok: boolean; id: string; feature: string; path: string }>("/api/open", {
    method: "POST",
    body: JSON.stringify({ feature, config }),
  });
}

export function featureStatus(id: string) {
  return req<import("./types").FeatureStatus & { feature?: string }>(`/api/instances/${id}/status`);
}

export function startFeature(id: string, config: Record<string, unknown> = {}) {
  return req<import("./types").FeatureStatus>(`/api/instances/${id}/start`, {
    method: "POST",
    body: JSON.stringify(config),
  });
}

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
