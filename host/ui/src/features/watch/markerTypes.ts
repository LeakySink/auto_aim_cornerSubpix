/** Watch plot.markers / marker_v1 types and merge helpers. */

export type Vec3 = [number, number, number];
export type Quat = [number, number, number, number]; // w,x,y,z
export type Rgba = [number, number, number, number];

export type MarkerType = "sphere" | "arrow" | "box" | "line_list" | "axes";

export type MarkerItem = {
  ns: string;
  id: string;
  type: MarkerType | string;
  frame_id: string;
  pose: { p: Vec3; q: Quat };
  scale: Vec3;
  color: Rgba;
  dir?: Vec3;
  shaft_len?: number;
  points?: Vec3[];
  text?: string;
};

export type MarkerArrayPayload = {
  schema: string;
  frame_id: string;
  items: MarkerItem[];
};

/** Per-ns cache entry after merge. */
export type NsLayer = {
  frame_id: string;
  items: MarkerItem[];
};

export type MarkerCache = Map<string, NsLayer>;

function isNumArr(a: unknown, n: number): a is number[] {
  return Array.isArray(a) && a.length >= n && a.slice(0, n).every((x) => typeof x === "number");
}

function asVec3(a: unknown, fallback: Vec3 = [0, 0, 0]): Vec3 {
  if (isNumArr(a, 3)) return [a[0], a[1], a[2]];
  return fallback;
}

function asQuat(a: unknown): Quat {
  if (isNumArr(a, 4)) return [a[0], a[1], a[2], a[3]];
  return [1, 0, 0, 0];
}

function asRgba(a: unknown): Rgba {
  if (isNumArr(a, 4)) return [a[0], a[1], a[2], a[3]];
  if (isNumArr(a, 3)) return [a[0], a[1], a[2], 1];
  return [1, 1, 1, 1];
}

export function parseMarkerArray(raw: unknown): MarkerArrayPayload | null {
  if (!raw || typeof raw !== "object") return null;
  const o = raw as Record<string, unknown>;
  const schema = typeof o.schema === "string" ? o.schema : "marker_v1";
  if (schema !== "marker_v1") return null;
  if (!Array.isArray(o.items)) return null;

  let frame_id = typeof o.frame_id === "string" ? o.frame_id : "world";
  if (!frame_id) frame_id = "world";

  const items: MarkerItem[] = [];
  for (const it of o.items) {
    if (!it || typeof it !== "object") continue;
    const m = it as Record<string, unknown>;
    const ns = typeof m.ns === "string" ? m.ns : "";
    const id = typeof m.id === "string" ? m.id : "";
    const type = typeof m.type === "string" ? m.type : "";
    if (!ns || !id || !type) continue;

    let mFrame = typeof m.frame_id === "string" ? m.frame_id : frame_id;
    if (!mFrame) mFrame = frame_id;

    const poseRaw = (m.pose && typeof m.pose === "object" ? m.pose : {}) as Record<string, unknown>;
    const scale = asVec3(m.scale, [1, 1, 1]);
    const item: MarkerItem = {
      ns,
      id,
      type,
      frame_id: mFrame,
      pose: { p: asVec3(poseRaw.p), q: asQuat(poseRaw.q) },
      scale,
      color: asRgba(m.color),
    };
    if (isNumArr(m.dir, 3)) item.dir = [m.dir[0], m.dir[1], m.dir[2]];
    if (typeof m.shaft_len === "number") item.shaft_len = m.shaft_len;
    if (Array.isArray(m.points)) {
      item.points = m.points
        .filter((p) => isNumArr(p, 3))
        .map((p) => [p[0], p[1], p[2]] as Vec3);
    }
    if (typeof m.text === "string") item.text = m.text;
    items.push(item);
  }

  return { schema, frame_id, items };
}

/** Merge one MarkerArray into cache: replace each ns that appears in this packet. */
export function mergeMarkers(cache: MarkerCache, arr: MarkerArrayPayload): MarkerCache {
  const next = new Map(cache);
  const byNs = new Map<string, MarkerItem[]>();
  for (const it of arr.items) {
    const list = byNs.get(it.ns) || [];
    list.push(it);
    byNs.set(it.ns, list);
  }
  for (const [ns, items] of byNs) {
    const frame_id = items[0]?.frame_id || arr.frame_id || "world";
    next.set(ns, { frame_id, items });
  }
  // Empty items with only schema: if packet has items:[] and no ns, leave cache
  // Optional markers_reset handled by caller
  return next;
}

export function flattenCache(cache: MarkerCache): MarkerItem[] {
  const out: MarkerItem[] = [];
  for (const layer of cache.values()) out.push(...layer.items);
  return out;
}

export function listNamespaces(cache: MarkerCache): string[] {
  return [...cache.keys()].sort();
}

/** Build temporary markers from flat EKF plot fields (legacy debug binaries). */
export function markersFromFlatEkf(data: Record<string, unknown>): MarkerArrayPayload | null {
  const num = (k: string) => (typeof data[k] === "number" ? (data[k] as number) : null);
  const x = num("x");
  const y = num("y");
  const z = num("z");
  if (x == null || y == null || z == null) return null;
  const vx = num("vx") ?? 0;
  const vy = num("vy") ?? 0;
  const vz = num("vz") ?? 0;
  const items: MarkerItem[] = [
    {
      ns: "kalman.center",
      id: "c",
      type: "sphere",
      frame_id: "world",
      pose: { p: [x, y, z], q: [1, 0, 0, 0] },
      scale: [0.04, 0.04, 0.04],
      color: [1, 0.45, 0.1, 1],
    },
  ];
  const vnorm = Math.hypot(vx, vy, vz);
  if (vnorm > 1e-6) {
    const shaft = Math.min(0.8, Math.max(0.05, vnorm * 0.15));
    items.push({
      ns: "kalman.vel",
      id: "v",
      type: "arrow",
      frame_id: "world",
      pose: { p: [x, y, z], q: [1, 0, 0, 0] },
      dir: [vx / vnorm, vy / vnorm, vz / vnorm],
      shaft_len: shaft,
      scale: [shaft, 1, 1],
      color: [0.2, 0.9, 0.3, 1],
    });
  }
  return { schema: "marker_v1", frame_id: "world", items };
}
