// Image panel plugin — streams keyed by meta.name.
var imgSources = {};
var imgFpsTracker = {};

function applyImgTransform(p) {
  if (!p.imgEl) return;
  p.imgEl.style.transform = 'translate(-50%,-50%) translate(' + p.imgTx.toFixed(1) + 'px,' + p.imgTy.toFixed(1) + 'px) scale(' + p.imgScale.toFixed(3) + ')';
}

function resetImgView(p) {
  if (p.type !== 'image') return;
  p.imgScale = 1; p.imgTx = 0; p.imgTy = 0;
  applyImgTransform(p);
}

function showPanelImage(p) {
  if (p.type !== 'image' || !p.imgSel) return;
  var name = p.imgSel.value;
  if (!name) { var keys = Object.keys(imgSources); if (keys.length > 0) { name = keys[0]; p.imgSel.value = name; } }
  var s = imgSources[name];
  if (s && s.b64) {
    p.imgEl.src = s.b64; p.imgEl.style.display = '';
    p.placeholder.style.display = 'none'; p.imgInfo.textContent = s.kb + ' KB';
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

function setImage(b64, meta) {
  var name = (meta && meta.name) ? meta.name : 'default';
  var now = Date.now();

  if (!imgFpsTracker[name]) imgFpsTracker[name] = [];
  imgFpsTracker[name].push(now);
  while (imgFpsTracker[name].length > 10) imgFpsTracker[name].shift();
  var fps = 0;
  if (imgFpsTracker[name].length >= 2) {
    var dt = now - imgFpsTracker[name][0];
    fps = Math.round((imgFpsTracker[name].length - 1) * 1000 / dt);
  }

  imgSources[name] = { b64: 'data:image/jpeg;base64,' + b64, ts: now, kb: (b64.length * 0.75 / 1024).toFixed(0) };

  forEachPanel(function(p) {
    if (p.type !== 'image') return;
    refreshImgOptions(p);
    if (p.imgFps) p.imgFps.textContent = fps + ' fps';
    if (p.imgSel.value === name) showPanelImage(p);
  });
}

Rdbg.registerPanel({
  id: 'image',
  title: '图像',
  setupHeader: function(p, hdr, typeSel) {
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
  },
  setupBody: function(p, body) {
    var img = document.createElement('img'); img.style.display = 'none'; body.appendChild(img);
    var ph = document.createElement('span'); ph.className = 'placeholder'; ph.textContent = '等待中\u2026'; body.appendChild(ph);
    var info = document.createElement('div'); info.className = 'panel-img-info'; body.appendChild(info);
    p.imgEl = img; p.placeholder = ph; p.imgInfo = info;
  },
  init: function(p) { initImagePanel(p); },
  destroy: function(p) {
    p.imgDragging = false;
    if (p.imgSel) { p.imgSel.remove(); p.imgSel = null; }
    if (p.imgFps) { p.imgFps.remove(); p.imgFps = null; }
  },
  reset: function(p) { resetImgView(p); }
});
