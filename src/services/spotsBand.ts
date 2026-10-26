/* spotsBand.ts — V12 DIGITAL SPOTS in the app: the server's mode list, BAND mode and the governor's chips
 * (docs/v12/DIGITAL-SPOTS.md). The drawing rules (colour, letter, band stepping, the drawn span) are the shared
 * src/utils/digiModes.ts; this is the part the web keeps in main.ts (spotsReadCap / spotsBandRender / spotsTitle /
 * renderSpotModeChips) and decoders.ts (digital_spots_status), as pure rules — ONE RULE, TWO READERS, held equal by
 * scripts/test_spots_band_app.ts.
 *
 * ★ The server says what exists (vibeserver.json): `spotModes` (the list filter, the Mode column, the map legend),
 *   `spotBands` (what Band steps through — only bands THIS radio reaches; absent = no Band key, AGENTS.md) and `spotsBand`
 *   true / false + `spotsBandWhy` (greyed with the reason — the owner's switch or the benchmark's default; never hidden).
 */
import { readSpotBands, spotBandAt, digiModeFamily, type SpotBand } from '../utils/digiModes';

export interface SpotsBandCap { ok: boolean; why: string; bands: SpotBand[] }

/** The modes this server decodes; an older server (none listed) = FT8 and FT4, as the web assumes. */
export const SPOT_MODES_DEFAULT: readonly string[] = ['FT8', 'FT4'];
export function parseSpotModes(raw: unknown): string[] | undefined {
  if (!Array.isArray(raw)) return undefined;
  const m = raw.filter((x) => typeof x === 'string').map((x: string) => x.replace(/[^0-9A-Za-z+-]/g, '').slice(0, 12))
    .filter(Boolean).slice(0, 16);
  return m.length ? m : undefined;
}

/** Band mode's capability: undefined = no Band key (no band reachable, or an older server). */
export function parseSpotsBandCap(j: any): SpotsBandCap | undefined {
  const bands = readSpotBands(j?.spotBands);
  if ((j?.spotsBand !== true && j?.spotsBand !== false) || !bands.length) return undefined;
  const why = typeof j.spotsBandWhy === 'string' ? j.spotsBandWhy.trim().slice(0, 200) : '';
  return { ok: j.spotsBand === true, why: j.spotsBand === true ? '' : (why || 'Band spots are not available on this receiver'), bands };
}

/** ★ The MODE filter's choices: ALL, then the server's modes (FT8 and FT4 on an older one) — never a fixed list (a key for
 *  a mode this server does not decode would be dead). */
export function spotFilterModes(spotModes: readonly string[] | undefined): string[] {
  return ['ALL', ...(spotModes && spotModes.length ? spotModes : SPOT_MODES_DEFAULT)];
}
/** Does a spot pass the MODE filter — by FAMILY (PSK31 / PSK63 → PSK), as the web's filteredSpots. */
export function spotModeMatches(spotMode: string, filter: string, spotModes: readonly string[] | undefined): boolean {
  if (filter === 'ALL') return true;
  return digiModeFamily(spotMode, spotModes && spotModes.length ? spotModes : SPOT_MODES_DEFAULT) === filter.toUpperCase();
}

/** "FT8 / FT4 SPOTS" (Single, a server with those two) · "DIGITAL SPOTS" (Single, more modes) · "DIGITAL SPOTS · 20 M
 *  BAND" (Band) — the web's spotsTitle. */
export function spotsTitleText(band: boolean, cap: SpotsBandCap | undefined, dialHz: number, spotModes: readonly string[] | undefined): string {
  if (band && cap) {
    const b = spotBandAt(cap.bands, dialHz);
    return b ? `DIGITAL SPOTS · ${b.name.toUpperCase()} BAND` : 'DIGITAL SPOTS · BAND';
  }
  const m = spotModes && spotModes.length ? spotModes : SPOT_MODES_DEFAULT;
  return m.length > 0 && m.length <= 2 ? m.join(' / ') + ' SPOTS' : 'DIGITAL SPOTS';
}

/** The Band key's line when it cannot run, and its tooltip-equivalent when it can (the web's spotsBandRender). */
export function spotsBandHint(cap: SpotsBandCap): string {
  return cap.ok ? 'A whole band’s digital frequencies at once — every mode on its own dial ('
    + cap.bands.map((x) => x.name).join(', ') + ')' : cap.why;
}

/* ★★ V12 BAND GOVERNOR (2026-10-27): WHICH MODES ARE LIVE. In Band the server decodes a mode only while its window has
 *  something in it and the box has the CPU (decoders/digi_governor.h) — so the header says, per mode, what it is doing.
 *  {"type":"digital_spots_status","data":{scope:"band",modes:[{mode,state,dials,idle,shed,live}],budget}} — when a state
 *  changes and every 15 s. */
export type SpotModeStateName = 'active' | 'light' | 'idle' | 'shed' | 'off';
export interface SpotModeState { mode: string; state: SpotModeStateName; dials: number; idle: number; shed: number; live: number }
const STATES: readonly string[] = ['active', 'light', 'idle', 'shed', 'off'];
/** The status frame's modes, checked as the web's decoders.ts: a state not in the five is dropped, at most 16. null = not one. */
export function parseSpotsStatus(d: any): SpotModeState[] | null {
  if (!d || !Array.isArray(d.modes)) return null;
  const n = (v: unknown) => (typeof v === 'number' && Number.isFinite(v) && v >= 0 ? Math.floor(v) : 0);
  return d.modes.slice(0, 16).filter((m: any) => m && typeof m.mode === 'string' && STATES.includes(m.state))
    .map((m: any) => ({ mode: m.mode.replace(/[^0-9A-Za-z+-]/g, '').slice(0, 12), state: m.state as SpotModeStateName,
                        dials: n(m.dials), idle: n(m.idle), shed: n(m.shed), live: n(m.live) }));
}
/** What a chip's state means — the web's SPOT_STATE_WORDS, word for word. */
export const SPOT_STATE_WORDS: Record<SpotModeStateName, string> = {
  active: 'decoding', light: 'decoding, one pass — the server is busy', idle: 'nothing in its window just now — not decoded',
  shed: 'signals there, but over this server\'s CPU budget — not decoded', off: 'switched off for Band by the server owner',
};
/** A chip's spoken / long text: "FT8: decoding" (+ " (3 periods shed)"). */
export function spotChipText(m: SpotModeState): string {
  return `${m.mode}: ${SPOT_STATE_WORDS[m.state] ?? m.state}` + (m.shed ? ` (${m.shed} period${m.shed === 1 ? '' : 's'} shed)` : '');
}
