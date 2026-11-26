/* ★★★ beta96 b96-setupux THE SERVER SCREEN IN SECTIONS (Stuart 11-26) — the rules, pure, so scripts/test_setup_sections.ts
 *  proves them without a phone.
 *
 *  "Made for the Sony TV: scrolling the long screen with a remote took ages." Every block of ServerModeScreen is a section:
 *  a header (title + one line saying what it is set to now), closed by default, remembered per device (AsyncStorage
 *  SECTIONS_KEY), OPEN ALL / CLOSE ALL at the top. A section holding something the owner MUST set shows its header RED
 *  with "!" and the reason, opens by itself, and clears live — and once the owner has fixed it, it stays open under their
 *  hands until they close it.
 *
 *  ★★ STUART'S ORDER, the same as the Setup page's (vibe_setup_page.h; scripts/test-setup-order.mjs):
 *    server: benchmark/CPU · [the device password section — b96-serverchoice] · networking incl. location · sharing ·
 *            audio · frame rate · EiBi · then the rest in their old order;
 *    radio:  name · operating mode · listeners · antenna · rate + gain + notches + direct sampling · where listeners may
 *            tune · gain / IF limits · modes and decoders · raw IQ · where new listeners land · background decoding ·
 *            then the rest in their old order.
 *  ★ A key not in the list is drawn AFTER the listed ones, in the order it was given — so a section another branch adds
 *    (b96-serverchoice's device password) is never lost, only placed; adding its key here puts it where it belongs. */

export type SectionGroup = 'server' | 'radio';

/** ★★★ b96-serverchoice HOOK: 'devicePassword' is the slot for the device-only "Change admin password / PIN" section —
 *  ServerModeScreen renders `serverChoiceSections.devicePassword` there when that branch provides it. */
export const SERVER_ORDER: readonly string[] = [
  'bench', 'protocol', 'devicePassword', 'security',
  'network', 'location', 'proxy', 'webClient',
  'community', 'adsbFeed', 'pskr',
  'audio', 'frameRate', 'eibi',
  // the rest, in their old order
  'gallery', 'landingMsg', 'battery',
];
export const RADIO_ORDER: readonly string[] = [
  'name', 'opMode', 'listeners', 'antenna', 'hardware', 'whereTune', 'gainLimits', 'modes', 'rawIq', 'landing', 'background',
  // the rest, in their old order
  'fmDefaults', 'timeLimit', 'power', 'idle', 'dropout', 'zoom',
];

export const SECTIONS_KEY = 'vs_setup_sections';

/** Stuart's order; unknown keys after, as given (stable). */
export function orderSections<T extends { key: string }>(list: T[], order: readonly string[]): T[] {
  const at = (k: string) => { const i = order.indexOf(k); return i < 0 ? order.length : i; };
  return list.map((s, i) => ({ s, i })).sort((a, b) => (at(a.s.key) - at(b.s.key)) || (a.i - b.i)).map((x) => x.s);
}

/** The saved open/closed map — anything unreadable is "nothing saved" (every section closed). */
export function parseStored(json: string | null | undefined): Record<string, number> {
  if (!json) return {};
  try {
    const j = JSON.parse(json);
    if (!j || typeof j !== 'object' || Array.isArray(j)) return {};
    const out: Record<string, number> = {};
    for (const [k, v] of Object.entries(j)) if (v === 0 || v === 1) out[k] = v;
    return out;
  } catch { return {}; }
}

export type SectionMemo = {
  stored: Record<string, number>;      // the owner's choice per section, remembered (1 open, 0 closed, absent = closed)
  dismissed: Record<string, string>;   // a red section closed by hand: the reason it was red when they did
  stay: Record<string, boolean>;       // its "!" cleared while it was open: it stays open until they close it
};
export const EMPTY_MEMO: SectionMemo = { stored: {}, dismissed: {}, stay: {} };

/** Is this section open? Remembered open · kept open after its "!" cleared · red (unless closed by hand while red for
 *  this same reason). */
export function sectionOpen(m: SectionMemo, key: string, must: string): boolean {
  if (m.stored[key] === 1) return true;
  if (m.stay[key]) return true;
  return !!must && m.dismissed[key] !== must;
}

/** A header pressed: flip it, remember the choice; closing a red one dismisses that reason only. */
export function toggleSection(m: SectionMemo, key: string, must: string): SectionMemo {
  const open = !sectionOpen(m, key, must);
  const stay = { ...m.stay }; delete stay[key];
  const dismissed = { ...m.dismissed };
  if (!open && must) dismissed[key] = must;
  return { stored: { ...m.stored, [key]: open ? 1 : 0 }, dismissed, stay };
}

/** OPEN ALL / CLOSE ALL over the sections shown. */
export function setAllSections(m: SectionMemo, items: { key: string; must: string }[], open: boolean): SectionMemo {
  const stored = { ...m.stored }, dismissed = { ...m.dismissed }, stay = { ...m.stay };
  for (const it of items) {
    stored[it.key] = open ? 1 : 0;
    delete stay[it.key];
    if (!open && it.must) dismissed[it.key] = it.must;
  }
  return { stored, dismissed, stay };
}

/** The "!" cleared while the section was open: keep it open (never snap shut under the owner's hands). */
export function mustCleared(m: SectionMemo, key: string): SectionMemo {
  return m.stay[key] ? m : { ...m, stay: { ...m.stay, [key]: true } };
}

/** What the header line says: the reason in red when there is one, else the summary. */
export function headerLine(summary: string, must: string): string {
  return must ? '! ' + must : summary;
}

// ── a few summaries the screen shares with the test ──

/** Operating mode, in the Setup page's words: "Unlocked" / "Locked 88–108 MHz" / "Single use · ADS-B". */
export function opModeSummary(use: 'single' | 'locked', single: string, singleName: string, centreHz: number, rateHz: number): string {
  if (single && single !== 'general') return 'Single use · ' + (singleName || single);
  if (use !== 'locked') return 'Unlocked';
  if (!(centreHz > 0) || !(rateHz > 0)) return 'Locked';
  const mhz = (hz: number) => { const v = hz / 1e6; return (v >= 100 ? v.toFixed(1) : v >= 10 ? v.toFixed(2) : v.toFixed(3)).replace(/\.?0+$/, ''); };
  return 'Locked ' + mhz(centreHz - rateHz / 2) + '–' + mhz(centreHz + rateHz / 2) + ' MHz';
}

/** No location: satellites and maps need one (the satellite engine's own "this server has no location set"). */
export function locationMust(mode: string, hasPlace: boolean, moving: boolean): string {
  if (moving) return '';   // ★ on the move: the nearest town is the location
  if (mode === 'manual' && hasPlace) return '';
  return 'Location not set: satellites and maps need it';
}
