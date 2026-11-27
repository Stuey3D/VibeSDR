// xpdr.ts — ★★★ beta94 THE TRANSPONDER SIGNAL FINDER, the clients' half (both: the app and the web page —
// docs/v12/TRANSPONDER-FINDER.md). The server's "xpdr" decoder (vibe_xpdr_run.h) sends {"type":"xpdr_status","data":…}
// once a second; this parses it DEFENSIVELY (anything off the wire is bounded and type-checked), says what each row is in
// words, lays out the waterfall overlay (ORBCOMM's: the slice shaded, an arrow per signal — src/utils/orbcommView.ts) and
// says what LISTEN does: tune VFO A to the row's dial in USB (its sideband) and ask the finder to follow it.
//
// Stuart, 2026-11-24 (RS-44): "present it the same way as orbcomm a large wide vfo covering the whole receive range then
// the vfo a that snaps to the chosen voice signal, CW simply decodes" · "the little arrows you added for orbcom to show the
// signals being detected" · "if a user sees a signal that we havent detected they can tune to it manually".
// ★★★ beta94 TRANSPONDER + TELEMETRY (Stuart 11-25, JO-97 / AO-73: "handled as one" · "allow the audio from the telemetry to
// also be listenable too"): when the span holds the bird's FUNcube telemetry ("tlm"), the finder decodes it too — the
// SATELLITE box shows the telemetry line and CHARTS beside these rows (fctlm_status / sat_tlm reach the box while "xpdr" is
// attached) — and the BPSK is ONE row, kind "tlm", whose LISTEN puts VFO A 1.5 kHz below its carrier in USB.
// ★ Pure — scripts/test_xpdr_app.ts.
import type { OrbOverlay } from '../utils/orbcommView';

export type XpdrKind = 'cw' | 'voice' | 'tlm';
export interface XpdrRow {
  id: string;
  kind: XpdrKind;
  /** RF now (Hz) — where the arrow goes */
  hz: number;
  /** satellite frame (Hz) — where LISTEN puts the dial (a voice's suppressed carrier, a CW tone) */
  dial: number;
  snr: number;
  /** seconds since last heard */
  ago: number;
  on: boolean;
  beacon: boolean;
  call: string;
  text: string;
  wpm: number;
  /** ★★★ beta94 a TELEMETRY row: its carrier (satellite frame — `dial` is 1.5 kHz below it) and the frames decoded */
  car: number;
  frames: number;
}
export interface XpdrFollow { id: string; src: 'wait' | 'guess' | 'voice' | 'cw' | 'held' | 'tlm' | ''; off: number }
export interface XpdrStatus {
  slice: { lo: number; hi: number } | null;
  span: { lo: number; hi: number } | null;
  beacon: number;
  /** ★★★ beta94 the listed FUNcube telemetry the finder also decodes (satellite frame, Hz), 0 = none */
  tlm: number;
  dop: number;
  rows: XpdrRow[];
  follow: XpdrFollow | null;
  /** ★★★ beta97 the digital-mode skimmer (FT8 / FT4 across the whole passband — decoders/sat_skim.h), null = none */
  skim: XpdrSkim | null;
}

/* ★★★ beta97 THE SATELLITE DIGITAL-MODE SKIMMER, the clients' half (docs/v12/SAT-SKIM.md). The finder's status carries
 *  "skim":{on, mode "live"|"catchup", windows, workers, depth, queued, qslots, qmb, capmb, lag, dropped, decoded, spots, late,
 *  wait "doppler"?, heard:[{t slot UTC, m mode, c call, g grid, s snr, f Hz (satellite frame), x message, late?}]}. The SATELLITE
 *  box shows a SKIM switch (→ {"type":"xpdr_skim","on":…}), one line of state and the compact list of what it heard. */
export interface XpdrHeard { t: number; mode: 'FT8' | 'FT4'; call: string; grid: string; snr: number; hz: number; text: string; late: boolean }
export interface XpdrSkim {
  on: boolean;
  mode: 'live' | 'catchup';
  windows: number; workers: number; depth: number;
  queued: number; qslots: number; qmb: number; capmb: number; lag: number;
  dropped: number; decoded: number; spots: number; late: number;
  waiting: boolean;
  heard: XpdrHeard[];
}
export const XPDR_SKIM_MAX_HEARD = 20;
/** The SKIM switch's words for a screen reader. */
export const XPDR_SKIM_TITLE = 'Decode FT8 and FT4 across the whole transponder as each slot ends (spots tagged via the satellite, not sent to PSK Reporter)';
export function parseXpdrSkim(v: unknown): XpdrSkim | null {
  if (!v || typeof v !== 'object') return null;
  const o = v as Record<string, unknown>;
  const n = (x: unknown) => Math.max(0, num(x));
  const heard: XpdrHeard[] = [];
  if (Array.isArray(o.heard)) {
    for (const h of o.heard.slice(0, XPDR_SKIM_MAX_HEARD)) {
      if (!h || typeof h !== 'object') continue;
      const x = h as Record<string, unknown>;
      const call = str(x.c, 12).replace(/[^A-Za-z0-9/]/g, '').toUpperCase();
      const hz = num(x.f);
      if (!call || !(hz > 0)) continue;
      heard.push({ t: num(x.t), mode: x.m === 'FT4' ? 'FT4' : 'FT8', call, grid: str(x.g, 6).replace(/[^A-Za-z0-9]/g, '').toUpperCase(),
                   snr: Math.round(num(x.s)), hz, text: str(x.x, 48), late: x.late === true });
    }
  }
  return { on: o.on === true, mode: o.mode === 'catchup' ? 'catchup' : 'live', windows: n(o.windows), workers: n(o.workers), depth: n(o.depth),
           queued: n(o.queued), qslots: n(o.qslots), qmb: n(o.qmb), capmb: n(o.capmb), lag: n(o.lag), dropped: n(o.dropped),
           decoded: n(o.decoded), spots: n(o.spots), late: n(o.late), waiting: o.wait === 'doppler', heard };
}
/** What SKIM sends. */
export const xpdrSkimMsg = (on: boolean): { type: 'xpdr_skim'; on: boolean } => ({ type: 'xpdr_skim', on });
/** The skimmer's one line: "FT8 + FT4 · 9 windows · live · 3 spots", "catching up — 2 slots waiting", "waiting for the Doppler lock". */
export function xpdrSkimLine(k: XpdrSkim | null | undefined): string {
  if (!k) return '';
  if (!k.on) return k.mode === 'catchup'
    ? 'FT8 / FT4 skimmer off — this server decodes it as catch-up (behind the pass, as the processor allows). SKIM switches it on.'
    : 'FT8 / FT4 skimmer off — SKIM switches it on.';
  if (k.waiting) return 'FT8 / FT4: waiting for the Doppler lock';
  const parts = [`FT8 + FT4 · ${k.windows} window${k.windows === 1 ? '' : 's'} · ${k.mode === 'catchup' ? 'catch-up' : 'live'}`];
  if (k.qslots > 1 || k.lag > 20) parts.push(`catching up — ${k.qslots} slot${k.qslots === 1 ? '' : 's'} waiting${k.lag > 0 ? `, ${Math.round(k.lag)} s behind` : ''}`);
  if (k.dropped > 0) parts.push(`${k.dropped} slot${k.dropped === 1 ? '' : 's'} dropped (queue full)`);
  parts.push(`${k.spots} spot${k.spots === 1 ? '' : 's'}`);
  return parts.join(' · ');
}
/** One heard row: "20:55:07 FT4 DL6UMR JO71 −6 dB 145.8590" (+ " · caught up" when decoded from the queue). */
export function xpdrHeardText(h: XpdrHeard): string {
  const d = new Date(h.t * 1000);
  const two = (x: number) => String(x).padStart(2, '0');
  const t = Number.isFinite(d.getTime()) ? `${two(d.getUTCHours())}:${two(d.getUTCMinutes())}:${two(d.getUTCSeconds())}` : '';
  return `${t} ${h.mode} ${h.text || h.call} ${h.snr > 0 ? '+' : ''}${h.snr} dB ${(h.hz / 1e6).toFixed(4)}${h.late ? ' · caught up' : ''}`;
}

/** A row is drawn FADED after this long unheard (Stuart: "a row fades after a minute of silence"). */
export const XPDR_FADE_SEC = 60;
/** The overlay's label — ORBCOMM's style, the finder's words. */
export const XPDR_SLICE_LABEL = 'TRANSPONDER · every signal found here';
/** The SATELLITE box's plain line for a transponder pick. */
export const XPDR_HINT = 'Every CW signal is decoded and every voice gets LISTEN. Tap the waterfall to tune anywhere else.';
/** ★★★ beta94 …when the finder also decodes the bird's telemetry. */
export const XPDR_HINT_TLM = 'Every CW signal is decoded, every voice gets LISTEN, and the telemetry decodes here too — LISTEN on it to hear it. Tap the waterfall to tune anywhere else.';
/** The box's plain line for this status (the telemetry's when the finder decodes it). */
export const xpdrHint = (st: XpdrStatus | null | undefined): string => (st && st.tlm > 0 ? XPDR_HINT_TLM : XPDR_HINT)
  + (st?.skim ? XPDR_HINT_SKIM : '');   // ★★★ beta97 the skimmer, when the server has one
/** ★★★ beta97 …and when the server skims the transponder's digital modes. */
export const XPDR_HINT_SKIM = ' SKIM FT8/FT4 decodes FT8 and FT4 across the whole passband (spots tagged via the satellite, not sent to PSK Reporter).';
/** ★★★ beta94 LISTEN on the telemetry puts the dial this far below its carrier (USB: the carrier heard at ~1.5 kHz). */
export const XPDR_TLM_LISTEN_BELOW_HZ = 1500;
export const XPDR_MAX_ROWS = 40;

const num = (v: unknown, d = 0): number => (typeof v === 'number' && Number.isFinite(v) ? v : d);
const str = (v: unknown, max: number): string => (typeof v === 'string' ? v.replace(/[^\x20-\x7e]/g, '').slice(0, max) : '');

/** {"type":"xpdr_status","data":…} → a status, or null when it is not one. */
export function parseXpdrStatus(d: unknown): XpdrStatus | null {
  if (!d || typeof d !== 'object') return null;
  const o = d as Record<string, unknown>;
  const box = (v: unknown) => {
    if (!v || typeof v !== 'object') return null;
    const b = v as Record<string, unknown>;
    const lo = num(b.lo), hi = num(b.hi);
    return lo > 0 && hi > lo && hi - lo < 2e6 ? { lo, hi } : null;
  };
  const rows: XpdrRow[] = [];
  if (Array.isArray(o.rows)) {
    for (const r of o.rows.slice(0, XPDR_MAX_ROWS)) {
      if (!r || typeof r !== 'object') continue;
      const x = r as Record<string, unknown>;
      const id = str(x.id, 12);
      const kind = x.kind === 'voice' ? 'voice' : x.kind === 'cw' ? 'cw' : x.kind === 'tlm' ? 'tlm' : null;
      const hz = num(x.hz), dial = num(x.dial);
      if (!id || !kind || !(hz > 0) || !(dial > 0)) continue;
      rows.push({ id, kind, hz, dial, snr: num(x.snr), ago: Math.max(0, num(x.ago)), on: x.on === true, beacon: x.beacon === true,
                  call: str(x.call, 12).replace(/[^A-Za-z0-9/]/g, '').toUpperCase(), text: str(x.text, 80), wpm: Math.max(0, Math.round(num(x.wpm))),
                  car: kind === 'tlm' ? (num(x.car) > 0 ? num(x.car) : dial + XPDR_TLM_LISTEN_BELOW_HZ) : 0,
                  frames: Math.max(0, Math.round(num(x.frames))) });
    }
  }
  let follow: XpdrFollow | null = null;
  if (o.follow && typeof o.follow === 'object') {
    const f = o.follow as Record<string, unknown>;
    const src = (['wait', 'guess', 'voice', 'cw', 'held', 'tlm'] as const).find((s) => s === f.src) ?? '';
    const id = str(f.id, 12);
    if (id) follow = { id, src, off: num(f.off) };
  }
  const tlm = num(o.tlm);
  return { slice: box(o.slice), span: box(o.span), beacon: num(o.beacon), tlm: tlm > 1e6 ? tlm : 0, dop: num(o.dop), rows, follow,
           skim: parseXpdrSkim(o.skim) };
}

/** Is this row FADED (not heard for a minute)? */
export const xpdrFaded = (r: XpdrRow): boolean => !r.on && r.ago >= XPDR_FADE_SEC;

/** The rows in the order the box lists them: on air now first (voices, then the telemetry, then CW, strongest first), then
 *  the rest by how recently heard. The beacon is one more CW row (Stuart: "the beacon is just one more CW row"). */
const KIND_ORDER: Record<XpdrKind, number> = { voice: 0, tlm: 1, cw: 2 };
export function xpdrRowsSorted(rows: XpdrRow[]): XpdrRow[] {
  return rows.slice().sort((a, b) => {
    if (a.on !== b.on) return a.on ? -1 : 1;
    if (a.on && a.kind !== b.kind) return KIND_ORDER[a.kind] - KIND_ORDER[b.kind];
    return a.on ? b.snr - a.snr : a.ago - b.ago;
  });
}

/** The row's frequency in words: "435.6220" (MHz, satellite frame — what the dial shows after LISTEN; the telemetry's
 *  CARRIER, which is what its listing names). */
export const xpdrFreqText = (r: XpdrRow): string => ((r.kind === 'tlm' ? r.car : r.dial) / 1e6).toFixed(4);
/** What the row is, in words: "VOICE", "CW 22 wpm G4ABC", "BEACON", "TELEMETRY · 12 frames". */
export function xpdrWhat(r: XpdrRow): string {
  if (r.kind === 'voice') return 'VOICE';
  if (r.kind === 'tlm') return r.frames > 0 ? `TELEMETRY · ${r.frames} frame${r.frames === 1 ? '' : 's'}` : 'TELEMETRY';
  const parts = [r.beacon ? 'BEACON' : 'CW'];
  if (r.wpm > 0) parts.push(`${r.wpm} wpm`);
  if (r.call) parts.push(r.call);
  return parts.join(' ');
}
/** When it was last heard: "now", "12 s ago", "3 min ago". */
export function xpdrAgo(r: XpdrRow): string {
  if (r.on) return 'now';
  if (r.ago < 60) return `${Math.round(r.ago)} s ago`;
  return `${Math.round(r.ago / 60)} min ago`;
}
/** The follow in words for the box ("VFO A follows · measured from the voice"), '' = not following. */
export function xpdrFollowText(f: XpdrFollow | null): string {
  if (!f) return '';
  switch (f.src) {
    case 'wait': return 'LISTEN: tuning…';
    case 'guess': return 'VFO A follows · predicted from the pass';
    case 'voice': return 'VFO A follows · measured from the voice';
    case 'cw': return 'VFO A follows · locked to the tone';
    case 'held': return 'VFO A follows · held between overs';
    case 'tlm': return 'VFO A follows · measured by the telemetry decoder';
    default: return '';
  }
}

/** ★ LISTEN = tune VFO A to the row (a voice's carrier in USB — the downlink's sideband; a CW row in CW) and ask the
 *  finder to follow it. Returns what to send. Only voices have LISTEN (Stuart: "CW simply decodes"); a CW row returns
 *  null. */
export function xpdrListen(r: XpdrRow, sideband: 'usb' | 'lsb' = 'usb'): { hz: number; mode: string; follow: { type: 'xpdr_follow'; id: string } } | null {
  // ★★★ beta94 …and the TELEMETRY ("allow the audio from the telemetry to also be listenable too"): USB, the dial 1.5 kHz
  //   below its carrier (the row's `dial`), so the carrier is heard at ~1.5 kHz; decoding stays on the slice
  if (r.kind === 'tlm') return { hz: Math.round(r.dial), mode: 'usb', follow: { type: 'xpdr_follow', id: r.id } };
  if (r.kind !== 'voice') return null;
  return { hz: Math.round(r.dial), mode: sideband, follow: { type: 'xpdr_follow', id: r.id } };
}
/** Has this row a LISTEN key? (voices and the telemetry) */
export const xpdrListenable = (r: XpdrRow): boolean => r.kind === 'voice' || r.kind === 'tlm';
/** LISTEN's words for a screen reader. */
export const xpdrListenLabel = (r: XpdrRow): string => (r.kind === 'tlm'
  ? `Listen to the telemetry at ${xpdrFreqText(r)} MHz — VFO A goes 1.5 kHz below its carrier and follows it; it keeps decoding either way`
  : `Put VFO A on the voice at ${xpdrFreqText(r)} MHz — it follows the station while they drift`);

/** The waterfall overlay — ORBCOMM's (src/utils/orbcommView.ts): the slice shaded and labelled, an arrow per signal at its
 *  RF, labelled CW / VOICE (the call once known), solid while heard, hollow when faded. null = nothing to draw. */
export function xpdrOverlayFrom(st: XpdrStatus | null | undefined): OrbOverlay | null {
  if (!st || !st.slice) return null;
  const carriers = xpdrRowsSorted(st.rows)
    .filter((r) => r.hz > 0)
    .slice(0, 16)
    .map((r) => ({ rf: r.hz, locked: r.on || !xpdrFaded(r), label: r.kind === 'voice' ? 'VOICE' : r.kind === 'tlm' ? 'TELEMETRY' : r.call || (r.beacon ? 'BEACON' : 'CW') }));
  return { lo: st.slice.lo, hi: st.slice.hi, carriers, title: XPDR_SLICE_LABEL };
}
