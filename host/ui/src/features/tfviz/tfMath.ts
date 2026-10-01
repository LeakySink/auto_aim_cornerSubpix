/** Coordinate math matching tasks/auto_aim/solver.cpp */

export type TfPayload = {
  q: number[]; // [w, x, y, z]
  R_gimbal2imubody: number[]; // row-major 9
  R_camera2gimbal: number[];
  t_camera2gimbal: number[];
  R_gimbal2world?: number[];
  R_camera2world?: number[];
  t_camera2world?: number[];
};

export type TfFrame = {
  R_gimbal2world: number[];
  R_camera2world: number[];
  t_camera2world: number[];
  q: number[];
  t_camera2gimbal: number[];
};

function isNine(a: unknown): a is number[] {
  return Array.isArray(a) && a.length === 9 && a.every((x) => typeof x === "number");
}

function isThree(a: unknown): a is number[] {
  return Array.isArray(a) && a.length === 3 && a.every((x) => typeof x === "number");
}

function isQuat(a: unknown): a is number[] {
  return Array.isArray(a) && a.length === 4 && a.every((x) => typeof x === "number");
}

/** Row-major 3x3 * 3x3 */
export function matMul3(A: number[], B: number[]): number[] {
  const C = new Array(9).fill(0);
  for (let r = 0; r < 3; r++) {
    for (let c = 0; c < 3; c++) {
      C[r * 3 + c] = A[r * 3 + 0] * B[0 * 3 + c] + A[r * 3 + 1] * B[1 * 3 + c] + A[r * 3 + 2] * B[2 * 3 + c];
    }
  }
  return C;
}

export function matT3(A: number[]): number[] {
  return [A[0], A[3], A[6], A[1], A[4], A[7], A[2], A[5], A[8]];
}

export function matVec3(R: number[], v: number[]): number[] {
  return [
    R[0] * v[0] + R[1] * v[1] + R[2] * v[2],
    R[3] * v[0] + R[4] * v[1] + R[5] * v[2],
    R[6] * v[0] + R[7] * v[1] + R[8] * v[2],
  ];
}

/** Eigen Quaterniond(w,x,y,z) → row-major rotation matrix */
export function quatToMat3(q: number[]): number[] {
  const [w, x, y, z] = q;
  const n = Math.hypot(w, x, y, z) || 1;
  const W = w / n;
  const X = x / n;
  const Y = y / n;
  const Z = z / n;
  return [
    1 - 2 * (Y * Y + Z * Z),
    2 * (X * Y - W * Z),
    2 * (X * Z + W * Y),
    2 * (X * Y + W * Z),
    1 - 2 * (X * X + Z * Z),
    2 * (Y * Z - W * X),
    2 * (X * Z - W * Y),
    2 * (Y * Z + W * X),
    1 - 2 * (X * X + Y * Y),
  ];
}

/**
 * Same as Solver::set_R_gimbal2world:
 * R_g2w = R_g2imu^T * R_imuabs * R_g2imu
 */
export function computeRGimbal2World(q: number[], R_gimbal2imubody: number[]): number[] {
  const R_imuabs = quatToMat3(q);
  return matMul3(matMul3(matT3(R_gimbal2imubody), R_imuabs), R_gimbal2imubody);
}

export function parseTf(raw: unknown): TfPayload | null {
  if (!raw || typeof raw !== "object") return null;
  const o = raw as Record<string, unknown>;
  if (!isQuat(o.q) || !isNine(o.R_gimbal2imubody) || !isNine(o.R_camera2gimbal) || !isThree(o.t_camera2gimbal)) {
    return null;
  }
  return {
    q: o.q,
    R_gimbal2imubody: o.R_gimbal2imubody,
    R_camera2gimbal: o.R_camera2gimbal,
    t_camera2gimbal: o.t_camera2gimbal,
    R_gimbal2world: isNine(o.R_gimbal2world) ? o.R_gimbal2world : undefined,
    R_camera2world: isNine(o.R_camera2world) ? o.R_camera2world : undefined,
    t_camera2world: isThree(o.t_camera2world) ? o.t_camera2world : undefined,
  };
}

/** Host-side recompute (must match car). Prefer local math; car fields are cross-check. */
export function resolveFrame(tf: TfPayload): TfFrame {
  const R_gimbal2world = computeRGimbal2World(tf.q, tf.R_gimbal2imubody);
  const R_camera2world = matMul3(R_gimbal2world, tf.R_camera2gimbal);
  const t_camera2world = matVec3(R_gimbal2world, tf.t_camera2gimbal);
  return {
    R_gimbal2world,
    R_camera2world,
    t_camera2world,
    q: tf.q,
    t_camera2gimbal: tf.t_camera2gimbal,
  };
}

export function fmtVec(v: number[], digits = 3): string {
  return v.map((x) => x.toFixed(digits)).join(", ");
}
