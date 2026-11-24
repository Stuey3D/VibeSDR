/* ★★★ WHAT THIS PHONE CAN RUN — the benchmark read for someone using the phone as their OWN radio (Local Hardware, Android).
 *
 *  Stuart, 2026-11-20: "we should offer the benchmark to the local hardware mode too so people on weaker devices know exactly
 *  what they are getting". It is the SAME benchmark as the server screen's (vibe_benchmark.h, one result per device in
 *  benchmark.json — a run from either screen shows in both); what differs is the READING. The server screen lists every row
 *  as "% of a core" for an owner deciding what to offer listeners; here each thing a person might do with their radio gets
 *  a plain verdict from the row's grade, with the figure as small print.
 *
 *  ★★ NO DEAD ROWS (AGENTS.md "a control that only works on one radio should not be there"): an activity is listed only
 *     where THIS radio, on this phone, can do it — the same per-radio answers the decoder keys are drawn from
 *     (/vibeserver.json from the phone's own engine: dab, ais, adsb, ism bands, freedv sub-modes, m17 …). An Airspy HF+
 *     is not told how DAB would run; an RTL with no HF is not told about HFDL.
 *  ★ No row, no line: an activity whose benchmark row is absent (an older engine, a recording that could not be fetched)
 *    is left out, never guessed. DRM has no benchmark row at all, so it is never listed here.
 *  ★ Pure — no React, no native — so scripts/test-local-bench.mjs holds it to fake results and fake radios. */

/** One row of the engine's result (vibe_benchmark.h toJson). `pct` is % of ONE core in real time; grade green < 70,
 *  amber 70–85, red > 85, none = not measured. */
export interface BenchRow { id: string; label?: string; pct: number; grade: string; rate?: number; thread?: string }
export interface BenchResult { v?: number; at?: number; benchSec?: number; rows?: BenchRow[] }

/** What THIS radio can do here — built by SDRScreen from the phone's own engine's per-radio answers. Every field is a
 *  plain fact; the mapping below never infers a capability the engine did not state. */
export interface LocalReach {
  /** The capture rate in use, Hz — picks the WFM row measured at the nearest rate. */
  sampleRate: number;
  /** vibeserver.json `tunable` — where this radio can be tuned, Hz. [] = not stated (then FM and HF are not judged). */
  tunable: Array<[number, number]>;
  dab: boolean;
  /** FreeDV sub-modes this engine runs (freedvOffered): 'RADE' | '700D' | '700E' | '1600'. */
  freedv: readonly string[];
  m17: boolean;
  adsb: boolean;
  ais: boolean;
  /** Centre frequencies (Hz) of the ISM bands this radio reaches (IsmCap.bands[].hz), [] = none. */
  ismHz: readonly number[];
  ndb: boolean;
  avdata: boolean;
  sondes: boolean;
  /** ★ V12 the VOR / ILS map: the radio tunes 108–118 MHz (vibeserver.json vormap). */
  vormap?: boolean;
  hfdl: boolean;
  /** The CW decoder is offered (and the radio is not an Icom — `skim`), and the RTTY skimmer. */
  cwSkim: boolean;
  rttySkim: boolean;
  spotsBand: boolean;
}

export type Verdict = 'good' | 'heavy' | 'too-heavy' | 'unmeasured';

export interface LocalBenchLine {
  key: string;
  /** What the person would be doing — "FM radio in stereo". */
  title: string;
  verdict: Verdict;
  /** The plain verdict — "Runs comfortably". */
  words: string;
  /** The figure as small print — "42% of one processor core", or for a group the heaviest of them named. */
  small: string;
}

/** ★ THE WORDS, ONE PLACE. From the grade alone — the engine's thresholds, never re-derived here. */
export function verdictFor(grade: string): Verdict {
  return grade === 'green' ? 'good' : grade === 'amber' ? 'heavy' : grade === 'red' ? 'too-heavy' : 'unmeasured';
}
export function verdictWords(v: Verdict): string {
  switch (v) {
    case 'good': return 'Runs comfortably';
    case 'heavy': return 'Runs, but uses a lot of this phone';
    case 'too-heavy': return 'Too heavy for this phone';
    default: return 'Not measured on this phone';
  }
}

const overlaps = (t: Array<[number, number]>, lo: number, hi: number) =>
  t.some(([a, b]) => Number(a) < hi && Number(b) > lo);
/** ★ Not stated = judged reachable: an engine that does not say where it tunes has not said it cannot. Only the FM and HF
 *  lines rest on this; everything else has its own explicit per-radio answer. */
const reaches = (r: LocalReach, lo: number, hi: number) => !r.tunable.length || overlaps(r.tunable, lo, hi);

interface Activity {
  key: string;
  title: string;
  /** Candidate rows; the line is judged on the HEAVIEST of those present (a group is only as light as its worst part). */
  rows: (r: LocalReach) => string[];
  /** Short names for a group's small print ("SSB 30% · AM 12% …" would be noise — the heaviest is named). */
  names?: Record<string, string>;
  /** ★ Alternatives, not parts: judged on the FIRST measured row (WFM at the radio's own rate; ADS-B at 2.4 MS/s, its
   *  ordinary rate — 3.2 is an owner's opt-in). */
  first?: boolean;
  can: (r: LocalReach) => boolean;
}

/** The WFM row at the rate nearest the radio's own (the engine measures 2.4, 2.048 and 1.024 MS/s). */
function wfmRows(r: LocalReach): string[] {
  const at: Array<[string, number]> = [['wfm_2400', 2_400_000], ['wfm_2048', 2_048_000], ['wfm_1024', 1_024_000]];
  const sr = r.sampleRate > 0 ? r.sampleRate : 2_400_000;
  return at.sort((a, b) => Math.abs(a[1] - sr) - Math.abs(b[1] - sr)).map(([id]) => id);
}

const ACTIVITIES: Activity[] = [
  { key: 'wfm', title: 'FM radio in stereo', rows: wfmRows, first: true, can: (r) => reaches(r, 87.5e6, 108e6) },
  { key: 'rdsx', title: 'FM stereo with Advanced RDS', rows: () => ['rdsx_2048'], can: (r) => reaches(r, 87.5e6, 108e6) },
  { key: 'dab', title: 'DAB and DAB+ radio', rows: () => ['dab'], can: (r) => r.dab },
  { key: 'narrow', title: 'SSB, AM and NFM', rows: () => ['ssb_2048', 'am_2048', 'nfm_2048'],
    names: { ssb_2048: 'SSB', am_2048: 'AM', nfm_2048: 'NFM' }, can: () => true },
  { key: 'text', title: 'Text decoders (RTTY, NAVTEX, CW, PSK …)', rows: () => ['dec_rtty', 'dec_navtex', 'dec_cw', 'dec_psk'],
    names: { dec_rtty: 'RTTY', dec_navtex: 'NAVTEX', dec_cw: 'CW', dec_psk: 'PSK31' },
    // ★ HF below 24 MHz: a plain RTL tunes from 24 MHz, which is not an HF radio (its 10 m sliver aside) — with direct
    //   sampling or a V4 its engine states the HF it reaches, and the line appears.
    can: (r) => reaches(r, 0.1e6, 24e6) },
  { key: 'skim', title: 'CW and RTTY skimmers (every signal at once)',
    rows: (r) => [...(r.cwSkim ? ['dec_cwskim'] : []), ...(r.rttySkim ? ['dec_rttyskim'] : [])],
    names: { dec_cwskim: 'CW skimmer', dec_rttyskim: 'RTTY skimmer' }, can: (r) => r.cwSkim || r.rttySkim },
  { key: 'spots', title: 'FT8 and FT4 spots across a whole band', rows: () => ['dec_ft8band'], can: (r) => r.spotsBand },
  { key: 'freedv', title: 'FreeDV digital voice (700D, 700E, 1600)', rows: () => ['dec_freedv'],
    can: (r) => r.freedv.some((s) => s !== 'RADE') },
  { key: 'rade', title: 'FreeDV RADE digital voice', rows: () => ['dec_rade'], can: (r) => r.freedv.includes('RADE') },
  { key: 'm17', title: 'M17 digital voice', rows: () => ['dec_m17'], can: (r) => r.m17 },
  { key: 'adsb', title: 'ADS-B aircraft', rows: () => ['adsb_2400', 'adsb_3200', 'adsb_3000'], first: true, can: (r) => r.adsb },
  { key: 'ais', title: 'AIS ships', rows: () => ['dec_ais'], can: (r) => r.ais },
  { key: 'ism433', title: 'ISM sensors (433 MHz)', rows: () => ['dec_ism'],
    can: (r) => r.ismHz.some((hz) => hz >= 430e6 && hz <= 440e6) },
  { key: 'ism868', title: 'ISM meters (868 MHz)', rows: () => ['dec_wmbus'],
    can: (r) => r.ismHz.some((hz) => hz >= 863e6 && hz <= 870e6) },
  { key: 'avdata', title: 'Aircraft datalink (VDL2, ACARS)', rows: () => ['dec_vdl2', 'dec_acars'],
    names: { dec_vdl2: 'VDL2', dec_acars: 'ACARS' }, can: (r) => r.avdata },
  { key: 'sondes', title: 'Radiosondes', rows: () => ['dec_rs41', 'dec_dfm', 'dec_m10'],
    names: { dec_rs41: 'RS41', dec_dfm: 'DFM', dec_m10: 'M10 / M20' }, can: (r) => r.sondes },
  { key: 'ndb', title: 'NDB beacons', rows: () => ['dec_ndb'], can: (r) => r.ndb },
  { key: 'vormap', title: 'VOR / ILS map (the whole navaid band)', rows: () => ['dec_vormap'], can: (r) => !!r.vormap },
  { key: 'hfdl', title: 'HFDL aircraft', rows: () => ['dec_hfdl'], can: (r) => r.hfdl },
];

const pctWords = (pct: number) => `${Math.round(pct)}% of one processor core`;

/** ★★ THE LINES — only what this radio can do here, and only where the benchmark has a row for it. For a group, the
 *  heaviest measured row decides (a line saying "runs comfortably" must be true of everything it names); an unmeasured
 *  row in a group is ignored while another was measured. */
export function localBenchLines(result: BenchResult | null | undefined, reach: LocalReach): LocalBenchLine[] {
  const rows = Array.isArray(result?.rows) ? result!.rows! : [];
  if (!rows.length) return [];
  const byId = new Map(rows.map((r) => [r.id, r]));
  const out: LocalBenchLine[] = [];
  for (const a of ACTIVITIES) {
    if (!a.can(reach)) continue;
    const ids = a.rows(reach);
    // ★ Alternatives (`first`): the first one measured; a group: every candidate present.
    const present = ids.map((id) => byId.get(id)).filter((r): r is BenchRow => !!r);
    if (!present.length) continue;
    const pick = a.first ? [present.find((r) => r.grade !== 'none') ?? present[0]] : present;
    const measured = pick.filter((r) => r.grade !== 'none' && Number.isFinite(r.pct) && r.pct >= 0);
    if (!measured.length) {
      out.push({ key: a.key, title: a.title, verdict: 'unmeasured', words: verdictWords('unmeasured'), small: '' });
      continue;
    }
    const worst = measured.reduce((m, r) => (r.pct > m.pct ? r : m));
    const v = verdictFor(worst.grade);
    const name = a.names && measured.length > 1 ? a.names[worst.id] : '';
    out.push({ key: a.key, title: a.title, verdict: v, words: verdictWords(v),
               small: name ? `${name}, the heaviest: ${pctWords(worst.pct)}` : pctWords(worst.pct) });
  }
  return out;
}

/** ★ WHEN IT WAS MEASURED, in words — "today at 14:05", "yesterday", "12 days ago". null when the result does not say
 *  (an engine older than `at`). `now` is passed in so the test is deterministic. */
export function measuredWhen(at: number | undefined, now: Date = new Date()): string | null {
  if (!at || !Number.isFinite(at) || at <= 0) return null;
  const then = new Date(at * 1000);
  const day = (d: Date) => new Date(d.getFullYear(), d.getMonth(), d.getDate()).getTime();
  const days = Math.round((day(now) - day(then)) / 86_400_000);
  const hhmm = `${String(then.getHours()).padStart(2, '0')}:${String(then.getMinutes()).padStart(2, '0')}`;
  if (days <= 0) return `Measured today at ${hhmm}`;
  if (days === 1) return `Measured yesterday at ${hhmm}`;
  return `Measured ${days} days ago`;
}

/** ★ "The radio pauses for about N minutes" — from how long this phone's last run took (benchSec), rounded UP to whole
 *  minutes; null when it has never run here (the caller then says "a few minutes", never a made-up figure). */
export function pauseMinutes(result: BenchResult | null | undefined): number | null {
  const s = Number(result?.benchSec);
  return s > 0 ? Math.max(1, Math.ceil(s / 60)) : null;
}
