import React, { useEffect, useState } from 'react';
import { Image, StyleSheet } from 'react-native';
import { resolveStationLogo } from '../services/stationLogoCache';
import { ituToIso, validIso } from '../services/rdsCountry';

/**
 * A station's logo, resolved lazily and cached.
 *
 * Renders nothing at all when there is no logo — a placeholder in a long list of
 * shortwave stations is just noise, and most of them will never have one.
 *
 * `itu` is EiBi's transmitter-country code and is AUTHORITATIVE: the schedule states
 * the country outright, so it is passed as a hard country filter rather than as the
 * receiver's country used as a mere preference. That makes an EiBi row's logo strictly
 * more trustworthy than one resolved from RDS, where the country often has to be
 * inferred from the PI nibble.
 */
export default function StationLogo({ name, itu, size = 18, uri }: {
  name?: string;
  itu?: string;
  size?: number;
  /** ★★ AN ALREADY-RESOLVED LOGO, resolved by somebody who knew MORE than this component does.
   *  The identity lookup needs the PI and the tuned FREQUENCY (RadioDNS is keyed on the bearer —
   *  one PI can be on several transmitters and the SPI lists each by frequency), and neither is
   *  available from a name and a country. A caller that has them should resolve once and pass the
   *  answer in, rather than have this quietly fall back to a name search and disagree with the
   *  rest of the app about what station it is looking at. */
  uri?: string | null;
}) {
  const [url, setUrl] = useState<string | null>(null);
  /** ★★★ THE WHITE PLATE MUST NOT PRECEDE THE PICTURE. `backgroundColor` was on the Image style
   *  unconditionally, so a URL that resolved but would not RENDER — an SVG or an .ico, which
   *  React Native cannot draw, or a host that 404s — left a bright white square sitting in the
   *  panel. Reported as "RadioDNS not showing properly" (Stuart, 2026-08-15), and the white box
   *  reads as a broken logo rather than as an absent one, which is a worse lie: it says "we found
   *  artwork and it looks like this". */
  const [loaded, setLoaded] = useState(false);

  useEffect(() => { setLoaded(false); }, [url, uri]);

  useEffect(() => {
    let cancelled = false;
    setUrl(null);
    if (uri) return;                      // already resolved by the caller — do not look it up
    const n = name?.trim();
    if (!n) return;
    // ★★ AN ISO ARRIVES HERE TOO, AND ituToIso() SILENTLY ATE IT. AdvRdsPanel passes
    //    `itu={p.countryIso}` — already an ISO ("GB") — but ITU_TO_ISO is keyed on ITU codes
    //    ("G", "D", "F"), so the lookup missed and the country fell to '', dropping the one filter
    //    that keeps a foreign station's artwork off a British one. Accept either: a two-letter
    //    value that is already a valid ISO is passed straight through.
    const iso = itu ? (validIso(itu.toUpperCase()) ? itu.toUpperCase() : ituToIso(itu)) : '';
    resolveStationLogo({ name: n, iso: iso || undefined })
      .then((u) => { if (!cancelled) setUrl(u); })
      .catch(() => {});
    return () => { cancelled = true; };
  }, [name, itu, uri]);

  const shown = uri || url;
  if (!shown) return null;
  return (
    <Image
      source={{ uri: shown }}
      style={[styles.logo, { width: size, height: size },
              loaded ? styles.plate : styles.bare]}
      resizeMode="contain"
      onLoad={() => setLoaded(true)}
      // ★ An image that fails stays transparent rather than becoming a white square.
      onError={() => setLoaded(false)}
    />
  );
}

const styles = StyleSheet.create({
  logo: {
    borderRadius: 3,
    marginRight: 6,
  },
  // ★ The plate exists because most station artwork is dark-on-transparent and would vanish on
  //   this background. It is applied only once the picture is actually on screen.
  plate: { backgroundColor: 'rgba(255,255,255,0.9)' },
  bare:  { backgroundColor: 'transparent' },
});
