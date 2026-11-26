// Stand-in for JUCE's bridge (juce/index.js) so ui/ can run in a plain browser (tools/ui_test.mjs).
// It answers the same native functions as Source/PluginEditor.cpp and fakes a singer: a slightly
// sharp, wobbly melody, tuned to the enabled notes, sent as 30 Hz "tick" events.
const listeners = {};
window.__JUCE__ = { backend: { addEventListener(n, f) { (listeners[n] ||= []).push(f); } } };
const emit = (n, p) => (listeners[n] || []).forEach((f) => f(p));

const defs = [
  ['amount', 0, 100, 1, 100], ['glide', 0, 500, 1, 20], ['pitch', -24, 24, 0.01, 0],
  ['formantCorrect', 0, 100, 1, 100], ['formantShift', -12, 12, 0.01, 0], ['unison', 1, 8, 1, 1],
  ['detune', 0, 100, 0.1, 15], ['spread', 0, 100, 1, 50], ['mix', 0, 100, 1, 100],
  ['output', -24, 24, 0.1, 0], ['midiMode', 0, 1, 1, 0], ['key', 0, 11, 1, 0], ['scale', 0, 19, 1, 0], ['range', 0, 3, 1, 1],
  ...Array.from({ length: 12 }, (_, i) => [`note${i}`, 0, 1, 1, 1]),
];
// Same order as Scales::list in Source/PluginProcessor.cpp.
const scales = [
  ['chromatic', 'CHROMATIC', [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]], ['major', 'MAJOR', [0, 2, 4, 5, 7, 9, 11]],
  ['minor', 'MINOR', [0, 2, 3, 5, 7, 8, 10]], ['harmonic', 'HARMONIC MINOR', [0, 2, 3, 5, 7, 8, 11]],
  ['melodic', 'MELODIC MINOR', [0, 2, 3, 5, 7, 9, 11]], ['dorian', 'DORIAN', [0, 2, 3, 5, 7, 9, 10]],
  ['phrygian', 'PHRYGIAN', [0, 1, 3, 5, 7, 8, 10]], ['lydian', 'LYDIAN', [0, 2, 4, 6, 7, 9, 11]],
  ['mixolydian', 'MIXOLYDIAN', [0, 2, 4, 5, 7, 9, 10]], ['locrian', 'LOCRIAN', [0, 1, 3, 5, 6, 8, 10]],
  ['majpent', 'MAJOR PENTATONIC', [0, 2, 4, 7, 9]], ['minpent', 'MINOR PENTATONIC', [0, 3, 5, 7, 10]],
  ['blues', 'BLUES', [0, 3, 5, 6, 7, 10]], ['phrygdom', 'PHRYGIAN DOMINANT', [0, 1, 4, 5, 7, 8, 10]],
  ['hungarian', 'HUNGARIAN MINOR', [0, 2, 3, 6, 7, 8, 11]], ['wholetone', 'WHOLE TONE', [0, 2, 4, 6, 8, 10]],
  ['diminished', 'DIMINISHED', [0, 2, 3, 5, 6, 8, 9, 11]], ['hirajoshi', 'HIRAJOSHI', [0, 2, 3, 7, 8]],
  ['insen', 'IN SEN', [0, 1, 5, 7, 10]], ['custom', 'CUSTOM', null],
].map(([id, name, steps]) => ({ id, name, steps }));
// TuneEngine::latencyFor at 48 kHz.
const ranges = [['BASS', 70, 2411], ['TENOR', 100, 1690], ['ALTO', 150, 1130], ['SOPRANO', 220, 775]].map(([name, hz, latency]) => ({ name, hz, latency }));
const st = {
  params: Object.fromEntries(defs.map((d) => [d[0], d[4]])),
  colours: {}, calls: [], held: [], gestures: {},
};
// The notes it tunes to: the scale from the key, or the note parameters in CUSTOM.
const enabled = (pc) => {
  const s = scales[st.params.scale];
  if (!s.steps) return st.params[`note${pc}`] >= 0.5;
  return s.steps.some((i) => (st.params.key + i) % 12 === pc);
};
window.__mock = { st, emit, scales };
window.__mock = { st, emit };

const impl = {
  getInit: () => ({
    params: defs.map(([id, min, max, step, def]) => ({ id, min, max, step, def, value: st.params[id] })),
    scales, ranges, colours: st.colours, version: 'mock', rate: 48000, latency: ranges[st.params.range].latency,
  }),
  setParam(id, v) { st.params[id] = v; },
  gesture(id, begin) { st.gestures[id] = (st.gestures[id] || 0) + (begin ? 1 : -1); },
  setColours(c) { st.colours = c || {}; return st.colours; },
};

export function getNativeFunction(name) {
  return async (...args) => {
    st.calls.push([name, ...args]);
    return impl[name](...args);
  };
}

// A melody in A minor, sung 30 cents sharp with vibrato.
const melody = [57, 60, 64, 62, 60, 59, 57, 55];
const start = performance.now();
let out = -1;
function snap(p) {
  let best = -1, bd = 99;
  for (let n = Math.round(p) - 7; n <= Math.round(p) + 7; n++)
    if (enabled(((n % 12) + 12) % 12) && Math.abs(p - n) < bd) { bd = Math.abs(p - n); best = n; }
  return best;
}
setInterval(() => {
  const t = (performance.now() - start) / 1000;
  const note = melody[Math.floor(t / 0.6) % melody.length];
  const gap = (t % 0.6) > 0.52;
  const hist = [];
  let inMidi = -1, target = -1;
  for (let k = 0; k < 6; k++) {
    const tt = t + k * 0.0053;
    inMidi = gap ? -1 : note + 0.3 + 0.25 * Math.sin(tt * 2 * Math.PI * 5.5) + st.params.pitch;
    target = inMidi < 0 ? -1 : snap(inMidi);
    const want = target < 0 ? inMidi : inMidi + (st.params.amount / 100) * (target - inMidi);
    out = inMidi < 0 ? -1 : out < 0 ? want : out + (want - out) * 0.3;
    hist.push(inMidi < 0 ? -1 : Math.round(inMidi * 100), out < 0 ? -1 : Math.round(out * 100));
  }
  emit('tick', {
    inHz: inMidi < 0 ? 0 : 440 * 2 ** ((inMidi - st.params.pitch - 69) / 12), inMidi, outMidi: out, target,
    inPeak: gap ? 0.01 : 0.4, outPeak: gap ? 0.01 : 0.38, hist,
    ...(window.__mockHeld?.length ? { held: window.__mockHeld.shift() } : {}),
  });
}, 33);
