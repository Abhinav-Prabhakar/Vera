// Records a scripted tour of Vera with Chrome's screencast stream.
// usage: node record.mjs <outDir> <url> <width> <height> <seconds>
import { launch, sleep } from './cdp.mjs';  // run from tools/: SCRATCH=/tmp node record.mjs ../rec http://localhost:8791/index.html?quality=high 1280 800 16
import { mkdirSync, writeFileSync } from 'node:fs';
const [outDir, url, W = 1280, H = 800, SECS = 14] = process.argv.slice(2);
mkdirSync(outDir, { recursive: true });
const b = await launch({ width: +W, height: +H, scale: 1, port: 9444 });
await b.goto(url);
// wait for the scene to be ready
for (let i = 0; i < 80; i++){ await sleep(500); if (await b.evaluate('!!(window.__VH && document.body.classList.contains("ready"))')) break; }
await b.evaluate(`(() => {
  const V = window.__VH;
  V.controls.autoRotateSpeed = 1.6;
  document.getElementById('hint')?.remove();
})()`);
await sleep(2500);

const frames = [];
let n = 0;
b.onEvent = async (m) => {
  if (m.method !== 'Page.screencastFrame') return;
  const { data, metadata, sessionId } = m.params;
  const f = `${outDir}/f${String(n++).padStart(4, '0')}.jpg`;
  writeFileSync(f, Buffer.from(data, 'base64'));
  frames.push({ f, ts: metadata.timestamp });
  b.send('Page.screencastFrameAck', { sessionId });
};
await b.send('Page.startScreencast', { format: 'jpeg', quality: 92, maxWidth: +W, maxHeight: +H, everyNthFrame: 1 });
const t0 = Date.now();
const script = [
  [3800, `__VH.setScope({ level: 'bed', bed: 1 })`],
  [6200, `document.querySelector('#dock .ctl[data-id="lights"]').click()`],
  [6900, `(() => { const s = document.getElementById('pSlider'); const r = s.getBoundingClientRect(); const ev = (t, x) => s.dispatchEvent(new PointerEvent(t, { clientX: x, clientY: r.top + 10, pointerId: 1, bubbles: true })); ev('pointerdown', r.right - 4); let k = 0; const id = setInterval(() => { k++; ev('pointermove', r.right - 4 - k * 6); if (k > 26){ clearInterval(id); ev('pointerup', r.right - 4 - k * 6); } }, 30); })()`],
  [9000, `document.getElementById('pClose').click()`],
  [9400, `document.getElementById('dwBtn').click()`],
  [12400, `(() => { const p = __VH.PLANT.filter(x => x.det && x.det.conf >= .75).sort((a,b)=>b.det.conf-a.det.conf)[0]; const e = __VH.PE[p.i]; __VH.setScope({ level: 'plant', bed: e.bed, row: e.row, plant: p.i }); })()`]
];
let si = 0;
while (Date.now() - t0 < SECS * 1000){
  const el = Date.now() - t0;
  if (si < script.length && el >= script[si][0]){ b.evaluate(script[si][1]); si++; }
  await sleep(20);
}
await b.send('Page.stopScreencast');
writeFileSync(`${outDir}/frames.json`, JSON.stringify(frames));
console.log('frames', frames.length, 'span', (frames.at(-1).ts - frames[0].ts).toFixed(2), 's');
b.close();
process.exit(0);
