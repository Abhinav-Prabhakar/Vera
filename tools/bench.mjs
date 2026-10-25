import { launch, sleep } from './cdp.mjs';
const files = process.argv.slice(2);
for (const f of files){
  const b = await launch({ width: 1600, height: 1000, scale: 2 });
  await b.goto(`http://localhost:8791/${f}`);
  let t = '';
  for (let i = 0; i < 80 && !t.startsWith('BENCH'); i++){ await sleep(500); t = (await b.evaluate('document.title')) || ''; }
  console.log(f.padEnd(40), t);
  b.close();
  await sleep(800);
}
process.exit(0);
