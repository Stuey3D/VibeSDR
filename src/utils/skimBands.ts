/**
 * ★★ V12 beta90 — THE SKIMMERS' BANDS AND WINDOWS, for every client but the Setup page (which runs its own copy of the
 * same few lines in vibe_setup_page.h): the web client's decoder box (web/client/src/main.ts), the app's ModeSelector,
 * and the Android server screen's Background Decoding rows (services/bgDecode.ts).
 *
 * The bands AND their windows are the SERVER's (`skimBands` in vibeserver.json / hwinfo / backgroundCapsJson —
 * vibe_skim_bands.h points()): each band's segment tiled at the skimmer's width from the bottom (40 m CW R1 at 24 kHz:
 * 7012, 7036), then the region's practice segments by the same rule, labelled ("US RTTY").
 * Nothing is computed here but a window's edges (centre ± width / 2). An OLDER server (bands without `points`) gets the
 * one rule-centre per band: lo + width / 2.
 *
 * Listener decoder box (Stuart: "a band selection in the decoder box header and the normal tune buttons could fine tune
 * it"): a band pick goes to its first window; the band's other windows step from there; the tune buttons still move the
 * dial, and the row then reads "40 m · tuned" or "—" — it never snaps back.
 */
export type SkimKind = 'cw' | 'rtty' | 'psk';
export type SkimPointJson = { hz: number; note?: string };
export type SkimBand = { id: string; name: string; lo: number; hi: number; points?: Record<string, SkimPointJson[]> };
export type SkimBands = { region: number; cw: SkimBand[]; rtty: SkimBand[]; psk: SkimBand[] };
/** One window: its band, centre and edges (Hz), and a note ("US RTTY") or ''. */
export type SkimPoint = { band: SkimBand; hz: number; lo: number; hi: number; note: string };

/** The server's `skimBands` object, or null (an older server: no picker is drawn). */
export function parseSkimBands(j: any): SkimBands | null {
  if (!j || typeof j !== 'object') return null;
  const pts = (p: any): Record<string, SkimPointJson[]> | undefined => {
    if (!p || typeof p !== 'object') return undefined;
    const o: Record<string, SkimPointJson[]> = {};
    for (const k of Object.keys(p)) if (Array.isArray(p[k]))
      o[k] = p[k].filter((q: any) => q && +q.hz > 0).map((q: any) => (q.note ? { hz: +q.hz, note: String(q.note) } : { hz: +q.hz }));
    return o;
  };
  const list = (a: any): SkimBand[] => (Array.isArray(a) ? a.filter((b: any) => b && typeof b.id === 'string' && +b.lo > 0 && +b.hi > +b.lo)
    .map((b: any) => { const p = pts(b.points); return { id: b.id, name: String(b.name || b.id), lo: +b.lo, hi: +b.hi, ...(p ? { points: p } : {}) }; }) : []);
  const out = { region: +j.region || 1, cw: list(j.cw), rtty: list(j.rtty), psk: list(j.psk) };
  return out.cw.length || out.rtty.length || out.psk.length ? out : null;
}

/** ★★ A band's windows at `width`: the server's points, or (an older server) the one rule-centre lo + width / 2. */
export function skimPoints(b: SkimBand, width: number): SkimPoint[] {
  const raw: SkimPointJson[] = b.points ? (b.points[String(width)] ?? []) : [{ hz: Math.round(b.lo + width / 2) }];
  return raw.map((q) => ({ band: b, hz: q.hz, lo: q.hz - width / 2, hi: q.hz + width / 2, note: q.note ?? '' }));
}
/** Every band's windows, in the server's order (band by band). */
export const skimAllPoints = (bands: SkimBand[], width: number): SkimPoint[] => bands.flatMap((b) => skimPoints(b, width));
/** The listed window a centre is (to the hertz), or null — any other centre is the owner's own / tuned by hand. */
export const skimPointAt = (bands: SkimBand[], width: number, hz: number): SkimPoint | null =>
  hz > 0 ? skimAllPoints(bands, width).find((q) => Math.abs(q.hz - hz) < 1) ?? null : null;
const kHz = (hz: number) => (hz / 1e3).toFixed(1).replace(/\.0$/, '');
/** "7000–7024 kHz" (+ " · US RTTY") — the window as a picker reads it. */
export const skimPointLabel = (q: SkimPoint) => kHz(q.lo) + '–' + kHz(q.hi) + ' kHz' + (q.note ? ' · ' + q.note : '');

/** ★ The first window's centre (kept for callers that place by band). */
export const skimDial = (b: SkimBand, width: number) => skimPoints(b, width)[0]?.hz ?? Math.round(b.lo + width / 2);

/**
 * The bands a listener may pick, each with only the windows this radio can tune end to end (`canTune(hz)` is the page's
 * own clamp: the locked window, the owner's ranges, the radio's reach). A band with none is not offered; none at all →
 * an empty list, and the row is not drawn (no dead control).
 */
export function skimPickable(bands: SkimBand[], width: number, canTune: (hz: number) => boolean): { band: SkimBand; points: SkimPoint[] }[] {
  return bands.map((band) => ({ band, points: skimPoints(band, width).filter((q) => canTune(q.lo) && canTune(q.hi)) }))
    .filter((x) => x.points.length > 0);
}

/** What the row shows for this dial: the window it is exactly on (`exact`), else the band the window overlaps (tuned by
 *  hand), else none. */
export function skimBandAtDial(bands: SkimBand[], width: number, dialHz: number): { band: SkimBand | null; point: SkimPoint | null; exact: boolean } {
  const ex = skimPointAt(bands, width, dialHz);
  if (ex) return { band: ex.band, point: ex, exact: true };
  const on = bands.find((b) => dialHz - width / 2 < b.hi && dialHz + width / 2 > b.lo);
  return { band: on || null, point: null, exact: false };
}

/** The row's label for a dial on no listed window: "40 m · tuned" (fine-tuned) or "—". */
export const skimOffLabel = (at: { band: SkimBand | null; exact: boolean }) => (at.band ? at.band.name + ' · tuned' : '—');
