/**
 * dabSlide — the query that names the slideshow picture a DAB report describes (2026-10-19).
 *
 * ★★★ WHY (Stuart: "light everywhere … light on data"). The playing service's slide used to be fetched as
 *     `/vibeserver/dabslide?seq=N`, a parameter the server ignored: the picture went out `no-cache` with no validator,
 *     and `seq` restarts from 0 with the server — so after a restart `?seq=3` named a DIFFERENT picture than the one a
 *     phone had cached under it (Android's Fresco caches by URL alone). A dab=2 live report now carries `h`, the hash of
 *     the picture's bytes (vibe_dab_service.h Slide::hash); `?h=` is a URL unique to the picture, which the server sends
 *     `immutable` — fetched once, then never asked for again. An older server sends no `h`: `?seq=` as before.
 * ★ Shared by the app (DabPanel) and the web page (main.ts), so the two cannot drift.
 * ★ `h` came off the wire: only 16 lowercase hex characters go into a URL.
 */
export function dabSlideQuery(slide: { seq?: number; h?: string } | null | undefined): string {
  if (!slide || !slide.seq) return '';
  if (typeof slide.h === 'string' && /^[0-9a-f]{16}$/.test(slide.h)) return `h=${slide.h}`;
  return `seq=${slide.seq}`;
}
