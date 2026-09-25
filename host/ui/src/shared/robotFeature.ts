/** Beacon `app` → portal feature. Missing / unknown → watch. */
export function featureFromApp(app?: string): "calibrate" | "watch" {
  const a = (app || "normal").trim().toLowerCase() || "normal";
  if (a === "calibrate") return "calibrate";
  return "watch";
}
