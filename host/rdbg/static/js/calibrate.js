(function () {
  var es = null;
  var pkt = 0;
  var lastPkt = 0;
  var status = {};

  var $ = function (id) { return document.getElementById(id); };

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
    ctx.fillStyle = "#16161e";
    ctx.fillRect(0, 0, w, h);
    ctx.strokeStyle = "#303040";
    ctx.strokeRect(0.5, 0.5, w - 1, h - 1);
    ctx.fillStyle = "#606070";
    ctx.font = "11px monospace";
    ctx.fillText("coverage XY", 8, 14);
    (samples || []).forEach(function (s) {
      var x = 4 + Math.max(0, Math.min(1, s.x || 0)) * (w - 8);
      var y = 4 + Math.max(0, Math.min(1, s.y || 0)) * (h - 8);
      var r = Math.max(3, (s.size || 0.1) * 28);
      ctx.strokeStyle = "#50b4e6";
      ctx.beginPath();
      ctx.arc(x, y, r, 0, Math.PI * 2);
      ctx.stroke();
      ctx.fillStyle = ctx.strokeStyle;
      ctx.beginPath();
      ctx.arc(x, y, 2, 0, Math.PI * 2);
      ctx.fill();
    });
  }

  function applyStatus(s) {
    status = s || {};
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
      cov.style.color = "#4ee06a";
    } else if ((status.n || 0) >= 10) {
      cov.textContent = "coverage low, still ok";
      cov.style.color = "#50b4e6";
    } else {
      cov.textContent = "need more poses";
      cov.style.color = "#707080";
    }

    var res = [];
    if (status.has_cam) res.push("reproj " + Number(status.reproj).toFixed(4) + " px");
    if (status.calibrated_at) res.push(status.calibrated_at);
    if (status.undistort) res.push("undistort ON");
    $("result").textContent = res.join(" · ");
    $("hint").textContent = status.hint || "";

    $("btn-c").disabled = (status.n || 0) < 10;
    $("btn-s").disabled = !status.has_cam;
    $("btn-u").disabled = !status.has_cam;
    $("btn-d").disabled = !(status.n > 0);
    $("btn-r").disabled = !(status.n > 0 || status.has_cam);
  }

  function showImage(b64, meta) {
    if (!b64) return;
    if (meta && meta.name && meta.name !== "calibrate") return;
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
      showImage(msg.jpg_b64, msg.meta);
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
    es = new EventSource("/events");
    es.onmessage = onEvent;
    es.onerror = function () {
      $("dot").className = "dot dead";
      $("status").textContent = "sse reconnecting…";
    };
  }

  function sendCmd(cmd) {
    fetch("/api/calib?cmd=" + encodeURIComponent(cmd)).catch(function () {});
  }

  document.querySelectorAll(".btns button").forEach(function (btn) {
    btn.addEventListener("click", function () {
      sendCmd(btn.getAttribute("data-cmd"));
    });
  });

  $("sender-sel").addEventListener("change", function () {
    fetch("/select?sender=" + encodeURIComponent($("sender-sel").value));
  });

  window.addEventListener("keydown", function (e) {
    if (e.target && (e.target.tagName === "INPUT" || e.target.tagName === "SELECT")) return;
    var map = {
      " ": "add", a: "add", A: "add",
      c: "calibrate", C: "calibrate",
      s: "save", S: "save",
      u: "undistort", U: "undistort",
      d: "drop", D: "drop",
      r: "reset", R: "reset"
    };
    if (map[e.key]) {
      e.preventDefault();
      sendCmd(map[e.key]);
    }
  });

  setInterval(function () {
    $("stats").textContent = (pkt - lastPkt) + " pkt/s";
    lastPkt = pkt;
  }, 1000);

  connect();
})();
