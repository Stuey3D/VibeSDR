/**
 * ★★ WHERE EACH DECODER LIVES — the decoder panel's BAND TABS, its groups by USE, and which keys are DIMMED because the
 * dial is nowhere near where that decoder is normally used (Stuart, 2026-10-28: "dimming out the decoders that dont
 * apply will make it easier"). ONE pure module, shared by the app (ModeSelector) and the web client (web/client/src/
 * decoderBands.ts), tested by scripts/test_decoder_bands.ts.
 *
 *   TUNES THE RADIO ITSELF (above the tabs, never dimmed — WHOLE_RADIO_IDS):
 *                     ADS-B · AIS · DAB · ISM · NDB · DIGITAL SPOTS: BAND
 *   HF (incl. LF/MF)  ✈ Aviation  HFDL (a server map)
 *                     ⚓ Maritime  NAVTEX · DSC · SITOR-A · WEFAX
 *                     Amateur     DIGITAL SPOTS · CW · PSK · RTTY · JTTY · SSTV · APRS (300 Bd HF packet)
 *                     Utility     TIME
 *   VHF               ⚓ Maritime  DSC (channel 70)
 *                     Amateur     APRS · DIGITAL SPOTS (6 m / 2 m) · CW · SSTV
 *                     Broadcast   ADV RDS · SAME (EAS alert headers, NOAA Weather Radio)
 *                     Paging      PAGE (POCSAG — Android local hardware only: the phone's own radio)
 *   UHF               Paging      PAGE (the 450 MHz and 900 MHz paging bands)
 *                     (ADS-B and ISM tune the radio themselves — no tab is drawn for an empty band)
 *   Every band        SCANNER · GALLERY · TRANSCRIBE (tools, not decoders of one band)
 *
 * ★★★ TUNES THE RADIO ITSELF (Stuart, 2026-10-28: "any decoders that trigger their own view Digital Spots (Full Band) DAB
 *     ISM ADS-B etc need their own separate section that is always lit up as they tune the SDR themselves"). They take
 *     the radio to their own frequencies and open their own view, so where the dial is now says nothing about them:
 *     never dimmed, never inside a band tab.
 *
 * ★★★ DIMMED, NEVER DISABLED OR HIDDEN (Stuart: "someone who hasnt setup an upconverter correctly will be tuned to 130MHz
 *     or so for HF"). A dimmed key is a HINT and stays fully tappable; the band tab only picks which tab OPENS — the
 *     others are one tap away. Availability (what the radio / owner allows) is the callers' existing rules, untouched.
 * ★★ THE FREQUENCY IS THE ONE THE USER SEES — after any converter offset (the app's status.frequency is already the
 *    display frequency; the server reports its own converter's display frequency).
 * ★ A tab that is NOT the dial's band is dimmed whole (decoderDim): on 7.074 MHz the VHF tab's CW is the same decoder as
 *   HF's, but nothing on that tab applies where you are.
 * ★ The DIAL, not the whole view: a 2.4 MHz view around 7.074 MHz takes in the 6.6 MHz HFDL band, and Stuart's example
 *   is exactly that HFDL should be dim there.
 * ★ The knowledge is the code's own wherever it has it: the band plan's AMATEUR bands (constants/bandPlan), NAVTEX's
 *   bands (navtexStations.generated), DSC's channels (services/dsc). Only what the code had nowhere is tabled here:
 *   the HF aeronautical (R) bands HFDL uses, the major WEFAX stations, the time-signal carriers (DecoderClient's
 *   timeStationFor reads the same ones — the test checks they agree), and the FM broadcast range for ADV RDS.
 */
import { BAND_PLAN } from '../constants/bandPlan.ts';
import { NAVTEX_BANDS_KHZ } from './navtexStations.generated.ts';
import { DSC_ALL } from './dscChannels.ts';

export type RadioBand = 'hf' | 'vhf' | 'uhf';
export type DecoderGroup = 'aviation' | 'maritime' | 'amateur' | 'paging' | 'other';

export const RADIO_BANDS: ReadonlyArray<{ id: RadioBand; label: string }> = [
  { id: 'hf', label: 'HF' }, { id: 'vhf', label: 'VHF' }, { id: 'uhf', label: 'UHF' },
];
export const DECODER_GROUPS: readonly DecoderGroup[] = ['aviation', 'maritime', 'amateur', 'paging', 'other'];

/** The band a (displayed) frequency is in: HF below 30 MHz (LF and MF with it), VHF to 300 MHz, UHF above. */
export function radioBandOf(hz: number): RadioBand {
  if (!Number.isFinite(hz) || hz < 30e6) return 'hf';
  return hz < 300e6 ? 'vhf' : 'uhf';
}

/** A group's heading in a band — the fourth group is named for what it holds there. */
export function decoderGroupLabel(g: DecoderGroup, band: RadioBand): string {
  if (g === 'aviation') return 'AVIATION';
  if (g === 'maritime') return 'MARITIME';
  if (g === 'amateur') return 'AMATEUR';
  if (g === 'paging') return 'PAGING';
  return band === 'hf' ? 'UTILITY' : band === 'vhf' ? 'BROADCAST' : 'SENSORS';
}
/** The id whose ModeIcon a group's heading carries (utils/modeIcons modeIconFor; 'amateur' is the transceiver). */
export function decoderGroupIcon(g: DecoderGroup, band: RadioBand): string {
  if (g === 'aviation') return 'adsb';
  if (g === 'maritime') return 'ais';
  if (g === 'amateur') return 'amateur';
  if (g === 'paging') return 'paging';
  return band === 'hf' ? 'time' : band === 'vhf' ? 'rds' : 'ism';
}

/**
 * Where each key sits: band → group, in display order. A decoder that genuinely works in two bands is in both (the
 * same key, the same decoder). Ids are both clients' (the app's DecId / whole-radio keys, the web's data-dec).
 * 'spots' = DIGITAL SPOTS (FT8, FT4, WSPR… — the server's spotting); 'rds' = ADVANCED RDS; 'hfdl' = the HFDL server map.
 */
export const DECODER_PLACES: Readonly<Record<RadioBand, ReadonlyArray<{ group: DecoderGroup; ids: readonly string[] }>>> = {
  hf: [
    { group: 'aviation', ids: ['hfdl'] },
    { group: 'maritime', ids: ['navtex', 'dsc', 'sitora', 'wefax'] },
    { group: 'amateur',  ids: ['spots', 'cw', 'psk', 'rtty', 'jtty', 'sstv', 'aprs'] },
    { group: 'other',    ids: ['time'] },
  ],
  vhf: [
    { group: 'maritime', ids: ['dsc'] },
    { group: 'amateur',  ids: ['aprs', 'spots', 'cw', 'sstv'] },
    { group: 'paging',   ids: ['paging'] },   // ★ V12: only drawn on the phone's own radio (Android local hardware)
    { group: 'other',    ids: ['rds', 'same'] },   // ★ V12 SAME: NOAA Weather Radio, 162.400–162.550 MHz
  ],
  uhf: [
    { group: 'paging',   ids: ['paging'] },
  ],
};

/**
 * ★★★ THE MODES THAT TUNE THE RADIO THEMSELVES and open their own view — their own section ABOVE the band tabs, in this
 * order, never dimmed (decoderDim is false for them). 'spotsband' = DIGITAL SPOTS: BAND (a whole band's digital
 * frequencies; the tune keys then step bands). Their availability is the callers' existing rules (locked range,
 * blocked modes, what the radio reaches), untouched.
 */
export const WHOLE_RADIO_IDS: readonly string[] = ['adsb', 'ais', 'dab', 'ism', 'ndb', 'spotsband'];
export const WHOLE_RADIO_LABEL = 'TUNES THE RADIO ITSELF';
export const WHOLE_RADIO_HINT = 'Each moves the radio to its own frequencies and opens its own view.';

/** Every band an id is placed in. */
export function bandsOfDecoder(id: string): RadioBand[] {
  return RADIO_BANDS.map(b => b.id).filter(b => DECODER_PLACES[b].some(g => g.ids.includes(id)));
}

/**
 * The tab to OPEN: the dial's band, unless nothing is on it here (an HF-only server's panel opened at 145 MHz through an
 * unset upconverter) — then the first band that has something. `present(band)` says whether a band has any key drawn.
 */
export function openingBand(hz: number, present: (b: RadioBand) => boolean): RadioBand {
  const b = radioBandOf(hz);
  if (present(b)) return b;
  return RADIO_BANDS.map(x => x.id).find(present) ?? b;
}

// ── "near": the dial against each decoder's usual frequencies ─────────────────────────────────────────────────────────
type Range = readonly [number, number];
const kHz = (lo: number, hi = lo) => [lo * 1e3, hi * 1e3] as const;
const MHz = (lo: number, hi = lo) => [lo * 1e6, hi * 1e6] as const;

/** Amateur bands, from the band plan (every region — a hint, not a licence check). */
const HAM: readonly Range[] = BAND_PLAN.filter(b => b.type === 'ham').map(b => [b.lo, b.hi] as const);
/** HF aeronautical mobile (R) bands (ITU RR Appendix 27) — where HFDL ground stations transmit. */
const HFDL: readonly Range[] = [
  kHz(2850, 3155), kHz(3400, 3500), kHz(4650, 4750), kHz(5450, 5730), kHz(6525, 6765), kHz(8815, 9040),
  kHz(10005, 10100), kHz(11175, 11400), kHz(13200, 13360), kHz(15010, 15100), kHz(17900, 18030), kHz(21924, 22000),
];
/** Major HF WEFAX stations' carriers (kHz): Northwood, DWD Pinneberg, NOAA Boston / Pt Reyes / New Orleans / Kodiak /
 *  Honolulu, JMH Tokyo, Charleville, Wellington, Kyodo. ±4 kHz covers the USB dial 1.9 kHz below. */
const WEFAX_KHZ = [
  2618.5, 4610, 8040, 11086.5, 3855, 7880, 13882.5, 4235, 6340.5, 9110, 12750, 4346, 8682, 12786, 17151.2, 22527,
  4317.9, 8503.9, 12789.9, 17146.4, 2054, 4298, 8459, 12412.5, 9982.5, 11090, 16135, 3622.5, 7795, 13988.5,
  5100, 11030, 13920, 20469, 5807, 9459, 13550.5, 16340.1, 16971, 17069.6, 22542,
];
/** Standard time signals (Hz) — the stations DecoderClient's timeStationFor names (MSF/WWVB, DCF77, RWM, WWV). */
export const TIME_SIGNAL_HZ: readonly number[] = [
  60_000, 77_500, 66_666, 4_996_000, 9_996_000, 14_996_000,
  2_500_000, 5_000_000, 10_000_000, 15_000_000, 20_000_000, 25_000_000,
];

const pad = (hz: number, r: Range, tol: number) => hz >= r[0] - tol && hz <= r[1] + tol;
const anyNear = (hz: number, rs: readonly Range[], tol = 0) => rs.some(r => pad(hz, r, tol));
const pointsNear = (hz: number, pts: readonly number[], tol: number) => pts.some(p => Math.abs(hz - p) <= tol);

/** ★ The HF maritime mobile bands (ITU Radio Regulations Article 5) — where SITOR-A's ships and coast stations work. */
const HF_MARITIME: readonly Range[] = [
  [4063e3, 4438e3], [6200e3, 6525e3], [8195e3, 8815e3], [12230e3, 13200e3], [16360e3, 17410e3],
  [18780e3, 18900e3], [19680e3, 19800e3], [22000e3, 22855e3], [25070e3, 25210e3], [26100e3, 26175e3],
];
const NEAR: Record<string, (hz: number) => boolean> = {
  hfdl:   hz => anyNear(hz, HFDL, 5e3),
  navtex: hz => pointsNear(hz, NAVTEX_BANDS_KHZ.map(k => k * 1e3), 5e3),
  dsc:    hz => DSC_ALL.some(c => Math.abs(hz - c.hz) <= (c.vhf ? 50e3 : 5e3)),
  sitora: hz => anyNear(hz, HF_MARITIME, 2e3),
  wefax:  hz => pointsNear(hz, WEFAX_KHZ.map(k => k * 1e3), 4e3),
  spots:  hz => anyNear(hz, HAM),
  cw:     hz => anyNear(hz, HAM),
  psk:    hz => anyNear(hz, HAM),
  rtty:   hz => anyNear(hz, HAM),
  jtty:   hz => anyNear(hz, HAM),
  sstv:   hz => anyNear(hz, HAM),
  aprs:   hz => anyNear(hz, HAM),
  time:   hz => pointsNear(hz, TIME_SIGNAL_HZ, 5e3),
  rds:    hz => anyNear(hz, [MHz(87.5, 108)], 500e3),
  same:   hz => anyNear(hz, [MHz(162.4, 162.55)], 25e3),   // NOAA Weather Radio's seven channels   // Japan's 76–95 MHz FM is below; a hint only
};

/**
 * True when the dial (the DISPLAYED frequency) is near where `id` is normally used — the key is drawn bright; false =
 * drawn DIM (still tappable). An id with no rule (a tool: SCANNER, GALLERY…) is never dimmed, nor is an unknown dial.
 */
export function decoderNear(id: string, hz: number): boolean {
  const f = NEAR[id];
  if (!f || !Number.isFinite(hz) || hz <= 0) return true;
  return f(hz);
}

/**
 * DIMMED in tab `tab`: not near its usual frequencies — or the tab is not the dial's band (an HF dial looking at the VHF
 * tab: none of it applies where you are). A HINT: the caller keeps the key fully tappable. Never for an unknown dial.
 */
// ★★★ NOTHING IS DIMMED (Stuart, 2026-10-28, the same day it was built: "dont dim out the decoders as i was tuned to MW
//     and the navtex icon was greyed out even though the navtex band was in view"). The dial alone cannot say what the
//     user can see or reach; the band tab still opens on the dial's band. Kept as a function so both clients' call sites
//     stay one line; decoderNear stays for anyone who wants the hint back.
export function decoderDim(_id: string, _hz: number, _tab: RadioBand): boolean {
  return false;
}
