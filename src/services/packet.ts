/**
 * packet.ts — V12 PACKET (docs/v12/AX25-9600-FX25-IL2P.md): the APRS decoder's packet choices — AUTO, VHF 1200, 9600,
 * HF 300 — its FX.25 and IL2P switches, and the FRAME LOG (every AX.25 frame, APRS or not: cubesat telemetry,
 * connected-mode traffic), for the app AND the web client (web/client/src/main.ts imports this file).
 *
 * ★★ ONE RULE, TWO READERS: what is offered comes from the server's capability, never a list of our own
 *    (feedback_app_no_hardcoded_settings):
 *      aprsFec: true            — this server's packet decoder reads FX.25 and IL2P (the two switches exist)
 *      pkt9600: true            — its 9600 option (G3RUH on the channel IQ) is live
 *      pkt9600: false + pkt9600Why — greyed with the reason (the radio reaches neither 2 m nor 70 cm)
 *      no pkt9600 at all        — an older server, or an Icom (its FM audio is demodulated in the radio): no 9600, no AUTO
 * ★★ AUTO tries everything at once: 1200 on the audio AND 9600 on the channel IQ, FX.25 and IL2P on both — the
 *    default where 9600 is live. The manual choices are the fallback (feedback_auto_decoders_manual_fallback); FX.25 and
 *    IL2P are switches under 1200 and 9600 (feedback_decoder_toggles_nested_options), both on unless switched off.
 * ★ Wire: attach 'aprs' {baud_rate, packet, fx25?, il2p?} — `baud_rate` stays (1200 with AUTO) so an older server runs
 *   1200; ← {"type":"aprs_frame","data":{…}} each frame; aprs_list carries fx25, il2p, fixed, other, auto, log (to a joiner).
 * ★ Everything in a frame came off the air: the text is printable ASCII by construction (the server's), and is still
 *   cut to length and escaped where it meets HTML by the caller.
 */

export type PacketMode = 'auto' | '1200' | '9600' | '300';

export interface PacketCaps {
  fec: boolean;          // FX.25 + IL2P switches
  has9600: boolean;      // the server knows 9600 at all (pkt9600 present)
  live9600: boolean;     // …and this radio can use it
  why9600: string;       // the reason when it cannot
}
export function parsePacketCaps(j: unknown): PacketCaps {
  const o = (j && typeof j === 'object' ? j : {}) as Record<string, unknown>;
  const has = typeof o.pkt9600 === 'boolean';
  const why = typeof o.pkt9600Why === 'string' ? o.pkt9600Why.trim().slice(0, 200) : '';
  return { fec: o.aprsFec === true, has9600: has, live9600: o.pkt9600 === true, why9600: has && o.pkt9600 !== true ? why : '' };
}

/** The choices to draw, in order. 9600 and AUTO only where the server has 9600 (live or greyed with its reason). */
export function packetChoices(c: PacketCaps): { id: PacketMode; label: string; why?: string }[] {
  const out: { id: PacketMode; label: string; why?: string }[] = [];
  if (c.has9600) out.push({ id: 'auto', label: 'AUTO', ...(c.live9600 ? {} : { why: c.why9600 || '9600 packet cannot run on this radio' }) });
  out.push({ id: '1200', label: 'VHF 1200' });
  if (c.has9600) out.push({ id: '9600', label: '9600', ...(c.live9600 ? {} : { why: c.why9600 || '9600 packet cannot run on this radio' }) });
  out.push({ id: '300', label: 'HF 300' });
  return out;
}
/** What a fresh listener starts on: AUTO where it can run, else 1200. A stored choice the server cannot do falls back. */
export function packetDefault(c: PacketCaps, stored?: unknown): PacketMode {
  const s = typeof stored === 'string' ? stored : '';
  const usable = (m: string) => m === '1200' || m === '300' || ((m === 'auto' || m === '9600') && c.live9600);
  if (usable(s)) return s as PacketMode;
  return c.live9600 ? 'auto' : '1200';
}
/** The FX.25 / IL2P switches show under 1200 and 9600 (AUTO has them all on; HF 300 has none). */
export function packetFecShown(c: PacketCaps, m: PacketMode): boolean { return c.fec && (m === '1200' || m === '9600'); }

/** The attach parameters. */
export function packetParams(m: PacketMode, fx25: boolean, il2p: boolean, c: PacketCaps): Record<string, number | string> {
  if (m === '300') return { baud_rate: 300 };
  if (!c.fec && !c.has9600) return { baud_rate: 1200 };                 // an older server: exactly what it knew
  if (m === 'auto') return { baud_rate: 1200, packet: 'auto' };
  const p: Record<string, number | string> = { baud_rate: m === '9600' ? 9600 : 1200, packet: m };
  if (c.fec) { p.fx25 = fx25 ? 1 : 0; p.il2p = il2p ? 1 : 0; }
  return p;
}
/** The TUNE key — null for 9600 (it has no one channel: the listener tunes the signal; no dead control). */
export function packetTune(m: PacketMode, aprsHz: number): { hz: number; mode: 'nfm' | 'usb'; label: string } | null {
  if (m === '9600') return null;
  if (m === '300') return { hz: 10147600, mode: 'usb', label: 'TUNE 10.1476 USB' };
  return { hz: Math.round(aprsHz), mode: 'nfm', label: `TUNE ${(aprsHz / 1e6).toFixed(3)} NFM` };
}
/** The hint under the settings (plain text; the web bolds nothing it does not need to). */
export function packetHint(m: PacketMode, aprsHz: number): string {
  const ch = `${(aprsHz / 1e6).toFixed(3)} MHz`;
  switch (m) {
    case '300': return 'HF packet: 300 Bd on 10.1476 MHz USB (30 m; 14.105 on 20 m). The decoder tries a few tunings either side, so close is close enough. The speaker is muted while APRS listens.';
    case '9600': return '9600 packet (G3RUH): tune the signal in NFM — terrestrial 9k6 on 2 m and 70 cm, or a cubesat downlink. FX.25 and IL2P are decoded too when switched on. Every frame goes in FRAMES; APRS ones also on the station list. The speaker is muted while APRS listens.';
    case 'auto': return `AUTO: 1200 and 9600 at once, with FX.25 and IL2P — APRS on ${ch} NFM here, or tune any packet signal. Every frame goes in FRAMES; APRS ones also on the station list. The speaker is muted while APRS listens.`;
    default: return `VHF APRS: ${ch} NFM here. Every station heard is listed, with its position, weather, messages and the digipeater that carried it. MAP in the box shows them on the map. The speaker is muted while APRS listens.`;
  }
}
/** The box's "listening" line for a mode. */
export function packetIdle(m: PacketMode): string {
  return m === '300' ? 'HF packet, 300 Bd' : m === '9600' ? 'packet, 9600 Bd' : m === 'auto' ? 'packet, 1200 + 9600' : 'APRS, 1200 Bd';
}

// ── the frame log ──────────────────────────────────────────────────────────────────────────────────────────────────
export interface PacketFrame {
  age: number; from: string; to: string; path?: string; ctl: string; pid?: string;
  fec: 'ax25' | 'fx25' | 'il2p'; fixed?: number; crc?: boolean; sat?: string;
  len: number; hex: string; text: string; aprs?: boolean;
}
const s = (v: unknown, n: number) => (typeof v === 'string' ? v.slice(0, n) : '');
const call = (v: unknown) => s(v, 12).replace(/[^A-Z0-9-]/g, '');
export function parsePacketFrame(d: unknown): PacketFrame | null {
  if (!d || typeof d !== 'object') return null;
  const o = d as Record<string, unknown>;
  const from = call(o.from), to = call(o.to);
  if (!from || !to) return null;
  const fec = o.fec === 'fx25' || o.fec === 'il2p' ? o.fec : 'ax25';
  const num = (v: unknown) => (typeof v === 'number' && Number.isFinite(v) && v >= 0 ? v : undefined);
  const f: PacketFrame = {
    age: num(o.age) ?? 0, from, to, ctl: s(o.ctl, 12).replace(/[^A-Za-z0-9 /]/g, ''), fec,
    len: Math.min(2048, Math.round(num(o.len) ?? 0)),
    hex: s(o.hex, 512).replace(/[^0-9a-f]/g, ''),
    text: s(o.text, 256).replace(/[^\x20-\x7e]/g, '.'),
  };
  const path = s(o.path, 120).replace(/[^A-Z0-9,*-]/g, ''); if (path) f.path = path;
  const pid = s(o.pid, 2).replace(/[^0-9A-F]/g, ''); if (pid.length === 2) f.pid = pid;
  const fixed = num(o.fixed); if (fixed) f.fixed = Math.round(fixed);
  if (o.crc === 1 || o.crc === true) f.crc = true;
  const sat = s(o.sat, 40).replace(/[\u0000-\u001f<>]/g, '').trim(); if (sat) f.sat = sat;
  if (o.aprs === 1 || o.aprs === true) f.aprs = true;
  return f;
}
/** The FEC in words for a row: "FX.25 · 4 fixed", "IL2P · CRC", "" for plain AX.25. */
export function packetFecLabel(f: PacketFrame): string {
  if (f.fec === 'ax25') return '';
  const name = f.fec === 'fx25' ? 'FX.25' : 'IL2P';
  const bits = [name];
  if (f.fixed) bits.push(`${f.fixed} fixed`);
  if (f.crc) bits.push('CRC');
  return bits.join(' · ');
}
/** A frame's heading line: "OM9GRB → CQ · UI F0 · GRBAlpha · FX.25 · 4 fixed · 120 bytes". */
export function packetFrameHead(f: PacketFrame): string {
  const fec = packetFecLabel(f);
  return [`${f.from} → ${f.to}${f.path ? ` via ${f.path}` : ''}`, `${f.ctl}${f.pid ? ' ' + f.pid : ''}`, f.sat || '', fec,
          `${f.len} byte${f.len === 1 ? '' : 's'}`].filter(Boolean).join(' · ');
}
/** The hex grouped in fours for reading ("6a98 f983 …"), at most `max` bytes. */
export function packetHexGrouped(hex: string, max = 64): string {
  const h = hex.slice(0, max * 2);
  return (h.match(/.{1,8}/g) ?? []).join(' ') + (hex.length > max * 2 ? ' …' : '');
}
/** Keep the newest `cap` frames, newest first; a joiner's log (oldest first on the wire) replaces the list. */
export function packetMergeLog(cur: PacketFrame[], incoming: PacketFrame[], cap = 60): PacketFrame[] {
  const out = [...incoming.slice().reverse(), ...cur];
  return out.slice(0, cap);
}
/** The status line's packet half, from aprs_list: "· 3 FX.25 · 1 IL2P · 12 bytes fixed · 2 other frames". */
export function packetStatusExtra(d: unknown): string {
  const o = (d && typeof d === 'object' ? d : {}) as Record<string, unknown>;
  const n = (v: unknown) => (typeof v === 'number' && Number.isFinite(v) && v > 0 ? Math.round(v) : 0);
  const parts: string[] = [];
  if (n(o.fx25)) parts.push(`${n(o.fx25)} FX.25`);
  if (n(o.il2p)) parts.push(`${n(o.il2p)} IL2P`);
  if (n(o.fixed)) parts.push(`${n(o.fixed)} bytes fixed`);
  if (n(o.other)) parts.push(`${n(o.other)} other frame${n(o.other) === 1 ? '' : 's'}`);
  return parts.length ? ' · ' + parts.join(' · ') : '';
}
