// Night Lamp IoT dashboard: subscribes over MQTT/WebSockets and renders
// device status, latest values, two time-series charts and device health.
// No build step, no backend: open index.html or serve the folder.
(() => {
  "use strict";

  const CFG = Object.assign({}, window.DASHBOARD_CONFIG);
  // ?broker=ws://localhost:8888 points the page at another broker (local testing).
  const brokerParam = new URLSearchParams(location.search).get("broker");
  if (brokerParam && /^wss?:\/\//.test(brokerParam)) CFG.brokerUrl = brokerParam;
  const LATE_MS = 3000;             // a reading older than this on arrival was buffered
  const KEEP_MS = 2 * 3600 * 1000;  // history kept in the page
  const LEVEL_MAX = 4095, LAMP_MAX = 255;
  const BAD_RESETS = { task_watchdog: "watchdog", int_watchdog: "watchdog", watchdog: "watchdog",
                       panic: "crash", brownout: "power dip" };

  const state = {
    devices: new Map(),
    selected: null,
    userPicked: false,
    rangeMs: 15 * 60 * 1000,
    brokerUp: false,
    hover: null,               // reading under the crosshair
    demo: null,
    startedAt: Date.now(),
  };

  const $ = (id) => document.getElementById(id);
  const el = (tag, cls, text) => {
    const n = document.createElement(tag);
    if (cls) n.className = cls;
    if (text != null) n.textContent = text;   // all device data goes in as text, never HTML
    return n;
  };
  const SVG = "http://www.w3.org/2000/svg";
  const sv = (tag, attrs, parent) => {
    const n = document.createElementNS(SVG, tag);
    for (const k in attrs) n.setAttribute(k, attrs[k]);
    if (parent) parent.appendChild(n);
    return n;
  };

  // ---- Formatting ----------------------------------------------------------
  const pad = (n) => String(n).padStart(2, "0");
  const clock = (t) => { const d = new Date(t); return `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`; };
  const hhmm = (t) => { const d = new Date(t); return `${pad(d.getHours())}:${pad(d.getMinutes())}`; };
  const ago = (ms) => {
    const s = Math.max(0, Math.round(ms / 1000));
    if (s < 60) return `${s} s ago`;
    if (s < 3600) return `${Math.floor(s / 60)} min ago`;
    return `${Math.floor(s / 3600)} h ${Math.floor((s % 3600) / 60)} min ago`;
  };
  const duration = (s) => {
    if (s < 60) return `${s} s`;
    if (s < 3600) return `${Math.floor(s / 60)} min`;
    if (s < 86400) return `${Math.floor(s / 3600)} h ${Math.floor((s % 3600) / 60)} min`;
    return `${Math.floor(s / 86400)} d ${Math.floor((s % 86400) / 3600)} h`;
  };
  const lightWord = (lvl) => lvl < 1000 ? "bright" : lvl < 2200 ? "dim" : lvl < 3400 ? "dark" : "very dark";
  const rssiWord = (r) => r >= -55 ? "excellent" : r >= -67 ? "good" : r >= -75 ? "fair" : "weak";

  // ---- Device model ----------------------------------------------------------
  function device(id) {
    if (!state.devices.has(id)) {
      state.devices.set(id, {
        id, status: null, statusHistory: [], health: null, healthAt: 0,
        readings: [], lastSeq: null, missing: 0, duplicates: 0, late: 0, received: 0,
        lastMsgAt: 0, events: [], lateBurst: null, demo: false,
      });
    }
    return state.devices.get(id);
  }

  function addEvent(d, text, t = Date.now()) {
    d.events.push({ t, text });
    if (d.events.length > 100) d.events.shift();
  }

  function setStatus(d, s, t, retained) {
    if (d.status === s) return;
    const prev = d.status;
    d.status = s;
    if (retained && prev === null) {
      addEvent(d, `Status on record: ${s}`, t);
      return;
    }
    d.statusHistory.push({ t, s });
    addEvent(d, s === "online" ? "Device came online" : "Device went offline (broker published its last will)", t);
  }

  function addReading(d, r, receivedAt) {
    const age = Number(r.age) || 0;
    const late = age > LATE_MS;
    if (d.lastSeq !== null) {
      if (r.why === "boot" && r.seq < d.lastSeq) {
        addEvent(d, "Device restarted (sequence numbers began again)", receivedAt);
        d.lastSeq = null;
      } else if (r.seq <= d.lastSeq) {
        d.duplicates++;
        return;
      } else if (r.seq > d.lastSeq + 1) {
        const gap = r.seq - d.lastSeq - 1;
        d.missing += gap;
        addEvent(d, `${gap} reading(s) missing (seq ${d.lastSeq + 1}–${r.seq - 1})`, receivedAt);
      }
    }
    d.lastSeq = r.seq;
    d.received++;
    d.lastMsgAt = Math.max(d.lastMsgAt, receivedAt);
    const reading = { seq: r.seq, t: receivedAt - age, level: r.level, lamp: r.lamp, why: r.why || "", age, late };
    const arr = d.readings;
    if (!arr.length || arr[arr.length - 1].t <= reading.t) arr.push(reading);
    else { arr.push(reading); arr.sort((a, b) => a.t - b.t); }
    while (arr.length && arr[0].t < receivedAt - KEEP_MS) arr.shift();

    if (late) {
      d.late++;
      // Group a backlog flush into one event.
      const b = d.lateBurst || (d.lateBurst = { n: 0, maxAge: 0, at: receivedAt });
      b.n++; b.maxAge = Math.max(b.maxAge, age);
    } else if (d.lateBurst) {
      flushLateBurst(d);
    }
  }

  function flushLateBurst(d) {
    const b = d.lateBurst;
    if (!b) return;
    addEvent(d, `${b.n} buffered reading(s) delivered after reconnect (oldest waited ${Math.round(b.maxAge / 1000)} s)`, b.at);
    d.lateBurst = null;
  }

  function setHealth(d, h, receivedAt, retained) {
    const isNewBoot = !d.health || d.health.boots !== h.boots;
    d.health = h;
    d.healthAt = h.ts ? h.ts * 1000 : receivedAt;
    if (!retained) d.lastMsgAt = Math.max(d.lastMsgAt, receivedAt);
    if (isNewBoot && BAD_RESETS[h.reset]) {
      addEvent(d, `Last restart was caused by a ${BAD_RESETS[h.reset]} (reset reason "${h.reset}", boot #${h.boots})`, receivedAt);
    }
  }

  // ---- MQTT ------------------------------------------------------------------
  function handleMessage(topic, text, retained) {
    const parts = topic.split("/");
    const kind = parts.pop();
    const id = parts.pop();
    if (!id || !["reading", "health", "status"].includes(kind)) return;
    const d = device(id);
    const now = Date.now();
    try {
      if (kind === "status") setStatus(d, text === "online" ? "online" : "offline", now, retained);
      else if (kind === "health") setHealth(d, JSON.parse(text), now, retained);
      else addReading(d, JSON.parse(text), now);
    } catch (e) {
      addEvent(d, `Ignored a malformed ${kind} message`);
    }
    autoSelect();
    scheduleRender();
  }

  function setBroker(cls, text) {
    const p = $("broker-pill");
    p.className = `pill ${cls}`;
    p.querySelector(".pill-text").textContent = text;
  }

  function connectBroker() {
    $("broker-url").textContent = CFG.brokerUrl;
    $("topic-base").textContent = CFG.topicBase;
    if (!window.mqtt) { setBroker("bad", "MQTT library failed to load"); return; }
    const client = window.mqtt.connect(CFG.brokerUrl, {
      clientId: "dashboard-" + Math.random().toString(16).slice(2, 10),
      username: CFG.username || undefined,
      password: CFG.password || undefined,
      reconnectPeriod: 3000,      // the dashboard reconnects on its own too
      connectTimeout: 10000,
      clean: true,
    });
    client.on("connect", () => {
      state.brokerUp = true;
      setBroker("ok", "Broker connected");
      client.subscribe(`${CFG.topicBase}/+/+`, { qos: 1 });
    });
    client.on("reconnect", () => setBroker("", "Reconnecting to broker…"));
    client.on("close", () => { if (state.brokerUp) setBroker("bad", "Broker disconnected, retrying"); state.brokerUp = false; });
    client.on("error", () => setBroker("bad", "Broker error, retrying"));
    client.on("message", (topic, payload, packet) => handleMessage(topic, payload.toString(), packet.retain));
  }

  // ---- Device selection --------------------------------------------------------
  function rank(d) {
    return (d.status === "online" ? 2e13 : 0) + Math.max(d.lastMsgAt, d.healthAt);
  }
  function autoSelect() {
    const list = [...state.devices.values()].sort((a, b) => rank(b) - rank(a));
    const best = list[0];
    const cur = state.devices.get(state.selected);
    if (!state.selected || (!state.userPicked && best && best !== cur && rank(best) > rank(cur || { lastMsgAt: 0, healthAt: 0 }) && best.status === "online")) {
      state.selected = best ? best.id : null;
    }
  }
  function renderSelect() {
    const sel = $("device-select");
    const list = [...state.devices.values()].sort((a, b) => rank(b) - rank(a));
    const sig = list.map((d) => d.id + d.status).join("|") + state.selected;
    if (sel.dataset.sig === sig) return;
    sel.dataset.sig = sig;
    sel.replaceChildren();
    if (!list.length) { sel.appendChild(el("option", null, "Waiting for devices…")); return; }
    for (const d of list) {
      const o = el("option", null, `${d.id} · ${d.demo ? "demo" : d.status || "unknown"}`);
      o.value = d.id;
      o.selected = d.id === state.selected;
      sel.appendChild(o);
    }
  }

  // ---- Rendering ------------------------------------------------------------------
  let renderQueued = false;
  function scheduleRender() {
    if (renderQueued) return;
    renderQueued = true;
    requestAnimationFrame(() => { renderQueued = false; render(); });
  }

  function render() {
    renderSelect();
    const d = state.devices.get(state.selected);
    renderStatus(d);
    renderTiles(d);
    renderCharts(d);
    renderHealth(d);
    renderEvents(d);
    if (!$("table-view").hidden) renderTable(d);
  }

  function isStale(d) {
    return d.status === "online" && d.lastMsgAt && Date.now() - d.lastMsgAt > CFG.heartbeatMs * 2 + 10000;
  }

  function renderStatus(d) {
    const chip = $("device-status");
    const icon = chip.querySelector(".icon"), label = chip.querySelector(".label");
    const fresh = $("freshness");
    if (!d) {
      chip.className = "status-chip neutral"; icon.textContent = "…";
      const waited = Date.now() - state.startedAt > 8000;
      label.textContent = waited ? "No device online" : "Looking for devices…";
      fresh.textContent = waited ? "Start the firmware in Wokwi (see README), or try Demo data." : "";
      return;
    }
    const last = d.readings[d.readings.length - 1];
    if (isStale(d)) {
      chip.className = "status-chip stale"; icon.textContent = "!";
      label.textContent = "Online, but no data";
      fresh.textContent = `No message for ${ago(Date.now() - d.lastMsgAt).replace(" ago", "")}; expected at least every ${CFG.heartbeatMs / 1000} s.`;
    } else if (d.status === "online") {
      chip.className = "status-chip online"; icon.textContent = "✓";
      label.textContent = d.demo ? "Online (demo)" : "Online";
      fresh.textContent = last ? `Last reading ${ago(Date.now() - last.t)} · readings are sent on change, or at least every ${CFG.heartbeatMs / 1000} s` : "Connected, waiting for the first reading";
    } else if (d.status === "offline") {
      chip.className = "status-chip offline"; icon.textContent = "✕";
      label.textContent = "Offline";
      fresh.textContent = d.lastMsgAt ? `Last heard from ${ago(Date.now() - d.lastMsgAt)}. Readings taken while offline will arrive when it reconnects.`
                                      : "Last will on record. This device has not been seen since this page opened.";
    } else {
      chip.className = "status-chip neutral"; icon.textContent = "?";
      label.textContent = "Status unknown";
      fresh.textContent = "";
    }
  }

  function renderTiles(d) {
    const last = d && d.readings[d.readings.length - 1];
    $("tile-level").textContent = last ? last.level.toLocaleString() : "–";
    $("tile-level-note").textContent = last ? `${lightWord(last.level)} · 0 bright, 4095 dark` : "0 bright · 4095 dark";
    $("tile-lamp").textContent = last ? (last.lamp ? `${Math.round(last.lamp / 2.55)}%` : "Off") : "–";
    $("tile-lamp-note").textContent = last ? (last.lamp ? `on · PWM ${last.lamp} of 255` : "room is bright enough") : "brightness";
    $("tile-count").textContent = d ? d.received.toLocaleString() : "–";
    $("tile-count-note").textContent = d && d.late ? `${d.late} arrived late, after an outage` : "since this page opened";
    const note = $("tile-delivery-note");
    if (!d || !d.received) {
      $("tile-delivery").textContent = "–"; note.className = "tile-note"; note.textContent = "gaps in sequence numbers";
    } else if (d.missing === 0) {
      $("tile-delivery").textContent = "100%"; note.className = "tile-note good"; note.textContent = "✓ no missing readings";
    } else {
      const pct = (100 * d.received / (d.received + d.missing)).toFixed(1);
      $("tile-delivery").textContent = `${pct}%`; note.className = "tile-note bad"; note.textContent = `${d.missing} missing (sequence gaps)`;
    }
  }

  // ---- Charts ------------------------------------------------------------------------
  const charts = [
    { svg: () => $("chart-level"), key: "level", max: LEVEL_MAX, ticks: [0, 1000, 2000, 3000, 4095], cls: "level" },
    { svg: () => $("chart-lamp"),  key: "lamp",  max: LAMP_MAX,  ticks: [0, 64, 128, 192, 255],      cls: "lamp" },
  ];
  const M = { l: 46, r: 14, t: 10, b: 24 };

  function geometry(svg) {
    const W = svg.clientWidth || 600, H = svg.clientHeight || 170;
    const x1 = Date.now(), x0 = x1 - state.rangeMs;
    return {
      W, H, x0, x1,
      x: (t) => M.l + ((t - x0) / (x1 - x0)) * (W - M.l - M.r),
      t: (px) => x0 + ((px - M.l) / (W - M.l - M.r)) * (x1 - x0),
    };
  }

  function outageIntervals(d, x0, x1) {
    const out = [];
    let start = null;
    for (const h of d.statusHistory) {
      if (h.s === "offline" && start === null) start = h.t;
      if (h.s === "online" && start !== null) { out.push([start, h.t]); start = null; }
    }
    if (start !== null) out.push([start, x1]);
    return out.filter(([a, b]) => b > x0 && a < x1);
  }

  function drawChart(c, d) {
    const svg = c.svg();
    const g = geometry(svg);
    const y = (v) => g.H - M.b - (v / c.max) * (g.H - M.t - M.b);
    svg.setAttribute("viewBox", `0 0 ${g.W} ${g.H}`);
    svg.replaceChildren();

    if (d) for (const [a, b] of outageIntervals(d, g.x0, g.x1)) {
      const xa = Math.max(M.l, g.x(a)), xb = Math.min(g.W - M.r, g.x(b));
      sv("rect", { x: xa, y: M.t, width: Math.max(1, xb - xa), height: g.H - M.t - M.b, class: "outage-band" }, svg);
    }
    for (const v of c.ticks) {
      sv("line", { x1: M.l, x2: g.W - M.r, y1: y(v), y2: y(v), class: v === 0 ? "axis" : "grid" }, svg);
      const lbl = sv("text", { x: M.l - 8, y: y(v) + 4, "text-anchor": "end", class: "tick" }, svg);
      lbl.textContent = v.toLocaleString();
    }
    const stepMin = [1, 2, 5, 10, 15, 30].find((m) => state.rangeMs / (m * 60000) <= 6) || 30;
    const step = stepMin * 60000;
    for (let t = Math.ceil(g.x0 / step) * step; t <= g.x1; t += step) {
      const lbl = sv("text", { x: g.x(t), y: g.H - 6, "text-anchor": "middle", class: "tick" }, svg);
      lbl.textContent = hhmm(t);
    }

    const pts = d ? d.readings : [];
    let firstIdx = pts.findIndex((p) => p.t >= g.x0);
    if (firstIdx === -1) firstIdx = pts.length;
    const startIdx = Math.max(0, firstIdx - 1);   // carry the value in from the left edge
    const vis = pts.slice(startIdx);
    if (!vis.length) {
      const msg = sv("text", { x: (g.W + M.l) / 2, y: g.H / 2, "text-anchor": "middle", class: "empty-msg" }, svg);
      msg.textContent = d ? "No readings in this window" : "Waiting for data";
      return;
    }
    // Step line: a reading holds until the next one (report-by-exception).
    const clampX = (t) => Math.min(g.W - M.r, Math.max(M.l, g.x(t)));
    let dStr = `M${clampX(vis[0].t)},${y(vis[0][c.key])}`;
    for (let i = 1; i < vis.length; i++) {
      dStr += `H${clampX(vis[i].t)}V${y(vis[i][c.key])}`;
    }
    const lastP = vis[vis.length - 1];
    const live = d.status === "online" && !isStale(d);
    dStr += `H${clampX(live ? g.x1 : lastP.t)}`;
    sv("path", { d: dStr, class: `line-${c.cls}` }, svg);

    for (const p of vis) {
      if (p.late && p.t >= g.x0) sv("circle", { cx: g.x(p.t), cy: y(p[c.key]), r: 4, class: `late-${c.cls}` }, svg);
    }
    const h = state.hover;
    if (h && h.t >= g.x0) {
      sv("line", { x1: g.x(h.t), x2: g.x(h.t), y1: M.t, y2: g.H - M.b, class: "crosshair" }, svg);
      sv("circle", { cx: g.x(h.t), cy: y(h[c.key]), r: 5, class: `hover-dot-${c.cls}` }, svg);
    }
  }

  function renderCharts(d) { for (const c of charts) drawChart(c, d); }

  // Crosshair + tooltip, shared by both charts.
  function nearest(d, t) {
    const g0 = Date.now() - state.rangeMs;
    let best = null;
    for (const p of d.readings) {
      if (p.t < g0) continue;
      if (!best || Math.abs(p.t - t) < Math.abs(best.t - t)) best = p;
    }
    return best;
  }

  function showTooltip(p, svg, px) {
    const tt = $("tooltip");
    tt.replaceChildren();
    tt.appendChild(el("div", "tt-time", `${clock(p.t)} · seq ${p.seq}`));
    for (const [name, val, color] of [["light level", p.level.toLocaleString(), "var(--series-level)"],
                                       ["lamp", p.lamp ? `${p.lamp} / 255` : "off", "var(--series-lamp)"]]) {
      const row = el("div", "tt-row");
      const key = el("span", "tt-key"); key.style.background = color;
      row.append(key, el("strong", null, val), el("span", null, name));
      tt.appendChild(row);
    }
    const why = p.why === "change" ? "sent because the light changed" : p.why === "heartbeat" ? "heartbeat (no change)" : p.why === "boot" ? "first reading after boot" : "";
    tt.appendChild(el("div", "tt-note", p.late ? `buffered: arrived ${Math.round(p.age / 1000)} s late` : why));
    tt.hidden = false;
    const wrap = svg.parentElement.getBoundingClientRect();
    const r = svg.getBoundingClientRect();
    const left = Math.min(px + 14, wrap.width - tt.offsetWidth - 4);
    tt.style.left = `${Math.max(4, left)}px`;
    tt.style.top = `${r.top - wrap.top + 8}px`;
  }

  function hideTooltip() { state.hover = null; $("tooltip").hidden = true; scheduleRender(); }

  function wireChart(svg) {
    svg.addEventListener("pointermove", (e) => {
      const d = state.devices.get(state.selected);
      if (!d) return;
      const r = svg.getBoundingClientRect();
      const px = e.clientX - r.left;
      const p = nearest(d, geometry(svg).t(px));
      if (!p) return hideTooltip();
      state.hover = p;
      renderCharts(d);
      showTooltip(p, svg, geometry(svg).x(p.t));
    });
    svg.addEventListener("pointerleave", hideTooltip);
    svg.addEventListener("blur", hideTooltip);
    svg.addEventListener("keydown", (e) => {
      const d = state.devices.get(state.selected);
      if (!d || !d.readings.length) return;
      const vis = d.readings.filter((p) => p.t >= Date.now() - state.rangeMs);
      if (!vis.length) return;
      let i = state.hover ? vis.indexOf(state.hover) : vis.length;
      if (e.key === "ArrowLeft") i = Math.max(0, i - 1);
      else if (e.key === "ArrowRight") i = Math.min(vis.length - 1, i + 1);
      else if (e.key === "Escape") return hideTooltip();
      else return;
      e.preventDefault();
      state.hover = vis[i];
      renderCharts(d);
      showTooltip(vis[i], svg, geometry(svg).x(vis[i].t));
    });
  }

  function renderTable(d) {
    const body = $("table-body");
    body.replaceChildren();
    if (!d) return;
    for (const p of [...d.readings].reverse().slice(0, 300)) {
      const tr = el("tr");
      for (const v of [clock(p.t), p.seq, p.level, p.lamp, p.why, p.late ? `${Math.round(p.age / 1000)} s` : "live"]) tr.appendChild(el("td", null, String(v)));
      body.appendChild(tr);
    }
  }

  // ---- Health and events ------------------------------------------------------------
  function renderHealth(d) {
    const box = $("health");
    box.replaceChildren();
    $("health-age").textContent = d && d.health ? `reported ${ago(Date.now() - d.healthAt)}` : "";
    if (!d || !d.health) { box.appendChild(el("div", "empty", "No health report yet.")); return; }
    const h = d.health;
    const rows = [
      ["Firmware", `v${h.fw}`],
      ["Uptime", duration(h.up)],
      ["WiFi signal", `${h.rssi} dBm (${rssiWord(h.rssi)})`],
      ["Free memory", `${Math.round(h.heap / 1024)} KB`],
      ["CPU clock", `${h.cpu} MHz${h.cpu <= 80 ? " (power-saving)" : ""}`],
      ["Last restart", h.reset, BAD_RESETS[h.reset] ? `⚠ ${BAD_RESETS[h.reset]}` : null],
      ["Boot count", String(h.boots)],
      ["Offline buffer", `${h.buffered} waiting · ${h.dropped} dropped`],
      ["Reconnects", `WiFi ${h.wifi_reconnects} · MQTT ${h.mqtt_reconnects}`],
      ["Credentials from", h.creds === "nvs" ? "device storage (NVS)" : "secrets.h (development)"],
    ];
    for (const [k, v, flag] of rows) {
      box.appendChild(el("dt", null, k));
      const dd = el("dd", null, v);
      if (flag) dd.appendChild(el("span", "flag warn", flag));
      box.appendChild(dd);
    }
  }

  function renderEvents(d) {
    const ol = $("events");
    ol.replaceChildren();
    if (!d || !d.events.length) { ol.appendChild(el("li", "empty", "Nothing yet.")); return; }
    for (const e of [...d.events].sort((a, b) => b.t - a.t).slice(0, 40)) {
      const li = el("li");
      const time = el("time", null, clock(e.t));
      time.dateTime = new Date(e.t).toISOString();
      li.append(time, el("span", null, e.text));
      ol.appendChild(li);
    }
  }

  // ---- Demo mode: a simulated device, entirely in the browser ----------------------
  function startDemo() {
    if (state.demo) return;
    const d = device("demo-lamp");
    d.demo = true;
    const now = Date.now();
    const simStart = now - 20 * 60000;
    const sim = { lastLevel: -1e4, lastLampOn: false, lastAt: 0, seq: 1, buffer: [], offlineUntil: 0, boots: 1 };
    const lampFor = (lvl, on) => on ? Math.round(20 + (Math.min(Math.max(lvl, 1000), 3400) - 1000) * 235 / 2400) : 0;
    let lampOn = false;
    // Simulated light: bright, then dusk falls (around minute 9), the room
    // light is switched on for a while, and every 10 min after that.
    const lightAt = (t) => {
      const m = (t - simStart) / 60000;
      let v = 700 + 2600 / (1 + Math.exp(-(m - 9) / 1.5));
      const cycle = m % 10;
      if (m > 13 && cycle > 4 && cycle < 6.5) v -= 1500;
      return Math.max(300, Math.min(4000, Math.round(v + (Math.random() - 0.5) * 60)));
    };
    const evalAt = (t, receive) => {
      const lvl = lightAt(t);
      if (!lampOn && lvl >= 1200) lampOn = true;
      if (lampOn && lvl < 1000) lampOn = false;
      let why = null;
      if (sim.seq === 1) why = "boot";
      else if (Math.abs(lvl - sim.lastLevel) >= 100 || lampOn !== sim.lastLampOn) why = "change";
      else if (t - sim.lastAt >= CFG.heartbeatMs) why = "heartbeat";
      if (!why) return;
      sim.lastLevel = lvl; sim.lastLampOn = lampOn; sim.lastAt = t;
      sim.buffer.push({ seq: sim.seq++, takenAt: t, level: lvl, lamp: lampFor(lvl, lampOn), why });
      if (receive) flush(t);
    };
    const flush = (t) => {
      if (t < sim.offlineUntil) return;
      while (sim.buffer.length) {
        const r = sim.buffer.shift();
        addReading(d, { seq: r.seq, age: t - r.takenAt, level: r.level, lamp: r.lamp, why: r.why }, t);
      }
    };
    const health = (t) => setHealth(d, {
      fw: "1.0.0", ts: Math.floor(t / 1000), up: Math.floor((t - simStart) / 1000), rssi: -58 - Math.round(Math.random() * 6),
      heap: 212000 - Math.round(Math.random() * 4000), cpu: 80, reset: "poweron", boots: sim.boots,
      buffered: sim.buffer.length, dropped: 0, published: d.received, wifi_reconnects: 2, mqtt_reconnects: 2, creds: "nvs",
    }, t, false);

    // Back-fill the last 20 minutes so the charts have something to show,
    // including one 90 s outage whose readings arrive late.
    const outageStart = simStart + 8 * 60000, outageEnd = outageStart + 90000;  // during dusk
    setStatus(d, "online", simStart, false);
    for (let t = simStart; t <= now; t += 2000) {
      if (t >= outageStart && t < outageStart + 2000) { sim.offlineUntil = outageEnd; setStatus(d, "offline", t + 30000, false); }
      if (t >= outageEnd && t < outageEnd + 2000) setStatus(d, "online", t, false);
      evalAt(t, true);
      if ((t - simStart) % 60000 < 2000 && t >= sim.offlineUntil) health(t);
    }
    flushLateBurst(d);

    let tick = 0;
    const timer = setInterval(() => {
      const t = Date.now();
      evalAt(t, true);
      if (++tick % 30 === 0) health(t);
      scheduleRender();
    }, 2000);
    state.demo = { timer };
    state.selected = d.id;
    state.userPicked = true;
    $("demo-banner").hidden = false;
    $("demo-btn").textContent = "Demo running";
    scheduleRender();
  }

  function stopDemo() {
    if (!state.demo) return;
    clearInterval(state.demo.timer);
    state.demo = null;
    state.devices.delete("demo-lamp");
    state.selected = null;
    state.userPicked = false;
    $("demo-banner").hidden = true;
    $("demo-btn").textContent = "Demo data";
    autoSelect();
    scheduleRender();
  }

  // ---- Wiring --------------------------------------------------------------------------
  function init() {
    $("device-select").addEventListener("change", (e) => {
      state.selected = e.target.value || null;
      state.userPicked = true;
      state.hover = null;
      scheduleRender();
    });
    for (const b of document.querySelectorAll(".seg button")) {
      b.addEventListener("click", () => {
        state.rangeMs = Number(b.dataset.range);
        document.querySelectorAll(".seg button").forEach((x) => x.classList.toggle("on", x === b));
        $("range-label").textContent = b.textContent.replace("min", "minutes").replace("1 h", "hour");
        scheduleRender();
      });
    }
    $("table-toggle").addEventListener("click", (e) => {
      const tv = $("table-view");
      tv.hidden = !tv.hidden;
      e.target.setAttribute("aria-expanded", String(!tv.hidden));
      e.target.textContent = tv.hidden ? "Show as table" : "Hide table";
      scheduleRender();
    });
    $("demo-btn").addEventListener("click", () => (state.demo ? stopDemo() : startDemo()));
    $("demo-off").addEventListener("click", stopDemo);
    charts.forEach((c) => wireChart(c.svg()));
    new ResizeObserver(scheduleRender).observe($("chart-level"));
    setInterval(() => {                  // keep time-based text and the charts moving
      for (const d of state.devices.values()) if (d.lateBurst && Date.now() - d.lateBurst.at > 1500) flushLateBurst(d);
      scheduleRender();
    }, 1000);

    connectBroker();
    if (new URLSearchParams(location.search).has("demo")) startDemo();
    scheduleRender();
  }

  init();
})();
