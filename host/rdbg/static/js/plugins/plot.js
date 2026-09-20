// Plot panel plugin — field subscription lives on the panel; data is global for replay.js.
const COLORS = ['#4fc3f7','#ffb74d','#81c784','#e57373','#ba68c8','#4dd0e1','#fff176','#a1887f','#90a4ae','#f48fb1','#ef5350','#26c6da','#7e57c2','#66bb6a','#ff7043'];
var fieldMeta = {}, colorIdx = 0, xField = 'ts', firstTs = null, lastX = 0;
var plotData = [], activePanelId = null;
var replayCursorX = null;  // seconds; null = watch 模式不画竖线
const MAX_PTS = 50000;

function getColor() { var c = COLORS[colorIdx % COLORS.length]; colorIdx++; return c; }

var replayCursorPlugin = {
  id: 'replayCursor',
  afterDraw: function(chart) {
    if (replayCursorX == null || !isFinite(replayCursorX)) return;
    var xScale = chart.scales.x;
    if (!xScale) return;
    var x = xScale.getPixelForValue(replayCursorX);
    var area = chart.chartArea;
    if (x < area.left || x > area.right) return;
    var ctx = chart.ctx;
    ctx.save();
    ctx.beginPath();
    ctx.moveTo(x, area.top);
    ctx.lineTo(x, area.bottom);
    ctx.lineWidth = 1.5;
    ctx.strokeStyle = '#ff7043';
    ctx.setLineDash([]);
    ctx.stroke();
    ctx.beginPath();
    ctx.moveTo(x, area.top);
    ctx.lineTo(x - 4, area.top - 5);
    ctx.lineTo(x + 4, area.top - 5);
    ctx.closePath();
    ctx.fillStyle = '#ff7043';
    ctx.fill();
    ctx.restore();
  }
};
if (typeof Chart !== 'undefined' && Chart.register) Chart.register(replayCursorPlugin);

function setReplayCursor(tSec) {
  replayCursorX = (tSec == null || !isFinite(tSec)) ? null : tSec;
  forEachPanel(function(p) {
    if (p.type === 'plot' && p.chart) p.chart.update('none');
  });
}

function getPanelById(id) {
  return panelMap[id] || null;
}

function activatePanel(p) {
  if (!p || p.type !== 'plot') return;
  var old = activePanelId !== null ? getPanelById(activePanelId) : null;
  if (old && old !== p && old.fieldDiv) {
    var cbs = old.fieldDiv.querySelectorAll('input');
    for (var i = 0; i < cbs.length; i++) old.subscribed[cbs[i].value] = cbs[i].checked;
  }
  activePanelId = p.id;
  rebuildSidebar(p);
  forEachPanel(function(rp) {
    if (rp.el) rp.el.classList.toggle('active', rp.id === p.id);
  });
}

function rebuildSidebar(p) {
  var fl = document.getElementById('field-list');
  if (!fl) return;
  fl.innerHTML = '';
  if (!p || Object.keys(fieldMeta).length === 0) {
    fl.innerHTML = '<span style="color:#404050;font-size:13px">等待数据…</span>';
    return;
  }
  for (var name in fieldMeta) {
    var lb = document.createElement('label');
    lb.style.cssText = 'display:flex;align-items:center;gap:4px;font-size:14px;padding:1px 0;color:#a0a0b0;cursor:pointer';
    var cb = document.createElement('input');
    cb.type = 'checkbox'; cb.value = name;
    cb.checked = p.subscribed[name] || false;
    cb.onchange = function() {
      p.subscribed[this.value] = this.checked;
      rebuildPanelChart(p);
    };
    lb.appendChild(cb);
    lb.appendChild(document.createTextNode(' ' + name));
    fl.appendChild(lb);
  }
  p.fieldDiv = fl;
}

function ensureField(name) {
  if (fieldMeta[name]) return;
  fieldMeta[name] = { color: getColor() };
  forEachPanel(function(p) {
    if (p.type === 'plot') p.subscribed[name] = false;
  });
  var ap = activePanelId !== null ? getPanelById(activePanelId) : null;
  if (ap) rebuildSidebar(ap);
}

function renderLegend(p) {
  if (!p || !p.legendDiv || p.type !== 'plot') return;
  if (!p.chart) return;
  p.legendDiv.innerHTML = '';
  for (var n in fieldMeta) {
    if (!p.subscribed[n]) continue;
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
      var di = parseInt(this.getAttribute('data-idx'));
      if (di >= 0 && p.chart) {
        p.chart.data.datasets[di].hidden = !p.chart.data.datasets[di].hidden;
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
    if (!p.subscribed[name]) continue;
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
    if (!p.subscribed[name]) continue;
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
    if (!window.REPLAY_MODE) {
      while (ds.data.length > 1 && ds.data[0].x < last.x - history) ds.data.shift();
      if (ds.data.length > MAX_PTS) ds.data.splice(0, ds.data.length - MAX_PTS);
    }
  }
  renderLegend(p);
  updatePanelView(p);
}

function updatePanelView(p) {
  if (!p.chart) return;
  var followX = (replayCursorX != null && isFinite(replayCursorX)) ? replayCursorX : lastX;
  if (followX == null || (!followX && followX !== 0)) return;
  if (p.manualView) { p.chart.update('none'); return; }
  var modeEl = document.querySelector('input[name="mode"]:checked');
  var mode = modeEl ? modeEl.value : 'sliding';
  var winEl = document.getElementById('win-size');
  var win = parseFloat(winEl && winEl.value) || 10;
  if (mode === 'paused') { p.chart.update('none'); return; }
  else if (mode === 'centered') {
    p.chart.options.scales.x.min = followX - win / 2;
    p.chart.options.scales.x.max = followX + win / 2;
  } else {
    p.chart.options.scales.x.min = Math.max(0, followX - win);
    p.chart.options.scales.x.max = Math.max(win, followX);
  }
  p.chart.update('none');
}

function updateAllCharts() {
  forEachPanel(function(p) { updatePanelView(p); });
}

function rebuildAllCharts() {
  forEachPanel(function(p) { rebuildPanelChart(p); });
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
    pt.fields[k] = data[k];
  }
  plotData.push(pt);
  if (!window.REPLAY_MODE) {
    var history = parseFloat(document.getElementById('history-sel').value);
    while (plotData.length > 1 && plotData[0].x < lastX - history) plotData.shift();
    if (plotData.length > MAX_PTS) plotData.splice(0, plotData.length - MAX_PTS);
  }

  forEachPanel(function(p) { pushDataToPanel(p); });
}

function trimData() {
  if (window.REPLAY_MODE) return;
  var history = parseFloat(document.getElementById('history-sel').value);
  if (lastX === 0) return;
  while (plotData.length > 1 && plotData[0].x < lastX - history) plotData.shift();
  forEachPanel(function(p) {
    if (!p.chart) return;
    var cutoff = lastX - history;
    for (var j = 0; j < p.chart.data.datasets.length; j++) {
      var ds = p.chart.data.datasets[j];
      while (ds.data.length > 1 && ds.data[0].x < cutoff) ds.data.shift();
    }
    updatePanelView(p);
  });
}

function onSettingsChange() {
  forEachPanel(function(p) { updatePanelView(p); });
}

function initPlotPanel(p) {
  if (!p.chartEl) return;
  p.manualView = false;
  p.lastMouseX = 0;
  p.subscribed = {};
  for (var n in fieldMeta) p.subscribed[n] = false;
  if (activePanelId === null) activatePanel(p);
  var winEl = document.getElementById('win-size');
  var win = parseFloat(winEl && winEl.value) || 10;
  p.chart = new Chart(p.chartEl.getContext('2d'), {
    type: 'line', data: { datasets: [] },
    options: {
      responsive: true, maintainAspectRatio: false, animation: false,
      interaction: { mode: 'nearest', intersect: false },
      onClick: function(evt, _els, chart) {
        if (typeof window.replaySeek !== 'function') return;
        var xScale = chart.scales.x;
        if (!xScale || !evt || evt.x == null) return;
        var t = xScale.getValueForPixel(evt.x);
        if (t != null && isFinite(t)) window.replaySeek(t);
      },
      plugins: {
        legend: { display: false },
        zoom: {
          zoom: {
            wheel: { enabled: true },
            pinch: { enabled: true },
            mode: function() { return p.lastMouseX < 50 ? 'y' : 'x'; },
            overScaleMode: 'y',
            onZoomStart: function() { p.manualView = true; }
          },
          pan: {
            enabled: true,
            mode: 'x',
            overScaleMode: 'y',
            onPanStart: function() { p.manualView = true; }
          },
          limits: { x: { min: 0 } }
        }
      },
      scales: {
        x: { type: 'linear', title:{display:true,text:'time (s)',color:'#606070',font:{size:12}}, ticks:{color:'#505060',font:{size:11}}, grid:{color:'#202030'}, min:0, max:win },
        y: { title:{display:true,text:'value',color:'#606070',font:{size:12}}, ticks:{color:'#505060',font:{size:11}}, grid:{color:'#202030'} }
      }
    }
  });
  p.chart.canvas.addEventListener('mousemove', function(e) {
    var r = p.chart.canvas.getBoundingClientRect();
    p.lastMouseX = e.clientX - r.left;
  });
  renderLegend(p);
  rebuildPanelChart(p);
}

Rdbg.registerPanel({
  id: 'plot',
  title: '绘图',
  setupHeader: function(p, hdr, typeSel) {
    var pidLabel = document.createElement('span');
    pidLabel.className = 'panel-id';
    pidLabel.textContent = 'P' + p.id;
    pidLabel.title = '点击切换设置';
    pidLabel.onclick = function(e) { e.stopPropagation(); activatePanel(p); };
    hdr.insertBefore(pidLabel, typeSel);
  },
  setupBody: function(p, body) {
    var cvs = document.createElement('canvas');
    body.appendChild(cvs);
    var legend = document.createElement('div');
    legend.className = 'plot-legend';
    body.appendChild(legend);
    p.legendDiv = legend;
    p.chartEl = cvs;
    body.addEventListener('click', function() { activatePanel(p); });
  },
  init: function(p) { initPlotPanel(p); },
  destroy: function(p) {
    if (p.chart) { p.chart.destroy(); p.chart = null; }
    if (p.el) {
      var oldPid = p.el.querySelector('.panel-id');
      if (oldPid) oldPid.remove();
    }
    if (activePanelId === p.id) activePanelId = null;
  },
  reset: function(p) {
    if (!p.chart) return;
    p.manualView = false;
    if (p.chart.resetZoom) p.chart.resetZoom();
    updatePanelView(p);
  }
});
