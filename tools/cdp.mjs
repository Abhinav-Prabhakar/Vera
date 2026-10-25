// Minimal Chrome DevTools Protocol helper (Node >= 22: global WebSocket + fetch).
import { spawn } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';

const CHROME = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const sleep = ms => new Promise(r => setTimeout(r, ms));

export async function launch({ width = 1600, height = 1000, scale = 1, port = 9333 } = {}){
  const dir = mkdtempSync(path.join(process.env.SCRATCH || tmpdir(), 'cdp-'));
  const proc = spawn(CHROME, [
    '--headless=new', `--remote-debugging-port=${port}`, `--user-data-dir=${dir}`,
    '--use-angle=metal', '--enable-gpu', '--ignore-gpu-blocklist', '--hide-scrollbars',
    '--disable-background-timer-throttling', '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows',
    `--window-size=${width},${height}`, `--force-device-scale-factor=${scale}`, 'about:blank'
  ], { stdio: 'ignore' });
  let target;
  for (let i = 0; i < 60; i++){
    try {
      const list = await (await fetch(`http://127.0.0.1:${port}/json`)).json();
      target = list.find(t => t.type === 'page');
      if (target) break;
    } catch {}
    await sleep(150);
  }
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise(r => ws.addEventListener('open', r, { once: true }));
  let id = 0;
  const api = {};
  const pending = new Map();
  ws.addEventListener('message', ev => {
    const m = JSON.parse(ev.data);
    if (m.id && pending.has(m.id)){ pending.get(m.id)(m); pending.delete(m.id); }
    else if (m.method && api.onEvent) api.onEvent(m);
  });
  const send = (method, params = {}) => new Promise(res => { const i = ++id; pending.set(i, res); ws.send(JSON.stringify({ id: i, method, params })); });
  await send('Page.enable');
  await send('Runtime.enable');
  await send('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor: scale, mobile: false });
  const evaluate = async (expr) => (await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true })).result?.result?.value;
  const goto = async (url) => { await send('Page.navigate', { url }); };
  const shot = async (file, format = 'png', quality = 90) => {
    const r = await send('Page.captureScreenshot', { format, quality, captureBeyondViewport: false });
    const { writeFileSync } = await import('node:fs');
    writeFileSync(file, Buffer.from(r.result.data, 'base64'));
  };
  const close = () => { try { ws.close(); } catch {} proc.kill('SIGKILL'); };
  Object.assign(api, { send, evaluate, goto, shot, close, sleep });
  return api;
}
export { sleep };
