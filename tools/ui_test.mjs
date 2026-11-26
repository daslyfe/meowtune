// Drives ui/ in headless Chrome against tools/mock/juce (a fake singer) and saves screenshots.
// Catches layout and wiring bugs; it is not WebKit, so it can't catch WKWebView click behaviour
// (tools/proc_test does that with real held clicks).
// usage: node tools/ui_test.mjs <screenshot dir>
import { spawn } from 'node:child_process';
import { cpSync, mkdirSync, writeFileSync, rmSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { setTimeout as sleep } from 'node:timers/promises';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const shots = path.resolve(process.argv[2] ?? path.join(root, 'build', 'ui_test'));
const site = path.join(shots, 'site');
rmSync(site, { recursive: true, force: true });
mkdirSync(site, { recursive: true });
cpSync(path.join(root, 'ui'), site, { recursive: true });
cpSync(path.join(root, 'tools', 'mock', 'juce'), path.join(site, 'juce'), { recursive: true });

const HTTP = 8751, CDP = 9353;
let Wd = 1040, Ht = 720;
const server = spawn('python3', ['-m', 'http.server', String(HTTP), '--bind', '127.0.0.1', '--directory', site], { stdio: 'ignore' });
const chrome = spawn('/Applications/Google Chrome.app/Contents/MacOS/Google Chrome', [
  '--headless=new', `--remote-debugging-port=${CDP}`, `--user-data-dir=${shots}/profile`, '--no-first-run',
  `--window-size=${Wd},${Ht}`, 'about:blank'], { stdio: 'ignore' });

let failures = 0;
const check = (ok, what, extra = '') => { console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${what}${extra !== '' ? ` (${extra})` : ''}`); if (!ok) ++failures; };

try {
  let target;
  for (let i = 0; i < 150 && !target; ++i) {
    await sleep(200);
    try { target = (await (await fetch(`http://127.0.0.1:${CDP}/json`)).json()).find((t) => t.type === 'page'); } catch {}
  }
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((r) => ws.addEventListener('open', r));
  let id = 0;
  const pending = new Map();
  const errors = [];
  ws.addEventListener('message', (m) => {
    const msg = JSON.parse(m.data);
    if (msg.id && pending.has(msg.id)) { pending.get(msg.id)(msg); pending.delete(msg.id); }
    if (msg.method === 'Runtime.exceptionThrown') errors.push(msg.params.exceptionDetails.exception?.description || msg.params.exceptionDetails.text);
    if (msg.method === 'Runtime.consoleAPICalled' && msg.params.type === 'error') errors.push(msg.params.args.map((a) => a.value ?? a.description).join(' '));
    if (msg.method === 'Log.entryAdded' && msg.params.entry.level === 'error') errors.push(msg.params.entry.text + ' ' + msg.params.entry.url);
  });
  const send = (method, params = {}) => new Promise((r) => { const i = ++id; pending.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
  const evaluate = async (expr) => (await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true })).result?.result?.value;
  const shot = async (name) => {
    const r = await send('Page.captureScreenshot', { format: 'png' });
    writeFileSync(`${shots}/${name}.png`, Buffer.from(r.result.data, 'base64'));
  };
  const mouse = (type, x, y, extra = {}) => send('Input.dispatchMouseEvent', { type, x, y, button: 'left', clickCount: 1, ...extra });
  const rectOf = (sel) => evaluate(`(() => { const b = document.querySelector(${JSON.stringify(sel)}).getBoundingClientRect(); return [b.x, b.y, b.width, b.height]; })()`);
  const slowClickAt = async (x, y) => { await mouse('mousePressed', x, y); await sleep(120); await mouse('mouseReleased', x, y); await sleep(80); };
  const slowClick = async (sel) => { const [x, y, w, h] = await rectOf(sel); await slowClickAt(x + w / 2, y + h / 2); };
  const st = (expr) => evaluate(`window.__mock.st.${expr}`);
  const text = (sel) => evaluate(`document.querySelector(${JSON.stringify(sel)}).textContent`);
  const size = async (w, h) => { Wd = w; Ht = h; await send('Emulation.setDeviceMetricsOverride', { width: w, height: h, deviceScaleFactor: 2, mobile: false }); await sleep(300); };
  // Centre of the key for MIDI note n, from the page's own layout.
  const keyCentre = async (n) => {
    const [x, y] = await rectOf('#keys');
    const r = await evaluate(`(() => { const k = window.__keys().find((r) => r.n === ${n}); return [k.x + k.w / 2, k.y + (k.black ? k.h * 0.5 : k.h * 0.85)]; })()`);
    return [x + r[0], y + r[1]];
  };

  await send('Runtime.enable');
  await send('Log.enable');
  await send('Page.enable');
  await size(Wd, Ht);
  await send('Page.navigate', { url: `http://127.0.0.1:${HTTP}/index.html` });
  await sleep(1500);
  await evaluate(`window.__keys = () => window.__meowtuneKeys()`);

  console.log('load');
  check(await evaluate(`document.fonts.check("15px '3270'")`), '3270 font loaded');
  check(await evaluate(`document.documentElement.scrollWidth <= innerWidth && document.documentElement.scrollHeight <= innerHeight`), 'nothing overflows the window');
  const head = await evaluate(`(() => { const h = document.querySelector('.panel-head'); return h.scrollWidth <= h.clientWidth + 1; })()`);
  check(head, 'header fits');
  const g = await rectOf('#graph');
  check(g[3] > 140, 'the pitch graph has room', `${Math.round(g[2])}x${Math.round(g[3])}`);
  check((await text('#r-in')).startsWith('IN:'), 'input pitch readout', await text('#r-in'));
  await sleep(600);
  check(/^IN:[A-G]#?\d[+-]\d+$/.test(await text('#r-in')) || (await text('#r-in')) === 'IN:---', 'readout is NOTE+CENTS', await text('#r-in'));
  check(/^OUT:/.test(await text('#r-out')), 'output note readout', await text('#r-out'));
  check(await evaluate(`[...document.querySelectorAll('[data-param] input[type=number]')].every((i) => i.value !== '')`), 'every slider shows its value');
  const fields = await evaluate(`[...document.querySelectorAll('.controls .field')].every((f) => f.scrollWidth <= f.clientWidth + 1)`);
  check(fields, 'controls fit their columns');
  await shot('01-tune');

  console.log('keyboard');
  const [cx, cy] = await keyCentre(61); // C#4
  await slowClickAt(cx, cy);
  check((await st('params.note1')) === 0, 'held click on C# turns C# off');
  check((await st('gestures.note1')) === 0, 'the click is one complete gesture');
  check((await text('#r-notes')) === 'NOTES:11', 'note count follows', await text('#r-notes'));
  check((await st('params.scale')) === 19, 'editing the keyboard switches SCALE to CUSTOM');
  check(await evaluate(`document.querySelector('#scale').selectedOptions[0].text`) === 'CUSTOM', 'the scale picker shows CUSTOM');
  check(await evaluate(`document.querySelector('#key').disabled && document.querySelector('#key-next').disabled`), 'KEY is disabled in CUSTOM');
  await slowClickAt(cx, cy);
  check((await st('params.note1')) === 1, 'clicking again turns it back on');
  // Paint: drag from D4 across E4 and F4 turns all three off.
  const [dx, dy] = await keyCentre(62);
  const [fx] = await keyCentre(65);
  await mouse('mousePressed', dx, dy);
  for (let k = 1; k <= 8; ++k) { await mouse('mouseMoved', dx + ((fx - dx) * k) / 8, dy); await sleep(20); }
  await mouse('mouseReleased', fx, dy);
  await sleep(100);
  check((await st('params.note2')) === 0 && (await st('params.note4')) === 0 && (await st('params.note5')) === 0, 'dragging paints D, E and F off');
  check((await st('params.note7')) === 1, 'G is untouched');
  check(await evaluate(`['note2', 'note4', 'note5'].every((n) => window.__mock.st.gestures[n] === 0)`), 'painting closes its gestures');

  console.log('scales');
  const notes = () => evaluate(`Array.from({ length: 12 }, (_, i) => window.__mock.st.params['note' + i]).join('')`);
  const scaleName = () => evaluate(`document.querySelector('#scale').selectedOptions[0].text`);
  const customNotes = await notes();
  await slowClick('#all-notes');
  check((await st('params.scale')) === 0 && (await scaleName()) === 'CHROMATIC', '>>ALL picks CHROMATIC');
  check((await text('#r-notes')) === 'NOTES:12', 'with 12 notes', await text('#r-notes'));
  await slowClick('#scale-next');
  check((await st('params.scale')) === 1 && (await scaleName()) === 'MAJOR', '[>] steps to MAJOR');
  check((await st('gestures.scale')) === 0, 'as one complete gesture');
  await slowClick('#key-next');
  await slowClick('#key-next');
  check((await st('params.key')) === 2, 'two [>] on KEY: D');
  check((await text('#r-notes')) === 'NOTES:7', 'D major has 7 notes', await text('#r-notes'));
  check((await notes()) === customNotes, 'choosing a scale leaves the CUSTOM notes alone');
  await evaluate(`(() => { const s = document.querySelector('#scale'); s.value = '11'; s.dispatchEvent(new Event('change')); })()`);
  check((await st('params.scale')) === 11, 'picking MINOR PENTATONIC from the list');
  check((await text('#r-notes')) === 'NOTES:5', 'D minor pentatonic has 5 notes', await text('#r-notes'));
  await slowClick('#scale-prev');
  check((await scaleName()) === 'MAJOR PENTATONIC', '[<] steps back');
  // Host automation or a mapped MIDI CC moves KEY and SCALE: the pickers and keyboard follow.
  await evaluate(`(() => { window.__mock.st.params.key = 9; window.__mock.st.params.scale = 2; window.__mock.emit('tick', { inHz: 0, inMidi: -1, outMidi: -1, target: -1, inPeak: 0, outPeak: 0, params: { key: 9, scale: 2 } }); })()`);
  await sleep(50);
  check((await evaluate(`document.querySelector('#key').value`)) === '9' && (await scaleName()) === 'MINOR', 'KEY and SCALE from the host show in the pickers');
  // Clicking a key on A minor: CUSTOM starts from A minor with that key toggled.
  const [ex, ey] = await keyCentre(61); // C#4, not in A minor
  await slowClickAt(ex, ey);
  check((await st('params.scale')) === 19 && (await notes()) === '111011010101', 'a click on A minor makes CUSTOM = A minor + C#', await notes());
  await slowClick('#scale-next');
  check((await scaleName()) === 'CHROMATIC', 'from CUSTOM, [>] goes to the first scale');
  await sleep(500);
  await shot('02-scale');

  console.log('range');
  check((await text('#r-latency')) === '35.2 MS', 'TENOR: 35.2 ms latency', await text('#r-latency'));
  await slowClick('#range-next');
  check((await st('params.range')) === 2 && (await text('#r-latency')) === '23.5 MS', 'RANGE [>]: ALTO, 23.5 ms', await text('#r-latency'));
  await slowClick('#range-next');
  await slowClick('#range-next');
  check((await st('params.range')) === 0 && (await text('#r-latency')) === '50.2 MS', 'wraps to BASS, 50.2 ms', await text('#r-latency'));
  await slowClick('#range-next');
  check((await st('gestures.range')) === 0, 'range changes are complete gestures');

  console.log('sliders');
  const r = await rectOf('[data-param=amount] input[type=range]');
  await mouse('mousePressed', r[0] + r[2] * 0.9, r[1] + r[3] / 2);
  for (let k = 1; k <= 6; ++k) { await mouse('mouseMoved', r[0] + r[2] * (0.9 - 0.08 * k), r[1] + r[3] / 2); await sleep(20); }
  await mouse('mouseReleased', r[0] + r[2] * 0.42, r[1] + r[3] / 2);
  await sleep(100);
  const amt = await st('params.amount');
  check(amt > 30 && amt < 55, 'dragging AMOUNT sends values', amt);
  check((await st('gestures.amount')) === 0, 'the drag is one complete gesture');
  check(await evaluate(`document.querySelector('[data-param=amount] input[type=number]').value`) === String(Math.round(amt)), 'the number box follows');
  await mouse('mousePressed', r[0] + r[2] * 0.5, r[1] + r[3] / 2, { clickCount: 2 });
  await mouse('mouseReleased', r[0] + r[2] * 0.5, r[1] + r[3] / 2, { clickCount: 2 });
  await sleep(100);
  check((await st('params.amount')) === 100, 'double-click resets AMOUNT to 100');
  await evaluate(`(() => { const n = document.querySelector('[data-param=pitch] input[type=number]'); n.value = '99'; n.dispatchEvent(new Event('change')); })()`);
  check((await st('params.pitch')) === 24, 'a typed PITCH is clamped to 24');
  await evaluate(`window.__mock.emit('tick', { inHz: 0, inMidi: -1, outMidi: -1, target: -1, inPeak: 0, outPeak: 0, params: { detune: 42, unison: 5 } })`);
  await sleep(50);
  check(await evaluate(`document.querySelector('[data-param=detune] input[type=number]').value`) === '42.0', 'host automation shows on DETUNE');
  check(await evaluate(`document.querySelector('[data-param=unison] input[type=range]').value`) === '5', 'and on VOICES');

  console.log('midi');
  await slowClick('#midi-mode button[data-value="1"]');
  check((await st('params.midiMode')) === 1, 'CHORD selected');
  await evaluate(`window.__mockHeld = [[60, 64, 67]]`);
  await sleep(150);
  check((await text('#r-midi')) === 'MIDI:3 NOTES', 'held notes show', await text('#r-midi'));
  await shot('03-chord');
  await evaluate(`window.__mockHeld = [[]]`);
  await sleep(150);
  check((await text('#r-midi')) === 'MIDI:CHORD', 'released', await text('#r-midi'));
  await slowClick('#midi-mode button[data-value="0"]');
  check((await text('#r-midi')) === 'MIDI:SNAP', 'SNAP again');

  console.log('settings');
  await slowClick('.panel-tabs button[data-tab=settings]');
  check(await evaluate(`!document.querySelector('#tab-settings').hidden && document.querySelector('#tab-tune').hidden`), 'SETTINGS tab');
  await evaluate(`(() => { const i = document.querySelector('[data-colour=accent]'); i.value = '#ff8800'; i.dispatchEvent(new Event('input')); })()`);
  await sleep(400);
  check((await st('colours.accent')) === '#ff8800', 'a colour change is saved');
  check(await evaluate(`getComputedStyle(document.querySelector('.btn')).getPropertyValue('--accent').trim()`) === '#ff8800', 'and applied live');
  await shot('04-settings');
  await slowClick('.panel-tabs button[data-tab=tune]');
  await sleep(300);
  await shot('05-orange');
  await slowClick('.panel-tabs button[data-tab=settings]');
  await slowClick('#reset-colours');
  check(await evaluate(`getComputedStyle(document.documentElement).getPropertyValue('--accent').trim()`) === '#118ac1', '>>RESET COLOURS restores the accent');
  check(await evaluate(`Object.keys(window.__mock.st.colours).length`) === 0, 'and clears the saved colours');
  await slowClick('.panel-tabs button[data-tab=tune]');

  console.log('sizes');
  await size(1400, 820);
  check(await evaluate(`getComputedStyle(document.querySelector('.controls')).gridTemplateColumns.split(' ').length`) === 4, 'wide: four columns');
  check(await evaluate(`[...document.querySelectorAll('.controls .field')].every((f) => f.scrollWidth <= f.clientWidth + 1)`), 'wide: controls fit');
  await shot('06-wide');
  await size(760, 560);
  check(await evaluate(`document.documentElement.scrollWidth <= innerWidth`), 'smallest size: no sideways overflow');
  const small = await rectOf('#graph');
  check(small[3] >= 60, 'smallest size: the graph still shows', Math.round(small[3]));
  check(await evaluate(`(() => { const c = document.querySelector('.controls'); return c.getBoundingClientRect().bottom <= document.querySelector('.panel-body').getBoundingClientRect().bottom + 1 && c.scrollHeight > c.clientHeight; })()`), 'smallest size: the controls scroll inside the pane');
  await shot('07-small');

  check(errors.length === 0, 'no page errors', errors.join(' | '));
} finally {
  chrome.kill();
  server.kill();
}
console.log(failures === 0 ? '\nALL PASSED' : `\n${failures} FAILED`);
process.exit(failures ? 1 : 0);
