// Log panel plugin — shared buffer so replay.js can rebuild.
var LOG_CAP = 500;
var logBuffer = [];
var logLevelOn = { DEBUG: true, INFO: true, WARN: true, ERROR: true };

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
  div.innerHTML = '<span class="log-ts">' + formatLogTs(entry.ts) +
    '</span><span class="log-lv ' + lv + '">' + lv +
    '</span><span class="log-msg"></span>';
  div.querySelector('.log-msg').textContent = msg;
  return div;
}

function isLogStuckToBottom(el) {
  return el.scrollTop + el.clientHeight >= el.scrollHeight - 24;
}

function replayLogsToPanel(p, stickBottom) {
  if (!p.logDiv) return;
  p.logDiv.innerHTML = '';
  var n = 0;
  for (var i = 0; i < logBuffer.length; i++) {
    if (!logLevelVisible(logBuffer[i].level)) continue;
    p.logDiv.appendChild(makeLogLine(logBuffer[i]));
    n++;
  }
  p.logCount.textContent = n ? String(n) : '';
  if (stickBottom !== false) p.logDiv.scrollTop = p.logDiv.scrollHeight;
}

function refreshAllLogPanels() {
  forEachPanel(function(p) {
    if (p.type === 'log' && p.logDiv) replayLogsToPanel(p, true);
  });
}

function addLog(ts, level, msg) {
  logBuffer.push({ ts: ts, level: level, msg: msg });
  while (logBuffer.length > LOG_CAP) logBuffer.shift();
  var visible = logLevelVisible(level);
  forEachPanel(function(p) {
    if (p.type !== 'log' || !p.logDiv) return;
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
  replayLogsToPanel(p, true);
}

Rdbg.registerPanel({
  id: 'log',
  title: '日志',
  setupBody: function(p, body) { setupLogBody(p, body); },
  destroy: function() {},
  reset: function() {}
});
