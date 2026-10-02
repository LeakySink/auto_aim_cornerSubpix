import {
  matMul3,
  matT3,
  matVec3,
  parseTf,
  quatToMat3,
  resolveFrame,
  type TfPayload,
} from "../tfviz/tfMath";

export type Vec3 = [number, number, number];
export type Quat = [number, number, number, number];

type PoseInWorld = { R: number[]; t: number[] };

const I3 = [1, 0, 0, 0, 1, 0, 0, 0, 1];

function matToQuat(R: number[]): Quat {
  // row-major → Eigen-style quaternion (w,x,y,z)
  const t = R[0] + R[4] + R[8];
  if (t > 0) {
    const s = Math.sqrt(t + 1) * 2;
    return [(s / 4), (R[7] - R[5]) / s, (R[2] - R[6]) / s, (R[3] - R[1]) / s];
  }
  if (R[0] > R[4] && R[0] > R[8]) {
    const s = Math.sqrt(1 + R[0] - R[4] - R[8]) * 2;
    return [(R[7] - R[5]) / s, s / 4, (R[1] + R[3]) / s, (R[2] + R[6]) / s];
  }
  if (R[4] > R[8]) {
    const s = Math.sqrt(1 + R[4] - R[0] - R[8]) * 2;
    return [(R[2] - R[6]) / s, (R[1] + R[3]) / s, s / 4, (R[5] + R[7]) / s];
  }
  const s = Math.sqrt(1 + R[8] - R[0] - R[4]) * 2;
  return [(R[3] - R[1]) / s, (R[2] + R[6]) / s, (R[5] + R[7]) / s, s / 4];
}

/** Absolute poses in world: frame → {R,t} with p_world = R p_frame + t */
export class FrameStore {
  private poses = new Map<string, PoseInWorld>();

  constructor() {
    this.poses.set("world", { R: [...I3], t: [0, 0, 0] });
  }

  clear() {
    this.poses.clear();
    this.poses.set("world", { R: [...I3], t: [0, 0, 0] });
  }

  knownFrames(): string[] {
    return [...this.poses.keys()].sort();
  }

  setPoseInWorld(frame: string, R: number[], t: number[]) {
    if (!frame) return;
    this.poses.set(frame, { R: [...R], t: [...t] });
  }

  /** Ingest plot.tf (tf_pub shape). */
  ingestTf(raw: unknown) {
    const tf = parseTf(raw);
    if (!tf) return;
    const f = resolveFrame(tf);
    this.setPoseInWorld("gimbal", f.R_gimbal2world, [0, 0, 0]);
    this.setPoseInWorld("camera", f.R_camera2world, f.t_camera2world);
  }

  /**
   * Ingest plot.frames:
   * { "gimbal": { "parent": "world", "R":[9], "t":[3] }, ... }
   * R,t map child→parent: p_parent = R p_child + t
   */
  ingestFrames(raw: unknown) {
    if (!raw || typeof raw !== "object") return;
    const o = raw as Record<string, unknown>;
    for (const [frame, val] of Object.entries(o)) {
      if (!val || typeof val !== "object") continue;
      const v = val as Record<string, unknown>;
      const parent = typeof v.parent === "string" ? v.parent : "world";
      const R = Array.isArray(v.R) && v.R.length === 9 && v.R.every((x) => typeof x === "number")
        ? (v.R as number[])
        : null;
      const t = Array.isArray(v.t) && v.t.length === 3 && v.t.every((x) => typeof x === "number")
        ? (v.t as number[])
        : null;
      if (!R || !t) continue;
      const parentPose = this.poses.get(parent);
      if (!parentPose) continue;
      // p_world = R_pw (R p_child + t) + t_pw = (R_pw R) p_child + R_pw t + t_pw
      const Rw = matMul3(parentPose.R, R);
      const tw = matVec3(parentPose.R, t);
      this.setPoseInWorld(frame, Rw, [tw[0] + parentPose.t[0], tw[1] + parentPose.t[1], tw[2] + parentPose.t[2]]);
    }
  }

  /** Transform pose from `from` into `to`. null if missing chain. */
  transformPose(
    from: string,
    to: string,
    p: Vec3,
    q: Quat = [1, 0, 0, 0]
  ): { p: Vec3; q: Quat } | null {
    const src = from || "world";
    const dst = to || "world";
    if (src === dst) return { p: [...p], q: [...q] };

    const Pf = this.poses.get(src);
    const Pt = this.poses.get(dst);
    if (!Pf || !Pt) return null;

    // p_world = Rf p + tf
    const pw = matVec3(Pf.R, p);
    const pWorld: Vec3 = [pw[0] + Pf.t[0], pw[1] + Pf.t[1], pw[2] + Pf.t[2]];
    // p_to = Rt^T (p_world - tt)
    const RtT = matT3(Pt.R);
    const diff: Vec3 = [pWorld[0] - Pt.t[0], pWorld[1] - Pt.t[1], pWorld[2] - Pt.t[2]];
    const pTo = matVec3(RtT, diff) as Vec3;

    // R_world_from = Rf * R(q); R_to_from = Rt^T * R_world_from
    const Rq = quatToMat3(q);
    const Rw = matMul3(Pf.R, Rq);
    const Rto = matMul3(RtT, Rw);
    const qTo = matToQuat(Rto);
    return { p: [pTo[0], pTo[1], pTo[2]], q: qTo };
  }

  /** Transform a direction vector (no translation). */
  transformDir(from: string, to: string, dir: Vec3): Vec3 | null {
    const r = this.transformPose(from, to, [0, 0, 0], [1, 0, 0, 0]);
    if (!r) return null;
    // Use rotation only via two point transforms of origin and tip
    const tip = this.transformPose(from, to, dir, [1, 0, 0, 0]);
    if (!tip) return null;
    return [tip.p[0] - r.p[0], tip.p[1] - r.p[1], tip.p[2] - r.p[2]];
  }
}

export function ingestTfPayload(store: FrameStore, tf: TfPayload | unknown) {
  store.ingestTf(tf);
}
