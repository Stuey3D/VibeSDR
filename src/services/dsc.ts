/* dsc.ts — V12 DSC on a VibeServer (docs/v12/DSC.md), the pure part of the app's DSC box.
 *
 * ★★ ONE RULE, TWO READERS: the web client's DSC box (web/client/src/dsc.ts + main.ts renderDsc / dscTune /
 *    initDscChannels / decParams) is the reference. Every rule here is that one — the channels, where a channel key
 *    tunes, the attach parameters worked out from the nearest DSC frequency, the sanitiser, one row per transmission
 *    (repeats ×n), the colours. scripts/test_psk_jtty_dsc_app.ts runs both and holds them equal.
 *
 * Wire (vibe_dsc_run.h, on /ws/dxcluster after `audio_extension_attach` 'dsc' {band, center_frequency}):
 *   {"type":"dsc_call","data":{id, utc, rf, repeat, format, category, level, self, selfCountry, to?, distressId?,
 *     nature?, lat?, lon?, posText?, time?, tc1?, tc2?, freqs?, number?, eos, check, quality, band, line}}
 *
 * ★★ READABLE, NEVER A SYMBOL DUMP: each call is the server's own line, coloured by what it is — distress red, urgency
 *    orange, safety amber, routine plain — newest first.
 */
import { esc, rangeBearing, compass8, ago, type LatLon, type TrackItem, type TrackRow } from './tracks';
import { DSC_DISTRESS, dscTuneFor as sharedTuneFor, dscCentreFor, dscReachable, dscBandOf, dscBandChannels,
         type DscBand, type DscChannel } from '../utils/dscChannels';
export { DSC_DISTRESS, DSC_ROUTINE, DSC_ALL, dscBandOf, dscBandLabel, dscBandChannels, dscChannelOn, dscChannelsForDial,
         dscTuneOffer, dscKeyTitle, dscDialText, type DscBand, type DscChannel } from '../utils/dscChannels';

export interface DscCall {
  id: number; utc: number; rf: number; repeat: number;
  format: string; category: string; level: number;
  self: string; selfCountry: string; to: string; toCountry: string;
  distressId: string; nature: string; lat: number | null; lon: number | null; posText: string; time: string;
  tc1: string; tc2: string; freqs: string[]; number: string; eos: string; check: 'ok' | 'corrected' | 'unconfirmed';
  quality: number; band: 'hf' | 'vhf'; line: string;
}

/** The DSC distress and safety frequencies (ITU RR Appendix 15) and channel 70 — the web's DSC_CHANNELS. The routine
 *  channels (ITU-R M.541) and every rule about where to tune live in utils/dscChannels (shared with the web). */
export const DSC_CHANNELS: ReadonlyArray<{ hz: number; label: string; vhf: boolean }> =
  DSC_DISTRESS.map(({ hz, label, vhf }) => ({ hz, label, vhf }));

/** Where to tune for a DSC channel: USB 1.7 kHz below, or channel 70 in NFM. */
export function dscTuneFor(ch: { hz: number; vhf: boolean }): { hz: number; mode: 'usb' | 'nfm' } {
  return sharedTuneFor(ch);
}

/** The attach parameters for the listener's dial and mode. ★ The audio centre is worked out from the NEAREST DSC
 *  channel — distress or routine (dscCentreFor) — so a dial a little off the usual 1.7 kHz (2186.0 USB) still decodes;
 *  anywhere else, 1700 Hz. */
export function dscParams(dialHz: number, mode: string): { band: 'vhf' } | { band: 'hf'; center_frequency: number } {
  const m = (mode || '').toLowerCase();
  if (m === 'nfm' || m === 'fm' || Math.abs(dialHz - 156525000) < 25000) return { band: 'vhf' };
  return { band: 'hf', center_frequency: dscCentreFor(dialHz, m) };
}

/** A dsc_call's data, re-checked field by field. null = not a call. */
export function parseDscCall(d: any, now: number): DscCall | null {
  if (!d || typeof d !== 'object' || typeof d.line !== 'string') return null;
  const s = (v: unknown, n: number) => (typeof v === 'string' ? v.slice(0, n) : '');
  const n = (v: unknown) => (typeof v === 'number' && Number.isFinite(v) ? v : 0);
  const nn = (v: unknown) => (typeof v === 'number' && Number.isFinite(v) ? v : null);
  const lat = nn(d.lat), lon = nn(d.lon);
  return {
    id: n(d.id), utc: n(d.utc) || now, rf: n(d.rf), repeat: Math.max(1, n(d.repeat)),
    format: s(d.format, 40), category: s(d.category, 20), level: Math.max(0, Math.min(3, n(d.level))),
    self: s(d.self, 9), selfCountry: s(d.selfCountry, 60), to: s(d.to, 9), toCountry: s(d.toCountry, 60),
    distressId: s(d.distressId, 9), nature: s(d.nature, 60),
    lat: lat !== null && Math.abs(lat) <= 90 ? lat : null, lon: lon !== null && Math.abs(lon) <= 180 ? lon : null,
    posText: s(d.posText, 60), time: s(d.time, 8), tc1: s(d.tc1, 60), tc2: s(d.tc2, 80),
    freqs: Array.isArray(d.freqs) ? d.freqs.slice(0, 4).map((x: unknown) => s(x, 24)) : [], number: s(d.number, 20),
    eos: s(d.eos, 20), check: d.check === 'corrected' ? 'corrected' : d.check === 'unconfirmed' ? 'unconfirmed' : 'ok',
    quality: n(d.quality), band: d.band === 'vhf' ? 'vhf' : 'hf', line: s(d.line, 600),
  };
}

/** ★ The calls heard while the box is open, newest first; a repeat (same id and frequency) updates its row and moves it
 *  to the top. At most 300. A new array. */
export function mergeDscCall(calls: readonly DscCall[], c: DscCall, cap = 300): DscCall[] {
  const out = [c, ...calls.filter((o) => !(o.id === c.id && o.rf === c.rf))];
  if (out.length > cap) out.length = cap;
  return out;
}

export const DSC_LEVEL_NAMES = ['routine', 'safety', 'urgency', 'distress'] as const;
/** ★ The colours, the web's #dscList rules (scripts/test_psk_jtty_dsc_app.ts reads them out of index.html): the row's
 *  left rule — safety amber, urgency orange, distress red; the line's text — urgency and distress lifted (distress bold),
 *  routine and safety plain (null = the theme's text); the row's wash — urgency and distress. */
export const DSC_LEVEL_BORDER: ReadonlyArray<string | null> = [null, '#E8C547', '#FF8A3D', '#FF4B4B'];
export const DSC_LEVEL_TEXT: ReadonlyArray<string | null> = [null, null, '#FF9D5C', '#FF6B6B'];
export const DSC_LEVEL_BG: ReadonlyArray<string | null> = [null, null, 'rgba(255,138,61,0.08)', 'rgba(255,75,75,0.12)'];

/** "8414.5 kHz" / "156.525 MHz" / "" — the web's dscFreqText. */
export function dscFreqText(hz: number): string {
  if (!(hz > 0)) return '';
  return hz >= 30e6 ? `${(hz / 1e6).toFixed(3)} MHz` : `${(hz / 1e3).toFixed(1)} kHz`;
}
/** "14:32:07z" — the web's row time. */
export function dscTimeText(ms: number): string {
  const d = new Date(ms);
  const p = (x: number) => String(x).padStart(2, '0');
  return `${p(d.getUTCHours())}:${p(d.getUTCMinutes())}:${p(d.getUTCSeconds())}z`;
}

/** The idle line and the hint — the web's, word for word (the hint's bold dropped). */
export const DSC_IDLE = 'listening — each DSC call appears here as it is decoded. A few an hour on each frequency; most are '
  + 'test calls to coast stations and their acknowledgements.';
export const DSC_HINT = 'A listed DSC frequency is the ASSIGNED frequency, not the dial: tune USB with the dial 1.7 kHz '
  + 'below it (4206.8 for 4208.5 kHz) — or press a channel. Channel 70 (156.525 MHz) is NFM. DISTRESS holds the distress '
  + 'and safety channels; the band you are on lists its routine ones too. 8414.5 kHz is the busiest by day, 2187.5 kHz at '
  + 'night. Each call is shown as it is decoded; a position sent gets MAP.';

/** The status line for the decoder's 0x03 state (the web's onState). */
export function dscStateText(st: number): string { return st ? 'receiving a call…' : 'listening…'; }

/** ★ The distress channels this radio can reach (where each would be TUNED) — the rest are not drawn (the web's
 *  initDscChannels). */
export function dscChannelsReachable(reach: (hz: number) => boolean): Array<{ hz: number; label: string; vhf: boolean }> {
  return DSC_CHANNELS.filter((ch) => reach(dscTuneFor(ch).hz));
}

/** ★ The Mode panel's DSC options: DISTRESS (every band this radio reaches) and the ROUTINE channels of the band the dial
 *  is on (none off every DSC band) — the web's initDscChannels. */
export function dscPanelChannels(dialHz: number, reach: (hz: number) => boolean): { distress: DscChannel[]; band: DscBand | null; routine: DscChannel[] } {
  const band = dscBandOf(dialHz);
  return {
    distress: dscReachable(DSC_DISTRESS, reach),
    band,
    routine: band ? dscReachable(dscBandChannels(band).filter((c) => c.kind === 'routine'), reach) : [],
  };
}

/** A call with a position, as a map pin: the call's line, its level's colour (the web's PIN_COLOUR). */
export const DSC_PIN_COLOURS = ['#9fb3c8', '#E8C547', '#FF8A3D', '#FF4B4B'];

/* ★★ THE MAP (the box's MAP — the web's openDscMap / dscMapHtml): every call that sent a position, pinned on this server's
 *  own map with this receiver's house beside it, a pin's colour its level, its card the call's own line. In the app it is
 *  the shared track layer (MapOverlay kind 'dsc', TrackLiveList for the rows), so the pins are TrackItems. */

/** One pin per transmission (id + frequency), newest first, only calls with a position. */
export const dscKey = (c: DscCall) => `${c.id}:${c.rf}`;
export function dscItems(calls: readonly DscCall[]): TrackItem[] {
  return calls.filter((c) => c.lat !== null && c.lon !== null).slice(0, 200).map((c) => {
    const col = DSC_PIN_COLOURS[Math.max(0, Math.min(3, c.level))];
    return {
      key: dscKey(c), lat: c.lat as number, lon: c.lon as number,
      _ic: { w: 20, h: 20, html: `<svg width="20" height="20" viewBox="0 0 20 20"><circle cx="10" cy="10" r="8" fill="${col}" stroke="#000000aa" stroke-width="2"/></svg>` },
      _lb: esc(c.self ? `MMSI ${c.self}` : c.distressId ? `MMSI ${c.distressId}` : DSC_LEVEL_NAMES[c.level] ?? ''),
      _cd: `<div class="trkCard"><div style="white-space:normal;max-width:300px">${esc(c.line)}</div></div>`,
    };
  });
}
/** The live list's rows: the calls with a position, newest first — level, who, the line, range and bearing from here. */
export function dscListRows(calls: readonly DscCall[], rx: LatLon | null, now: number): TrackRow[] {
  return calls.filter((c) => c.lat !== null && c.lon !== null).slice(0, 200).map((c) => {
    const rb = rx ? rangeBearing(rx, c.lat as number, c.lon as number) : null;
    return {
      key: dscKey(c), colour: DSC_PIN_COLOURS[Math.max(0, Math.min(3, c.level))], flag: '',
      name: c.self ? `MMSI ${c.self}` : c.format || 'DSC call',
      second: [DSC_LEVEL_NAMES[c.level]?.toUpperCase(), c.nature, c.posText].filter(Boolean).join(' · '),
      sp: '', trend: 0, range: rb ? `${rb.nm < 10 ? rb.nm.toFixed(1) : Math.round(rb.nm)} NM` : '', brg: rb ? compass8(rb.brg) : '',
      noPos: false, age: ago((now - c.utc) / 1000), em: c.level >= 3,
    };
  });
}
export const DSC_MAP_EMPTY = 'No DSC call heard yet has sent a position.';
