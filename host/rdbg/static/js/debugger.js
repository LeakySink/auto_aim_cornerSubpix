// Layout shell — tiling, type switch, SSE. Panel internals live in plugins/*.js.

const sidebar = document.getElementById('sidebar');
const toggleBtn = document.getElementById('sidebar-toggle');
toggleBtn.addEventListener('click', function() {
  sidebar.classList.toggle('collapsed');
  toggleBtn.textContent = sidebar.classList.contains('collapsed') ? '\u25b6' : '\u25c0';
  setTimeout(relayout, 200);
});

const mainArea = document.getElementById('main-area');
const topBar = document.getElementById('top-bar');
const SPLIT_PX = 3;

var layoutRoot = null;
let panelIdSeq = 0;
var panelMap = {};

function newPanelId() { return panelIdSeq++; }

function makePanel(type) {
  var p = { id: newPanelId(), type: type || 'image' };
  panelMap[p.id] = p;
  return p;
}

function forEachPanel(fn) {
  function walk(n) {
    if (!n) return;
    if (n.type === 'leaf') fn(n.panel);
    else for (var i = 0; i < n.children.length; i++) walk(n.children[i]);
  }
  walk(layoutRoot);
}

function countLeaves(n) {
  if (!n) return 0;
  if (n.type === 'leaf') return 1;
  var c = 0;
  for (var i = 0; i < n.children.length; i++) c += countLeaves(n.children[i]);
  return c;
}

function findPanelNode(panelId) {
  function walk(node, parent, index) {
    if (node.type === 'leaf') {
      if (node.panel.id === panelId) return { node: node, parent: parent, index: index };
      return null;
    }
    for (var i = 0; i < node.children.length; i++) {
      var f = walk(node.children[i], node, i);
      if (f) return f;
    }
    return null;
  }
  return layoutRoot ? walk(layoutRoot, null, 0) : null;
}

function normalizeRatios(children) {
  var s = 0, i;
  for (i = 0; i < children.length; i++) s += children[i].ratio;
  if (s <= 0) {
    for (i = 0; i < children.length; i++) children[i].ratio = 1 / children.length;
    return;
  }
  for (i = 0; i < children.length; i++) children[i].ratio /= s;
}

function childSizes(node, totalPx) {
  var n = node.children.length;
  var inner = Math.max(0, totalPx - (n - 1) * SPLIT_PX);
  var totalR = 0, i;
  for (i = 0; i < n; i++) totalR += node.children[i].ratio;
  if (totalR <= 0) totalR = n;
  var sizes = [], used = 0;
  for (i = 0; i < n; i++) {
    var sz = (i === n - 1) ? (inner - used) : Math.floor(inner * node.children[i].ratio / totalR);
    if (sz < 0) sz = 0;
    sizes.push(sz);
    used += sz;
  }
  return sizes;
}

function initLayout() {
  layoutRoot = {
    type: 'split',
    dir: 'h',
    children: [
      { type: 'leaf', ratio: 0.4, panel: makePanel('image') },
      { type: 'leaf', ratio: 0.6, panel: makePanel('plot') }
    ]
  };
  relayout();
}

function relayout() {
  var area = mainArea.getBoundingClientRect();
  var tH = topBar.offsetHeight;
  mainArea.querySelectorAll('.splitter').forEach(function(el) { el.remove(); });
  var splitters = [];
  if (layoutRoot) layoutNode(layoutRoot, 0, tH, area.width, Math.max(0, area.height - tH), splitters);
  splitters.forEach(function(s) {
    var el = document.createElement('div');
    el.className = 'splitter ' + (s.dir === 'h' ? 'splitter-v' : 'splitter-h');
    el.style.left = s.left + 'px';
    el.style.top = s.top + 'px';
    el.style.width = s.width + 'px';
    el.style.height = s.height + 'px';
    el.addEventListener('mousedown', function(e) { startDrag(s, e); });
    mainArea.appendChild(el);
  });
  if (typeof updateAllCharts === 'function') updateAllCharts();
}

function layoutNode(node, x, y, w, h, splitters) {
  if (node.type === 'leaf') {
    var p = node.panel;
    ensurePanelDOM(p);
    p.el.style.left = x + 'px';
    p.el.style.top = y + 'px';
    p.el.style.width = w + 'px';
    p.el.style.height = h + 'px';
    return;
  }
  var horiz = node.dir === 'h';
  var sizes = childSizes(node, horiz ? w : h);
  var cursor = horiz ? x : y;
  for (var i = 0; i < node.children.length; i++) {
    var sz = sizes[i];
    if (horiz) layoutNode(node.children[i], cursor, y, sz, h, splitters);
    else layoutNode(node.children[i], x, cursor, w, sz, splitters);
    if (i < node.children.length - 1) {
      var next = sizes[i + 1];
      if (horiz) {
        splitters.push({
          dir: 'h', left: cursor + sz, top: y, width: SPLIT_PX, height: h,
          node: node, index: i, origin: cursor, span: sz + SPLIT_PX + next
        });
      } else {
        splitters.push({
          dir: 'v', left: x, top: cursor + sz, width: w, height: SPLIT_PX,
          node: node, index: i, origin: cursor, span: sz + SPLIT_PX + next
        });
      }
    }
    cursor += sz + SPLIT_PX;
  }
}

function panelTypeSel(p) {
  return p.el ? p.el.querySelector('select.panel-type') : null;
}

function fillTypeSelect(sel, current) {
  sel.innerHTML = '';
  var specs = Rdbg.list();
  var seen = {};
  for (var i = 0; i < specs.length; i++) {
    var opt = document.createElement('option');
    opt.value = specs[i].id;
    opt.textContent = specs[i].title;
    if (specs[i].id === current) opt.selected = true;
    sel.appendChild(opt);
    seen[specs[i].id] = true;
  }
  if (current && !seen[current]) {
    var extra = document.createElement('option');
    extra.value = current;
    extra.textContent = current;
    extra.selected = true;
    sel.appendChild(extra);
  }
}

function mountPlugin(p) {
  var spec = Rdbg.get(p.type);
  if (!spec) return;
  var hdr = p.el.querySelector('.panel-header');
  var typeSel = panelTypeSel(p);
  var body = p.el.querySelector('.panel-body');
  if (spec.setupHeader) spec.setupHeader(p, hdr, typeSel);
  if (spec.setupBody) spec.setupBody(p, body);
  if (spec.init) spec.init(p);
}

function clearPanelContent(p) {
  var spec = Rdbg.get(p.type);
  if (spec && spec.destroy) spec.destroy(p);
  if (!p.el) return;
  var body = p.el.querySelector('.panel-body');
  if (body) {
    body.innerHTML = '';
    body.classList.remove('log-body');
  }
  p.legendDiv = null; p.chartEl = null; p.chart = null; p.manualView = false; p.fieldDiv = null;
  p.imgEl = null; p.placeholder = null; p.imgInfo = null; p.imgSel = null; p.imgFps = null;
  p.logDiv = null; p.logCount = null;
}

function ensurePanelDOM(p) {
  if (p.el) return;
  var el = document.createElement('div');
  el.className = 'panel';
  var hdr = document.createElement('div');
  hdr.className = 'panel-header';

  var typeSel = document.createElement('select');
  typeSel.className = 'panel-type';
  fillTypeSelect(typeSel, p.type);
  typeSel.onchange = function() { switchPanelType(p, typeSel.value); };
  hdr.appendChild(typeSel);

  var sp = document.createElement('span'); sp.style.flex = '1'; hdr.appendChild(sp);

  var dirs = [
    { sym: '\u25c0', title: '向左拆分', side: 'left' },
    { sym: '\u25b2', title: '向上拆分', side: 'up' },
    { sym: '\u25bc', title: '向下拆分', side: 'down' },
    { sym: '\u25b6', title: '向右拆分', side: 'right' },
  ];
  dirs.forEach(function(d) {
    var btn = document.createElement('button');
    btn.className = 'dir-btn'; btn.textContent = d.sym; btn.title = d.title;
    btn.onclick = function() { splitPanel(p.id, d.side); };
    hdr.appendChild(btn);
  });

  var closeBtn = document.createElement('button');
  closeBtn.textContent = '\u00d7'; closeBtn.title = '关闭';
  closeBtn.onclick = function() { removePanel(p.id); };
  hdr.appendChild(closeBtn);
  el.appendChild(hdr);

  var body = document.createElement('div');
  body.className = 'panel-body';
  el.appendChild(body);
  mainArea.appendChild(el);
  p.el = el;
  mountPlugin(p);
}

function switchPanelType(p, newType) {
  if (p.type === newType) return;
  clearPanelContent(p);
  p.type = newType;
  mountPlugin(p);
  var sel = panelTypeSel(p);
  if (sel) sel.value = p.type;
}

function splitPanel(panelId, side) {
  var found = findPanelNode(panelId);
  if (!found) return;
  var wantDir = (side === 'left' || side === 'right') ? 'h' : 'v';
  var before = (side === 'left' || side === 'up');
  var src = found.node.panel;
  var np = makePanel(src.type);
  var newLeaf = { type: 'leaf', ratio: 0.5, panel: np };
  var parent = found.parent;

  if (parent && parent.dir === wantDir) {
    var half = found.node.ratio / 2;
    found.node.ratio = half;
    newLeaf.ratio = half;
    parent.children.splice(found.index + (before ? 0 : 1), 0, newLeaf);
  } else {
    var keep = { type: 'leaf', ratio: 0.5, panel: src };
    var wrapped = {
      type: 'split',
      dir: wantDir,
      ratio: found.node.ratio,
      children: before ? [newLeaf, keep] : [keep, newLeaf]
    };
    if (!parent) layoutRoot = wrapped;
    else parent.children[found.index] = wrapped;
  }
  relayout();
}

function replaceNode(target, replacement) {
  replacement.ratio = target.ratio;
  if (layoutRoot === target) {
    layoutRoot = replacement;
    flattenSplit(layoutRoot);
    collapseUnary(layoutRoot, null, 0);
    return;
  }
  function walk(n) {
    if (!n || n.type !== 'split') return false;
    for (var i = 0; i < n.children.length; i++) {
      if (n.children[i] === target) {
        n.children[i] = replacement;
        flattenSplit(n);
        collapseUnary(layoutRoot, null, 0);
        return true;
      }
      if (walk(n.children[i])) return true;
    }
    return false;
  }
  walk(layoutRoot);
}

function flattenSplit(n) {
  if (!n || n.type !== 'split') return;
  var out = [], i, j;
  for (i = 0; i < n.children.length; i++) {
    flattenSplit(n.children[i]);
    var ch = n.children[i];
    if (ch.type === 'split' && ch.dir === n.dir) {
      var sub = ch.children;
      var rs = 0;
      for (j = 0; j < sub.length; j++) rs += sub[j].ratio;
      for (j = 0; j < sub.length; j++) {
        sub[j].ratio = ch.ratio * (rs > 0 ? sub[j].ratio / rs : 1 / sub.length);
        out.push(sub[j]);
      }
    } else {
      out.push(ch);
    }
  }
  n.children = out;
  if (n.children.length > 1) normalizeRatios(n.children);
}

function collapseUnary(n, parent, idx) {
  if (!n || n.type !== 'split') return;
  for (var i = n.children.length - 1; i >= 0; i--) collapseUnary(n.children[i], n, i);
  if (n.children.length === 1) {
    var only = n.children[0];
    only.ratio = parent ? n.ratio : 1;
    if (!parent) layoutRoot = only;
    else parent.children[idx] = only;
  }
}

function removePanel(panelId) {
  if (countLeaves(layoutRoot) <= 1) return;
  var found = findPanelNode(panelId);
  if (!found) return;
  var p = found.node.panel;
  var spec = Rdbg.get(p.type);
  if (spec && spec.destroy) spec.destroy(p);
  if (p.el) p.el.remove();
  delete panelMap[p.id];
  if (typeof activePanelId !== 'undefined' && activePanelId === p.id) activePanelId = null;
  if (!found.parent) return;
  found.parent.children.splice(found.index, 1);
  if (found.parent.children.length === 1) {
    replaceNode(found.parent, found.parent.children[0]);
  } else {
    normalizeRatios(found.parent.children);
  }
  relayout();
}

function startDrag(info, e) {
  e.preventDefault();
  var c0 = info.node.children[info.index];
  var c1 = info.node.children[info.index + 1];
  if (!c0 || !c1) return;
  var total = c0.ratio + c1.ratio;
  var origin = info.origin, span = info.span;
  if (span < 1) return;
  function onMove(ev) {
    var pos = (info.dir === 'h' ? ev.clientX : ev.clientY) - origin;
    var r0 = total * (pos / span);
    var min = total * 0.08;
    if (r0 < min) r0 = min;
    if (r0 > total - min) r0 = total - min;
    c0.ratio = r0;
    c1.ratio = total - r0;
    relayout();
  }
  function onUp() {
    document.removeEventListener('mousemove', onMove);
    document.removeEventListener('mouseup', onUp);
  }
  document.addEventListener('mousemove', onMove);
  document.addEventListener('mouseup', onUp);
}

function resetAllViews() {
  forEachPanel(function(p) {
    var spec = Rdbg.get(p.type);
    if (spec && spec.reset) spec.reset(p);
  });
}

function onSenderChange() {
  var name = document.getElementById('sender-sel').value;
  if (!name) return;
  plotData = [];
  forEachPanel(function(p) {
    if (p.chart) p.chart.data.datasets.length = 0;
    if (typeof rebuildPanelChart === 'function') rebuildPanelChart(p);
  });
  fetch('/select?sender=' + encodeURIComponent(name));
}

function clearPlots() {
  plotData = []; fieldMeta = {}; firstTs = null; lastX = 0; activePanelId = null;
  colorIdx = 0;
  logBuffer = [];
  var fl = document.getElementById('field-list');
  if (fl) fl.innerHTML = '<span style="color:#404050;font-size:13px">等待数据\u2026</span>';
  forEachPanel(function(p) {
    if (p.chart) { p.chart.data.datasets.length = 0; p.chart.update('none'); }
    if (p.legendDiv) p.legendDiv.innerHTML = '';
  });
  if (typeof refreshAllLogPanels === 'function') refreshAllLogPanels();
}

const dot = document.getElementById('dot'), stats = document.getElementById('stats');
var pktCount = 0, lastPktTime = Date.now(), lastAnyPkt = 0;
var clientTimeout = 4000;

function setConnected(c, sender) {
  var st = document.getElementById('status');
  if (!st || !dot) return;
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
      if (stats) stats.textContent = Math.round(pktCount * 1000 / (now - lastPktTime)) + ' pkt/s';
      pktCount = 0; lastPktTime = now;
    }
    var snd = msg._from || (msg.data && msg.data._from) || (msg.meta && msg.meta._from) || '';
    if (msg.type === 'plot') { addPoint(msg.ts, msg.data || {}); setConnected(true, snd); }
    else if (msg.type === 'image') { setImage(msg.jpg_b64, msg.meta); setConnected(true, snd); }
    else if (msg.type === 'log') { addLog(msg.ts, msg.level, msg.msg); setConnected(true, snd); }
    else if (msg.type === 'status') { setConnected(msg.connected, msg.sender); }
    else if (msg.type === 'state') {
      var sel = document.getElementById('sender-sel');
      if (!sel) return;
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
es.onerror = function() { if (dot) dot.className = 'dot dead'; };

window.addEventListener('resize', function() { relayout(); });
initLayout();
