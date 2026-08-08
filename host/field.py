#!/usr/bin/env python3
"""Field Control — multi-robot monitoring grid view."""

import http.server
import json
import os
import queue
import socketserver
import sys
import threading

from backend_mgr import BackendManager

# ── HTML ─────────────────────────────────────────────────────────────────────

HTML = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Field Control</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js"
  onerror="document.body.innerHTML='Chart.js CDN failed'"></script>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{background:#0a0a10;color:#c8c8d0;font-family:monospace;padding:8px;display:flex;flex-wrap:wrap;gap:8px;min-height:100vh}
.tile{background:#12121c;border:1px solid #252530;border-radius:4px;width:380px;height:460px;display:flex;flex-direction:column;overflow:hidden}
.tile-header{background:#181820;padding:4px 8px;font-size:14px;font-weight:bold;color:#4fc3f7;flex-shrink:0;display:flex;align-items:center;gap:6px}
.tile-header .dot{width:8px;height:8px;border-radius:50%;background:#40c040;flex-shrink:0}
.tile-header .dot.dead{background:#c04040}
.tile-body{flex:1;display:flex;flex-direction:column;overflow:hidden}
.tile-img{flex:1;position:relative;overflow:hidden;min-height:0}
.tile-img img{position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);max-width:100%;max-height:100%;object-fit:contain}
.tile-plot{height:140px;flex-shrink:0}
.tile-plot canvas{width:100%!important;height:100%!important}
</style>
</head>
<body>
<div id="empty" style="position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);color:#404050;font-size:18px">等待机器人连接…</div>
<script>
const TILES = {};
const COLORS = ['#4fc3f7','#ffb74d','#81c784','#e57373','#ba68c8','#4dd0e1','#fff176','#f48fb1'];

function ensureTile(name) {
  if (TILES[name]) return TILES[name];
  var tile = document.createElement('div'); tile.className = 'tile';
  tile.innerHTML =
    '<div class="tile-header"><div class="dot"></div><span>'+name+'</span></div>'+
    '<div class="tile-body">'+
      '<div class="tile-img"></div>'+
      '<div class="tile-plot"><canvas></canvas></div>'+
    '</div>';
  document.body.appendChild(tile);

  var cvs = tile.querySelector('canvas');
  var chart = new Chart(cvs.getContext('2d'), {
    type:'line', data:{datasets:[]},
    options:{
      responsive:true, maintainAspectRatio:false, animation:false,
      plugins:{legend:{display:false}},
      scales:{
        x:{type:'linear',ticks:{color:'#505060',font:{size:10}},grid:{color:'#1a1a26'}},
        y:{ticks:{color:'#505060',font:{size:10}},grid:{color:'#1a1a26'}}
      }
    }
  });

  TILES[name] = {tile:tile, chart:chart, fields:{}, imgEl:tile.querySelector('.tile-img'),
    dot:tile.querySelector('.dot'), lastTs:0};
  return TILES[name];
}

var firstTs=null, xField='ts';

function handlePlot(msg) {
  var name = msg._from||(msg.data&&msg.data._from)||'unknown';
  var t = ensureTile(name);
  t.lastTs = Date.now(); t.dot.className = 'dot';
  var ts = msg.ts||0;
  if (firstTs===null) firstTs=ts;
  var xv = (ts-firstTs)/1e9;
  var data = msg.data||{};
  delete data._from; delete data.ts;

  for (var k in data) {
    if (typeof data[k]!=='number') continue;
    if (!t.fields[k]) {
      var ci = Object.keys(t.fields).length % COLORS.length;
      var ds = {label:k,data:[],borderColor:COLORS[ci],borderWidth:1,pointRadius:0};
      t.chart.data.datasets.push(ds);
      t.fields[k] = ds;
    }
    t.chart.data.datasets.forEach(function(ds) {
      if (ds.label===k) ds.data.push({x:xv,y:data[k]});
    });
  }
  t.chart.options.scales.x.min = Math.max(0, xv-30);
  t.chart.options.scales.x.max = Math.max(30, xv);
  t.chart.update('none');
}

function handleImage(msg) {
  var name = (msg.meta&&msg.meta._from)||'unknown';
  var t = ensureTile(name);
  t.lastTs = Date.now(); t.dot.className = 'dot';
  t.imgEl.innerHTML = '<img src="data:image/jpeg;base64,'+msg.jpg_b64+'">';
}

var es = new EventSource('/events');
es.onmessage = function(e) {
  try {
    var msg = JSON.parse(e.data);
    if (msg.type==='plot') handlePlot(msg);
    else if (msg.type==='image') handleImage(msg);
  } catch(err) {}
};

setInterval(function() {
  var now = Date.now();
  for (var n in TILES)
    TILES[n].dot.className = (now-TILES[n].lastTs > 4000) ? 'dot dead' : 'dot';
}, 2000);
</script>
</body>
</html>"""

# ── SSE Queue ────────────────────────────────────────────────────────────────

class SSEQueue:
    def __init__(self, maxsize=4096):
        self._q = queue.Queue(maxsize=maxsize)
    def put(self, line):
        try: self._q.put_nowait(line)
        except queue.Full:
            try: self._q.get_nowait()
            except queue.Empty: pass
            self._q.put_nowait(line)
    def get(self, timeout=0.5):
        try: return self._q.get(timeout=timeout)
        except queue.Empty: return None

sse_queue = SSEQueue()

# ── HTTP ─────────────────────────────────────────────────────────────────────

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, f, *a): pass
    def do_GET(self):
        if self.path == '/': self._html()
        elif self.path == '/events': self._sse()
        else: self.send_error(404)
    def _html(self):
        b = HTML.encode()
        self.send_response(200)
        self.send_header('Content-Type','text/html;charset=utf-8')
        self.send_header('Content-Length', str(len(b)))
        self.end_headers()
        self.wfile.write(b)
    def _sse(self):
        self.send_response(200)
        self.send_header('Content-Type','text/event-stream')
        self.send_header('Cache-Control','no-cache')
        self.send_header('Connection','keep-alive')
        self.send_header('Access-Control-Allow-Origin','*')
        self.end_headers()
        try:
            while True:
                line = sse_queue.get(timeout=1.0)
                if line:
                    self.wfile.write(f"data: {line}\n\n".encode())
                    self.wfile.flush()
        except Exception: pass

class ThreadingHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True

# ── Main ─────────────────────────────────────────────────────────────────────

def main():
    import argparse
    p = argparse.ArgumentParser(description='Field Control')
    p.add_argument('--backend', default='build/udp_backend', help='path to udp_backend')
    p.add_argument('--port', type=int, default=8888, help='HTTP port')
    p.add_argument('--data-port', type=int, default=20000, help='UDP data port')
    args = p.parse_args()

    script_dir = os.path.dirname(os.path.abspath(__file__))
    binary = os.path.join(script_dir, args.backend)
    if not os.path.exists(binary):
        alt = os.path.join(script_dir, 'build', 'udp_backend')
        if os.path.exists(alt): binary = alt
        else:
            print(f"backend not found: {binary}", file=sys.stderr)
            sys.exit(1)

    mgr = BackendManager(binary)
    mgr.on_output = lambda line: sse_queue.put(line)
    mgr.start(args.data_port, "field")

    try:
        httpd = ThreadingHTTPServer(('0.0.0.0', args.port), Handler)
    except OSError as e:
        print(f"cannot bind {args.port}: {e}", file=sys.stderr)
        mgr.stop(); sys.exit(1)

    print(f"http://localhost:{args.port}")
    print(f"data port {args.data_port}")
    try: httpd.serve_forever()
    except KeyboardInterrupt: print("\nshutdown")
    finally: httpd.server_close(); mgr.stop()

if __name__ == '__main__':
    main()
