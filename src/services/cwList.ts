/* cwList.ts — V12 CW on a VibeServer (docs/v12/CW-SKIMMER.md), the pure part of the app's CW box.
 *
 * ★★ ONE RULE, TWO READERS: the web client's CW panel (web/client/src/decoders.ts + main.ts renderCw / cwTune /
 *    cwPinWatch) is the reference, and every rule here is that one — the wire's sanitiser, the order of the list, the
 *    confidence colours, the spots kept, the age and frequency text, the scroll pin. scripts/test_cw_list.ts tests them.
 *
 * Wire (vibe_cw_run.h, on /ws/dxcluster after `audio_extension_attach` 'cw' or 'cwskim' {width}):
 *   {"type":"cw_signals","data":{skimmer,centre?,width?,primary,signals:[{id,hz,audio,wpm,snr,q,active,last,first,call,country,text}]}}
 *   {"type":"cw_spot","data":{callsign,frequency,audio,wpm,snr,how,country,timestamp}}
 *   and, ONE SIGNAL only, the primary signal's text as RTTY's 0x01 frame, the state as 0x03.
 */

import { cwWpmRangeOf, cwWpmText } from '../utils/cwFormat.ts';

export type CwQ = 'high' | 'medium' | 'low' | 'poor';

/** One signal the server's CW engine is decoding (or heard recently). `hz` is RF (0 for ONE SIGNAL's audio-only rows);
 *  `audio` the tone. `last` / `first` are epoch ms. */
export interface CwSignal {
  id: number; hz: number; audio: number; wpm: number; snr: number; q: CwQ; active: boolean;
  last: number; first: number; call: string; country: string; text: string;
  /** ★ V12 (2026-10-29): the speed as a range — the slowest and fastest the signal was read at (absent from an older
   *  server; utils/cwFormat cwWpmText draws "17–21 wpm"). */
  wpmLo?: number; wpmHi?: number;
  /** ★ V12 PSK only: 31 or 63, and the IMD (dB, 0 = not measured). One list draws both decoders (the web's rule). */
  baud?: 31 | 63; imd?: number;
}
export interface CwList { skimmer: boolean; centre: number; width: number; primary: number; signals: CwSignal[] }
export interface CwSpot {
  callsign: string; frequency: number; audio: number; wpm: number; snr: number; how: string; country: string; timestamp: number;
  /** ★ V12 (2026-10-29): the speed as a range, as the signal rows (absent from an older server). */
  wpmLo?: number; wpmHi?: number;
  /** ★ V12 PSK only: 'BPSK31' | 'BPSK63' (shown as PSK31 / PSK63 where CW shows its speed). */
  mode?: string;
}

/** ★ The skimmer's segments — the server takes 12–192 kHz; these are the three the web offers (and background decoding). */
export const CW_WIDTHS = [24000, 48000, 96000] as const;
export type CwWidth = typeof CW_WIDTHS[number];
export const CW_DEFAULT_WIDTH: CwWidth = 48000;
export function cwWidthOf(v: unknown): CwWidth {
  const n = Number(v);
  return (CW_WIDTHS as readonly number[]).includes(n) ? (n as CwWidth) : CW_DEFAULT_WIDTH;
}

/** ★★ THE HEALTH PILL'S COLOURS (Stuart, 2026-10-25): red poor, orange low, amber medium, green high — the message text is
 *  drawn in its signal's confidence, so the eye finds the clean copy without reading the marker. */
export const CW_Q_COLOURS: Record<CwQ, string> = {
  poor: '#FF4B4B', low: '#FF8A3D', medium: '#E8C547', high: '#5BE36B',
};

/** The list from the wire, sanitised as the web's DecoderClient does: plain fields of the types read, at most 80 rows,
 *  strings cut short. null = not a list. ★ `psk`: a psk_signals frame — the same list with the mode (31/63) and IMD. */
export function parseCwSignals(d: any, psk = false): CwList | null {
  if (!d || typeof d !== 'object' || !Array.isArray(d.signals)) return null;
  const str = (v: unknown, n: number) => (typeof v === 'string' ? v.slice(0, n) : '');
  const num = (v: unknown) => (typeof v === 'number' && Number.isFinite(v) ? v : 0);
  const q = (v: unknown): CwQ => (v === 'high' || v === 'medium' || v === 'low' ? v : 'poor');
  return {
    skimmer: d.skimmer === true, centre: num(d.centre), width: num(d.width), primary: num(d.primary),
    signals: d.signals.slice(0, 80).map((x: any) => ({
      id: num(x?.id), hz: num(x?.hz), audio: num(x?.audio), wpm: num(x?.wpm), snr: num(x?.snr), q: q(x?.q),
      active: x?.active === true, last: num(x?.last), first: num(x?.first),
      call: str(x?.call, 16), country: str(x?.country, 40), text: str(x?.text, 160),
      ...(psk ? {} : cwWpmRangeOf(x)),
      ...(psk ? { baud: (num(x?.baud) > 40 ? 63 : 31) as 31 | 63, imd: num(x?.imd) } : {}),
    })),
  };
}

/** A spot from the wire; null without a callsign. `now` stands in for a missing timestamp. ★ `psk`: a psk_spot. */
export function parseCwSpot(d: any, now: number, psk = false): CwSpot | null {
  if (!d || typeof d.callsign !== 'string' || !d.callsign) return null;
  return {
    callsign: d.callsign.slice(0, 16), frequency: Number(d.frequency) || 0, audio: Number(d.audio) || 0,
    wpm: Number(d.wpm) || 0, snr: Number(d.snr) || 0, how: String(d.how || '').slice(0, 8),
    country: String(d.country || '').slice(0, 40), timestamp: Number(d.timestamp) || now,
    ...(psk ? {} : cwWpmRangeOf(d)),
    ...(psk ? { mode: d.mode === 'BPSK63' ? 'BPSK63' : 'BPSK31' } : {}),
  };
}

/* ★★ V12 PSK31/63 (docs/v12/PSK.md) — CW's two views exactly, on the same list: ONE SIGNAL ('psk', the listener's audio
 *  — the signal nearest `centre`, the passband's middle, is the box's) and SKIMMER ('pskskim', a 6/12/24 kHz segment of IQ
 *  centred on the dial). The web's pskWidth / passbandCentreHz / cwTune / pskHint, as pure rules (test_psk_jtty_dsc_app.ts). */
export const PSK_WIDTHS = [6000, 12000, 24000] as const;
export type PskWidth = typeof PSK_WIDTHS[number];
export const PSK_DEFAULT_WIDTH: PskWidth = 12000;
export function pskWidthOf(v: unknown): PskWidth {
  const n = Number(v);
  return (PSK_WIDTHS as readonly number[]).includes(n) ? (n as PskWidth) : PSK_DEFAULT_WIDTH;
}
/** The middle of the listener's passband, audio Hz — ONE SIGNAL's cursor (USB 200–2800 → 1500); 1500 when it is not an
 *  audio passband (CW, AM). */
export function passbandCentreHz(low: number, high: number): number {
  const c = Math.abs((low + high) / 2);
  return c >= 300 && c <= 3500 ? Math.round(c) : 1500;
}
/** ★ Tap a PSK row: USB with the signal in the middle of the passband (LSB if the listener is in LSB), so it is the one
 *  ONE SIGNAL decodes. `mode` the listener's. */
export function pskTuneFor(rfHz: number, mode: string, low: number, high: number): { hz: number; mode: 'usb' | 'lsb' } {
  const m = (mode || '').toLowerCase();
  const lsb = m === 'lsb';
  const c = lsb || m === 'usb' ? passbandCentreHz(low, high) : 1500;
  return { hz: Math.round(lsb ? rfHz + c : rfHz - c), mode: lsb ? 'lsb' : 'usb' };
}
/** The hint under the PSK key's settings — the web's pskHint, word for word (its bold dropped). */
export function pskHintText(skimmer: boolean, width: number): string {
  return skimmer
    ? `Every PSK31 and PSK63 signal ${width / 2000} kHz either side of your dial, decoded at once — tune the dial to `
      + 'the middle of the PSK segment (e.g. 14.0715). A call heard calling twice is spotted. Tap a row to tune to it.'
    : 'Tune USB with the PSK segment in your passband (e.g. 14.070): every PSK31/63 signal in it is listed, '
      + 'and the one in the middle of the passband is decoded above. Tap a row to put it there.';
}
/** The list's idle line — the web's renderCw, both decoders. */
export function kbIdleText(psk: boolean, skimmer: boolean): string {
  const what = psk ? 'PSK' : 'CW';
  return skimmer ? `listening across the segment — each ${what} signal appears here as it is decoded`
       : psk ? 'listening — every PSK signal in your passband appears here; the one in the middle is decoded above'
             : 'listening — tune a CW signal into the passband';
}
/** A row's speed column: "PSK31" for PSK, "18 wpm" for CW. A spot's: its mode (BPSK→PSK) or its speed. */
export function kbRateText(sg: { baud?: number; wpm: number; wpmLo?: number; wpmHi?: number }): string {
  return sg.baud !== undefined ? `PSK${sg.baud}` : cwWpmText(sg);
}
export function kbSpotRateText(s: { mode?: string; wpm: number; wpmLo?: number; wpmHi?: number }): string {
  return s.mode ? s.mode.replace('BPSK', 'PSK') : cwWpmText(s);
}

/** ★★ LOWEST FREQUENCY FIRST, ALWAYS (Stuart, 2026-10-25: "the CW Skimmer list likes to reorder itself a lot so I think we
 *  list it from frequency lowest to highest regardless of confidence"). A row keeps its place while its signal lives; the
 *  order is the band's, as on the spectrum above. A signal with no RF frequency (ONE SIGNAL's audio) sorts by its tone;
 *  ties by id, so two rows never swap. A new array — the list passed in is left alone. */
export function sortCwSignals(sigs: readonly CwSignal[]): CwSignal[] {
  return [...sigs].sort((a, b) => (a.hz > 0 && b.hz > 0 ? a.hz - b.hz : a.audio - b.audio) || a.id - b.id);
}

/** ★ A spot heard again moves to the top (one row per call), newest first, at most `cap` kept (the web keeps 200 and
 *  draws 60). A new array. */
export function mergeCwSpot(spots: readonly CwSpot[], sp: CwSpot, cap = 200): CwSpot[] {
  const out = [sp, ...spots.filter((o) => o.callsign !== sp.callsign)];
  if (out.length > cap) out.length = cap;
  return out;
}

/** "12s" · "4m" · "2h" since `ms`. */
export function cwAge(ms: number, now: number): string {
  const s = Math.max(0, Math.round((now - ms) / 1000));
  return s < 60 ? `${s}s` : s < 3600 ? `${Math.round(s / 60)}m` : `${Math.round(s / 3600)}h`;
}

/** A row's frequency: kHz to two places on RF, else its audio tone ("714 Hz"). */
export function cwFreqText(hz: number, audio: number): string {
  return hz > 0 ? (hz / 1e3).toFixed(2) : `${Math.round(audio)} Hz`;
}

/** "+12 dB" / "-3 dB". */
export function cwSnrText(snr: number): string {
  return `${snr >= 0 ? '+' : ''}${Math.round(snr)} dB`;
}

/* ★★ THE VIEW STAYS WHERE THE LISTENER PUT IT (Stuart, 2026-10-25: "trying to look at the spots at the bottom … the new
 *  text above kept pulling the view up again"). The rows are rebuilt about once a second and new signals add rows ABOVE
 *  the spots, so a plain scroll offset jumps. Scrolled down at all = pinned to the SPOTS heading: it keeps its place on
 *  screen whatever happens above it. At the top = stays at the top (new rows arrive where the eye already is).
 *  The web's cwPinWatch / renderCw, as two pure steps: */

/** On the LISTENER's scroll: where the SPOTS heading sits in the view (its content y less the offset), or null at the top. */
export function cwPinFromScroll(scrollY: number, headY: number): number | null {
  return scrollY > 0 ? headY - scrollY : null;
}
/** After the content changed: the offset that puts the heading back where the listener left it, or null to leave the view
 *  alone (at the top, or already there to the pixel). */
export function cwPinRestore(pinAt: number | null, headY: number, scrollY: number): number | null {
  if (pinAt === null) return null;
  const want = Math.max(0, headY - pinAt);
  return Math.abs(want - scrollY) >= 1 ? want : null;
}

/** The hint under the CW key's settings — the web's cwHint, word for word. */
export function cwHintText(skimmer: boolean, width: number): string {
  return skimmer
    ? `Every CW signal ${width / 2000} kHz either side of your dial, decoded at once — the speed found for each. `
      + 'A call heard calling twice is spotted. Tap a row to tune to it.'
    : 'Tune the signal in CW — the decoder finds the speed itself (5–60 WPM). A wider USB filter lists every '
      + 'CW signal inside it. Tap a row to tune to it.';
}

/** ★★ THE CW SKIMMER'S VFO IS ITS SEGMENT (Stuart, 2026-10-25: "the VFO doesnt actually show me where i am tuning … the VFO
 *  should grow to 96KHz and be tunable"). The passband DRAWN on the waterfall: the skimmer's segment while it runs, else the
 *  listener's own. Drawn only, never sent — the CW passband is untouched and comes back when the skimmer closes. */
export function cwDrawnPassband(skimming: boolean, width: number, low: number, high: number): { low: number; high: number } {
  return skimming && width > 0 ? { low: -width / 2, high: width / 2 } : { low, high };
}
