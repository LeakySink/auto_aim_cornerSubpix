// Replayer — local .rlog viewer (preload frames, centered chart, scrub)

const COLORS = [
  '#4fc3f7', '#ffb74d', '#81c784', '#e57373', '#ba68c8', '#4dd0e1',
  '#fff176', '#a1887f', '#90a4ae', '#f48fb1', '#ef5350', '#26c6da',
];

let session = null;
let frameUrls = [];
let duration = 0;
let currentT = 0;
let playing = false;
let playWall0 = 0;
let playT0 = 0;
let manualView = false;
let chart = null;
let fieldColors = {};
let hiddenFields = {};
let rafId = 0;

const playheadPlugin = {
  id: 'playhead',
  afterDraw(c) {
    if (!session) return;
    const xScale = c.scales.x;
    if (!xScale) return;
    const px = xScale.getPixelForValue(currentT);
    const area = c.chartArea;
    if (px < area.left || px > area.right) return;
    const ctx = c.ctx;
    ctx.save();
    ctx.strokeStyle = 'rgba(79,195,247,0.85)';
    ctx.lineWidth = 2;
    ctx.setLineDash([4, 3]);
    ctx.beginPath();
    ctx.moveTo(px, area.top);
    ctx.lineTo(px, area.bottom);
    ctx.stroke();
    ctx.restore();
  },
};

function fmtTime(t) {
  return t.toFixed(2);
}

function updateTimeLabel() {
  document.getElementById('time-label').textContent =
    fmtTime(currentT) + ' / ' + fmtTime(duration) + ' s';
}

function findFrameIndex(t) {
  const frames = session.frames;
  if (!frames.length) return -1;
  let lo = 0, hi = frames.length - 1;
  if (t <= frames[0].t) return 0;
  if (t >= frames[hi].t) return hi;
  while (lo < hi) {
    const mid = (lo + hi + 1) >> 1;
    if (frames[mid].t <= t) lo = mid;
    else hi = mid - 1;
  }
  return lo;
}

function showFrameAt(t) {
  const wrap = document.getElementById('img-wrap');
  const img = document.getElementById('frame-img');
  const ph = document.getElementById('img-placeholder');
  const metaEl = document.getElementById('img-meta');

  if (!session.frames.length) {
    wrap.classList.add('empty');
    ph.textContent = '无图像';
    metaEl.textContent = '';
    return;
  }

  const idx = findFrameIndex(t);
  const fr = session.frames[idx];
  const gen = ++frameGen;

  const meta = fr.meta || {};
  const parts = [];
  if (meta.cam) parts.push('cam=' + meta.cam);
  if (meta.name) parts.push(meta.name);
  parts.push('t=' + fmtTime(fr.t) + 's');
  parts.push('# frame ' + (idx + 1) + '/' + session.frames.length);
  metaEl.textContent = parts.join(' · ');

  function apply(url) {
    if (gen !== frameGen) return;
    if (!url) {
      wrap.classList.add('empty');
      ph.textContent = '图像加载失败';
      return;
    }
    wrap.classList.remove('empty');
    if (img.src !== url) img.src = url;
  }

  if (frameUrls[fr.i]) apply(frameUrls[fr.i]);
  else {
    wrap.classList.add('empty');
    ph.textContent = '加载图像…';
    loadFrame(fr.i).then(apply);
  }
  prefetchAround(idx);
}

const FRAME_CONC = 4;
let frameGen = 0;
const frameInflight = {};

function loadFrame(i) {
  if (frameUrls[i]) return Promise.resolve(frameUrls[i]);
  if (frameInflight[i]) return frameInflight[i];
  frameInflight[i] = fetch('/api/frame/' + i)
    .then(function(r) {
      if (!r.ok) throw new Error('frame ' + i);
      return r.blob();
    })
    .then(function(blob) {
      const url = URL.createObjectURL(blob);
      frameUrls[i] = url;
      return url;
    })
    .catch(function(err) {
      console.warn(err);
      return null;
    })
    .finally(function() {
      delete frameInflight[i];
    });
  return frameInflight[i];
}

function prefetchAround(idx) {
  const frames = session.frames;
  for (var d = 1; d <= 3; d++) {
    if (idx + d < frames.length) loadFrame(frames[idx + d].i);
    if (idx - d >= 0) loadFrame(frames[idx - d].i);
  }
}

async function warmupFrames() {
  if (!session.frames.length) return;
  var i = 0;
  async function worker() {
    while (i < session.frames.length) {
      var idx = i++;
      await loadFrame(session.frames[idx].i);
    }
  }
  var n = Math.min(FRAME_CONC, session.frames.length);
  await Promise.all(Array.from({ length: n }, worker));
}

function windowSec() {
  return parseFloat(document.getElementById('win-size').value) || 10;
}

function speedMul() {
  return parseFloat(document.getElementById('speed').value) || 1;
}

function updateChartView(force) {
  if (!chart) return;
  if (manualView && !force) {
    chart.update('none');
    return;
  }
  const win = windowSec();
  chart.options.scales.x.min = Math.max(0, currentT - win / 2);
  chart.options.scales.x.max = Math.max(win, currentT + win / 2);
  chart.update('none');
}

function seekTo(t, fromScrub) {
  currentT = Math.max(0, Math.min(duration, t));
  if (!fromScrub) {
    const scrub = document.getElementById('scrub');
    scrub.value = duration > 0 ? Math.round(currentT / duration * 1000) : 0;
  }
  updateTimeLabel();
  showFrameAt(currentT);
  updateChartView(false);
  if (playing) {
    playWall0 = performance.now();
    playT0 = currentT;
  }
}

function buildChart() {
  const ctx = document.getElementById('chart').getContext('2d');
  const datasets = [];
  session.fields.forEach(function(name, i) {
    const color = fieldColors[name];
    const pts = session.series[name] || [];
    datasets.push({
      label: name,
      data: pts.map(function(p) { return { x: p[0], y: p[1] }; }),
      borderColor: color,
      borderWidth: 1.8,
      pointRadius: 0,
      spanGaps: false,
      hidden: hiddenFields[name] !== false,
    });
  });

  if (chart) chart.destroy();
  chart = new Chart(ctx, {
    type: 'line',
    data: { datasets: datasets },
    plugins: [playheadPlugin],
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: false,
      interaction: { mode: 'nearest', intersect: false },
      plugins: {
        legend: { display: false },
        zoom: {
          zoom: {
            wheel: { enabled: true },
            pinch: { enabled: true },
            mode: 'x',
            onZoomStart: function() { manualView = true; },
          },
          pan: {
            enabled: true,
            mode: 'x',
            onPanStart: function() { manualView = true; },
          },
        },
      },
      scales: {
        x: {
          type: 'linear',
          title: { display: true, text: 'time (s)', color: '#606070' },
          ticks: { color: '#505060' },
          grid: { color: '#202030' },
          min: 0,
          max: windowSec(),
        },
        y: {
          title: { display: true, text: 'value', color: '#606070' },
          ticks: { color: '#505060' },
          grid: { color: '#202030' },
        },
      },
    },
  });
}

function renderFieldList() {
  const el = document.getElementById('field-list');
  el.innerHTML = '';
  session.fields.forEach(function(name) {
    const item = document.createElement('div');
    item.className = 'item';
    const box = document.createElement('div');
    const on = hiddenFields[name] === false;
    box.className = 'box' + (on ? ' on' : '');
    box.style.setProperty('--c', fieldColors[name]);
    item.appendChild(box);
    item.appendChild(document.createTextNode(name));
    item.onclick = function() {
      hiddenFields[name] = hiddenFields[name] === false ? true : false;
      const ds = chart.data.datasets.find(function(d) { return d.label === name; });
      if (ds) ds.hidden = hiddenFields[name] !== false;
      chart.update('none');
      renderFieldList();
    };
    el.appendChild(item);
  });
}

function setPlaying(on) {
  playing = on;
  document.getElementById('btn-play').textContent = on ? '⏸' : '▶';
  if (on) {
    playWall0 = performance.now();
    playT0 = currentT;
    cancelAnimationFrame(rafId);
    tick();
  } else {
    cancelAnimationFrame(rafId);
  }
}

function tick() {
  if (!playing) return;
  const elapsed = (performance.now() - playWall0) / 1000 * speedMul();
  let t = playT0 + elapsed;
  if (t >= duration) {
    t = duration;
    setPlaying(false);
  }
  seekTo(t, false);
  if (playing) rafId = requestAnimationFrame(tick);
}

function resetZoom() {
  manualView = false;
  if (chart && chart.resetZoom) chart.resetZoom();
  updateChartView(true);
}

function bindControls() {
  document.getElementById('btn-play').onclick = function() {
    setPlaying(!playing);
  };
  document.getElementById('btn-reset').onclick = resetZoom;
  document.getElementById('win-size').onchange = function() {
    manualView = false;
    updateChartView(true);
  };
  document.getElementById('speed').onchange = function() {
    if (playing) {
      playWall0 = performance.now();
      playT0 = currentT;
    }
  };

  const scrub = document.getElementById('scrub');
  scrub.oninput = function() {
    const t = (parseInt(scrub.value, 10) / 1000) * duration;
    seekTo(t, true);
  };
  scrub.onpointerdown = function() {
    if (playing) setPlaying(false);
  };

  window.addEventListener('keydown', function(e) {
    if (e.code === 'Space') {
      e.preventDefault();
      setPlaying(!playing);
    } else if (e.code === 'ArrowLeft') {
      seekTo(currentT - 0.05, false);
    } else if (e.code === 'ArrowRight') {
      seekTo(currentT + 0.05, false);
    }
  });
}

async function init() {
  const status = document.getElementById('title');
  try {
    status.textContent = '加载中…';
    const resp = await fetch('/api/meta');
    if (!resp.ok) throw new Error('meta HTTP ' + resp.status);
    session = await resp.json();
    duration = session.duration || 0;
    status.textContent = session.file || 'Replayer';
    status.title = session.path || '';

    session.fields.forEach(function(name, i) {
      fieldColors[name] = COLORS[i % COLORS.length];
      hiddenFields[name] = true;
    });

    frameUrls = new Array(session.frames.length);
    showFrameAt(0);
    updateTimeLabel();

    try {
      buildChart();
      renderFieldList();
      updateChartView(true);
    } catch (e) {
      console.error('chart init failed', e);
    }

    bindControls();
    seekTo(0, false);
    warmupFrames().catch(function(e) { console.warn('warmup', e); });
  } catch (e) {
    console.error(e);
    var msg = (e && e.message) ? String(e.message) : String(e);
    document.body.innerHTML =
      '<div style="padding:24px;color:#e57373;line-height:1.6">' +
      '<div style="font-size:16px;margin-bottom:8px">加载失败</div>' +
      '<div style="color:#9090a0;font-size:13px">' + msg + '</div>' +
      '<div style="color:#606070;font-size:12px;margin-top:12px">请用 ./host/replay.sh &lt;file.rlog&gt; 启动，并 Ctrl+Shift+R 硬刷新。</div>' +
      '</div>';
  }
}

init();
