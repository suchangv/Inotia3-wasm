// Drive a real click into the running page and report what the engine saw.
//
// The one thing that cannot be checked without a browser: whether a pointer
// event actually reaches the guest, on the right slot, with the right
// coordinates. Everything on the way there -- the listener registration, the
// CSS-to-canvas scaling, the slot state machine, the six-argument AAPCS call --
// is only proven by doing it once against a live page.
//
// Chrome for Testing is already on this machine (run_browser.sh finds it), and
// node 24 has a global WebSocket, so the DevTools Protocol needs no packages.
//
//   node tools/click_test.mjs [url] [seconds]
import { spawn } from 'node:child_process';
import { readdirSync, existsSync } from 'node:fs';
import { homedir } from 'node:os';
import { join } from 'node:path';

const URL_ = process.argv[2] || 'http://127.0.0.1:8080/spike.html';
const SECONDS = Number(process.argv[3] || 90);
const PORT = 9333;

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
if (!chrome) { console.error('no Chrome for Testing found'); process.exit(1); }

const proc = spawn(chrome, [
  '--headless=new', '--no-sandbox', '--disable-gpu', '--enable-unsafe-swiftshader',
  `--remote-debugging-port=${PORT}`, '--window-size=1100,700', 'about:blank',
], { stdio: ['ignore', 'ignore', 'pipe'] });

const sleep = (ms) => new Promise(r => setTimeout(r, ms));

async function main() {
  // wait for the debugging endpoint
  let list = null;
  for (let i = 0; i < 60; i++) {
    try {
      const r = await fetch(`http://127.0.0.1:${PORT}/json/list`);
      list = await r.json();
      if (list.length) break;
    } catch { /* not up yet */ }
    await sleep(250);
  }
  if (!list || !list.length) throw new Error('devtools never came up');
  const page = list.find(t => t.type === 'page') || list[0];

  const ws = new WebSocket(page.webSocketDebuggerUrl);
  let id = 0;
  const pending = new Map();
  const events = [];

  const send = (method, params = {}) => new Promise((res, rej) => {
    const n = ++id;
    pending.set(n, { res, rej });
    ws.send(JSON.stringify({ id: n, method, params }));
  });

  ws.addEventListener('message', (ev) => {
    const m = JSON.parse(ev.data);
    if (m.id && pending.has(m.id)) {
      const { res, rej } = pending.get(m.id);
      pending.delete(m.id);
      m.error ? rej(new Error(JSON.stringify(m.error))) : res(m.result);
      return;
    }
    if (m.method === 'Runtime.consoleAPICalled') {
      const line = m.params.args.map(a => a.value ?? a.description ?? '').join(' ');
      events.push(line);
      process.stdout.write(line + '\n');
    }
  });

  await new Promise(r => ws.addEventListener('open', r));
  await send('Runtime.enable');
  await send('Page.enable');
  await send('Page.navigate', { url: URL_ });

  // Wait for the listeners to be installed, then for the game to be running.
  const deadline = Date.now() + SECONDS * 1000;
  let installed = false;
  while (Date.now() < deadline) {
    if (events.some(l => l.includes('pointer listeners installed'))) { installed = true; break; }
    await sleep(500);
  }
  console.log(installed ? '\n--- listeners installed, clicking ---'
                        : '\n--- listeners never installed ---');

  // Where the canvas actually is, so the click lands on it.
  const box = await send('Runtime.evaluate', {
    expression: `(() => { const c = document.getElementById('canvas');
      const r = c.getBoundingClientRect();
      return JSON.stringify({x: r.left, y: r.top, w: r.width, h: r.height,
                             cw: c.width, ch: c.height}); })()`,
    returnByValue: true,
  });
  const b = JSON.parse(box.result.value);
  console.log('canvas:', JSON.stringify(b));

  // Centre of the canvas, in viewport coordinates.
  const cx = b.x + b.w / 2, cy = b.y + b.h / 2;
  const mouse = (type, x, y, button = 'left', clicks = 1) => send('Input.dispatchMouseEvent', {
    type, x, y, button, clickCount: clicks, buttons: type === 'mouseReleased' ? 0 : 1,
  });

  // 1. A quick tap: press and release with no move. This is the case the edge
  //    FIFO exists for -- both events can land inside one tick, which runs for
  //    hundreds of milliseconds here.
  console.log('\n--- quick tap at', cx, cy, '---');
  await mouse('mousePressed', cx, cy);
  await sleep(30);
  await mouse('mouseReleased', cx, cy, 'left', 1);
  await sleep(2500);

  // 2. A press, then a real drag well past the 40 ms / 2 px gate, then release.
  console.log('\n--- drag from', cx, cy, '---');
  await mouse('mousePressed', cx, cy);
  await sleep(60);
  for (let i = 1; i <= 4; i++) {
    await mouse('mouseMoved', cx + i * 30, cy + i * 12);
    await sleep(80);
  }
  await sleep(200);
  await mouse('mouseReleased', cx + 120, cy + 48, 'left', 1);
  await sleep(2500);

  console.log('\n--- done ---');
  ws.close();
}

main().catch(e => { console.error('FAILED:', e.message); })
  .finally(() => { proc.kill('SIGKILL'); setTimeout(() => process.exit(0), 200); });
