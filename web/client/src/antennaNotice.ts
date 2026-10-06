/**
 * ★★★ THE AERIAL'S RANGES AND FILTERS, TOLD TO A LISTENER IN THE BROWSER (Stuart, 2026-10-06) — the
 * web half of the app's AntennaBandNotice. Same parser, same sentence, same once-per-entry rule:
 * src/utils/antennaBands.ts decides; this only draws.
 *
 * ★★ TWO VOICES, THE ONES THIS CLIENT ALREADY HAS. On ENTERING a filtered band (or leaving the
 *    aerial's range) the full sentence goes up as the standard notice pill — "FM band-stop filter
 *    fitted — reception here is deliberately reduced". While you stay inside, a still amber chip
 *    sits on the status line beside OVERLOAD (#antChip), in the controls' own font: it is a STATE
 *    the owner chose, not a fault, so it is drawn like DIRECT SAMPLING and does not breathe.
 *    Clicking the chip says the sentence again. Leaving clears it, so coming back is a new entry —
 *    someone flicking about the FM band is told once, not on every step.
 * ★★ IT WATCHES THE DIAL ITSELF. The dial moves by a dozen routes (waterfall click, wheel, keys,
 *    bookmarks, a shared-dial neighbour) and not all of them pass one function; a 400 ms look at
 *    one number catches every one. It runs ONLY while the owner has described bands — a server
 *    that never filled them in costs nothing at all.
 * ★ Kept out of main.ts (several people edit that file): main.ts hands it the server's text, the
 *   socket in use, a way to read the dial and its notice pill — nothing else.
 */
import {
  antennaNoticeAt, antennaNoticeTrack, hasAntennaBands, parseAntennaBands, type AntennaBands,
} from '../../../src/utils/antennaBands';

let bands: AntennaBands | null = null;
let port: string | null = null;
let lastKey: string | null = null;
let lastHz = 0;
let rawText: string | null = null;
let readHz: () => number = () => 0;
let announce: (text: string) => void = () => {};
let timer = 0;

/** From /vibeserver.json — the owner's text, per radio. Empty or absent = nothing, ever. */
export function setAntennaBands(ranges: unknown, filters: unknown,
                                dial: () => number, say: (text: string) => void) {
  readHz = dial;
  announce = say;
  // ★ The same text again (a re-read of /vibeserver.json) is not a new entry — never re-announce.
  const raw = `${typeof ranges === 'string' ? ranges : ''}\u0000${typeof filters === 'string' ? filters : ''}`;
  if (raw === rawText) return;
  rawText = raw;
  const b = parseAntennaBands(ranges, filters);
  bands = hasAntennaBands(b) ? b : null;
  lastKey = null;
  clearInterval(timer); timer = 0;
  if (bands) timer = window.setInterval(tick, 400);
  lastHz = readHz();
  render();
}

/** The socket in use (caps.antenna). Moving socket can move you into or out of a filtered band. */
export function setAntennaPort(p: string | null | undefined) {
  const next = typeof p === 'string' && p ? p : null;
  if (next === port) return;
  port = next;
  render();
}

function tick() {
  const hz = readHz();
  if (hz === lastHz) return;
  lastHz = hz;
  render();
}

function render() {
  const n = bands && lastHz > 0 ? antennaNoticeAt(lastHz, bands, port) : null;
  const t = antennaNoticeTrack(lastKey, n);
  lastKey = t.key;
  const chip = document.getElementById('antChip');
  if (chip) {
    chip.classList.toggle('set', !!n);
    chip.textContent = n ? n.short.toUpperCase() : '';
    chip.title = n ? `${n.text} — click to read it again` : '';
    chip.setAttribute('aria-label', n ? n.text : '');
    chip.onclick = n ? () => announce(n.text) : null;
    chip.onkeydown = n ? (e: KeyboardEvent) => { if (e.key === 'Enter' || e.key === ' ') announce(n.text); } : null;
  }
  if (n && t.entered) announce(n.text);
}
