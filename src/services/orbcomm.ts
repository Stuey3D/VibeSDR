// orbcomm.ts — V12 2026-11-24 ORBCOMM, the clients' half (docs/v12/ORBCOMM.md; the server is
// android/app/src/main/cpp/vibe_orbcomm_run.h). One copy for the app (SatellitePanel) and the web page (satellites.ts),
// so both say the same thing about the birds being decoded.
//
// ★★★ WHAT IS SHOWN: ORBCOMM's commercial messaging satellites (137.2–137.8 MHz) are a REGULAR satellite pick now — the
//     server decodes every ORBCOMM carrier in its slice and names each bird by the satellite's own GPS position report.
//     The box shows, per bird: its name (and how it was named), SNR, Doppler measured against predicted, az / el, packets
//     by type, and the latest HOUSEKEEPING as named values — the satellite's position, height and speed, its clock and how
//     far it is from ours, the downlink and uplink channel lists, its orbital elements.
// ★★★ SUBSCRIBER MESSAGES ARE NEVER DECODED. They are private user traffic: the server sends each as "subscriber message —
//     not decoded" with its raw bytes, and the box shows exactly that — the bytes, never a reading of them.
// ★ Wire (decoder socket, beside the decoder's text lines):
//     {"type":"orbcomm_status","data":{t,good,bad,messages,dropped,maxChans,located,birds,counts:{…},sats:[…],carriers:[…],
//       pick,up,dots:[[az10,el10,snr|null,t,norad]…]}} once a second
//     {"type":"orbcomm_pkt","data":{t,norad,type,snr,rf,sat,text,hex,raw,v,fixed}} per packet worth a line
// ★ Pure — scripts/test_orbcomm_app.ts. Everything from the air is cleaned and bounded here; junk → null, never a throw.
import { parseSkyDotsLive, type SkyDot } from '../utils/skyPlot';
import type { SatInfo, SatList } from './satellites';

export type OrbHow = 'position' | 'id' | 'doppler' | '';
export interface OrbEph { lat: number; lon: number; altKm: number; kmS: number; clock: number; id?: number; week?: number; tow?: number }
export interface OrbSat {
  norad: number; name: string; how: OrbHow; carrier: number; rf: number; snr: number; dop: number; pred: number;
  az: number; el: number; dcn: number; nominal: number; scId: number; ok: number; messages: number; age: number;
  eph?: OrbEph;
  down?: { part: number; of: number; hz: number[] };
  up?: { hz: number[]; retries: number; slots: number };
  elements?: { sc: number; ma: number; mm: number };
}
export interface OrbCarrier {
  id: number; rf: number; snr: number; lock: boolean; ok: number; bad: number; fixed: number;
  /** ★ 2026-11-24 the bird on this carrier once named ('' = not yet) — the waterfall's carrier mark */
  sat: string;
}
/** ★★ 2026-11-24 THE SLICE the decoder is fed and searches (RF edges lo…hi — drawn on the waterfall), how much has arrived,
 *  how often its centre moved (a dial move on a one-pipeline radio moves it), and the scanner's floor. */
export interface OrbSlice { rate: number; centre: number; lo: number; hi: number; fedSec: number; blocks: number; moves: number; floorDb: number }
export interface OrbPeak { rf: number; db: number }
export interface OrbRefused { spurs: number; notSdpsk: number; last: { rf: number; why: string; ago: number; ban: number }[] }
export interface OrbStatus {
  t: number; good: number; bad: number; messages: number; dropped: number; maxChans: number; located: boolean; birds: number;
  counts: Record<string, number>; sats: OrbSat[]; carriers: OrbCarrier[];
  /** the SATELLITE pick's NORAD (0 none) and whether it is up (1), not (0), or there is no pick (−1) */
  pick: number; up: number;
  /** ★★★ beta94 THE BOX FOLLOWS THE DECODER (vibe_orbcomm_run.h followStep_): the pick's pass has been seen up and is over
   *  (`ended`), and the bird the decoder hears now (`follow`, 0 = none; `followName`). An older server sends neither:
   *  ended false — the box stays on the pick, as before. */
  ended: boolean; follow: number; followName: string;
  /** the sky dots new since the last status */
  dots: SkyDot[];
  /** ★★ 2026-11-24 where the decoder searches (null = an older server: nothing drawn), its strongest peaks, what it refused */
  slice: OrbSlice | null;
  peaks: OrbPeak[];
  refused: OrbRefused | null;
}
export type OrbPktType = 'ephemeris' | 'downlink' | 'uplink' | 'elements' | 'ncc' | 'message' | 'unknown';
export interface OrbPkt {
  t: number; norad: number; type: OrbPktType; snr: number; rf: number; sat: string; text: string;
  /** the packet's bytes, "1A 30 01 82 …" */
  hex: string;
  /** a subscriber message: shown as raw bytes only, never decoded */
  raw: boolean;
  /** symbols the decoder repaired (0–2) */
  fixed: number;
  /** the satellite's clock against ours (s) — an ephemeris packet only */
  clockOff?: number;
}

const n = (v: unknown, d = 0): number => (typeof v === 'number' && Number.isFinite(v) ? v : d);
const cnt = (v: unknown): number => Math.max(0, Math.floor(n(v)));
const s = (v: unknown, max = 60): string => (typeof v === 'string' ? v.replace(/[\u0000-\u001f\u007f]/g, '').slice(0, max) : '');
const obj = (v: unknown): Record<string, unknown> | null => (v && typeof v === 'object' && !Array.isArray(v) ? v as Record<string, unknown> : null);
const hzList = (v: unknown): number[] => (Array.isArray(v) ? v.filter((x): x is number => typeof x === 'number' && Number.isFinite(x) && x > 0 && x < 1e10).slice(0, 24) : []);
const HOWS: OrbHow[] = ['position', 'id', 'doppler', ''];
const TYPES: OrbPktType[] = ['ephemeris', 'downlink', 'uplink', 'elements', 'ncc', 'message', 'unknown'];
/** The packet types counted (the order the box lists them). */
export const ORB_COUNT_KEYS = ['ephemeris', 'downlink', 'uplink', 'elements', 'ncc', 'message', 'sync', 'fill'] as const;

function ephOf(v: unknown): OrbEph | undefined {
  const o = obj(v);
  if (!o) return undefined;
  const lat = n(o.lat, NaN), lon = n(o.lon, NaN);
  if (!(Math.abs(lat) <= 90) || !(Math.abs(lon) <= 360)) return undefined;
  return { lat, lon, altKm: Math.max(0, Math.min(5000, n(o.altKm))), kmS: Math.max(0, Math.min(20, n(o.kmS))), clock: Math.max(0, n(o.clock)),
           id: o.id != null ? cnt(o.id) : undefined, week: o.week != null ? cnt(o.week) : undefined, tow: o.tow != null ? cnt(o.tow) : undefined };
}

/** {"type":"orbcomm_status","data":{…}}'s data → the shape, or null. */
export function parseOrbcommStatus(d: unknown): OrbStatus | null {
  const o = obj(d);
  if (!o) return null;
  const counts: Record<string, number> = {};
  const c = obj(o.counts);
  if (c) for (const [k, v] of Object.entries(c).slice(0, 16)) if (/^[a-z]{1,12}$/.test(k) && typeof v === 'number' && Number.isFinite(v) && v >= 0) counts[k] = Math.floor(v);
  const sats: OrbSat[] = [];
  if (Array.isArray(o.sats))
    for (const x of (o.sats as unknown[]).slice(0, 12)) {
      const q = obj(x);
      if (!q) continue;
      const how = HOWS.includes(q.how as OrbHow) ? (q.how as OrbHow) : '';
      const sat: OrbSat = {
        norad: cnt(q.norad), name: s(q.name, 40), how, carrier: Math.floor(n(q.carrier, -1)), rf: Math.max(0, n(q.rf)), snr: n(q.snr),
        dop: n(q.dop), pred: n(q.pred), az: ((n(q.az) % 360) + 360) % 360, el: Math.max(-90, Math.min(90, n(q.el))),
        dcn: cnt(q.dcn), nominal: Math.max(0, n(q.nominal)), scId: Math.floor(n(q.scId, -1)), ok: cnt(q.ok), messages: cnt(q.messages), age: Math.max(0, n(q.age)),
      };
      const eph = ephOf(q.eph); if (eph) sat.eph = eph;
      const dn = obj(q.down); if (dn) sat.down = { part: cnt(dn.part), of: cnt(dn.of), hz: hzList(dn.hz) };
      const up = obj(q.up); if (up) sat.up = { hz: hzList(up.hz), retries: cnt(up.retries), slots: cnt(up.slots) };
      const el = obj(q.elements); if (el) sat.elements = { sc: cnt(el.sc), ma: n(el.ma), mm: n(el.mm) };
      sats.push(sat);
    }
  const carriers: OrbCarrier[] = [];
  if (Array.isArray(o.carriers))
    for (const x of (o.carriers as unknown[]).slice(0, 12)) {
      const q = obj(x);
      if (!q) continue;
      carriers.push({ id: Math.floor(n(q.id)), rf: Math.max(0, n(q.rf)), snr: n(q.snr), lock: q.lock === true, ok: cnt(q.ok), bad: cnt(q.bad), fixed: cnt(q.fixed),
                      sat: s(q.sat, 40) });
    }
  // ★★ the slice: a window only when its edges are real RF (lo < hi, under 10 GHz, at most 20 MHz wide)
  let slice: OrbSlice | null = null;
  { const q = obj(o.slice);
    const lo = q ? n(q.lo, NaN) : NaN, hi = q ? n(q.hi, NaN) : NaN;
    if (q && lo > 0 && hi > lo && hi < 1e10 && hi - lo <= 20e6)
      slice = { rate: Math.max(0, n(q.rate)), centre: n(q.centre, (lo + hi) / 2), lo, hi, fedSec: Math.max(0, n(q.fedSec)), blocks: cnt(q.blocks), moves: cnt(q.moves), floorDb: n(q.floorDb) }; }
  const peaks: OrbPeak[] = Array.isArray(o.peaks)
    ? (o.peaks as unknown[]).slice(0, 12).map(obj).filter((q): q is Record<string, unknown> => !!q && n(q.rf) > 0 && n(q.rf) < 1e10).map((q) => ({ rf: n(q.rf), db: n(q.db) }))
    : [];
  let refused: OrbRefused | null = null;
  { const q = obj(o.refused);
    if (q) refused = { spurs: cnt(q.spurs), notSdpsk: cnt(q.notSdpsk),
      last: Array.isArray(q.last) ? (q.last as unknown[]).slice(0, 8).map(obj).filter((x): x is Record<string, unknown> => !!x && n(x.rf) > 0)
        .map((x) => ({ rf: n(x.rf), why: s(x.why, 40), ago: Math.max(0, n(x.ago)), ban: Math.max(0, n(x.ban)) })) : [] }; }
  const up = Math.floor(n(o.up, -1));
  return {
    t: Math.max(0, n(o.t)), good: cnt(o.good), bad: cnt(o.bad), messages: cnt(o.messages), dropped: cnt(o.dropped),
    maxChans: cnt(o.maxChans), located: o.located === true, birds: cnt(o.birds), counts, sats, carriers,
    pick: cnt(o.pick), up: up === 1 || up === 0 ? up : -1, dots: parseSkyDotsLive(o.dots), slice, peaks, refused,
    ended: o.ended === true, follow: o.ended === true ? cnt(o.follow) : 0, followName: o.ended === true ? s(o.followName, 40) : '',
  };
}

/** {"type":"orbcomm_pkt","data":{…}}'s data → the shape, or null (no time / no text). */
export function parseOrbcommPkt(d: unknown): OrbPkt | null {
  const o = obj(d);
  if (!o) return null;
  const t = n(o.t), text = s(o.text, 240);
  if (!(t > 0) || !text) return null;
  const type = TYPES.includes(o.type as OrbPktType) ? (o.type as OrbPktType) : 'unknown';
  const hex = typeof o.hex === 'string' ? (o.hex.toUpperCase().match(/[0-9A-F]{2}/g) ?? []).slice(0, 32).join(' ') : '';
  const p: OrbPkt = { t, norad: cnt(o.norad), type, snr: n(o.snr), rf: Math.max(0, n(o.rf)), sat: s(o.sat, 40), text,
                      hex, raw: o.raw === true || type === 'message', fixed: Math.min(9, cnt(o.fixed)) };
  const v = obj(o.v);
  if (type === 'ephemeris' && v && n(v.clock) > 0) p.clockOff = n(v.clock) - t;
  return p;
}

// ── the box's state (both clients keep one of these per pick) ───────────────────────────────────────────────────────────
export interface OrbView {
  status: OrbStatus | null;
  /** the recent packets, newest first (bounded) */
  pkts: OrbPkt[];
  /** each bird's clock against ours, from its last position report (norad → s) */
  clockOff: Record<number, number>;
  /** ★★ 2026-11-24 THE HOLD (navaids.ts vorHold's pattern — Stuart: a bird's block vanished when its carrier dropped): each
   *  named bird's last block and the server time (status t) it was last listed — kept, drawn dimmed with its age, until
   *  ORB_HOLD_SEC passes or the decoder ends (the box's state is made fresh when the decoder changes). ★★★ beta94: NOT on
   *  a status for another pick — another ORBCOMM pick keeps the same whole-band decoder (and the lock's `pick` goes to 0
   *  at LOS), so the birds it heard stay. */
  held: Record<number, { sat: OrbSat; t: number }>;
  /** the pick the hold belongs to (status `pick`) */
  heldPick: number;
}
export const ORB_PKTS_MAX = 40;
/** How long a bird's last block stays after the server stops listing it (s). */
export const ORB_HOLD_SEC = 600;
export const orbViewEmpty = (): OrbView => ({ status: null, pkts: [], clockOff: {}, held: {}, heldPick: 0 });
/** A status or a packet folded into the box's state (a status keeps the packets; a packet keeps the status). */
export function orbViewMerge(prev: OrbView | null, m: { status?: OrbStatus | null; pkt?: OrbPkt | null }): OrbView {
  const v = prev ?? orbViewEmpty();
  let next = v;
  if (m.status) {
    const st = m.status;
    // ★ the hold: each listed, named bird refreshes its entry; one unheard for ORB_HOLD_SEC goes (★★★ beta94 kept across
    //   picks: the same decoder runs on)
    const held: OrbView['held'] = { ...(v.held ?? {}) };
    for (const b of st.sats) if (b.norad > 0) held[b.norad] = { sat: b, t: st.t };
    for (const k of Object.keys(held)) if (st.t - held[Number(k)].t + held[Number(k)].sat.age > ORB_HOLD_SEC) delete held[Number(k)];
    next = { ...next, status: st, held, heldPick: st.pick };
  }
  if (m.pkt) {
    const pkts = [m.pkt, ...next.pkts];
    if (pkts.length > ORB_PKTS_MAX) pkts.length = ORB_PKTS_MAX;
    const clockOff = m.pkt.clockOff != null && m.pkt.norad ? { ...next.clockOff, [m.pkt.norad]: m.pkt.clockOff } : next.clockOff;
    next = { ...next, pkts, clockOff };
  }
  return next;
}

/** The birds the box shows: those the server lists now, then the held ones it has stopped listing — each with `heldSecs`
 *  (seconds since it was last heard, drawn dimmed) — newest heard first. */
export function orbShownSats(v: OrbView | null | undefined): { sat: OrbSat; heldSecs?: number }[] {
  const st = v?.status;
  if (!st) return [];
  const out: { sat: OrbSat; heldSecs?: number }[] = st.sats.map((sat) => ({ sat }));
  const listed = new Set(st.sats.map((b) => b.norad).filter((x) => x > 0));
  const held = Object.values(v!.held ?? {}).filter((h) => !listed.has(h.sat.norad))
    .map((h) => ({ sat: h.sat, heldSecs: Math.max(0, Math.round(st.t - h.t + h.sat.age)) }))
    .filter((h) => h.heldSecs <= ORB_HOLD_SEC)
    .sort((a, b) => a.heldSecs - b.heldSecs);
  return out.concat(held);
}
/** "last heard 42 s ago" / "last heard 3 min ago". */
export function orbHeldText(secs: number): string {
  return secs < 120 ? `last heard ${Math.max(0, Math.round(secs))} s ago` : `last heard ${Math.round(secs / 60)} min ago`;
}

// ── words ───────────────────────────────────────────────────────────────────────────────────────────────────────────
/** The line every ORBCOMM box carries. */
export const ORB_CAVEAT = 'Subscriber messages are private and are not decoded — shown as raw bytes only.';
/** How a bird was named. */
export function orbHowText(how: OrbHow): string {
  return how === 'position' ? 'named by its own position report'
    : how === 'id' ? 'named earlier by its own position report'
    : how === 'doppler' ? 'named by its channel and Doppler'
    : 'not yet named';
}
const kHz = (hz: number) => `${hz < 0 ? '−' : '+'}${(Math.abs(hz) / 1000).toFixed(2)} kHz`;
const mhz = (hz: number) => (hz / 1e6).toFixed(4);
/** The box's head: "ORBCOMM · 2 birds · 341 good · 12 bad · 5 subscriber messages (raw)". */
export function orbHeadText(st: OrbStatus | null): string {
  if (!st) return 'ORBCOMM · listening for carriers';
  const parts = ['ORBCOMM'];
  parts.push(st.sats.length ? `${st.sats.length} bird${st.sats.length === 1 ? '' : 's'} decoding` : 'no bird decoding yet');
  parts.push(`${st.good.toLocaleString('en-GB')} good`, `${st.bad.toLocaleString('en-GB')} bad`);
  if (st.messages) parts.push(`${st.messages.toLocaleString('en-GB')} subscriber message${st.messages === 1 ? '' : 's'} (raw)`);
  if (st.dropped) parts.push(`${st.dropped.toLocaleString('en-GB')} lines skipped`);
  return parts.join(' · ');
}
/** Under the head when something stops the plot / the names: no location, no orbital elements. '' = nothing to say. */
export function orbNoteText(st: OrbStatus | null): string {
  if (!st) return '';
  if (!st.located) return 'This server has no location set — the birds are named by their own position reports, but there is no sky plot, azimuth or elevation.';
  if (!st.birds) return 'No ORBCOMM orbital elements on this server yet — the birds are named only by their own position reports.';
  return '';
}
/** "ephemeris 12 · downlink 3 · uplink 3 · messages 5" — the counts that are not zero, in a fixed order. */
export function orbCountsText(c: Record<string, number> | undefined): string {
  if (!c) return '';
  const word: Record<string, string> = { ephemeris: 'position', downlink: 'downlink lists', uplink: 'uplink lists', elements: 'elements',
                                         ncc: 'gateway', message: 'subscriber (raw)', sync: 'sync', fill: 'fill' };
  return ORB_COUNT_KEYS.filter((k) => (c[k] ?? 0) > 0).map((k) => `${word[k]} ${c[k].toLocaleString('en-GB')}`).join(' · ');
}
/** A bird's lines in the box (name first). `clockOff` = its clock against ours from its last position report. */
export function orbSatLines(b: OrbSat, clockOff?: number, heldSecs?: number): string[] {
  const out: string[] = [];
  out.push(`${b.name || (b.norad ? `NORAD ${b.norad}` : 'Unknown ORBCOMM bird')} · ${orbHowText(b.how)}${heldSecs != null ? ` · ${orbHeldText(heldSecs)}` : ''}`);
  const sky = b.el > -90 && (b.az || b.el) ? ` · az ${Math.round(b.az)}° el ${Math.round(b.el)}°` : '';
  out.push(`SNR ${b.snr.toFixed(1)} dB · Doppler ${kHz(b.dop)}${b.pred ? ` (predicted ${kHz(b.pred)})` : ''}${sky}`);
  out.push(`${b.ok.toLocaleString('en-GB')} good packets${b.messages ? ` · ${b.messages.toLocaleString('en-GB')} subscriber messages (raw)` : ''}`
    + `${b.rf ? ` · ${mhz(b.rf)} MHz` : ''}${heldSecs == null && b.age >= 3 ? ` · last ${Math.round(b.age)} s ago` : ''}`);
  if (b.eph) {
    const e = b.eph;
    out.push(`Position ${Math.abs(e.lat).toFixed(2)}°${e.lat >= 0 ? 'N' : 'S'} ${Math.abs(e.lon > 180 ? e.lon - 360 : e.lon).toFixed(2)}°${(e.lon > 180 ? e.lon - 360 : e.lon) >= 0 ? 'E' : 'W'}`
      + ` · ${Math.round(e.altKm)} km up · ${e.kmS.toFixed(2)} km/s`);
    if (e.clock > 0) {
      const d = new Date(e.clock * 1000);
      const p2 = (x: number) => String(x).padStart(2, '0');
      const off = clockOff != null && Number.isFinite(clockOff) ? ` (${clockOff >= 0 ? '+' : '−'}${Math.abs(clockOff).toFixed(1)} s from ours)` : '';
      out.push(`Satellite clock ${p2(d.getUTCHours())}:${p2(d.getUTCMinutes())}:${p2(d.getUTCSeconds())} UTC${off}`);
    }
  }
  if (b.down && b.down.hz.length) out.push(`Downlink channels${b.down.of > 1 ? ` (${b.down.part}/${b.down.of})` : ''}: ${b.down.hz.map(mhz).join(', ')} MHz`);
  if (b.up && b.up.hz.length) out.push(`Uplink channels: ${b.up.hz.map(mhz).join(', ')} MHz · ${b.up.retries} retries · ${b.up.slots} access slots`);
  if (b.elements) out.push(`Elements for spacecraft ${b.elements.sc}: mean anomaly ${b.elements.ma.toFixed(3)}° · mean motion ${b.elements.mm.toFixed(6)} rev/day`);
  return out;
}
/** A packet's line in the list: "08:41:03 · FM114 · position 51.20N 0.40W, …" — a subscriber message ALWAYS reads
 *  "Subscriber message — not decoded" (its part, if the server said it); its bytes are shown apart (orbPktHex). */
export function orbPktLine(p: OrbPkt): string {
  const d = new Date(p.t * 1000);
  const p2 = (x: number) => String(x).padStart(2, '0');
  const when = `${p2(d.getUTCHours())}:${p2(d.getUTCMinutes())}:${p2(d.getUTCSeconds())}`;
  const who = p.sat ? p.sat.replace(/^ORBCOMM\s+/i, '') : '';
  let what = p.text;
  if (p.raw) {
    const part = p.text.match(/part\s+(\d+)\s+of\s+(\d+)/i);
    what = `Subscriber message — not decoded${part ? ` (part ${part[1]} of ${part[2]})` : ''}`;
  }
  return [when, who, what].filter(Boolean).join(' · ') + (p.fixed ? ` · ${p.fixed} repaired` : '');
}
/** The bytes a subscriber message is shown as (and any packet's, behind RAW). */
export function orbPktHex(p: OrbPkt): string { return p.hex; }

// ── the manual fallback under AUTO: how many carriers at most ───────────────────────────────────────────────────────────
/** 0 = AUTO (the server's 4), else 1–6. */
export const ORB_CARRIER_CHOICES = [0, 1, 2, 3, 4, 5, 6] as const;
export const orbCarriersLabel = (k: number) => (k ? String(k) : 'AUTO');
/** The attach parameters: the pick's own (so the page JOINS its run), with "max_carriers" when one is set by hand. */
export function orbAttachParams(pick: Record<string, unknown> | null | undefined, maxCarriers: number): Record<string, unknown> {
  const p: Record<string, unknown> = { ...(pick ?? {}) };
  delete p.max_carriers;
  const k = Math.floor(maxCarriers);
  if (k >= 1 && k <= 6) p.max_carriers = k;
  return p;
}
/** The decoder's word before it has said anything. */
export const ORB_LISTENING = 'listening for ORBCOMM carriers';

// ── ★★★ beta94 THE BOX FOLLOWS THE DECODER (Stuart 11-25) ─────────────────────────────────────────────────────────────
/* "i left an expired pass open and the next pass started decoding as expected but when i selected the correct pass to see
 * the half globe correctly it reset the pass that was decoding. So in orbcomm mode if someone has left it open can the half
 * globe switch to the next pass when a new orbcomm comes into range and starts decoding."
 * An ORBCOMM pick runs ONE whole-band decoder: when the picked bird sets it carries on with the next birds. The SATELLITE
 * box (name, half-globe, pass line, "in range for…", arrows) then FOLLOWS, with no new pick:
 *   1. the picked bird while its pass is in progress (or before it has been up — a pick made ahead of its pass);
 *   2. once the server says its pass is over (status `ended`): the bird the decoder hears now (status `follow`) — one
 *      answer for the whole room, so a listener who joins a shared dial sees the same bird;
 *   3. nothing decoding yet: the next ORBCOMM bird to rise (the soonest AOS; one already up counts as risen first).
 * Picking another ORBCOMM bird from the list keeps the decoder (the server's satPick) and starts this over.
 * Pure — scripts/test_orbcomm_app.ts. Both clients: the web's satellites.ts, the app's SDRScreen / SatellitePanel. */
export type OrbFollowWhy = 'pick' | 'decoding' | 'next';
export interface OrbFollow { norad: number; why: OrbFollowWhy }
/** An ORBCOMM bird in the satellite list (a downlink the ORBCOMM decoder reads, or the constellation's name). */
export function orbIsBird(sat: Pick<SatInfo, 'name' | 'tx'>): boolean {
  return /^ORBCOMM\b/i.test(sat.name) || sat.tx.some((t) => t.decoder === 'orbcomm' || t.mode === 'orbcomm' || t.consumer === 'orbcomm');
}
/** The bird the box shows for an ORBCOMM pick of `pickNorad`, from the decoder's status and the list, at `nowSec`. */
export function orbFollowChoice(pickNorad: number, st: OrbStatus | null | undefined, list: SatList | null | undefined, nowSec: number): OrbFollow {
  const pick: OrbFollow = { norad: pickNorad, why: 'pick' };
  if (!st || !st.ended || !list) return pick;
  const sats = list.sats;
  const picked = sats.find((x) => x.norad === pickNorad);
  // the pick has risen again (the server's 1-s status can trail this clock): its own pass
  if (picked && picked.passes.some((p) => p.aos <= nowSec && p.los > nowSec)) return pick;
  if (st.follow > 0 && st.follow !== pickNorad && sats.some((x) => x.norad === st.follow)) return { norad: st.follow, why: 'decoding' };
  let best: { sat: SatInfo; aos: number } | null = null;
  for (const sat of sats) {
    if (!orbIsBird(sat)) continue;
    const p = sat.passes.find((q) => q.los > nowSec);
    if (!p) continue;
    if (!best || p.aos < best.aos || (p.aos === best.aos && sat.name.localeCompare(best.sat.name) < 0)) best = { sat, aos: p.aos };
  }
  if (!best || best.sat.norad === pickNorad) return pick;
  return { norad: best.sat.norad, why: 'next' };
}
/** The box's words for a follow: "following ORBCOMM FM112 — now decoding" / "… — next to rise"; '' for the pick itself. */
export function orbFollowText(f: OrbFollow | null | undefined, name: string | undefined): string {
  if (!f || f.why === 'pick') return '';
  const n = name || `NORAD ${f.norad}`;
  return f.why === 'decoding' ? `following ${n} — now decoding` : `following ${n} — next to rise`;
}
