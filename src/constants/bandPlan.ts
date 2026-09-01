import type { SDRMode } from '../services/UberSDRClient';

export type BandType = 'ham' | 'broadcast' | 'utility';

export interface Band {
  lo: number;
  hi: number;
  name: string;
  type: BandType;
  bandLabel?: string;
  regions?: number[];
  /** Demodulator applied when this band is explicitly selected, or auto-applied
   *  on boundary crossing while in a car. Undefined = don't touch the mode. */
  mode?: SDRMode;
  /** Tuning step (Hz) applied alongside `mode`. Undefined = don't touch step. */
  step?: number;
}

// `mode`/`step` drive the band-aware tuning (explicit band selection always;
// boundary crossing only in a car). Conventions: SW broadcast = AM/1 kHz; MW/AM
// broadcast = AM with 9 kHz (ITU R1) or 10 kHz (R2/3) spacing; HF ham phone =
// LSB below 10 MHz, USB at/above, 500 Hz; 60m is USB (the odd one below 10 MHz);
// LF/MF ham + 30m = CW (data/CW only, no phone), 100 Hz; utility/beacon = USB
// 500 Hz; CB is receiver-region split (R1/R3 = NFM, R2 = AM, 10 kHz). Steps must
// be members of STEPS (sdrTypes.ts).
export const BAND_PLAN: Band[] = [
  { lo: 9000,      hi: 148500,    name: 'LW Broadcast Band',         type: 'broadcast', mode: 'am',  step: 9000 },
  { lo: 135700,    hi: 137800,    name: '2200m Ham Band',             type: 'ham',  bandLabel: '2200m', mode: 'cwu', step: 100 },
  { lo: 148500,    hi: 283500,    name: 'NDB / Navigational Beacons', type: 'utility', mode: 'usb', step: 500 },
  { lo: 283500,    hi: 525000,    name: 'NDB / Maritime Beacons',     type: 'utility', mode: 'usb', step: 500 },
  { lo: 472000,    hi: 479000,    name: '630m Ham Band',              type: 'ham',  bandLabel: '630m', mode: 'cwu', step: 100 },
  { lo: 525000,    hi: 1605000,   name: 'AM Broadcast Band',          type: 'broadcast', regions: [2, 3], mode: 'am', step: 10000 },
  { lo: 525000,    hi: 1705000,   name: 'AM Broadcast Band',          type: 'broadcast', regions: [1], mode: 'am', step: 9000 },
  { lo: 1800000,   hi: 2000000,   name: '160m Ham Band',              type: 'ham',  bandLabel: '160m', mode: 'lsb', step: 500 },
  { lo: 2300000,   hi: 2495000,   name: '120m Tropical Broadcast',    type: 'broadcast', mode: 'am', step: 1000 },
  { lo: 2495000,   hi: 2850000,   name: '90m Tropical Broadcast',     type: 'broadcast', mode: 'am', step: 1000 },
  { lo: 3500000,   hi: 3800000,   name: '80m Ham Band',               type: 'ham',  bandLabel: '80m', regions: [1], mode: 'lsb', step: 500 },
  { lo: 3500000,   hi: 4000000,   name: '80m Ham Band',               type: 'ham',  bandLabel: '80m', regions: [2], mode: 'lsb', step: 500 },
  { lo: 3500000,   hi: 3900000,   name: '80m Ham Band',               type: 'ham',  bandLabel: '80m', regions: [3], mode: 'lsb', step: 500 },
  { lo: 3800000,   hi: 4000000,   name: '75m Broadcast Band',         type: 'broadcast', regions: [1, 3], mode: 'am', step: 1000 },
  { lo: 3900000,   hi: 4000000,   name: '75m Broadcast Band',         type: 'broadcast', regions: [2], mode: 'am', step: 1000 },
  { lo: 5250000,   hi: 5450000,   name: '60m Ham Band',               type: 'ham',  bandLabel: '60m', mode: 'usb', step: 500 },
  { lo: 5900000,   hi: 6200000,   name: '49m Broadcast Band',         type: 'broadcast', mode: 'am', step: 1000 },
  { lo: 7000000,   hi: 7200000,   name: '40m Ham Band',               type: 'ham',  bandLabel: '40m', regions: [1, 3], mode: 'lsb', step: 500 },
  { lo: 7000000,   hi: 7300000,   name: '40m Ham Band',               type: 'ham',  bandLabel: '40m', regions: [2], mode: 'lsb', step: 500 },
  { lo: 7200000,   hi: 7450000,   name: '41m Broadcast Band',         type: 'broadcast', regions: [1, 3], mode: 'am', step: 1000 },
  { lo: 7300000,   hi: 7450000,   name: '41m Broadcast Band',         type: 'broadcast', regions: [2], mode: 'am', step: 1000 },
  { lo: 9400000,   hi: 9900000,   name: '31m Broadcast Band',         type: 'broadcast', mode: 'am', step: 1000 },
  { lo: 10100000,  hi: 10150000,  name: '30m Ham Band',               type: 'ham',  bandLabel: '30m', mode: 'cwu', step: 100 },
  { lo: 11600000,  hi: 12100000,  name: '25m Broadcast Band',         type: 'broadcast', mode: 'am', step: 1000 },
  { lo: 13570000,  hi: 13870000,  name: '22m Broadcast Band',         type: 'broadcast', mode: 'am', step: 1000 },
  { lo: 14000000,  hi: 14350000,  name: '20m Ham Band',               type: 'ham',  bandLabel: '20m', mode: 'usb', step: 500 },
  { lo: 15100000,  hi: 15800000,  name: '19m Broadcast Band',         type: 'broadcast', mode: 'am', step: 1000 },
  { lo: 18068000,  hi: 18168000,  name: '17m Ham Band',               type: 'ham',  bandLabel: '17m', mode: 'usb', step: 500 },
  { lo: 17480000,  hi: 17900000,  name: '16m Broadcast Band',         type: 'broadcast', mode: 'am', step: 1000 },
  { lo: 21000000,  hi: 21450000,  name: '15m Ham Band',               type: 'ham',  bandLabel: '15m', mode: 'usb', step: 500 },
  { lo: 21450000,  hi: 21850000,  name: '13m Broadcast Band',         type: 'broadcast', mode: 'am', step: 1000 },
  { lo: 24890000,  hi: 24990000,  name: '12m Ham Band',               type: 'ham',  bandLabel: '12m', mode: 'usb', step: 500 },
  { lo: 26965000,  hi: 27405000,  name: '11m CB Band',                type: 'utility', regions: [1, 3], mode: 'nfm', step: 10000 },
  { lo: 26965000,  hi: 27405000,  name: '11m CB Band',                type: 'utility', regions: [2], mode: 'am', step: 10000 },
  { lo: 28000000,  hi: 29700000,  name: '10m Ham Band',               type: 'ham',  bandLabel: '10m', mode: 'usb', step: 500 },

  // ── VHF / UHF (OWRX & other wideband backends; UberSDR caps at 30 MHz) ──────
  // Steps here come from STEPS_VHF [1k,5k,12.5k,25k,50k,100k] (sdrTypes.ts) since
  // getStepsFor(hz) switches at 30 MHz. Broadcast-FM and DAB leave `mode` unset:
  // WFM/DAB are server-side demods (the OWRX profile's start_mod selects WFM, and
  // DAB is chosen from the digital picker) — pinning them to narrow nfm/am here
  // would yank the audio to the wrong demodulator on a boundary cross.
  { lo: 30000000,  hi: 50000000,   name: 'VHF Low / Public Service',   type: 'utility', mode: 'nfm', step: 12500 },
  { lo: 50000000,  hi: 54000000,   name: '6m Ham Band',                type: 'ham',  bandLabel: '6m',  mode: 'usb', step: 1000 },
  { lo: 70000000,  hi: 70500000,   name: '4m Ham Band',                type: 'ham',  bandLabel: '4m',  regions: [1], mode: 'usb', step: 1000 },
  { lo: 87500000,  hi: 108000000,  name: 'FM Broadcast Band',          type: 'broadcast', step: 100000 },
  { lo: 108000000, hi: 137000000,  name: 'Airband (VHF Air)',          type: 'utility', mode: 'am',  step: 25000 },
  { lo: 144000000, hi: 146000000,  name: '2m Ham Band',                type: 'ham',  bandLabel: '2m',  regions: [1], mode: 'nfm', step: 12500 },
  { lo: 144000000, hi: 148000000,  name: '2m Ham Band',                type: 'ham',  bandLabel: '2m',  regions: [2, 3], mode: 'nfm', step: 12500 },
  { lo: 156000000, hi: 162050000,  name: 'Marine VHF',                 type: 'utility', mode: 'nfm', step: 25000 },
  { lo: 162400000, hi: 162550000,  name: 'NOAA Weather Radio',         type: 'utility', regions: [2], mode: 'nfm', step: 25000 },
  { lo: 174000000, hi: 240000000,  name: 'DAB / DAB+ (Band III)',      type: 'broadcast', step: 1000 },
  { lo: 222000000, hi: 225000000,  name: '1.25m Ham Band',             type: 'ham',  bandLabel: '1.25m', regions: [2], mode: 'nfm', step: 12500 },
  { lo: 420000000, hi: 450000000,  name: '70cm Ham Band',              type: 'ham',  bandLabel: '70cm', regions: [2], mode: 'nfm', step: 25000 },
  { lo: 430000000, hi: 440000000,  name: '70cm Ham Band',              type: 'ham',  bandLabel: '70cm', regions: [1, 3], mode: 'nfm', step: 25000 },
  // ★ PMR446 is TWO allocations with different rasters, and treating it as one
  //   put digital users on the wrong grid. Analogue is 16 channels at 12.5 kHz
  //   across the lower half; the upper half is digital (dPMR446 / DMR446) on the
  //   6.25 kHz raster. Split so tuning in lands on the right step either side.
  { lo: 446000000, hi: 446100000,  name: 'PMR446 (analogue)',           type: 'utility', regions: [1], mode: 'nfm', step: 12500 },
  { lo: 446100000, hi: 446200000,  name: 'PMR446 (digital)',            type: 'utility', regions: [1], mode: 'nfm', step: 6250 },
  { lo: 462000000, hi: 468000000,  name: 'FRS / GMRS',                 type: 'utility', regions: [2], mode: 'nfm', step: 12500 },
  { lo: 1240000000, hi: 1300000000, name: '23cm Ham Band',            type: 'ham',  bandLabel: '23cm', mode: 'nfm', step: 25000 },

  /* ── SPACE AND SATELLITE ───────────────────────────────────────────────────────────────────
   * ★★★ WHAT IS UP THERE, IN THE PART OF THE SPECTRUM PEOPLE CAN ACTUALLY REACH. Stuart,
   *   2026-09-01: "would've been cool to see the satellite frequencies and military satcoms etc."
   *   Every entry here was previously BLANK — a dial that goes there and names nothing.
   * ★★★ SERVICE ALLOCATIONS, NOT THE IEEE LETTER BANDS (L/S/C/X/Ku). The letters are radar
   *   nomenclature rather than allocations, they overlap everything, and they disagree with the
   *   hardware: 10489.5 MHz is X band by IEEE while the LNB you point at it is sold as Ku. A
   *   label that argues with the box in the user's hand is worse than no label. These say what is
   *   THERE instead, which is the useful half.
   * ★★ Most of it is below 2 GHz and needs no converter at all — an ordinary dongle reaches every
   *   one of these, which is what makes them worth labelling.
   * ★ MODE ONLY WHERE THE CONVENTION IS REAL. ADS-B, Iridium and Inmarsat are digital formats no
   *   analogue demodulator here can resolve, so they name the band and touch nothing. */
  { lo: 137000000,  hi: 138000000,  name: 'Weather Satellites (APT / LRPT)', type: 'utility' },
  /* ★★ THE ONE MOST PEOPLE MEAN BY "MILSATCOM". The UHF SATCOM downlink is where the
   *   well-documented unauthorised traffic on the old FLTSATCOM/UFO birds turns up, on a 25 kHz
   *   raster in narrow FM — a convention solid enough to set the mode from. The uplink is listed
   *   too so the band is not a mystery when a dial lands in it. */
  { lo: 240000000,  hi: 270000000,  name: 'UHF Military Satcom (downlink)', type: 'utility', mode: 'nfm', step: 25000 },
  { lo: 292000000,  hi: 318000000,  name: 'UHF Military Satcom (uplink)',   type: 'utility', mode: 'nfm', step: 25000 },
  { lo: 400150000,  hi: 406000000,  name: 'Radiosondes / Met Satellites',   type: 'utility', mode: 'nfm', step: 12500 },
  { lo: 406000000,  hi: 406100000,  name: 'Distress Beacons (COSPAS-SARSAT)', type: 'utility' },
  { lo: 977000000,  hi: 979000000,  name: 'ADS-B UAT',                      type: 'utility', regions: [2] },
  /* ★★★ 1090 WAS NOT IN THIS PLAN AT ALL, which is odd for an app that decodes it — the aircraft
   *   panel has existed for months and the dial that feeds it named the frequency nothing. */
  { lo: 1089000000, hi: 1091000000, name: 'ADS-B (Mode S, 1090 MHz)',       type: 'utility' },
  { lo: 1525000000, hi: 1559000000, name: 'Inmarsat (AERO / STD-C)',        type: 'utility' },
  { lo: 1574000000, hi: 1577000000, name: 'GPS L1',                         type: 'utility' },
  { lo: 1616000000, hi: 1626500000, name: 'Iridium',                        type: 'utility' },

  /* ── MICROWAVE, above 23cm ─────────────────────────────────────────────────────────────────
   * ★★★ ADDED FOR THE CONVERTERS. This plan stopped at 1300 MHz, which was fine while nothing
   *   could tune higher — and a down-converter changes that: a 9750 MHz LNB puts a listener at
   *   10489.5 MHz, where every band-keyed feature returned nothing and the label was simply
   *   blank. Nothing was BROKEN by that (getBandsAtRegion returns an empty list and callers leave
   *   the mode alone), but a dial that can reach a band and cannot name it is a dial that looks
   *   like it does not know where it is.
   *
   * ★★★ LABELS ONLY, NO mode/step, FOR THE WIDE ONES — and that is the honest choice rather than
   *   a lazy one. 3cm is 500 MHz wide and carries narrowband SSB, FM, ATV and beacons; picking a
   *   demodulator for the whole span would be inventing a convention and would fire on every
   *   boundary crossing in a car. `mode`/`step` undefined means "don't touch", which is exactly
   *   right for a band whose contents vary by where in it you are.
   *   ★ QO-100's narrowband transponder is the exception, because there the convention is real
   *     and universal: USB, and it is why most people own an LNB at all.
   *
   * ★★ REGIONS WHERE THEY GENUINELY DIFFER. 9cm is the big one — R1 has a narrow 3400-3475
   *   allocation while R2/R3 have 3300-3500 — so it is split rather than averaged into a span
   *   that is wrong in both places. */
  { lo: 2300000000,  hi: 2450000000,  name: '13cm Ham Band',           type: 'ham',  bandLabel: '13cm' },
  { lo: 3400000000,  hi: 3475000000,  name: '9cm Ham Band',            type: 'ham',  bandLabel: '9cm', regions: [1] },
  { lo: 3300000000,  hi: 3500000000,  name: '9cm Ham Band',            type: 'ham',  bandLabel: '9cm', regions: [2, 3] },
  { lo: 5650000000,  hi: 5850000000,  name: '6cm Ham Band',            type: 'ham',  bandLabel: '6cm' },
  /* ★★★ THE ONE MOST PEOPLE POINT A DISH AT, AND IT MUST COME FIRST. The QO-100 (Es'hail-2)
   *   narrowband transponder — SSB, CW and the beacons — sits INSIDE 3cm, and bandTuneDefaults
   *   sorts by band TYPE and then takes the first: both of these are 'ham', so a stable sort
   *   leaves them in array order and whichever is written first wins. With 3cm above, the wide
   *   band won and QO-100's USB convention never fired — the specific allocation losing to the
   *   general one purely because of where it sat in a list.
   * ★ 500 Hz is a member of STEPS_VHF, the ladder in force above 30 MHz. */
  { lo: 10489500000, hi: 10490000000, name: 'QO-100 Narrowband',       type: 'ham',  bandLabel: 'QO-100', mode: 'usb', step: 500 },
  { lo: 10000000000, hi: 10500000000, name: '3cm Ham Band',            type: 'ham',  bandLabel: '3cm' },
  { lo: 24000000000, hi: 24250000000, name: '1.2cm Ham Band',          type: 'ham',  bandLabel: '1.2cm' },
];

/** Band type → colour, as HEX and in ONE place.
 *
 *  The waterfall's band plan already had these (as rgba strings, local to
 *  WaterfallView), and the watch needs the same colours — so they move here rather than
 *  being typed out a second time. Two lists of brand colours is one list of brand colours
 *  and one bug waiting to be noticed. WaterfallView now derives its rgba from these. */
export const BAND_HEX: Record<string, string> = {
  ham:       '#CF0000',
  broadcast: '#0900FF',
  utility:   '#07BD00',
  cb:        '#FF7700',
};

/** 11m CB is typed 'utility' in the plan above but has always been coloured orange. */
export function bandHex(b: Band): string {
  if (b.name.includes('CB')) return BAND_HEX.cb;
  return BAND_HEX[b.type] ?? BAND_HEX.utility;
}

export function getBandsAt(hz: number): Band[] {
  return BAND_PLAN.filter(b => hz >= b.lo && hz <= b.hi);
}

export function getPrimaryBandAt(hz: number): Band | null {
  const bands = getBandsAt(hz);
  if (!bands.length) return null;
  const ham = bands.find(b => b.type === 'ham');
  return ham ?? bands[0];
}

/** Region-aware lookup (skin getBandsAt): bands restricted to other ITU
 *  regions are skipped, duplicates by name+type collapsed. region 0 = unknown
 *  → no filtering. */
export function getBandsAtRegion(hz: number, region: number): Band[] {
  const seen: Record<string, boolean> = {};
  const out: Band[] = [];
  for (const b of BAND_PLAN) {
    if (hz < b.lo || hz > b.hi) continue;
    if (b.regions && b.regions.length && region && !b.regions.includes(region)) continue;
    const key = b.name + '|' + b.type;
    if (!seen[key]) { seen[key] = true; out.push(b); }
  }
  return out;
}

/** Mode + step to apply for the band containing `hz` (ham preferred over
 *  broadcast/utility, matching the VTS band-crossing priority). Returns empty
 *  fields when the band — or no band — defines none, so callers leave the
 *  current mode/step untouched. */
export function bandTuneDefaults(
  hz: number, region: number,
): { mode?: SDRMode; step?: number } {
  const order: Record<BandType, number> = { ham: 0, broadcast: 1, utility: 2 };
  const bands = getBandsAtRegion(hz, region)
    .sort((a, b) => (order[a.type] ?? 9) - (order[b.type] ?? 9));
  const primary = bands[0];
  if (!primary) return {};
  return { mode: primary.mode, step: primary.step };
}
