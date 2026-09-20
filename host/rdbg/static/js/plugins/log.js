// Log panel plugin — shared buffer so replay.js can rebuild.
var LOG_CAP = 500;
var logBuffer = [];
var logLevelOn = { DEBUG: true, INFO: true, WARN: true, ERROR: true };
var replayLogCursorTs = null;  // ns; null = no cursor highlight
var replayLogLastKey = '';

function normLogLevel(lv) {
  lv = String(lv || 'INFO').toUpperCase();
  if (lv === 'WARNING') return 'WARN';
  if (lv === 'FATAL' || lv === 'CRITICAL') return 'ERROR';
  if (logLevelOn[lv] === undefined) return 'INFO';
  return lv;
}

function logLevelVisible(lv) {
  return !!logLevelOn[normLogLevel(lv)];
}

function syncLogFilterUI() {
  document.querySelectorAll('.log-lv-cb').forEach(function(cb) {
    cb.checked = !!logLevelOn[cb.value];
  });
}

function onLogFilterChange(ev) {
  var t = ev && ev.target;
  if (t && t.classList && t.classList.contains('log-lv-cb')) {
    logLevelOn[t.value] = t.checked;
  } else {
    document.querySelectorAll('.log-lv-cb').forEach(function(cb) {
      logLevelOn[cb.value] = cb.checked;
    });
  }
  syncLogFilterUI();
  refreshAllLogPanels();
}

document.addEventListener('change', function(ev) {
  if (ev.target && ev.target.classList && ev.target.classList.contains('log-lv-cb')) {
    onLogFilterChange(ev);
  }
});

function formatLogTs(ts) {
  var t = new Date(ts / 1e6);
  return t.toTimeString().slice(0, 8) + '.' + String(t.getMilliseconds()).padStart(3, '0');
}

function makeLogLine(entry) {
  var div = document.createElement('div');
  div.className = 'log-line';
  var lv = normLogLevel(entry.level);
  var msg = String(entry.msg == null ? '' : entry.msg);
  div.dataset.ts = String(entry.ts);
  div.innerHTML = '<span class="log-ts">' + formatLogTs(entry.ts) +
    '</span><span class="log-lv ' + lv + '">' + lv +
    '</span><span class="log-msg"></span>';
  div.querySelector('.log-msg').textContent = msg;
  if (typeof window.replaySeekByTs === 'function') {
    div.classList.add('log-clickable');
    div.title = '跳转到此时刻';
    div.onclick = function() {
      window.replaySeekByTs(Number(div.dataset.ts));
    };
  }
  return div;
}

function isLogStuckToBottom(el) {
  return el.scrollTop + el.clientHeight >= el.scrollHeight - 24;
}

function visibleLogEntries() {
  var out = [];
  for (var i = 0; i < logBuffer.length; i++) {
    if (logLevelVisible(logBuffer[i].level)) out.push(logBuffer[i]);
  }
  return out;
}

/** 按时间戳定位：返回 { idx, frac, onLine }
 *  idx = 不大于 cursor 的最后一条；frac∈[0,1] 到下一条的时间比例；
 *  onLine = 落在某条日志上（非行间）。
 */
function locateLogCursor(entries, cursorTs) {
  if (cursorTs == null || !entries.length) {
    return { idx: -1, frac: 0, onLine: false };
  }
  var lo = 0, hi = entries.length - 1, ans = -1;
  while (lo <= hi) {
    var mid = (lo + hi) >> 1;
    if (entries[mid].ts <= cursorTs) { ans = mid; lo = mid + 1; }
    else hi = mid - 1;
  }
  if (ans < 0) return { idx: -1, frac: 0, onLine: false };
  if (ans >= entries.length - 1) {
    return { idx: ans, frac: 0, onLine: true };
  }
  var t0 = entries[ans].ts;
  var t1 = entries[ans + 1].ts;
  var span = t1 - t0;
  if (span <= 0) return { idx: ans, frac: 0, onLine: true };
  var frac = (cursorTs - t0) / span;
  // 靠近端点视为「落在该行」，中间为行间
  var onLine = frac <= 0.12 || frac >= 0.88;
  if (frac >= 0.88) {
    return { idx: ans + 1, frac: 0, onLine: true };
  }
  if (frac <= 0.12) {
    return { idx: ans, frac: 0, onLine: true };
  }
  return { idx: ans, frac: frac, onLine: false };
}

function updateLogProgress(p, loc, total) {
  if (!p.logProgressThumb || total <= 0) return;
  var idx = loc.idx < 0 ? 0 : loc.idx;
  var pos = idx;
  if (!loc.onLine && loc.idx >= 0 && loc.idx < total - 1) pos = loc.idx + loc.frac;
  var pct = total <= 1 ? 0 : (pos / (total - 1)) * 100;
  p.logProgressThumb.style.top = pct + '%';
}

/** 根据时间戳把三角/高亮放到正确像素位置（可在行间）。 */
function placeLogTimeMarker(p, loc, entries) {
  if (!p.logDiv || !p.logCursorMarker) return;
  var kids = p.logDiv.children;
  var marker = p.logCursorMarker;
  marker.classList.remove('visible', 'between', 'on-line');

  for (var i = 0; i < kids.length; i++) {
    kids[i].classList.remove('log-current');
  }

  if (replayLogCursorTs == null || !entries.length) return;

  var scroll = p.logDiv.scrollTop;
  var y;

  if (loc.idx < 0) {
    // 早于第一条：三角在第一条上方
    var first = kids[0];
    if (!first) return;
    y = first.offsetTop - scroll;
    marker.classList.add('visible', 'between');
    marker.style.top = Math.max(0, y) + 'px';
    return;
  }

  if (loc.onLine) {
    var line = kids[loc.idx];
    if (!line) return;
    line.classList.add('log-current');
    marker.classList.add('on-line'); // 行上只用高亮，不画三角
    // 可选：仍把侧栏进度对齐
    return;
  }

  // 行间：插值到两条日志之间，画小三角
  var a = kids[loc.idx];
  var b = kids[loc.idx + 1];
  if (!a || !b) {
    if (a) a.classList.add('log-current');
    return;
  }
  var y0 = a.offsetTop + a.offsetHeight; // a 底边
  var y1 = b.offsetTop;                  // b 顶边
  // 行距中间带
  var yGap0 = a.offsetTop + a.offsetHeight * 0.5;
  var yGap1 = b.offsetTop + b.offsetHeight * 0.5;
  y = yGap0 + (yGap1 - yGap0) * loc.frac - scroll;
  marker.classList.add('visible', 'between');
  marker.style.top = y + 'px';
}

function ensureLogCursorInView(p, loc) {
  if (!p.logDiv || loc.idx < 0) return;
  var kids = p.logDiv.children;
  var el = kids[loc.onLine ? loc.idx : loc.idx];
  var el2 = !loc.onLine ? kids[loc.idx + 1] : null;
  var target = el2 || el;
  if (target) target.scrollIntoView({ block: 'nearest', behavior: 'auto' });
}

function replayLogsToPanel(p, stickBottom) {
  if (!p.logDiv) return;
  var stick = stickBottom === true || (stickBottom !== false && isLogStuckToBottom(p.logDiv));
  p.logDiv.innerHTML = '';
  var entries = visibleLogEntries();
  for (var i = 0; i < entries.length; i++) {
    p.logDiv.appendChild(makeLogLine(entries[i]));
  }
  p.logCount.textContent = entries.length ? String(entries.length) : '';
  var loc = locateLogCursor(entries, replayLogCursorTs);
  placeLogTimeMarker(p, loc, entries);
  updateLogProgress(p, loc, entries.length);
  if (window.REPLAY_MODE && loc.idx >= 0) {
    ensureLogCursorInView(p, loc);
  } else if (stick) {
    p.logDiv.scrollTop = p.logDiv.scrollHeight;
  }
}

function setReplayLogCursor(tsNs) {
  replayLogCursorTs = (tsNs == null || !isFinite(tsNs)) ? null : tsNs;
  var entries = visibleLogEntries();
  var loc = locateLogCursor(entries, replayLogCursorTs);
  var key = loc.idx + ':' + (loc.onLine ? 'L' : loc.frac.toFixed(3));
  var moved = key !== replayLogLastKey;
  replayLogLastKey = key;

  forEachPanel(function(p) {
    if (p.type !== 'log' || !p.logDiv) return;
    var kids = p.logDiv.children;
    if (kids.length !== entries.length) {
      replayLogsToPanel(p, false);
      return;
    }
    placeLogTimeMarker(p, loc, entries);
    updateLogProgress(p, loc, entries.length);
    if (moved) ensureLogCursorInView(p, loc);
  });
}

function refreshAllLogPanels() {
  forEachPanel(function(p) {
    if (p.type === 'log' && p.logDiv) replayLogsToPanel(p, !window.REPLAY_MODE);
  });
}

function addLog(ts, level, msg) {
  logBuffer.push({ ts: ts, level: level, msg: msg });
  if (!window.REPLAY_MODE) {
    while (logBuffer.length > LOG_CAP) logBuffer.shift();
  }
  var visible = logLevelVisible(level);
  forEachPanel(function(p) {
    if (p.type !== 'log' || !p.logDiv) return;
    if (window.REPLAY_MODE) {
      replayLogsToPanel(p, false);
      return;
    }
    var stick = isLogStuckToBottom(p.logDiv);
    if (visible) p.logDiv.appendChild(makeLogLine({ ts: ts, level: level, msg: msg }));
    while (p.logDiv.children.length > LOG_CAP) p.logDiv.firstChild.remove();
    var n = p.logDiv.children.length;
    p.logCount.textContent = n ? String(n) : '';
    if (stick) p.logDiv.scrollTop = p.logDiv.scrollHeight;
  });
}

function setupLogBody(p, body) {
  body.classList.add('log-body');
  var bar = document.createElement('div');
  bar.className = 'log-toolbar';
  ['DEBUG', 'INFO', 'WARN', 'ERROR'].forEach(function(lv) {
    var lab = document.createElement('label');
    var cb = document.createElement('input');
    cb.type = 'checkbox';
    cb.className = 'log-lv-cb';
    cb.value = lv;
    cb.checked = !!logLevelOn[lv];
    lab.appendChild(cb);
    lab.appendChild(document.createTextNode(lv));
    bar.appendChild(lab);
  });
  var lc = document.createElement('span');
  lc.className = 'log-count';
  bar.appendChild(lc);
  body.appendChild(bar);

  if (window.REPLAY_MODE) {
    var wrap = document.createElement('div');
    wrap.className = 'log-stream-wrap';
    var marker = document.createElement('div');
    marker.className = 'log-cursor-marker';
    wrap.appendChild(marker);
    var logs = document.createElement('div');
    logs.className = 'log-stream';
    wrap.appendChild(logs);
    body.appendChild(wrap);
    p.logDiv = logs;
    p.logCursorMarker = marker;
    p.logStreamWrap = wrap;

    logs.addEventListener('scroll', function() {
      if (replayLogCursorTs == null) return;
      var entries = visibleLogEntries();
      placeLogTimeMarker(p, locateLogCursor(entries, replayLogCursorTs), entries);
    });

    var track = document.createElement('div');
    track.className = 'log-progress';
    track.title = '日志时间进度（点击跳转）';
    var thumb = document.createElement('div');
    thumb.className = 'log-progress-thumb';
    track.appendChild(thumb);
    body.appendChild(track);
    p.logProgress = track;
    p.logProgressThumb = thumb;
    track.onclick = function(e) {
      if (typeof window.replaySeek !== 'function') return;
      var rect = track.getBoundingClientRect();
      var ratio = (e.clientY - rect.top) / Math.max(1, rect.height);
      ratio = Math.max(0, Math.min(1, ratio));
      var entries = visibleLogEntries();
      if (!entries.length) return;
      if (entries.length === 1) {
        window.replaySeekByTs(entries[0].ts);
        return;
      }
      var pos = ratio * (entries.length - 1);
      var i0 = Math.floor(pos);
      var i1 = Math.min(entries.length - 1, i0 + 1);
      var f = pos - i0;
      var ts = entries[i0].ts + (entries[i1].ts - entries[i0].ts) * f;
      window.replaySeekByTs(ts);
    };
  } else {
    var logs2 = document.createElement('div');
    logs2.className = 'log-stream';
    body.appendChild(logs2);
    p.logDiv = logs2;
  }

  p.logCount = lc;
  replayLogsToPanel(p, true);
}

Rdbg.registerPanel({
  id: 'log',
  title: '日志',
  setupBody: function(p, body) { setupLogBody(p, body); },
  destroy: function() {},
  reset: function() {}
});
