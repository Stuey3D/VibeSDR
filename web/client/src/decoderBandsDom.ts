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
 */
import { DECODER_PLACES, RADIO_BANDS, decoderGroupIcon, decoderGroupLabel, decoderDim, openingBand,
         radioBandOf, type RadioBand } from '../../../src/utils/decoderBands';
import { modeIconFor, modeIconSvg } from '../../../src/utils/modeIcons';

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

/** DIGITAL SPOTS says SINGLE MODE beside BAND in its own section; in a group it is just DIGITAL SPOTS (the app's legend —
 *  and "…: SINGLE MODE" is wider than a 320 px panel). Only the text node changes; the glyph stays. */
function spotsLabel(el: HTMLElement, inGroup: boolean): void {
  const t = Array.from(el.childNodes).find(n => n.nodeType === Node.TEXT_NODE && (n.textContent || '').trim());
  if (t) t.textContent = inGroup ? 'DIGITAL SPOTS' : 'DIGITAL SPOTS: SINGLE MODE';
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
  if (spots) { document.getElementById('spotsBandBtn')?.before(spots); spotsLabel(spots, false); }
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
    const row = document.createElement('div'); row.className = 'btnRow';
    for (const el of els) { row.appendChild(el); if (el === spots) spotsLabel(el, true); }
    box.append(head, row);
    groups.appendChild(box);
  }
  paintDim();
}

/** The dial moved with the panel open: re-dim (and re-open on the new band only if the user has not picked a tab). */
export function decoderBandsDial(hz: number): void {
  if (!Number.isFinite(hz) || hz === lastHz) return;
  const was = lastHz;
  lastHz = hz;
  if (tab == null && radioBandOf(was) !== radioBandOf(hz)) layoutDecoderBands(hz);
  else paintDim();
}
