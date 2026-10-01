import { useEffect, useRef } from "react";
import * as THREE from "three";
import { OrbitControls } from "three/examples/jsm/controls/OrbitControls.js";
import type { TfFrame } from "./tfMath";

function applyPose(obj: THREE.Object3D, R: number[], t: number[]) {
  // Eigen row-major R; Three.js matrix.elements is column-major.
  const e = obj.matrix.elements;
  e[0] = R[0];
  e[1] = R[3];
  e[2] = R[6];
  e[3] = 0;
  e[4] = R[1];
  e[5] = R[4];
  e[6] = R[7];
  e[7] = 0;
  e[8] = R[2];
  e[9] = R[5];
  e[10] = R[8];
  e[11] = 0;
  e[12] = t[0];
  e[13] = t[1];
  e[14] = t[2];
  e[15] = 1;
  obj.matrixAutoUpdate = false;
  obj.updateMatrixWorld(true);
}

function makeAxes(length: number): THREE.Group {
  const g = new THREE.Group();
  const mk = (dir: THREE.Vector3, color: number) => {
    const geo = new THREE.BufferGeometry().setFromPoints([
      new THREE.Vector3(0, 0, 0),
      dir.clone().multiplyScalar(length),
    ]);
    return new THREE.Line(geo, new THREE.LineBasicMaterial({ color }));
  };
  g.add(mk(new THREE.Vector3(1, 0, 0), 0xe74c3c));
  g.add(mk(new THREE.Vector3(0, 1, 0), 0x2ecc71));
  g.add(mk(new THREE.Vector3(0, 0, 1), 0x3498db));
  return g;
}

/** ConeGeometry 默认沿 +Y；转到沿 localAxis，底在原点附近、尖端朝向。 */
function makeAimCone(
  radius: number,
  height: number,
  localAxis: "x" | "z",
  color: number,
  opts?: { wireframe?: boolean; opacity?: number }
): THREE.Mesh {
  const geo = new THREE.ConeGeometry(radius, height, 24);
  const mat = new THREE.MeshBasicMaterial({
    color,
    wireframe: !!opts?.wireframe,
    transparent: (opts?.opacity ?? 1) < 1,
    opacity: opts?.opacity ?? 1,
  });
  const mesh = new THREE.Mesh(geo, mat);
  // default tip at +Y → rotate onto +X or +Z
  if (localAxis === "x") {
    mesh.rotation.z = -Math.PI / 2;
    mesh.position.x = height / 2;
  } else {
    mesh.rotation.x = Math.PI / 2;
    mesh.position.z = height / 2;
  }
  return mesh;
}

/** 相机：长方体机身（局部 +Z 向前）+ 圆锥光轴提示。 */
function makeCameraModel(): THREE.Group {
  const g = new THREE.Group();
  const bodyLen = 0.05; // along +Z
  const body = new THREE.Mesh(
    new THREE.BoxGeometry(0.036, 0.028, bodyLen),
    new THREE.MeshBasicMaterial({ color: 0xf1c40f, transparent: true, opacity: 0.9 })
  );
  body.position.z = bodyLen / 2;
  g.add(body);

  const coneH = 0.045;
  const lens = makeAimCone(0.016, coneH, "z", 0xe67e22, { opacity: 0.95 });
  // makeAimCone 已把中心放到 height/2；再平移到机身前端，使底贴合、尖端朝 +Z
  lens.position.z = bodyLen + coneH / 2;
  g.add(lens);

  g.add(makeAxes(0.08));
  return g;
}

/** 枪管：原点，圆锥沿云台 +X（与标定 ideal 前向一致）。 */
function makeBarrelModel(): THREE.Group {
  const g = new THREE.Group();
  const barrel = makeAimCone(0.012, 0.12, "x", 0xbdc3c7, { opacity: 0.95 });
  g.add(barrel);
  // 略粗的底座示意安装点
  const hub = new THREE.Mesh(
    new THREE.SphereGeometry(0.012, 16, 12),
    new THREE.MeshBasicMaterial({ color: 0x7f8c8d })
  );
  g.add(hub);
  g.add(makeAxes(0.1));
  return g;
}

type Props = {
  frame: TfFrame | null;
};

export function TfScene({ frame }: Props) {
  const hostRef = useRef<HTMLDivElement | null>(null);
  const frameRef = useRef<TfFrame | null>(null);
  frameRef.current = frame;

  useEffect(() => {
    const host = hostRef.current;
    if (!host) return;

    const scene = new THREE.Scene();
    scene.background = new THREE.Color(0x111217);

    const camera = new THREE.PerspectiveCamera(50, 1, 0.01, 50);
    camera.up.set(0, 0, 1);
    camera.position.set(0.55, -0.75, 0.45);

    const renderer = new THREE.WebGLRenderer({ antialias: true });
    renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
    host.appendChild(renderer.domElement);

    const controls = new OrbitControls(camera, renderer.domElement);
    controls.enableDamping = true;
    controls.target.set(0.08, 0, 0.04);

    const grid = new THREE.GridHelper(2, 20, 0x333844, 0x22252e);
    grid.rotation.x = Math.PI / 2;
    scene.add(grid);

    const worldAxes = makeAxes(0.2);
    scene.add(worldAxes);

    const barrelGroup = makeBarrelModel();
    scene.add(barrelGroup);

    const cameraGroup = makeCameraModel();
    scene.add(cameraGroup);

    const resize = () => {
      const w = host.clientWidth || 1;
      const h = host.clientHeight || 1;
      camera.aspect = w / h;
      camera.updateProjectionMatrix();
      renderer.setSize(w, h, false);
    };
    resize();
    const ro = new ResizeObserver(resize);
    ro.observe(host);

    let raf = 0;
    const tick = () => {
      raf = requestAnimationFrame(tick);
      const f = frameRef.current;
      if (f) {
        // 枪管：电控姿态，位置在原点，指向云台 +X
        applyPose(barrelGroup, f.R_gimbal2world, [0, 0, 0]);
        // 相机：外参位姿，圆锥沿相机光轴 +Z
        applyPose(cameraGroup, f.R_camera2world, f.t_camera2world);
      }
      controls.update();
      renderer.render(scene, camera);
    };
    tick();

    return () => {
      cancelAnimationFrame(raf);
      ro.disconnect();
      controls.dispose();
      renderer.dispose();
      if (renderer.domElement.parentElement === host) host.removeChild(renderer.domElement);
    };
  }, []);

  return <div className="tfviz-canvas" ref={hostRef} />;
}
