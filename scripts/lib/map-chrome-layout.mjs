// map-chrome-layout.mjs — ★★★ b97 THE OVERLAP / VISIBILITY CHECK for a map on the card system (map-chrome.js /
// mapChrome.ts), shared by the amateur map's and the community map's e2e tests. Run in the page (an expression string):
//   const L = JSON.parse(await js(chromeLayoutExpr({ il, ir, ib })));
// → { short, w, h, pieces: [{ k, l, t, r, b }], outside: [...], overlaps: [[a, b, px²]], hidden: [...] }
//   · every piece of chrome that should be on screen IS (a visible box, at least 2 × 2 px, not hidden under another);
//   · inside the window (a border's width past an edge allowed — the pieces sit flush) and, in SHORT LANDSCAPE, inside
//     Stuart's safe rectangle (x from the left inset to the width less the right inset, above the bottom inset);
//   · no two controls overlap (an open card may lie over the LIST drawer: that pair is not counted).
// The insets are simulated the way test-web-map-landscape.mjs does it: --sai-l / -r / -b on the view (setInsets).
export const setInsetsExpr = (ins) => `(() => { const v = document.querySelector('.trkView'); if (!v) return 0;
  for (const [k, x] of Object.entries(${JSON.stringify(ins || {})})) { if (x == null) v.style.removeProperty('--sai-' + k); else v.style.setProperty('--sai-' + k, x + 'px'); }
  window.dispatchEvent(new Event('resize')); return 1; })()`;

export const chromeLayoutExpr = (ins = {}) => `JSON.stringify((() => {
  const IL = ${Number(ins.l) || 0}, IR = ${Number(ins.r) || 0}, IB = ${Number(ins.b) || 0};
  const v = document.querySelector('.trkView:not([hidden])');
  if (!v) return { none: true };
  const W = Math.round(window.visualViewport ? visualViewport.width : innerWidth), H = Math.round(window.visualViewport ? visualViewport.height : innerHeight);
  const short = v.hasAttribute('data-short');
  const SEL = [['x', '.trkTop .exit'], ['help', '.mHelp'], ['scale', '.mChipsScale .mScale'], ['zoom', '.maplibregl-ctrl-top-left .maplibregl-ctrl-group, .leaflet-top.leaflet-left .leaflet-control-zoom']];
  const pieces = [], hidden = [];
  const vis = (e) => { if (!e || !e.getClientRects().length) return null; const cs = getComputedStyle(e); if (cs.visibility === 'hidden' || cs.display === 'none' || Number(cs.opacity) < 0.05) return null;
    const q = e.getBoundingClientRect(); return q.width >= 2 && q.height >= 2 ? q : null; };
  for (const [k, s] of SEL) { const e = v.querySelector(s); const q = vis(e); if (q) pieces.push({ k, l: q.left, t: q.top, r: q.right, b: q.bottom, e }); else if (k !== 'zoom' && k !== 'scale') hidden.push(k); }
  for (const d of v.querySelectorAll('.mDock:not([hidden]):not(.mScale)')) { const q = vis(d.querySelector('.hCard')); if (q) pieces.push({ k: 'chip:' + d.dataset.dock, l: q.left, t: q.top, r: q.right, b: q.bottom, e: d }); else hidden.push('chip:' + d.dataset.dock); }
  for (const c of v.querySelectorAll('.trkCards .trkCard2:not([hidden])')) { const q = vis(c); if (q) pieces.push({ k: 'card:' + c.dataset.card, l: q.left, t: q.top, r: q.right, b: q.bottom, e: c }); }
  // ★ the collapsed LIST chip sits past the left and bottom edges by its border + radius in portrait / desktop (by design)
  const past = (p) => (p.k === 'chip:list' && !short ? 16 : 3);
  const outside = pieces.filter((p) => p.l < -past(p) || p.t < -3 || p.r > W + 3 || p.b > H + past(p)
    || (short && (p.l < IL - 3 || p.r > W - IR + 3 || p.b > H - IB + 3))).map((p) => p.k);
  // ★ covered: the piece's centre answers with something else of the chrome (a card over a chip)
  for (const p of pieces) { if (p.k.startsWith('card:')) continue; const x = (p.l + p.r) / 2, y = (p.t + p.b) / 2; const at = document.elementFromPoint(Math.min(W - 1, Math.max(0, x)), Math.min(H - 1, Math.max(0, y)));
    if (at && !p.e.contains(at) && at.closest('.trkCard2, .mDock, .mHelp, .trkTop')) p.under = (at.closest('[data-card]') || at.closest('[data-dock]') || at).getAttribute('data-card') || at.closest('[data-dock]')?.dataset.dock || at.className; }
  const overlaps = [];
  for (let i = 0; i < pieces.length; i++) for (let j = i + 1; j < pieces.length; j++) {
    const a = pieces[i], b = pieces[j];
    if ((a.k === 'card:list' || b.k === 'card:list') && (a.k.startsWith('card:') || b.k.startsWith('card:'))) continue;   // a card may lie over the drawer
    if ((a.k === 'card:list' && b.k === 'chip:list') || (b.k === 'card:list' && a.k === 'chip:list')) continue;              // the chip is the drawer's own foot
    // a chip and the card it opened are one piece (the card grows out of it in place)
    if (a.k.startsWith('chip:') && b.k === 'card:' + a.k.slice(5) || b.k.startsWith('chip:') && a.k === 'card:' + b.k.slice(5)) continue;
    const w = Math.min(a.r, b.r) - Math.max(a.l, b.l), h = Math.min(a.b, b.b) - Math.max(a.t, b.t);
    if (w > 2 && h > 2) overlaps.push([a.k, b.k, Math.round(w * h)]);
  }
  const covered = pieces.filter((p) => p.under).map((p) => p.k + ' under ' + p.under);
  return { short, w: W, h: H, pieces: pieces.map(({ k, l, t, r, b }) => ({ k, l: Math.round(l), t: Math.round(t), r: Math.round(r), b: Math.round(b) })), outside, overlaps, hidden, covered };
})())`;

/** The sizes every map is checked at (Stuart's devices): [name, w, h, insets, mobile]. */
export const MAP_SIZES = [
  ['desktop 1440×900', 1440, 900, {}, false],
  ['portrait 390×844', 390, 844, {}, true],
  ['Android 412×915', 412, 915, {}, true],
  ['Android 915×412', 915, 412, {}, true],
  ['iPhone 956×376 (L62 R62 B20)', 956, 376, { l: 62, r: 62, b: 20 }, true],
  ['SE 667×320', 667, 320, {}, true],
];
