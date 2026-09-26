(function () {
  var es = null;
  var pkt = 0;
  var lastPkt = 0;
  var status = {};
  var doneSent = false;
  var iid = new URLSearchParams(location.search).get("i") || "";
  var expTimer = null;
  var expSynced = false;
  var lastImgTs = 0;  // 丢弃乱序/迟到旧帧，避免预览前后抖

  function api(path) {
    if (!iid) return path;
    if (path.indexOf("/api/") === 0) return "/api/i/" + iid + path.slice(4);
    return "/api/i/" + iid + path;
  }

  var $ = function (id) { return document.getElementById(id); };

  function setExpLabel(us) {
    $("exp-val").textContent = Math.round(Number(us)) + " us";
  }

  function sendExposure(us) {
    fetch(api("/api/calib"), {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ cmd: "set_exposure", exposure_us: Number(us) }),
    }).catch(function () {});
  }

  function setBar(idFill, idVal, v) {
    var pct = Math.max(0, Math.min(1, Number(v) || 0));
    var fill = $(idFill);
    fill.style.width = (pct * 100).toFixed(0) + "%";
    fill.classList.toggle("ready", pct >= 1);
    $(idVal).textContent = (pct * 100).toFixed(0) + "%";
  }

  function drawMap(samples) {
    var c = $("map");
    var ctx = c.getContext("2d");
    var w = c.width, h = c.height;
    ctx.fillStyle = "#111217";
    ctx.fillRect(0, 0, w, h);
    ctx.strokeStyle = "#2c3038";
    ctx.strokeRect(0.5, 0.5, w - 1, h - 1);
    ctx.fillStyle = "rgba(204, 204, 220, 0.4)";
    ctx.font = "11px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace";
    ctx.fillText("coverage XY", 8, 14);
    (samples || []).forEach(function (s) {
      var x = 4 + Math.max(0, Math.min(1, s.x || 0)) * (w - 8);
      var y = 4 + Math.max(0, Math.min(1, s.y || 0)) * (h - 8);
      var r = Math.max(3, (s.size || 0.1) * 28);
      ctx.strokeStyle = "#5e6ad2";
      ctx.beginPath();
      ctx.arc(x, y, r, 0, Math.PI * 2);
      ctx.stroke();
      ctx.fillStyle = ctx.strokeStyle;
      ctx.beginPath();
      ctx.arc(x, y, 2, 0, Math.PI * 2);
      ctx.fill();
    });
  }

  function fmtVec(v, cols) {
    if (!v || !v.length) return "";
    var lines = [];
    for (var i = 0; i < v.length; i += cols) {
      lines.push(v.slice(i, i + cols).map(function (x) {
        return Number(x).toFixed(6);
      }).join(", "));
    }
    return lines.join("\n");
  }

  function showDone(s) {
    var detail = [];
    detail.push("calibrated_at: " + (s.calibrated_at || ""));
    detail.push("reproj: " + Number(s.reproj).toFixed(4) + " px");
    detail.push("saved: " + (s.saved ? "yes" : "NO"));
    if (s.result_path) detail.push("path: " + s.result_path);
    if (s.camera_matrix) {
      detail.push("camera_matrix:");
      detail.push(fmtVec(s.camera_matrix, 3));
    }
    if (s.distort_coeffs) {
      detail.push("distort_coeffs:");
      detail.push(fmtVec(s.distort_coeffs, 5));
    }
    $("result-detail").textContent = detail.join("\n");
    $("hint").textContent = s.hint || (iid ? "done — quitting robot" : "done — quitting robot & host");
    $("btn-c").disabled = true;
    $("btn-d").disabled = true;
    $("btn-r").disabled = true;
    document.querySelectorAll(".btns button").forEach(function (b) { b.disabled = true; });
    var exp = $("exp");
    if (exp) exp.disabled = true;

    if (doneSent) return;
    doneSent = true;
    fetch(api("/api/done"), { method: "POST" }).catch(function () {});
  }

  function applyStatus(s) {
    status = s || {};
    if (status.calib_done) {
      showDone(status);
      var res = [];
      if (status.has_cam) res.push("reproj " + Number(status.reproj).toFixed(4) + " px");
      if (status.calibrated_at) res.push(status.calibrated_at);
      if (status.saved) res.push("SAVED");
      $("result").textContent = res.join(" · ");
      return;
    }

    var board = $("board");
    if (status.board) {
      board.textContent = "BOARD OK";
      board.className = "pill ok";
    } else {
      board.textContent = "BOARD --";
      board.className = "pill dead";
    }
    $("counts").textContent = "samples " + (status.n || 0);
    setBar("bx", "vx", status.x);
    setBar("by", "vy", status.y);
    setBar("bs", "vs", status.size);
    setBar("bk", "vk", status.skew);
    drawMap(status.samples);

    var cov = $("cov");
    if (status.goodenough) {
      cov.textContent = "coverage READY";
      cov.className = "ready";
    } else if ((status.n || 0) >= (status.min_n || 20)) {
      cov.textContent = "coverage low, still ok";
      cov.className = "okish";
    } else {
      cov.textContent = "need more poses (min " + (status.min_n || 20) + ")";
      cov.className = "muted";
    }

    var res = [];
    if (status.has_cam) res.push("reproj " + Number(status.reproj).toFixed(4) + " px");
    if (status.calibrated_at) res.push(status.calibrated_at);
    $("result").textContent = res.join(" · ");
    $("hint").textContent = status.hint || "";

    $("btn-c").disabled = (status.n || 0) < (status.min_n || 20);
    $("btn-d").disabled = !(status.n > 0);
    $("btn-r").disabled = !(status.n > 0 || status.has_cam);

    var exp = $("exp");
    if (exp) {
      exp.disabled = !!status.calib_done;
      if (status.exposure_us != null && !expSynced) {
        expSynced = true;
        exp.value = String(status.exposure_us);
        setExpLabel(status.exposure_us);
      }
    }
  }

  function showImage(b64, meta, ts) {
    if (!b64) return;
    if (meta && meta.name && meta.name !== "calibrate") return;
    // UDP 分片重组可能让旧帧晚到；只显示更新的时间戳
    var t = Number(ts) || 0;
    if (t > 0) {
      if (t < lastImgTs) return;
      lastImgTs = t;
    }
    var img = $("frame");
    img.src = "data:image/jpeg;base64," + b64;
    img.style.display = "block";
    $("placeholder").style.display = "none";
  }

  function onEvent(ev) {
    var msg;
    try { msg = JSON.parse(ev.data); } catch (e) { return; }
    pkt++;
    if (msg.type === "state") {
      var sel = $("sender-sel");
      var cur = sel.value;
      sel.innerHTML = "";
      (msg.senders || []).forEach(function (n) {
        var o = document.createElement("option");
        o.value = n; o.textContent = n;
        if (n === (msg.active_sender || cur)) o.selected = true;
        sel.appendChild(o);
      });
      $("dot").className = "dot" + ((msg.senders || []).length ? "" : " dead");
      $("status").textContent = (msg.senders || []).length
        ? ("linked · " + (msg.active_sender || ""))
        : "waiting for robot beacon";
      return;
    }
    if (msg.type === "image") {
      showImage(msg.jpg_b64, msg.meta, msg.ts);
      return;
    }
    if (msg.type === "plot" && msg.data && msg.data.calib) {
      applyStatus(msg.data);
      return;
    }
    if (msg.calib) applyStatus(msg);
  }

  function connect() {
    if (es) es.close();
    es = new EventSource(api("/events"));
    es.onmessage = onEvent;
    es.onerror = function () {
      $("dot").className = "dot dead";
      $("status").textContent = "sse reconnecting…";
    };
  }

  function sendCmd(cmd) {
    fetch(api("/api/calib") + "?cmd=" + encodeURIComponent(cmd)).catch(function () {});
  }

  document.querySelectorAll(".btns button").forEach(function (btn) {
    btn.addEventListener("click", function () {
      sendCmd(btn.getAttribute("data-cmd"));
    });
  });

  (function setupExposure() {
    var exp = $("exp");
    if (!exp) return;
    setExpLabel(exp.value);
    exp.addEventListener("input", function () {
      setExpLabel(exp.value);
      if (expTimer) clearTimeout(expTimer);
      expTimer = setTimeout(function () {
        sendExposure(exp.value);
      }, 120);
    });
  })();

  $("sender-sel").addEventListener("change", function () {
    fetch(api("/select") + "?sender=" + encodeURIComponent($("sender-sel").value));
  });

  window.addEventListener("keydown", function (e) {
    if (e.target && (e.target.tagName === "INPUT" || e.target.tagName === "SELECT")) return;
    var map = {
      " ": "add", a: "add", A: "add",
      c: "calibrate", C: "calibrate",
      d: "drop", D: "drop",
      r: "reset", R: "reset"
    };
    if (map[e.key]) {
      e.preventDefault();
      sendCmd(map[e.key]);
    }
  });

  if (iid) {
    window.addEventListener("pagehide", function () {
      var url = "/api/instances/" + iid + "/stop?forget=1";
      if (navigator.sendBeacon) navigator.sendBeacon(url, "");
    });
  }

  setInterval(function () {
    $("stats").textContent = (pkt - lastPkt) + " pkt/s";
    lastPkt = pkt;
  }, 1000);

  (function setupSplitter() {
    var KEY = "rdbg.calibrate.panelW";
    var panel = $("panel");
    var split = $("splitter");
    var main = document.querySelector("main");
    if (!panel || !split || !main) return;

    function clamp(w) {
      var max = Math.floor(main.clientWidth * 0.7);
      return Math.max(200, Math.min(max, Math.floor(w)));
    }

    function apply(w) {
      panel.style.width = clamp(w) + "px";
    }

    try {
      var saved = parseInt(localStorage.getItem(KEY) || "", 10);
      if (saved) apply(saved);
      else apply(280);
    } catch (e) {
      apply(280);
    }

    var drag = null;
    split.addEventListener("pointerdown", function (e) {
      drag = {
        startX: e.clientX,
        startW: panel.getBoundingClientRect().width,
        id: e.pointerId,
      };
      split.classList.add("active");
      document.body.classList.add("resizing");
      try { split.setPointerCapture(e.pointerId); } catch (err) {}
      e.preventDefault();
    });

    function endDrag() {
      if (!drag) return;
      drag = null;
      split.classList.remove("active");
      document.body.classList.remove("resizing");
      try {
        localStorage.setItem(KEY, String(Math.round(panel.getBoundingClientRect().width)));
      } catch (err) {}
    }

    split.addEventListener("pointermove", function (e) {
      if (!drag) return;
      // panel is on the right: dragging left grows panel
      apply(drag.startW - (e.clientX - drag.startX));
    });
    split.addEventListener("pointerup", endDrag);
    split.addEventListener("pointercancel", endDrag);

    window.addEventListener("resize", function () {
      apply(panel.getBoundingClientRect().width);
    });
  })();

  connect();
})();
