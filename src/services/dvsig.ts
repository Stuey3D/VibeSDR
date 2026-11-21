/**
 * ★★★ V12 DIGITAL-VOICE ID (DMR, D-STAR, YSF, NXDN, dPMR) — ONE RULE, SEVERAL READERS: the app and the VibeServer web client
 * import this file; the R36S UI mirrors its line in handheld-ui/src/dvsig.h. docs/v12/DV-SIGNALLING.md.
 *
 * A MODE beside NFM: the server takes the 12.5 kHz channel, runs its own receiver for all five systems at once (AUTO) and
 * reports WHO is on the channel — colour code / RAN, slot, talkgroup or private call, source → destination, callsigns,
 * talker alias, slow-data text, GPS. ★★★ NEVER THE VOICE: AMBE / AMBE+2 are excluded for good; the NFM audio plays as it is
 * (an analogue over on a mixed repeater is heard) and is muted while a digital transmission is followed.
 *
 * ★★★ WHAT MAY BE SHOWN IS THE SERVER'S DECISION (UK WTA 2006 s.48): on an amateur frequency the report carries the
 *     identities ("ids": true); elsewhere only "DMR heard · CC 1 · slot 2" ("ids": false), unless the owner opted in and this
 *     is the owner's own signed-in socket. A client shows what it was sent and never asks for more. Positions go on a map
 *     ONLY from an identities copy on an amateur band (dvsigMapPoints).
 * ★★ OFFERED ONLY WHERE THE SERVER LISTS IT (vibeserver.json `modes` has "dvsig": the radio reaches VHF 136–174 or UHF
 *    400–470 MHz, the owner has not switched it off). No other backend has it.
 * ★ Under the key: AUTO (the receiver says which system it hears) or one system pinned — the manual fallback for a busy
 *   channel. {"type":"dvsig_pin","sys":…} sets it; {"type":"dvsig_status"} asks for the report.
 */

export const DVSIG_LABEL = 'DV ID';
export const DVSIG_NAME = 'Digital voice ID: DMR, D-STAR, YSF, NXDN, dPMR';
export const DVSIG_HINT = 'Who is talking on a DMR, D-STAR, System Fusion, NXDN or dPMR channel: callsigns, radio IDs, '
  + 'talkgroup, talker alias and position on the amateur bands. The data only: the voice is carried by the patented AMBE '
  + 'codec, so it is never decoded. Analogue FM still plays.';

export type DvSys = 'DMR' | 'D-STAR' | 'YSF' | 'NXDN' | 'dPMR';
export type DvPin = 'auto' | 'dmr' | 'dstar' | 'ysf' | 'nxdn' | 'dpmr';
export const DV_PINS: { id: DvPin; label: string }[] = [
  { id: 'auto', label: 'AUTO' }, { id: 'dmr', label: 'DMR' }, { id: 'dstar', label: 'D-STAR' },
  { id: 'ysf', label: 'YSF' }, { id: 'nxdn', label: 'NXDN' }, { id: 'dpmr', label: 'dPMR' },
];

/** One transmission, as the server logged it (vibe_dvsig.h Receiver::json). */
export interface DvEntry {
  id: number; t: number; t1: number; sys: DvSys; kind: string; open: boolean; enc: boolean;
  slot?: number; cc?: number; ran?: number; mode?: string;
  // ★ the identities — present only in an identities copy
  src?: number; dst?: number; grp?: boolean;
  /** ★ V12 beta90: the callsign RadioID.net registers to the source / a private call's destination ID — the server's
   *  owner-switched lookup (vibe_dmr_id.h), amateur bands only. The talker alias, when sent, still wins as the name. */
  srcCall?: string; dstCall?: string;
  from?: string; to?: string; via1?: string; via2?: string; alias?: string; text?: string; dg?: number;
  gps?: { lat: number; lon: number };
  ber?: number;
}
export interface DvState {
  sync: boolean; pin: DvPin; amateur: boolean; band: string; ids: boolean; sys?: DvSys;
  log: DvEntry[];   // newest first, at most 60
}

const SYSES: DvSys[] = ['DMR', 'D-STAR', 'YSF', 'NXDN', 'dPMR'];
const clean = (s: unknown, n: number): string | undefined => {
  if (typeof s !== 'string') return undefined;
  // eslint-disable-next-line no-control-regex
  const t = s.replace(/[\u0000-\u001f\u007f]/g, ' ').replace(/\s+/g, ' ').trim().slice(0, n);
  return t || undefined;
};
/** A looked-up callsign: A–Z 0–9 / only, 3–12 (it is shown to listeners). */
const callOf = (v: unknown): string | undefined => (typeof v === 'string' && /^[A-Z0-9/]{3,12}$/.test(v) ? v : undefined);
const num = (v: unknown): number | undefined => (typeof v === 'number' && Number.isFinite(v) ? v : undefined);
const int = (v: unknown, lo: number, hi: number): number | undefined => { const x = num(v); return x !== undefined && x >= lo && x <= hi ? Math.round(x) : undefined; };

/** Parse a report; null when it is not one. Unknown fields are ignored, every string cleaned. */
export function parseDvsig(msg: unknown): DvState | null {
  if (!msg || typeof msg !== 'object') return null;
  const m = msg as Record<string, unknown>;
  if (m.type !== 'dvsig') return null;
  const pin = (DV_PINS.find((p) => p.id === m.pin)?.id ?? 'auto') as DvPin;
  const st: DvState = { sync: m.state === 'sync', pin, amateur: m.amateur === true, band: clean(m.band, 30) ?? '', ids: m.ids === true, log: [] };
  if (SYSES.includes(m.sys as DvSys)) st.sys = m.sys as DvSys;
  if (Array.isArray(m.log)) {
    for (const r of m.log.slice(0, 60)) {
      if (!r || typeof r !== 'object') continue;
      const g = r as Record<string, unknown>;
      const id = num(g.id), t = num(g.t), t1 = num(g.t1);
      if (id === undefined || t === undefined || !SYSES.includes(g.sys as DvSys)) continue;
      const e: DvEntry = { id, t, t1: t1 ?? t, sys: g.sys as DvSys, kind: clean(g.kind, 12) ?? 'voice', open: g.open === true, enc: g.enc === true };
      e.slot = int(g.slot, 0, 2); e.cc = int(g.cc, 0, 4095); e.ran = int(g.ran, 0, 63); e.mode = clean(g.mode, 24);
      if (st.ids) {   // ★ an identities copy only; a public one has none of these and none is invented
        e.src = int(g.src, 0, 0xFFFFFF); e.dst = int(g.dst, 0, 0xFFFFFF); if (typeof g.grp === 'boolean') e.grp = g.grp;
        e.srcCall = callOf(g.srcCall); e.dstCall = callOf(g.dstCall);
        e.from = clean(g.from, 16); e.to = clean(g.to, 16); e.via1 = clean(g.via1, 16); e.via2 = clean(g.via2, 16);
        e.alias = clean(g.alias, 32); e.text = clean(g.text, 120); e.dg = int(g.dg, 0, 127);
        if (g.gps && typeof g.gps === 'object') {
          const p = g.gps as Record<string, unknown>; const lat = num(p.lat), lon = num(p.lon);
          if (lat !== undefined && lon !== undefined && Math.abs(lat) <= 90 && Math.abs(lon) <= 180 && !(lat === 0 && lon === 0)) e.gps = { lat, lon };
        }
        const ber = num(g.ber); if (ber !== undefined) e.ber = Math.max(0, Math.round(ber));
      }
      for (const k of Object.keys(e) as (keyof DvEntry)[]) if (e[k] === undefined) delete e[k];
      st.log.push(e);
    }
  }
  return st;
}

/** Is DV ID offered by THIS server (vibeserver.json `modes`)? */
export function dvsigOffered(j: { modes?: unknown } | null | undefined): boolean {
  return !!j && Array.isArray(j.modes) && j.modes.includes('dvsig');
}

/** "CC 1 · TS2", "RAN 5", "DG-ID 23", "CC 0x28A" — the channel's own numbers, shown to everyone. */
export function dvWhere(e: DvEntry): string {
  const p: string[] = [];
  if (e.sys === 'DMR') { if (e.cc !== undefined) p.push(`CC ${e.cc}`); if (e.slot) p.push(`TS${e.slot}`); }
  else if (e.sys === 'NXDN') { if (e.ran !== undefined) p.push(`RAN ${e.ran}`); }
  else if (e.sys === 'dPMR') { if (e.cc !== undefined) p.push(`CC 0x${e.cc.toString(16).toUpperCase()}`); }
  else if (e.sys === 'YSF' && e.dg !== undefined) p.push(`DG-ID ${e.dg}`);
  if ((e.sys === 'YSF' || e.sys === 'NXDN') && e.mode) p.push(e.mode);
  return p.join(' · ');
}
/** Who → whom, as far as this copy says: "2345678 → TG 2350", "G4ABC → CQCQCQ via GB7XX B", "" in a public copy. */
export function dvWho(e: DvEntry): string {
  if (e.sys === 'DMR' || e.sys === 'NXDN' || e.sys === 'dPMR') {
    if (e.src === undefined && e.dst === undefined) return '';
    // ★ V12 beta90: "G4ABC (2355254) → TG 2351" once the server's RadioID lookup answered — the alias wins when sent
    const id = (n: number, call?: string) => (call && !e.alias ? `${call} (${n})` : `${n}`);
    const to = e.dst === undefined ? '' : e.grp === false ? (e.dstCall ? `${e.dstCall} (${e.dst})` : `${e.dst}`) : e.sys === 'dPMR' ? `${e.dst}` : `TG ${e.dst}`;
    return [e.src !== undefined ? id(e.src, e.srcCall) : '…', to && `→ ${to}`].filter(Boolean).join(' ');
  }
  if (!e.from && !e.to) return '';
  const via = e.via1 ? ` via ${e.via1.replace(/\s+/g, ' ')}` : '';
  return `${e.from ?? '…'}${e.to ? ` → ${e.to}` : ''}${via}`;
}
/** What it is, beside who: "voice", "SMS", "encrypted", "data". */
export function dvKind(e: DvEntry): string {
  if (e.enc) return 'encrypted';
  if (e.kind === 'sms') return 'SMS';
  if (e.kind === 'preamble') return 'data preamble';
  return e.kind;
}
/** A position as a person reads it: "52.6235° N 1.1235° W". */
export function dvPosition(g: { lat: number; lon: number }): string {
  return `${Math.abs(g.lat).toFixed(4)}° ${g.lat >= 0 ? 'N' : 'S'} ${Math.abs(g.lon).toFixed(4)}° ${g.lon >= 0 ? 'E' : 'W'}`;
}
/** One log row's text (after its time and system): where · who · alias · text · position. */
export function dvRow(e: DvEntry): string {
  const p = [dvWhere(e)];
  const who = dvWho(e);
  p.push(who || (e.sys === 'D-STAR' || e.sys === 'YSF' ? 'heard' : 'heard'));
  if (e.alias) p.push(e.alias);
  if (e.kind !== 'voice' || e.enc) p.push(dvKind(e));
  if (e.text) p.push(e.text);
  if (e.gps) p.push(dvPosition(e.gps));
  return p.filter(Boolean).join(' · ');
}

/** ★ The VTS line — short (the R36S screen is small). The newest transmission open, else the last heard. */
export function dvsigVts(s: DvState | null): string {
  if (!s || !s.log.length) return s?.pin && s.pin !== 'auto' ? `${DV_PINS.find((p) => p.id === s.pin)?.label} · searching` : 'DV · searching';
  const e = s.log[0];
  const who = dvWho(e);
  const name = e.alias || who;
  if (e.open) return [e.sys, name || 'heard', !name ? dvWhere(e) : ''].filter(Boolean).join(' · ') + (e.enc ? ' · encrypted' : '');
  return `DV · last ${e.sys}${name ? ` ${name}` : ''}`;
}
/** The info row under it: the channel's numbers, what the station sends besides its voice. */
export function dvsigInfo(s: DvState | null): string {
  if (!s || !s.log.length) return s && !s.amateur && !s.ids ? 'Not an amateur frequency: who is talking is not shown' : '';
  const e = s.log[0];
  const p: string[] = [];
  const w = dvWhere(e); if (w) p.push(w);
  if (e.alias && dvWho(e)) p.push(dvWho(e));
  if (e.text) p.push(e.text);
  if (e.gps) p.push(dvPosition(e.gps));
  if (!s.ids) p.push('IDs not shown off the amateur bands');
  return p.join(' · ');
}
/** The VTS line and its info as one line, for a bar with one text (the app, the R36S). */
export function dvsigLine(s: DvState | null): string {
  return [dvsigVts(s), dvsigInfo(s)].filter(Boolean).join(' · ');
}
/** ★★ The map's points: ONLY from an identities copy ON AN AMATEUR BAND (non-amateur positions never reach a map, even the
 *  owner's), newest per station. */
export function dvsigMapPoints(s: DvState | null): { key: string; lat: number; lon: number; label: string; t: number }[] {
  if (!s || !s.ids || !s.amateur) return [];
  const seen = new Set<string>();
  const out: { key: string; lat: number; lon: number; label: string; t: number }[] = [];
  for (const e of s.log) {
    if (!e.gps) continue;
    const key = `${e.sys}|${e.from ?? e.src ?? e.id}`;
    if (seen.has(key)) continue;
    seen.add(key);
    out.push({ key, lat: e.gps.lat, lon: e.gps.lon, label: `${e.sys} ${e.alias || e.from || e.srcCall || e.src || ''}`.trim(), t: e.t1 });
  }
  return out;
}
/** "14:03:21" in the viewer's own clock. */
export function dvTime(t: number): string {
  const d = new Date(t);
  const z = (n: number) => String(n).padStart(2, '0');
  return `${z(d.getHours())}:${z(d.getMinutes())}:${z(d.getSeconds())}`;
}

/** The passband to draw (Hz from the dial): the 12.5 kHz channel the server keeps. */
export const DVSIG_PASSBAND: [number, number] = [-6250, 6250];

export function dvsigStatusMessage(): { type: 'dvsig_status' } { return { type: 'dvsig_status' }; }
export function dvsigPinMessage(pin: DvPin): { type: 'dvsig_pin'; sys: DvPin } { return { type: 'dvsig_pin', sys: pin }; }
