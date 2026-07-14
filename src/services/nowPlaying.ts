// FM MediaSession metadata mapping for the CarFM build.
//
// Gadgetbridge relays Android MediaMetadata (title / artist / album) into its
// music messages, which it forwards over BLE to the ESP32 display. So this
// mapping IS this app's contract with the rest of the system. Per the CarFM
// spec §5b the sensible default (tune later against how the ESP32 renders it):
//
//   RadioText (RT)    -> TITLE   (the per-song / rolling text)
//   Station name (PS) -> ARTIST  (stable identifier)
//   Frequency         -> ALBUM
//
// FM has no track position/duration and RDS carries no album art, so neither is
// faked here: the native session leaves position unset and reuses the app icon.

export interface FmRdsInput {
  /** RDS Programme Service name (PS), e.g. "BBC R2". */
  ps?: string | null;
  /** RDS RadioText (RT) — the rolling per-song / now-playing line. */
  rt?: string | null;
  /** Tuned frequency in Hz. */
  freqHz: number;
}

export interface FmNowPlaying {
  title: string;
  artist: string;
  album: string;
}

/** Format a broadcast-FM frequency the way a car radio shows it: "101.1 MHz". */
export function formatFmFreq(freqHz: number): string {
  // Broadcast FM sits on a 100 kHz (Region 1) / 200 kHz (Region 2) raster, so
  // one decimal place is what every car dial shows.
  return `${(freqHz / 1e6).toFixed(1)} MHz`;
}

/**
 * Map decoded RDS + the tuned frequency into the three MediaMetadata slots the
 * ESP32 display reads. Empty RDS fields fall back so the card never goes blank
 * before RDS locks — PS/RT can take several seconds to arrive after a tune.
 */
export function fmNowPlaying({ ps, rt, freqHz }: FmRdsInput): FmNowPlaying {
  const freq = formatFmFreq(freqHz);
  const station = (ps ?? '').trim();
  const text = (rt ?? '').trim();
  return {
    // RT is the per-song text; before it locks show the station, then the freq,
    // so the title line is never empty.
    title: text || station || freq,
    // PS is the stable station identifier; fall back to "FM <freq>".
    artist: station || `FM ${freq}`,
    album: freq,
  };
}
