/** Client-side .rlog player — live-like sync: charts + images on one clock.
 *  - Playhead = recording time (actual rate)
 *  - Sliding/centered window follows playhead (near image ts)
 *  - Prefetch + decode cache for smooth video
 */

(function() {
  var PREFETCH = 30;
  var rp = null;
  var imgCache = {}; // idx -> HTMLImageElement

  function $(id) { return document.getElementById(id); }

  function fmtTime(s) {
    s = Math.max(0, Math.floor(s + 1e-6));
    var m = Math.floor(s / 60);
    var sec = s % 60;
    return m + ':' + (sec < 10 ? '0' : '') + sec;
  }

  function fmtRec(s) {
    if (!isFinite(s) || s < 0) s = 0;
    return s.toFixed(2) + 's';
  }

  /** Real-time playhead: seconds since first event (same axis as chart x). */
  function buildPlayTimes(events, t0) {
    var times = new Array(events.length);
    for (var i = 0; i < events.length; i++) {
      times[i] = ((events[i].ts || 0) - t0) / 1e9;
      if (times[i] < 0) times[i] = 0;
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

  function clearImgCache() {
    imgCache = {};
  }

  function prefetchFrom(imgIdx) {
    if (imgIdx == null || imgIdx < 0) return;
    var maxIdx = rp.nImg - 1;
    for (var k = 0; k <= PREFETCH; k++) {
      var id = imgIdx + k;
      if (id > maxIdx) break;
      if (imgCache[id]) continue;
      var im = new Image();
      im.decoding = 'async';
      imgCache[id] = im;
      im.src = '/img/' + id;
    }
  }

  function showImage(e) {
    if (!e || e.type !== 'image') return;
    var id = e.idx;
    prefetchFrom(id);
    var im = imgCache[id];
    var meta = e.meta || {};

    function paint(src) {
      setImageSrc(src, meta, '?');
    }

    if (im && im.complete && im.naturalWidth > 0) {
      paint(im.src);
      return;
    }
    if (im) {
      // Keep previous frame until decode finishes — avoids flicker/stutter.
      var done = false;
      im.onload = function() {
        if (done) return;
        done = true;
        if (rp && rp.lastImgIdx === id) paint(im.src);
      };
      // If already broken, fall through
      if (im.complete && im.naturalWidth === 0) paint('/img/' + id);
      return;
    }
    paint('/img/' + id);
  }

  function applyPlotOrLog(e) {
    if (e.type === 'plot') addPoint(e.ts, e.data || {});
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
    if (label) label.textContent = fmtTime(t) + ' / ' + fmtTime(dur) + '  ·  t=' + fmtRec(t);
    var btn = $('rp-play');
    if (btn) btn.textContent = rp.playing ? '\u23F8' : '\u25B6';
  }

  function currentPlayTime() {
    if (!rp) return 0;
    if (!rp.playing) return rp.playAnchor;
    var now = performance.now() / 1000;
    return rp.playAnchor + (now - rp.wallAnchor) * rp.speed;
  }

  /** Keep chart window glued to playhead (same second axis as image ts). */
  function syncChartWindow(playT) {
    firstTs = rp.t0;
    if (typeof setReplayClockX === 'function') setReplayClockX(playT);
    else if (typeof setReplayViewX === 'function') setReplayViewX(playT);
    else {
      lastX = playT;
      updateAllCharts();
    }
  }

  function tick() {
    if (!rp || !rp.playing) return;
    var target = currentPlayTime();
    if (target >= rp.duration && rp.idx >= rp.events.length) {
      rp.playing = false;
      rp.playAnchor = rp.duration;
      syncChartWindow(rp.duration);
      updateChrome(rp.duration);
      return;
    }

    // Advance events up to playhead. If several images are due, keep only the
    // latest (drop late frames) so decode cost cannot stall the clock.
    var pendingImg = null;
    while (rp.idx < rp.events.length && rp.playT[rp.idx] <= target) {
      var e = rp.events[rp.idx++];
      if (e.type === 'image') {
        pendingImg = e;
      } else {
        applyPlotOrLog(e);
      }
    }
    if (pendingImg) {
      rp.lastImgIdx = pendingImg.idx;
      showImage(pendingImg);
    }

    syncChartWindow(Math.min(target, rp.duration));
    updateChrome(target);

    if (rp.idx >= rp.events.length && target >= rp.duration) {
      rp.playing = false;
      rp.playAnchor = rp.duration;
      updateChrome(rp.duration);
      return;
    }
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
    var img = lastImageBefore(rp.idx);
    if (img) prefetchFrom(img.idx);
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
    syncChartWindow(rp.playAnchor);
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
      clearImgCache();
      firstTs = rp.t0;
      lastX = 0;
      rp.lastImgIdx = null;
      var i = 0;
      var lastImg = null;
      while (i < rp.events.length && rp.playT[i] < t) {
        var e = rp.events[i];
        if (e.type === 'image') lastImg = e;
        else applyPlotOrLog(e);
        i++;
      }
      rp.idx = i;
      if (lastImg) {
        rp.lastImgIdx = lastImg.idx;
        showImage(lastImg);
      }
    } else {
      var lo = 0, hi = rp.events.length;
      while (lo < hi) {
        var mid = (lo + hi) >> 1;
        if (rp.playT[mid] < t) lo = mid + 1;
        else hi = mid;
      }
      rp.idx = lo;
      var img2 = lastImageBefore(lo);
      if (img2) {
        rp.lastImgIdx = img2.idx;
        showImage(img2);
      }
    }

    rp.playAnchor = t;
    rp.wallAnchor = performance.now() / 1000;
    syncChartWindow(t);
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

    // Live-like window: sliding with "now" at the right edge (= image/playhead).
    var slide = document.querySelector('input[name="mode"][value="sliding"]');
    if (slide) {
      slide.checked = true;
      if (typeof onSettingsChange === 'function') onSettingsChange();
    }

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
      seekTo(t, { rebuild: true, autoplay: false });
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
    var q = new URLSearchParams(location.search);
    var speed = parseFloat(q.get('speed') || data.speed || 1) || 1;
    var events = data.events || [];
    var t0 = events.length ? (events[0].ts || 0) : 0;

    rp = {
      events: events,
      playT: buildPlayTimes(events, t0),
      duration: 0,
      idx: 0,
      playing: false,
      speed: speed,
      playAnchor: 0,
      wallAnchor: 0,
      dragging: false,
      raf: 0,
      t0: t0,
      nImg: data.n_img || 0,
      lastImgIdx: null,
    };
    rp.duration = rp.playT.length ? rp.playT[rp.playT.length - 1] : 0;
    // Count max image idx if needed
    if (!rp.nImg) {
      for (var i = 0; i < events.length; i++) {
        if (events[i].type === 'image' && events[i].idx != null)
          rp.nImg = Math.max(rp.nImg, events[i].idx + 1);
      }
    }

    firstTs = t0;

    var sel = $('rp-speed');
    if (sel) {
      var opts = ['0.25', '0.5', '1', '1.5', '2', '4'];
      var want = String(speed);
      if (opts.indexOf(want) < 0) { want = '1'; rp.speed = 1; }
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

    // Prefetch opening frames
    for (var j = 0; j < events.length; j++) {
      if (events[j].type === 'image') { prefetchFrom(events[j].idx); break; }
    }

    bindControls();
    updateChrome(0);
    play();
  }

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
