import { useEffect, useRef } from "react";
import * as THREE from "three";
import { OrbitControls } from "three/examples/jsm/controls/OrbitControls.js";
import type { FrameStore } from "./FrameStore";
import type { MarkerItem } from "./markerTypes";

export type DisplayMarker = MarkerItem & { _skipped?: boolean };

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

function colorOf(c: number[]): THREE.Color {
  return new THREE.Color(c[0] ?? 1, c[1] ?? 1, c[2] ?? 1);
}

function opacityOf(c: number[]) {
  return c[3] ?? 1;
}

function setPose(obj: THREE.Object3D, p: number[], q: number[]) {
  obj.position.set(p[0], p[1], p[2]);
  obj.quaternion.set(q[1], q[2], q[3], q[0]); // three: x,y,z,w ; ours: w,x,y,z
  obj.updateMatrixWorld(true);
}

function buildArrow(dir: number[], shaftLen: number, color: number[], key: string): THREE.Group {
  const g = new THREE.Group();
  g.name = key;
  const d = new THREE.Vector3(dir[0], dir[1], dir[2]);
  if (d.lengthSq() < 1e-12) d.set(1, 0, 0);
  d.normalize();
  const len = Math.max(0.01, shaftLen);
  const headLen = Math.min(len * 0.25, 0.08);
  const shaft = len - headLen;
  const col = colorOf(color);
  const op = opacityOf(color);
  const mat = new THREE.MeshBasicMaterial({
    color: col,
    transparent: op < 1,
    opacity: op,
  });
  const cyl = new THREE.Mesh(new THREE.CylinderGeometry(0.008, 0.008, shaft, 8), mat);
  cyl.position.y = shaft / 2;
  const cone = new THREE.Mesh(new THREE.ConeGeometry(0.02, headLen, 12), mat.clone());
  cone.position.y = shaft + headLen / 2;
  g.add(cyl);
  g.add(cone);
  // local +Y → dir
  const quat = new THREE.Quaternion().setFromUnitVectors(new THREE.Vector3(0, 1, 0), d);
  g.quaternion.copy(quat);
  return g;
}

function buildItem(m: MarkerItem): THREE.Object3D | null {
  const key = `${m.ns}/${m.id}`;
  const op = opacityOf(m.color);
  const col = colorOf(m.color);
  if (m.type === "sphere") {
    const r = m.scale[0] || 0.04;
    const mesh = new THREE.Mesh(
      new THREE.SphereGeometry(r, 16, 12),
      new THREE.MeshBasicMaterial({ color: col, transparent: op < 1, opacity: op })
    );
    setPose(mesh, m.pose.p, m.pose.q);
    mesh.name = key;
    return mesh;
  }
  if (m.type === "box") {
    const geo = new THREE.BoxGeometry(m.scale[0] || 0.01, m.scale[1] || 0.1, m.scale[2] || 0.05);
    const mesh = new THREE.Mesh(
      geo,
      new THREE.MeshBasicMaterial({ color: col, transparent: op < 1, opacity: op })
    );
    setPose(mesh, m.pose.p, m.pose.q);
    mesh.name = key;
    return mesh;
  }
  if (m.type === "arrow") {
    const dir = m.dir || [1, 0, 0];
    const shaft = m.shaft_len ?? m.scale[0] ?? 0.2;
    const g = buildArrow(dir, shaft, m.color, key);
    g.position.set(m.pose.p[0], m.pose.p[1], m.pose.p[2]);
    return g;
  }
  if (m.type === "line_list" && m.points && m.points.length >= 2) {
    const g = new THREE.Group();
    g.name = key;
    const mat = new THREE.LineBasicMaterial({
      color: col,
      transparent: op < 1,
      opacity: op,
    });
    for (let i = 0; i + 1 < m.points.length; i += 2) {
      const a = m.points[i];
      const b = m.points[i + 1];
      const geo = new THREE.BufferGeometry().setFromPoints([
        new THREE.Vector3(a[0], a[1], a[2]),
        new THREE.Vector3(b[0], b[1], b[2]),
      ]);
      g.add(new THREE.Line(geo, mat));
    }
    return g;
  }
  if (m.type === "axes") {
    const len = m.scale[0] || 0.1;
    const g = makeAxes(len);
    setPose(g, m.pose.p, m.pose.q);
    g.name = key;
    return g;
  }
  return null;
}

/** Transform markers into display_frame using FrameStore; skip if no transform. */
export function projectMarkers(
  items: MarkerItem[],
  displayFrame: string,
  store: FrameStore
): { drawn: MarkerItem[]; skipped: number } {
  const drawn: MarkerItem[] = [];
  let skipped = 0;
  const df = displayFrame || "world";
  for (const m of items) {
    const src = m.frame_id || "world";
    if (src === df) {
      drawn.push(m);
      continue;
    }
    const pose = store.transformPose(src, df, m.pose.p, m.pose.q);
    if (!pose) {
      skipped += 1;
      continue;
    }
    const copy: MarkerItem = {
      ...m,
      frame_id: df,
      pose: { p: pose.p, q: pose.q },
    };
    if (m.dir) {
      const d = store.transformDir(src, df, m.dir);
      if (!d) {
        skipped += 1;
        continue;
      }
      copy.dir = d;
    }
    if (m.points) {
      const pts: [number, number, number][] = [];
      let ok = true;
      for (const pt of m.points) {
        const tp = store.transformPose(src, df, pt);
        if (!tp) {
          ok = false;
          break;
        }
        pts.push(tp.p);
      }
      if (!ok) {
        skipped += 1;
        continue;
      }
      copy.points = pts;
    }
    drawn.push(copy);
  }
  return { drawn, skipped };
}

export function MarkerScene({
  items,
  skippedCount = 0,
}: {
  items: MarkerItem[];
  skippedCount?: number;
}) {
  const hostRef = useRef<HTMLDivElement>(null);
  const itemsRef = useRef(items);
  const rootRef = useRef<THREE.Group | null>(null);
  itemsRef.current = items;

  useEffect(() => {
    const host = hostRef.current;
    if (!host) return;

    const scene = new THREE.Scene();
    scene.background = new THREE.Color(0x111217);

    const camera = new THREE.PerspectiveCamera(50, 1, 0.01, 100);
    camera.up.set(0, 0, 1);
    camera.position.set(1.2, -1.6, 0.9);

    const renderer = new THREE.WebGLRenderer({ antialias: true });
    renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
    host.appendChild(renderer.domElement);

    const controls = new OrbitControls(camera, renderer.domElement);
    controls.enableDamping = true;
    controls.target.set(0, 0, 0.2);

    const grid = new THREE.GridHelper(4, 40, 0x333844, 0x22252e);
    grid.rotation.x = Math.PI / 2;
    scene.add(grid);
    scene.add(makeAxes(0.25));

    const root = new THREE.Group();
    scene.add(root);
    rootRef.current = root;

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
    let lastSig = "";
    const tick = () => {
      raf = requestAnimationFrame(tick);
      const list = itemsRef.current;
      const sig = list.map((m) => `${m.ns}/${m.id}:${m.pose.p.join(",")}`).join("|");
      if (sig !== lastSig) {
        lastSig = sig;
        while (root.children.length) {
          const ch = root.children[0];
          root.remove(ch);
          ch.traverse((o) => {
            const mesh = o as THREE.Mesh;
            if (mesh.geometry) mesh.geometry.dispose();
            const mat = mesh.material as THREE.Material | THREE.Material[] | undefined;
            if (Array.isArray(mat)) mat.forEach((x) => x.dispose());
            else mat?.dispose();
          });
        }
        for (const m of list) {
          const obj = buildItem(m);
          if (obj) root.add(obj);
        }
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
      if (renderer.domElement.parentNode === host) host.removeChild(renderer.domElement);
      rootRef.current = null;
    };
  }, []);

  return (
    <div className="marker-pane">
      <div className="marker-canvas" ref={hostRef} />
      {skippedCount > 0 && (
        <div className="marker-hud mono muted">{skippedCount} 条缺变换，未绘制</div>
      )}
      {items.length === 0 && skippedCount === 0 && (
        <div className="marker-empty mono muted">等待 Markers…</div>
      )}
    </div>
  );
}
