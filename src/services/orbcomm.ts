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
export interface OrbCarrier { id: number; rf: number; snr: number; lock: boolean; ok: number; bad: number; fixed: number }
export interface OrbStatus {
  t: number; good: number; bad: number; messages: number; dropped: number; maxChans: number; located: boolean; birds: number;
  counts: Record<string, number>; sats: OrbSat[]; carriers: OrbCarrier[];
  /** the SATELLITE pick's NORAD (0 none) and whether it is up (1), not (0), or there is no pick (−1) */
  pick: number; up: number;
  /** the sky dots new since the last status */
  dots: SkyDot[];
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
      carriers.push({ id: Math.floor(n(q.id)), rf: Math.max(0, n(q.rf)), snr: n(q.snr), lock: q.lock === true, ok: cnt(q.ok), bad: cnt(q.bad), fixed: cnt(q.fixed) });
    }
  const up = Math.floor(n(o.up, -1));
  return {
    t: Math.max(0, n(o.t)), good: cnt(o.good), bad: cnt(o.bad), messages: cnt(o.messages), dropped: cnt(o.dropped),
    maxChans: cnt(o.maxChans), located: o.located === true, birds: cnt(o.birds), counts, sats, carriers,
    pick: cnt(o.pick), up: up === 1 || up === 0 ? up : -1, dots: parseSkyDotsLive(o.dots),
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
}
export const ORB_PKTS_MAX = 40;
export const orbViewEmpty = (): OrbView => ({ status: null, pkts: [], clockOff: {} });
/** A status or a packet folded into the box's state (a status keeps the packets; a packet keeps the status). */
export function orbViewMerge(prev: OrbView | null, m: { status?: OrbStatus | null; pkt?: OrbPkt | null }): OrbView {
  const v = prev ?? orbViewEmpty();
  let next = v;
  if (m.status) next = { ...next, status: m.status };
  if (m.pkt) {
    const pkts = [m.pkt, ...next.pkts];
    if (pkts.length > ORB_PKTS_MAX) pkts.length = ORB_PKTS_MAX;
    const clockOff = m.pkt.clockOff != null && m.pkt.norad ? { ...next.clockOff, [m.pkt.norad]: m.pkt.clockOff } : next.clockOff;
    next = { ...next, pkts, clockOff };
  }
  return next;
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
export function orbSatLines(b: OrbSat, clockOff?: number): string[] {
  const out: string[] = [];
  out.push(`${b.name || (b.norad ? `NORAD ${b.norad}` : 'Unknown ORBCOMM bird')} · ${orbHowText(b.how)}`);
  const sky = b.el > -90 && (b.az || b.el) ? ` · az ${Math.round(b.az)}° el ${Math.round(b.el)}°` : '';
  out.push(`SNR ${b.snr.toFixed(1)} dB · Doppler ${kHz(b.dop)}${b.pred ? ` (predicted ${kHz(b.pred)})` : ''}${sky}`);
  out.push(`${b.ok.toLocaleString('en-GB')} good packets${b.messages ? ` · ${b.messages.toLocaleString('en-GB')} subscriber messages (raw)` : ''}`
    + `${b.rf ? ` · ${mhz(b.rf)} MHz` : ''}${b.age >= 3 ? ` · last ${Math.round(b.age)} s ago` : ''}`);
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
