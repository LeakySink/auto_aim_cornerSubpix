#!/usr/bin/env python3
"""
Remote Debugger - Python frontend server
Serves the web UI and streams data from the C++ UDP backend via SSE.

Usage: python3 server.py [--backend PATH] [--port PORT] [--udp-port PORT]
"""

import http.server
import json
import os
import queue
import socketserver
import subprocess
import sys
import threading
import time

# ── HTML frontend (single-page app) ──────────────────────────────────────────

HTML_PAGE = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Remote Debugger</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js"
  onerror="var s=document.createElement('script');s.src='/chart.js';document.head.appendChild(s);s.onerror=function(){document.body.innerHTML='<h1 style=color:red;text-align:center;padding-top:40vh>Chart.js failed to load.<br>Check your network or run:<br><code>python3 server.py --download-chartjs</code></h1>'}"></script>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{display:flex;height:100vh;font-family:'Consolas','Courier New',monospace;background:#0f0f14;color:#c8c8d0;overflow:hidden;font-size:14px}

#sidebar{width:210px;flex-shrink:0;background:#101018;border-right:2px solid #252530;display:flex;flex-direction:column;overflow-y:auto;overflow-x:hidden;transition:width 0.15s}
#sidebar.collapsed{width:28px}
#sidebar.collapsed .sidebar-content,#sidebar.collapsed .sidebar-header{display:none}
.sidebar-header{padding:8px 10px;font-size:13px;font-weight:bold;color:#888;border-bottom:1px solid #202030;flex-shrink:0;display:flex;justify-content:space-between;align-items:center}
.sidebar-content{flex:1;overflow-y:auto;padding-bottom:8px}
.section{padding:6px 10px;border-bottom:1px solid #1a1a26}
.section-title{font-size:11px;color:#606070;margin-bottom:4px;text-transform:uppercase;letter-spacing:0.5px}
.section label{display:block;font-size:12px;padding:2px 0;cursor:pointer;color:#a0a0b0}
.section label:hover{color:#d0d0d0}
.section input[type=radio],.section input[type=checkbox]{margin-right:5px;accent-color:#4a6a8a}
.section input[type=number],.section select{width:100%;background:#1a1a26;color:#c8c8d0;border:1px solid #303040;border-radius:3px;padding:3px 6px;font:inherit;font-size:12px;margin-top:2px}
#field-list label{display:flex;align-items:center;gap:4px;font-size:12px;padding:1px 0}
#field-list .dot{width:8px;height:8px;border-radius:50%;flex-shrink:0}
#sidebar-toggle{position:sticky;top:0;z-index:1;background:#181820;border:none;color:#707080;cursor:pointer;font-size:16px;padding:4px 0;width:100%;text-align:center;border-bottom:1px solid #202030;flex-shrink:0}
#sidebar-toggle:hover{color:#c8c8d0;background:#20202a}

#main-area{flex:1;position:relative;min-width:0}

#top-bar{position:absolute;top:0;left:0;right:0;height:38px;background:#14141c;border-bottom:2px solid #252530;display:flex;align-items:center;gap:12px;padding:0 12px;font-size:14px;z-index:10}
#top-bar .dot{width:8px;height:8px;border-radius:50%;background:#40c040;flex-shrink:0}
#top-bar .dot.dead{background:#c04040}
#top-bar select,#top-bar button{background:#1e1e2a;color:#c8c8d0;border:1px solid #333;border-radius:4px;padding:3px 8px;font:inherit;cursor:pointer}
#top-bar button:hover{background:#2a2a3a}

#mid-zone{position:absolute;top:38px;left:0;right:0}
#img-panel{position:absolute;top:0;left:0;bottom:0;display:flex;flex-direction:column;background:#14141c}
#img-box{flex:1;display:flex;align-items:center;justify-content:center;padding:8px;overflow:hidden}
#img-box img{max-width:100%;max-height:100%;object-fit:contain;border-radius:4px}
#img-box .placeholder{color:#404050;font-size:17px}
#img-info{text-align:center;color:#505060;font-size:13px;padding:0 0 6px 0;flex-shrink:0}

#v-split{position:absolute;top:0;bottom:0;width:4px;background:#252530;cursor:col-resize;z-index:5}
#v-split:hover,#v-split.active{background:#4a6a8a}

#log-panel{position:absolute;top:0;right:0;bottom:0;display:flex;flex-direction:column;background:#0c0c12}
#log-toolbar{display:flex;gap:6px;padding:4px 10px;background:#14141c;border-bottom:1px solid #252530;font-size:13px;flex-shrink:0}
#log-toolbar span{cursor:pointer;padding:2px 8px;border-radius:3px;color:#707080}
#log-toolbar span.on{background:#252535;color:#c8c8d0}
#log-list{flex:1;overflow-y:auto;padding:4px 0;font-size:13px;line-height:1.5}
.log-line{padding:1px 10px;white-space:nowrap;display:flex;gap:8px}
.log-ts{color:#404050;flex-shrink:0}
.log-lv{flex-shrink:0;font-weight:bold;min-width:40px}
.log-lv.INFO{color:#80b0e0}
.log-lv.WARN,.log-lv.WARNING{color:#e0a040}
.log-lv.ERROR,.log-lv.FATAL{color:#e04a4a}
.log-lv.DEBUG{color:#505060}
.log-msg{overflow:hidden;text-overflow:ellipsis}

#h-split{position:absolute;left:0;right:0;height:4px;background:#252530;cursor:row-resize;z-index:5}
#h-split:hover,#h-split.active{background:#4a6a8a}

#chart-panel{position:absolute;left:0;right:0;bottom:0;padding:6px 10px 4px 10px}
#chart-panel canvas{width:100%!important;height:100%!important}
</style>
</head>
<body>

<div id="sidebar">
  <button id="sidebar-toggle" title="Toggle sidebar">◀</button>
  <div class="sidebar-header">Settings</div>
  <div class="sidebar-content">
    <div class="section">
      <div class="section-title">Display Mode</div>
      <label><input type="radio" name="mode" value="sliding" checked onchange="updateChartView()"> Sliding</label>
      <label><input type="radio" name="mode" value="centered" onchange="updateChartView()"> Centered</label>
      <label><input type="radio" name="mode" value="paused" onchange="updateChartView()"> Paused</label>
    </div>
    <div class="section">
      <div class="section-title">Window (s)</div>
      <input type="number" id="win-size" value="10" min="1" max="120" step="1" onchange="updateChartView()">
    </div>
    <div class="section">
      <div class="section-title">History</div>
      <select id="history-sel" onchange="trimData()">
        <option value="30">30 s</option>
        <option value="60" selected>1 min</option>
        <option value="120">2 min</option>
        <option value="300">5 min</option>
      </select>
    </div>
    <div class="section">
      <div class="section-title">Fields <span style="float:right;cursor:pointer;color:#4a6a8a" onclick="toggleAllFields(true)">all</span> / <span style="cursor:pointer;color:#4a6a8a" onclick="toggleAllFields(false)">none</span></div>
      <div id="field-list"><span style="color:#404050;font-size:11px">waiting for data…</span></div>
    </div>
  </div>
</div>

<div id="main-area">

<div id="top-bar">
  <div class="dot" id="dot"></div>
  <span id="status">Connected</span>
  <span id="stats" style="color:#505060">0 pkt/s</span>
  <span style="flex:1"></span>
  <button onclick="clearPlots()">Clear</button>
</div>

<div id="mid-zone">
  <div id="img-panel">
    <div id="img-box">
      <span class="placeholder" id="no-img">Waiting for image…</span>
      <img id="img" alt="" style="display:none">
    </div>
    <div id="img-info"></div>
  </div>
  <div id="v-split"></div>
  <div id="log-panel">
    <div id="log-toolbar">
      <span class="on" data-lv="ALL">All</span>
      <span data-lv="INFO">Info</span>
      <span data-lv="WARN">Warn</span>
      <span data-lv="ERROR">Error</span>
      <span data-lv="DEBUG">Debug</span>
      <span style="flex:1"></span>
      <span id="log-count" style="color:#505060">0</span>
    </div>
    <div id="log-list"></div>
  </div>
</div>

<div id="h-split"></div>

<div id="chart-panel">
  <canvas id="plot-canvas"></canvas>
</div>

</div>

<script>
// ── Sidebar toggle ────────────────────────────────────────────────────────────
const sidebar = document.getElementById('sidebar');
const toggleBtn = document.getElementById('sidebar-toggle');
toggleBtn.addEventListener('click', () => {
  sidebar.classList.toggle('collapsed');
  toggleBtn.textContent = sidebar.classList.contains('collapsed') ? '▶' : '◀';
  setTimeout(applyLayout, 200);
});

// ── Layout (draggable splitters) ──────────────────────────────────────────────
const mainArea = document.getElementById('main-area');
const topBar = document.getElementById('top-bar');
const midZone = document.getElementById('mid-zone');
const imgPanel = document.getElementById('img-panel');
const vSplit = document.getElementById('v-split');
const logPanel = document.getElementById('log-panel');
const hSplit = document.getElementById('h-split');
const chartPanel = document.getElementById('chart-panel');

let midRatio = 0.35, imgRatio = 0.38;

function applyLayout() {
  const H = mainArea.clientHeight;
  const W = mainArea.clientWidth;
  if (H === 0 || W === 0) return;
  const topH = topBar.offsetHeight;
  const availH = H - topH;
  const midH = Math.floor(availH * midRatio);
  const chartH = availH - midH - 4;

  midZone.style.height = midH + 'px';
  hSplit.style.top = (topH + midH) + 'px';
  chartPanel.style.top = (topH + midH + 4) + 'px';
  chartPanel.style.height = chartH + 'px';

  const imgW = Math.floor(W * imgRatio);
  imgPanel.style.width = imgW + 'px';
  vSplit.style.left = imgW + 'px';
  logPanel.style.left = (imgW + 4) + 'px';
}

window.addEventListener('resize', applyLayout);
new ResizeObserver(applyLayout).observe(mainArea);

let dragging = null, dragStart = 0, dragVal = 0;
vSplit.addEventListener('mousedown', function(e) {
  dragging = 'v'; dragStart = e.clientX; dragVal = imgRatio;
  vSplit.classList.add('active'); e.preventDefault();
});
hSplit.addEventListener('mousedown', function(e) {
  dragging = 'h'; dragStart = e.clientY; dragVal = midRatio;
  hSplit.classList.add('active'); e.preventDefault();
});
document.addEventListener('mousemove', function(e) {
  if (!dragging) return;
  if (dragging === 'v') {
    const dx = e.clientX - dragStart;
    imgRatio = Math.min(0.75, Math.max(0.15, dragVal + dx / mainArea.clientWidth));
    applyLayout();
  } else if (dragging === 'h') {
    const dy = e.clientY - dragStart;
    midRatio = Math.min(0.7, Math.max(0.12, dragVal + dy / (mainArea.clientHeight - topBar.offsetHeight)));
    applyLayout();
  }
});
document.addEventListener('mouseup', function() {
  if (dragging) { vSplit.classList.remove('active'); hSplit.classList.remove('active'); dragging = null; }
});
applyLayout();

// ── Chart ────────────────────────────────────────────────────────────────────
const ctx = document.getElementById('plot-canvas').getContext('2d');
const chart = new Chart(ctx, {
  type: 'line', data: { datasets: [] },
  options: {
    responsive: true, maintainAspectRatio: false, animation: false,
    interaction: { mode: 'nearest', intersect: false },
    plugins: {
      legend: { display: true, labels: { color: '#a0a0b0', usePointStyle: true, pointStyleWidth:8, boxHeight:8, font:{size:11}, padding:6 } },
    },
    scales: {
      x: { type: 'linear', title:{display:true,text:'time (s)',color:'#606070',font:{size:12}}, ticks:{color:'#505060',maxTicksLimit:10,font:{size:11}}, grid:{color:'#202030'}, min:0, max:10 },
      y: { title:{display:true,text:'value',color:'#606070',font:{size:12}}, ticks:{color:'#505060',font:{size:11}}, grid:{color:'#202030'} }
    }
  }
});

const COLORS = ['#4fc3f7','#ffb74d','#81c784','#e57373','#ba68c8','#4dd0e1','#fff176','#a1887f','#90a4ae','#f48fb1','#ef5350','#26c6da','#7e57c2','#66bb6a','#ff7043'];
let fieldMeta = {}, colorIdx = 0, xField = 'ts', firstTs = null;
let allData = [];  // [{x, fields: {name: val}}]
const MAX_PTS = 50000;

function getColor() { const c = COLORS[colorIdx % COLORS.length]; colorIdx++; return c; }

function ensureDataset(field) {
  if (fieldMeta[field]) return fieldMeta[field];
  const color = getColor();
  const ds = { label: field, data: [], borderColor: color, borderWidth: 1.8, pointRadius: 0, spanGaps: false, hidden: false };
  chart.data.datasets.push(ds);
  const meta = { datasetIndex: chart.data.datasets.length - 1, color };
  fieldMeta[field] = meta;

  // add checkbox to sidebar
  const fl = document.getElementById('field-list');
  if (fl.querySelector('.placeholder')) fl.innerHTML = '';
  const lb = document.createElement('label');
  lb.innerHTML = `<span class="dot" style="background:${color}"></span>${field}`;
  const cb = document.createElement('input');
  cb.type = 'checkbox'; cb.checked = true;
  cb.onchange = function() {
    const ds = chart.data.datasets[fieldMeta[field].datasetIndex];
    ds.hidden = !this.checked;
    chart.update('none');
  };
  lb.prepend(cb);
  fl.appendChild(lb);

  return meta;
}

function toggleAllFields(show) {
  for (const [name, meta] of Object.entries(fieldMeta)) {
    chart.data.datasets[meta.datasetIndex].hidden = !show;
  }
  document.querySelectorAll('#field-list input[type=checkbox]').forEach(cb => cb.checked = show);
  chart.update('none');
}

function addPoint(ts, data) {
  let xv;
  if (xField === 'ts') { if (firstTs === null) firstTs = ts; xv = (ts - firstTs) / 1e9; }
  else if (data[xField] !== undefined) xv = data[xField];
  else xv = (allData.length > 0 ? allData[allData.length - 1].x + 0.02 : 0);

  const entry = { x: xv, fields: {} };
  for (const [key, val] of Object.entries(data)) {
    if (key === xField || typeof val !== 'number') continue;
    entry.fields[key] = val;
    ensureDataset(key);
  }
  allData.push(entry);
  while (allData.length > MAX_PTS) allData.shift();

  // pad unseen fields
  for (const name of Object.keys(fieldMeta)) {
    if (!(name in entry.fields)) entry.fields[name] = NaN;
  }

  rebuildChartData();
  trimData();
  updateChartView();
}

function rebuildChartData() {
  for (const [name, meta] of Object.entries(fieldMeta)) {
    const ds = chart.data.datasets[meta.datasetIndex];
    ds.data.length = 0;
    for (const pt of allData) {
      ds.data.push({ x: pt.x, y: pt.fields[name] });
    }
  }
}

function trimData() {
  const history = parseFloat(document.getElementById('history-sel').value);
  if (allData.length === 0) return;
  const cutoff = allData[allData.length - 1].x - history;
  while (allData.length > 1 && allData[0].x < cutoff) allData.shift();
  rebuildChartData();
  updateChartView();
}

function updateChartView() {
  if (allData.length === 0) return;
  const mode = document.querySelector('input[name="mode"]:checked').value;
  const win = parseFloat(document.getElementById('win-size').value) || 10;
  const latest = allData[allData.length - 1].x;

  if (mode === 'paused') {
    // don't change axis, let user pan/zoom
  } else if (mode === 'centered') {
    chart.options.scales.x.min = latest - win / 2;
    chart.options.scales.x.max = latest + win / 2;
  } else { // sliding
    chart.options.scales.x.min = Math.max(0, latest - win);
    chart.options.scales.x.max = Math.max(win, latest);
  }
  chart.update('none');
}

function clearPlots() {
  chart.data.datasets = [];
  fieldMeta = {};
  allData = [];
  firstTs = null;
  document.getElementById('field-list').innerHTML = '<span style="color:#404050;font-size:11px">waiting for data…</span>';
  chart.update();
}

// ── Image ────────────────────────────────────────────────────────────────────
const imgEl = document.getElementById('img'), noImg = document.getElementById('no-img'), imgInfo = document.getElementById('img-info');
function setImage(b64) {
  imgEl.src = 'data:image/jpeg;base64,' + b64;
  imgEl.style.display = ''; noImg.style.display = 'none';
  imgInfo.textContent = (b64.length * 0.75 / 1024).toFixed(0) + ' KB';
}

// ── Log ──────────────────────────────────────────────────────────────────────
const logList = document.getElementById('log-list'), logCountEl = document.getElementById('log-count');
let logFilter = 'ALL', logEntries = 0;
const MAX_LOG = 500;
document.getElementById('log-toolbar').addEventListener('click', (e) => {
  if (e.target.tagName === 'SPAN' && e.target.dataset.lv) {
    document.querySelectorAll('#log-toolbar span').forEach(s => s.classList.remove('on'));
    e.target.classList.add('on');
    logFilter = e.target.dataset.lv;
    for (const c of logList.children) {
      const lv = c.dataset.level;
      c.style.display = (logFilter === 'ALL' || logFilter === lv || (logFilter === 'WARN' && (lv === 'WARN' || lv === 'WARNING'))) ? '' : 'none';
    }
  }
});
function addLog(ts, level, msg) {
  const div = document.createElement('div');
  div.className = 'log-line'; div.dataset.level = level;
  const t = new Date(ts / 1e6);
  const tsStr = t.toTimeString().slice(0,8) + '.' + String(t.getMilliseconds()).padStart(3,'0');
  div.innerHTML = `<span class="log-ts">${tsStr}</span><span class="log-lv ${level}">${level}</span><span class="log-msg">${msg}</span>`;
  if (logFilter !== 'ALL' && logFilter !== level && !(logFilter === 'WARN' && (level === 'WARN' || level === 'WARNING')))
    div.style.display = 'none';
  logList.appendChild(div); logEntries++;
  while (logList.children.length > MAX_LOG) { logList.firstChild.remove(); logEntries--; }
  logCountEl.textContent = logEntries;
  if (logList.scrollTop + logList.clientHeight >= logList.scrollHeight - 30) logList.scrollTop = logList.scrollHeight;
}

// ── SSE ──────────────────────────────────────────────────────────────────────
const dot = document.getElementById('dot'), stats = document.getElementById('stats');
let pktCount = 0, lastPktTime = Date.now();
const es = new EventSource('/events');
es.onmessage = (e) => {
  try {
    const msg = JSON.parse(e.data);
    pktCount++;
    const now = Date.now();
    if (now - lastPktTime >= 1000) {
      stats.textContent = Math.round(pktCount * 1000 / (now - lastPktTime)) + ' pkt/s';
      pktCount = 0; lastPktTime = now;
    }
    if (msg.type === 'plot') addPoint(msg.ts, msg.data || {});
    else if (msg.type === 'image') setImage(msg.jpg_b64);
    else if (msg.type === 'log') addLog(msg.ts, msg.level, msg.msg);
    dot.className = 'dot';
  } catch (err) {}
};
es.onerror = () => { dot.className = 'dot dead'; };

// zoom/pan resets to paused mode
chart.options.plugins.zoom = null; // not using zoom plugin
chart.options.onHover = null;
const origPan = chart.pan;
</script>
</body>
</html>"""

# ── Thread-safe queue for SSE messages ──────────────────────────────────────

class SSEQueue:
    def __init__(self, maxsize=4096):
        self._q = queue.Queue(maxsize=maxsize)

    def put(self, line):
        try:
            self._q.put_nowait(line)
        except queue.Full:
            try:
                self._q.get_nowait()
            except queue.Empty:
                pass
            self._q.put_nowait(line)

    def get(self, timeout=0.5):
        try:
            return self._q.get(timeout=timeout)
        except queue.Empty:
            return None


sse_queue = SSEQueue()


# ── Subprocess reader thread ─────────────────────────────────────────────────

def reader_thread(proc):
    for line in proc.stdout:
        line = line.strip()
        if line:
            sse_queue.put(line)
    print("[server] backend process ended", file=sys.stderr)


# ── HTTP request handler ─────────────────────────────────────────────────────

class ThreadingHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass  # suppress access logs

    def do_GET(self):
        if self.path == '/':
            self._serve_html()
        elif self.path == '/events':
            self._serve_sse()
        elif self.path == '/chart.js':
            self._serve_chartjs()
        else:
            self.send_error(404)

    def _serve_html(self):
        body = HTML_PAGE.encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-cache')
        self.end_headers()
        self.wfile.write(body)

    def _serve_chartjs(self):
        chart_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'chart.umd.min.js')
        if os.path.exists(chart_path):
            with open(chart_path, 'rb') as f:
                body = f.read()
            self.send_response(200)
            self.send_header('Content-Type', 'application/javascript')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_error(404, 'chart.js not found locally, use --download-chartjs')

    def _serve_sse(self):
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.send_header('Cache-Control', 'no-cache')
        self.send_header('Connection', 'keep-alive')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        try:
            while True:
                line = sse_queue.get(timeout=1.0)
                if line:
                    self.wfile.write(f"data: {line}\n\n".encode())
                    self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass


# ── Main ─────────────────────────────────────────────────────────────────────

def main():
    import argparse
    p = argparse.ArgumentParser(description='Remote Debugger Frontend')
    p.add_argument('--backend', default='./udp_backend', help='path to C++ backend binary')
    p.add_argument('--port', type=int, default=8080, help='HTTP server port')
    p.add_argument('--udp-port', type=int, default=9871, help='UDP listen port for backend')
    p.add_argument('--download-chartjs', action='store_true', help='download Chart.js locally for offline use')
    args = p.parse_args()

    script_dir = os.path.dirname(os.path.abspath(__file__))

    if args.download_chartjs:
        import urllib.request
        url = 'https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js'
        dst = os.path.join(script_dir, 'chart.umd.min.js')
        print(f"[server] downloading {url} ...", file=sys.stderr)
        urllib.request.urlretrieve(url, dst)
        print(f"[server] saved to {dst} ({os.path.getsize(dst)} bytes)", file=sys.stderr)
        return

    backend_path = os.path.join(script_dir, args.backend)
    if not os.path.exists(backend_path):
        alt = os.path.join(script_dir, 'build', 'udp_backend')
        if os.path.exists(alt):
            backend_path = alt
        else:
            print(f"[server] backend not found at {backend_path}", file=sys.stderr)
            sys.exit(1)

    print(f"[server] starting backend: {backend_path} --port {args.udp_port}", file=sys.stderr)
    proc = subprocess.Popen(
        [backend_path, '--port', str(args.udp_port)],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        bufsize=1,
    )

    t = threading.Thread(target=reader_thread, args=(proc,), daemon=True)
    t.start()

    try:
        server = ThreadingHTTPServer(('0.0.0.0', args.port), Handler)
    except OSError as e:
        print(f"[server] cannot bind port {args.port}: {e}", file=sys.stderr)
        print(f"[server] try: python3 server.py --port {args.port + 1}", file=sys.stderr)
        proc.terminate()
        proc.wait(timeout=3)
        sys.exit(1)

    print(f"[server]  http://localhost:{args.port}   (open this in browser)", file=sys.stderr)
    print(f"[server]  UDP listening on port {args.udp_port}", file=sys.stderr)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[server] shutting down...", file=sys.stderr)
    finally:
        server.server_close()
        proc.terminate()
        proc.wait(timeout=3)


if __name__ == '__main__':
    main()
