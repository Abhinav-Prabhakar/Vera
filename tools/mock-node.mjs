#!/usr/bin/env node
/**
 * Vera mock node — speaks the vera-node firmware protocol over a WebSocket so
 * the dashboard's device link can be tried without hardware.
 *
 *   node tools/mock-node.mjs            # ws://localhost:8181, serves bed 0
 *   node tools/mock-node.mjs 8181 1     # port, bed
 *
 * Open Vera over http (e.g. `npx serve .`), click the link icon, LAN, and
 * connect to ws://localhost:8181. Zero dependencies (Node ≥ 18).
 */
import { createServer } from 'node:http';
import { createHash } from 'node:crypto';

const PORT = +(process.argv[2] || 8181);
const BED = +(process.argv[3] || 0);
const clients = new Set();
const ctl = Object.fromEntries(['lights', 'pump', 'air', 'chiller', 'valve', 'nutrients', 'phc']
  .map(id => [id, { mode: 'auto', on: true, level: 100, target: id === 'phc' ? 5.8 : id === 'nutrients' ? 1.85 : id === 'chiller' ? 20 : undefined }]));
const s = { ph: 5.84, ec: 1.86, waterTemp: 20.4, airTemp: 22.6, humidity: 68, do2: 8.4, level: 82, par: 420 };
const t0 = Date.now();

function frame(text){
  const data = Buffer.from(text);
  const len = data.length;
  const head = len < 126 ? Buffer.from([0x81, len]) : len < 65536 ? Buffer.from([0x81, 126, len >> 8, len & 255]) : null;
  return Buffer.concat([head, data]);
}
function parse(buf){
  const len0 = buf[1] & 127, masked = buf[1] & 128;
  let off = 2, len = len0;
  if (len0 === 126){ len = buf.readUInt16BE(2); off = 4; }
  const mask = masked ? buf.subarray(off, off + 4) : null; off += masked ? 4 : 0;
  const out = Buffer.alloc(len);
  for (let i = 0; i < len; i++) out[i] = buf[off + i] ^ (mask ? mask[i % 4] : 0);
  return { op: buf[0] & 15, text: out.toString() };
}
const send = obj => { const f = frame(JSON.stringify(obj)); for (const c of clients) c.write(f); };
function telemetry(){
  const jitter = (v, a) => v + (Math.random() - 0.5) * a;
  s.ph += 0.0009; if (s.ph > 6.0 && ctl.phc.on && ctl.phc.mode === 'auto'){ s.ph -= 0.22; send({ type: 'event', node: 'mock-node', bed: BED, kind: 'dose', control: 'phc', ml: 5, auto: true }); }
  s.do2 += ((ctl.air.on ? 8.5 : 5.2) - s.do2) * 0.05;
  const lights = ctl.lights.on ? (ctl.lights.mode === 'manual' ? ctl.lights.level : 100) : 0;
  send({
    type: 'telemetry', node: 'mock-node', bed: BED, fw: '1.0.0-mock', uptime: Math.round((Date.now() - t0) / 1000), rssi: -58, heap: 21344,
    airTemp: +jitter(s.airTemp, 0.1).toFixed(2), humidity: +jitter(s.humidity, 0.6).toFixed(1), waterTemp: +jitter(s.waterTemp, 0.04).toFixed(2),
    ph: +jitter(s.ph, 0.01).toFixed(3), ec: +jitter(s.ec, 0.006).toFixed(3), do2: +jitter(s.do2, 0.05).toFixed(2), level: +s.level.toFixed(1),
    par: Math.round(s.par * lights / 100), controls: Object.fromEntries(Object.entries(ctl).map(([k, c]) => [k, { ...c, level: c.on ? (c.mode === 'manual' ? c.level : 100) : 0 }]))
  });
}
function command(m){
  const c = ctl[m.control];
  if (m.type === 'auto_all') for (const k in ctl){ ctl[k].mode = 'auto'; ctl[k].on = true; }
  if (m.type === 'set' && c){ if (m.mode) c.mode = m.mode; if (typeof m.level === 'number') c.level = m.level; if (typeof m.on === 'boolean') c.on = m.on; if (m.target != null) c.target = m.target; }
  if (m.type === 'dose' && c){ if (m.control === 'phc') s.ph -= m.ml * 0.03; else s.ec += m.ml * 0.004; send({ type: 'event', node: 'mock-node', bed: BED, kind: 'dose', control: m.control, ml: m.ml, auto: false }); }
  if (m.ref) send({ type: 'ack', node: 'mock-node', ref: m.ref, ok: true });
  console.log('←', JSON.stringify(m));
  telemetry();
}
const server = createServer((_, res) => { res.writeHead(200); res.end('vera mock node — connect over WebSocket'); });
server.on('upgrade', (req, socket) => {
  const key = createHash('sha1').update(req.headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
  socket.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${key}\r\n\r\n`);
  clients.add(socket);
  console.log('dashboard connected');
  socket.on('data', buf => { const f = parse(buf); if (f.op === 8){ socket.end(); return; } if (f.op === 1){ try { command(JSON.parse(f.text)); } catch {} } });
  socket.on('close', () => clients.delete(socket));
  socket.on('error', () => clients.delete(socket));
  telemetry();
});
setInterval(telemetry, 2000);
server.listen(PORT, () => console.log(`vera mock node on ws://localhost:${PORT} · bed ${BED}`));
