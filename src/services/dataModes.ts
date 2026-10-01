/**
 * Modes where the PROFILE IS THE CONTENT — and tuning inside it is meaningless.
 *
 * These are not "modes you happen to be in". They are whole-profile modes: the
 * server dedicates the receiver to a fixed block (a DAB ensemble, the 1090 MHz
 * ADS-B channel, an ISM band) and decodes it. There is nothing to hunt for and
 * nowhere to go — the only thing a VFO can do is drag you OFF the block, which
 * kills the decode and is a nuisance to re-find.
 *
 * This existed already, but only for DAB, as the literal string 'dab' hardcoded
 * into five separate guards (the drum, the waterfall tap, direct tune, bandwidth,
 * and the watch crown). ADS-B had NO guard at all, so the crown would happily tune
 * you off 1090 MHz and stop the aircraft decoding. One predicate, consulted by every
 * guard, so the next data mode can't fall through the same gap.
 */
export function isWholeProfileMode(mode: string | undefined | null): boolean {
  if (!mode) return false;
  const m = String(mode).toLowerCase();
  // LoRa profiles are named per-spreading-factor (lora-sf7 …), hence the prefix.
  return WHOLE_PROFILE.has(m) || m.startsWith('lora');
}

/**
 * OWRX calls these "raw-IF standalone digimodes": their `underlying` modulation is
 * "empty" — they ARE the primary mod, rather than a decoder layered on top of an
 * analog carrier. (RTTY/SSTV/WEFAX/packet/ACARS are the other kind: they ride a real
 * usb/lsb/nfm sideband, so those you DO tune.)
 *
 * DAB is here for the same reason even though it isn't a digimode — it's locked to
 * its ensemble block.
 *
 * NOT included, deliberately: 'drm'. It's a whole-profile primary mode on the wire,
 * but it's a broadcast you genuinely tune between stations of.
 */
const WHOLE_PROFILE = new Set([
  'dab',
  'adsb',
  'ism',
  'wmbus',
  'meshtastic',
  'meshcore',
]);

/** The same modes as a list — what the mode box's longest-label test enumerates (a standalone digimode
 *  is its own label: "MESHTASTIC", constants/modeBox.ts composeModeLabel). */
export const WHOLE_PROFILE_MODES: readonly string[] = [...WHOLE_PROFILE];
