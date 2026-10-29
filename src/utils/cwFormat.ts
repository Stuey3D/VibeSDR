// cwFormat.ts — small shared formatters for the CW box and the spot lists, app AND web (V12, 2026-10-29). ONE source:
//   • the speed as a RANGE (the server's `wpmLo` / `wpmHi` beside `wpm`): "17–21 wpm", or "21 wpm";
//   • the QRZ.COM links — the site itself (the key under "NOT SURE WHAT A SIGNAL IS?") and a callsign's page (every
//     spotted or decoded callsign is tappable). ★ Links only: nothing is ever fetched, scraped or stored from QRZ.
// ★ Erasable TypeScript only (no enums, no imports): scripts run this file directly under Node.

/** The speed a row or a spot was read at. ★ A RANGE when the server sends one (`wpmLo` < `wpmHi`, en dash) — a hand-sent
 *  fist speeds up and slows down inside an over; a single figure when they are equal, or absent (an older server). */
export function cwWpmText(s: { wpm: number; wpmLo?: number; wpmHi?: number }): string {
  const ok = (v: unknown): v is number => typeof v === 'number' && Number.isFinite(v) && v > 0;
  if (ok(s.wpmLo) && ok(s.wpmHi)) {
    const lo = Math.round(Math.min(s.wpmLo, s.wpmHi)), hi = Math.round(Math.max(s.wpmLo, s.wpmHi));
    if (lo !== hi) return `${lo}–${hi} wpm`;
    return `${hi} wpm`;
  }
  return `${Math.round(Number(s.wpm) || 0)} wpm`;
}
/** True when `cwWpmText` would draw a range (the list widens its speed column for it). */
export function cwHasWpmRange(s: { wpmLo?: number; wpmHi?: number }): boolean {
  const ok = (v: unknown): v is number => typeof v === 'number' && Number.isFinite(v) && v > 0;
  return ok(s.wpmLo) && ok(s.wpmHi) && Math.round(s.wpmLo) !== Math.round(s.wpmHi);
}
/** The wire's `wpmLo` / `wpmHi`, kept only when they are positive numbers — absent on an older server, so a parsed row
 *  carries no such keys at all. */
export function cwWpmRangeOf(x: any): { wpmLo?: number; wpmHi?: number } {
  const lo = x?.wpmLo, hi = x?.wpmHi;
  const ok = (v: unknown): v is number => typeof v === 'number' && Number.isFinite(v) && v > 0 && v < 200;
  return ok(lo) && ok(hi) ? { wpmLo: Math.round(lo), wpmHi: Math.round(hi) } : {};
}

/** QRZ.COM's home page — the key beside SIGNAL ID WIKI. */
export const QRZ_HOME = 'https://www.qrz.com';
/** ★ Said beside the key (and in the terms guide): QRZ shows the full entry only to a signed-in visitor. */
export const QRZ_SIGN_IN_NOTE = 'QRZ.com shows full details only when you are signed in.';

/** The call QRZ looks up: upper case, an FT8 hashed call's <…> brackets dropped, and of a portable call
 *  (EA8/G4ABC/P, G4ABC/MM) the LONGEST part — the station's own call. null = nothing that looks like a callsign (a
 *  letter AND a figure, 3–10 characters), so nothing is linked. */
export function qrzBaseCall(call: string): string | null {
  const c = String(call || '').trim().toUpperCase().replace(/^<|>$/g, '');
  if (!c || c.length > 24 || !/^[A-Z0-9/]+$/.test(c)) return null;
  let best = '';
  for (const p of c.split('/')) if (p.length > best.length) best = p;
  if (best.length < 3 || best.length > 10 || !/[A-Z]/.test(best) || !/[0-9]/.test(best)) return null;
  return best;
}
/** A callsign's QRZ.COM page, or null when `call` is not a callsign (see qrzBaseCall). */
export function qrzCallUrl(call: string): string | null {
  const b = qrzBaseCall(call);
  return b ? `${QRZ_HOME}/db/${encodeURIComponent(b)}` : null;
}
