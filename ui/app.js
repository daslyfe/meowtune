import * as Juce from './juce/index.js';
import { setText, setClass, trackDrag, readTokens } from './dom.js';

const $ = (sel) => document.querySelector(sel);
const native = (name) => Juce.getNativeFunction(name);
const call = {
  getInit: native('getInit'),
  setParam: native('setParam'),
  gesture: native('gesture'),
  setColours: native('setColours'),
};

const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
const noteName = (n) => `${NOTE_NAMES[((n % 12) + 12) % 12]}${Math.floor(n / 12) - 1}`;

// ---- parameters ------------------------------------------------------------------------------
// P[id] = { min, max, step, def, value, views: [fn(value)] }
const P = {};
const views = (id) => (P[id] ||= { min: 0, max: 1, step: 0, def: 0, value: 0, views: [] }).views;

function setLocal(id, v) {
  const p = P[id];
  if (!p) return;
  p.value = v;
  for (const f of p.views) f(v);
}

function sendParam(id, v) {
  setLocal(id, v);
  call.setParam(id, v);
}

function gesture(id, fn) {
  call.gesture(id, true);
  fn();
  call.gesture(id, false);
}

// KEY and SCALE are parameters, so a host can automate or MIDI-map them. Every scale but CUSTOM
// (the last) is its intervals from KEY; CUSTOM is the noteN parameters, which the keyboard edits.
let SCALES = []; // [{ id, name, steps }] from getInit, in parameter order
const customIndex = () => SCALES.length - 1;
const keyIndex = () => Math.round(P.key?.value ?? 0);
const scaleIndex = () => Math.round(P.scale?.value ?? 0);
const isCustom = () => scaleIndex() >= customIndex();
const notesMask = () => {
  let m = 0;
  for (let i = 0; i < 12; i++) if (P[`note${i}`]?.value >= 0.5) m |= 1 << i;
  return m;
};
const scaleMask = (key, index) => {
  const s = SCALES[index];
  if (!s || !s.steps) return null;
  return s.steps.reduce((m, i) => m | (1 << ((key + i) % 12)), 0);
};
// The notes it tunes to now.
const mask = () => (isCustom() ? notesMask() : scaleMask(keyIndex(), scaleIndex()) ?? 0);
const setChoice = (id, v) => gesture(id, () => sendParam(id, v));

// ---- tabs ------------------------------------------------------------------------------------
for (const tab of document.querySelectorAll('[data-tab]')) {
  tab.addEventListener('click', () => {
    for (const t of document.querySelectorAll('[data-tab]')) {
      const on = t === tab;
      t.classList.toggle('selected', on);
      t.setAttribute('aria-selected', String(on));
      $(`#tab-${t.dataset.tab}`).hidden = !on;
    }
    resizeCanvases();
  });
}

// ---- sliders: range + number in step, bound to a parameter -----------------------------------
function bindSlider(field) {
  const id = field.dataset.param;
  const [range, num] = field.querySelectorAll('input');
  const decimals = Number(field.dataset.decimals);
  const fmt = (v) => Number(v).toFixed(decimals);
  const clamp = (v) => Math.min(P[id].max, Math.max(P[id].min, v));
  let dragging = false;

  views(id).push((v) => {
    if (!dragging) range.value = v;
    if (document.activeElement !== num) num.value = fmt(v);
  });

  range.addEventListener('pointerdown', () => { dragging = true; call.gesture(id, true); });
  const endDrag = () => { if (dragging) { dragging = false; call.gesture(id, false); } };
  window.addEventListener('pointerup', endDrag);
  window.addEventListener('pointercancel', endDrag);
  range.addEventListener('input', () => {
    const v = Number(range.value);
    if (!dragging) { gesture(id, () => sendParam(id, v)); return; } // keyboard arrows
    sendParam(id, v);
  });
  range.addEventListener('dblclick', () => gesture(id, () => sendParam(id, P[id].def)));

  num.addEventListener('change', () => {
    const v = Number(num.value);
    if (Number.isFinite(v)) gesture(id, () => sendParam(id, clamp(v)));
    num.value = fmt(P[id].value);
  });
  num.addEventListener('blur', () => { num.value = fmt(P[id].value); });
  num.addEventListener('keydown', (e) => { if (e.key === 'Enter') num.blur(); });

  return () => {
    for (const el of [range, num]) {
      el.min = P[id].min;
      el.max = P[id].max;
      el.step = P[id].step > 0 ? P[id].step : 'any';
    }
    range.value = P[id].value;
    num.value = fmt(P[id].value);
  };
}
const sliderSetups = [...document.querySelectorAll('[data-param]')].map(bindSlider);

// ---- MIDI mode choice ------------------------------------------------------------------------
const modeButtons = [...document.querySelectorAll('#midi-mode button')];
views('midiMode').push((v) => {
  for (const b of modeButtons) {
    const on = Number(b.dataset.value) === Math.round(v);
    b.classList.toggle('selected', on);
    b.setAttribute('aria-checked', String(on));
  }
  renderMidiIndicator();
});
for (const b of modeButtons) b.addEventListener('click', () => gesture('midiMode', () => sendParam('midiMode', Number(b.dataset.value))));

// ---- key, scale and range pickers -----------------------------------------------------------
const keySel = $('#key');
const scaleSel = $('#scale');
const rangeSel = $('#range');
NOTE_NAMES.forEach((n, i) => keySel.add(new Option(n, String(i))));
let RANGES = []; // [{ name, hz, latency }] from getInit
let rate = 48000;

function setNotes(m) {
  for (let i = 0; i < 12; i++) {
    const id = `note${i}`;
    const v = (m >> i) & 1;
    if ((P[id].value >= 0.5 ? 1 : 0) !== v) gesture(id, () => sendParam(id, v));
  }
}

// Editing the keyboard while a named scale is on: its notes become CUSTOM's, then CUSTOM is chosen.
function toCustom() {
  if (isCustom()) return;
  setNotes(mask());
  setChoice('scale', customIndex());
}

const countBits = (m) => { let c = 0; for (let i = 0; i < 12; i++) c += (m >> i) & 1; return c; };

function renderPickers() {
  keySel.value = String(keyIndex());
  scaleSel.value = String(scaleIndex());
  // KEY does nothing in CUSTOM.
  const custom = isCustom();
  for (const el of [keySel, $('#key-prev'), $('#key-next')]) el.disabled = custom;
  keySel.title = custom ? 'KEY does nothing in CUSTOM: the keyboard sets the notes' : '';
  setText($('#r-notes'), `NOTES:${countBits(mask())}`);
}

function renderRange() {
  const i = Math.round(P.range?.value ?? 0);
  rangeSel.value = String(i);
  const r = RANGES[i];
  if (!r) return;
  const ms = ((1000 * r.latency) / rate).toFixed(1);
  setText($('#r-latency'), `${ms} MS`);
  setText($('#i-latency'), `${r.latency} SAMPLES (${ms} MS)`);
}

// [<] [>] wrap around; on SCALE they skip CUSTOM (from CUSTOM, [>] goes to the first scale).
const wrap = (i, n) => ((i % n) + n) % n;
function stepScale(dir) {
  const n = customIndex();
  const i = isCustom() ? (dir > 0 ? -1 : n) : scaleIndex();
  setChoice('scale', wrap(i + dir, n));
}

keySel.addEventListener('change', () => setChoice('key', Number(keySel.value)));
$('#key-prev').addEventListener('click', () => setChoice('key', wrap(keyIndex() - 1, 12)));
$('#key-next').addEventListener('click', () => setChoice('key', wrap(keyIndex() + 1, 12)));
scaleSel.addEventListener('change', () => setChoice('scale', Number(scaleSel.value)));
$('#scale-prev').addEventListener('click', () => stepScale(-1));
$('#scale-next').addEventListener('click', () => stepScale(1));
rangeSel.addEventListener('change', () => setChoice('range', Number(rangeSel.value)));
$('#range-prev').addEventListener('click', () => setChoice('range', wrap(Math.round(P.range.value) - 1, RANGES.length)));
$('#range-next').addEventListener('click', () => setChoice('range', wrap(Math.round(P.range.value) + 1, RANGES.length)));
$('#all-notes').addEventListener('click', () => setChoice('scale', 0)); // CHROMATIC
$('#no-notes').addEventListener('click', () => { setNotes(0); setChoice('scale', customIndex()); });

for (const id of ['key', 'scale', ...Array.from({ length: 12 }, (_, i) => `note${i}`)])
  views(id).push(() => { renderPickers(); requestDraw(); });
views('range').push(renderRange);

// ---- live state from the processor -----------------------------------------------------------
const live = { inMidi: -1, outMidi: -1, target: -1, inHz: 0, held: [] };
const HIST = 900; // about 4.8 s at the detector's rate
const hist = new Float32Array(HIST * 2).fill(-1);
let histPos = 0;

function renderMidiIndicator() {
  const el = $('#r-midi');
  const mode = Math.round(P.midiMode?.value ?? 0) === 1 ? 'CHORD' : 'SNAP';
  if (live.held.length === 0) {
    setText(el, `MIDI:${mode}`);
    setClass(el, 'indicator idle');
    el.title = 'No MIDI notes held';
  } else {
    setText(el, `MIDI:${live.held.length} ${live.held.length === 1 ? 'NOTE' : 'NOTES'}`);
    setClass(el, 'indicator ok');
    el.title = live.held.map(noteName).join(' ');
  }
}

// ---- canvases --------------------------------------------------------------------------------
let tok = readTokens();
const graph = $('#graph');
const keys = $('#keys');
let drawQueued = false;

function sizeCanvas(c) {
  const r = c.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  const w = Math.max(1, Math.round(r.width * dpr)), h = Math.max(1, Math.round(r.height * dpr));
  if (c.width !== w || c.height !== h) { c.width = w; c.height = h; }
  return { w: r.width, h: r.height, dpr };
}

function resizeCanvases() { requestDraw(); }
new ResizeObserver(resizeCanvases).observe($('#graph-wrap'));
new ResizeObserver(resizeCanvases).observe($('#keys-wrap'));

function requestDraw() {
  if (drawQueued) return;
  drawQueued = true;
  requestAnimationFrame(() => { drawQueued = false; drawGraph(); drawKeys(); });
}

const font = (px) => `${px}px ${tok.font}`;

// Pitch over time: the enabled notes as rows, the input in muted, the tuned pitch in text.
let centre = 60;
function drawGraph() {
  if ($('#tab-tune').hidden) return;
  const { w, h, dpr } = sizeCanvas(graph);
  const g = graph.getContext('2d');
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.fillStyle = tok.bg;
  g.fillRect(0, 0, w, h);

  // Follow the voice: keep the latest pitch inside the middle of a two-octave window.
  const span = 24;
  const latest = live.outMidi >= 0 ? live.outMidi : live.inMidi;
  if (latest >= 0) {
    if (latest > centre + span * 0.3) centre = latest - span * 0.3;
    if (latest < centre - span * 0.3) centre = latest + span * 0.3;
  }
  const top = 8, bottom = h - 22, left = 44;
  const y = (m) => bottom - ((m - (centre - span / 2)) / span) * (bottom - top);
  const m = mask();

  g.font = font(13);
  g.textBaseline = 'middle';
  let lastLabel = Infinity; // labels go bottom up; skip one that would touch the one below
  for (let n = Math.ceil(centre - span / 2); n <= Math.floor(centre + span / 2); n++) {
    const on = (m >> (((n % 12) + 12) % 12)) & 1;
    const yy = Math.round(y(n)) + 0.5;
    if (on) {
      g.strokeStyle = tok.border;
      g.beginPath(); g.moveTo(left, yy); g.lineTo(w, yy); g.stroke();
    }
    const isTarget = n === live.target;
    const nextIsTarget = n + 1 === live.target;
    if ((on || n % 12 === 0 || isTarget) && (isTarget || lastLabel - yy >= 14) && !(nextIsTarget && y(n + 1) > yy - 14)) {
      g.fillStyle = isTarget ? tok.accent : on ? tok.muted : tok.border;
      g.fillText(noteName(n), 6, yy);
      lastLabel = yy;
    }
  }
  g.strokeStyle = tok.border;
  g.beginPath(); g.moveTo(left - 0.5, top); g.lineTo(left - 0.5, bottom); g.stroke();

  // Traces, newest at the right.
  const dx = (w - left) / (HIST - 1);
  const trace = (offset, colour) => {
    g.strokeStyle = colour;
    g.lineWidth = 1;
    g.beginPath();
    let pen = false;
    for (let i = 0; i < HIST; i++) {
      const v = hist[((histPos + i) % HIST) * 2 + offset];
      if (v < 0) { pen = false; continue; }
      const px = left + i * dx, py = y(v);
      if (pen) g.lineTo(px, py); else g.moveTo(px, py);
      pen = true;
    }
    g.stroke();
  };
  trace(0, tok.muted);
  trace(1, tok.text);
}

// The keyboard: C2..C7. Enabled notes are inverted (text fill); the detected pitch, the target
// and held MIDI notes are marked in accent.
const LOW = 36, HIGH = 96;
const isBlack = (n) => [1, 3, 6, 8, 10].includes(((n % 12) + 12) % 12);
let keyRects = [];

function layoutKeys(w, h) {
  const whites = [];
  for (let n = LOW; n <= HIGH; n++) if (!isBlack(n)) whites.push(n);
  const lane = 12;
  const kw = w / whites.length;
  const rects = [];
  whites.forEach((n, i) => rects.push({ n, x: i * kw, y: lane, w: kw, h: h - lane, black: false }));
  for (let n = LOW; n <= HIGH; n++) {
    if (!isBlack(n)) continue;
    const left = rects.find((r) => r.n === n - 1);
    rects.push({ n, x: left.x + kw * 0.68, y: lane, w: kw * 0.64, h: (h - lane) * 0.6, black: true });
  }
  return { rects, kw, lane };
}

function xOfPitch(p, rects) {
  // Interpolate between key centres so cents show as position.
  const lo = Math.floor(p), f = p - lo;
  const a = rects.find((r) => r.n === lo), b = rects.find((r) => r.n === lo + 1);
  if (!a || !b) return null;
  return a.x + a.w / 2 + (b.x + b.w / 2 - (a.x + a.w / 2)) * f;
}

function drawKeys() {
  if ($('#tab-tune').hidden) return;
  const { w, h, dpr } = sizeCanvas(keys);
  const g = keys.getContext('2d');
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.fillStyle = tok.bg;
  g.fillRect(0, 0, w, h);
  const { rects, lane } = layoutKeys(w, h);
  keyRects = rects;
  const m = mask();
  const held = new Set(live.held);
  g.font = font(12);
  g.textAlign = 'center';
  g.textBaseline = 'bottom';

  for (const r of rects) {
    const on = (m >> (r.n % 12)) & 1;
    const x = Math.round(r.x) + 0.5, yy = Math.round(r.y) + 0.5;
    const ww = Math.round(r.x + r.w) - Math.round(r.x), hh = Math.round(r.h) - 1;
    if (r.black || on) {
      g.fillStyle = on ? tok.text : tok.bg;
      g.fillRect(x, yy, ww, hh);
    }
    g.strokeStyle = tok.border;
    g.strokeRect(x, yy, ww, hh);
    if (!r.black) {
      g.fillStyle = on ? tok.bg : tok.muted;
      g.fillText(r.n % 12 === 0 ? noteName(r.n) : NOTE_NAMES[r.n % 12], r.x + r.w / 2, r.y + r.h - 4);
    }
    if (r.n === live.target) {
      g.strokeStyle = tok.accent;
      g.strokeRect(x + 2, yy + 2, ww - 4, hh - 4);
    }
    if (held.has(r.n)) {
      g.fillStyle = tok.accent;
      const s = Math.max(4, Math.min(8, r.w * 0.3));
      g.fillRect(r.x + r.w / 2 - s / 2, r.y + (r.black ? r.h - s - 4 : r.h - s - 20), s, s);
    }
  }

  // The lane above the keys: the detected pitch as an accent hairline at its exact position.
  g.strokeStyle = tok.border;
  g.beginPath(); g.moveTo(0, lane - 0.5); g.lineTo(w, lane - 0.5); g.stroke();
  if (live.inMidi >= 0) {
    const x = xOfPitch(live.inMidi, rects);
    if (x !== null) {
      g.strokeStyle = tok.accent;
      g.beginPath(); g.moveTo(Math.round(x) + 0.5, 0); g.lineTo(Math.round(x) + 0.5, lane - 1); g.stroke();
    }
  }
}

window.__meowtuneKeys = () => keyRects; // for the tests

// Click a key to toggle its note in every octave; drag across keys to paint the same state.
function keyAt(e) {
  const b = keys.getBoundingClientRect();
  const x = e.clientX - b.left, y = e.clientY - b.top;
  const hit = (r) => x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
  return keyRects.find((r) => r.black && hit(r)) || keyRects.find((r) => !r.black && hit(r));
}

let paint = null; // { value, touched:Set }
trackDrag(keys, {
  down(e) {
    const r = keyAt(e);
    if (!r) return false;
    toCustom();
    const pc = r.n % 12;
    const value = (mask() >> pc) & 1 ? 0 : 1;
    paint = { value, touched: new Set([pc]) };
    call.gesture(`note${pc}`, true);
    sendParam(`note${pc}`, value);
  },
  move(e) {
    if (!paint) return;
    const r = keyAt(e);
    if (!r) return;
    const pc = r.n % 12;
    if (paint.touched.has(pc)) return;
    paint.touched.add(pc);
    call.gesture(`note${pc}`, true);
    sendParam(`note${pc}`, paint.value);
  },
  up() {
    if (!paint) return;
    for (const pc of paint.touched) call.gesture(`note${pc}`, false);
    paint = null;
  },
});

// ---- meters ----------------------------------------------------------------------------------
const CELLS = 16;
const held = { in: -90, out: -90 };
const toDb = (v) => (v > 0 ? 20 * Math.log10(v) : -90);
function bar(db) {
  const filled = Math.round(Math.min(1, Math.max(0, (db + 60) / 60)) * CELLS);
  return `[${'#'.repeat(filled)}<span class="empty-cells">${'.'.repeat(CELLS - filled)}</span>] ${(db <= -89 ? '-INF' : db.toFixed(1)).padStart(5)}`;
}
const meterHtml = { in: '', out: '' };

// ---- colours ---------------------------------------------------------------------------------
const COLOURS = ['bg', 'text', 'accent', 'muted', 'border'];
const colourInputs = Object.fromEntries([...document.querySelectorAll('[data-colour]')].map((el) => [el.dataset.colour, el]));
const DEFAULT_COLOURS = { bg: '#000000', text: '#ffffff', accent: '#118ac1', muted: '#76787e', border: '#525252' };
let saveTimer = 0;

function currentColours() {
  return Object.fromEntries(COLOURS.map((c) => [c, colourInputs[c].value]));
}

function applyColours() {
  tok = readTokens();
  requestDraw();
}

for (const c of COLOURS) {
  colourInputs[c].addEventListener('input', () => {
    document.documentElement.style.setProperty(`--${c}`, colourInputs[c].value);
    applyColours();
    clearTimeout(saveTimer);
    saveTimer = setTimeout(() => call.setColours(currentColours()), 250);
  });
}

$('#reset-colours').addEventListener('click', () => {
  clearTimeout(saveTimer);
  for (const c of COLOURS) {
    document.documentElement.style.removeProperty(`--${c}`);
    colourInputs[c].value = DEFAULT_COLOURS[c];
  }
  $('#user-colours').textContent = '';
  call.setColours(null);
  applyColours();
});

function showColours(saved) {
  for (const c of COLOURS) colourInputs[c].value = (saved && saved[c]) || DEFAULT_COLOURS[c];
}

// ---- init and ticks --------------------------------------------------------------------------
let lastStatus = '';
let lastSignal = 0;

async function init() {
  const s = await call.getInit();
  SCALES = s.scales;
  RANGES = s.ranges;
  rate = s.rate;
  scaleSel.length = 0;
  SCALES.forEach((x, i) => scaleSel.add(new Option(x.name, String(i))));
  rangeSel.length = 0;
  RANGES.forEach((x, i) => rangeSel.add(new Option(`${x.name} ${Math.round(x.hz)} HZ+`, String(i))));
  for (const p of s.params) {
    const v = views(p.id);
    P[p.id] = { min: p.min, max: p.max, step: p.step, def: p.def, value: p.value, views: v };
  }
  for (const setup of sliderSetups) setup();
  for (const id of Object.keys(P)) setLocal(id, P[id].value);
  renderPickers();
  renderRange();
  showColours(s.colours);
  setText($('#i-rate'), `${Math.round(s.rate)} HZ`);
  setText($('#i-version'), s.version);
  await document.fonts.ready;
  tok = readTokens();
  requestDraw();
}

window.__JUCE__.backend.addEventListener('reload', () => init());

window.__JUCE__.backend.addEventListener('tick', (t) => {
  if (t.params) for (const [id, v] of Object.entries(t.params)) setLocal(id, v);
  if (t.held) { live.held = t.held; renderMidiIndicator(); }
  if (t.hist) {
    for (let i = 0; i < t.hist.length; i += 2) {
      hist[histPos * 2] = t.hist[i] < 0 ? -1 : t.hist[i] / 100;
      hist[histPos * 2 + 1] = t.hist[i + 1] < 0 ? -1 : t.hist[i + 1] / 100;
      histPos = (histPos + 1) % HIST;
    }
  }
  live.inMidi = t.inMidi;
  live.outMidi = t.outMidi;
  live.target = t.target;
  live.inHz = t.inHz;

  if (t.inMidi >= 0) {
    const n = Math.round(t.inMidi);
    const cents = Math.round((t.inMidi - n) * 100);
    setText($('#r-in'), `IN:${noteName(n)}${cents >= 0 ? '+' : ''}${cents}`);
    setText($('#r-hz'), `HZ:${t.inHz.toFixed(1)}`);
  } else {
    setText($('#r-in'), 'IN:---');
    setText($('#r-hz'), 'HZ:---');
  }
  setText($('#r-out'), t.target >= 0 ? `OUT:${noteName(t.target)}` : t.outMidi >= 0 ? `OUT:${noteName(Math.round(t.outMidi))}` : 'OUT:---');

  for (const k of ['in', 'out']) {
    held[k] = Math.max(toDb(t[`${k}Peak`]), held[k] - 1.5);
    const html = bar(held[k]);
    if (html !== meterHtml[k]) { $(`#m-${k}`).innerHTML = html; meterHtml[k] = html; }
  }

  const now = performance.now();
  if (t.inPeak > 0.001) lastSignal = now;
  const text = now - lastSignal > 1000 ? '&gt; AWAITING SIGNAL' : t.inMidi < 0 ? '&gt; NO PITCH' : live.held.length && Math.round(P.midiMode.value) === 1 ? '&gt; CHORD' : '&gt; TUNING';
  if (text !== lastStatus) {
    $('#status').innerHTML = `${text}<span class="cursor">_</span>`;
    lastStatus = text;
  }
  requestDraw();
});

init();
