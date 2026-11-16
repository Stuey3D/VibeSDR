/**
 * ★★★ V12 M17 DIGITAL VOICE — ONE RULE, SEVERAL READERS (the app and the VibeServer web client import this file; the R36S
 * UI mirrors it in handheld-ui/src/m17.h). docs/v12/M17.md.
 *
 * M17 is a MODE beside NFM: the server takes the 12.5 kHz channel, runs its own M17 receiver (4-FSK, the Viterbi, the LSF and
 * the LICH) and plays the Codec 2 speech instead of the FM audio. It is SILENT until a transmission is decoded; the VTS line
 * says what is happening ("M17 · searching", "M17 · G4ABC → @ALL", "M17 · last G4ABC") and the info row carries what the
 * station sends besides its voice: META text, a GNSS position, the station a repeater relays, and SMS packets.
 *
 * ★★ OFFERED ONLY WHERE THE SERVER LISTS IT (vibeserver.json `modes` has "m17": libcodec2 loaded and the radio reaches 2 m or
 *    70 cm). No other backend has it — an M17 key there would be dead.
 * ★ There is nothing to choose inside the mode (no sub-modes, no width: the receiver cuts its own channel), so there is no
 *   control under it; {"type":"m17_status"} only asks for the report.
 */

export const M17_LABEL = 'M17';
export const M17_NAME = 'M17 digital voice';
export const M17_HINT = 'Digital voice and data on 2 m and 70 cm. Silent until an M17 transmission is decoded; shows the callsigns, text, position and messages it carries.';

export interface M17Gnss { lat: number; lon: number; alt?: number; spd?: number; brg?: number; r?: number }
export interface M17Msg { id: number; src?: string; dst?: string; ptype: number; bytes: number; text?: string }
/** The server's {"type":"m17"} report (vibe_m17.h publish). */
export interface M17State {
  sync: boolean;                                       // a transmission is being decoded now
  kind?: 'voice' | 'voicedata' | 'data' | 'packet';    // what it carries
  src?: string; dst?: string;                          // the callsigns (dst "@ALL" = broadcast)
  can?: number;                                        // the channel access number
  enc?: 'scrambler' | 'aes' | 'other';                 // encrypted: no voice is played
  ber?: number;                                        // % of channel bits the FEC corrected (about once a second)
  off?: number;                                        // Hz the station is off the dial (50 Hz steps)
  last?: string;                                       // while searching: the last station heard
  text?: string;                                       // META text
  via?: string;                                        // the extended callsign ("G7XYZ via M17-GBR A")
  gnss?: M17Gnss;
  msg?: M17Msg;                                        // the newest packet (SMS shown as text)
}

/** Callsigns and text come off the air: printable, clipped. */
const clean = (s: unknown, n: number): string | undefined => {
  if (typeof s !== 'string') return undefined;
  // eslint-disable-next-line no-control-regex
  const t = s.replace(/[\u0000-\u001f\u007f]/g, ' ').replace(/\s+/g, ' ').trim().slice(0, n);
  return t || undefined;
};
const num = (v: unknown): number | undefined => (typeof v === 'number' && Number.isFinite(v) ? v : undefined);

/** Parse a report; null when it is not one. Unknown fields are ignored. */
export function parseM17(msg: unknown): M17State | null {
  if (!msg || typeof msg !== 'object') return null;
  const m = msg as Record<string, unknown>;
  if (m.type !== 'm17') return null;
  const st: M17State = { sync: m.state === 'sync' };
  if (st.sync) {
    if (m.kind === 'voice' || m.kind === 'voicedata' || m.kind === 'data' || m.kind === 'packet') st.kind = m.kind;
    st.src = clean(m.src, 12); st.dst = clean(m.dst, 14);
    const can = num(m.can); if (can !== undefined && can >= 0 && can <= 15) st.can = Math.round(can);
    if (m.enc === 'scrambler' || m.enc === 'aes' || m.enc === 'other') st.enc = m.enc;
    const ber = num(m.ber); if (ber !== undefined) st.ber = Math.max(0, Math.round(ber));
    const off = num(m.off); if (off !== undefined) st.off = Math.round(off);
  } else {
    st.last = clean(m.last, 12);
  }
  st.text = clean(m.text, 52);
  st.via = clean(m.via, 32);
  if (m.gnss && typeof m.gnss === 'object') {
    const g = m.gnss as Record<string, unknown>;
    const lat = num(g.lat), lon = num(g.lon);
    if (lat !== undefined && lon !== undefined && Math.abs(lat) <= 90 && Math.abs(lon) <= 180) {
      st.gnss = { lat, lon };
      const alt = num(g.alt), spd = num(g.spd), brg = num(g.brg), r = num(g.r);
      if (alt !== undefined) st.gnss.alt = alt;
      if (spd !== undefined) st.gnss.spd = spd;
      if (brg !== undefined) st.gnss.brg = brg;
      if (r !== undefined) st.gnss.r = r;
    }
  }
  if (m.msg && typeof m.msg === 'object') {
    const g = m.msg as Record<string, unknown>;
    const id = num(g.id), ptype = num(g.ptype), bytes = num(g.bytes);
    if (id !== undefined && ptype !== undefined && bytes !== undefined) {
      st.msg = { id, ptype, bytes, src: clean(g.src, 12), dst: clean(g.dst, 14) };
      const t = clean(g.text, 200);
      if (ptype === 5 && t) st.msg.text = t;
    }
  }
  // ★ drop the undefined keys, so a state compares by its JSON
  for (const k of Object.keys(st) as (keyof M17State)[]) if (st[k] === undefined) delete st[k];
  if (st.msg) for (const k of Object.keys(st.msg) as (keyof M17Msg)[]) if (st.msg[k] === undefined) delete st.msg[k];
  return st;
}

/** Is M17 offered by THIS server (vibeserver.json `modes`)? */
export function m17Offered(j: { modes?: unknown } | null | undefined): boolean {
  return !!j && Array.isArray(j.modes) && j.modes.includes('m17');
}

/** ★ The VTS line — short, the R36S screen is small (two callsigns of nine characters fit in 30). */
export function m17Vts(s: M17State | null): string {
  if (!s) return 'M17 · searching';
  if (!s.sync) return s.last ? `M17 · last ${s.last}` : 'M17 · searching';
  const who = s.src ?? '…';
  const line = s.dst && s.dst !== '@ALL' ? `M17 · ${who} → ${s.dst}` : `M17 · ${who}`;
  if (s.enc) return `${line} · encrypted`;
  if (s.kind === 'packet' || s.kind === 'data') return `${line} · data`;
  return line;
}

/** A GNSS position as a person reads it: "52.6235° N 1.1235° W", with the altitude and speed where sent. */
export function m17Position(g: M17Gnss): string {
  const lat = `${Math.abs(g.lat).toFixed(4)}° ${g.lat >= 0 ? 'N' : 'S'}`;
  const lon = `${Math.abs(g.lon).toFixed(4)}° ${g.lon >= 0 ? 'E' : 'W'}`;
  const parts = [`${lat} ${lon}`];
  if (g.alt !== undefined) parts.push(`${Math.round(g.alt)} m`);
  if (g.spd !== undefined && g.spd > 0) parts.push(`${Math.round(g.spd)} km/h`);
  return parts.join(', ');
}
/** A packet as one line: "SMS G4ABC → M0XYZ: Hello", or "Packet (type 2, 41 bytes) from M0XYZ". */
export function m17MsgLine(m: M17Msg): string {
  const who = [m.src, m.dst ? `→ ${m.dst}` : ''].filter(Boolean).join(' ');
  if (m.ptype === 5 && m.text !== undefined) return `SMS${who ? ` ${who}` : ''}: ${m.text}`;
  return `Packet (type ${m.ptype}, ${m.bytes} bytes)${m.src ? ` from ${m.src}` : ''}`;
}
/** ★ The small info row under the VTS line: what the station sends besides its voice, most useful first. '' = nothing. */
export function m17Info(s: M17State | null): string {
  if (!s) return '';
  const parts: string[] = [];
  if (s.via) parts.push(s.via);
  if (s.text) parts.push(s.text);
  if (s.gnss) parts.push(m17Position(s.gnss));
  if (s.msg) parts.push(m17MsgLine(s.msg));
  if (s.sync && s.ber !== undefined && s.ber >= 5) parts.push(`${s.ber} % errors`);
  return parts.join(' · ');
}

/** ★ The packets heard, newest first, kept across reports (each report carries only the newest, and repeats it). A new
 *  receiver (a retune) numbers from 1 again, so a packet is known by its number AND its contents. */
export function m17MergeMsgs(list: M17Msg[], s: M17State | null, max = 20): M17Msg[] {
  const n = s?.msg;
  if (!n) return list;
  const key = (m: M17Msg) => `${m.id}|${m.src ?? ''}|${m.dst ?? ''}|${m.ptype}|${m.bytes}|${m.text ?? ''}`;
  if (list.length && key(list[0]) === key(n)) return list;
  return [n, ...list].slice(0, max);
}

/** The passband to draw (Hz from the dial): M17's 9 kHz channel. */
export const M17_PASSBAND: [number, number] = [-4500, 4500];

/** The report-request message (entering the mode, a new socket, joining a shared dial already in M17). */
export function m17StatusMessage(): { type: 'm17_status' } {
  return { type: 'm17_status' };
}
