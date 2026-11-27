/**
 * mapWfModel.ts — ★★★ V12 WHAT THE MAP DRAWER'S LIVE WATERFALL SHOWS (Stuart, 2026-11-27): the span, the markers and the
 * ident stamps for what the radio is doing now. One rule for both clients (the web's web/client/src/mapWf.ts draws it;
 * the app's port reads the same). Pure — no DOM — so scripts/test_map_wf.ts proves it.
 *
 *  • a single channel: a modest slice centred on the dial, ~5× the filter wide (12 kHz … 1.2 MHz), one marker + passband
 *  • a multichannel decoder (VDL2, ACARS): every channel the SERVER's table lists (hwinfo avdataBands[].chans) plus a
 *    margin, one labelled marker each ("VDL2 Ch1 136.700") — never a frequency of our own
 *  • a scan (the VOR / ILS map): the radio's WHOLE window for this dwell, fresh at each step (`key` changes → the history
 *    breaks; Stuart reversed a band-wide composite — a 2 MHz step only leaves narrow smears); the stations heard in it,
 *    and each DECODED ident stamped into the history as it is read (never a listed ident: the list is never the judge)
 *  • NDB: the band the radio holds, each beacon whose ident was decoded on air, stamped likewise
 */

export interface MapWfMark {
  hz: number;
  /** "VDL2 Ch1 136.700", "DTY 116.40", "NFM 145.500"; '' = a faint line only */
  label: string;
  /** bright (the channel being dwelt on / tuned now) or faint */
  active?: boolean;
  /** the passband, shaded (single-channel modes) */
  loHz?: number;
  hiHz?: number;
}
export interface MapWfModel {
  lo: number;
  hi: number;
  marks: MapWfMark[];
  /** changes when a scan retunes (its window): the history breaks */
  key: string;
  /** the drawer's bottom bar in words */
  info: string;
  /** decoded idents to stamp into the history now, at their frequency */
  stamps: Array<{ hz: number; text: string }>;
}
/** A station's last ident reading, so only a NEW reading is stamped. Kept by the caller, fresh each time the view opens. */
export type WfSeen = Map<string, string>;

const mhz3 = (hz: number) => (hz / 1e6).toFixed(3);

function noter(seen: WfSeen, prime: boolean, stamps: MapWfModel['stamps']) {
  return (key: string, sig: string, hz: number, text: string) => {
    if (seen.get(key) === sig) return;
    seen.set(key, sig);
    if (!prime && text) stamps.push({ hz, text });   // ★ prime: what was read before the view opened is not "now"
  };
}

export interface WfVorBeacon { hz: number; ident: string; reads: number; votes: number; last: number; fresh: boolean }
export interface WfVorReport { now: number; window: number; windows: number; wins: Array<[number, number]>; wlo: number; whi: number; dwell: number; beacons: WfVorBeacon[] }
export function wfModelVor(r: WfVorReport, seen: WfSeen, prime: boolean): MapWfModel {
  const stamps: MapWfModel['stamps'] = [], note = noter(seen, prime, stamps);
  const w = r.wins[r.window];
  const lo = w ? w[0] : r.wlo, hi = w ? w[1] : r.whi;
  const marks: MapWfMark[] = [];
  for (const b of r.beacons) {
    if (b.hz < lo || b.hz > hi) continue;
    marks.push({ hz: b.hz, label: b.ident ? `${b.ident} ${(b.hz / 1e6).toFixed(2)}` : '', active: b.fresh && r.now - b.last < Math.max(5, r.dwell) });
    if (b.ident) note(`vor${b.hz}`, `${b.ident}|${b.reads}|${b.votes}|${b.last}`, b.hz, b.ident);
  }
  return { lo, hi, marks, key: `vor${r.window}|${lo}`, stamps,
           info: `VOR / ILS · window ${r.window + 1} of ${r.windows || r.wins.length || 1} · ${(lo / 1e6).toFixed(2)}–${(hi / 1e6).toFixed(2)} MHz` };
}

export interface WfNdbBeacon { id: number; hz: number; ident: string; active: boolean; lastId: number }
export interface WfNdbReport { centre: number; lo: number; hi: number; identified: number; beacons: WfNdbBeacon[] }
export function wfModelNdb(r: WfNdbReport, seen: WfSeen, prime: boolean): MapWfModel {
  const stamps: MapWfModel['stamps'] = [], note = noter(seen, prime, stamps);
  const marks: MapWfMark[] = [];
  for (const b of r.beacons) {
    marks.push({ hz: b.hz, label: b.ident ? `${b.ident} ${(b.hz / 1e3).toFixed(1)}` : '', active: b.active });
    if (b.ident) note(`ndb${b.id}`, `${b.ident}|${b.lastId}`, b.hz, b.ident);
  }
  return { lo: r.lo, hi: r.hi, marks, key: `ndb${Math.round(r.centre)}`, stamps,
           info: `NDB · ${(r.lo / 1e3).toFixed(0)}–${(r.hi / 1e3).toFixed(0)} kHz · ${r.identified} identified` };
}

/** The datalink band (hwinfo avdataBands[]): null when the server lists no channels (an older server) — the dial's slice then. */
export function wfModelChannels(band: { id: string; name: string; chans?: number[] }): MapWfModel | null {
  const ch = (band.chans ?? []).filter((c) => c > 0).sort((p, q) => p - q);
  if (!ch.length) return null;
  const a = ch[0], z = ch[ch.length - 1], pad = Math.max(25e3, (z - a) * 0.08);
  // ★ the combined band holds both sets: each channel named by its own kind (VDL2 lives at 136 MHz and up)
  const combined = /\+|&/.test(band.name) || band.id === 'both';
  const kind = (hz: number) => (combined ? (hz >= 136e6 ? 'VDL2' : 'ACARS') : band.name);
  const n = new Map<string, number>();
  const marks = ch.map((hz) => { const k = kind(hz), i = (n.get(k) ?? 0) + 1; n.set(k, i); return { hz, label: `${k} Ch${i} ${mhz3(hz)}`, active: true }; });
  return { lo: a - pad, hi: z + pad, marks, key: `ch${band.id}`, stamps: [],
           info: `${band.name} · ${ch.length} channel${ch.length === 1 ? '' : 's'} · ${mhz3(a)}–${mhz3(z)} MHz` };
}

export const WF_SPAN_MIN = 12e3, WF_SPAN_MAX = 1.2e6, WF_SPAN_X = 5;
export function wfModelVfo(v: { frequency: number; mode: string; bandwidthLow: number; bandwidthHigh: number }): MapWfModel {
  const pbLo = v.frequency + v.bandwidthLow, pbHi = v.frequency + v.bandwidthHigh;
  const bw = Math.max(500, pbHi - pbLo);
  const span = Math.max(WF_SPAN_MIN, Math.min(WF_SPAN_MAX, bw * WF_SPAN_X));
  const mode = String(v.mode || '').toUpperCase();
  return { lo: v.frequency - span / 2, hi: v.frequency + span / 2, key: 'vfo', stamps: [],
           marks: [{ hz: v.frequency, label: `${mode} ${mhz3(v.frequency)}`, active: true, loHz: pbLo, hiHz: pbHi }],
           info: `${mode} ${mhz3(v.frequency)} MHz · ${span >= 1e6 ? (span / 1e6).toFixed(1) + ' MHz' : Math.round(span / 1e3) + ' kHz'} shown` };
}
