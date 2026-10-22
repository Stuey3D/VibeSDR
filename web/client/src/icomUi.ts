/**
 * icomUi.ts — THE ICOM PANEL in the browser (driver "icom", RECEIVE ONLY). Browser parity with the app's
 * Icom panel: the web client is the moat, so a listener in a browser gets every Icom control the app has.
 *
 * ★★★ EVERY LIST AND EVERY CURRENT VALUE IS THE RADIO'S — hwinfo.radio plus the `{"type":"icom"}` status
 *     line, parsed by src/services/icomRadio.ts (the module the app reads too). Nothing here is a constant
 *     of ours, and nothing paints from what this page last SENT: the radio answers, the server re-broadcasts,
 *     and only then does a button light (the shared-dial contract — the radio IS the server state).
 * ★★ WHO MAY: tune / mode / FIL / span / scope mode follow the OWNER LOCK (icomMayTune); ATT / preamp / AGC /
 *    RF gain / NB / NR / ANF / scope speed are admin-only unless the owner opened them (icomMayRf). A control
 *    this listener may not use is DISABLED with the reason beside it, never left to bounce off the server.
 * ★ Built in the panel's own vocabulary (.mrow / .seg / .btn) so it reads as one more radio panel beside the
 *   RSP / HF+ / Airspy / HackRF ones, in every theme. Standard font — the display font is for the main
 *   controls and the VTS only.
 * ★ Structure is rebuilt only when the radio's LISTS change (a new radio); state is painted in place, so the
 *   RF gain slider survives a status line arriving mid-drag.
 */
import {
  icomModeLabel, icomFilterWidths, fmtWidth, fmtSpan, fmtAtt, fmtPreamp, rfGainPct,
  icomMayTune, icomMayRf, icomFollowNote, nearestSpanIndex, icomWidthFamily,
  type IcomRadio, type IcomControlFields,
} from '../../../src/services/icomRadio';

export interface IcomPanelDeps {
  /** Send icom_control (validated against the radio's lists by the caller's SpectrumClient). */
  send: (f: IcomControlFields) => void;
}

let builtKey = '';
let rfDragging = false;

const esc = (s: string) => s.replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c] || c));
const upper = (s: string) => s.toUpperCase();

function seg(id: string, items: { v: string; label: string; title?: string }[]): string {
  return `<div class="seg" id="${id}" style="flex-wrap:wrap">`
    + items.map((it) => `<button class="btn" data-v="${esc(it.v)}"${it.title ? ` title="${esc(it.title)}"` : ''}>${esc(it.label)}</button>`).join('')
    + '</div>';
}
// ★ The label may shrink and wrap (min-width 0): a sub-line must never push a phone-width panel sideways.
function row(label: string, body: string, id = ''): string {
  return `<div class="mrow"${id ? ` id="${id}"` : ''}><label style="min-width:0;flex:0 1 auto">${label}</label>${body}</div>`;
}

/** Build (once per radio shape) and paint the panel into `root` (#icomCtls). */
export function renderIcomPanel(root: HTMLElement, r: IcomRadio | null, admin: boolean, deps: IcomPanelDeps) {
  if (!r) { root.hidden = true; return; }
  root.hidden = false;
  const key = JSON.stringify([r.model, r.modes, r.filters, r.att, r.preamp, r.agc, r.rfGain, r.nb, r.nr, r.anf,
                              r.scope.spans, r.scope.modes, r.scope.speeds]);
  if (key !== builtKey) {
    builtKey = key;
    let h = '';
    h += `<div class="lbl wrap" id="icomFollow" hidden style="color:var(--amber, #ffcc88)"></div>`;
    h += row('RADIO MODE', seg('icomMode', r.modes.map((m) => ({ v: m, label: icomModeLabel(m) }))));
    if (r.filters.length)
      h += row('FILTER<span class="sub" id="icomFilSub">approx. factory widths</span>',
               seg('icomFil', r.filters.map((f) => ({ v: String(f), label: `FIL${f}` }))));
    if (r.scope.spans.length)
      h += row('SPAN<span class="sub">the zoom keys step it</span>',
               seg('icomSpan', r.scope.spans.map((s) => ({ v: String(s), label: fmtSpan(s), title: `${s / 1000} kHz wide` }))));
    if (r.scope.modes.length > 1)
      h += row('SCOPE', seg('icomScopeMode', r.scope.modes.map((m) => ({ v: m, label: upper(m),
        title: m === 'fixed' ? 'The window stays put; the dial moves inside it' : 'The window follows the dial' }))));
    h += `<div class="lbl wrap" id="icomRfNote" hidden>Only the owner can change this radio’s RF settings. You can still see what they are set to.</div>`;
    if (r.scope.speeds.length > 1) h += row('SCOPE SPEED', seg('icomScopeSpeed', r.scope.speeds.map((s) => ({ v: s, label: upper(s) }))));
    if (r.att.length > 1) h += row('ATT', seg('icomAtt', r.att.map((a) => ({ v: String(a), label: fmtAtt(a) }))));
    if (r.preamp.length > 1) h += row('PREAMP', seg('icomPreamp', r.preamp.map((p) => ({ v: String(p), label: fmtPreamp(p) }))));
    if (r.agc.length > 1) h += row('AGC<span class="sub">the radio’s own</span>', seg('icomAgc', r.agc.map((a) => ({ v: a, label: upper(a) }))));
    if (r.rfGain)
      h += row('RF GAIN', `<input type="range" id="icomRfGain" min="${r.rfGain[0]}" max="${r.rfGain[1]}" value="${r.rfGainNow}">`
               + `<span class="val" id="icomRfGainVal">—</span>`);
    const dsp = [r.nb && ['nb', 'NB'], r.nr && ['nr', 'NR'], r.anf && ['anf', 'ANF']].filter(Boolean) as string[][];
    if (dsp.length)
      h += row('RADIO DSP<span class="sub">the radio’s own</span>',
               `<div class="seg" id="icomDsp">${dsp.map(([k, l]) => `<button class="btn" data-k="${k}">${l}</button>`).join('')}</div>`);
    h += `<div class="lbl wrap" id="icomScopeLine"></div>`;
    root.innerHTML = h;
    // ── wiring: every click SENDS and nothing else — the radio's answer paints the button ──
    const on = (id: string, f: (v: string) => IcomControlFields) => {
      const el = root.querySelector('#' + id);
      if (!el) return;
      el.addEventListener('click', (e) => {
        const b = (e.target as HTMLElement).closest('button') as HTMLButtonElement | null;
        if (!b || b.disabled) return;
        deps.send(f(b.dataset.v || ''));
      });
    };
    on('icomMode', (v) => ({ mode: v }));
    on('icomFil', (v) => ({ fil: Number(v) }));
    on('icomSpan', (v) => ({ span: Number(v) }));
    on('icomScopeMode', (v) => ({ scopeMode: v }));
    on('icomScopeSpeed', (v) => ({ scopeSpeed: v }));
    on('icomAtt', (v) => ({ att: Number(v) }));
    on('icomPreamp', (v) => ({ preamp: Number(v) }));
    on('icomAgc', (v) => ({ agc: v }));
    root.querySelector('#icomDsp')?.addEventListener('click', (e) => {
      const b = (e.target as HTMLElement).closest('button') as HTMLButtonElement | null;
      if (!b || b.disabled) return;
      const k = b.dataset.k as 'nb' | 'nr' | 'anf';
      deps.send({ [k]: !b.classList.contains('on') });
    });
    const rf = root.querySelector('#icomRfGain') as HTMLInputElement | null;
    if (rf) {
      rf.addEventListener('pointerdown', () => { rfDragging = true; });
      rf.addEventListener('input', () => {
        const v = root.querySelector('#icomRfGainVal');
        if (v) v.textContent = `${rfGainPct(Number(rf.value), lastR?.rfGain ?? null)} %`;
      });
      // ★ One message per RELEASE, not per pixel: CI-V is a serial line shared with the scope.
      rf.addEventListener('change', () => { rfDragging = false; deps.send({ rfGain: Number(rf.value) }); });
    }
  }
  lastR = r;
  paint(root, r, admin);
}
let lastR: IcomRadio | null = null;

function lightSeg(root: HTMLElement, id: string, cur: string, enabled: boolean) {
  const el = root.querySelector('#' + id);
  if (!el) return;
  for (const b of Array.from(el.querySelectorAll('button')) as HTMLButtonElement[]) {
    const v = b.dataset.v ?? '';
    if (b.classList.contains('on') !== (v === cur)) b.classList.toggle('on', v === cur);
    if (b.disabled !== !enabled) b.disabled = !enabled;
  }
}

function paint(root: HTMLElement, r: IcomRadio, admin: boolean) {
  const mayTune = icomMayTune(r, admin), mayRf = icomMayRf(r, admin);
  const follow = root.querySelector('#icomFollow') as HTMLElement | null;
  if (follow) { const t = icomFollowNote(r, admin); follow.hidden = !t; follow.textContent = t; }
  const rfNote = root.querySelector('#icomRfNote') as HTMLElement | null;
  if (rfNote) rfNote.hidden = mayRf;
  lightSeg(root, 'icomMode', r.modeNow, mayTune);
  // ★ FIL buttons carry the width for THIS mode family — "FIL2 · 2.4k".
  const widths = icomFilterWidths(r);
  const fil = root.querySelector('#icomFil');
  if (fil) {
    for (const b of Array.from(fil.querySelectorAll('button')) as HTMLButtonElement[]) {
      const f = Number(b.dataset.v);
      const t = `FIL${f}${widths[f - 1] ? ` · ${fmtWidth(widths[f - 1])}` : ''}`;
      if (b.textContent !== t) b.textContent = t;
    }
    const sub = root.querySelector('#icomFilSub');
    if (sub) sub.textContent = `${icomWidthFamily(r.modeNow).toUpperCase()} · approx. factory widths`;
  }
  lightSeg(root, 'icomFil', String(r.filNow), mayTune);
  const si = nearestSpanIndex(r.scope.spans, r.scope.span);
  lightSeg(root, 'icomSpan', si >= 0 ? String(r.scope.spans[si]) : '', mayTune && r.scope.mode !== 'fixed');
  lightSeg(root, 'icomScopeMode', r.scope.mode, mayTune);
  lightSeg(root, 'icomScopeSpeed', '', mayRf);   // ★ the radio does not report its speed back — no lit button
  lightSeg(root, 'icomAtt', String(r.attNow), mayRf);
  lightSeg(root, 'icomPreamp', String(r.preampNow), mayRf);
  lightSeg(root, 'icomAgc', r.agcNow, mayRf);
  const dsp = root.querySelector('#icomDsp');
  if (dsp) for (const b of Array.from(dsp.querySelectorAll('button')) as HTMLButtonElement[]) {
    const k = b.dataset.k;
    const on = k === 'nb' ? r.nbNow : k === 'nr' ? r.nrNow : r.anfNow;
    if (b.classList.contains('on') !== on) b.classList.toggle('on', on);
    if (b.disabled !== !mayRf) b.disabled = !mayRf;
  }
  const rf = root.querySelector('#icomRfGain') as HTMLInputElement | null;
  if (rf) {
    if (!rfDragging && document.activeElement !== rf && rf.value !== String(r.rfGainNow)) rf.value = String(r.rfGainNow);
    rf.disabled = !mayRf;
    const v = root.querySelector('#icomRfGainVal');
    if (v && !rfDragging) v.textContent = `${rfGainPct(r.rfGainNow, r.rfGain)} %`;
  }
  const line = root.querySelector('#icomScopeLine');
  if (line) {
    const t = `Scope: ${r.scope.points || 475} points, ${r.scope.fps > 0 ? `${r.scope.fps.toFixed(1)} lines/s` : 'waiting'}`
      + (r.scope.mode === 'fixed' ? ' · fixed window: the span is set on the radio' : '')
      + ' · the radio’s own scope and audio, not IQ — receive only.';
    if (line.textContent !== t) line.textContent = t;
  }
}

/** ★ The passband to DRAW for the radio's FIL (approximate: factory widths), in the client's offsets. Written
 *  straight into the view, never sent — the radio owns its filter. null = leave the mode default. */
export function icomPassband(r: IcomRadio | null, clientMode: string, modeDefault: [number, number]): [number, number] | null {
  if (!r) return null;
  const w = icomFilterWidths(r)[r.filNow - 1];
  if (!(w > 0)) return null;
  switch (clientMode) {
    case 'usb': return [50, 50 + w];
    case 'lsb': return [-(50 + w), -50];
    case 'cwu': case 'cwl': { const c = (modeDefault[0] + modeDefault[1]) / 2; return [Math.round(c - w / 2), Math.round(c + w / 2)]; }
    default: return [-Math.round(w / 2), Math.round(w / 2)];
  }
}
