// ism.ts — VibeServer's ISM mode as the app reads it (V12, docs/v12/ISM-DECODER.md): the capability, the report, the
// readable line in the listener's units, the list's filter and its text.
//
// ★★ ONE RULE, TWO READERS: web/client/src/ism.ts is the web client's (ismSanitise, ismLine, ismAgo, ismStatus and the
//    list's filter there) — this is the same, word for word, and scripts/test_ism_app.ts runs both on the same reports.
//    (Not imported from the web: that module also carries its DOM box.)
// ★★★ PRIVACY IS THE SERVER'S (vibe_ism_service.h): a TPMS, a key fob or a fixed-code remote arrives already as "TPMS #2" —
//     no ID, no code. Nothing here could show one; nothing here tries.
// ★ PURE: no React Native — node runs it.

export interface IsmDevice {
  key: string; kind: string; make: string; model: string; name: string; line: string;
  nick?: number; id?: string; ch?: number;
  temp?: number; hum?: number; wind?: number; gust?: number; dir?: number; rain?: number; rainRate?: number;
  kpa?: number; uv?: number; watts?: number; baro?: number; batt?: number; button?: string; note?: string; extra?: string;
  freq: number; level: number; snr: number; seen: number; age: number; count: number;
  /** ★ V12 Wireless M-Bus meters (kind 'meter', docs/v12/WMBUS-DECODER.md §4.5). The serial is `id`. Never a byte of an
   *  encrypted payload — only how long it was. */
  mfr?: string; wm?: string; ver?: number; devType?: number; sec?: number; secText?: string; encBlocks?: number; encLen?: number;
  acc?: number; every?: number; rx?: number; sts?: number; via?: string; vol?: number; energy?: number; flow?: number;
  damaged?: number; auth?: number; records?: IsmRecord[];
}
export interface IsmRecord { q: string; v?: number; u?: string; t?: string; st: number; fn: string; tariff?: number; qual?: number }
export interface IsmBand { id: string; name: string; hz: number }
export interface IsmReport {
  type: 'ism'; band?: string; bandName?: string; centre: number; custom?: number; rate?: number; now?: number;
  bursts?: number; decoded?: number; unknown?: number; noise?: number; dropped?: number; capture?: number;
  gain?: number; gainAuto?: number;
  /** ★ V12 meters (868 band only): how many are listed, frames whose later block failed, collectors' frames, and
   *  wmGap = 1 when the capture cannot hold 868.95 MHz's meters (0.9 MS/s). */
  meters?: number; damaged?: number; collector?: number; wmGap?: number;
  devices: IsmDevice[];
}
export interface IsmUnits { mph: boolean; pressure: 'psi' | 'kpa' | 'bar' }
export const ISM_UNITS_DEFAULT: IsmUnits = { mph: false, pressure: 'psi' };

/** What a radio's /vibeserver.json says about ISM. `undefined` = no control at all (an HF+ reaches no ISM band, or an
 *  older server); `ok: false` = drawn greyed with `why` on a press; `bands` = the bands THIS radio reaches, in step order. */
export interface IsmCap { ok: boolean; why: string; bands: IsmBand[]; landing: string }
export function parseIsmCapability(j: unknown): IsmCap | undefined {
  if (!j || typeof j !== 'object') return undefined;
  const o = j as Record<string, unknown>;
  const bands: IsmBand[] = Array.isArray(o.ismBands)
    ? (o.ismBands as unknown[]).filter((b: any) => b && typeof b.id === 'string' && typeof b.name === 'string' && typeof b.hz === 'number')
        .slice(0, 8).map((b: any) => ({ id: String(b.id).slice(0, 8), name: String(b.name).slice(0, 40), hz: Number(b.hz) }))
    : [];
  if ((o.ism !== true && o.ism !== false) || !bands.length) return undefined;
  const why = typeof o.ismWhy === 'string' ? o.ismWhy.trim().slice(0, 200) : '';
  return { ok: o.ism === true, why: o.ism === true ? '' : (why || 'ISM is not available on this receiver'), bands,
           landing: typeof o.ismLanding === 'string' ? o.ismLanding.slice(0, 8) : bands[0].id };
}

/** The bands either side of the one the report names — the arrows step to them; null at an end (the server clamps). */
export function ismNeighbours(cap: IsmCap | undefined | null, r: IsmReport | null): { prev: IsmBand | null; next: IsmBand | null } {
  const bands = cap?.bands ?? [];
  const i = bands.findIndex((b) => b.id === r?.band);
  if (i < 0) return { prev: null, next: null };
  return { prev: i > 0 ? bands[i - 1] : null, next: i < bands.length - 1 ? bands[i + 1] : null };
}

/** The kinds the filter offers, in this order (Stuart's list). */
export const ISM_KINDS: Array<{ id: string; label: string }> = [
  { id: 'all', label: 'ALL' }, { id: 'weather', label: 'WEATHER' }, { id: 'tpms', label: 'TPMS' },
  { id: 'sensor', label: 'SENSOR' }, { id: 'doorbell', label: 'DOORBELL' }, { id: 'energy', label: 'ENERGY' },
  { id: 'keyfob', label: 'KEY FOB' }, { id: 'meter', label: 'METER' },
];

/** A report from the wire, sanitised: only plain fields of the types we read, at most 300 devices. null = not one. */
export function ismSanitise(msg: any): IsmReport | null {
  if (!msg || msg.type !== 'ism') return null;
  const num = (v: any) => (typeof v === 'number' && Number.isFinite(v) ? v : undefined);
  const str = (v: any, n = 120) => (typeof v === 'string' ? v.slice(0, n) : undefined);
  const devices: IsmDevice[] = [];
  // ★ 800: the server keeps 300 sensors AND its own 500 meters (vibe_ism_service.h Tracker::kMaxMeters)
  for (const d of Array.isArray(msg.devices) ? msg.devices.slice(0, 800) : []) {
    if (!d || typeof d !== 'object' || typeof d.line !== 'string') continue;
    devices.push({
      key: str(d.key, 24) ?? '', kind: str(d.kind, 16) ?? 'sensor', make: str(d.make, 40) ?? '', model: str(d.model, 60) ?? '',
      name: str(d.name, 80) ?? '', line: str(d.line, 400) ?? '', nick: num(d.nick), id: str(d.id, 16), ch: num(d.ch),
      temp: num(d.temp), hum: num(d.hum), wind: num(d.wind), gust: num(d.gust), dir: num(d.dir), rain: num(d.rain),
      rainRate: num(d.rainRate), kpa: num(d.kpa), uv: num(d.uv), watts: num(d.watts), baro: num(d.baro), batt: num(d.batt),
      button: str(d.button, 16), note: str(d.note, 40), extra: str(d.extra, d.kind === 'meter' ? 200 : 60),
      freq: num(d.freq) ?? 0, level: num(d.level) ?? -99, snr: num(d.snr) ?? 0, seen: num(d.seen) ?? 0, age: num(d.age) ?? 0,
      count: num(d.count) ?? 1,
      ...(d.kind === 'meter' ? meterFields(d, num, str) : {}),
    });
  }
  return {
    type: 'ism', band: str(msg.band, 8), bandName: str(msg.bandName, 40), centre: num(msg.centre) ?? 0, custom: num(msg.custom),
    rate: num(msg.rate), now: num(msg.now), bursts: num(msg.bursts), decoded: num(msg.decoded), unknown: num(msg.unknown),
    noise: num(msg.noise), dropped: num(msg.dropped), capture: num(msg.capture), gain: num(msg.gain), gainAuto: num(msg.gainAuto),
    meters: num(msg.meters), damaged: num(msg.damaged), collector: num(msg.collector), wmGap: num(msg.wmGap),
    devices,
  };
}
/** A meter's own fields, sanitised. */
function meterFields(d: any, num: (v: any) => number | undefined, str: (v: any, n?: number) => string | undefined): Partial<IsmDevice> {
  const records: IsmRecord[] = [];
  for (const r of Array.isArray(d.records) ? d.records.slice(0, 16) : []) {
    if (!r || typeof r !== 'object' || typeof r.q !== 'string') continue;
    records.push({ q: r.q.slice(0, 40), v: num(r.v), u: str(r.u, 12), t: str(r.t, 24), st: num(r.st) ?? 0, fn: str(r.fn, 8) ?? 'inst',
                   tariff: num(r.tariff), qual: num(r.qual) });
  }
  return {
    mfr: str(d.mfr, 4), wm: str(d.wm, 6), ver: num(d.ver), devType: num(d.devType), sec: num(d.sec), secText: str(d.secText, 40),
    encBlocks: num(d.encBlocks), encLen: num(d.encLen), acc: num(d.acc), every: num(d.every), rx: num(d.rx), sts: num(d.sts),
    via: str(d.via, 24), vol: num(d.vol), energy: num(d.energy), flow: num(d.flow), damaged: num(d.damaged), auth: num(d.auth),
    records: records.length ? records : undefined,
  };
}

/** ★ A meter's detail (the hover / tap): the mode heard, the frequency, the signal, the reception, the access number, how
 *  long the encrypted part was, the route, and its readings. One string per line. */
export function ismMeterDetail(d: IsmDevice): string[] {
  const out: string[] = [];
  const head: string[] = [];
  if (d.wm) head.push(d.wm);
  head.push(`${(d.freq / 1e6).toFixed(3)} MHz`);
  head.push(`${Math.round(d.snr)} dB over the noise`);
  if (d.rx !== undefined) head.push(`heard ${Math.round(d.rx)} %`);
  if (d.acc !== undefined) head.push(`access number ${d.acc}`);
  out.push(head.join(' · '));
  const sec: string[] = [];
  if (d.secText) sec.push(d.secText);
  if (d.encBlocks) sec.push(`${d.encBlocks} ${d.encBlocks === 1 ? 'block' : 'blocks'}`);
  if (d.encLen && d.sec !== 0) sec.push(`${d.encLen} bytes`);
  if (d.auth) sec.push('authenticated');
  if (d.via) sec.push(`via ${d.via}`);
  if (d.damaged) sec.push('last frame damaged');
  if (sec.length) out.push(sec.join(' · '));
  for (const r of d.records ?? []) {
    const val = r.t ?? (r.v !== undefined ? `${r.v}${r.u ? ' ' + r.u : ''}` : '—');
    const when = r.st ? ` (stored ${r.st})` : '';
    const fn = r.fn && r.fn !== 'inst' ? ` ${r.fn}` : '';
    out.push(`${r.q}${fn}: ${val}${when}${r.qual ? ' (qualified)' : ''}`);
  }
  return out;
}

const COMPASS = ['N', 'NNE', 'NE', 'ENE', 'E', 'ESE', 'SE', 'SSE', 'S', 'SSW', 'SW', 'WSW', 'W', 'WNW', 'NW', 'NNW'];
export function compass(deg: number): string { return COMPASS[Math.floor((((deg % 360) + 360 + 11.25) % 360) / 22.5) & 15]; }

/** The readable line in the listener's units — the server's readable() order exactly. With the defaults (km/h, PSI) it is
 *  the server's own line, which is used as sent. */
export function ismLine(d: IsmDevice, u: IsmUnits): string {
  if (!u.mph && u.pressure === 'psi') return d.line;
  if (d.kind === 'meter') return d.line;   // ★ a meter's line has no unit choice (m³, kWh)
  const secret = d.kind === 'tpms' || d.kind === 'keyfob' || d.kind === 'doorbell' || d.kind === 'suspectfob';
  const p: string[] = [d.name];
  if (secret && d.kind === 'tpms' && d.model) p.push(d.model);
  if (secret && d.kind === 'keyfob' && d.make) p.push(d.make);
  if (d.kind === 'tpms' && d.kpa !== undefined)
    p.push(u.pressure === 'kpa' ? `${Math.round(d.kpa)} kPa` : u.pressure === 'bar' ? `${(d.kpa / 100).toFixed(2)} bar` : `${Math.round(d.kpa * 0.1450377)} PSI`);
  if (d.temp !== undefined) p.push(`${d.kind === 'tpms' ? Math.round(d.temp) : d.temp.toFixed(1)} °C`);
  if (d.hum !== undefined) p.push(`${Math.round(d.hum)} % RH`);
  const sp = (kmh: number) => (u.mph ? `${Math.round(kmh / 1.609344)} mph` : `${Math.round(kmh)} km/h`);
  if (d.wind !== undefined) p.push(`wind ${sp(d.wind)}${d.dir !== undefined ? ' ' + compass(d.dir) : ''}`);
  if (d.gust !== undefined) p.push(`gust ${sp(d.gust)}`);
  if (d.rain !== undefined) p.push(`rain ${d.rain.toFixed(1)} mm`);
  if (d.rainRate !== undefined && d.rainRate > 0) p.push(`${d.rainRate.toFixed(1)} mm/h`);
  if (d.uv !== undefined) p.push(`UV ${Math.round(d.uv)}`);
  if (d.baro !== undefined) p.push(`${Math.round(d.baro)} hPa`);
  if (d.watts !== undefined) p.push(d.watts >= 1000 ? `${(d.watts / 1000).toFixed(2)} kW` : `${Math.round(d.watts)} W`);
  if (d.kind === 'keyfob' || d.kind === 'doorbell' || d.kind === 'suspectfob') p.push(d.button ? `button ${d.button}` : d.note === 'learn' ? 'learn' : 'button press');
  if (d.extra) p.push(d.extra);
  if (d.batt === 1) p.push('battery OK'); else if (d.batt === 0) p.push('battery low');
  if (!secret && d.ch !== undefined) p.push(`ch ${d.ch}`);
  return p.join(' · ');
}

/** "now", "12 s", "3 min", "1 h 5 min" — how long since the device was last heard. */
export function ismAgo(sec: number): string {
  if (!(sec >= 0) || sec < 2) return 'now';
  if (sec < 60) return `${Math.round(sec)} s`;
  if (sec < 3600) return `${Math.round(sec / 60)} min`;
  const h = Math.floor(sec / 3600), m = Math.round((sec % 3600) / 60);
  return m ? `${h} h ${m} min` : `${h} h`;
}

/** The band's name, or the exact frequency when the capture was put on one ("433.420 MHz"). */
export function ismBandText(r: IsmReport): string {
  return r.custom ? `${(r.centre / 1e6).toFixed(3)} MHz` : (r.bandName || 'ISM');
}

/** The status line: "433.92 MHz ISM · 6 devices · 41 bursts (3 unidentified) · gain 33.8 dB auto · capture 100 %". */
export function ismStatus(r: IsmReport): string {
  const parts: string[] = [];
  parts.push(ismBandText(r));
  parts.push(`${r.devices.length} ${r.devices.length === 1 ? 'device' : 'devices'}`);
  if (r.meters) parts.push(`${r.meters} ${r.meters === 1 ? 'meter' : 'meters'}`);
  if (r.wmGap === 1 && r.rate) parts.push(`868.95 not covered at ${(r.rate / 1e6).toFixed(r.rate % 1e6 ? 1 : 0)} MS/s`);
  if (r.bursts !== undefined) parts.push(`${r.bursts} bursts${r.unknown ? ` (${r.unknown} unidentified)` : ''}`);
  /* ★ THE GAIN ISM IS ACTUALLY USING (Stuart, 2026-10-25) — ISM sets the radio's gain itself and the dongle AGC off, so the
   *  ordinary gain readout has nothing true to say; the report's `gain` is the figure. */
  if (r.gain !== undefined && r.gain >= 0) parts.push(`gain ${r.gain.toFixed(1)} dB${r.gainAuto === 1 ? ' auto' : ''}`);
  if (r.capture !== undefined && r.capture >= 0) parts.push(`capture ${r.capture} %`);
  return parts.join(' · ');
}

/** The list: newest first (the server's order), filtered by kind. ★ KEY FOB shows the suspected ones too (FobWatch): a car
 *  key we cannot name is still a key fob. */
export function ismRows(r: IsmReport | null, filter: string): IsmDevice[] {
  if (!r) return [];
  return r.devices.filter((d) => filter === 'all' || d.kind === filter || (filter === 'keyfob' && d.kind === 'suspectfob'));
}
/** What an empty list says (the web's ismListHtml). */
export function ismEmptyText(filter: string): string {
  if (filter === 'meter') return 'No meters heard yet — they send every 16 s to a few minutes, on the 868 MHz band only.';
  return `${filter === 'all' ? 'No sensors heard yet' : 'Nothing of that kind heard yet'} — most send every 30–60 s; tyres only while the car moves.`;
}

/** The listener's choices, from storage — the web's loadPrefs (filter, wind units, tyre pressure units). */
export function parseIsmPrefs(s: string | null | undefined): { filter: string; units: IsmUnits } {
  try {
    const j = JSON.parse(s || '{}');
    const filter = ISM_KINDS.some((k) => k.id === j.filter) ? j.filter : 'all';
    const pressure = j.pressure === 'kpa' || j.pressure === 'bar' ? j.pressure : 'psi';
    return { filter, units: { mph: j.mph === true, pressure } };
  } catch { return { filter: 'all', units: { ...ISM_UNITS_DEFAULT } }; }
}
export function nextPressure(p: IsmUnits['pressure']): IsmUnits['pressure'] { return p === 'psi' ? 'kpa' : p === 'kpa' ? 'bar' : 'psi'; }

/** ★★ THE ISM BOX BY PAD AND KEYBOARD (the DAB box's pattern — controller audit group B): the stops the arrows walk, in
 *  the order drawn — the header's keys (◀ ▶ only while ISM runs, BIG where it is offered, EXIT/CLOSE), then the chips
 *  (every kind, then the two unit keys), then each device row (reveal-only: a row is read, not pressed). `row` is the
 *  visual row the up/down arrows move between: 0 the header, 1 the chips, 2… the list. ◀ / ▶ are stops even at an end
 *  (pressing one there does nothing, as the touch key does), so the order never shifts under the highlight. */
export type IsmNavKey = 'down' | 'up' | 'big' | 'exit' | `k:${string}` | 'u:speed' | 'u:pressure' | `r:${number}`;
export function ismNavKeys(o: { on: boolean; big: boolean; chips: boolean; rows: number }): Array<{ key: IsmNavKey; row: number }> {
  const out: Array<{ key: IsmNavKey; row: number }> = [];
  if (o.on) { out.push({ key: 'down', row: 0 }, { key: 'up', row: 0 }); }
  if (o.big) out.push({ key: 'big', row: 0 });
  out.push({ key: 'exit', row: 0 });
  if (o.chips) {
    for (const k of ISM_KINDS) out.push({ key: `k:${k.id}`, row: 1 });
    out.push({ key: 'u:speed', row: 1 }, { key: 'u:pressure', row: 1 });
  }
  for (let i = 0; i < o.rows; i++) out.push({ key: `r:${i}`, row: 2 + i });
  return out;
}
