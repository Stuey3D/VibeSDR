/**
 * ★★ V12 THE DECODER PANEL BY BAND (Stuart, 2026-10-28) — the web half of src/utils/decoderBands.ts (the app's
 * ModeSelector reads the same table): HF · VHF · UHF tabs opening on the dial's band, the decoders grouped by use inside
 * a band, and a decoder whose usual frequencies are nowhere near the dial DIMMED (.dimFar) — never disabled or hidden.
 *
 * ★★★ THE BUTTONS ARE MOVED, NOT COPIED. Every data-dec button (and DIGITAL SPOTS) is the one in index.html, with its
 *     handlers and its `hidden` (where this server lacks it, or the owner blocked it) untouched; a layout parks them all
 *     in #decPark and moves the open tab's into #decGroups. A decoder in two bands (DSC, CW, SSTV, APRS, DIGITAL SPOTS)
 *     is one node, shown under whichever tab is open.
 * ★ Whole-radio modes (DAB, ADS-B, ISM, NDB, AIS) stay in the web's MODE menu, beside the demodulators, as before — so a
 *   band with nothing here (UHF on the web) simply has no tab.
 * ★★★ ONE KEY SIZE, AND OPTIONS THAT POP OUT OF THEIR KEY (Stuart, 2026-10-29 — src/utils/decoderKeyGrid.ts, shared with
 *     the app): every key in TUNES THE RADIO ITSELF, the band groups and VIEWS is one column of one grid, sized by the
 *     widest legend against the panel's own width (sizeKeys); the selected decoder's .subPanel is moved onto the row
 *     directly under its key's row, on a tab of the key's colour (placeOptions).
 */
import { DECODER_PLACES, RADIO_BANDS, WHOLE_RADIO_HINT, decoderGroupIcon, decoderGroupLabel, decoderDim, openingBand,
         radioBandOf, type RadioBand } from '../../../src/utils/decoderBands';
import { modeIconFor, modeIconSvg } from '../../../src/utils/modeIcons';
import { decoderKeyGrid, optionsInsertAt, tabCorners, type KeyLegend } from '../../../src/utils/decoderKeyGrid';
import { setModeLabel } from './modeIconDom';

/** ★★★ A TUNES THE RADIO ITSELF key, as main.ts knows it — its own availability rule already applied (absent = not listed). */
export interface WholeRadioKey { id: string; label: string; title: string; on: boolean; dis: boolean; press: () => void }
let wholeKeys: () => WholeRadioKey[] = () => [];
/** main.ts hands over its keys once (the same toggles and caps as the mode menu's). */
export function setWholeRadioKeys(fn: () => WholeRadioKey[]): void { wholeKeys = fn; }

/** The section above the tabs: ADS-B · AIS · DAB · ISM · NDB, then DIGITAL SPOTS: BAND (the spots section's own node,
 *  moved in while that section is shown, with its reason line). Never dimmed — they tune the radio themselves. */
function layoutWholeRadio(): void {
  const sec = document.getElementById('wholeRadioSec');
  const row = document.getElementById('wholeRadio');
  if (!sec || !row) return;
  const band = document.getElementById('spotsBandBtn');
  const why = document.getElementById('spotsBandWhy');
  for (const b of Array.from(row.querySelectorAll('[data-whole]'))) b.remove();
  for (const k of wholeKeys()) {
    const b = document.createElement('button');
    b.className = 'btn' + (k.on ? ' on' : '') + (k.dis ? ' dis' : '');
    b.dataset.whole = k.id;
    b.title = k.title;
    if (k.dis) b.setAttribute('aria-disabled', 'true');
    setModeLabel(b, k.id, k.label);
    b.onclick = () => k.press();
    row.appendChild(b);
  }
  const spotsShown = !document.getElementById('spotsSection')?.hidden;
  const park = document.getElementById('spotsPark');
  if (band && why) {
    // ★ In TUNES THE RADIO ITSELF it says DIGI BAND (the app's legend — the full "DIGITAL SPOTS: BAND" was the widest
    //   key on the panel and halved every row); back in its own section it says it in full.
    if (spotsShown) { row.appendChild(band); sec.appendChild(why); bandLabel(band, true); }
    else if (park) { park.appendChild(band); park.after(why); bandLabel(band, false); }
  }
  const hint = document.getElementById('wholeRadioHint');
  if (hint) hint.textContent = WHOLE_RADIO_HINT;
  sec.hidden = !Array.from(row.children).some(c => !(c as HTMLElement).hidden);
}

let tab: RadioBand | null = null;   // the user's pick since the panel opened (null = follow the dial)
let lastHz = NaN;
let shown: RadioBand = 'hf';         // the tab on screen

function keyEl(id: string): HTMLElement | null {
  if (id === 'spots') return document.getElementById('spotsBtn');
  return document.querySelector(`#decodersPanel [data-dec="${id}"]`);
}
/** Drawn here at all — the server has it and the owner allows it (the existing rules, read off the DOM). */
function usable(id: string): HTMLElement | null {
  const el = keyEl(id);
  if (!el || el.hidden) return null;
  if (id === 'spots' && document.getElementById('spotsSection')?.hidden) return null;   // 'spots' blocked
  return el;
}

/** Dim = the dial is not near where it is used — and every key of a tab that is not the dial's band (an HF dial looking at
 *  the VHF tab: nothing there applies to where you are). A hint only: the key works the same. */
function paintDim(): void {
  for (const b of RADIO_BANDS) for (const g of DECODER_PLACES[b.id]) for (const id of g.ids) {
    const el = keyEl(id);
    if (!el) continue;
    const far = decoderDim(id, lastHz, shown);
    el.classList.toggle('dimFar', far);
    el.title = el.title.replace(/ — not usually used near this frequency \(still works\)$/, '')
      + (far ? ' — not usually used near this frequency (still works)' : '');
  }
}

/** DIGITAL SPOTS says SINGLE MODE beside BAND in its own section; in a group it is DIGI SPOTS (the app's legend —
 *  and "…: SINGLE MODE" is wider than a 320 px panel). Only the text node changes; the glyph stays.
 *  ★★ DIGI SPOTS / DIGI BAND (Stuart, 2026-10-29: "dont want super big buttons if we can help it"): every key is the widest
 *  legend's size, and DIGITAL SPOTS — then SPOTS: BAND — held a phone to two keys a row. */
export const SPOTS_KEY_LABEL = 'DIGI SPOTS';
export const BAND_KEY_LABEL = 'DIGI BAND';
function spotsLabel(el: HTMLElement, inGroup: boolean): void {
  const t = Array.from(el.childNodes).find(n => n.nodeType === Node.TEXT_NODE && (n.textContent || '').trim());
  if (t) t.textContent = inGroup ? SPOTS_KEY_LABEL : 'DIGITAL SPOTS: SINGLE MODE';
}

function bandLabel(el: HTMLElement, inWhole: boolean): void {
  const t = Array.from(el.childNodes).find(n => n.nodeType === Node.TEXT_NODE && (n.textContent || '').trim());
  if (t) t.textContent = inWhole ? BAND_KEY_LABEL : 'DIGITAL SPOTS: BAND';
}

/**
 * Lay the panel out for `hz` (the DISPLAYED dial). `reset` (the panel opening) drops the user's tab pick, so it opens on
 * the dial's band; otherwise the picked tab is kept.
 */
export function layoutDecoderBands(hz: number, reset = false): void {
  const park = document.getElementById('decPark');
  const groups = document.getElementById('decGroups');
  const tabs = document.getElementById('decBandTabs');
  if (!park || !groups || !tabs) return;
  if (reset) tab = null;
  lastHz = hz;
  const allIds = new Set(RADIO_BANDS.flatMap(b => DECODER_PLACES[b.id].flatMap(g => g.ids)));
  for (const id of allIds) { const el = keyEl(id); if (el && id !== 'spots') park.appendChild(el); }
  // DIGITAL SPOTS lives in its own section's row between layouts (where it always was) — never left in a group being cleared
  const spots = keyEl('spots');
  if (spots) { document.getElementById('spotsPark')?.appendChild(spots); spotsLabel(spots, false); }
  layoutWholeRadio();
  const present = (b: RadioBand) => DECODER_PLACES[b].some(g => g.ids.some(id => !!usable(id)));
  const bands = RADIO_BANDS.map(b => b.id).filter(present);
  const cur: RadioBand = tab && bands.includes(tab) ? tab : openingBand(hz, present);
  shown = cur;

  tabs.textContent = '';
  tabs.hidden = bands.length < 2;
  for (const b of bands) {
    const t = document.createElement('button');
    t.className = 'btn' + (b === cur ? ' on' : '');
    t.textContent = RADIO_BANDS.find(x => x.id === b)!.label;
    t.setAttribute('role', 'tab');
    t.setAttribute('aria-selected', String(b === cur));
    t.onclick = () => { tab = b; layoutDecoderBands(lastHz); };
    tabs.appendChild(t);
  }

  groups.textContent = '';
  for (const { group, ids } of DECODER_PLACES[cur]) {
    const els = ids.map(usable).filter((e): e is HTMLElement => !!e);
    if (!els.length) continue;
    const box = document.createElement('div'); box.className = 'decGroup';
    const head = document.createElement('div'); head.className = 'decGroupHead'; head.setAttribute('role', 'heading');
    const gi = modeIconFor(decoderGroupIcon(group, cur));
    head.innerHTML = gi ? modeIconSvg(gi) : '';
    head.appendChild(document.createTextNode(decoderGroupLabel(group, cur)));
    const row = document.createElement('div'); row.className = 'btnRow decKeys';
    for (const el of els) { row.appendChild(el); if (el === spots) spotsLabel(el, true); }
    box.append(head, row);
    groups.appendChild(box);
  }
  paintDim();
  sizeKeys();
  placeOptions(openDec);
}

// ── ★★★ ONE KEY SIZE (decoderKeyGrid) ───────────────────────────────────────────────────────────────────────────────
let measurer: HTMLButtonElement | null = null;
/** A key's legend width as it would be drawn on its own (glyph removed: it is added back by the grid's rule), and the
 *  glyph's own width and gap — measured in the panel's own font, unscaled (offsetWidth ignores the open animation). */
function legendOf(el: HTMLElement): { w: number; icon: boolean; iconW: number; iconGap: number } {
  const panel = document.getElementById('decodersPanel')!;
  if (!measurer || !measurer.isConnected) {
    measurer = document.createElement('button');
    measurer.className = 'btn dkMeasure';
    measurer.setAttribute('aria-hidden', 'true');
    measurer.tabIndex = -1;
    measurer.style.cssText = 'position:absolute;visibility:hidden;left:-9999px;top:0;width:auto;white-space:nowrap;pointer-events:none';
    panel.appendChild(measurer);
  }
  measurer.innerHTML = el.innerHTML;
  const ico = measurer.querySelector<HTMLElement>('.modeIco');
  let iconW = 0, iconGap = 0;
  if (ico) {
    iconW = ico.getBoundingClientRect().width / Math.max(0.01, measurer.getBoundingClientRect().width / Math.max(1, measurer.offsetWidth));
    iconGap = parseFloat(getComputedStyle(ico).marginRight) || 0;
    ico.remove();
  }
  return { w: measurer.offsetWidth, icon: !!ico, iconW, iconGap };
}
/** Every key the panel can draw — all band tabs (so a tab switch never resizes them), TUNES THE RADIO ITSELF, VIEWS. */
function allKeys(): HTMLElement[] {
  const out: HTMLElement[] = [];
  for (const b of RADIO_BANDS) for (const g of DECODER_PLACES[b.id]) for (const id of g.ids) {
    const el = usable(id);
    if (el && !out.includes(el)) out.push(el);
  }
  for (const id of ['wholeRadio', 'viewsRow']) {
    for (const el of Array.from(document.getElementById(id)?.children ?? []) as HTMLElement[]) {
      if (el.classList.contains('btn') && !el.hidden && !out.includes(el)) out.push(el);
    }
  }
  return out;
}
/** Size every key: the widest legend against the panel's width → --dk-cols, and .dkStack where the glyph goes above. */
export function sizeKeys(): void {
  const panel = document.getElementById('decodersPanel');
  const groups = document.getElementById('decGroups');
  if (!panel || !groups) return;
  const views = document.getElementById('viewsSec');
  if (views) views.hidden = !Array.from(document.getElementById('viewsRow')?.children ?? []).some(c => !(c as HTMLElement).hidden);
  const keys = allKeys();
  if (!keys.length) return;
  const gap = parseFloat(getComputedStyle(groups.querySelector('.decKeys') ?? groups).columnGap) || 6;
  const grid = () => {
    const ls = keys.map(legendOf);
    const withIcon = ls.find(l => l.icon);
    const legends: KeyLegend[] = ls.map(l => ({ w: l.w, icon: l.icon }));
    return decoderKeyGrid(legends, groups.clientWidth, {
      gap, padX: 0, iconW: withIcon?.iconW ?? 0, iconGap: withIcon?.iconGap ?? 0, slack: 2, maxCols: 5,
    });
  };
  // ★ The page's key face is large (0.95em, wide letter-spacing): where it would leave fewer than THREE keys a row (a
  //   phone), the keys take the COMPACT face (.dkSmall) — if that holds more of them, or as many with the glyph beside.
  panel.classList.remove('dkSmall');
  let g = grid();
  if (g.cols < 3) {
    panel.classList.add('dkSmall');
    const small = grid();
    // more keys a row — or as many with the glyph back beside the legend (a shorter key)
    if (small.cols > g.cols || (small.cols === g.cols && g.stack && !small.stack)) g = small; else panel.classList.remove('dkSmall');
  }
  panel.style.setProperty('--dk-cols', String(g.cols));
  panel.classList.toggle('dkStack', g.stack);
  cols = g.cols;
}
let cols = 3;

// ── ★★★ OPTIONS POP OUT OF THEIR KEY ─────────────────────────────────────────────────────────────────────────────────
const OPTION_PANELS: Record<string, string> = {
  rtty: 'rttySettings', wefax: 'wefaxSettings', time: 'timeSettings', cw: 'cwSettings', psk: 'pskSettings',
  aprs: 'aprsSettings', dsc: 'dscSettings', jtty: 'jttySettings',
};
let openDec: string | null = null;
/**
 * Put the selected decoder's settings on the row under its key (main.ts syncDecButtons calls this after it shows or hides
 * them). A key not on screen (another band tab is open) leaves its settings at home, under the groups, where they were.
 */
export function placeOptions(dec: string | null): void {
  openDec = dec;
  const park = document.getElementById('decPark');
  if (!park) return;
  // every panel home first (in their own order, after the park), and no tab drawn
  let at: Element = park;
  for (const pid of Object.values(OPTION_PANELS)) {
    const p = document.getElementById(pid);
    if (!p) continue;
    p.classList.remove('tabL', 'tabR');
    if (p.parentElement?.classList.contains('decKeys') || p.previousElementSibling !== at) at.after(p);
    at = p;
  }
  const pid = dec ? OPTION_PANELS[dec] : undefined;
  const panel = pid ? document.getElementById(pid) : null;
  const key = dec ? document.querySelector<HTMLElement>(`#decGroups [data-dec="${dec}"]`) : null;
  if (!panel || panel.hidden || !key || !key.parentElement) return;
  const row = key.parentElement;
  const keys = (Array.from(row.children) as HTMLElement[]).filter(c => c.classList.contains('btn') && !c.hidden);
  const i = keys.indexOf(key);
  if (i < 0) return;
  const ins = optionsInsertAt(i, cols, keys.length);
  const after = keys[ins - 1];
  after.after(panel);
  const c = tabCorners(i, cols);
  panel.classList.toggle('tabL', c.left);
  panel.classList.toggle('tabR', c.right);
  // the tab: from 4 px round the key's top and sides down to the panel's top edge (both in the grid's own box)
  const l = key.offsetLeft - panel.offsetLeft - 4;
  panel.style.setProperty('--tab-l', `${l}px`);
  panel.style.setProperty('--tab-w', `${key.offsetWidth + 8}px`);
  panel.style.setProperty('--tab-h', `${Math.max(0, panel.offsetTop - key.offsetTop + 4)}px`);
}

/** The window changed size with the panel open: the same keys, re-sized and the options re-placed. */
if (typeof window !== 'undefined') {
  window.addEventListener('resize', () => {
    if (!document.getElementById('decodersPanel')?.classList.contains('open')) return;
    sizeKeys();
    placeOptions(openDec);
  });
}

/** The dial moved with the panel open: re-dim (and re-open on the new band only if the user has not picked a tab). */
export function decoderBandsDial(hz: number): void {
  if (!Number.isFinite(hz) || hz === lastHz) return;
  const was = lastHz;
  lastHz = hz;
  if (tab == null && radioBandOf(was) !== radioBandOf(hz)) layoutDecoderBands(hz);
  else paintDim();
}
