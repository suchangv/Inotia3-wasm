// Click at a point in the game's own 960x544 space, then screenshot.
//
//   node tools/click_at.mjs <url> <x> <y> [waitSeconds] [out.png]
//
// PRE=<seconds> waits before clicking, for screens that take a while to appear.
//
// Separate from click_test.mjs, which exercises the input protocol at the
// canvas centre. This one is for "there is a button on screen, press it" --
// the coordinates are engine pixels, so they can be read straight off a
// screenshot of the canvas.
import { spawn } from 'node:child_process';
import { readdirSync, existsSync, writeFileSync } from 'node:fs';
import { homedir } from 'node:os';
import { join } from 'node:path';

const URL_ = process.argv[2];
const TX = Number(process.argv[3] || 480);
const TY = Number(process.argv[4] || 500);
const WAIT = Number(process.argv[5] || 20);
const OUT = process.argv[6] || '/tmp/click_at.png';
const PORT = 9335;

function findChrome() {
  const base = join(homedir(), '.cache', 'puppeteer', 'chrome');
  if (!existsSync(base)) return null;
  for (const d of readdirSync(base)) {
    const p = join(base, d, 'chrome-linux64', 'chrome');
    if (existsSync(p)) return p;
  }
  return null;
}
const chrome = findChrome();
if (!chrome) { console.error('no Chrome for Testing'); process.exit(1); }

const proc = spawn(chrome, [
  '--headless=new', '--no-sandbox', '--disable-gpu', '--enable-unsafe-swiftshader',
  `--remote-debugging-port=${PORT}`, '--window-size=1100,700', 'about:blank',
], { stdio: ['ignore', 'ignore', 'pipe'] });

const sleep = (ms) => new Promise(r => setTimeout(r, ms));

async function main() {
  let list = null;
  for (let i = 0; i < 60; i++) {
    try {
      const r = await fetch(`http://127.0.0.1:${PORT}/json/list`);
      list = await r.json();
      if (list.length) break;
    } catch { /* not up yet */ }
    await sleep(250);
  }
  if (!list?.length) throw new Error('devtools never came up');
  const page = list.find(t => t.type === 'page') || list[0];
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  let id = 0;
  const pending = new Map();
  let lastFrame = -1;

  const send = (method, params = {}) => new Promise((res, rej) => {
    const n = ++id; pending.set(n, { res, rej });
    ws.send(JSON.stringify({ id: n, method, params }));
  });
  ws.addEventListener('message', (ev) => {
    const m = JSON.parse(ev.data);
    if (m.id && pending.has(m.id)) {
      const { res, rej } = pending.get(m.id); pending.delete(m.id);
      m.error ? rej(new Error(JSON.stringify(m.error))) : res(m.result);
      return;
    }
    if (m.method === 'Runtime.consoleAPICalled') {
      const line = m.params.args.map(a => a.value ?? a.description ?? '').join(' ');
      const f = /frame\s+(\d+)\s*:/.exec(line);
      if (f) lastFrame = Number(f[1]);
      if (process.env.DUMP) console.log(line);
    }
  });

  await new Promise(r => ws.addEventListener('open', r));
  await send('Runtime.enable');
  await send('Page.enable');
  await send('Page.navigate', { url: URL_ });

  // The game boots synchronously inside main(); nothing is interactive until
  // the rAF loop starts producing frames. Wait for that rather than for a
  // fixed delay.
  const deadline = Date.now() + 120000;
  while (Date.now() < deadline && lastFrame < 0) await sleep(500);
  console.log(`frames started (last frame ${lastFrame}), waiting for the screen`);
  // A fixed delay is the wrong knob: the interpreter's frame rate varies with
  // what the engine is doing, so "wait 125 s" can land well before the screen
  // is up. Wait for a frame number instead when one is given.
  const wantFrame = Number(process.env.FRAME || 0);
  if (wantFrame > 0) {
    const fd = Date.now() + 300000;
    while (Date.now() < fd && lastFrame < wantFrame) await sleep(1000);
    console.log(`reached frame ${lastFrame} (wanted ${wantFrame})`);
  } else {
    await sleep(Number(process.env.PRE || 8) * 1000);
  }

  const box = await send('Runtime.evaluate', {
    expression: `(() => { const c = document.getElementById('canvas');
      const r = c.getBoundingClientRect();
      return JSON.stringify({x: r.left, y: r.top, w: r.width, h: r.height}); })()`,
    returnByValue: true,
  });
  const b = JSON.parse(box.result.value);
  const cx = b.x + TX * (b.w / 960);
  const cy = b.y + TY * (b.h / 544);
  console.log(`clicking engine (${TX},${TY}) -> viewport (${cx.toFixed(1)},${cy.toFixed(1)})`);

  const mouse = (type, x, y) => send('Input.dispatchMouseEvent', {
    type, x, y, button: 'left', clickCount: 1,
    buttons: type === 'mouseReleased' ? 0 : 1,
  });
  await mouse('mousePressed', cx, cy);
  // HOLD lets the press land in one engine tick and the release in a later
  // one. A tap entirely inside a single tick is fine for the input protocol
  // (click_test.mjs proves it), but screen handlers that latch on the press
  // and act on the release need the two separated.
  await sleep(Number(process.env.HOLD || 80));
  await mouse('mouseReleased', cx, cy);
  console.log(`clicked, waiting ${WAIT}s`);
  await sleep(WAIT * 1000);

  const r = await send('Page.captureScreenshot', { format: 'png' });
  writeFileSync(OUT, Buffer.from(r.data, 'base64'));
  console.log(`screenshot -> ${OUT} (frame ${lastFrame})`);
  ws.close();
}

main().catch(e => console.error('FAILED:', e.message))
  .finally(() => { proc.kill('SIGKILL'); setTimeout(() => process.exit(0), 200); });
