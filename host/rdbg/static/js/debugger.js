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

  if (p.type === 'plot') {
    var pidLabel = document.createElement('span');
    pidLabel.className = 'panel-id';
    pidLabel.textContent = 'P' + p.id;
    pidLabel.title = '点击切换设置';
    pidLabel.onclick = function(e) { e.stopPropagation(); activatePanel(p); };
    hdr.appendChild(pidLabel);
  }

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
  if (p.type === 'plot') {
    body.addEventListener('click', function() { activatePanel(p); });
  }
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
  p.manualView = false;
  p.lastMouseX = 0;
  p.subscribed = {};
  for (var n in fieldMeta) p.subscribed[n] = false;
  if (activePanelId === null) activatePanel(p);
  var win = parseFloat(document.getElementById('win-size').value) || 10;
  p.chart = new Chart(p.chartEl.getContext('2d'), {
    type: 'line', data: { datasets: [] },
    options: {
      responsive: true, maintainAspectRatio: false, animation: false,
      interaction: { mode: 'nearest', intersect: false },
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

function initImagePanel(p) {
  if (!p.imgEl) return;
  p.imgScale = 1;
  p.imgTx = 0; p.imgTy = 0;
  p.imgDragging = false;

  var body = p.el.querySelector('.panel-body');
  body.addEventListener('wheel', function(e) {
    e.preventDefault();
    var s0 = p.imgScale;
    p.imgScale *= e.deltaY > 0 ? 0.9 : 1.1;
    if (p.imgScale < 0.1) p.imgScale = 0.1;
    if (p.imgScale > 15) p.imgScale = 15;
    var r = body.getBoundingClientRect();
    var mx = e.clientX - r.left - r.width / 2;
    var my = e.clientY - r.top - r.height / 2;
    p.imgTx = mx + (p.imgTx - mx) * p.imgScale / s0;
    p.imgTy = my + (p.imgTy - my) * p.imgScale / s0;
    applyImgTransform(p);
  });
  body.addEventListener('mousedown', function(e) {
    if (e.button !== 0) return;
    p.imgDragging = true;
    p.imgDragX = e.clientX - p.imgTx;
    p.imgDragY = e.clientY - p.imgTy;
    body.style.cursor = 'grabbing';
  });
  window.addEventListener('mousemove', function(e) {
    if (!p.imgDragging) return;
    p.imgTx = e.clientX - p.imgDragX;
    p.imgTy = e.clientY - p.imgDragY;
    applyImgTransform(p);
  });
  window.addEventListener('mouseup', function() {
    if (p.imgDragging) {
      p.imgDragging = false;
      body.style.cursor = '';
    }
  });
  p.imgEl.addEventListener('dragstart', function(e) { e.preventDefault(); });

  refreshImgOptions(p);
  if (Object.keys(imgSources).length > 0) showPanelImage(p);
}

function applyImgTransform(p) {
  if (!p.imgEl) return;
  p.imgEl.style.transform = 'translate(-50%,-50%) translate(' + p.imgTx.toFixed(1) + 'px,' + p.imgTy.toFixed(1) + 'px) scale(' + p.imgScale.toFixed(3) + ')';
}

function resetImgView(p) {
  if (p.type !== 'image') return;
  p.imgScale = 1; p.imgTx = 0; p.imgTy = 0;
  applyImgTransform(p);
}

function renderLegend(p) {
  if (!p || !p.legendDiv || p.type !== 'plot') return;
  if (!p.chart) return;
  p.legendDiv.innerHTML = '';
  for (var n in fieldMeta) {
    if (!p.subscribed[n]) continue;
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
    while (ds.data.length > 1 && ds.data[0].x < last.x - history) ds.data.shift();
    if (ds.data.length > MAX_PTS) ds.data.splice(0, ds.data.length - MAX_PTS);
  }
  renderLegend(p);
  updatePanelView(p);
}

function updatePanelView(p) {
  if (!p.chart || lastX === 0) return;
  if (p.manualView) { p.chart.update('none'); return; }
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

function resetAllViews() {
  for (var ri = 0; ri < rows.length; ri++)
    for (var pi = 0; pi < rows[ri].panels.length; pi++) {
      var p = rows[ri].panels[pi];
      if (p.type === 'plot' && p.chart) {
        p.manualView = false;
        if (p.chart.resetZoom) p.chart.resetZoom();
        updatePanelView(p);
      } else if (p.type === 'image') {
        resetImgView(p);
      }
    }
}

function showPanelImage(p) {
  if (p.type !== 'image' || !p.imgSel) return;
  var name = p.imgSel.value;
  if (!name) {
    var keys = Object.keys(imgSources);
    if (keys.length > 0) { name = keys[0]; p.imgSel.value = name; }
  }
  var s = imgSources[name];
  var src = s && (s.src || s.b64);
  if (src) {
    p.imgEl.src = src;
    p.imgEl.style.display = '';
    p.placeholder.style.display = 'none';
    p.imgInfo.textContent = (s.kb != null ? s.kb : '?') + ' KB';
    if (typeof applyImgTransform === 'function') applyImgTransform(p);
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
  if (p.type === 'image') { p.imgDragging = false; }
  if (p.imgSel) { p.imgSel.remove(); p.imgSel = null; }
  if (p.imgFps) { p.imgFps.remove(); p.imgFps = null; }
  var oldPid = p.el.querySelector('.panel-id');
  if (oldPid) oldPid.remove();
  p.type = newType;
  p.el.querySelector('.panel-body').innerHTML = '';
  p.legendDiv = null; p.chartEl = null; p.chart = null; p.manualView = false; p.fieldDiv = null;
  p.imgEl = null; p.placeholder = null; p.imgInfo = null;
  p.logDiv = null; p.logCount = null;
  var body = p.el.querySelector('.panel-body');
  if (newType === 'plot') {
    var typeSel = p.el.querySelector('.panel-header select');
    var pidLabel = document.createElement('span');
    pidLabel.className = 'panel-id';
    pidLabel.textContent = 'P' + p.id;
    pidLabel.title = '点击切换设置';
    pidLabel.onclick = function(e) { e.stopPropagation(); activatePanel(p); };
    typeSel.parentNode.insertBefore(pidLabel, typeSel);
    var cvs = document.createElement('canvas'); body.appendChild(cvs);
    var legend = document.createElement('div'); legend.className = 'plot-legend';
    body.appendChild(legend);
    body.addEventListener('click', function() { activatePanel(p); });
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
var plotData = [], activePanelId = null;
const MAX_PTS = 50000;
var panelMap = {};

function getColor() { var c = COLORS[colorIdx % COLORS.length]; colorIdx++; return c; }

function onSenderChange() {
  var name = document.getElementById('sender-sel').value;
  if (!name) return;
  plotData = [];
  for (var ri = 0; ri < rows.length; ri++)
    for (var pi = 0; pi < rows[ri].panels.length; pi++) {
      var p = rows[ri].panels[pi];
      if (p.chart) p.chart.data.datasets.length = 0;
      rebuildPanelChart(p);
    }
  fetch('/select?sender=' + encodeURIComponent(name));
}

function getPanelById(id) {
  for (var ri = 0; ri < rows.length; ri++)
    for (var pi = 0; pi < rows[ri].panels.length; pi++)
      if (rows[ri].panels[pi].id === id) return rows[ri].panels[pi];
  return null;
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
  for (var ri = 0; ri < rows.length; ri++)
    for (var pi = 0; pi < rows[ri].panels.length; pi++) {
      var rp = rows[ri].panels[pi];
      if (rp.el) rp.el.classList.toggle('active', rp.id === p.id);
    }
}

function rebuildSidebar(p) {
  var fl = document.getElementById('field-list');
  fl.innerHTML = '';
  if (!p || Object.keys(fieldMeta).length === 0) {
    fl.innerHTML = '<span style=\"color:#404050;font-size:13px\">等待数据…</span>';
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
  for (var ri = 0; ri < rows.length; ri++)
    for (var pi = 0; pi < rows[ri].panels.length; pi++) {
      var p = rows[ri].panels[pi];
      if (p.type === 'plot') p.subscribed[name] = false;
    }
  var ap = activePanelId !== null ? getPanelById(activePanelId) : null;
  if (ap) rebuildSidebar(ap);
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
    pt.fields[k] = data[k];
  }
  plotData.push(pt);
  var history = parseFloat(document.getElementById('history-sel').value);
  while (plotData.length > 1 && plotData[0].x < lastX - history) plotData.shift();
  if (plotData.length > MAX_PTS) plotData.splice(0, plotData.length - MAX_PTS);

  scheduleChartFlush();
}

var _chartFlushRaf = 0;
function scheduleChartFlush() {
  if (_chartFlushRaf) return;
  _chartFlushRaf = requestAnimationFrame(function() {
    _chartFlushRaf = 0;
    for (var ri = 0; ri < rows.length; ri++)
      for (var pi = 0; pi < rows[ri].panels.length; pi++)
        pushDataToPanel(rows[ri].panels[pi]);
  });
}

function clearPlots() {
  plotData = []; fieldMeta = {}; firstTs = null; lastX = 0; activePanelId = null;
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
  if (!b64) return;
  setImageSrc('data:image/jpeg;base64,' + b64, meta, (b64.length * 0.75 / 1024).toFixed(0));
}

function setImageSrc(src, meta, kb) {
  if (!src) return;
  var name = (meta && meta.name) ? meta.name : 'default';
  var now = Date.now();
  var isNew = !imgSources[name];

  if (!imgFpsTracker[name]) imgFpsTracker[name] = [];
  imgFpsTracker[name].push(now);
  while (imgFpsTracker[name].length > 10) imgFpsTracker[name].shift();
  var fps = 0;
  if (imgFpsTracker[name].length >= 2) {
    var dt = now - imgFpsTracker[name][0];
    fps = Math.round((imgFpsTracker[name].length - 1) * 1000 / dt);
  }

  // Keep both keys: older cached showPanelImage looked up .b64
  imgSources[name] = { src: src, b64: src, ts: now, kb: kb != null ? kb : '?' };

  for (var ri = 0; ri < rows.length; ri++)
    for (var pi = 0; pi < rows[ri].panels.length; pi++) {
      var p = rows[ri].panels[pi];
      if (p.type !== 'image') continue;
      if (isNew) refreshImgOptions(p);
      if (p.imgFps) p.imgFps.textContent = fps + ' fps';
      var sel = p.imgSel ? p.imgSel.value : '';
      if (!sel || sel === name) showPanelImage(p);
    }
}

function setImageFromUrl(url, meta) {
  if (!url) return;
  // Bust cache so rapid /img/0,/img/1,... always refresh the <img>.
  var src = url + (url.indexOf('?') >= 0 ? '&' : '?') + 't=' + Date.now();
  setImageSrc(src, meta, '?');
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
var pktCount = 0, lastPktTime = Date.now(), lastAnyPkt = 0;
var clientTimeout = 4000;

function setConnected(c, sender) {
  var st = document.getElementById('status');
  if (c) {
    dot.className = 'dot';
    st.textContent = sender ? '已连接 \u2192 ' + sender : '已连接';
  } else {
    dot.className = 'dot dead';
    st.textContent = '断连';
  }
}
setConnected(false);
setInterval(function() {
  if (lastAnyPkt && Date.now() - lastAnyPkt > clientTimeout) setConnected(false);
}, 1000);
const es = new EventSource('/events');
es.onmessage = function(e) {
  try {
    var msg = JSON.parse(e.data);
    lastAnyPkt = Date.now();
    pktCount++;
    var now = Date.now();
    if (now - lastPktTime >= 1000) {
      stats.textContent = Math.round(pktCount * 1000 / (now - lastPktTime)) + ' pkt/s';
      pktCount = 0; lastPktTime = now;
    }
    var snd = msg._from || (msg.data && msg.data._from) || (msg.meta && msg.meta._from) || '';
    if (msg.type === 'plot') { addPoint(msg.ts, msg.data || {}); setConnected(true, snd); }
    else if (msg.type === 'image') {
      if (msg.url) setImageFromUrl(msg.url, msg.meta || {});
      else if (msg.jpg_b64) setImage(msg.jpg_b64, msg.meta || {});
      setConnected(true, snd);
    }
    else if (msg.type === 'log') { addLog(msg.ts, msg.level, msg.msg); setConnected(true, snd); }
    else if (msg.type === 'status') { setConnected(msg.connected, msg.sender); }
    else if (msg.type === 'state') {
      var sel = document.getElementById('sender-sel');
      var cur = sel.value;
      sel.innerHTML = '';
      var list = msg.senders || [];
      for (var i = 0; i < list.length; i++) {
        var opt = document.createElement('option');
        opt.value = list[i]; opt.textContent = list[i];
        sel.appendChild(opt);
      }
      if (list.indexOf(cur) >= 0) sel.value = cur;
      else if (msg.active_sender && list.indexOf(msg.active_sender) >= 0) sel.value = msg.active_sender;
      else if (list.length > 0) sel.value = list[0];
    }
  } catch (err) {}
};
es.onerror = function() { dot.className = 'dot dead'; };

// ── Init ─────────────────────────────────────────────────────────────────────
window.addEventListener('resize', function() { relayout(); });
initLayout();
