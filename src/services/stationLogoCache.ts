// On-device station-logo cache for FM (and anything named), on the shared logo store (logoStore.ts — the bytes on
// disk, logoCachePolicy.ts — the rules, one copy with the web client). A logo discovered online (RadioDNS by identity,
// or a radio-browser name search, on ANY backend) is downloaded and remembered against the station's identity, so it
// displays OFFLINE later — e.g. discover Pride Radio's logo on an FM-DX server, then tune it locally on an offline
// RTL-SDR (the PI + ECC decode locally, no network).
// ★★ V12 (Stuart, 2026-10-30: "cache the logos and only change them when a new one is published"): checked in the
//    background at most daily (was a fortnight) and REPLACED ONLY WHEN THE PICTURE CHANGED (md5); a lookup that found
//    nothing is retried 1 min / 5 min / 30 min / daily instead of on every call; bounded (LRU) for 1 GB phones; and
//    keyed on country + PI + the bearer's frequency, as RadioDNS is — a name-only station (bookmarks) is kept too.

import { lookupStationLogo } from './stationLogo';
import { radioDnsLogo } from './radiodns';
import { receiverIso, eccForIso } from './rdsCountry';
import { fmLogoKey, nameLogoKey } from './logoCachePolicy';
import { resolveLogo, offerLogo, peekLogo, logoStoreReady, type LogoResolver } from './logoStore';

/**
 * Resolve a station logo, cache-first (offline-capable):
 *  1. Cached (by country+PI) → return the local file URI; refresh in the
 *     background if stale + online.
 *  2. Not cached → online radio-browser lookup, download, cache by PI.
 *  3. Offline + not cached → null (caller shows a monogram, no placeholder).
 */
export async function resolveStationLogo(
  opts: { pi?: string; name: string; iso?: string; ecc?: string; freqHz?: number;
          /** ★ The server's own identity answer (/vibeserver/stationlogo) — asked FIRST in the identity tier, and now
           *  CACHED like any other: its URL used to be drawn straight from the network every time. */
          server?: () => Promise<string | null> },
): Promise<string | null> {
  const { pi, name, iso, ecc, freqHz } = opts;
  // ★★★ A NAME IS NOT AN IDENTITY, AND A WRONG LOGO LOCKS. The cache is keyed on the PI — right,
  //     because the PS rotates — so whatever lands first is kept until the PI changes. One bad PS
  //     decode ("BBC R1" is two bits from "BBC R3") therefore sticks for good, which is exactly
  //     what Stuart photographed on 2026-08-11: a weak Radio 3 wearing the Radio 1 roundel.
  //     ▶ So ask RadioDNS FIRST, which is keyed on the PI + ECC + frequency the transmitter
  //       error-protects, and returns the broadcaster's OWN artwork. The name search stays as the
  //       fallback for the many stations that publish no SPI at all.
  //     ★★★ HEART IS NOT ONE OF THEM, whatever this comment used to say. It read "(Heart, FLEX —
  //         verified)", and that was wrong: 09660.c363.ce1.fm.radiodns.org resolves to
  //         rdns.musicradio.com, whose _radioepg._tcp SRV gives epg.musicradio.com:80 (checked
  //         2026-08-17, after Stuart said "heart does have radiodns as the pi gets it" — his Pi
  //         was fetching it while this file claimed it could not exist).
  //     ★★ A comment asserting a fact is a place a reader STOPS LOOKING. This one sent the search
  //        off to radio-browser's artwork for a station whose broadcaster publishes its own.
  //     ★ This is the app's PRIMARY case, not an edge one: driving local hardware there is no
  //       server to ask, so the in-app lookup is the only route to identity-keyed artwork.
  if (!name && !(pi && ecc && freqHz)) return null;
  const cIso = (iso || receiverIso() || '').toUpperCase();
  const key = pi ? fmLogoKey(pi, cIso, freqHz) : nameLogoKey(name, iso);
  // ★ The frequency-less key the pre-V12 store used (migrated): drawn while the bearer's own entry is first resolved.
  const legacy = pi ? fmLogoKey(pi, cIso) : '';
  // ★ The ECC is derived from the country when the station did not transmit one — which is most
  //   of them. See eccForIso: it is a table lookup against an ISO that has already been validated,
  //   not an assumption about where the signal came from.
  // ★★★ AND FALL BACK TO THE RECEIVER'S OWN POSITION. Without an ECC the RadioDNS lookup is
  //     skipped entirely (fqdnFor returns ''), so a server configured with only a Maidenhead
  //     locator — no country — never asked RadioDNS anything and every station dropped to the name
  //     search. That is why the phone "missed stations the Pi finds": the Pi's config carries a
  //     country and the phone's carried a grid square. See receiverIsoFromGrid.
  const gccEcc = ecc || eccForIso(iso, pi) || eccForIso(receiverIso(), pi);
  // ★ Identity first, name second — the store asks them in this order and never lets the second displace the first.
  const resolvers: LogoResolver[] = [];
  if (pi) resolvers.push({ src: 'radiodns', run: async () =>
    (await opts.server?.().catch(() => null))
    || (gccEcc && freqHz ? await radioDnsLogo(pi, gccEcc, freqHz).catch(() => '') : '') || null });
  // receiverIso() is a PREFERENCE, not a filter: it searches the receiver's own
  // country first (a global name search buries the local station — "Kiss" found a
  // Greek one because the UK one was nowhere near the top by votes) but still falls
  // back worldwide, so a sporadic-E catch from abroad is not excluded.
  if (name) resolvers.push({ src: 'name', run: () =>
    lookupStationLogo(name, iso, receiverIso() || undefined).catch(() => null) });
  await logoStoreReady();
  const now = peekLogo(key);
  const got = await resolveLogo(key, resolvers);
  if (got) return got;
  /* ★★ A MARQUEE PS IS SEVERAL NAMES. The store backs off per STATION, so "NOW ON" missing would otherwise hold back
   *  "BBC RADIO 1" for a minute. With nothing held, this fragment gets its own name search (lookupStationLogo caches per
   *  name, so a repeat costs nothing) and a hit is filed under the station — still only as a 'name' answer. */
  if (pi && name) {
    const u = await lookupStationLogo(name, iso, receiverIso() || undefined).catch(() => null);
    if (u) return (await offerLogo(key, 'name', u)) ?? u;
  }
  return now ?? (legacy ? peekLogo(legacy) : null);
}
