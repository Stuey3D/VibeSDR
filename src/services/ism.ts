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
  /** ★ V12 LoRa (kinds meshtastic / meshcore / lorawan / lora — docs/v12/LORA-MESH.md). Mesh nodes publish their names,
   *  hardware and positions; LoRaWAN is header only (`encLen` = how long the encrypted payload was, never its bytes). */
  node?: string; short?: string; long?: string; hw?: string; role?: string; chan?: string; net?: string; mtype?: string; what?: string;
  lat?: number; lon?: number; posAge?: number; prec?: number; alt?: number; battPct?: number; volt?: number; chUtil?: number;
  hops?: number; fcnt?: number; fport?: number; priv?: number; sf?: number; bw?: number; cr?: number; sync?: string;
  msgs?: IsmMsg[];
  /** ★ V12 the ISM map: 1 = the position is a person's, rounded to ~1 km by the server (vibe_lora_track.h publishedPos);
   *  `ntype` what the node is — repeater / room / sensor / companion. */
  approx?: number; ntype?: string;
}
/** A public mesh message (Meshtastic's default channel, MeshCore's Public channel — nothing else is ever sent). */
export interface IsmMsg { ago: number; from: string; text: string; net?: string; node?: string; ch?: string; freq?: number }
export interface IsmLora { chans: number; plan: number; dets: number; frames: number; badCrc: number; texts: number; mapped: number; dropped: number }
export interface IsmRecord { q: string; v?: number; u?: string; t?: string; st: number; fn: string; tariff?: number; qual?: number }
export interface IsmBand { id: string; name: string; hz: number }
export interface IsmReport {
  type: 'ism'; band?: string; bandName?: string; centre: number; custom?: number; rate?: number; now?: number;
  bursts?: number; decoded?: number; unknown?: number; noise?: number; dropped?: number; capture?: number;
  gain?: number; gainAuto?: number;
  /** ★ V12 meters (868 band only): how many are listed, frames whose later block failed, collectors' frames, and
   *  wmGap = 1 when the capture cannot hold 868.95 MHz's meters (0.9 MS/s). */
  meters?: number; damaged?: number; collector?: number; wmGap?: number;
  /** ★ V12 LoRa: the engine's status and the public messages, newest first (vibe_ism_service.h report()). */
  lora?: IsmLora; msgs?: IsmMsg[];
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
  // ★ V12 LoRa (868 / 433 / 915 bands): the mesh networks, LoRaWAN (with any LoRa frame we cannot name), the public messages
  { id: 'meshtastic', label: 'MESHTASTIC' }, { id: 'meshcore', label: 'MESHCORE' }, { id: 'lorawan', label: 'LORAWAN' },
  { id: 'msgs', label: 'MESSAGES' },
];
export const LORA_KINDS = ['meshtastic', 'meshcore', 'lorawan', 'lora'];

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
      ...(LORA_KINDS.includes(d.kind) ? loraFields(d, num, str) : {}),
    });
  }
  const msgsOf = (a: any): IsmMsg[] => (Array.isArray(a) ? a.slice(0, 60) : []).filter((m: any) => m && typeof m.text === 'string')
    .map((m: any) => ({ ago: num(m.ago) ?? 0, from: str(m.from, 40) ?? '', text: str(m.text, 240) ?? '', net: str(m.net, 12), node: str(m.node, 16), ch: str(m.ch, 20), freq: num(m.freq) }));
  const lo = msg.lora && typeof msg.lora === 'object' ? msg.lora : null;
  return {
    type: 'ism', band: str(msg.band, 8), bandName: str(msg.bandName, 40), centre: num(msg.centre) ?? 0, custom: num(msg.custom),
    rate: num(msg.rate), now: num(msg.now), bursts: num(msg.bursts), decoded: num(msg.decoded), unknown: num(msg.unknown),
    noise: num(msg.noise), dropped: num(msg.dropped), capture: num(msg.capture), gain: num(msg.gain), gainAuto: num(msg.gainAuto),
    meters: num(msg.meters), damaged: num(msg.damaged), collector: num(msg.collector), wmGap: num(msg.wmGap),
    lora: lo ? { chans: num(lo.chans) ?? 0, plan: num(lo.plan) ?? 0, dets: num(lo.dets) ?? 0, frames: num(lo.frames) ?? 0, badCrc: num(lo.badCrc) ?? 0,
                 texts: num(lo.texts) ?? 0, mapped: num(lo.mapped) ?? 0, dropped: num(lo.dropped) ?? 0 } : undefined,
    msgs: lo ? msgsOf(msg.msgs) : undefined,
    devices,
  };
}
/** ★ V12 LoRa: a mesh node's / LoRaWAN device's own fields, sanitised. A position must be a real one. */
function loraFields(d: any, num: (v: any) => number | undefined, str: (v: any, n?: number) => string | undefined): Partial<IsmDevice> {
  const lat = num(d.lat), lon = num(d.lon);
  const pos = lat !== undefined && lon !== undefined && Math.abs(lat) <= 90 && Math.abs(lon) <= 180 && !(lat === 0 && lon === 0);
  const msgs: IsmMsg[] = [];
  for (const m of Array.isArray(d.msgs) ? d.msgs.slice(0, 5) : []) if (m && typeof m.text === 'string') msgs.push({ ago: num(m.ago) ?? 0, from: str(m.from, 40) ?? '', text: str(m.text, 240) ?? '' });
  return {
    node: str(d.node, 16), short: str(d.short, 8), long: str(d.long, 40), hw: str(d.hw, 40), role: str(d.role, 20), chan: str(d.chan, 20),
    net: str(d.net, 30), mtype: str(d.mtype, 24), what: str(d.what, 24), lat: pos ? lat : undefined, lon: pos ? lon : undefined,
    posAge: num(d.posAge), prec: num(d.prec), alt: num(d.alt), battPct: num(d.battPct), volt: num(d.volt), chUtil: num(d.chUtil), hops: num(d.hops),
    fcnt: num(d.fcnt), fport: num(d.fport), encLen: num(d.encLen), priv: num(d.priv), sf: num(d.sf), bw: num(d.bw), cr: num(d.cr), sync: str(d.sync, 2),
    temp: num(d.temp), hum: num(d.hum), baro: num(d.baro), msgs: msgs.length ? msgs : undefined,
    approx: num(d.approx), ntype: NODE_TYPES.includes(d.ntype) ? d.ntype : undefined,
  };
}
export const NODE_TYPES = ['repeater', 'room', 'sensor', 'companion'];
/** ★ V12 LoRa: does this row go on the map? */
export function ismOnMap(d: IsmDevice): boolean { return d.lat !== undefined && d.lon !== undefined; }
/** The network's name as the list says it. */
export function ismNetName(kind: string): string { return kind === 'meshtastic' ? 'Meshtastic' : kind === 'meshcore' ? 'MeshCore' : kind === 'lorawan' ? 'LoRaWAN' : 'LoRa'; }

/** ★ A LoRa row's detail (the tap): the radio side, then what the node published, then its recent public messages. */
export function ismLoraDetail(d: IsmDevice, rx?: { lat: number; lon: number } | null, withMsgs = true): string[] {
  const out: string[] = [];
  const head: string[] = [`${(d.freq / 1e6).toFixed(3)} MHz`];
  if (d.sf) head.push(`SF${d.sf}`);
  if (d.bw) head.push(`${d.bw >= 1e5 ? Math.round(d.bw / 1e3) : (d.bw / 1e3).toFixed(1)} kHz`);
  if (d.cr) head.push(`CR 4/${4 + d.cr}`);
  head.push(`SNR ${d.snr.toFixed(1)} dB`);
  if (d.hops !== undefined) head.push(d.hops === 0 ? 'heard first-hand' : `${d.hops} ${d.hops === 1 ? 'hop' : 'hops'}`);
  out.push(head.join(' · '));
  const me: string[] = [];
  if (d.node) me.push(d.kind === 'lorawan' ? `DevAddr ${d.node}` : d.kind === 'meshcore' ? `key ${d.node}…` : d.node);
  if (d.hw) me.push(d.hw);
  if (d.role) me.push(d.role);
  if (d.chan) me.push(`channel ${d.chan}`);
  if (d.net) me.push(d.net);
  if (d.battPct !== undefined) me.push(d.battPct > 100 ? 'powered' : `battery ${d.battPct} %`);
  if (d.volt !== undefined) me.push(`${d.volt.toFixed(2)} V`);
  if (d.chUtil !== undefined) me.push(`channel use ${d.chUtil.toFixed(0)} %`);
  if (me.length) out.push(me.join(' · '));
  if (d.lat !== undefined && d.lon !== undefined) {
    let p = `${d.lat.toFixed(4)}, ${d.lon.toFixed(4)}`;
    if (d.alt !== undefined) p += ` · ${Math.round(d.alt)} m`;
    if (rx) { const k = rangeKm(rx, d.lat, d.lon); p += ` · ${k.km < 10 ? k.km.toFixed(1) : Math.round(k.km)} km ${compass(k.brg)}`; }
    if (d.approx) p += ' · approximate (~1 km): a companion node is a person';
    else if (d.prec && d.prec < 32) p += ' · position deliberately approximate';
    out.push(p);
  }
  if (d.kind === 'lorawan') out.push('Payload encrypted by the network — its length is shown, never its bytes.');
  if (d.priv && !d.msgs) out.push(d.kind === 'meshtastic' ? 'Heard on a private channel — only its header is shown.' : 'Encrypted traffic — only headers are shown.');
  if (withMsgs) for (const m of d.msgs ?? []) out.push(`${ismAgo(m.ago)} ago${m.from ? ' · ' + m.from : ''}: “${m.text}”`);
  return out;
}
/** Range and bearing from the receiver (great circle). */
export function rangeKm(rx: { lat: number; lon: number }, lat: number, lon: number): { km: number; brg: number } {
  const r = Math.PI / 180, p1 = rx.lat * r, p2 = lat * r, dl = (lon - rx.lon) * r;
  const a = Math.sin((p2 - p1) / 2) ** 2 + Math.cos(p1) * Math.cos(p2) * Math.sin(dl / 2) ** 2;
  const km = 12742 * Math.asin(Math.min(1, Math.sqrt(a)));
  const brg = (Math.atan2(Math.sin(dl) * Math.cos(p2), Math.cos(p1) * Math.sin(p2) - Math.sin(p1) * Math.cos(p2) * Math.cos(dl)) / r + 360) % 360;
  return { km, brg };
}
/** ★ V12 LoRa: the MESSAGES chip's lines, newest first — the web's ismMsgsHtml, as text. */
export function ismMsgLines(r: IsmReport | null): Array<{ ago: string; text: string }> {
  return (r?.msgs ?? []).map((x) => ({ ago: ismAgo(x.ago), text: `${ismNetName(x.net || '')}${x.ch ? ' ' + x.ch : ''} · ${x.from || x.node || '?'}: ${x.text}` }));
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
  if (d.kind === 'meter' || LORA_KINDS.includes(d.kind)) return d.line;   // ★ a LoRa row's line has no unit choice either   // ★ a meter's line has no unit choice (m³, kWh)
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

/** ★ V12: the report as it stands `sec` seconds after it arrived — every "last heard" aged by that much. A busy 868 band
 *  (many meters) is sent every 5 s rather than every second (local_sdr_shim.cpp ismTick), so the list ages itself. */
export function ismAged(r: IsmReport, sec: number): IsmReport {
  if (!(sec > 0)) return r;
  return { ...r, devices: r.devices.map((d) => ({ ...d, seen: d.seen + sec })) };
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
  // ★ V12 LoRa: frames decoded, and how many of the band's LoRa channels this capture holds
  if (r.lora) parts.push(`LoRa ${r.lora.frames} ${r.lora.frames === 1 ? 'frame' : 'frames'} · ${r.lora.chans} of ${r.lora.plan} channels`);
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
  if (filter === 'msgs') return [];   // ★ the MESSAGES chip lists messages (ismMsgLines), not devices
  return r.devices.filter((d) => filter === 'all' || d.kind === filter || (filter === 'keyfob' && d.kind === 'suspectfob')
                                  || (filter === 'lorawan' && d.kind === 'lora'));
}
/** What an empty list says (the web's ismListHtml). */
export function ismEmptyText(filter: string): string {
  if (filter === 'meter') return 'No meters heard yet — they send every 16 s to a few minutes, on the 868 MHz band only.';
  if (filter === 'meshtastic' || filter === 'meshcore') return 'No mesh nodes heard yet — most send their name every few hours and their position every 15 min to an hour.';
  if (filter === 'lorawan') return 'No LoRaWAN heard yet — devices send every few minutes to an hour. Payloads are encrypted; only headers are shown.';
  if (filter === 'msgs') return "No public messages yet — only the mesh networks’ public channels are shown; private channels and direct messages never are.";
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
 *  (every kind, then the two unit keys), then each device row (reveal-only: a row is read, not pressed — except a METER row, whose Enter opens its detail). `row` is the
 *  visual row the up/down arrows move between: 0 the header, 1 the chips, 2… the list. ◀ / ▶ are stops even at an end
 *  (pressing one there does nothing, as the touch key does), so the order never shifts under the highlight. */
export type IsmNavKey = 'down' | 'up' | 'big' | 'map' | 'exit' | `k:${string}` | 'u:speed' | 'u:pressure' | `r:${number}` | `p:${number}`;
/** ★ V12 `pos`: the rows (by index) whose line carries a position link — each gets a stop of its own after its row ('p:i',
 *  the same visual row), so a pad or keyboard reaches "show on the map" as a finger does. */
export function ismNavKeys(o: { on: boolean; big: boolean; chips: boolean; rows: number; map?: boolean; pos?: number[] }): Array<{ key: IsmNavKey; row: number }> {
  const out: Array<{ key: IsmNavKey; row: number }> = [];
  if (o.on) { out.push({ key: 'down', row: 0 }, { key: 'up', row: 0 }); }
  if (o.big) out.push({ key: 'big', row: 0 });
  if (o.map) out.push({ key: 'map', row: 0 });   // ★ V12: MAP, only while something has a position
  out.push({ key: 'exit', row: 0 });
  if (o.chips) {
    for (const k of ISM_KINDS) out.push({ key: `k:${k.id}`, row: 1 });
    out.push({ key: 'u:speed', row: 1 }, { key: 'u:pressure', row: 1 });
  }
  const pos = new Set(o.pos ?? []);
  for (let i = 0; i < o.rows; i++) { out.push({ key: `r:${i}`, row: 2 + i }); if (pos.has(i)) out.push({ key: `p:${i}`, row: 2 + i }); }
  return out;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  ★★ V12 THE ISM MAP (the box's MAP — the web's ismMapOpen): everything ISM lists with a position, on the shared track
//  layer (MapOverlay kind 'ism', TrackLiveList for the rows). Pins are TrackItems; the icon, label and card are the web's
//  (web/client/src/ism.ts — the same words and the same SVG).
//  ★★ 2026-11-01: an icon per node TYPE in the NETWORK's colour, the name always shown, a node fading as it ages, a
//     person's node where the server put it (~1 km) and labelled approximate.
//  ★★★ NO MESSAGE CONTENT ON THE MAP.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
export const MAP_COLOURS: Record<string, string> = { meshtastic: '#67ea94', meshcore: '#ff9f43', lorawan: '#7ab8ff', lora: '#b9a7ff' };
function escM(s: string): string { return s.replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c] as string)); }
export function ismNodeType(d: { ntype?: string; role?: string; kind: string }): string {
  if (d.ntype && NODE_TYPES.includes(d.ntype)) return d.ntype;
  const r = d.role || '';
  if (r === 'repeater' || r === 'router' || r === 'router-client' || r === 'router (late)') return 'repeater';
  if (r === 'room server') return 'room';
  if (r === 'sensor') return 'sensor';
  return 'companion';
}
export const NODE_TYPE_WORD: Record<string, string> = { repeater: 'repeater', room: 'room server', sensor: 'sensor', companion: 'companion' };
export function ismAgeOpacity(seen: number): number {
  if (!(seen > 900)) return 1;
  return Math.max(0.35, 1 - 0.65 * Math.min(1, (seen - 900) / (86400 - 900)));
}
const GLYPH: Record<string, string> = {
  repeater: '<path d="M11 6.5 L8 17 M11 6.5 L14 17 M9 13 H13" stroke="#111" stroke-width="1.6" fill="none" stroke-linecap="round"/>'
          + '<path d="M7.2 5.2 a5 5 0 0 0 0 5.6 M14.8 5.2 a5 5 0 0 1 0 5.6" stroke="#111" stroke-width="1.3" fill="none" stroke-linecap="round"/>',
  room: '<rect x="6.5" y="5.5" width="9" height="4.6" rx="1" fill="none" stroke="#111" stroke-width="1.5"/>'
      + '<rect x="6.5" y="11.6" width="9" height="4.6" rx="1" fill="none" stroke="#111" stroke-width="1.5"/>'
      + '<circle cx="13.3" cy="7.8" r=".9" fill="#111"/><circle cx="13.3" cy="13.9" r=".9" fill="#111"/>',
  sensor: '<path d="M11 5.5 V13" stroke="#111" stroke-width="2.6" stroke-linecap="round"/><circle cx="11" cy="15" r="2.6" fill="#111"/>',
  companion: '<circle cx="11" cy="7.6" r="2.6" fill="#111"/><path d="M6.6 16.6 a4.4 4.4 0 0 1 8.8 0 Z" fill="#111"/>',
};
export function ismMapIcon(d: IsmDevice): { html: string; w: number; h: number } {
  const c = MAP_COLOURS[d.kind] || '#ffb833';
  const t = ismNodeType(d);
  const op = ismAgeOpacity(d.seen).toFixed(2);
  const shape = t === 'companion'
    ? `<circle cx="11" cy="11" r="9.5" fill="${c}" stroke="#111" stroke-width="1.2"/>`
    : `<rect x="1.5" y="1.5" width="19" height="19" rx="4.5" fill="${c}" stroke="#111" stroke-width="1.2"/>`;
  const ring = d.approx ? `<circle cx="11" cy="11" r="10.6" fill="none" stroke="${c}" stroke-width="1" stroke-dasharray="2 2"/>` : '';
  return { html: `<svg width="24" height="24" viewBox="-1 -1 24 24" style="opacity:${op}" aria-hidden="true">${shape}${GLYPH[t] || GLYPH.companion}${ring}</svg>`, w: 24, h: 24 };
}
export function ismMapLabel(d: IsmDevice): string {
  const n = d.long || d.short || d.name || d.node || d.key;
  return d.approx ? `${n} (approximate)` : n;
}
export function ismMapCard(d: IsmDevice, rx: { lat: number; lon: number } | null): string {
  const lines = ismLoraDetail(d, rx, false);
  const t = NODE_TYPE_WORD[ismNodeType(d)];
  return `<div class="trkCard"><div style="white-space:normal;max-width:300px"><b>${escM(d.long || d.name)}</b>${d.short && d.short !== d.long ? ` (${escM(d.short)})` : ''}<br>`
    + `${escM(ismNetName(d.kind))} ${escM(t)} · heard ${escM(ismAgo(d.seen))}${d.seen >= 2 ? ' ago' : ''} · ${d.count} ${d.count === 1 ? 'packet' : 'packets'}<br>`
    + lines.map(escM).join('<br>') + '</div></div>';
}
export const ISM_MAP_EMPTY = 'Nothing heard with a position yet — mesh nodes send theirs every 15 min to an hour.';
/** The pins: every row with a position. ★ `_lb` is plain text — the layer escapes it. */
export function ismMapItems(r: IsmReport | null, rx: { lat: number; lon: number } | null): Array<{ key: string; lat: number; lon: number; _ic: { html: string; w: number; h: number }; _lb: string; _cd: string }> {
  if (!r) return [];
  return r.devices.filter(ismOnMap).map((d) => ({ key: d.key, lat: d.lat as number, lon: d.lon as number, _ic: ismMapIcon(d), _lb: ismMapLabel(d), _cd: ismMapCard(d, rx) }));
}
type MapRow = { key: string; colour: string; flag: string; name: string; second: string; sp: string; trend: -1 | 0 | 1; range: string; brg: string; noPos: boolean; age: string; em: boolean };
/** The live list (TrackRow's shape): nearest first when the receiver's position is known, else newest first. */
export function ismMapRows(r: IsmReport | null, rx: { lat: number; lon: number } | null): MapRow[] {
  if (!r) return [];
  const rows = r.devices.filter(ismOnMap).map((d) => ({ d, k: rx ? rangeKm(rx, d.lat as number, d.lon as number) : null }));
  if (rx) rows.sort((a, b) => a.k!.km - b.k!.km);
  return rows.map(({ d, k }) => ({
    key: d.key, colour: MAP_COLOURS[d.kind] || '#ffb833', flag: '', name: ismMapLabel(d),
    second: [`${ismNetName(d.kind)} ${NODE_TYPE_WORD[ismNodeType(d)]}`, d.hw, d.hops !== undefined ? (d.hops === 0 ? 'first-hand' : `${d.hops} hop${d.hops === 1 ? '' : 's'}`) : '',
             d.battPct !== undefined && d.battPct <= 100 ? `${d.battPct} %` : '', `${d.snr.toFixed(0)} dB`].filter(Boolean).join(' · '),
    sp: '', trend: 0 as const, range: k ? `${k.km < 10 ? k.km.toFixed(1) : Math.round(k.km)} km` : '', brg: k ? compass(k.brg) : '',
    noPos: false, age: ismAgo(d.seen), em: false,
  }));
}
/** The map's status line: how many on the map, of which repeaters, the furthest. */
export function ismMapStatus(r: IsmReport | null, rx: { lat: number; lon: number } | null): string {
  if (!r) return '';
  const items = r.devices.filter(ismOnMap);
  let far = '';
  if (rx && items.length) {
    let best = 0, who = '';
    for (const d of items) { const k = rangeKm(rx, d.lat as number, d.lon as number).km; if (k > best) { best = k; who = ismMapLabel(d); } }
    far = ` · furthest ${who} ${best < 10 ? best.toFixed(1) : Math.round(best)} km`;
  }
  const rep = items.filter((d) => ismNodeType(d) === 'repeater').length;
  return `${items.length} on the map${rep ? ` · ${rep} ${rep === 1 ? 'repeater' : 'repeaters'}` : ''}${far}`;
}
/** ★★ The position in a node's line (the web's POS_RE / ismPosSplit — the same rule): a link to the map on that node. */
export const POS_RE = /(-?\d{1,2}\.\d{2,6}), (-?\d{1,3}\.\d{2,6})( approx\.)?/;
export function ismPosSplit(line: string): { pre: string; pos: string; post: string } | null {
  const m = POS_RE.exec(line);
  return m ? { pre: line.slice(0, m.index), pos: m[0], post: line.slice(m.index + m[0].length) } : null;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  ★★ V12 2026-11-01 THE GALLERY'S "STATIONS RECEIVED" (vibe_ism_gallery.h NodeRow; the web's ismNodesParse — the same rule).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
export interface IsmGalNode extends IsmDevice { first: number; last: number; origin: string }
export function ismIsNodeCard(it: { s?: string }): boolean { return typeof it.s === 'string' && it.s.startsWith('ismnodes-'); }
export function ismNodesParse(body: string, dayUtc: number, nowSec: number): { nodes: IsmGalNode[]; rx: { lat: number; lon: number } | null } {
  const out: IsmGalNode[] = [];
  let rx: { lat: number; lon: number } | null = null;
  const rm = /^Receiver:\s+(-?\d+\.\d+), (-?\d+\.\d+)$/m.exec(body);
  if (rm) { const la = Number(rm[1]), lo = Number(rm[2]); if (Math.abs(la) <= 90 && Math.abs(lo) <= 180) rx = { lat: la, lon: lo }; }
  const at = body.indexOf('Each node, most recent first');
  if (at < 0) return { nodes: out, rx };
  const day = Math.floor(dayUtc / 86400) * 86400;
  const hms = (s: string) => { const m = /^(\d\d):(\d\d):(\d\d)$/.exec(s); return m ? day + Number(m[1]) * 3600 + Number(m[2]) * 60 + Number(m[3]) : NaN; };
  const lines = body.slice(at).split('\n').slice(1);
  for (const ln of lines) {
    if (out.length >= 500) break;
    const f = ln.split(' · ');
    if (f.length !== 12) continue;
    const last = hms(f[0]), first = hms(f[1]), count = parseInt(f[2], 10);
    const net = f[4] === 'MeshCore' ? 'meshcore' : f[4] === 'Meshtastic' ? 'meshtastic' : '';
    const type = f[5] === 'room server' ? 'room' : NODE_TYPES.includes(f[5]) ? f[5] : '';
    const pm = /^(-?\d+\.\d+), (-?\d+\.\d+)( approx)?$/.exec(f[8]);
    if (!isFinite(last) || !isFinite(first) || !(count > 0) || !net || !type || !pm) continue;
    let lat = Number(pm[1]), lon = Number(pm[2]);
    if (Math.abs(lat) > 90 || Math.abs(lon) > 180) continue;
    let approx = pm[3] ? 1 : undefined;
    if (type === 'companion' && !approx) { lat = Math.round(lat * 100) / 100; lon = Math.round(lon * 100) / 100; approx = 1; }
    const name = f[6].slice(0, 40), node = f[7] === '-' ? '' : f[7].slice(0, 16);
    out.push({
      key: `g${out.length}`, kind: net, make: ismNetName(net), model: '', name, long: name, node, line: '', ntype: type, approx,
      lat, lon, hops: f[9] === '-' ? undefined : Number(f[9]), snr: Number(f[10]) || 0, freq: (Number(f[11]) || 0) * 1e6,
      level: -99, seen: Math.max(0, nowSec - last), age: Math.max(0, nowSec - first), count,
      first, last, origin: f[3] === 'background' || f[3] === 'live' || f[3] === 'both' ? f[3] : '',
    });
  }
  return { nodes: out, rx };
}
const galOrigin = (o: string) => (o === 'background' ? 'heard in the background' : o === 'live' ? 'heard live' : o === 'both' ? 'heard in the background and live' : '');
const hhmm = (t: number) => { const d = new Date(t * 1000); return `${String(d.getUTCHours()).padStart(2, '0')}:${String(d.getUTCMinutes()).padStart(2, '0')}`; };
export function ismGalCard(d: IsmGalNode, rx: { lat: number; lon: number } | null): string {
  const t = NODE_TYPE_WORD[ismNodeType(d)];
  const lines: string[] = [];
  const head = [`${(d.freq / 1e6).toFixed(3)} MHz`, `SNR ${d.snr.toFixed(1)} dB`];
  if (d.hops !== undefined) head.push(d.hops === 0 ? 'heard first-hand' : `${d.hops} ${d.hops === 1 ? 'hop' : 'hops'}`);
  lines.push(head.join(' · '));
  if (d.node) lines.push(d.kind === 'meshcore' ? `key ${d.node}…` : d.node);
  let p = `${d.lat!.toFixed(d.approx ? 2 : 4)}, ${d.lon!.toFixed(d.approx ? 2 : 4)}`;
  if (rx) { const k = rangeKm(rx, d.lat!, d.lon!); p += ` · ${k.km < 10 ? k.km.toFixed(1) : Math.round(k.km)} km ${compass(k.brg)}`; }
  if (d.approx) p += ' · approximate (~1 km): a companion node is a person';
  lines.push(p);
  return `<div class="trkCard"><div style="white-space:normal;max-width:300px"><b>${escM(d.name)}</b><br>${escM(ismNetName(d.kind))} ${escM(t)} · ${escM(galOrigin(d.origin))}<br>`
    + `first heard ${hhmm(d.first)} · last heard ${hhmm(d.last)} UTC · ${d.count} ${d.count === 1 ? 'time' : 'times'}<br>`
    + lines.map(escM).join('<br>') + '</div></div>';
}
const ORIGIN_TAG: Record<string, string> = { background: 'BG', live: 'LIVE', both: 'BG + LIVE' };
export function ismGalItems(nodes: IsmGalNode[], rx: { lat: number; lon: number } | null) {
  return nodes.map((d) => ({ key: d.key, lat: d.lat as number, lon: d.lon as number, _ic: ismMapIcon(d), _lb: ismMapLabel(d), _cd: ismGalCard(d, rx) }));
}
export function ismGalRows(nodes: IsmGalNode[], rx: { lat: number; lon: number } | null): MapRow[] {
  const rows = nodes.map((d) => ({ d, k: rx ? rangeKm(rx, d.lat as number, d.lon as number) : null }));
  if (rx) rows.sort((a, b) => a.k!.km - b.k!.km);
  return rows.map(({ d, k }) => ({
    key: d.key, colour: MAP_COLOURS[d.kind] || '#ffb833', flag: '', name: ismMapLabel(d),
    second: [ORIGIN_TAG[d.origin] || '', `${ismNetName(d.kind)} ${NODE_TYPE_WORD[ismNodeType(d)]}`, `${d.count}×`, `${hhmm(d.first)}–${hhmm(d.last)}`].filter(Boolean).join(' · '),
    sp: '', trend: 0 as const, range: k ? `${k.km < 10 ? k.km.toFixed(1) : Math.round(k.km)} km` : '', brg: k ? compass(k.brg) : '',
    noPos: false, age: hhmm(d.last), em: false,
  }));
}
export function ismGalStatus(nodes: IsmGalNode[], rx: { lat: number; lon: number } | null): string {
  const rep = nodes.filter((d) => ismNodeType(d) === 'repeater').length;
  let far = '';
  if (rx && nodes.length) {
    let best = 0, who = '';
    for (const d of nodes) { const k = rangeKm(rx, d.lat as number, d.lon as number).km; if (k > best) { best = k; who = d.name; } }
    far = ` · furthest ${who} ${best < 10 ? best.toFixed(1) : Math.round(best)} km`;
  }
  return `${nodes.length} ${nodes.length === 1 ? 'node' : 'nodes'} heard${rep ? ` · ${rep} ${rep === 1 ? 'repeater' : 'repeaters'}` : ''}${far}`;
}
