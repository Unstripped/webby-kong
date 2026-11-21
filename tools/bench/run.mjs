#!/usr/bin/env node
// Browser 60 fps bench: serves build/web + tools/bench/bench.html + the ROM, runs each scene in a fresh page of
// headless Chrome (GPU via ANGLE/Metal) over CDP, and prints `METRIC name=value` lines.
// Scene = one map booted through the resume point; see bench.html for what is timed.
// Primary: game_fps (images presented per second of game time). Fails when the frame-time p95 exceeds the budget,
// the rendered image or the real-time movement speed drifts from the reference written by the first run, or a
// presented image repeats its predecessor while the camera turns (a duplicated frame counted as new).
// Usage: node tools/bench/run.mjs [--web build/web] [--rom decomp/donkey-kong.z64] [--ref build/bench/ref]
//                                 [--warm 600] [--measure 480] [--scenes japes,isles,...] [--update-ref]
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';

const SCENES = {
  japes: 0x07, isles: 0x22, aztec: 0x26, factory: 0x1a, galleon: 0x1e, fungi: 0x30, caves: 0x48, castle: 0x57,
};
const VI_MS = 16.6834; // PORT_VI_PERIOD_MS
const BUDGET_MS = 10; // per browser frame (one VI step + gl.finish), p95 over all scenes
const RENDER_DIFF_LIMIT = 6; // mean |delta| per channel (0..255) of a 64x36 thumbnail vs the reference
const MOTION_TOLERANCE = 0.1; // speed-probe distance vs the reference, relative
const MOTION_FLOOR = 50; // distances below this compare in absolute terms (DK blocked by a wall)
const SMOOTH_MIN_CHANGE = 1.0; // median change between presented 96x54 thumbnails below which a window is static
const SMOOTH_DUP_SHARE = 0.1; // a change below this share of the window's median is a repeated image

const argv = process.argv.slice(2);
const opt = (name, def) => {
  const i = argv.indexOf(`--${name}`);
  return i >= 0 ? argv[i + 1] : def;
};
const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), '../..');
const webDir = path.resolve(opt('web', path.join(root, 'build/web')));
const romPath = path.resolve(opt('rom', path.join(root, 'decomp/donkey-kong.z64')));
const refDir = path.resolve(opt('ref', path.join(root, 'build/bench/ref')));
const warm = Number(opt('warm', 600));
const measure = Number(opt('measure', 480));
const sceneNames = opt('scenes', Object.keys(SCENES).join(',')).split(',');
const updateRef = argv.includes('--update-ref');
const chromeCandidates = [
  process.env.CHROME,
  path.join(os.homedir(), 'Library/Caches/ms-playwright/chromium-1254/chrome-mac-arm64/Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing'),
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
].filter(Boolean);
const chromePath = chromeCandidates.find((p) => fs.existsSync(p));

function fail(msg) {
  console.error(`bench: ${msg}`);
  process.exit(1);
}

if (!chromePath) fail('no Chrome found (set CHROME)');
if (!fs.existsSync(path.join(webDir, 'dk64.wasm'))) fail(`no dk64.wasm in ${webDir}`);
if (!fs.existsSync(romPath)) fail(`no ROM at ${romPath}`);
for (const s of sceneNames) if (!(s in SCENES)) fail(`unknown scene ${s}`);

// ---------------------------------------------------------------- static server
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.z64': 'application/octet-stream' };
const server = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://x');
  let file;
  if (url.pathname === '/bench.html') file = path.join(root, 'tools/bench/bench.html');
  else if (url.pathname === '/rom.z64') file = romPath;
  else file = path.join(webDir, path.normalize(url.pathname).replace(/^(\.\.[/\\])+/, ''));
  fs.readFile(file, (err, data) => {
    if (err) {
      res.writeHead(404);
      res.end();
      return;
    }
    // cross-origin isolation: performance.now() at 5 us instead of 100 us resolution
    res.writeHead(200, {
      'Content-Type': TYPES[path.extname(file)] || 'application/octet-stream', 'Cache-Control': 'no-store',
      'Cross-Origin-Opener-Policy': 'same-origin', 'Cross-Origin-Embedder-Policy': 'require-corp',
    });
    res.end(data);
  });
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const origin = `http://127.0.0.1:${server.address().port}`;

// ---------------------------------------------------------------- chrome + CDP
const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'dk64-bench-'));
const chrome = spawn(chromePath, [
  '--headless=new', '--remote-debugging-port=0', `--user-data-dir=${profile}`, '--no-first-run',
  '--no-default-browser-check', '--window-size=1280,720', '--force-device-scale-factor=2', '--use-angle=metal',
  '--enable-gpu', '--ignore-gpu-blocklist', '--autoplay-policy=no-user-gesture-required',
  '--disable-background-timer-throttling', '--disable-renderer-backgrounding',
  '--disable-backgrounding-occluded-windows', '--mute-audio', 'about:blank',
], { stdio: ['ignore', 'ignore', 'pipe'] });
let chromeErr = '';
chrome.stderr.on('data', (d) => { chromeErr = (chromeErr + d).slice(-4000); });
const chromeExited = new Promise((r) => chrome.once('exit', r));
// on any exit: kill Chrome; the profile is removed best-effort (its helpers may still be writing after a kill)
const cleanup = () => {
  try { chrome.kill('SIGKILL'); } catch { /* gone */ }
  server.close();
  try { fs.rmSync(profile, { recursive: true, force: true }); } catch { /* temp dir, left behind */ }
};
process.on('exit', cleanup);

const portFile = path.join(profile, 'DevToolsActivePort');
for (let i = 0; i < 200 && !fs.existsSync(portFile); i++) await new Promise((r) => setTimeout(r, 50));
if (!fs.existsSync(portFile)) fail(`Chrome did not start: ${chromeErr}`);
const [port, wsPath] = fs.readFileSync(portFile, 'utf8').trim().split('\n');
const ws = new WebSocket(`ws://127.0.0.1:${port}${wsPath}`);
await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
let nextId = 1;
const pending = new Map();
ws.onmessage = (ev) => {
  const msg = JSON.parse(ev.data);
  if (msg.id && pending.has(msg.id)) {
    const { resolve, reject } = pending.get(msg.id);
    pending.delete(msg.id);
    if (msg.error) reject(new Error(JSON.stringify(msg.error)));
    else resolve(msg.result);
  }
};
const send = (method, params = {}, sessionId) => new Promise((resolve, reject) => {
  const id = nextId++;
  pending.set(id, { resolve, reject });
  ws.send(JSON.stringify({ id, method, params, sessionId }));
});

async function runScene(name) {
  const url = `${origin}/bench.html?map=${SCENES[name]}&warm=${warm}&measure=${measure}`;
  const { targetId } = await send('Target.createTarget', { url: 'about:blank' });
  const { sessionId } = await send('Target.attachToTarget', { targetId, flatten: true });
  await send('Page.enable', {}, sessionId);
  // 1280x720 CSS at DPR 2, like a laptop browser window: drawable 2560x1440, render scale 6
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 720, deviceScaleFactor: 2, mobile: false }, sessionId);
  await send('Page.navigate', { url }, sessionId);
  const t0 = Date.now();
  const deadline = t0 + 180000;
  let result = null;
  while (Date.now() < deadline) {
    await new Promise((r) => setTimeout(r, 250));
    const r = await send('Runtime.evaluate', {
      expression: 'window.benchResult ? JSON.stringify(window.benchResult) : null', returnByValue: true,
    }, sessionId);
    if (r.result && r.result.value) {
      result = JSON.parse(r.result.value);
      break;
    }
  }
  await send('Target.closeTarget', { targetId });
  if (!result) fail(`scene ${name}: timed out`);
  if (!result.ok) fail(`scene ${name}: ${result.error}\n${result.log.join('\n')}`);
  if (!result.isolated) fail(`scene ${name}: page is not cross-origin isolated (coarse timer)`);
  result.wallMs = Date.now() - t0;
  return result;
}

const pct = (sorted, p) => sorted[Math.min(sorted.length - 1, Math.floor(p / 100 * sorted.length))];
const fmt = (v) => v.toFixed(3);
const allTotal = [], allCpu = [];
const lines = [];
let renderDiffMax = 0, motionErrMax = 0, renderer = '', framesAll = 0, fpsMin = Infinity;
fs.mkdirSync(refDir, { recursive: true });
const meanAbs = (a, b) => {
  let d = 0;
  for (let i = 0; i < a.length; i++) d += Math.abs(a[i] - b[i]);
  return d / a.length;
};
let smoothFail = 0, smoothChecked = 0;
for (const name of sceneNames) {
  const r = await runScene(name);
  renderer = r.renderer;
  if (r.after.map !== SCENES[name]) fail(`scene ${name}: ended on map ${r.after.map}, want ${SCENES[name]}`);
  if (r.after.lastError) fail(`scene ${name}: lastError ${r.after.lastError}`);
  if (r.thumbs.some((t) => !t) || r.smooth.some((t) => !t)) fail(`scene ${name}: no capture`);
  if (r.motion == null) fail(`scene ${name}: no player for the speed probe`);
  // images presented per second of game time (VIs at 59.94 Hz)
  const frames = r.after.frames - r.before.frames;
  const fps = frames / measure * (1000 / VI_MS);
  framesAll += frames;
  fpsMin = Math.min(fpsMin, fps);
  // references come from the first (baseline) run: render thumbnail and speed-probe distance. The game frame
  // the reference shows is one of the next two presented images.
  const refFile = path.join(refDir, `${name}.json`);
  let diff = 0, motionErr = 0;
  if (fs.existsSync(refFile) && !updateRef) {
    const ref = JSON.parse(fs.readFileSync(refFile, 'utf8'));
    diff = Math.min(...r.thumbs.map((t) => meanAbs(ref.thumb, t)));
    motionErr = Math.abs(r.motion - ref.motion) / Math.max(ref.motion, MOTION_FLOOR);
  } else {
    fs.writeFileSync(refFile, JSON.stringify({ thumb: r.thumbs[0], motion: r.motion }));
  }
  renderDiffMax = Math.max(renderDiffMax, diff);
  motionErrMax = Math.max(motionErrMax, motionErr);
  // smoothness: while the camera turns, no presented image may repeat its predecessor (a duplicate counted as a
  // new frame). Windows whose median change is below SMOOTH_MIN_CHANGE (static views) are not checked.
  const steps = r.smooth.slice(1).map((t, i) => meanAbs(r.smooth[i], t));
  const median = steps.slice().sort((a, b) => a - b)[steps.length >> 1];
  let sf = 0, sc = 0;
  if (median >= SMOOTH_MIN_CHANGE) {
    sc = steps.length;
    sf = steps.filter((d) => d < SMOOTH_DUP_SHARE * median).length;
  }
  smoothFail += sf;
  smoothChecked += sc;
  const total = r.total.slice().sort((a, b) => a - b);
  const cpu = r.cpu.slice().sort((a, b) => a - b);
  allTotal.push(...r.total);
  allCpu.push(...r.cpu);
  console.log(`scene ${name.padEnd(8)} map=0x${SCENES[name].toString(16)} fps=${fps.toFixed(2)} drawable=${r.drawable.join('x')} `
    + `total p50=${fmt(pct(total, 50))} p95=${fmt(pct(total, 95))} max=${fmt(total[total.length - 1])} `
    + `cpu p95=${fmt(pct(cpu, 95))} render_diff=${diff.toFixed(2)} motion=${r.motion.toFixed(1)} `
    + `motion_err=${(motionErr * 100).toFixed(1)}% smooth=${sc - sf}/${sc} (${r.smooth.length} images in ${r.smoothVis} VIs) `
    + `wall=${r.wallMs}ms warm=${Math.round(r.warmMs)}ms`);
  lines.push(`METRIC p95_${name}_ms=${fmt(pct(total, 95))}`);
}
console.log(`renderer: ${renderer}`);
allTotal.sort((a, b) => a - b);
allCpu.sort((a, b) => a - b);
const mean = allTotal.reduce((a, b) => a + b, 0) / allTotal.length;
const over = allTotal.filter((v) => v > BUDGET_MS).length / allTotal.length * 100;
const gameFps = framesAll / (measure * sceneNames.length) * (1000 / VI_MS);
const p95 = pct(allTotal, 95);
console.log(`METRIC game_fps=${gameFps.toFixed(2)}`);
console.log(`METRIC game_fps_min=${fpsMin.toFixed(2)}`);
console.log(`METRIC frame_p95_ms=${fmt(p95)}`);
console.log(`METRIC frame_p50_ms=${fmt(pct(allTotal, 50))}`);
console.log(`METRIC frame_p99_ms=${fmt(pct(allTotal, 99))}`);
console.log(`METRIC frame_mean_ms=${fmt(mean)}`);
console.log(`METRIC over_budget_pct=${over.toFixed(2)}`);
console.log(`METRIC cpu_p95_ms=${fmt(pct(allCpu, 95))}`);
console.log(`METRIC render_diff_max=${renderDiffMax.toFixed(2)}`);
console.log(`METRIC motion_err_pct=${(motionErrMax * 100).toFixed(1)}`);
console.log(`METRIC smooth_fail=${smoothFail}`);
console.log(`METRIC smooth_checked=${smoothChecked}`);
for (const l of lines) console.log(l);
if (renderDiffMax > RENDER_DIFF_LIMIT) fail(`render differs from reference (${renderDiffMax.toFixed(2)} > ${RENDER_DIFF_LIMIT})`);
if (motionErrMax > MOTION_TOLERANCE) fail(`speed probe off by ${(motionErrMax * 100).toFixed(1)}% (> ${MOTION_TOLERANCE * 100}%): movement depends on frame rate`);
if (smoothFail > 0) fail(`${smoothFail} presented images are not between their neighbours`);
if (p95 > BUDGET_MS) fail(`frame p95 ${fmt(p95)} ms over the ${BUDGET_MS} ms budget`);
await send('Browser.close').catch(() => {});
await Promise.race([chromeExited, new Promise((r) => setTimeout(r, 5000))]);
ws.close();
process.exit(0);
