/**
 * ncdxf.ts — V12 2026-11-16 the NCDXF / IARU beacon tracker's PURE half, shared by the app and the web client
 * (web/client/src/main.ts imports it): the server's report, cut to its fields; the propagation grid as rows; the words.
 * The tracking itself is the server's (android/app/src/main/cpp/decoders/ncdxf_tracker.h, vibe_ncdxf_run.h;
 * docs/v12/NCDXF-BEACONS.md).
 *
 * Wire (decoder socket): {"type":"ncdxf","data":{now,band,tone,slot:{beacon,at},last:{…}|null,beacons:[[call,where,lat,lon]…],
 *                         bands:[Hz…],cells:[[beacon,band,heardAgo,steps,snr,id,triedAgo,best,bestAgo]…]}}
 *   + the text lines ("08:05:10z  14.100  OH2B  Lohja, Finland · 4 steps (down to 0.1 W) · 19 dB · ID ✓") and state.
 * ★★ THE GRID: one row per beacon in the published order, one column per band (20 17 15 12 10 m); a cell says how far
 *    down the four power steps it was heard (100 W · 10 W · 1 W · 0.1 W) and how long ago; blank = not tried yet, "—" =
 *    tried and not heard. A cell heard more than an hour ago is faded: propagation moves.
 */

export interface NcdxfCell { beacon: number; band: number; heardAgo: number; steps: number; snr: number; id: number; triedAgo: number; best: number; bestAgo: number }
export interface NcdxfLast { beacon: number; band: number; at: number; steps: number; snr: number[]; id: number; other: number; lag: number; off: number }
export interface NcdxfReport {
  now: number; band: number; tone: number; slot: { beacon: number; at: number };
  last: NcdxfLast | null; beacons: { call: string; where: string; lat: number; lon: number }[]; bands: number[]; cells: NcdxfCell[];
}

/** The five beacon frequencies (USB / CW dial), as the server's kBandHz. */
export const NCDXF_BANDS: readonly { hz: number; label: string }[] = [
  { hz: 14_100_000, label: '20 m' }, { hz: 18_110_000, label: '17 m' }, { hz: 21_150_000, label: '15 m' },
  { hz: 24_930_000, label: '12 m' }, { hz: 28_200_000, label: '10 m' },
];
export const NCDXF_STEPS = ['100 W', '10 W', '1 W', '0.1 W'];
/** ★ The words under the key: where to tune, and what the grid means. */
export const NCDXF_HINT = 'Tune 14.100, 18.110, 21.150, 24.930 or 28.200 MHz in CW or USB — or press a band. Eighteen beacons round '
  + 'the world take turns, ten seconds each, on a three-minute cycle locked to UTC: a callsign, then four dashes at 100 W, 10 W, 1 W and '
  + '0.1 W. The grid shows who is heard on each band and how far down the steps. It fills in as you visit the bands.';

const num = (v: unknown, d = 0) => (typeof v === 'number' && Number.isFinite(v) ? v : d);
const str = (v: unknown, n: number) => (typeof v === 'string' ? v.slice(0, n) : '');

/** ★ What a report may carry, cut to its fields (a broken server must not put an object where a number goes). */
export function ncdxfSanitise(d: any): NcdxfReport | null {
  if (!d || typeof d !== 'object') return null;
  const beacons = Array.isArray(d.beacons) ? d.beacons.slice(0, 18).map((b: any) => ({
    call: str(Array.isArray(b) ? b[0] : '', 10), where: str(Array.isArray(b) ? b[1] : '', 40),
    lat: num(Array.isArray(b) ? b[2] : 0), lon: num(Array.isArray(b) ? b[3] : 0) })) : [];
  if (beacons.length !== 18) return null;
  const cells: NcdxfCell[] = [];
  if (Array.isArray(d.cells)) for (const c of d.cells.slice(0, 90)) {
    if (!Array.isArray(c)) continue;
    const beacon = num(c[0], -1), band = num(c[1], -1);
    if (beacon < 0 || beacon > 17 || band < 0 || band > 4 || beacon % 1 || band % 1) continue;
    cells.push({ beacon, band, heardAgo: num(c[2], -1), steps: Math.max(0, Math.min(4, Math.round(num(c[3])))), snr: num(c[4], -99),
                 id: Math.sign(num(c[5])), triedAgo: num(c[6], -1), best: Math.max(0, Math.min(4, Math.round(num(c[7])))), bestAgo: num(c[8], -1) });
  }
  const l = d.last;
  const last: NcdxfLast | null = l && typeof l === 'object' && num(l.beacon, -1) >= 0 && num(l.beacon, -1) <= 17 ? {
    beacon: num(l.beacon), band: num(l.band, -1), at: num(l.at), steps: Math.max(0, Math.min(4, Math.round(num(l.steps)))),
    snr: Array.isArray(l.snr) ? l.snr.slice(0, 4).map((x: unknown) => num(x, -99)) : [], id: Math.sign(num(l.id)), other: num(l.other, -1),
    lag: num(l.lag), off: num(l.off) } : null;
  const band = num(d.band, -1);
  return { now: num(d.now), band: band >= 0 && band <= 4 ? band : -1, tone: num(d.tone), slot: { beacon: num(d.slot?.beacon, -1), at: num(d.slot?.at) },
           last, beacons, bands: NCDXF_BANDS.map((b) => b.hz), cells };
}

function ago(s: number): string {
  const v = Math.max(0, Math.round(s));
  return v < 60 ? `${v} s` : v < 3600 ? `${Math.round(v / 60)} min` : `${Math.floor(v / 3600)} h`;
}
/** One grid cell's words: '' not tried · '—' not heard · '4' … '1' steps heard (and a fade past an hour). */
export interface NcdxfCellView { text: string; title: string; steps: number; faded: boolean; now: boolean }
export function ncdxfCellView(r: NcdxfReport, beacon: number, band: number): NcdxfCellView {
  const c = r.cells.find((x) => x.beacon === beacon && x.band === band);
  const isNow = r.band === band && r.slot.beacon === beacon;
  if (!c) return { text: '', title: isNow ? 'on the air now' : 'not tried yet', steps: 0, faded: false, now: isNow };
  if (c.heardAgo < 0) return { text: '—', title: `not heard (tried ${ago(c.triedAgo)} ago)`, steps: 0, faded: false, now: isNow };
  const faded = c.heardAgo > 3600;
  return { text: String(c.steps), steps: c.steps, faded, now: isNow,
           title: `heard ${ago(c.heardAgo)} ago, down to ${NCDXF_STEPS[c.steps - 1]} · ${Math.round(c.snr)} dB${c.id === 1 ? ' · callsign checked' : ''}`
             + (c.best > c.steps && c.bestAgo >= 0 ? ` · best in the hour: down to ${NCDXF_STEPS[c.best - 1]}` : '') };
}
/** ★ The grid as rows (the published order), each with its five cells. */
export function ncdxfRows(r: NcdxfReport): { call: string; where: string; cells: NcdxfCellView[] }[] {
  return r.beacons.map((b, i) => ({ call: b.call, where: b.where, cells: [0, 1, 2, 3, 4].map((k) => ncdxfCellView(r, i, k)) }));
}
/** The status line: who is on now, and the last slot's verdict. */
export function ncdxfStatus(r: NcdxfReport | null): string {
  if (!r) return 'waiting for the beacons…';
  if (r.band < 0) return 'not on a beacon frequency — tune 14.100, 18.110, 21.150, 24.930 or 28.200 MHz in CW or USB';
  const now = r.beacons[r.slot.beacon];
  let s = `${NCDXF_BANDS[r.band].label}: ${now ? now.call + ' (' + now.where + ')' : '…'} on now`;
  const l = r.last;
  if (l && r.beacons[l.beacon]) {
    const who = r.beacons[l.beacon].call;
    if (l.id === -1 && r.beacons[l.other]) s += ` · last slot sounded like ${r.beacons[l.other].call}, not ${who} — check the receiver's clock`;
    else if (l.steps > 0) s += ` · ${who} heard down to ${NCDXF_STEPS[l.steps - 1]}${l.id === 1 ? ' ✓' : ''}`;
    else s += ` · ${who} not heard`;
    if (l.steps > 0 && Math.abs(l.lag) >= 0.5) s += ` · ${l.lag > 0 ? 'late' : 'early'} by ${Math.abs(l.lag).toFixed(1)} s`;
  }
  return s;
}
/** The cell colour by steps heard (green 4 … amber 1), the same scale both clients draw. */
export function ncdxfStepColour(steps: number): string {
  return ['transparent', '#E8C547', '#B8D84A', '#7EDB5A', '#3FD86B'][Math.max(0, Math.min(4, steps))];
}
