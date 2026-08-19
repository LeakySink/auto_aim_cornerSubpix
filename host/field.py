#!/usr/bin/env python3
"""Field Control — multi-robot monitoring grid view. Stdlib only."""

import http.server
import queue
import socketserver
import sys

import assets
from control import ControlServer
from udp_rx import UdpBackend

HTML = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Field Control</title>
<script src="/chart.js"></script>
<script src="/hammer.js"></script>
<script src="/chartjs-plugin-zoom.js"></script>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{background:#0e0e16;color:#c8c8d0;font-family:monospace;display:flex;gap:4px;padding:4px;height:100vh;overflow:hidden}
#sidebar{width:180px;flex-shrink:0;background:#101018;border-right:2px solid #252530;overflow-y:auto;padding:8px}
#sidebar h3{color:#707080;font-size:12px;margin-bottom:4px;text-transform:uppercase;border-bottom:1px solid #202030;padding-bottom:3px}
#sidebar label{display:flex;align-items:center;gap:4px;font-size:13px;padding:2px 0;color:#a0a0b0;cursor:pointer}
#sidebar label:hover{color:#d0d0d0}
#sidebar input[type=checkbox]{accent-color:#4a6a8a}
#tiles{flex:1;display:flex;flex-wrap:wrap;gap:4px;align-content:flex-start;overflow-y:auto}
.tile{background:#14141c;border:1px solid #252530;border-radius:4px;flex:1 1 320px;min-width:300px;height:calc(100vh - 12px);display:flex;flex-direction:column;overflow:hidden}
.tile.active{border-color:#4fc3f7}
.tile-header{background:#181820;padding:2px 6px;font-size:13px;font-weight:bold;color:#4fc3f7;flex-shrink:0;display:flex;align-items:center;gap:4px;height:24px;cursor:pointer}
.tile-header .dot{width:7px;height:7px;border-radius:50%;background:#40c040;flex-shrink:0}
.tile-header .dot.dead{background:#c04040}
.tile-header select{background:#1a1a26;color:#c8c8d0;border:1px solid #303040;border-radius:2px;font:inherit;font-size:11px;max-width:110px;margin-left:4px}
.tile-header .fps{color:#606070;font-size:11px;margin-left:4px;flex-shrink:0}
.tile-body{flex:1;display:flex;flex-direction:column;overflow:hidden;position:relative}
.tile-img{flex:1;position:relative;overflow:hidden;min-height:40px}
.tile-img img{position:absolute;top:50%;left:50%;max-width:100%;max-height:100%;object-fit:contain}
.tile-split{height:4px;flex-shrink:0;background:#303040;cursor:row-resize}.tile-split:hover{background:#4a6a8a}
.tile-plot{flex:1;min-height:40px;position:relative;overflow:hidden}
.tile-plot canvas{width:100%!important;height:100%!important}
.tile-legend{position:absolute;top:2px;right:2px;z-index:2;background:#181820cc;border:1px solid #303040;border-radius:3px;padding:2px 5px;max-height:60%;overflow-y:auto;font-size:10px}
.tile-legend .item{display:flex;align-items:center;gap:3px;cursor:pointer;padding:1px 0;white-space:nowrap}.tile-legend .item:hover{color:#d0d0d0}
.tile-legend .box{width:8px;height:8px;border:1px solid #fff;flex-shrink:0;border-radius:1px}
.tile-legend .box.on{background:var(--c)}.tile-legend .box.off{background:transparent}
</style>
</head>
<body>
<div id="sidebar">
  <h3>显示模式</h3>
  <label><input type="radio" name="fmode" value="slide" checked onchange="onModeChange()"> 滑动</label>
  <label><input type="radio" name="fmode" value="pause" onchange="onModeChange()"> 暂停</label>
  <h3>窗口 (秒)</h3>
  <input type="number" id="fwin" value="30" min="5" max="120" onchange="onModeChange()" style="width:100%;background:#1a1a26;color:#c8c8d0;border:1px solid #303040;border-radius:3px;padding:2px 6px;font:inherit">
  <button onclick="resetActiveView()" style="margin-top:4px;width:100%;background:#1e1e2a;color:#c8c8d0;border:1px solid #333;border-radius:3px;padding:2px;font:inherit;cursor:pointer">重置视角</button>
  <h3>数据字段</h3>
  <div id="field-list"><span style="color:#505060;font-size:12px">等待数据…</span></div>
</div>
<div id="tiles"></div>

<script>
const TILES={},COLORS=['#4fc3f7','#ffb74d','#81c784','#e57373','#ba68c8','#4dd0e1','#fff176','#f48fb1'];
var firstTs=null,activeTile=null,allFields={};

function onModeChange() {
  var t=TILES[activeTile];if(!t||!t.chart)return;
  t.manualView=false;if(t.chart.resetZoom)t.chart.resetZoom();t.chart.update('none');
}

function resetActiveView() {
  var t=TILES[activeTile];if(!t||!t.chart)return;
  t.manualView=false;if(t.chart.resetZoom)t.chart.resetZoom();
  t.imgS=1;t.imgTx=0;t.imgTy=0;
  var img=t.imgEl.querySelector('img');if(img)applyImgTrans(t,img);
  t.chart.update('none');
}

function activateTile(name) {
  if (activeTile===name) { rebuildActiveSidebar(); return; }
  var old=TILES[activeTile];
  if (old) {
    var cbs=document.querySelectorAll('#field-list input');
    for (var i=0;i<cbs.length;i++) old.subFields[cbs[i].value]=cbs[i].checked;
    old.tile.classList.remove('active');
  }
  activeTile=name;
  var t=TILES[name]; if(!t) return;
  t.tile.classList.add('active');
  rebuildActiveSidebar();
}

function rebuildActiveSidebar() {
  var t=TILES[activeTile]; if(!t) return;
  var fl=document.getElementById('field-list');fl.innerHTML='';
  if(!Object.keys(allFields).length){fl.innerHTML='<span style=\"color:#505060;font-size:12px\">等待数据…</span>';return}
  for (var k in allFields) {
    var lb=document.createElement('label'),cb=document.createElement('input');
    cb.type='checkbox';cb.value=k;cb.checked=t.subFields[k]!==false;
    cb.onchange=function(){t.subFields[this.value]=this.checked;syncChart(t,name);};
    lb.appendChild(cb);lb.appendChild(document.createTextNode(' '+k));fl.appendChild(lb);
  }
}

function syncChart(t,name) {
  var keep={};
  for (var k in allFields) if(t.subFields[k]) keep[k]=true;
  for (var i=t.chart.data.datasets.length-1;i>=0;i--)
    if(!keep[t.chart.data.datasets[i].label]){t.chart.data.datasets.splice(i,1);delete t.fields[t.chart.data.datasets[i].label];}
  for (var k in keep) if(!t.fields[k]){
    var ds={label:k,data:[],borderColor:COLORS[Object.keys(t.fields).length%COLORS.length],borderWidth:1,pointRadius:0};
    t.fields[k]=ds;t.chart.data.datasets.push(ds);
  }
  t.chart.update('none');renderLegend(name);
}

function renderLegend(name) {
  var t=TILES[name];if(!t)return;t.legendDiv.innerHTML='';
  for (var k in t.fields) {
    var ds=t.fields[k],idx=-1;
    for (var i=0;i<t.chart.data.datasets.length;i++) if(t.chart.data.datasets[i]===ds){idx=i;break}
    var vis=idx>=0?t.chart.isDatasetVisible(idx):true;
    var item=document.createElement('div');item.className='item';
    var box=document.createElement('div');
    box.className='box '+(vis?'on':'off');box.style.setProperty('--c',ds.borderColor);
    item.appendChild(box);item.appendChild(document.createTextNode(k));
    item.setAttribute('data-idx',idx);item.setAttribute('data-name',name);
    item.onclick=function(){
      var i=parseInt(this.getAttribute('data-idx')),n=this.getAttribute('data-name');
      if(i>=0){var tt=TILES[n];tt.chart.setDatasetVisibility(i,!tt.chart.isDatasetVisible(i));tt.chart.update();renderLegend(n);}
    };
    t.legendDiv.appendChild(item);
  }
}

function applyImgTrans(t,img) {
  img.style.transform='translate(-50%,-50%) translate('+t.imgTx.toFixed(1)+'px,'+t.imgTy.toFixed(1)+'px) scale('+t.imgS.toFixed(3)+')';
}

function showImg(name) {
  var t=TILES[name];if(!t)return;
  var sel=t.imgSel.value||Object.keys(t.imgSources)[0];if(sel)t.imgSel.value=sel;
  var s=t.imgSources[sel];if(s)t.imgEl.innerHTML='<img src="'+s.b64+'">';
  var img=t.imgEl.querySelector('img');if(img)applyImgTrans(t,img);
}

function refreshImgOpts(name) {
  var t=TILES[name];if(!t)return;
  var cur=t.imgSel.value,keys=Object.keys(t.imgSources);
  t.imgSel.innerHTML='';if(!keys.length){t.imgSel.innerHTML='<option>--</option>';return}
  for(var i=0;i<keys.length;i++){var o=document.createElement('option');o.value=keys[i];o.textContent=keys[i];t.imgSel.appendChild(o);}
  if(cur&&t.imgSources[cur])t.imgSel.value=cur;else t.imgSel.value=keys[0];
  showImg(name);
}

function ensureTile(name) {
  if (TILES[name]) return TILES[name];
  var tile=document.createElement('div');tile.className='tile';
  tile.innerHTML=
    '<div class="tile-header"><div class="dot"></div><span>'+name+'</span>'+
    '<select class="img-sel" onchange="showImg(\''+name+'\')"></select>'+
    '<span class="fps">0 fps</span></div>'+
    '<div class="tile-body"><div class="tile-img"></div>'+
    '<div class="tile-split"></div>'+
    '<div class="tile-plot"><canvas></canvas><div class="tile-legend"></div></div></div>';
  document.getElementById('tiles').appendChild(tile);

  var chart=new Chart(tile.querySelector('canvas').getContext('2d'),{type:'line',data:{datasets:[]},
    options:{responsive:true,maintainAspectRatio:false,animation:false,
      plugins:{legend:{display:false},
        zoom:{zoom:{wheel:{enabled:true},pinch:{enabled:true},mode:'x',overScaleMode:'y',
              onZoomStart:function(){if(TILES[name])TILES[name].manualView=true;}},
              pan:{enabled:true,mode:'x',overScaleMode:'y',
              onPanStart:function(){if(TILES[name])TILES[name].manualView=true;}},
              limits:{x:{min:0}}}},
      scales:{x:{type:'linear',ticks:{color:'#505060',font:{size:9}},grid:{color:'#1a1a26'}},y:{ticks:{color:'#505060',font:{size:9}},grid:{color:'#1a1a26'}}}
    }
  });

  var imgDiv=tile.querySelector('.tile-img'), plotDiv=tile.querySelector('.tile-plot');
  var t={tile:tile,chart:chart,fields:{},subFields:{},imgEl:imgDiv,
    imgSel:tile.querySelector('.img-sel'),fps:tile.querySelector('.fps'),
    legendDiv:tile.querySelector('.tile-legend'),dot:tile.querySelector('.dot'),
    lastTs:0,imgSources:{},imgFps:{},imgS:1,imgTx:0,imgTy:0,imgDrag:false,manualView:false};
  for (var k in allFields) t.subFields[k]=true;

  // header click to activate
  tile.querySelector('.tile-header').onclick=function(e){if(e.target.tagName!=='SELECT')activateTile(name);};
  // body click to activate
  tile.querySelector('.tile-body').onclick=function(){activateTile(name);};
  // splitter drag
  var split=tile.querySelector('.tile-split');
  split.onmousedown=function(e){e.preventDefault();
    var sy=e.clientY,sh1=imgDiv.offsetHeight,sh2=plotDiv.offsetHeight;
    function mv(ev){var dy=ev.clientY-sy;imgDiv.style.flex='0 0 '+(Math.max(40,sh1+dy))+'px';plotDiv.style.flex='0 0 '+(Math.max(40,sh2-dy))+'px';chart.resize();}
    function up(){document.removeEventListener('mousemove',mv);document.removeEventListener('mouseup',up);}
    document.addEventListener('mousemove',mv);document.addEventListener('mouseup',up);
  };
  // image zoom/pan
  imgDiv.addEventListener('wheel',function(e){e.preventDefault();
    var s0=t.imgS;t.imgS*=e.deltaY>0?0.9:1.1;if(t.imgS<0.1)t.imgS=0.1;if(t.imgS>15)t.imgS=15;
    var r=imgDiv.getBoundingClientRect(),mx=e.clientX-r.left-r.width/2,my=e.clientY-r.top-r.height/2;
    t.imgTx=mx+(t.imgTx-mx)*t.imgS/s0;t.imgTy=my+(t.imgTy-my)*t.imgS/s0;
    var img=t.imgEl.querySelector('img');if(img)applyImgTrans(t,img);
  });
  imgDiv.addEventListener('mousedown',function(e){if(e.button!==0)return;t.imgDrag=true;t.imgDx=e.clientX-t.imgTx;t.imgDy=e.clientY-t.imgTy;imgDiv.style.cursor='grabbing';});
  window.addEventListener('mousemove',function(e){if(!t.imgDrag)return;t.imgTx=e.clientX-t.imgDx;t.imgTy=e.clientY-t.imgDy;var img=t.imgEl.querySelector('img');if(img)applyImgTrans(t,img);});
  window.addEventListener('mouseup',function(){if(t.imgDrag){t.imgDrag=false;imgDiv.style.cursor='';}});

  TILES[name]=t;
  if (!activeTile) activateTile(name);
  return t;
}

function handlePlot(msg) {
  var name=msg._from||(msg.data&&msg.data._from)||'';if(!name)return;
  var t=ensureTile(name);t.lastTs=Date.now();t.dot.className='dot';
  var ts=msg.ts||0;if(firstTs===null)firstTs=ts;var xv=(ts-firstTs)/1e9,data=msg.data||{};
  for(var k in data){
    if(k==='_from'||k==='ts'||typeof data[k]!=='number')continue;
    if(!allFields[k]){allFields[k]=true;for(var n in TILES)TILES[n].subFields[k]=true;
      rebuildActiveSidebar();}
    if(!t.subFields[k])continue;
    if(!t.fields[k]){var ds={label:k,data:[],borderColor:COLORS[Object.keys(t.fields).length%COLORS.length],borderWidth:1,pointRadius:0};t.fields[k]=ds;t.chart.data.datasets.push(ds);}
  }
  t.chart.data.datasets.forEach(function(ds){var yv=data[ds.label];ds.data.push({x:xv,y:yv!==undefined?yv:NaN});while(ds.data.length>1&&ds.data[0].x<xv-120)ds.data.shift();if(ds.data.length>10000)ds.data.splice(0,ds.data.length-10000);});
  var mode=document.querySelector('input[name=\"fmode\"]:checked');
  if(!t.manualView&&(!mode||mode.value!=='pause')){
    var win=parseFloat(document.getElementById('fwin').value)||30;
    t.chart.options.scales.x.min=Math.max(0,xv-win);
    t.chart.options.scales.x.max=Math.max(win,xv);
  }
  t.chart.update('none');renderLegend(name);
}

function handleImage(msg) {
  var name=(msg.meta&&msg.meta._from)||'';if(!name)return;
  var t=ensureTile(name);t.lastTs=Date.now();t.dot.className='dot';
  var iname=(msg.meta&&msg.meta.name)||'default',now=Date.now();
  if(!t.imgFps[iname])t.imgFps[iname]=[];
  t.imgFps[iname].push(now);while(t.imgFps[iname].length>10)t.imgFps[iname].shift();
  var fps=0;if(t.imgFps[iname].length>=2)fps=Math.round((t.imgFps[iname].length-1)*1000/(now-t.imgFps[iname][0]));
  t.imgSources[iname]={b64:'data:image/jpeg;base64,'+msg.jpg_b64};t.fps.textContent=fps+' fps';
  refreshImgOpts(name);
}

var es=new EventSource('/events');
es.onmessage=function(e){try{var m=JSON.parse(e.data);if(m.type==='plot')handlePlot(m);else if(m.type==='image')handleImage(m);}catch(err){}}
setInterval(function(){var now=Date.now();for(var n in TILES)TILES[n].dot.className=(now-TILES[n].lastTs>4000)?'dot dead':'dot';},2000);
</script>
</body>
</html>"""

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

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, f, *a): pass
    def do_GET(self):
        if self.path == '/': self._html()
        elif self.path == '/events': self._sse()
        elif assets.try_serve(self, self.path): return
        else: self.send_error(404)
    def _html(self):
        b = HTML.encode()
        self.send_response(200);self.send_header('Content-Type','text/html;charset=utf-8')
        self.send_header('Content-Length',str(len(b)));self.end_headers();self.wfile.write(b)
    def _sse(self):
        self.send_response(200);self.send_header('Content-Type','text/event-stream')
        self.send_header('Cache-Control','no-cache');self.send_header('Connection','keep-alive')
        self.send_header('Access-Control-Allow-Origin','*');self.end_headers()
        try:
            while True:
                line = sse_queue.get(timeout=1.0)
                if line: self.wfile.write(f"data: {line}\n\n".encode());self.wfile.flush()
        except Exception: pass

class ThreadingHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True

def main():
    import argparse
    p = argparse.ArgumentParser(description='Field Control')
    p.add_argument('--port', type=int, default=8888)
    p.add_argument('--data-port', type=int, default=20000)
    p.add_argument('--ctrl-port', type=int, default=15000)
    p.add_argument('--download-assets', action='store_true',
                   help='download Chart.js / Hammer / zoom plugin for offline use')
    args = p.parse_args()
    if args.download_assets:
        assets.download_all()
        return
    cs = ControlServer(port=args.ctrl_port, data_port_start=args.data_port,
                       data_port_end=args.data_port, reuse_ports=True)
    cs.start()
    mgr = UdpBackend()
    mgr.on_output = lambda line: sse_queue.put(line)
    mgr.start(args.data_port, "field")
    try:
        httpd = ThreadingHTTPServer(('0.0.0.0', args.port), Handler)
    except OSError as e:
        print(f"bind error {e}", file=sys.stderr)
        mgr.stop()
        cs.stop()
        sys.exit(1)
    print(f"http://localhost:{args.port}  data:{args.data_port}  ctrl:{args.ctrl_port}")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nshutdown")
    finally:
        httpd.server_close()
        mgr.stop()
        cs.stop()

if __name__ == '__main__':
    main()
