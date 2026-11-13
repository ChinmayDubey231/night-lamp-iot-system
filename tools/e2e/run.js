// End-to-end check of the whole chain, with no hardware and no internet:
//
//   firmware logic (sketch.ino, compiled for the PC, real time)
//     -> e2e_bridge prints each MQTT action it takes
//     -> this script performs them on a local MQTT broker (aedes)
//     -> the dashboard subscribes over WebSockets, like it does in production
//
// Usage:  ./run.sh [seconds]      then open the dashboard with
//         dashboard/index.html?broker=ws://localhost:8888
const path = require("path");
const fs = require("fs");
const http = require("http");
const { spawn } = require("child_process");
const aedes = require("aedes")();
const ws = require("websocket-stream");
const mqtt = require("mqtt");

const WS_PORT = 8888;
const BRIDGE = process.argv[2];
const SECONDS = process.argv[3] || "120";
const LOG = process.argv[4] || path.join(__dirname, "e2e-log.txt");

const server = http.createServer();
ws.createServer({ server }, aedes.handle);
server.listen(WS_PORT, () => console.log(`[broker] MQTT over WebSockets on ws://localhost:${WS_PORT}`));
aedes.on("clientDisconnect", (c) => console.log(`[broker] ${c.id} disconnected`));

// An independent subscriber writes down everything that reaches the broker.
const log = fs.createWriteStream(LOG);
const t0 = Date.now();
const stamp = () => `+${((Date.now() - t0) / 1000).toFixed(1).padStart(6)}s`;
const seen = { readings: [], statuses: [], health: 0 };
const watcher = mqtt.connect(`ws://localhost:${WS_PORT}`, { clientId: "e2e-watcher" });
watcher.on("connect", () => watcher.subscribe("#"));
watcher.on("message", (topic, payload, pkt) => {
  const text = payload.toString();
  log.write(`${stamp()}  ${topic}${pkt.retain ? " (retained)" : ""}  ${text}\n`);
  if (topic.endsWith("/reading")) seen.readings.push(JSON.parse(text));
  else if (topic.endsWith("/status")) seen.statuses.push(text);
  else if (topic.endsWith("/health")) seen.health++;
});

// What a subscriber must have seen for the chain to count as working.
function verdict() {
  const seqs = seen.readings.map((r) => r.seq);
  const inOrder = seqs.every((s, i) => i === 0 || s === seqs[i - 1] + 1);
  const late = seen.readings.filter((r) => r.age > 3000).length;
  const off = seen.statuses.indexOf("offline");
  const checks = [
    [seen.readings.length >= 4, `readings arrived (${seen.readings.length})`],
    [inOrder, `every reading once, in order (seq ${seqs[0]}..${seqs[seqs.length - 1]})`],
    [late >= 1, `buffered readings delivered after the outage (${late})`],
    [off >= 0, "broker published the last will \"offline\" during the outage"],
    [off >= 0 && seen.statuses.indexOf("online", off) > off, "device came back \"online\" by itself"],
    [seen.health >= 2, `health snapshots published (${seen.health})`],
  ];
  let ok = true;
  for (const [pass, text] of checks) { console.log(`  ${pass ? "ok  " : "FAIL"} ${text}`); ok = ok && pass; }
  return ok;
}

// The device's MQTT session, driven by the firmware's own decisions.
let device = null;
function connectDevice(clientId, willTopic, willMsg) {
  device = mqtt.connect(`ws://localhost:${WS_PORT}`, {
    clientId, reconnectPeriod: 0, clean: true, keepalive: 30,
    will: { topic: willTopic, payload: willMsg, qos: 1, retain: true },
  });
}
function dropDevice() {
  // Vanish without an MQTT DISCONNECT, like a dead access point, so the
  // broker publishes the last will.
  if (device) { device.stream.destroy(); device.end(true); device = null; }
}

setTimeout(() => {
  const bridge = spawn(BRIDGE, [SECONDS], { stdio: ["ignore", "pipe", "inherit"] });
  let buf = "";
  bridge.stdout.on("data", (chunk) => {
    buf += chunk;
    let i;
    while ((i = buf.indexOf("\n")) >= 0) {
      const line = buf.slice(0, i); buf = buf.slice(i + 1);
      const [cmd, a, b, c, ...rest] = line.split("\t");
      if (cmd === "CONNECT") { console.log(`${stamp()} [device] connect as ${a}`); connectDevice(a, b, c); }
      else if (cmd === "DROP") { console.log(`${stamp()} [device] connection dropped`); dropDevice(); }
      else if (cmd === "PUB" && device) device.publish(b, [c, ...rest].join("\t"), { qos: 0, retain: a === "1" });
    }
  });
  bridge.on("exit", () => {
    console.log(`${stamp()} [bridge] finished; broker log in ${LOG}`);
    setTimeout(() => {
      console.log("End-to-end result:");
      process.exit(verdict() ? 0 : 1);
    }, 1500);
  });
}, 1000);
