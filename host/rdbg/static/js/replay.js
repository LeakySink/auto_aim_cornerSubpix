// Replay driver — 全量显示曲线/日志；当前时刻用竖线/高亮；点击可跳转。
(function() {
  window.REPLAY_MODE = true;
  var duration = 0;
  var t0ns = 0;
  var currentT = 0;
  var playing = false;
  var speed = 1;
  var playWall0 = 0;
  var playT0 = 0;
  var rafId = 0;
  var plots = [];
  var logs = [];
  var frames = [];
  var fileName = 'replay';
  var jpegB64 = [];
  var inflight = {};
  var lastImg = {};
  var dataLoaded = false;

  function tsOf(t) {
    return t0ns + Math.round(t * 1e9);
  }

  function tOfTs(ts) {
    return (ts - t0ns) / 1e9;
  }

  function fmt(t) {
    return t.toFixed(2);
  }

  function keepAlive() {
    if (typeof lastAnyPkt !== 'undefined') lastAnyPkt = Date.now();
    if (typeof setConnected === 'function') setConnected(true, fileName);
  }

  function buildPlots(series) {
    var byT = {};
    Object.keys(series || {}).forEach(function(name) {
      (series[name] || []).forEach(function(p) {
        var t = p[0];
        var key = String(t);
        if (!byT[key]) byT[key] = { t: t, data: {} };
        byT[key].data[name] = p[1];
      });
    });
    return Object.keys(byT).map(function(k) { return byT[k]; })
      .sort(function(a, b) { return a.t - b.t; });
  }

  function loadJpeg(i) {
    if (jpegB64[i]) return Promise.resolve(jpegB64[i]);
    if (inflight[i]) return inflight[i];
    inflight[i] = fetch('/api/frame/' + i)
      .then(function(r) {
        if (!r.ok) throw new Error('frame ' + i);
        return r.arrayBuffer();
      })
      .then(function(buf) {
        var bytes = new Uint8Array(buf);
        var s = '';
        var step = 0x8000;
        for (var o = 0; o < bytes.length; o += step) {
          s += String.fromCharCode.apply(null, bytes.subarray(o, o + step));
        }
        jpegB64[i] = btoa(s);
        return jpegB64[i];
      })
      .catch(function(err) {
        console.warn(err);
        return null;
      })
      .finally(function() { delete inflight[i]; });
    return inflight[i];
  }

  function latestImages(t) {
    var latest = {};
    for (var i = 0; i < frames.length; i++) {
      var fr = frames[i];
      if (fr.t > t) break;
      var n = (fr.meta && fr.meta.name) ? fr.meta.name : 'default';
      latest[n] = fr;
    }
    return latest;
  }

  function applyImages(t) {
    var latest = latestImages(t);
    Object.keys(latest).forEach(function(name) {
      var fr = latest[name];
      if (lastImg[name] === fr.i) return;
      lastImg[name] = fr.i;
      loadJpeg(fr.i).then(function(b64) {
        if (!b64) return;
        if (lastImg[name] !== fr.i) return;
        setImage(b64, fr.meta || { name: name });
      });
    });
  }

  /** 一次性载入全部曲线与日志（含当前时刻之后），之后只移动游标。 */
  function loadAllData() {
    plotData = [];
    firstTs = t0ns;
    lastX = 0;
    fieldMeta = {};
    colorIdx = 0;
    logBuffer = [];

    for (var i = 0; i < plots.length; i++) {
      var p = plots[i];
      var pt = { x: p.t, fields: {} };
      for (var k in p.data) {
        if (typeof p.data[k] !== 'number') continue;
        ensureField(k);
        pt.fields[k] = p.data[k];
      }
      plotData.push(pt);
      lastX = p.t;
    }
    if (duration > lastX) lastX = duration;

    for (var j = 0; j < logs.length; j++) {
      logBuffer.push({
        ts: logs[j].ts || tsOf(logs[j].t),
        level: logs[j].level,
        msg: logs[j].msg,
      });
    }

    if (typeof rebuildAllCharts === 'function') rebuildAllCharts();
    if (typeof refreshAllLogPanels === 'function') refreshAllLogPanels();
    dataLoaded = true;
    keepAlive();
  }

  function updateCursor(t) {
    if (typeof setReplayCursor === 'function') setReplayCursor(t);
    if (typeof setReplayLogCursor === 'function') setReplayLogCursor(tsOf(t));
    if (typeof lastX !== 'undefined') lastX = t;
    if (typeof updateAllCharts === 'function') updateAllCharts();
  }

  function updateScrub() {
    var scrub = document.getElementById('replay-scrub');
    var label = document.getElementById('replay-time');
    if (label) label.textContent = fmt(currentT) + ' / ' + fmt(duration) + ' s';
    if (scrub && duration > 0) {
      scrub.value = String(Math.round(currentT / duration * 1000));
    }
  }

  function seek(t, fromScrub) {
    t = Math.max(0, Math.min(duration, t));
    currentT = t;
    updateCursor(t);
    applyImages(t);
    keepAlive();
    if (playing) {
      playWall0 = performance.now();
      playT0 = currentT;
    }
    if (!fromScrub) updateScrub();
    else {
      var label = document.getElementById('replay-time');
      if (label) label.textContent = fmt(currentT) + ' / ' + fmt(duration) + ' s';
    }
  }

  window.replaySeek = function(tSec) {
    if (playing) setPlaying(false);
    seek(tSec, false);
  };

  window.replaySeekByTs = function(tsNs) {
    if (playing) setPlaying(false);
    seek(tOfTs(tsNs), false);
  };

  function setPlaying(on) {
    playing = !!on;
    var btn = document.getElementById('replay-play');
    if (btn) btn.textContent = playing ? '\u275a\u275a' : '\u25b6';
    if (playing) {
      if (currentT >= duration - 1e-4) seek(0, false);
      playWall0 = performance.now();
      playT0 = currentT;
      if (!rafId) rafId = requestAnimationFrame(tick);
    }
  }

  function tick(now) {
    rafId = 0;
    if (!playing) return;
    var dt = (now - playWall0) / 1000 * speed;
    var t = playT0 + dt;
    if (t >= duration) {
      seek(duration, false);
      setPlaying(false);
      return;
    }
    currentT = t;
    updateCursor(currentT);
    applyImages(currentT);
    updateScrub();
    keepAlive();
    rafId = requestAnimationFrame(tick);
  }

  function bind() {
    var playBtn = document.getElementById('replay-play');
    var speedSel = document.getElementById('replay-speed');
    var scrub = document.getElementById('replay-scrub');
    if (playBtn) playBtn.onclick = function() { setPlaying(!playing); };
    if (speedSel) {
      speedSel.onchange = function() {
        speed = parseFloat(speedSel.value) || 1;
        if (playing) {
          playWall0 = performance.now();
          playT0 = currentT;
        }
      };
    }
    if (scrub) {
      scrub.oninput = function() {
        var t = (parseInt(scrub.value, 10) / 1000) * duration;
        seek(t, true);
      };
      scrub.onpointerdown = function() {
        if (playing) setPlaying(false);
      };
    }
    window.addEventListener('keydown', function(e) {
      var tag = (e.target && e.target.tagName) || '';
      if (tag === 'INPUT' || tag === 'SELECT' || tag === 'TEXTAREA') return;
      if (e.code === 'Space') {
        e.preventDefault();
        setPlaying(!playing);
      } else if (e.code === 'ArrowLeft') {
        seek(currentT - 0.05, false);
      } else if (e.code === 'ArrowRight') {
        seek(currentT + 0.05, false);
      }
    });
    window.onSenderChange = function() {};
    if (typeof es !== 'undefined') es.onerror = function() {};
    setInterval(keepAlive, 1000);
  }

  async function init() {
    bind();
    try {
      var resp = await fetch('/api/meta');
      if (!resp.ok) throw new Error('meta HTTP ' + resp.status);
      var meta = await resp.json();
      duration = meta.duration || 0;
      t0ns = meta.t0_ns || 0;
      if (typeof firstTs !== 'undefined') firstTs = t0ns;
      fileName = meta.file || meta.sender || 'replay';
      frames = meta.frames || [];
      logs = meta.logs || [];
      plots = buildPlots(meta.series || {});
      jpegB64 = new Array(frames.length);

      var sel = document.getElementById('sender-sel');
      if (sel) {
        sel.innerHTML = '';
        var opt = document.createElement('option');
        opt.value = fileName;
        opt.textContent = fileName;
        sel.appendChild(opt);
        sel.value = fileName;
      }
      document.title = fileName + ' · Replayer';
      var st = document.getElementById('status');
      if (st) st.textContent = '回放';

      keepAlive();
      loadAllData();
      seek(0, false);
      applyImages(0);
      if (frames.length && Object.keys(latestImages(0)).length === 0) {
        var first = {};
        frames.forEach(function(fr) {
          var n = (fr.meta && fr.meta.name) ? fr.meta.name : 'default';
          if (!first[n]) first[n] = fr;
        });
        Object.keys(first).forEach(function(name) {
          var fr = first[name];
          lastImg[name] = fr.i;
          loadJpeg(fr.i).then(function(b64) {
            if (b64) setImage(b64, fr.meta || { name: name });
          });
        });
      }
    } catch (e) {
      console.error(e);
      var msg = (e && e.message) ? e.message : String(e);
      document.getElementById('status').textContent = '加载失败';
      alert('回放加载失败: ' + msg);
    }
  }

  // 面板晚于 replay.js 创建时，等 shell 建好默认面板后再灌数据
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', function() { setTimeout(init, 0); });
  } else {
    setTimeout(init, 0);
  }
})();
