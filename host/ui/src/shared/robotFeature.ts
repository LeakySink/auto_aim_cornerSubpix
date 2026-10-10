/** Beacon → portal feature. Prefer explicit `feature`, else map from `app`. */
export type PortalFeature = "calibrate" | "watch" | "tfviz";

const KNOWN: PortalFeature[] = ["calibrate", "watch", "tfviz"];

export function featureFromApp(app?: string): PortalFeature {
  const a = (app || "normal").trim().toLowerCase() || "normal";
  if (a === "calibrate") return "calibrate";
  if (a === "tfviz") return "tfviz";
  return "watch";
}

/** Resolve portal feature from robot beacon fields. */
export function resolvePortalFeature(feature?: string, app?: string): PortalFeature {
  const f = (feature || "").trim().toLowerCase();
  if ((KNOWN as string[]).includes(f)) return f as PortalFeature;
  return featureFromApp(app);
}
