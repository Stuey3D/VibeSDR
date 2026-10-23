/**
 * weatherReport.ts — the RTTY decoder's WEATHER REPORTS, as the server sends them (V12, 2026-10-23). Shared by the app
 * (DecoderClient / DecoderPanel) and the web client (web/client/src/decoders.ts + main.ts).
 *
 * ★★★ THE SERVER TRANSLATES, THE CLIENT SHOWS (Stuart: "a much wider range of devices can listen than can host").
 *     VibeServer's decoder host reads each SYNOP (FM 12) / SHIP (FM 13) report out of the RTTY text and sends its plain
 *     English as its own frame — vibe_synop.h. Nothing here decodes a group: this only checks what arrived and places it.
 *
 * Wire (the decoder socket, RTTY only):
 *     0x08 | u32 BE length | UTF-8 JSON
 *     {"type":"weather_report","kind":"synop"|"ship","raw":"10384 41/96 …","line":"10384 · Tempelhof, Germany · 06:00 UTC
 *      · wind 250° 8 kt (15 km/h) · 11.8 °C · 1020.6 hPa","unreadable":0,"details":["Visibility 4 km", …]}
 *  ★ Sent IN ITS PLACE in the text: after the 0x01 text that ended the report's line, before the next report's. So the
 *    box draws it at the END of the text so far — right under the raw groups it came from.
 *  ★ An older client ignores the op (its RTTY branch acts only on 0x01/0x03/0x06) and keeps the raw text as before; an
 *    older server never sends it.
 *
 * ★★ THE APP'S BOX IS ONE STRING (SDRScreen's decoderText, capped scrollback), so a report rides in it as a MARKER —
 *    WX_OPEN + id + WX_CLOSE, Unicode private-use characters no decoder prints (ITA2 and the server's ASCII mode are
 *    7-bit) — and DecoderPanel draws the report where the marker sits. A marker half cut off by the scrollback cap is
 *    dropped, never shown as text.
 */

export interface WeatherReport {
  kind: 'synop' | 'ship';
  raw: string;
  line: string;
  details: string[];
  unreadable: number;
}

export const WX_OPEN = '';
export const WX_CLOSE = '';
/** ★ The most reports a box keeps (the app's map; the web box's elements). Older ones scroll out with their text. */
export const WX_MAX_KEPT = 64;

const str = (v: unknown, max: number): string | null => {
  if (typeof v !== 'string') return null;
  // ★ Control characters and our own marker characters out: a report is shown as text, never as layout.
  const s = v.replace(/[\u0000-\u001f\u007f]/g, ' ').trim();
  return s ? s.slice(0, max) : null;
};

/** A report from the server, checked field by field — null when it is not one. */
export function cleanWeatherReport(v: unknown): WeatherReport | null {
  if (!v || typeof v !== 'object') return null;
  const r = v as Record<string, unknown>;
  if (r.type !== 'weather_report') return null;
  const kind = r.kind === 'ship' ? 'ship' : r.kind === 'synop' ? 'synop' : null;
  const line = str(r.line, 400);
  if (!kind || !line) return null;
  const raw = str(r.raw, 600) ?? '';
  const details = Array.isArray(r.details)
    ? r.details.slice(0, 40).map((d) => str(d, 200)).filter((d): d is string => !!d)
    : [];
  const unreadable = typeof r.unreadable === 'number' && Number.isFinite(r.unreadable) ? Math.max(0, Math.min(99, Math.round(r.unreadable))) : 0;
  return { kind, raw, line, details, unreadable };
}

/** The 0x08 frame → a report, or null (wrong op, short, not JSON, not a report). */
export function parseWeatherFrame(u8: Uint8Array): WeatherReport | null {
  if (u8.length < 5 || u8[0] !== 0x08) return null;
  const len = ((u8[1] << 24) >>> 0) + (u8[2] << 16) + (u8[3] << 8) + u8[4];
  if (len <= 0 || 5 + len > u8.length || len > 16384) return null;
  try {
    return cleanWeatherReport(JSON.parse(new TextDecoder().decode(u8.subarray(5, 5 + len))));
  } catch {
    return null;
  }
}

/** The app's marker for report `id`. */
export function wxMarker(id: number): string { return WX_OPEN + String(id) + WX_CLOSE; }

/** Incoming decoder text with any marker character removed — so only the client ever places one. */
export function stripWxMarkers(s: string): string {
  return s.indexOf(WX_OPEN) < 0 && s.indexOf(WX_CLOSE) < 0 ? s : s.replace(/[]/g, '');
}

export type WxPart = { text: string } | { id: number };
/** The box's text cut at its markers: text, report, text … A half marker (the cap cut it) is dropped. */
export function splitWxText(s: string): WxPart[] {
  if (s.indexOf(WX_OPEN) < 0 && s.indexOf(WX_CLOSE) < 0) return s ? [{ text: s }] : [];
  const out: WxPart[] = [];
  const push = (t: string) => { if (t) out.push({ text: t.replace(/[]/g, '') }); };
  // ★ A head that lost its opener: "12…" — the digits and the closer go.
  let rest = s.replace(/^\d*/, '');
  for (;;) {
    const a = rest.indexOf(WX_OPEN);
    if (a < 0) { push(rest); break; }
    const b = rest.indexOf(WX_CLOSE, a + 1);
    push(rest.slice(0, a));
    if (b < 0) break;                                  // ★ a tail that lost its closer: dropped
    const id = rest.slice(a + 1, b);
    if (/^\d{1,9}$/.test(id)) out.push({ id: Number(id) });
    rest = rest.slice(b + 1);
  }
  return out;
}
