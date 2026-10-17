// Watch a real-time run and report whether the engine is still advancing.
//
// "It stops on some screen" has two very different causes -- the loop was
// cancelled (a frame exhausted its budget, so spike_tick returned 0), or it is
// merely slow (a few ticks per second through a long splash sequence). Headless
// runs cannot tell them apart because --virtual-time-budget expires after a
// couple of dozen frames; this drives a real browser in real time instead.
//
//   node tools/watch_run.mjs [url] [seconds]
import { spawn } from 'node:child_process';
import { readdirSync, existsSync } from 'node:fs';
import { homedir } from 'node:os';
import { join } from 'node:path';

const URL_ = process.argv[2] || 'http://127.0.0.1:8080/spike.html';
const SECONDS = Number(process.argv[3] || 60);
const PORT = 9334;

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
  let send = null;

  ws.addEventListener('message', (ev) => {
    const m = JSON.parse(ev.data);
    if (m.id && pending.has(m.id)) {
      const { res, rej } = pending.get(m.id); pending.delete(m.id);
      m.error ? rej(new Error(JSON.stringify(m.error))) : res(m.result);
      return;
    }
    if (m.method === 'Runtime.consoleAPICalled') {
      const line = m.params.args.map(a => a.value ?? a.description ?? '').join(' ');
      onLine(line);
    }
  });
  send = (method, params = {}) => new Promise((res, rej) => {
    const n = ++id; pending.set(n, { res, rej });
    ws.send(JSON.stringify({ id: n, method, params }));
  });

  // Frame lines look like:
  //   "  frame  12 :      9272 instrs  ... -> PC left the loaded image"
  const frameRe = /frame\s+(\d+)\s*:\s*(\d+) instrs.*->\s*(.*)$/;
  let lastFrame = -1, lastAt = Date.now(), frames = 0, stopped = null;
  const seen = new Set();

  function onLine(line) {
    if (process.env.DUMP) console.log(line);
    const m = frameRe.exec(line);
    if (m) {
      lastFrame = Number(m[1]); frames++; lastAt = Date.now();
      const why = m[3].trim();
      seen.add(why);
      return;
    }
    if (/final pc=|average over/.test(line)) stopped = line.trim();
    if (/\[trap\]|Uncaught|out of bounds/.test(line)) stopped = 'TRAP: ' + line.trim();
  }

  await new Promise(r => ws.addEventListener('open', r));
  await send('Runtime.enable');
  await send('Page.enable');
  await send('Page.navigate', { url: URL_ });

  // Screenshots at intervals: headless capture via --screenshot cannot reach
  // far into a run (--virtual-time-budget expires after a couple of dozen
  // engine ticks), so this is the only way to see what the canvas holds after
  // the game has actually progressed.
  const shot = async (tag) => {
    const r = await send('Page.captureScreenshot', { format: 'png' });
    const path = `/tmp/watch-${tag}.png`;
    const { writeFileSync } = await import('node:fs');
    writeFileSync(path, Buffer.from(r.data, 'base64'));
    console.log(`  screenshot -> ${path} (frame ${lastFrame})`);
  };

  const t0 = Date.now();
  let lastReport = 0;
  while (Date.now() - t0 < SECONDS * 1000) {
    await sleep(2000);
    const el = ((Date.now() - t0) / 1000).toFixed(0);
    if (stopped) { console.log(`[${el}s] STOPPED: ${stopped}`); break; }
    const idle = ((Date.now() - lastAt) / 1000).toFixed(1);
    if (Number(el) - lastReport >= 10) {
      lastReport = Number(el);
      console.log(`[${el}s] frame=${lastFrame} framesSeen=${frames} lastFrameAge=${idle}s`);
      if (lastFrame > 0) await shot(String(el).padStart(3, '0'));
    }
  }
  console.log(`\n--- summary ---`);
  console.log(`frames seen : ${frames}, highest frame number: ${lastFrame}`);
  console.log(`reasons     : ${[...seen].join(' | ')}`);
  console.log(stopped ? `stopped     : ${stopped}` : `still running when the watch ended`);
  ws.close();
}

main().catch(e => console.error('FAILED:', e.message))
  .finally(() => { proc.kill('SIGKILL'); setTimeout(() => process.exit(0), 200); });
