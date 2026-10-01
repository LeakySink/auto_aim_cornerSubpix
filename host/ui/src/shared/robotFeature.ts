/** Beacon `app` → portal feature. Missing / unknown → watch. */
export type PortalFeature = "calibrate" | "watch" | "tfviz";

export function featureFromApp(app?: string): PortalFeature {
  const a = (app || "normal").trim().toLowerCase() || "normal";
  if (a === "calibrate") return "calibrate";
  if (a === "tfviz") return "tfviz";
  return "watch";
}
