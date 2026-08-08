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
body{display:flex;height:100vh;font-family:'Consolas','Courier New',monospace;background:#0f0f14;color:#c8c8d0;overflow:hidden;font-size:16px}

#sidebar{width:230px;flex-shrink:0;background:#101018;border-right:2px solid #252530;display:flex;flex-direction:column;overflow-y:auto;overflow-x:hidden;transition:width 0.15s}
#sidebar.collapsed{width:28px}
#sidebar.collapsed .sidebar-content,#sidebar.collapsed .sidebar-header{display:none}
.sidebar-header{padding:8px 10px;font-size:15px;font-weight:bold;color:#888;border-bottom:1px solid #202030;flex-shrink:0}
.sidebar-content{flex:1;overflow-y:auto;padding-bottom:8px}
.section{padding:6px 10px;border-bottom:1px solid #1a1a26}
.section-title{font-size:13px;color:#606070;margin-bottom:4px;text-transform:uppercase;letter-spacing:0.5px}
.section label{display:block;font-size:14px;padding:2px 0;cursor:pointer;color:#a0a0b0}
.section label:hover{color:#d0d0d0}
.section input[type=radio],.section input[type=checkbox]{margin-right:5px;accent-color:#4a6a8a}
.section input[type=number],.section select{width:100%;background:#1a1a26;color:#c8c8d0;border:1px solid #303040;border-radius:3px;padding:3px 6px;font:inherit;font-size:14px;margin-top:2px}
#field-list label{display:flex;align-items:center;gap:4px;font-size:14px;padding:1px 0}
#field-list .dot{width:8px;height:8px;border-radius:50%;flex-shrink:0}
#sidebar-toggle{position:sticky;top:0;z-index:1;background:#181820;border:none;color:#707080;cursor:pointer;font-size:18px;padding:4px 0;width:100%;text-align:center;border-bottom:1px solid #202030;flex-shrink:0}
#sidebar-toggle:hover{color:#c8c8d0;background:#20202a}

#main-area{flex:1;position:relative;min-width:0;overflow:hidden}
#top-bar{position:absolute;top:0;left:0;right:0;height:40px;background:#14141c;border-bottom:2px solid #252530;display:flex;align-items:center;gap:12px;padding:0 12px;font-size:16px;z-index:10}
#top-bar .dot{width:8px;height:8px;border-radius:50%;background:#40c040;flex-shrink:0}
#top-bar .dot.dead{background:#c04040}
#top-bar button{background:#1e1e2a;color:#c8c8d0;border:1px solid #333;border-radius:4px;padding:3px 8px;font:inherit;cursor:pointer}
#top-bar button:hover{background:#2a2a3a}

.panel{position:absolute;background:#14141c;display:flex;flex-direction:column;overflow:hidden}
.panel-header{display:flex;align-items:center;gap:4px;padding:2px 6px;background:#18181f;border-bottom:1px solid #202030;font-size:13px;flex-shrink:0;height:28px}
.panel-header select{background:#1a1a26;color:#c8c8d0;border:1px solid #303040;border-radius:3px;padding:1px 4px;font:inherit;font-size:12px}
.panel-header button{background:none;border:none;color:#707080;cursor:pointer;font-size:14px;padding:0 3px;line-height:1;border-radius:2px}
.panel-header button:hover{color:#c8c8d0;background:#2a2a3a}
.panel-header .dir-btn{font-size:11px;padding:0 2px}
.panel-body{flex:1;overflow:hidden;position:relative}
.panel-body canvas{width:100%!important;height:100%!important}
.panel-body img{max-width:100%;max-height:100%;object-fit:contain;position:absolute;top:50%;left:50%;transform:translate(-50%,-50%)}
.panel-body .placeholder{position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);color:#404050;font-size:15px}
.panel-img-info{text-align:center;color:#505060;font-size:12px;padding:0 0 4px 0;flex-shrink:0}
.plot-legend{position:absolute;top:4px;right:4px;z-index:3;background:#181820cc;border:1px solid #303040;border-radius:4px;padding:3px 6px;max-height:60%;overflow-y:auto;pointer-events:auto;user-select:none}
.plot-legend .item{display:flex;align-items:center;gap:4px;font-size:12px;color:#a0a0b0;cursor:pointer;padding:1px 0;white-space:nowrap}
.plot-legend .item:hover{color:#d0d0d0}
.plot-legend .box{width:10px;height:10px;border:1px solid #fff;flex-shrink:0;border-radius:1px}
.plot-legend .box.on{background:var(--c)}
.plot-legend .box.off{background:transparent}

.splitter{position:absolute;z-index:4;background:#252530}
.splitter:hover,.splitter.active{background:#4a6a8a}
.splitter-h{left:0;right:0;height:3px;cursor:row-resize}
.splitter-v{top:0;bottom:0;width:3px;cursor:col-resize}

.log-line{padding:0 8px;white-space:nowrap;display:flex;gap:8px;font-size:13px;line-height:1.5}
.log-ts{color:#404050;flex-shrink:0}
.log-lv{flex-shrink:0;font-weight:bold;min-width:48px}
.log-lv.INFO{color:#80b0e0}
.log-lv.WARN,.log-lv.WARNING{color:#e0a040}
.log-lv.ERROR,.log-lv.FATAL{color:#e04a4a}
.log-lv.DEBUG{color:#505060}
.log-msg{overflow:hidden;text-overflow:ellipsis}
</style>
</head>
<body>

<div id="sidebar">
  <button id="sidebar-toggle" title="Toggle sidebar">◀</button>
  <div class="sidebar-header">设置</div>
  <div class="sidebar-content">
    <div class="section">
      <div class="section-title">显示模式</div>
      <label><input type="radio" name="mode" value="sliding" checked onchange="onSettingsChange()"> 滑动</label>
      <label><input type="radio" name="mode" value="centered" onchange="onSettingsChange()"> 居中</label>
      <label><input type="radio" name="mode" value="paused" onchange="onSettingsChange()"> 暂停</label>
    </div>
    <div class="section">
      <div class="section-title">窗口 (秒)</div>
      <input type="number" id="win-size" value="10" min="1" max="120" step="1" onchange="onSettingsChange()">
    </div>
    <div class="section">
      <div class="section-title">历史</div>
      <select id="history-sel" onchange="trimData()">
        <option value="30">30 秒</option>
        <option value="60" selected>1 分钟</option>
        <option value="120">2 分钟</option>
        <option value="300">5 分钟</option>
      </select>
    </div>
    <div class="section">
      <div class="section-title">数据字段</div>
      <div id="field-list"><span style="color:#404050;font-size:13px">等待数据…</span></div>
    </div>
  </div>
</div>

<div id="main-area">
  <div id="top-bar">
    <div class="dot" id="dot"></div>
    <span id="status">已连接</span>
    <span id="stats" style="color:#505060">0 pkt/s</span>
    <span style="flex:1"></span>
    <button onclick="clearPlots()">清除</button>
  </div>
</div>

<script>
// ── Sidebar toggle ────────────────────────────────────────────────────────────
const sidebar = document.getElementById('sidebar');
const toggleBtn = document.getElementById('sidebar-toggle');
toggleBtn.addEventListener('click', function() {
  sidebar.classList.toggle('collapsed');
  toggleBtn.textContent = sidebar.classList.contains('collapsed') ? '\u25b6' : '\u25c0';
  setTimeout(relayout, 200);
});

// ── Layout engine ─────────────────────────────────────────────────────────────
const mainArea = document.getElementById('main-area');
const topBar = document.getElementById('top-bar');

let rows = [];
let panelIdSeq = 0;

function newPanelId() { return panelIdSeq++; }

function initLayout() {
  var id0 = newPanelId(), id1 = newPanelId();
  rows = [
    { ratio: 1.0, panels: [
      { id: id0, type: 'image', ratio: 0.4 },
      { id: id1, type: 'plot', ratio: 0.6 }
    ]}
  ];
  panelMap[id0] = null;
  panelMap[id1] = null;
  relayout();
}

function relayout() {
  var area = mainArea.getBoundingClientRect();
  var tH = topBar.offsetHeight;
  var top = tH, availH = area.height - tH, availW = area.width;
  // remove old splitters
  mainArea.querySelectorAll('.splitter').forEach(function(el) { el.remove(); });
  // position rows
  var y = top;
  var splitters = [];
  for (var ri = 0; ri < rows.length; ri++) {
    var row = rows[ri];
    var rh = Math.floor(availH * row.ratio);
    var x = 0;
    for (var pi = 0; pi < row.panels.length; pi++) {
      var p = row.panels[pi];
      var pw = Math.floor(availW * p.ratio);
      ensurePanelDOM(p);
      p.el.style.left = x + 'px';
      p.el.style.top = y + 'px';
      p.el.style.width = pw + 'px';
      p.el.style.height = (rh - (ri < rows.length - 1 ? 3 : 0)) + 'px';
      x += pw;
      if (pi < row.panels.length - 1) {
        splitters.push({ type: 'v', left: x, top: y, height: rh, rowIdx: ri, panelIdx: pi });
        x += 3;
      }
    }
    y += rh;
    if (ri < rows.length - 1) {
      splitters.push({ type: 'h', top: y, rowIdx: ri });
      y += 3;
    }
  }
  // create splitter DOM
  splitters.forEach(function(s) {
    var el = document.createElement('div');
    el.className = 'splitter ' + (s.type === 'h' ? 'splitter-h' : 'splitter-v');
    if (s.type === 'v') {
      el.style.left = s.left + 'px';
      el.style.top = s.top + 'px';
      el.style.height = s.height + 'px';
    } else {
      el.style.top = s.top + 'px';
    }
    el.addEventListener('mousedown', function(e) { startDrag(s, e); });
    mainArea.appendChild(el);
  });
  updateAllCharts();
}

function ensurePanelDOM(p) {
  if (p.el) return;
  var el = document.createElement('div');
  el.className = 'panel';
  var hdr = document.createElement('div');
  hdr.className = 'panel-header';

  var typeSel = document.createElement('select');
  [{v:'plot',t:'绘图'},{v:'image',t:'图像'},{v:'log',t:'日志'}].forEach(function(o) {
    var opt = document.createElement('option');
    opt.value = o.v; opt.textContent = o.t;
    if (o.v === p.type) opt.selected = true;
    typeSel.appendChild(opt);
  });
  typeSel.onchange = function() { switchPanelType(p, typeSel.value); };
  hdr.appendChild(typeSel);

  if (p.type === 'image') {
    var imgSel = document.createElement('select');
    imgSel.style.cssText = 'max-width:130px;margin-left:4px';
    imgSel.onchange = function() { showPanelImage(p); };
    hdr.appendChild(imgSel);
    p.imgSel = imgSel;
    var fpsSpan = document.createElement('span');
    fpsSpan.style.cssText = 'color:#606070;font-size:12px;margin-left:6px;flex-shrink:0';
    fpsSpan.textContent = '0 fps';
    hdr.appendChild(fpsSpan);
    p.imgFps = fpsSpan;
  }

  var sp = document.createElement('span'); sp.style.flex = '1'; hdr.appendChild(sp);

  var dirs = [
    { sym: '\u25c0', title: '向左拆分' },
    { sym: '\u25b2', title: '向上拆分' },
    { sym: '\u25bc', title: '向下拆分' },
    { sym: '\u25b6', title: '向右拆分' },
  ];
  dirs.forEach(function(d) {
    var btn = document.createElement('button');
    btn.className = 'dir-btn'; btn.textContent = d.sym; btn.title = d.title;
    btn.onclick = function() { splitPanel(p.id, d.row, d.col); };
    hdr.appendChild(btn);
  });

  var closeBtn = document.createElement('button');
  closeBtn.textContent = '\u00d7'; closeBtn.title = '关闭';
  closeBtn.onclick = function() { removePanel(p.id); };
  hdr.appendChild(closeBtn);
  el.appendChild(hdr);

  var body = document.createElement('div');
  body.className = 'panel-body';
  if (p.type === 'plot') {
    var cvs = document.createElement('canvas');
    body.appendChild(cvs);
    var legend = document.createElement('div');
    legend.className = 'plot-legend';
    body.appendChild(legend);
    p.legendDiv = legend; p.chartEl = cvs;
  } else if (p.type === 'image') {
    var img = document.createElement('img'); img.style.display = 'none'; body.appendChild(img);
    var ph = document.createElement('span'); ph.className = 'placeholder'; ph.textContent = '等待中\u2026'; body.appendChild(ph);
    var info = document.createElement('div'); info.className = 'panel-img-info'; body.appendChild(info);
    p.imgEl = img; p.placeholder = ph; p.imgInfo = info;
  } else {
    var logs = document.createElement('div');
    logs.style.cssText = 'flex:1;overflow-y:auto;padding:4px 0';
    body.appendChild(logs);
    var lc = document.createElement('span');
    lc.style.cssText = 'position:absolute;top:2px;right:6px;color:#505060;font-size:11px;z-index:1';
    body.appendChild(lc);
    logs.style.overflowY = 'auto';
    p.logDiv = logs; p.logCount = lc; p.logEntries = 0;
  }
  el.appendChild(body);
  mainArea.appendChild(el);
  p.el = el;
  initPanelContent(p);
}

function initPanelContent(p) {
  if (p.type === 'plot') initPlotPanel(p);
  else if (p.type === 'image') initImagePanel(p);
}

function initPlotPanel(p) {
  if (!p.chartEl) return;
  var win = parseFloat(document.getElementById('win-size').value) || 10;
  p.chart = new Chart(p.chartEl.getContext('2d'), {
    type: 'line', data: { datasets: [] },
    options: {
      responsive: true, maintainAspectRatio: false, animation: false,
      interaction: { mode: 'nearest', intersect: false },
      plugins: { legend: { display: false } },
      scales: {
        x: { type: 'linear', title:{display:true,text:'time (s)',color:'#606070',font:{size:12}}, ticks:{color:'#505060',font:{size:11}}, grid:{color:'#202030'}, min:0, max:win },
        y: { title:{display:true,text:'value',color:'#606070',font:{size:12}}, ticks:{color:'#505060',font:{size:11}}, grid:{color:'#202030'} }
      }
    }
  });
  renderLegend(p);
  rebuildPanelChart(p);
}

function initImagePanel(p) {
  if (!p.imgEl) return;
  refreshImgOptions(p);
  if (Object.keys(imgSources).length > 0) showPanelImage(p);
}

function renderLegend(p) {
  if (!p || !p.legendDiv || p.type !== 'plot') return;
  if (!p.chart) return;
  p.legendDiv.innerHTML = '';
  for (var n in fieldMeta) {
    if (!fieldMeta[n].subscribed) continue;
    var idx = -1;
    for (var i = 0; i < p.chart.data.datasets.length; i++) {
      if (p.chart.data.datasets[i].label === n) { idx = i; break; }
    }
    var visible = idx >= 0 ? p.chart.isDatasetVisible(idx) : true;
    var item = document.createElement('div');
    item.className = 'item';
    var box = document.createElement('div');
    box.className = 'box ' + (visible ? 'on' : 'off');
    box.style.setProperty('--c', fieldMeta[n].color);
    item.appendChild(box);
    item.appendChild(document.createTextNode(n));
    item.setAttribute('data-idx', idx);
    item.onclick = function(e) {
      e.stopPropagation();
      var i = parseInt(this.getAttribute('data-idx'));
      if (i >= 0 && p.chart) {
        p.chart.data.datasets[i].hidden = !p.chart.data.datasets[i].hidden;
        p.chart.update();
        renderLegend(p);
      }
    };
    item.style.cssText = 'min-height:18px;padding:2px 0';
    p.legendDiv.appendChild(item);
  }
}

function rebuildPanelChart(p) {
  if (!p.chart) return;
  p.chart.data.datasets.length = 0;
  for (var name in fieldMeta) {
    if (!fieldMeta[name].subscribed) continue;
    var ds = { label: name, data: [], borderColor: fieldMeta[name].color, borderWidth: 1.8, pointRadius: 0, spanGaps: false, hidden: false };
    for (var i = 0; i < plotData.length; i++) {
      var yv = plotData[i].fields[name];
      ds.data.push({ x: plotData[i].x, y: yv !== undefined ? yv : NaN });
    }
    p.chart.data.datasets.push(ds);
  }
  p.chart.update('none');
  renderLegend(p);
  updatePanelView(p);
}

function pushDataToPanel(p) {
  if (p.type !== 'plot' || !p.chart) return;

  var existingLabels = {};
  p.chart.data.datasets.forEach(function(ds) { existingLabels[ds.label] = true; });
  var addedNew = false;
  for (var name in fieldMeta) {
    if (!fieldMeta[name].subscribed) continue;
    if (existingLabels[name]) continue;
    var ds = { label: name, data: [], borderColor: fieldMeta[name].color, borderWidth: 1.8, pointRadius: 0, spanGaps: false, hidden: false };
    for (var i = 0; i < plotData.length - 1; i++) {
      var yv = plotData[i].fields[name];
      ds.data.push({ x: plotData[i].x, y: yv !== undefined ? yv : NaN });
    }
    p.chart.data.datasets.push(ds);
    existingLabels[name] = true;
    addedNew = true;
  }

  if (addedNew) p.chart.update('none');

  var last = plotData[plotData.length - 1];
  if (!last) return;
  var history = parseFloat(document.getElementById('history-sel').value);
  for (var i = 0; i < p.chart.data.datasets.length; i++) {
    var ds = p.chart.data.datasets[i];
    var yv = last.fields[ds.label];
    ds.data.push({ x: last.x, y: yv !== undefined ? yv : NaN });
    while (ds.data.length > 1 && ds.data[0].x < last.x - history) ds.data.shift();
    if (ds.data.length > MAX_PTS) ds.data.splice(0, ds.data.length - MAX_PTS);
  }
  renderLegend(p);
  updatePanelView(p);
}

function updatePanelView(p) {
  if (!p.chart || lastX === 0) return;
  var mode = document.querySelector('input[name="mode"]:checked').value;
  var win = parseFloat(document.getElementById('win-size').value) || 10;
  if (mode === 'paused') { p.chart.update('none'); return; }
  else if (mode === 'centered') {
    p.chart.options.scales.x.min = lastX - win / 2;
    p.chart.options.scales.x.max = lastX + win / 2;
  } else {
    p.chart.options.scales.x.min = Math.max(0, lastX - win);
    p.chart.options.scales.x.max = Math.max(win, lastX);
  }
  p.chart.update('none');
}

function updateAllCharts() {
  for (var i = 0; i < rows.length; i++)
    for (var j = 0; j < rows[i].panels.length; j++)
      updatePanelView(rows[i].panels[j]);
}

function showPanelImage(p) {
  if (p.type !== 'image' || !p.imgSel) return;
  var name = p.imgSel.value;
  if (!name) { var keys = Object.keys(imgSources); if (keys.length > 0) { name = keys[0]; p.imgSel.value = name; } }
  var s = imgSources[name];
  if (s && s.b64) {
    p.imgEl.src = s.b64; p.imgEl.style.display = '';
    p.placeholder.style.display = 'none'; p.imgInfo.textContent = s.kb + ' KB';
  }
}

function refreshImgOptions(p) {
  if (!p || !p.imgSel) return;
  var cur = p.imgSel.value;
  p.imgSel.innerHTML = '';
  var keys = Object.keys(imgSources);
  if (keys.length === 0) {
    p.imgSel.appendChild(Object.assign(document.createElement('option'), {value:'',textContent:'\u2014'}));
    return;
  }
  for (var i = 0; i < keys.length; i++) {
    var opt = document.createElement('option');
    opt.value = keys[i]; opt.textContent = keys[i];
    p.imgSel.appendChild(opt);
  }
  if (cur && imgSources[cur]) p.imgSel.value = cur;
  else p.imgSel.value = keys[0];
}

function switchPanelType(p, newType) {
  if (p.type === newType) return;
  if (p.type === 'plot' && p.chart) { p.chart.destroy(); p.chart = null; }
  if (p.imgSel) { p.imgSel.remove(); p.imgSel = null; }
  if (p.imgFps) { p.imgFps.remove(); p.imgFps = null; }
  p.type = newType;
  p.el.querySelector('.panel-body').innerHTML = '';
  p.legendDiv = null; p.chartEl = null; p.chart = null;
  p.imgEl = null; p.placeholder = null; p.imgInfo = null;
  p.logDiv = null; p.logCount = null;
  var body = p.el.querySelector('.panel-body');
  if (newType === 'plot') {
    var cvs = document.createElement('canvas'); body.appendChild(cvs);
    var legend = document.createElement('div'); legend.className = 'plot-legend';
    body.appendChild(legend);
    p.legendDiv = legend; p.chartEl = cvs;
    initPlotPanel(p);
  } else if (newType === 'image') {
    var img = document.createElement('img'); img.style.display = 'none'; body.appendChild(img);
    var ph = document.createElement('span'); ph.className = 'placeholder'; ph.textContent = 'Waiting\u2026'; body.appendChild(ph);
    var info = document.createElement('div'); info.className = 'panel-img-info'; body.appendChild(info);
    p.imgEl = img; p.placeholder = ph; p.imgInfo = info;
    var typeSel = p.el.querySelector('.panel-header select');
    var imgSel = document.createElement('select');
    imgSel.style.cssText = 'max-width:130px;margin-left:4px';
    imgSel.onchange = function() { showPanelImage(p); };
    typeSel.insertAdjacentElement('afterend', imgSel);
    p.imgSel = imgSel;
    var fpsSpan = document.createElement('span');
    fpsSpan.style.cssText = 'color:#606070;font-size:12px;margin-left:6px;flex-shrink:0';
    fpsSpan.textContent = '0 fps';
    imgSel.insertAdjacentElement('afterend', fpsSpan);
    p.imgFps = fpsSpan;
    initImagePanel(p);
  } else {
    var logs = document.createElement('div'); logs.style.cssText = 'flex:1;overflow-y:auto;padding:4px 0';
    body.appendChild(logs);
    var lc = document.createElement('span');
    lc.style.cssText = 'position:absolute;top:2px;right:6px;color:#505060;font-size:11px;z-index:1';
    body.appendChild(lc);
    p.logDiv = logs; p.logCount = lc; p.logEntries = 0;
  }
  refreshHeaderSelect(p);
}

function refreshHeaderSelect(p) {
  var sel = p.el.querySelector('.panel-header select');
  if (sel) sel.value = p.type;
}

function splitPanel(panelId, rowDir, colDir) {
  for (var ri = 0; ri < rows.length; ri++) {
    for (var pi = 0; pi < rows[ri].panels.length; pi++) {
      if (rows[ri].panels[pi].id !== panelId) continue;
      if (colDir !== 0) {
        var old = rows[ri].panels[pi];
        var half = old.ratio / 2;
        old.ratio = half;
        var nid = newPanelId();
        panelMap[nid] = null;
        var np = { id: nid, type: 'image', ratio: half };
        if (colDir > 0) rows[ri].panels.splice(pi + 1, 0, np);
        else rows[ri].panels.splice(pi, 0, np);
      }
      if (rowDir !== 0) {
        var nid = newPanelId();
        panelMap[nid] = null;
        var nr = { ratio: rows[ri].ratio / 2, panels: [{ id: nid, type: 'image', ratio: 1.0 }] };
        rows[ri].ratio = nr.ratio;
        if (rowDir > 0) rows.splice(ri + 1, 0, nr);
        else rows.splice(ri, 0, nr);
      }
      relayout();
      return;
    }
  }
}

function removePanel(panelId) {
  if (rows.length === 1 && rows[0].panels.length === 1) return;
  for (var ri = 0; ri < rows.length; ri++) {
    for (var pi = 0; pi < rows[ri].panels.length; pi++) {
      if (rows[ri].panels[pi].id !== panelId) continue;
      var p = rows[ri].panels[pi];
      if (p.chart) p.chart.destroy();
      if (p.el) p.el.remove();
      delete panelMap[p.id];
      if (rows[ri].panels.length === 1) {
        rows.splice(ri, 1);
        if (rows.length > 0) {
          var total = 0;
          for (var i = 0; i < rows.length; i++) total += rows[i].ratio;
          for (var i = 0; i < rows.length; i++) rows[i].ratio /= total;
        }
      } else {
        var remain = rows[ri].panels.length - 1;
        var freed = rows[ri].panels[pi].ratio;
        rows[ri].panels.splice(pi, 1);
        var total = 0;
        for (var i = 0; i < rows[ri].panels.length; i++) total += rows[ri].panels[i].ratio;
        for (var i = 0; i < rows[ri].panels.length; i++) rows[ri].panels[i].ratio = rows[ri].panels[i].ratio / total + freed / remain;
      }
      relayout();
      return;
    }
  }
}

// ── Dragging ──────────────────────────────────────────────────────────────────
function startDrag(splitInfo, e) {
  e.preventDefault();
  var area = mainArea.getBoundingClientRect();
  var tH = topBar.offsetHeight;
  var availH = area.height - tH, availW = area.width;
  var startX = e.clientX, startY = e.clientY;

  function onMove(ev) {
    if (splitInfo.type === 'v') {
      var dx = ev.clientX - startX;
      var row = rows[splitInfo.rowIdx];
      var p0 = row.panels[splitInfo.panelIdx], p1 = row.panels[splitInfo.panelIdx + 1];
      var total = p0.ratio + p1.ratio;
      var new0 = total * (splitInfo.left + dx) / availW;
      new0 = Math.max(0.05, Math.min(total - 0.05, new0));
      p0.ratio = new0; p1.ratio = total - new0;
      var sum = 0; for (var i = 0; i < row.panels.length; i++) sum += row.panels[i].ratio;
      for (var i = 0; i < row.panels.length; i++) row.panels[i].ratio /= sum;
    } else {
      var dy = ev.clientY - startY;
      var r0 = rows[splitInfo.rowIdx], r1 = rows[splitInfo.rowIdx + 1];
      var total = r0.ratio + r1.ratio;
      var new0 = total * (splitInfo.top - tH + dy) / availH;
      new0 = Math.max(0.05, Math.min(total - 0.05, new0));
      r0.ratio = new0; r1.ratio = total - new0;
      var sum = 0; for (var i = 0; i < rows.length; i++) sum += rows[i].ratio;
      for (var i = 0; i < rows.length; i++) rows[i].ratio /= sum;
    }
    relayout();
  }
  function onUp() {
    document.removeEventListener('mousemove', onMove);
    document.removeEventListener('mouseup', onUp);
  }
  document.addEventListener('mousemove', onMove);
  document.addEventListener('mouseup', onUp);
}

// ── Data ─────────────────────────────────────────────────────────────────────
const COLORS = ['#4fc3f7','#ffb74d','#81c784','#e57373','#ba68c8','#4dd0e1','#fff176','#a1887f','#90a4ae','#f48fb1','#ef5350','#26c6da','#7e57c2','#66bb6a','#ff7043'];
var fieldMeta = {}, colorIdx = 0, xField = 'ts', firstTs = null, lastX = 0;
var plotData = [];
const MAX_PTS = 50000;
var panelMap = {};

function getColor() { var c = COLORS[colorIdx % COLORS.length]; colorIdx++; return c; }

function ensureField(name) {
  if (fieldMeta[name]) return;
  fieldMeta[name] = { color: getColor(), subscribed: false };
  var fl = document.getElementById('field-list');
  if (fl.querySelector('.placeholder')) fl.innerHTML = '';
  var lb = document.createElement('label');
  lb.style.cssText = 'display:flex;align-items:center;gap:4px;font-size:14px;padding:1px 0;color:#a0a0b0;cursor:pointer';
  var cb = document.createElement('input');
  cb.type = 'checkbox'; cb.value = name;
  cb.onchange = function() {
    fieldMeta[name].subscribed = cb.checked;
    for (var ri = 0; ri < rows.length; ri++)
      for (var pi = 0; pi < rows[ri].panels.length; pi++)
        rebuildPanelChart(rows[ri].panels[pi]);
  };
  lb.appendChild(cb);
  lb.appendChild(document.createTextNode(' ' + name));
  fl.appendChild(lb);
}

function rebuildAllCharts() {
  for (var ri = 0; ri < rows.length; ri++)
    for (var pi = 0; pi < rows[ri].panels.length; pi++)
      rebuildPanelChart(rows[ri].panels[pi]);
}

function addPoint(ts, data) {
  var xv;
  if (xField === 'ts') { if (firstTs === null) firstTs = ts; xv = (ts - firstTs) / 1e9; }
  else if (data[xField] !== undefined) xv = data[xField];
  else xv = lastX + 0.02;
  lastX = xv;

  var pt = { x: xv, fields: {} };
  for (var k in data) {
    if (k === xField || typeof data[k] !== 'number') continue;
    ensureField(k);
    if (fieldMeta[k].subscribed) pt.fields[k] = data[k];
  }
  plotData.push(pt);
  var history = parseFloat(document.getElementById('history-sel').value);
  while (plotData.length > 1 && plotData[0].x < lastX - history) plotData.shift();
  if (plotData.length > MAX_PTS) plotData.splice(0, plotData.length - MAX_PTS);

  for (var ri = 0; ri < rows.length; ri++)
    for (var pi = 0; pi < rows[ri].panels.length; pi++)
      pushDataToPanel(rows[ri].panels[pi]);
}

function clearPlots() {
  plotData = []; fieldMeta = {}; firstTs = null; lastX = 0;
  colorIdx = 0;
  document.getElementById('field-list').innerHTML = '<span style="color:#404050;font-size:13px">等待数据\u2026</span>';
  for (var ri = 0; ri < rows.length; ri++)
    for (var pi = 0; pi < rows[ri].panels.length; pi++) {
      var p = rows[ri].panels[pi];
      if (p.chart) { p.chart.data.datasets.length = 0; p.chart.update('none'); }
      if (p.legendDiv) p.legendDiv.innerHTML = '';
    }
}

function trimData() {
  var history = parseFloat(document.getElementById('history-sel').value);
  if (lastX === 0) return;
  while (plotData.length > 1 && plotData[0].x < lastX - history) plotData.shift();
  for (var ri = 0; ri < rows.length; ri++)
    for (var pi = 0; pi < rows[ri].panels.length; pi++) {
      var p = rows[ri].panels[pi];
      if (!p.chart) continue;
      var cutoff = lastX - history;
      for (var j = 0; j < p.chart.data.datasets.length; j++) {
        var ds = p.chart.data.datasets[j];
        while (ds.data.length > 1 && ds.data[0].x < cutoff) ds.data.shift();
      }
      updatePanelView(p);
    }
}

function onSettingsChange() {
  for (var ri = 0; ri < rows.length; ri++)
    for (var pi = 0; pi < rows[ri].panels.length; pi++)
      updatePanelView(rows[ri].panels[pi]);
}

// ── Image ────────────────────────────────────────────────────────────────────
var imgSources = {};
var imgFpsTracker = {};

function setImage(b64, meta) {
  var name = (meta && meta.name) ? meta.name : 'default';
  var now = Date.now();

  if (!imgFpsTracker[name]) imgFpsTracker[name] = [];
  imgFpsTracker[name].push(now);
  while (imgFpsTracker[name].length > 10) imgFpsTracker[name].shift();
  var fps = 0;
  if (imgFpsTracker[name].length >= 2) {
    var dt = now - imgFpsTracker[name][0];
    fps = Math.round((imgFpsTracker[name].length - 1) * 1000 / dt);
  }

  imgSources[name] = { b64: 'data:image/jpeg;base64,' + b64, ts: now, kb: (b64.length * 0.75 / 1024).toFixed(0) };

  for (var ri = 0; ri < rows.length; ri++)
    for (var pi = 0; pi < rows[ri].panels.length; pi++) {
      var p = rows[ri].panels[pi];
      if (p.type !== 'image') continue;
      refreshImgOptions(p);
      if (p.imgFps) p.imgFps.textContent = fps + ' fps';
      if (p.imgSel.value === name) showPanelImage(p);
    }
}

// ── Log ──────────────────────────────────────────────────────────────────────
var logFilter = null;
function addLog(ts, level, msg) {
  for (var ri = 0; ri < rows.length; ri++)
    for (var pi = 0; pi < rows[ri].panels.length; pi++) {
      var p = rows[ri].panels[pi];
      if (p.type !== 'log' || !p.logDiv) continue;
      var div = document.createElement('div');
      div.className = 'log-line';
      var t = new Date(ts / 1e6);
      var tsStr = t.toTimeString().slice(0,8) + '.' + String(t.getMilliseconds()).padStart(3,'0');
      div.innerHTML = '<span class="log-ts">' + tsStr + '</span><span class="log-lv ' + level + '">' + level + '</span><span class="log-msg">' + msg + '</span>';
      p.logDiv.appendChild(div);
      p.logEntries++;
      while (p.logDiv.children.length > 500) { p.logDiv.firstChild.remove(); p.logEntries--; }
      p.logCount.textContent = p.logEntries;
      if (p.logDiv.scrollTop + p.logDiv.clientHeight >= p.logDiv.scrollHeight - 30)
        p.logDiv.scrollTop = p.logDiv.scrollHeight;
    }
}

// ── SSE ──────────────────────────────────────────────────────────────────────
const dot = document.getElementById('dot'), stats = document.getElementById('stats');
var pktCount = 0, lastPktTime = Date.now();
const es = new EventSource('/events');
es.onmessage = function(e) {
  try {
    var msg = JSON.parse(e.data);
    pktCount++;
    var now = Date.now();
    if (now - lastPktTime >= 1000) {
      stats.textContent = Math.round(pktCount * 1000 / (now - lastPktTime)) + ' pkt/s';
      pktCount = 0; lastPktTime = now;
    }
    if (msg.type === 'plot') addPoint(msg.ts, msg.data || {});
    else if (msg.type === 'image') setImage(msg.jpg_b64, msg.meta);
    else if (msg.type === 'log') addLog(msg.ts, msg.level, msg.msg);
    dot.className = 'dot';
  } catch (err) {}
};
es.onerror = function() { dot.className = 'dot dead'; };

// ── Init ─────────────────────────────────────────────────────────────────────
window.addEventListener('resize', function() { relayout(); });
initLayout();
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
            try: self._q.get_nowait()
            except queue.Empty: pass
            self._q.put_nowait(line)

    def get(self, timeout=0.5):
        try: return self._q.get(timeout=timeout)
        except queue.Empty: return None


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
        pass

    def do_GET(self):
        if self.path == '/': self._serve_html()
        elif self.path == '/events': self._serve_sse()
        elif self.path == '/chart.js': self._serve_chartjs()
        else: self.send_error(404)

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
            with open(chart_path, 'rb') as f: body = f.read()
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
        if os.path.exists(alt): backend_path = alt
        else:
            print(f"[server] backend not found at {backend_path}", file=sys.stderr)
            sys.exit(1)

    print(f"[server] starting backend: {backend_path} --port {args.udp_port}", file=sys.stderr)
    proc = subprocess.Popen(
        [backend_path, '--port', str(args.udp_port)],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1,
    )

    t = threading.Thread(target=reader_thread, args=(proc,), daemon=True)
    t.start()

    try:
        server = ThreadingHTTPServer(('0.0.0.0', args.port), Handler)
    except OSError as e:
        print(f"[server] cannot bind port {args.port}: {e}", file=sys.stderr)
        print(f"[server] try: python3 server.py --port {args.port + 1}", file=sys.stderr)
        proc.terminate(); proc.wait(timeout=3)
        sys.exit(1)

    print(f"[server]  http://localhost:{args.port}   (open this in browser)", file=sys.stderr)
    print(f"[server]  UDP listening on port {args.udp_port}", file=sys.stderr)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[server] shutting down...", file=sys.stderr)
    finally:
        server.server_close()
        proc.terminate(); proc.wait(timeout=3)


if __name__ == '__main__':
    main()
