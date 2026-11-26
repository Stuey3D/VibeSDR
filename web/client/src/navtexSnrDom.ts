/**
 * navtexSnrDom — beta96 (Stuart 11-26): NAVTEX's decode quality and SNR timeline, drawn in the web client.
 * The rules (colours, ranges, lines) are src/utils/navtexQuality.ts — the app draws the same from the same file.
 *
 *  · snrHBar      — the thin HORIZONTAL timeline (gallery list rows, the live decoder status): a segment per slice.
 *  · qualityBar   — the small 3-colour clean / FEC / lost bar.
 *  · paintSnrStrip — the VERTICAL strip down the LEFT of a message's text (4 px + 6 px gap): each RENDERED line is
 *    coloured by the worst slice it was decoded in. Measured from the laid-out text (Range.getClientRects: one rect per
 *    line box a slice's characters touch), so it follows re-wrapping at any width; repainted when the box resizes.
 * Nothing here is interactive (a title tooltip only) — nothing for a pad to reach.
 */
import { lineWorst, snrBand, snrColour, SNR_COLOURS, SNR_RED_DB, SNR_GREEN_DB, navtexSplitPct, type NavtexSnr, type NavtexSplit } from '../../../src/utils/navtexQuality';

/** Fill `el` with the timeline (hidden without one). */
export function snrHBar(el: HTMLElement, q: NavtexSnr | null | undefined): void {
  el.textContent = '';
  if (!q || !q.tl.length) { el.hidden = true; return; }
  el.hidden = false;
  const tot = q.tl.length * q.sliceSec;
  el.title = `SNR over the message, ${q.sliceSec} s a segment (${tot} s): red under ${SNR_RED_DB} dB, amber ${SNR_RED_DB}–${SNR_GREEN_DB}, green from ${SNR_GREEN_DB}`;
  el.setAttribute('role', 'img');
  el.setAttribute('aria-label', `SNR timeline: ${q.tl.filter((d) => snrBand(d) === 'red').length} of ${q.tl.length} segments under ${SNR_RED_DB} dB`);
  for (const db of q.tl) {
    const s = document.createElement('i');
    s.style.background = snrColour(db);
    el.appendChild(s);
  }
}
/** The 3-colour clean / FEC / lost bar (hidden without a split). */
export function qualityBar(el: HTMLElement, s: NavtexSplit | null | undefined): void {
  el.textContent = '';
  const p = navtexSplitPct(s);
  if (!p) { el.hidden = true; return; }
  el.hidden = false;
  el.setAttribute('role', 'img');
  el.setAttribute('aria-label', `Decode quality: ${p.clean.toFixed(1)} % clean, ${p.fec.toFixed(1)} % FEC, ${p.lost.toFixed(1)} % lost`);
  for (const [w, c] of [[p.clean, SNR_COLOURS.green], [p.fec, SNR_COLOURS.amber], [p.lost, SNR_COLOURS.red]] as const) {
    if (w <= 0) continue;
    const i = document.createElement('i'); i.style.flexGrow = String(w); i.style.background = c; el.appendChild(i);
  }
}

/** ★ The vertical strip. `wrap` holds `strip` (absolutely placed, clipping) and `pre` (the text, ONE text node); `ranges`
 *  are the slices as character ranges of the pre's text (snrRanges). Empty → the strip is cleared and the wrap loses its
 *  gutter. Placed against the STRIP's own top, so a pre that scrolls itself (the gallery's) is repainted on scroll. */
export function paintSnrStrip(wrap: HTMLElement, strip: HTMLElement, pre: HTMLElement,
                              ranges: { start: number; end: number; db: number }[] | null): void {
  strip.textContent = '';
  const node = pre.firstChild;
  if (!ranges || !ranges.length || !node || node.nodeType !== Node.TEXT_NODE) { wrap.classList.remove('snr'); return; }
  wrap.classList.add('snr');
  const len = (node as Text).length;
  const top0 = strip.getBoundingClientRect().top;
  // every line box as [top, bottom] with the worst dB over it: worst paints last, so a shared line takes the worst
  const lines = new Map<number, { top: number; bottom: number; db: number }>();
  const r = document.createRange();
  for (const g of ranges) {
    const a = Math.max(0, Math.min(len, g.start)), b = Math.max(a, Math.min(len, g.end));
    r.setStart(node, a); r.setEnd(node, b > a ? b : Math.min(len, a + 1));   // an empty slice: the line it sits on
    for (const rc of Array.from(r.getClientRects())) {
      if (rc.height <= 0) continue;
      const key = Math.round(rc.top);
      const cur = lines.get(key);
      if (!cur) lines.set(key, { top: rc.top, bottom: rc.bottom, db: g.db });
      else { cur.bottom = Math.max(cur.bottom, rc.bottom); if (g.db < cur.db) cur.db = g.db; }
    }
  }
  const frag = document.createDocumentFragment();
  for (const l of lines.values()) {
    const i = document.createElement('i');
    i.style.top = `${l.top - top0}px`; i.style.height = `${Math.max(1, l.bottom - l.top)}px`; i.style.background = snrColour(l.db);
    frag.appendChild(i);
  }
  strip.appendChild(frag);
}
/** The same rule as data, for the tests: the worst dB per laid-out line (lineWorst from navtexQuality). */
export { lineWorst };

/** Repaint the strip when the text box changes width (re-wrapping) — and, for a pre that scrolls itself, on its scroll.
 *  One observer per wrap; the latest `paint` is the one run. */
const observed = new WeakMap<HTMLElement, () => void>();
export function watchSnrStrip(wrap: HTMLElement, paint: () => void, scroller?: HTMLElement): void {
  const had = observed.has(wrap);
  observed.set(wrap, paint);
  if (had) return;
  if (scroller) {
    let q = false;
    scroller.addEventListener('scroll', () => {
      if (q || !wrap.classList.contains('snr')) return;
      q = true; requestAnimationFrame(() => { q = false; observed.get(wrap)?.(); });
    }, { passive: true });
  }
  if (typeof ResizeObserver === 'undefined') return;
  let w = 0;
  new ResizeObserver((e) => {
    const nw = Math.round(e[0]?.contentRect.width ?? 0);
    if (nw === w) return;
    w = nw;
    requestAnimationFrame(() => observed.get(wrap)?.());
  }).observe(wrap);
}
