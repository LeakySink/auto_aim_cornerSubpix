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

function makeAxes(length: number, lineWidthHint = 2): THREE.Group {
  const g = new THREE.Group();
  const mk = (dir: THREE.Vector3, color: number) => {
    const geo = new THREE.BufferGeometry().setFromPoints([
      new THREE.Vector3(0, 0, 0),
      dir.clone().multiplyScalar(length),
    ]);
    const mat = new THREE.LineBasicMaterial({ color, linewidth: lineWidthHint });
    return new THREE.Line(geo, mat);
  };
  g.add(mk(new THREE.Vector3(1, 0, 0), 0xe74c3c)); // X red
  g.add(mk(new THREE.Vector3(0, 1, 0), 0x2ecc71)); // Y green
  g.add(mk(new THREE.Vector3(0, 0, 1), 0x3498db)); // Z blue
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
    camera.up.set(0, 0, 1); // robotics Z-up
    camera.position.set(0.6, -0.8, 0.5);

    const renderer = new THREE.WebGLRenderer({ antialias: true });
    renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
    host.appendChild(renderer.domElement);

    const controls = new OrbitControls(camera, renderer.domElement);
    controls.enableDamping = true;
    controls.target.set(0, 0, 0.05);

    const grid = new THREE.GridHelper(2, 20, 0x333844, 0x22252e);
    grid.rotation.x = Math.PI / 2; // XY plane, Z up
    scene.add(grid);

    const worldAxes = makeAxes(0.25);
    scene.add(worldAxes);

    const gimbalAxes = makeAxes(0.18);
    scene.add(gimbalAxes);

    const cameraGroup = new THREE.Group();
    cameraGroup.add(makeAxes(0.12));
    const body = new THREE.Mesh(
      new THREE.BoxGeometry(0.04, 0.03, 0.025),
      new THREE.MeshBasicMaterial({ color: 0xf1c40f, transparent: true, opacity: 0.85 })
    );
    body.position.set(0, 0, 0);
    cameraGroup.add(body);
    // optical axis hint (+Z of camera in OpenCV often forward; here show +Z)
    const fr = new THREE.Mesh(
      new THREE.ConeGeometry(0.015, 0.04, 4),
      new THREE.MeshBasicMaterial({ color: 0xf39c12, wireframe: true })
    );
    fr.rotation.x = Math.PI / 2;
    fr.position.set(0, 0, 0.03);
    cameraGroup.add(fr);
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
        applyPose(gimbalAxes, f.R_gimbal2world, [0, 0, 0]);
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
