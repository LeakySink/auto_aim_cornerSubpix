/** Client-side .rlog player: seek / play-pause / speed. Requires debugger.js. */

(function() {
  var IMG_CAP = 1 / 25;
  var rp = null;

  function $(id) { return document.getElementById(id); }

  function fmtTime(s) {
    s = Math.max(0, Math.floor(s + 1e-6));
    var m = Math.floor(s / 60);
    var sec = s % 60;
    return m + ':' + (sec < 10 ? '0' : '') + sec;
  }

  function buildPlayTimes(events, cap) {
    var t = 0;
    var lastImgTs = null;
    var times = new Array(events.length);
    for (var i = 0; i < events.length; i++) {
      var e = events[i];
      if (e.type === 'image') {
        if (lastImgTs != null) {
          var raw = (e.ts - lastImgTs) / 1e9;
          if (raw < 0) raw = 0;
          t += Math.min(raw, cap);
        }
        lastImgTs = e.ts;
      }
      times[i] = t;
    }
    return times;
  }

  function clearLogs() {
    for (var ri = 0; ri < rows.length; ri++)
      for (var pi = 0; pi < rows[ri].panels.length; pi++) {
        var p = rows[ri].panels[pi];
        if (p.type === 'log' && p.logDiv) {
          p.logDiv.innerHTML = '';
          p.logEntries = 0;
          if (p.logCount) p.logCount.textContent = '0';
        }
      }
  }

  function applyEvent(e) {
    if (!e) return;
    if (e.type === 'plot') addPoint(e.ts, e.data || {});
    else if (e.type === 'image') setImageFromUrl('/img/' + e.idx, e.meta || {});
    else if (e.type === 'log') addLog(e.ts, e.level, e.msg);
  }

  function lastImageBefore(idx) {
    for (var i = Math.min(idx, rp.events.length) - 1; i >= 0; i--) {
      if (rp.events[i].type === 'image') return rp.events[i];
    }
    return null;
  }

  function updateChrome(playT) {
    if (!rp) return;
    var dur = rp.duration || 0;
    var t = Math.max(0, Math.min(playT, dur));
    if (!rp.dragging) {
      var el = $('rp-seek');
      if (el && dur > 0) el.value = String(Math.round(t / dur * 1000));
    }
    var label = $('rp-time');
    if (label) label.textContent = fmtTime(t) + ' / ' + fmtTime(dur);
    var btn = $('rp-play');
    if (btn) btn.textContent = rp.playing ? '\u23F8' : '\u25B6';
  }

  function currentPlayTime() {
    if (!rp) return 0;
    if (!rp.playing) return rp.playAnchor;
    var now = performance.now() / 1000;
    return rp.playAnchor + (now - rp.wallAnchor) * rp.speed;
  }

  function tick() {
    if (!rp || !rp.playing) return;
    var target = currentPlayTime();
    if (target >= rp.duration && rp.idx >= rp.events.length) {
      rp.playing = false;
      rp.playAnchor = rp.duration;
      updateChrome(rp.duration);
      return;
    }
    while (rp.idx < rp.events.length && rp.playT[rp.idx] <= target) {
      applyEvent(rp.events[rp.idx]);
      rp.idx++;
    }
    if (rp.idx >= rp.events.length) {
      rp.playing = false;
      rp.playAnchor = rp.duration;
      updateChrome(rp.duration);
      return;
    }
    updateChrome(target);
    rp.raf = requestAnimationFrame(tick);
  }

  function play() {
    if (!rp) return;
    if (rp.idx >= rp.events.length || rp.playAnchor >= rp.duration - 1e-6) {
      seekTo(0, { rebuild: true, autoplay: true });
      return;
    }
    rp.playing = true;
    rp.wallAnchor = performance.now() / 1000;
    updateChrome(rp.playAnchor);
    cancelAnimationFrame(rp.raf);
    rp.raf = requestAnimationFrame(tick);
  }

  function pause() {
    if (!rp) return;
    if (rp.playing) {
      rp.playAnchor = currentPlayTime();
      rp.playing = false;
    }
    cancelAnimationFrame(rp.raf);
    updateChrome(rp.playAnchor);
  }

  function togglePlay() {
    if (!rp) return;
    if (rp.playing) pause();
    else play();
  }

  function seekTo(t, opts) {
    opts = opts || {};
    if (!rp) return;
    t = Math.max(0, Math.min(t, rp.duration));
    var wasPlaying = rp.playing;
    rp.playing = false;
    cancelAnimationFrame(rp.raf);

    if (opts.rebuild !== false) {
      clearPlots();
      clearLogs();
      firstTs = null;
      var i = 0;
      var stride = opts.fast ? 4 : 1;
      while (i < rp.events.length && rp.playT[i] < t) {
        var e = rp.events[i];
        if (e.type === 'image' || e.type === 'log') applyEvent(e);
        else if (e.type === 'plot' && (stride === 1 || i % stride === 0)) applyEvent(e);
        i++;
      }
      rp.idx = i;
      var img = lastImageBefore(i);
      if (img) applyEvent(img);
    } else {
      // preview: only swap image
      var lo = 0, hi = rp.events.length;
      while (lo < hi) {
        var mid = (lo + hi) >> 1;
        if (rp.playT[mid] < t) lo = mid + 1;
        else hi = mid;
      }
      var img2 = lastImageBefore(lo);
      if (img2) applyEvent(img2);
      rp.idx = lo;
    }

    rp.playAnchor = t;
    rp.wallAnchor = performance.now() / 1000;
    updateChrome(t);
    if (opts.autoplay || (wasPlaying && opts.resume !== false && opts.rebuild !== false)) {
      play();
    }
  }

  function bindControls() {
    var bar = $('replay-bar');
    if (!bar) return;
    bar.hidden = false;
    document.body.classList.add('replay-mode');
    if (typeof relayout === 'function') relayout();

    $('rp-play').onclick = togglePlay;
    $('rp-speed').onchange = function() {
      var t = currentPlayTime();
      rp.speed = parseFloat(this.value) || 1;
      rp.playAnchor = t;
      rp.wallAnchor = performance.now() / 1000;
    };

    var seek = $('rp-seek');
    seek.addEventListener('pointerdown', function() {
      rp.dragging = true;
      pause();
    });
    seek.addEventListener('input', function() {
      var t = (parseInt(seek.value, 10) / 1000) * rp.duration;
      seekTo(t, { rebuild: false, resume: false });
      updateChrome(t);
    });
    function endDrag() {
      if (!rp || !rp.dragging) return;
      rp.dragging = false;
      var t = (parseInt(seek.value, 10) / 1000) * rp.duration;
      seekTo(t, { rebuild: true, fast: true, autoplay: false });
    }
    seek.addEventListener('pointerup', endDrag);
    seek.addEventListener('change', endDrag);

    window.addEventListener('keydown', function(ev) {
      if (ev.target && /input|select|textarea/i.test(ev.target.tagName)) return;
      if (ev.code === 'Space') {
        ev.preventDefault();
        togglePlay();
      }
    });
  }

  function startReplay(data) {
    IMG_CAP = data.img_cap || IMG_CAP;
    var q = new URLSearchParams(location.search);
    var speed = parseFloat(q.get('speed') || data.speed || 1) || 1;

    rp = {
      events: data.events || [],
      playT: [],
      duration: 0,
      idx: 0,
      playing: false,
      speed: speed,
      playAnchor: 0,
      wallAnchor: 0,
      dragging: false,
      raf: 0,
    };
    rp.playT = buildPlayTimes(rp.events, IMG_CAP);
    rp.duration = rp.playT.length ? rp.playT[rp.playT.length - 1] : 0;

    var sel = $('rp-speed');
    if (sel) {
      var opts = ['0.25', '0.5', '1', '1.5', '2', '4'];
      var want = String(speed);
      if (opts.indexOf(want) < 0) {
        // snap to nearest
        want = '1';
        rp.speed = 1;
      }
      sel.value = want;
    }

    setConnected(true, data.sender || data.file || 'rlog');
    var senderSel = $('sender-sel');
    if (senderSel) {
      senderSel.innerHTML = '';
      var opt = document.createElement('option');
      opt.value = data.sender || 'rlog';
      opt.textContent = data.sender || data.file || 'rlog';
      senderSel.appendChild(opt);
    }
    var st = $('status');
    if (st) st.textContent = '回放 \u2192 ' + (data.file || data.sender || 'rlog');
    clientTimeout = 1e12;

    bindControls();
    updateChrome(0);
    play();
  }

  // Prefer timeline API (replay server); fall back to live SSE.
  fetch('/api/timeline')
    .then(function(r) {
      if (!r.ok) throw new Error('no timeline');
      return r.json();
    })
    .then(startReplay)
    .catch(function() {
      if (typeof startLiveSSE === 'function') startLiveSSE();
    });
})();
