/**
 * ★★ V12 beta90 THIS SERVER'S RANKING — the gallery's standing card (web and app read this one file). The server keeps
 * what the directory published for ITS receiver callsign (vibe_spot_report.h ranksJson → /vibeserver/gallery/ranks.json,
 * directory/src/ranks.js); shown only while the owner has PSK Reporter reporting on. docs/v12/PSK-REPORTER.md.
 *   "PSK Reporter: No. 5 on 40 m for reports · No. 2 on 60 m for countries · WSPR: No. 3 on 40 m"
 *   "PSK Reporter's statistics, last 24 hours, 14 min ago · wspr.live (WSPRnet spots), last 24 hours, 14 min ago"
 * The idea is M9PSY's (he built PSK Reporter / WSPR rank fetching into UberSDR); this is our own code.
 */
type PskBand = { reports?: number; reportsN?: number; countries?: number; countriesN?: number };
type WsprBand = { rank?: number; n?: number };
export interface RanksAnswer {
  on?: boolean; call?: string; fetched?: number;
  ranks?: {
    call?: string;
    psk?: { fetched?: number; window?: string; source?: string; bands?: Record<string, PskBand> } | null;
    wspr?: { fetched?: number; window?: string; source?: string; bands?: Record<string, WsprBand> } | null;
  };
}
const bandName = (b: string) => b.replace(/^(\d+(?:\.\d+)?)(c?m)$/, '$1 $2');
const ago = (t: number | undefined, now: number) => {
  if (!t || !Number.isFinite(t)) return '';
  const m = Math.max(0, Math.round((now - t) / 60));
  return m < 1 ? 'just now' : m < 90 ? `${m} min ago` : `${Math.round(m / 60)} h ago`;
};
const okRank = (n: unknown): n is number => typeof n === 'number' && Number.isInteger(n) && n >= 1 && n <= 1000;
/** The best (lowest) place over the bands for one measure: [band, place] or null. */
function best<T>(bands: Record<string, T> | undefined, pick: (x: T) => unknown): [string, number] | null {
  let out: [string, number] | null = null;
  for (const [b, x] of Object.entries(bands ?? {})) {
    if (!/^\d{1,4}(\.\d+)?c?m$/.test(b)) continue;
    const r = pick(x);
    if (okRank(r) && (!out || r < out[1])) out = [b, r];
  }
  return out;
}

/** The card's two lines, or null when there is nothing to show (reporting off, not ranked, an answer for another call). */
export function rankCard(j: RanksAnswer | null | undefined, nowSec: number): { title: string; src: string } | null {
  if (!j || j.on !== true || !j.ranks || !j.call || (j.ranks.call && j.ranks.call !== j.call)) return null;
  const parts: string[] = [], srcs: string[] = [];
  const psk = j.ranks.psk;
  if (psk && psk.bands) {
    const r = best(psk.bands, (x) => x.reports), c = best(psk.bands, (x) => x.countries);
    const p: string[] = [];
    if (r) p.push(`No. ${r[1]} on ${bandName(r[0])} for reports`);
    if (c) p.push(`No. ${c[1]} on ${bandName(c[0])} for countries`);
    if (p.length) {
      parts.push(`PSK Reporter: ${p.join(' · ')}`);
      srcs.push(['PSK Reporter’s statistics', psk.window || 'last 24 hours', ago(psk.fetched, nowSec)].filter(Boolean).join(', '));
    }
  }
  const ws = j.ranks.wspr;
  if (ws && ws.bands) {
    const w = best(ws.bands, (x) => x.rank);
    if (w) {
      const n = ws.bands[w[0]]?.n;
      parts.push(`WSPR: No. ${w[1]} on ${bandName(w[0])}${typeof n === 'number' && n > 0 ? ` (${n} stations heard)` : ''}`);
      srcs.push([ws.source || 'wspr.live (WSPRnet spots)', ws.window || 'last 24 hours', ago(ws.fetched, nowSec)].filter(Boolean).join(', '));
    }
  }
  if (!parts.length) return null;
  return { title: `${j.call} · ${parts.join(' · ')}`, src: srcs.join(' · ') };
}
