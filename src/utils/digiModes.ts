/**
 * digiModes.ts — DIGITAL SPOTS on the client side (V12, 2026-10-26): how each MODE is drawn, and Band mode's band
 * stepping. Shared by the web client (web/client/src/main.ts) and the app (MapOverlay.tsx injects the styles into its
 * map page) — ONE table, so a WSPR spot is the same colour and letter on both.
 *
 * ★★★ COLOURED BY MODE, NOT BY BAND (Stuart, 2026-10-26: "most of our users will be using one SDR at a time so only able
 *     to cover one band at a time"). With one radio every spot on the map is from the same band, so a band colour says
 *     nothing; the mode does.
 * ★★ COLOUR-BLIND SAFE, AND NEVER COLOUR ALONE. The fills are the Okabe–Ito palette (designed to stay distinct under
 *    the common colour-vision deficiencies), and every marker also carries a LETTER — "8", "4", "2", "J", "W" … — so two
 *    modes are told apart by shape even where two colours merge. `ink` is the letter's colour, chosen for contrast on
 *    its own fill; the marker's dark rim keeps the pale fills (FT2's yellow) readable on a light map, and the fills are
 *    bright enough for the dark one.
 * ★ The server says which modes it decodes (vibeserver.json `spotModes`) — this only says how to DRAW a mode. A mode not
 *   in the table (a new engine) still draws: grey, with its first letter.
 */

export interface DigiModeStyle { colour: string; ink: string; letter: string; }

export const DIGI_MODE_STYLE: Record<string, DigiModeStyle> = {
  FT8:    { colour: '#E69F00', ink: '#000000', letter: '8' },   // orange
  FT4:    { colour: '#56B4E9', ink: '#000000', letter: '4' },   // sky blue
  FT2:    { colour: '#F0E442', ink: '#000000', letter: '2' },   // yellow
  JS8:    { colour: '#009E73', ink: '#000000', letter: 'J' },   // bluish green
  WSPR:   { colour: '#CC79A7', ink: '#000000', letter: 'W' },   // reddish purple
  Q65:    { colour: '#0072B2', ink: '#FFFFFF', letter: 'Q' },   // blue
  FST4:   { colour: '#D55E00', ink: '#000000', letter: 'F' },   // vermillion
  FST4W:  { colour: '#D55E00', ink: '#000000', letter: 'w' },   // vermillion, its own letter (FST4's beacon form)
  MSK144: { colour: '#BBBBBB', ink: '#000000', letter: 'M' },   // grey
  /* ★ V12 (2026-10-27): JTTY and PSK join Band. The eight Okabe–Ito fills are all taken, so these two come from outside
   *  it, picked for distance from EVERY fill under protan, deutan and tritan simulation (scripts/test_digi_modes.ts):
   *  JTTY white (achromatic — no deficiency moves it; ΔE ≥ 18 from all, the nearest FT2's yellow under tritanopia)
   *  and PSK Paul Tol's purple #AA3377 (ΔE ≥ 16 from all, nearest FST4's vermillion; it reads 3:1 on the dark map). */
  JTTY:   { colour: '#FFFFFF', ink: '#000000', letter: 'T' },   // white
  PSK:    { colour: '#AA3377', ink: '#FFFFFF', letter: 'P' },   // purple (Tol) — PSK31 and PSK63 alike
};

/** The marker rim: dark, so a pale fill reads on a light map and every fill stands off a dark one. */
export const DIGI_MARKER_RIM = '#1A1A1A';

/** ★ The FAMILY a spot's mode belongs to, among `families` (the server's spotModes, or the style table): "PSK31" and
 *  "PSK63" → "PSK", "Q65-60A" → "Q65", "FT8" → "FT8". Unknown → the mode itself (upper case). */
export function digiModeFamily(mode: string, families: readonly string[] = Object.keys(DIGI_MODE_STYLE)): string {
  const m = String(mode || '').toUpperCase();
  const fam = families.map((f) => String(f).toUpperCase());
  if (fam.includes(m)) return m;
  const hit = fam.find((k) => m.startsWith(k + '-') || (m.startsWith(k) && /^[0-9]+$/.test(m.slice(k.length))));
  return hit ?? m;
}

/** How to draw `mode` ("FT8", "ft4", "Q65-60A" → Q65's, "PSK63" → PSK's). Unknown → grey and its first letter. */
export function digiModeStyle(mode: string): DigiModeStyle {
  const m = String(mode || '').toUpperCase();
  const exact = DIGI_MODE_STYLE[m];
  if (exact) return exact;
  const base = digiModeFamily(m);
  if (DIGI_MODE_STYLE[base]) return DIGI_MODE_STYLE[base];
  const letter = (m.match(/[A-Z0-9]/) || ['?'])[0];
  return { colour: '#9E9E9E', ink: '#000000', letter };
}

/** One band as the server lists it for Band mode (vibeserver.json `spotBands`, decoders/digi_bands.h Span). */
export interface SpotBand {
  id: string;       // "20m"
  name: string;     // "20 m"
  hz: number;       // the dial Band mode sits on: the span's centre
  lo: number;       // the span the registered modes cover (the drawn VFO)…
  hi: number;
  bandLo: number;   // …and the band's own edges (which band a dial is "on")
  bandHi: number;
  modes: string[];
}

/** Parse and bound the server's list. Anything malformed is dropped, never trusted. */
export function readSpotBands(raw: unknown): SpotBand[] {
  if (!Array.isArray(raw)) return [];
  const ok = (v: unknown) => typeof v === 'number' && Number.isFinite(v) && v > 0;
  return raw.slice(0, 32).filter((b: any) => b && typeof b.id === 'string' && typeof b.name === 'string'
    && ok(b.hz) && ok(b.lo) && ok(b.hi) && ok(b.bandLo) && ok(b.bandHi) && b.hi > b.lo && b.bandHi > b.bandLo)
    .map((b: any) => ({
      id: String(b.id).replace(/[^0-9a-z]/gi, '').slice(0, 8), name: String(b.name).replace(/[^0-9a-z ]/gi, '').slice(0, 12),
      hz: b.hz, lo: b.lo, hi: b.hi, bandLo: b.bandLo, bandHi: b.bandHi,
      modes: Array.isArray(b.modes) ? b.modes.filter((m: unknown) => typeof m === 'string').map((m: string) => m.replace(/[^0-9A-Za-z+-]/g, '').slice(0, 12)).slice(0, 16) : [],
    }));
}

/** The band a dial is on, or null. */
export function spotBandAt(bands: SpotBand[], hz: number): SpotBand | null {
  return bands.find((b) => hz >= b.bandLo && hz <= b.bandHi) ?? null;
}

/**
 * ★ BAND STEPPING — the arrows, the knob, the D-pad, the keys, a game controller, all of them through nudge(): the next
 * band `dir` (+1/−1) from wherever the dial is. On a band: the one after it. Between bands: the nearest that way.
 * CLAMPED at the ends, never wrapped (DAB's and ISM's rule). The SAME rule as decoders/digi_bands.h stepSpan — ONE RULE,
 * TWO READERS: scripts/test_digi_modes.ts runs the C++ test's cases against this one.
 * Returns the index into `bands`, or −1 when there are none.
 */
export function stepSpotBand(bands: SpotBand[], dialHz: number, dir: number): number {
  const n = bands.length;
  if (!n) return -1;
  for (let i = 0; i < n; i++)
    if (dialHz >= bands[i].bandLo && dialHz <= bands[i].bandHi) return Math.max(0, Math.min(n - 1, i + (dir < 0 ? -1 : 1)));
  if (dir >= 0) { for (let i = 0; i < n; i++) if (bands[i].hz > dialHz) return i; return n - 1; }
  for (let i = n - 1; i >= 0; i--) if (bands[i].hz < dialHz) return i;
  return 0;
}

/** The band Band mode starts on: the one the dial is on, else the one this browser last used, else 20 m, else the first. */
export function landingSpotBand(bands: SpotBand[], dialHz: number, lastId: string | null): SpotBand | null {
  return spotBandAt(bands, dialHz) ?? bands.find((b) => b.id === lastId) ?? bands.find((b) => b.id === '20m') ?? bands[0] ?? null;
}

/** ★ What Band mode draws as the VFO, relative to the dial: the span of the band the dial is on (an exact frequency typed
 *  inside the band moves the dial, and the span stays where the modes are). Off any band: the 24 kHz the server cuts. */
export function bandPassband(bands: SpotBand[], dialHz: number): [number, number] {
  const b = spotBandAt(bands, dialHz);
  if (!b) return [-12000, 12000];
  return [b.lo - dialHz, b.hi - dialHz];
}
