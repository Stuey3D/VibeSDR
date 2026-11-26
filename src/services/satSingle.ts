/* ★★★ beta96 SINGLE MODE: SATELLITES — the WATCH-ONLY view's rules (docs/v12/SAT-SINGLE.md). ONE set of rules for the web
 * page (web/client/src/satellites.ts + main.ts) and the app (src/components/SatellitePanel.tsx + SDRScreen.tsx);
 * scripts/test_sat_single.ts holds them.
 *
 * Stuart 11-26: a radio set to "Single mode: SATELLITES" is the passes'. Listeners land straight in SATELLITE and see the
 * pass on now (or the next one with a countdown) and what it is decoding live, with no controls to retune — the radio makes
 * the picks. Between passes: the radio's timetable and the last pass's outcome (and the gallery), never a dead screen. No
 * way out for listeners; the signed-in admin may. During a LINEAR TRANSPONDER pass the radio is a locked range over the
 * passband and every listener has their own VFO across it with their own AUTO DOPPLER (`sat_hybrid`).
 *
 * The server sends: hwinfo `singleMode:"sat"`; `sat_single` {on, t, plan[], now?, last?, state?, hybrid?} at connect, with
 * every sat_list answer, at AOS and at LOS; `sat_hybrid` {on, norad, tx, name, label, mode, lo, hi, passLo, passHi, centre,
 * los} at AOS / LOS of a transponder pass (and to an arrival); the pass as the room's `picked` (shared:true) at AOS. */

import { hhmm, hmIn, mss } from './satellites';

export interface SatSinglePass { norad: number; tx: number; name: string; label: string; aos: number; los: number; maxEl: number; decoder?: string }
export interface SatHybridInfo {
  on: boolean; norad: number; tx: number; name: string; label: string; mode: string;
  lo: number; hi: number; passLo: number; passHi: number; centre: number; los: number;
}
export interface SatSingle {
  /** the owner's unattended passes are switched on (off: the radio has nothing to run — the view says so) */
  on: boolean;
  /** the server's satellite clock when it sent this (unix s) — the countdowns run from the listener's own clock */
  t: number;
  plan: SatSinglePass[];
  now?: SatSinglePass;
  last?: string;
  state?: string;
  hybrid?: SatHybridInfo;
}

const num = (v: unknown, d = 0) => (typeof v === 'number' && Number.isFinite(v) ? v : d);
const str = (v: unknown) => (typeof v === 'string' ? v : '');
function pass(o: unknown): SatSinglePass | null {
  if (!o || typeof o !== 'object') return null;
  const p = o as Record<string, unknown>;
  const norad = num(p.norad, -1), aos = num(p.aos, -1), los = num(p.los, -1);
  if (norad <= 0 || aos < 0 || los <= aos) return null;
  return { norad, tx: num(p.tx, 0), name: str(p.name) || `#${norad}`, label: str(p.label), aos, los, maxEl: num(p.maxEl),
           ...(str(p.decoder) ? { decoder: str(p.decoder) } : {}) };
}
/** `{"type":"sat_hybrid",…}` → the hybrid, or null when it is off / malformed. */
export function parseSatHybrid(o: unknown): SatHybridInfo | null {
  if (!o || typeof o !== 'object') return null;
  const h = o as Record<string, unknown>;
  if (h.on !== true) return null;
  const lo = num(h.lo), hi = num(h.hi);
  if (!(lo > 0) || !(hi > lo)) return null;
  return { on: true, norad: num(h.norad), tx: num(h.tx), name: str(h.name), label: str(h.label), mode: str(h.mode) || 'usb',
           lo, hi, passLo: num(h.passLo, lo), passHi: num(h.passHi, hi), centre: num(h.centre, (lo + hi) / 2), los: num(h.los) };
}
/** `{"type":"sat_single",…}` → the watch view's facts (null = malformed). */
export function parseSatSingle(o: unknown): SatSingle | null {
  if (!o || typeof o !== 'object') return null;
  const s = o as Record<string, unknown>;
  const plan = Array.isArray(s.plan) ? (s.plan.map(pass).filter(Boolean) as SatSinglePass[]).slice(0, 12) : [];
  const now = pass(s.now);
  const hybrid = parseSatHybrid(s.hybrid);
  return { on: s.on !== false, t: num(s.t), plan, ...(now ? { now } : {}), ...(str(s.last) ? { last: str(s.last) } : {}),
           ...(str(s.state) ? { state: str(s.state) } : {}), ...(hybrid ? { hybrid } : {}) };
}

/** Does THIS listener watch? hwinfo `singleMode` "sat" — never the signed-in admin (they may tune and leave). */
export function satSingleWatch(singleMode: string | null | undefined, admin: boolean): boolean {
  return singleMode === 'sat' && !admin;
}

/** The note in place of the EXIT key. */
export const SAT_SINGLE_ONLY = 'SATELLITES only on this receiver';
/** What a way out tried anyway (Back, Escape, another mode) says. */
export const SAT_SINGLE_WHY = 'This receiver follows the satellite passes itself — its owner has set it up to watch them. Watch what it decodes here.';
/** The line under the watch header between passes. */
export const SAT_SINGLE_BETWEEN = 'Between passes: the timetable below, and the last pass’s pictures and packets in the GALLERY.';

/** The pass the view shows now: the one running (the server's `now`), else the first of the plan still to rise. */
export function satSingleNext(s: SatSingle | null, nowSec: number): SatSinglePass | null {
  if (!s) return null;
  if (s.now && s.now.los > nowSec) return s.now;
  return s.plan.find((p) => p.los > nowSec && p.aos > nowSec) ?? s.plan.find((p) => p.los > nowSec) ?? null;
}
const passName = (p: SatSinglePass) => (p.label ? `${p.name} · ${p.label}` : p.name);

/** The watch header: "LIVE · RS-44 · Transponder · 6:12 left" / "NEXT · Meteor-M2-3 · LRPT · AOS 12:05 (in 0:38)". */
export function satSingleHead(s: SatSingle | null, nowSec: number): string {
  if (!s) return 'Asking the receiver for its timetable…';
  if (!s.on) return 'The owner has not switched the satellite passes on yet';
  const p = satSingleNext(s, nowSec);
  if (!p) return s.state ? `Waiting — ${s.state}` : 'No pass of this receiver’s satellites in the next 24 hours';
  const live = s.now && s.now.norad === p.norad && s.now.tx === p.tx && p.aos <= nowSec;
  if (live || (p.aos <= nowSec && p.los > nowSec)) return `LIVE · ${passName(p)} · ${mss(p.los - nowSec)} left`;
  return `NEXT · ${passName(p)} · AOS ${hhmm(p.aos)} (in ${hmIn(p.aos, nowSec)}) · max ${Math.round(p.maxEl)}°`;
}
/** The line under it: what the radio is doing, in words. */
export function satSingleSub(s: SatSingle | null, nowSec: number): string {
  if (!s || !s.on) return '';
  if (s.hybrid) return satHybridText(s.hybrid, nowSec);
  const p = satSingleNext(s, nowSec);
  if (p && p.aos <= nowSec) {
    const d = s.now?.decoder;
    return d === 'lrpt' ? 'The picture builds below as it is received — the radio follows the satellite and its Doppler itself.'
      : d === 'sstv' ? 'Any SSTV picture appears below as it is received.'
      : d === 'aprs' ? 'Packets appear below as they are decoded.'
      : d === 'fctlm' ? 'Telemetry appears below as it is decoded.'
      : 'The radio follows the satellite and its Doppler itself.';
  }
  return SAT_SINGLE_BETWEEN;
}
/** The hybrid in words: "Your own VFO on RS-44 · Transponder: tune 435.607–435.673 MHz · AUTO DOPPLER · 6:12 to LOS". */
export function satHybridText(h: SatHybridInfo, nowSec: number): string {
  const mhz = (v: number) => (v / 1e6).toFixed(3);
  const left = h.los > nowSec ? ` · ${mss(h.los - nowSec)} to LOS` : '';
  return `Your own VFO on ${h.name}${h.label ? ' · ' + h.label : ''}: tune anywhere in ${mhz(h.lo)}–${mhz(h.hi)} MHz, SSB or CW, with your own AUTO DOPPLER${left}.`;
}
/** Is a tune to `hz` inside the hybrid's window? (The client refuses first, so a drag does not fight the server.) */
export function satHybridInWindow(h: SatHybridInfo | null, hz: number): boolean {
  return !!h && hz >= h.lo && hz <= h.hi;
}
/** Clamp a tune into the window (a drag off the edge stops at it). */
export function satHybridClamp(h: SatHybridInfo, hz: number): number { return Math.max(h.lo, Math.min(h.hi, hz)); }

/** The timetable rows: "12:05–12:16 · Meteor-M2-3 · LRPT · max 56°" (+ "· now"), at most 12, the running one first. */
export function satSinglePlanRows(s: SatSingle | null, nowSec: number): { key: string; text: string; live: boolean }[] {
  if (!s) return [];
  return s.plan.filter((p) => p.los > nowSec).map((p) => {
    const live = p.aos <= nowSec;
    return { key: `${p.norad}/${p.tx}/${p.aos}`, live,
             text: `${hhmm(p.aos)}–${hhmm(p.los)} · ${passName(p)} · max ${Math.round(p.maxEl)}°${live ? ' · now' : ''}` };
  });
}
/** The last pass, as the server said it ("Meteor-M2-3 LRPT, max 56° — ended (LOS)"), prefixed. */
export function satSingleLastText(s: SatSingle | null): string { return s?.last ? `Last pass: ${s.last}` : ''; }
