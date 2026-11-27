/* ★★★ V12 beta96 THE GALLERY ON THE LANDING (Stuart 11-27: "along with the air/sea maps on the landing screen we could put
 * the servers gallery there too so a user who just wants to look at meteor pics doesnt tie up the entire radio").
 * The landing button's gate and caption — the half the web page (main.ts splashGalleryOffer) and the app (SDRScreen's
 * radio chooser) share. Pure: the request goes through the caller's `get` (parsed JSON or null), tested in node
 * (scripts/test_gallery_offer.ts).
 *
 * ★ ONE small read, once when the landing draws, never polled: gallery.json?offset=0&limit=1&since=86400 — the newest
 *   item of the last 24 hours, that window's "total", and the whole store's "count" (vibe_gallery.h listJson). A server
 *   that keeps no gallery, has made it private (403, the owner's galleryPublic off), holds nothing, or is behind a server
 *   PIN the page has not been given (401): no button from this — the PIN case is the caller's (it shows the key with no
 *   caption and asks for the PIN, so no count leaks).
 */
export type Getter = (path: string) => Promise<any | null>;

export interface GalleryOffer {
  show: boolean;
  /** items kept in the last 24 hours, and in the whole gallery */
  today: number;
  count: number;
  /** the newest of the last 24 hours (its kind and time), or null */
  newest: { k: string; t: number } | null;
  /** the caption after "GALLERY · " — '' when nothing worth saying */
  line: string;
}

/** ★ The filter chips' own words (the web's #galFilter, the app's GalleryOverlay KINDS) — a kind not listed is shown in capitals. */
export const GALLERY_KIND_WORDS: Record<string, string> = {
  navtex: 'NAVTEX', rtty: 'RTTY', wefax: 'WEFAX', sstv: 'SSTV', ft8: 'DIGITAL SPOTS', cw: 'CW', psk: 'PSK', ism: 'ISM',
  ndb: 'NDB', ais: 'AIS', aprs: 'APRS', dsc: 'DSC', sitora: 'SITOR-A', balloon: 'BALLOONS', lrpt: 'METEOR',
  fctlm: 'FUNCUBE', orbcomm: 'ORBCOMM', sondes: 'SONDES', vormap: 'VOR', rttyskim: 'RTTY',
};
export function galleryKindWord(k: string): string {
  const w = GALLERY_KIND_WORDS[String(k || '').toLowerCase()];
  return w || String(k || '').toUpperCase().slice(0, 16);
}

/** "just now" · "12 min ago" · "3 h ago" · "2 days ago" */
export function galleryAgeWords(t: number, nowSec: number): string {
  const s = Math.max(0, Math.floor(nowSec - t));
  if (s < 90) return 'just now';
  if (s < 3600) return `${Math.round(s / 60)} min ago`;
  if (s < 86400) return `${Math.floor(s / 3600)} h ago`;
  const d = Math.floor(s / 86400);
  return `${d} day${d === 1 ? '' : 's'} ago`;
}

const n0 = (v: unknown) => (typeof v === 'number' && isFinite(v) && v > 0 ? Math.floor(v) : 0);
const fmt = (n: number) => String(n).replace(/\B(?=(\d{3})+(?!\d))/g, ',');

/** ★★ The landing button's gate (AGENTS.md: no dead controls) and its caption —
 *  "12 today · newest METEOR 3 h ago", or "340 kept · nothing new today". */
export async function galleryOffer(get: Getter, nowSec = Math.floor(Date.now() / 1000)): Promise<GalleryOffer> {
  const none: GalleryOffer = { show: false, today: 0, count: 0, newest: null, line: '' };
  let j: any = null;
  try { j = await get('/vibeserver/gallery.json?offset=0&limit=1&since=86400'); } catch { j = null; }
  if (!j || typeof j !== 'object' || j.error || j.enabled === false) return none;
  const items: any[] = Array.isArray(j.items) ? j.items : [];
  const it = items.find((x) => x && typeof x.t === 'number' && typeof x.k === 'string') ?? null;
  // ★ an older server ignores since= (no "since" in its answer): its "total" is the whole gallery, not today's
  const knowsSince = typeof j.since === 'number';
  const count = n0(j.count) || n0(j.total) || items.length;
  const today = knowsSince ? n0(j.total) : (it && nowSec - it.t <= 86400 ? 1 : 0);
  if (!count && !today) return none;
  const newest = it && nowSec - it.t <= 86400 ? { k: String(it.k), t: it.t } : null;
  let line = '';
  if (newest && knowsSince) line = `${fmt(today)} today · newest ${galleryKindWord(newest.k)} ${galleryAgeWords(newest.t, nowSec)}`;
  else if (newest) line = `newest ${galleryKindWord(newest.k)} ${galleryAgeWords(newest.t, nowSec)}`;
  else line = `${fmt(count)} kept · nothing new today`;
  return { show: true, today, count, newest, line };
}
