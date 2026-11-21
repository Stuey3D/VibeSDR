/**
 * ★★ V12 beta90 — THE SKIMMERS' BAND PICKER (Stuart 2026-11-21: "cw/rtty skimmer could have a band selection in the
 * decoder box header and the normal tune buttons could fine tune it if needed"). ONE copy for the web client's decoder
 * box (web/client/src/main.ts) and the app's (components/DecoderPanel), as utils/tuneWindow.ts is.
 *
 * The bands are the SERVER's (`skimBands` in vibeserver.json / hwinfo — vibe_skim_bands.h, its region's IARU plan): no
 * table lives here (feedback: the app has no hardcoded settings against a server). THE RULE is the server's too: the
 * skimmer's window starts at the bottom of the band's segment for its mode — dial = lo + width / 2 (40 m CW in Region 1
 * at 48 kHz = 7024). The tune buttons still move the dial; the picker then names the band the window is on, marked as
 * tuned by hand, or "—" when it is on no band — it never snaps back.
 */
export type SkimKind = 'cw' | 'rtty' | 'psk';
export type SkimBand = { id: string; name: string; lo: number; hi: number };
export type SkimBands = { region: number; cw: SkimBand[]; rtty: SkimBand[]; psk: SkimBand[] };

/** The server's `skimBands` object, or null (an older server: no picker is drawn). */
export function parseSkimBands(j: any): SkimBands | null {
  if (!j || typeof j !== 'object') return null;
  const list = (a: any): SkimBand[] => (Array.isArray(a) ? a.filter((b: any) => b && typeof b.id === 'string' && +b.lo > 0 && +b.hi > +b.lo)
    .map((b: any) => ({ id: b.id, name: String(b.name || b.id), lo: +b.lo, hi: +b.hi })) : []);
  const out = { region: +j.region || 1, cw: list(j.cw), rtty: list(j.rtty), psk: list(j.psk) };
  return out.cw.length || out.rtty.length || out.psk.length ? out : null;
}

/** ★★ THE RULE: where the dial goes for a band at this width. */
export const skimDial = (b: SkimBand, width: number) => Math.round(b.lo + width / 2);

/**
 * The bands a listener may pick: the window (dial ± width / 2) must be tunable end to end — `canTune(hz)` is the page's
 * own clamp (the locked window, the owner's ranges, the radio's reach). A band whose window does not fit is not offered;
 * none fits → an empty list, and the picker is not drawn (no dead control).
 */
export function skimPickable(bands: SkimBand[], width: number, canTune: (hz: number) => boolean): SkimBand[] {
  return bands.filter((b) => { const c = skimDial(b, width); return canTune(c - width / 2) && canTune(c + width / 2); });
}

/** What the picker shows for this dial: the band whose rule-centre it is (`exact`), else the band the window is on
 *  (fine-tuned by hand), else none. */
export function skimBandAtDial(bands: SkimBand[], width: number, dialHz: number): { band: SkimBand | null; exact: boolean } {
  const ex = bands.find((b) => Math.abs(skimDial(b, width) - dialHz) < 1);
  if (ex) return { band: ex, exact: true };
  const on = bands.find((b) => dialHz - width / 2 < b.hi && dialHz + width / 2 > b.lo);
  return { band: on || null, exact: false };
}

/** The picker's label for a band not chosen exactly: "40 m · tuned" (fine-tuned off its rule-centre) or "—". */
export const skimOffLabel = (at: { band: SkimBand | null; exact: boolean }) => (at.band ? at.band.name + ' · tuned' : '—');
