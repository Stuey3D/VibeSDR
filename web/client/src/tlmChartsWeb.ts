/**
 * ★★ V12 beta91 TELEMETRY CHARTS, the web's drawing (the app's is src/components/TlmCharts.tsx; the geometry and the words
 * are both src/utils/tlmCharts.ts). Small multiples in inline SVG: THIS PASS (readings against time) and the LAST passes
 * (each pass's mean, its min–max shaded). One hue — the page's amber — the text in the page's text inks, so it follows
 * every theme (dark / light faceplates) through the same CSS variables the panel already uses.
 */
import { tlmAcross, tlmAcrossHead, tlmChartOrder, tlmPassHead, tlmRangeText, tlmSpark, tlmValueText, TLM_FIRST,
         type TlmChannel, type TlmPass } from '../../../src/utils/tlmCharts';

export const TLM_CSS = `
.tlm{margin-top:4px;font:12px/1.35 ui-monospace,Menlo,monospace}
.tlm .tlm-h{letter-spacing:.06em;margin:4px 0 3px}
.tlm .tlm-g{display:grid;gap:8px 12px}
.tlm .tlm-c{min-width:0}
.tlm .tlm-n{font-size:11px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.tlm .tlm-t{display:flex;justify-content:space-between;align-items:baseline;gap:6px;margin-top:1px}
.tlm .tlm-v{font-variant-numeric:tabular-nums;white-space:nowrap}
.tlm .tlm-f{color:var(--text-dim);font-size:10px;font-variant-numeric:tabular-nums;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.tlm svg{display:block}
.tlm .tlm-l{stroke:var(--amber,#ffb000);fill:none;stroke-width:1.5;stroke-linejoin:round;stroke-linecap:round}
.tlm .tlm-b{fill:var(--amber,#ffb000);fill-opacity:.16;stroke:none}
.tlm .tlm-d{fill:var(--amber,#ffb000)}
.tlm .tlm-0{stroke:var(--btn-border,#444);stroke-width:1}
.tlm .tlm-note{color:var(--text-dim);font-size:10px;margin-top:4px}
.tlm button.tlm-more{margin:6px 0 2px;min-height:28px}
`;
let cssDone = false;
/** The charts' style sheet, once per page. */
export function tlmEnsureCss() {
  if (cssDone || typeof document === 'undefined') return;
  const st = document.createElement('style'); st.id = 'tlmCss'; st.textContent = TLM_CSS; document.head.appendChild(st); cssDone = true;
}
const esc = (s: string) => s.replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]!));
const H = 34;

function cell(ch: TlmChannel, t: number[], v: (number | null)[], w: number, band?: { lo: (number | null)[]; hi: (number | null)[] }): string {
  const sp = tlmSpark(t, v, w, H, 3, band ? Infinity : undefined, band?.lo, band?.hi);
  const range = sp.n ? tlmRangeText(band?.lo ?? v, ch, band?.hi) : '';
  const label = `${ch.name}${ch.raw ? ', raw' : ''}: ${sp.n} ${band ? 'passes' : 'readings'}${range ? `, ${range}` : ''}, latest ${tlmValueText(sp.lastV, ch)}`;
  return `<div class="tlm-c" role="img" aria-label="${esc(label)}" title="${esc(label)}">`
    + `<div class="tlm-n">${esc(ch.name)}</div>`
    + `<svg width="${w}" height="${H}" viewBox="0 0 ${w} ${H}" aria-hidden="true"><line class="tlm-0" x1="0" y1="${H - 0.5}" x2="${w}" y2="${H - 0.5}"/>`
    + (sp.band ? `<path class="tlm-b" d="${sp.band}"/>` : '')
    + (sp.line ? `<path class="tlm-l" d="${sp.line}"/>` : '')
    + (sp.last ? `<circle class="tlm-d" cx="${sp.last.x.toFixed(1)}" cy="${sp.last.y.toFixed(1)}" r="2.5"/>` : '')
    + `</svg><div class="tlm-t"><span class="tlm-v">${esc(tlmValueText(sp.lastV, ch))}</span><span class="tlm-f">${esc(range)}</span></div></div>`;
}

/**
 * The charts as HTML for a box `width` px wide. `all` = every channel (else TLM_FIRST and a "+ N MORE CHANNELS" key,
 * `data-act="tlmall"` — the caller toggles it). `across` null = still loading (`acrossNote` says so / why).
 */
export function tlmChartsHtml(o: { pass: TlmPass | null; live: boolean; across: TlmPass[] | null; acrossNote?: string; width: number; all: boolean }): string {
  const w = Math.max(120, Math.floor(o.width));
  const cols = w >= 480 ? 3 : w >= 280 ? 2 : 1, gap = 12;
  const cw = Math.floor((w - gap * (cols - 1)) / cols);
  const ref = o.pass ?? (o.across && o.across.length ? o.across[o.across.length - 1] : null);
  const order = tlmChartOrder(ref);
  const shown = o.all ? order : order.slice(0, TLM_FIRST);
  const grid = (cells: string[]) => `<div class="tlm-g" style="grid-template-columns:repeat(${cols},${cw}px)">${cells.join('')}</div>`;
  let h = `<div class="tlm"><div class="tlm-h">${esc(tlmPassHead(o.pass, o.live))}</div>`;
  const p = o.pass;
  if (p && p.t.length) {
    h += grid(shown.map((i) => cell(p.channels[i], p.t, p.v[i], cw)));
  }
  if (order.length > TLM_FIRST) h += `<button class="btn tlm-more" data-act="tlmall" aria-expanded="${o.all}">${o.all ? '− FEWER CHANNELS' : `+ ${order.length - TLM_FIRST} MORE CHANNELS`}</button>`;
  h += `<div class="tlm-h" style="margin-top:8px">${esc(o.across == null ? (o.acrossNote || 'LAST PASSES · loading from the gallery…') : tlmAcrossHead(o.across.length, o.across))}</div>`;
  if (o.across && o.across.length && ref) {
    h += grid(shown.map((i) => { const a = tlmAcross(o.across!, ref.channels[i].name); return cell(ref.channels[i], a.t, a.mean, cw, { lo: a.min, hi: a.max }); }));
  }
  return h + `<div class="tlm-note">raw = sent uncalibrated (no published calibration) · times UTC · saved to the gallery a pass at a time</div></div>`;
}
