// Station logo lookup for the FM-DX tuner (v7, plan §2d). Source: radio-browser.info
// — free, public-domain (CC0), HTTPS, no key. Matches on station NAME + country
// (radio-browser has no PI-code index), so hit rate depends on name quality;
// the tuner falls back to a monogram when this returns null.
//
// No backend, no fees. Results cached in-memory per (name|country) for the session.

const cache = new Map<string, string | null>();
const inflight = new Map<string, Promise<string | null>>();

// de1 is a stable radio-browser mirror; the `all.` host is round-robin DNS which
// some RN networking stacks resolve poorly, so we pin a mirror.
const HOST = 'https://de1.api.radio-browser.info';

function norm(s: string): string {
  return s.toLowerCase()
    .replace(/([a-z])(\d)/g, '$1 $2')     // radio2 -> radio 2 (see tidyStationName)
    .replace(/[^a-z0-9]+/g, ' ')
    .trim();
}

/** DAB service labels arrive UNSPACED — the ensemble sends "BBC Radio2", not
 *  "BBC Radio 2" (verified on-air). radio-browser's byname query is a substring
 *  match, so the unspaced form finds nothing at all, and the fuzzy scorer then
 *  can't match the token "radio2" against "radio" + "2" either. Split the digits
 *  back off before looking anything up. */
export function tidyStationName(s: string): string {
  return expandStationAbbrevs(s.replace(/([A-Za-z])(\d)/g, '$1 $2').replace(/\s+/g, ' ').trim());
}

/** ★★★ "BBC R2" IS NOT WHAT ANY DATABASE CALLS IT. The PS field is EIGHT CHARACTERS, so
 *  broadcasters abbreviate to fit — and the abbreviation is the one thing a name search cannot
 *  match: radio-browser lists "BBC Radio 2", the transmitter says "BBC R2", and the two share only
 *  the token "BBC". Every BBC network failed to find a logo for this reason alone (Stuart,
 *  2026-08-11, looking at the decoded PS: "BBC R2 is pretty obvious").
 *
 *  ★★ Expanded, not guessed at: R<digit> as a WHOLE TOKEN only. "R2" between spaces is a station
 *     number; the same letters inside a word are not, and rewriting those would break more names
 *     than it fixed. This is the one abbreviation common enough — and unambiguous enough — to be
 *     worth encoding.
 *  ★ Applied to the SEARCH only. What is displayed stays exactly what the transmitter sent; the
 *    RDS name is authoritative and this is about finding artwork for it, not renaming it. */
export function expandStationAbbrevs(s: string): string {
  return s
    .replace(/\bR\s?(\d)\b/gi, 'Radio $1')      // BBC R2  -> BBC Radio 2
    .replace(/\s+/g, ' ')
    .trim();
}

/** Resolve a station favicon URL by name (+ optional ISO country). Returns null
 *  when there's no confident match or on any error. HTTPS only. */
export async function lookupStationLogo(
  name: string, iso?: string, preferIso?: string,
): Promise<string | null> {
  // ★ Expand before normalising: "BBC R2" must become "BBC Radio 2" for BOTH the byname query
  //   and the token scorer, or the scorer still has nothing to match "radio"/"2" against.
  name = expandStationAbbrevs(name);
  const q = norm(name);
  if (!q || q.length < 3) return null;
  const key = `${q}|${iso ?? ''}|${preferIso ?? ''}`;
  if (cache.has(key)) return cache.get(key)!;
  if (inflight.has(key)) return inflight.get(key)!;

  const p = (async (): Promise<string | null> => {
    try {
      // byname/ is a substring match (search?name= over-filters and returns []).
      // Ordered by votes so the popular station wins a common-name query.
      // 40, not 10: the right station is often outside the top 10 by votes — widening
      // the pool is what turned "Absolute" from a miss into a hit.
      const url = `${HOST}/json/stations/byname/${encodeURIComponent(name)}?limit=40&order=votes&reverse=true&hidebroken=true`;
      const res = await fetch(url, { headers: { 'User-Agent': 'VibeSDR/7 (FM-DX tuner)' } });
      const rows: any[] = await res.json();
      const list = Array.isArray(rows) ? rows : [];
      // Country-filtered fuzzy match. The COUNTRY filter (from the transmitter's
      // ITU, reliable) is the safety net against wrong-country logos, so within
      // the country we take the best shared-token name match rather than exact —
      // databases name the same station differently ("Pride Radio" vs "Pride FM").
      const qTokens = q.split(' ').filter((t) => t.length > 1);
      let bestFav: string | null = null, bestScore = 0;
      for (const r of list) {
        const fav = String(r?.favicon ?? '');
        if (!fav.startsWith('https://')) continue;
        if (iso && String(r?.countrycode ?? '').toUpperCase() !== iso.toUpperCase()) continue;
        const rTokens = norm(String(r?.name ?? '')).split(' ').filter((t) => t.length > 1);
        // Count DISTINCT QUERY tokens accounted for — not database tokens matched.
        // Counting the other way let a repeated word inflate the score past 1.0:
        // "Kiss" against "Radio Kiss Kiss Italia" counted "kiss" twice and scored 1.84,
        // which then beat every legitimate match, including the right country's.
        const shared = qTokens.filter((t) => rTokens.includes(t)).length;
        if (shared === 0) continue;                       // need at least one real word in common

        // CONTAINMENT, not symmetric overlap. The question is "is the name I have fully
        // accounted for?", because the database routinely carries extra words the RDS
        // name doesn't ("FM", "Radio", a city). Scoring symmetrically —
        // shared / max(q, r) — meant "Heart" vs "Heart FM" scored 1/2 = 0.5 and was
        // then REJECTED by the 0.8 floor below, so a single-word station could
        // essentially never be matched without a country to anchor on. Since the RDS
        // country code rides in group 1A and many stations never send it, that is the
        // usual case: the lookup was quietly failing nearly always.
        let score = shared / qTokens.length;
        // Light penalty for a database name padded with words we didn't ask for, so
        // "Heart" prefers "Heart FM" over "Heart Dance Radio Network".
        const extra = Math.max(0, rTokens.length - shared);
        score -= extra * 0.08;
        // Same country as the receiver? A TIE-BREAK only — never a filter. Sporadic-E
        // and border reception mean a foreign station is perfectly legitimate.
        if (preferIso && String(r?.countrycode ?? '').toUpperCase() === preferIso.toUpperCase()) {
          score += 0.05;
        }
        if (score > bestScore) { bestScore = score; bestFav = fav; }
      }
      // Every query token has to be accounted for; padding is what the penalty trims.
      if (bestScore < 0.8) return null;
      return bestFav;
    } catch {
      return null;
    }
  })();

  inflight.set(key, p);
  const result = await p;
  inflight.delete(key);
  cache.set(key, result);
  return result;
}
