// DOM helpers for a JUCE WebView plugin page that receives status events many times a second.
//
// WKWebView drops a click when the text node under the pointer is replaced between mouse down
// and mouse up, so per-tick code must never rewrite clickable elements unless something changed.

// Write text only when it changed.
export function setText(node, text) {
  if (node.textContent !== text) node.textContent = text;
}

// Write a class list only when it changed.
export function setClass(node, name) {
  if (node.className !== name) node.className = name;
}

// For a button whose label changes while it may be pressed (>>RECORD / >>RECORDING).
// Needs: <button class="btn live-label" data-label="&gt;&gt;RECORD" aria-label="RECORD"></button>
// and in app.css:  .live-label::after { content: attr(data-label); }
export function setLabel(btn, text) {
  if (btn.dataset.label !== text) {
    btn.dataset.label = text;
    btn.setAttribute('aria-label', text.replace('>>', ''));
  }
}

// Pointer capture can throw in WKWebView; track drags on window so they work without it.
export function trackDrag(el, { down, move, up }) {
  let active = false;
  el.addEventListener('pointerdown', (e) => {
    if (e.button !== 0 || down(e) === false) return;
    active = true;
    try { el.setPointerCapture(e.pointerId); } catch { /* not capturable right now */ }
  });
  window.addEventListener('pointermove', (e) => { if (active) move(e); });
  const end = (e) => { if (active) { active = false; up(e); } };
  window.addEventListener('pointerup', end);
  window.addEventListener('pointercancel', end);
}

// Stop WebKit navigating to a dropped file; hand its bytes to a native function instead.
// upload(b64, first, last, name) must be a JUCE native function that appends and, on last, loads.
export function forwardDroppedFiles(upload, onProgress = () => {}) {
  window.addEventListener('dragover', (e) => e.preventDefault());
  window.addEventListener('drop', async (e) => {
    e.preventDefault();
    const file = e.dataTransfer?.files?.[0];
    if (!file) return;
    const bytes = new Uint8Array(await file.arrayBuffer());
    const piece = 3 * 256 * 1024;
    for (let pos = 0; pos < bytes.length || pos === 0; pos += piece) {
      const part = bytes.subarray(pos, pos + piece);
      let bin = '';
      for (let i = 0; i < part.length; i += 0x8000) bin += String.fromCharCode.apply(null, part.subarray(i, i + 0x8000));
      onProgress(file.name, Math.min(1, (pos + piece) / Math.max(1, bytes.length)));
      await upload(btoa(bin), pos === 0, pos + piece >= bytes.length, file.name);
      if (bytes.length === 0) break;
    }
  });
}

// The five Meow Terminal colours, read from CSS for canvas drawing. Call again if a theme changes.
export function readTokens() {
  const css = (n) => getComputedStyle(document.documentElement).getPropertyValue(n).trim();
  return { bg: css('--bg'), text: css('--text'), accent: css('--accent'), muted: css('--muted'), border: css('--border'), font: css('--font-mono') };
}
