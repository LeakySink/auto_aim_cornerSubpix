// Log panel plugin — shared buffer so replay.js can rebuild.
var LOG_CAP = 500;
var logBuffer = [];
var logLevelOn = { DEBUG: true, INFO: true, WARN: true, ERROR: true };
var replayLogCursorTs = null;  // ns; null = no cursor highlight
var replayLogLastIdx = -1;

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

function findCurrentLogIndex(entries, cursorTs) {
  if (cursorTs == null || !entries.length) return -1;
  var lo = 0, hi = entries.length - 1, ans = -1;
  while (lo <= hi) {
    var mid = (lo + hi) >> 1;
    if (entries[mid].ts <= cursorTs) { ans = mid; lo = mid + 1; }
    else hi = mid - 1;
  }
  return ans;
}

function updateLogProgress(p, curIdx, total) {
  if (!p.logProgressThumb) return;
  if (total <= 1 || curIdx < 0) {
    p.logProgressThumb.style.top = '0%';
    return;
  }
  var pct = (curIdx / (total - 1)) * 100;
  p.logProgressThumb.style.top = pct + '%';
}

function replayLogsToPanel(p, stickBottom) {
  if (!p.logDiv) return;
  var stick = stickBottom === true || (stickBottom !== false && isLogStuckToBottom(p.logDiv));
  p.logDiv.innerHTML = '';
  var entries = visibleLogEntries();
  var curIdx = findCurrentLogIndex(entries, replayLogCursorTs);
  var currentEl = null;
  for (var i = 0; i < entries.length; i++) {
    var line = makeLogLine(entries[i]);
    if (i === curIdx) {
      line.classList.add('log-current');
      currentEl = line;
    }
    p.logDiv.appendChild(line);
  }
  p.logCount.textContent = entries.length ? String(entries.length) : '';
  updateLogProgress(p, curIdx, entries.length);
  if (window.REPLAY_MODE && currentEl) {
    currentEl.scrollIntoView({ block: 'nearest', behavior: 'auto' });
  } else if (stick) {
    p.logDiv.scrollTop = p.logDiv.scrollHeight;
  }
}

function setReplayLogCursor(tsNs) {
  replayLogCursorTs = (tsNs == null || !isFinite(tsNs)) ? null : tsNs;
  var entries = visibleLogEntries();
  var curIdx = findCurrentLogIndex(entries, replayLogCursorTs);
  var moved = curIdx !== replayLogLastIdx;
  replayLogLastIdx = curIdx;
  forEachPanel(function(p) {
    if (p.type !== 'log' || !p.logDiv) return;
    var kids = p.logDiv.children;
    // 若过滤后行数与 buffer 可见数不一致则整表重建
    if (kids.length !== entries.length) {
      replayLogsToPanel(p, false);
      return;
    }
    var currentEl = null;
    for (var i = 0; i < kids.length; i++) {
      kids[i].classList.toggle('log-current', i === curIdx);
      if (i === curIdx) currentEl = kids[i];
    }
    updateLogProgress(p, curIdx, kids.length);
    if (moved && currentEl) {
      currentEl.scrollIntoView({ block: 'nearest', behavior: 'auto' });
    }
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
  var logs = document.createElement('div');
  logs.className = 'log-stream';
  body.appendChild(logs);
  p.logDiv = logs;
  p.logCount = lc;

  if (window.REPLAY_MODE) {
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
      var idx = Math.round(ratio * (entries.length - 1));
      window.replaySeekByTs(entries[idx].ts);
    };
  }

  replayLogsToPanel(p, true);
}

Rdbg.registerPanel({
  id: 'log',
  title: '日志',
  setupBody: function(p, body) { setupLogBody(p, body); },
  destroy: function() {},
  reset: function() {}
});
