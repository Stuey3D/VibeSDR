/* jtty.ts — V12 JTTY on a VibeServer (docs/v12/JTTY.md), the pure part of the app's JTTY box.
 *
 * ★★ ONE RULE, TWO READERS: the web client's JTTY box (web/client/src/jtty.ts + main.ts renderJtty / jttyTune /
 *    initJttyDials) is the reference. Every rule here is that one — the sanitiser, the dials, the order, the spot merge
 *    (a CQ stays a CQ), where a tap tunes. scripts/test_psk_jtty_dsc_app.ts runs both and holds them equal.
 *
 * Wire (vibe_jtty_run.h, on /ws/dxcluster after `audio_extension_attach` 'jtty'):
 *   {"type":"jtty_signals","data":{"signals":[{id,hz,audio,snr,q,active,last,first,frames,call,country,text}]}}
 *   {"type":"jtty_spot","data":{callsign,frequency,audio,snr,how,country,timestamp}}
 *   and a finished message's line as RTTY's 0x01 text frame (the box draws from the JSON instead), the state as 0x03.
 */
import type { CwQ } from './cwList';

export interface JttySignal {
  id: number; hz: number; audio: number; snr: number; q: CwQ;
  active: boolean; last: number; first: number; frames: number; call: string; country: string; text: string;
}
export interface JttySpot { callsign: string; frequency: number; audio: number; snr: number; how: 'CQ' | 'QSO'; country: string; timestamp: number }

/** ★ The calling frequencies announced for JTTY in the WSJT-X 3.2 release candidates (USB dial) — PRELIMINARY, not
 *  band-plan allocations (docs/v12/JTTY.md §4). 20 m is the main meeting point. The web's JTTY_DIALS. */
export const JTTY_DIALS: ReadonlyArray<{ hz: number; label: string }> = [
  { hz: 1838000, label: '160m' }, { hz: 3575000, label: '80m' }, { hz: 7090000, label: '40m' },
  { hz: 10140000, label: '30m' }, { hz: 14090000, label: '20m' }, { hz: 18100000, label: '17m' },
  { hz: 21090000, label: '15m' }, { hz: 24920000, label: '12m' }, { hz: 28090000, label: '10m' },
  { hz: 50160000, label: '6m' }, { hz: 144160000, label: '2m' },
];

const QS = new Set(['high', 'medium', 'low', 'poor']);
const str = (v: unknown, cap: number) => (typeof v === 'string' ? v.slice(0, cap) : '');
const num = (v: unknown) => (typeof v === 'number' && Number.isFinite(v) ? v : 0);

/** A jtty_signals frame's list, re-checked field by field. null = not a list. */
export function parseJttySignals(d: any): JttySignal[] | null {
  if (!d || !Array.isArray(d.signals)) return null;
  const out: JttySignal[] = [];
  for (const s of d.signals.slice(0, 80)) {
    if (!s || typeof s !== 'object') continue;
    out.push({
      id: num(s.id), hz: num(s.hz), audio: num(s.audio), snr: num(s.snr), q: QS.has(s.q) ? s.q : 'poor',
      active: s.active === true, last: num(s.last), first: num(s.first), frames: num(s.frames),
      call: str(s.call, 16), country: str(s.country, 40), text: str(s.text, 600),
    });
  }
  return out;
}
/** A jtty_spot; null without a callsign. `now` stands in for a missing timestamp. */
export function parseJttySpot(d: any, now: number): JttySpot | null {
  if (!d || typeof d.callsign !== 'string' || !d.callsign) return null;
  return { callsign: str(d.callsign, 16), frequency: num(d.frequency), audio: num(d.audio), snr: num(d.snr),
           how: d.how === 'CQ' ? 'CQ' : 'QSO', country: str(d.country, 40), timestamp: num(d.timestamp) || now };
}

/** ★ The spots heard, newest first, one per call — a call heard again moves to the top with its latest frequency; a call
 *  once heard calling CQ stays a CQ spot (an exchange heard after does not demote it). At most 200. A new array. */
export function mergeJttySpot(spots: readonly JttySpot[], s: JttySpot, cap = 200): JttySpot[] {
  const old = spots.find((o) => o.callsign === s.callsign);
  const sp: JttySpot = old && old.how === 'CQ' && s.how !== 'CQ' ? { ...s, how: 'CQ' } : s;
  const out = [sp, ...spots.filter((o) => o.callsign !== s.callsign)];
  if (out.length > cap) out.length = cap;
  return out;
}
/** The spots drawn (the web draws 80). */
export const JTTY_SPOTS_SHOWN = 80;

/** Rows lowest frequency first — RF when the server knows it, else the audio tone; ties by id. A new array. */
export function sortJtty(sigs: readonly JttySignal[]): JttySignal[] {
  return [...sigs].sort((a, b) => (a.hz > 0 && b.hz > 0 ? a.hz - b.hz : a.audio - b.audio) || a.id - b.id);
}

/** Where to tune for a station: USB, the dial 1500 Hz below its tone 0 (its four tones then sit at 1500–1594 Hz). */
export function jttyTuneFor(rfHz: number): number { return Math.round(rfHz - 1500); }

/** The idle lines — the web's, word for word. */
export const JTTY_IDLE_ROWS = 'listening — every JTTY signal in your passband appears here as it is decoded (tune USB; 14.090 MHz is the main meeting point)';
export const JTTY_IDLE_SPOTS = 'every callsign heard — calling CQ or in an exchange — is spotted here';

/** The hint under the JTTY key's settings (the web's #jttyHint, its bold dropped). */
export const JTTY_HINT = 'Tune USB — 14.090 MHz is the main meeting point — or press a band. Every JTTY signal in your '
  + 'passband is decoded at once, whenever it starts: one row per station, lowest frequency first, coloured by confidence '
  + '(green high · amber medium · orange low · red poor). Calls heard are SPOTS. Tap a row or a spot to tune to it.';

/** The status line for the decoder's 0x03 state (the web's onState). */
export function jttyStateText(st: number): string { return st ? 'decoding…' : 'listening…'; }

/** ★ The dials this radio can reach — the rest are not drawn (AGENTS.md: no dead controls; the web's initJttyDials). */
export function jttyDialsReachable(reach: (hz: number) => boolean): Array<{ hz: number; label: string }> {
  return JTTY_DIALS.filter((d) => reach(d.hz));
}
