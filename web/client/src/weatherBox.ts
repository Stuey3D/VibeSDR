/**
 * weatherBox.ts — the RTTY box's translated WEATHER REPORTS (V12, 2026-10-23). The SERVER reads each SYNOP/SHIP report
 * out of the RTTY text (vibe_synop.h) and sends it as the 0x08 frame (src/utils/weatherReport.ts); main.ts's onWeatherReport
 * puts it here, at the end of #decText — under the raw groups it came from. Collapsed to its one line (<details>), open it
 * for what each group said and the raw groups again.
 * ★ Text only: every string goes in by textContent, never as markup — a report is data from the air.
 * ★ Its own module so the DOM test (scripts/test-web-weather-box.mjs) can drive it without the whole client.
 */
import { WX_MAX_KEPT, type WeatherReport } from '../../../src/utils/weatherReport';

/** ★ One translated weather report as the RTTY box draws it: a <details> whose summary is the line (V12, 2026-10-23).
 *  Text only — every string goes in by textContent, never as markup. */
export function weatherReportEl(r: WeatherReport): HTMLElement {
  const d = document.createElement('details');
  d.className = 'wxRep';
  const s = document.createElement('summary');
  s.textContent = r.line;
  d.appendChild(s);
  const body = document.createElement('div');
  body.className = 'wxRepBody';
  for (const t of r.details) { const p = document.createElement('div'); p.textContent = t; body.appendChild(p); }
  if (r.raw) { const p = document.createElement('div'); p.className = 'wxRepRaw'; p.textContent = r.raw; body.appendChild(p); }
  d.appendChild(body);
  return d;
}
/** ★ The RTTY box's cap once it holds reports as well as text: about `keep` characters of text and WX_MAX_KEPT reports,
 *  trimmed from the top in batches of `slack` (the single-node rule in onText, for a box with several nodes). */
export function trimWxBox(el: HTMLElement, keep: number, slack: number) {
  let total = 0, reports = 0;
  for (let n = el.firstChild; n; n = n.nextSibling) {
    if (n.nodeType === 3) total += (n as Text).length;
    else if ((n as Element).classList?.contains('wxRep')) reports++;
  }
  if (total <= keep + slack && reports <= WX_MAX_KEPT) return;
  while (el.firstChild && el.firstChild !== el.lastChild && (total > keep || reports > WX_MAX_KEPT)) {
    const n = el.firstChild;
    if (n.nodeType === 3) {
      const t = n as Text, over = total - keep;
      if (over > 0 && over < t.length) { t.deleteData(0, over); total -= over; continue; }
      total -= t.length;
    } else if ((n as Element).classList?.contains('wxRep')) reports--;
    el.removeChild(n);
  }
}

/** ★ The text node #decText's onText appends to. Without a report: the box's ONE text node (the long-standing rule —
 *  anything else a writer left is folded into one). With reports: the text after the newest report, in its own node, so
 *  text keeps arriving UNDER it and the reports stay where they landed. */
export function decTextNode(el: HTMLElement): Text {
  if (el.firstChild && el.firstChild === el.lastChild && el.firstChild.nodeType === 3) return el.firstChild as Text;
  const last = el.lastChild;
  if (last && last.nodeType === 3 && el.querySelector('.wxRep')) return last as Text;
  if (last && (last as Element).classList?.contains('wxRep')) {
    const n = document.createTextNode('');
    el.appendChild(n);
    return n;
  }
  const n = document.createTextNode(el.textContent || '');
  el.textContent = '';
  el.appendChild(n);
  return n;
}
