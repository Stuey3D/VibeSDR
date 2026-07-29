// Last-resort country derivation from a server's free-text name/location, for when
// coordinates can't place it — a missing/wrong GPS, or a tiny island the world map omits.
// Amateur SDR servers almost always name their location ("…Bermuda", "Graz, Austria",
// "Azores, Portugal", "Chatham Islands, New Zealand"), so a country-name match catches them.

import { COUNTRY_NAMES } from './countryNames';

// Longest names first so "papua new guinea" wins over "guinea", "united states" over "states".
const NAMES = Object.keys(COUNTRY_NAMES).sort((a, b) => b.length - a.length);

// Precompiled word-boundary matchers — avoid "chad" matching inside "Richard".
const escape = (s: string) => s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
const MATCHERS: Array<[RegExp, string]> = NAMES.map(n =>
  [new RegExp(`(^|[^a-z])${escape(n)}([^a-z]|$)`, 'i'), COUNTRY_NAMES[n]] as [RegExp, string],
);

/** ISO alpha-2 for the first country/territory name found in the text, or '' if none. */
export function countryFromText(text?: string | null): string {
  if (!text) return '';
  const t = ` ${text.toLowerCase()} `;
  for (const [re, iso] of MATCHERS) {
    if (re.test(t)) return iso;
  }
  return '';
}
