// adminBg.ts — THE ADMIN PAGE'S BACKGROUND-DECODING LINE (V12 beta 4, 2026-10-22).
//
// ★★ What background decoding is doing on each radio, in words, from the radio's own status (adminStatusJson →
//    "bgDecode", built by Impl::bgStatusJson and BgRunner::statusJson). Admin only: the status call is behind admin
//    auth, and nothing here is in anything a listener receives.
// ★ Pure — strings in, strings out — so scripts/test_admin_bg.ts holds the wording without a browser.
// ★ CPU is MEASURED (each thread's own CPU clock around its own work), as % of ONE core over ~10 s. "measuring…" until
//   the server has two samples; the shared FFT is "channels", never shared out between VFOs by guesswork.

export interface BgVfo {
  hz: number; dec: string; label: string;
  state: 'running' | 'paused' | 'out';
  why?: string; lock?: string; last?: number; cpuPct?: number | null;
  /** ★ V12 2026-11-24: paused FOR LISTENERS (easing — vibe_bg_ease.h), and a Band VFO's extra modes off for them. */
  eased?: boolean; chipsOff?: boolean;
  /** FT8/FT4 only: the run's summary so far (rewritten in the gallery every 10 min). */
  spots?: number; countries?: number;
  /** ★ V12 2026-10-27: a Band digital-spots VFO — its band ("40 m") and what the governor runs now. */
  band?: string; modes?: string;
  /** ★ V12 2026-10-27: an NDB skimmer VFO — the part of the band it covers (no frequency of its own) and how it is doing
   *  ("12 beacons identified of 41 carriers"). */
  ndbLo?: number; ndbHi?: number; ndb?: string;
  /** ★ V12 2026-11-22: a WEFAX stations VFO — each ticked frequency, its station and state (running / chart: part-way
   *  through one / paused / out: outside the radio's window). */
  fax?: { hz: number; label: string; state: 'running' | 'chart' | 'paused' | 'out' }[];
}
export interface BgStatus {
  state: 'off' | 'idle' | 'waiting' | 'running' | 'listener' | 'paused' | 'busy' | 'blocked';
  why?: string; profile?: number; profiles?: number; locked?: boolean;
  startsIn?: number; retryIn?: number;
  /** ★ V12 2026-10-25: an ISM profile — its band, and while it runs the devices listed now and the day so far. */
  ism?: { band: string; name: string; devices?: number; today?: string };
  runner?: { running: boolean; inRangeOnly?: boolean; listenerOn?: boolean; channelsCpuPct?: number | null;
             cpuPct?: number | null; vfos: BgVfo[];
             /** ★ V12 2026-11-24 what the CPU figure is SPREAD over: background-rung threads, the machine's cores, the
              *  busiest single thread (Linux; absent elsewhere and on older servers). */
             threads?: number; cores?: number; hottestPct?: number; hottestName?: string;
             /** ★ V12 2026-11-24 easing for listeners: VFOs paused, bands with extra modes off, and the line. */
             ease?: { paused: number; chips: number; excused?: boolean; note?: string };
             /** ★★★ beta94 nobody listening: background may use more of the machine (absent = today's budget). */
             fullCpu?: { cores: number; rttyWorkers?: number; extraSlots?: number };
             /** ★★★ beta94 the load ladder: the Band VFOs' decode depth now (0 full … 5 the minimum) and its name. */
             depth?: number; depthName?: string };
}

const pct = (v: number | null | undefined) =>
  typeof v === 'number' && Number.isFinite(v) ? `${v < 10 ? v.toFixed(1) : v.toFixed(0)} % of one core` : 'measuring…';
const pctShort = (v: number | null | undefined) =>
  typeof v === 'number' && Number.isFinite(v) ? `${v < 10 ? v.toFixed(1) : v.toFixed(0)} %` : '';
/** ★★★ V12 2026-11-24 — THE FIGURE SAYS WHAT IT IS (Stuart, 24 Nov: "does background decoding get spread across cores as
 *  right now it says 87% of 1 core"). It is a SUM over every background thread, and read bare it looks like one core
 *  nearly full. "87 % of one core in all — spread over 41 threads on 4 cores (22 % of this machine) · busiest thread
 *  33 % (vibe-bgdsp)". A server that does not send the spread (older, or not Linux) keeps the bare figure. */
export function bgCpuText(r: { cpuPct?: number | null; threads?: number; cores?: number; hottestPct?: number; hottestName?: string }): string {
  const v = r.cpuPct;
  if (typeof v !== 'number' || !Number.isFinite(v)) return 'measuring…';
  const base = pct(v);
  const threads = typeof r.threads === 'number' && r.threads > 0 ? r.threads : 0;
  const cores = typeof r.cores === 'number' && r.cores > 0 ? r.cores : 0;
  if (!threads && !cores) return base;
  let t = `${base} in all — spread over `
    + (threads ? `${threads} thread${threads === 1 ? '' : 's'}` : 'its threads')
    + (cores ? ` on ${cores} core${cores === 1 ? '' : 's'} (${pctShort(v / cores)} of this machine)` : '');
  if (typeof r.hottestPct === 'number' && Number.isFinite(r.hottestPct) && r.hottestPct >= 0)
    t += ` · busiest thread ${pctShort(r.hottestPct)}${r.hottestName ? ` (${r.hottestName})` : ''}`;
  return t;
}
/** "3 min 20 s", "45 s". */
export function bgDuration(sec: number): string {
  const s = Math.max(0, Math.round(sec));
  if (s < 60) return `${s} s`;
  const m = Math.floor(s / 60), r = s % 60;
  return r ? `${m} min ${r} s` : `${m} min`;
}
const khz = (hz: number) => (hz / 1000).toFixed(1);
/** "22:53", in the reader's own clock (the page is read where the owner is). */
function hhmm(utcSec: number): string {
  const d = new Date(utcSec * 1000);
  return `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`;
}

/** One VFO's line: "490.0 NAVTEX · locked · last message 22:53 · 1.4 %". */
export function bgVfoLine(v: BgVfo): string {
  // ★ An FT8/FT4 summary VFO is "FT8" on this line (its menu label "FT8 / FT4 (summary)" is the setup page's); a CW
  //   skimmer sends spots too, and is "CW skimmer" (V12 2026-10-25) — it read "FT8" until then.
  //   And a PSK skimmer (2026-10-26) is "PSK skimmer".
  // ★ V12 2026-10-27: a Band digital-spots VFO is "Digital spots: Band 40 m" (every mode on the band, one summary).
  const name = v.dec === 'spots-band' ? `Band skimmer${v.band ? ' ' + v.band : ''}` : v.dec === 'cwskim' ? 'CW skimmer' : v.dec === 'pskskim' ? 'PSK skimmer' : typeof v.spots === 'number' ? 'FT8' : v.label || v.dec.toUpperCase();
  // ★ V12 2026-10-27: an NDB skimmer has no frequency — "NDB skimmer 190–535 kHz · running · 12 beacons identified of 41 carriers"
  if (v.dec === 'ndb-skim') {
    const span = (v.ndbHi ?? 0) > (v.ndbLo ?? 0) ? ` ${((v.ndbLo ?? 0) / 1000).toFixed(0)}–${((v.ndbHi ?? 0) / 1000).toFixed(0)} kHz` : '';
    const p = [`NDB skimmer${span}`];
    if (v.state === 'out') p.push('out of range (the window holds too little of the NDB band)');
    else if (v.state === 'paused') p.push(`paused (${v.why || 'a listener needed the decoder slot'})`);
    else {
      p.push('running');
      p.push(v.ndb || 'nothing yet');
      const c = pctShort(v.cpuPct);
      if (c) p.push(c);
    }
    return p.join(' · ');
  }
  // ★ V12 2026-11-22: "WEFAX stations · running · Northwood GYA 4610.0 kHz receiving a chart, DWD 7880.0 kHz standing by, 8040.0 out of range · 0.4 %"
  if (Array.isArray(v.fax)) {
    const p = ['WEFAX stations'];
    if (v.state === 'out') p.push('out of range (none of its frequencies is in the window)');
    else if (v.state === 'paused') p.push(`paused (${v.why || 'a listener needed the decoder slot'})`);
    else {
      p.push(`running, one decoder slot`);
      p.push(v.fax.map((f) => `${f.label} ${f.state === 'chart' ? 'receiving a chart' : f.state === 'running' ? 'standing by' : f.state === 'out' ? 'out of range' : 'paused'}`).join(', '));
      p.push(v.last ? `last chart ${hhmm(v.last)}` : 'no chart yet');
      const c = pctShort(v.cpuPct);
      if (c) p.push(c);
    }
    return p.join(' · ');
  }
  const parts = [`${khz(v.hz)} ${name}`];
  // ★ V12 2026-11-24: a Band skimmer with its extra modes eased off for listeners says so (FT8/FT4/WSPR carry on)
  if (v.chipsOff && v.state === 'running') parts.push('CW / RTTY / PSK eased off for listeners');
  if (v.state === 'out') parts.push("out of range (outside the listener's window)");
  else if (v.state === 'paused') parts.push(`paused (${v.why || 'a listener needed the decoder slot'})`);
  else if (typeof v.spots === 'number') {
    // ★ "1840.0 FT8 · running · 142 spots, 23 countries so far · 0.9 %" — a live count, so a summary card that is only
    //   rewritten every 10 min is never read as "no decodes".
    parts.push('running');
    parts.push(`${v.spots} spot${v.spots === 1 ? '' : 's'}, ${v.countries ?? 0} countr${v.countries === 1 ? 'y' : 'ies'} so far`);
    const c = pctShort(v.cpuPct);
    if (c) parts.push(c);
  } else {
    if (v.lock) parts.push(v.lock);
    parts.push(v.last ? `last message ${hhmm(v.last)}` : 'nothing yet');
    const c = pctShort(v.cpuPct);
    if (c) parts.push(c);
  }
  return parts.join(' · ');
}

/** The radio's line, and one line per VFO (none when nothing is running). null = nothing to show for this radio
 *  (background decoding not set up on it). */
/** ★★★ beta94 — NOBODY LISTENING: what background decoding may use now. "nobody listening — using up to 4 cores
 *  (4 RTTY search workers, 4 extra decoder slots)". Empty when the server is on today's budget (or does not say). */
export function bgFullCpuText(f: { cores: number; rttyWorkers?: number; extraSlots?: number } | null | undefined): string {
  if (!f || typeof f.cores !== 'number' || !(f.cores > 0)) return '';
  const extras: string[] = [];
  if (typeof f.rttyWorkers === 'number' && f.rttyWorkers > 0) extras.push(`${f.rttyWorkers} RTTY search worker${f.rttyWorkers === 1 ? '' : 's'}`);
  if (typeof f.extraSlots === 'number' && f.extraSlots > 0) extras.push(`${f.extraSlots} extra decoder slot${f.extraSlots === 1 ? '' : 's'}`);
  return `nobody listening — using up to ${f.cores} core${f.cores === 1 ? '' : 's'}` + (extras.length ? ` (${extras.join(', ')})` : '');
}

export function bgLines(bg: BgStatus | null | undefined): { head: string; rows: string[]; warn: boolean } | null {
  if (!bg || !bg.profiles) return null;
  const r = bg.runner;
  const vfos = r?.vfos ?? [];
  const running = vfos.filter((v) => v.state === 'running').length;
  const of = `${running} of ${vfos.length} VFO${vfos.length === 1 ? '' : 's'} running`;
  const p = `profile ${bg.profile ?? 1}`;
  let head: string;
  let warn = false;
  switch (bg.state) {
    case 'off':      head = bg.why ? `off — ${bg.why}` : 'off'; break;
    case 'idle':     head = 'no profile covers this time of day'; break;
    case 'busy':     head = `another program has the radio — trying again in ${bgDuration(bg.retryIn ?? 60)}`; warn = true; break;
    // ★ an ISM profile this radio cannot run now (ISM blocked, the rate, the band out of its allowed ranges …)
    case 'blocked':  head = `${p}${bg.ism ? ` (ISM ${bg.ism.name})` : ''} cannot run — ${bg.why || 'not on this radio now'}`
                          + (bg.retryIn ? ` · tried again in ${bgDuration(bg.retryIn)}` : ''); warn = true; break;
    // ★ beta94: the server says WHY (a satellite pass, tuned away from its bands) — here only, never on the health pill
    case 'paused':   head = bg.why ? `${p} · idle — ${bg.why}` : `${p} · paused — a listener is on`; break;
    case 'waiting':
      head = bg.locked ? `${p} · starting`
           : `${p} starts in ${bgDuration(bg.startsIn ?? 0)} unless somebody arrives`
             + (r ? ` · ${running} of ${vfos.length} decoding in range meanwhile` : '');
      break;
    case 'listener': head = `${p} · carrying on with a listener on · ${of}`; break;
    default:
      // ★ an ISM profile has no VFOs: "profile 1 · ISM 433.92 MHz ISM · 6 devices listed · 14 devices · 212 readings today"
      head = bg.ism ? `${p} · ISM ${bg.ism.name}${typeof bg.ism.devices === 'number' ? ` · ${bg.ism.devices} device${bg.ism.devices === 1 ? '' : 's'} listed` : ''}`
                      + (bg.ism.today ? ` · ${bg.ism.today}` : '')
                    : `${p} · ${of}${bg.locked ? ' · locked window' : ''}`;
  }
  if (bg.ism && bg.state === 'waiting' && !bg.locked) head = `${p} (ISM ${bg.ism.name}) starts in ${bgDuration(bg.startsIn ?? 0)} unless somebody arrives`;
  if (r) head += ` · ${bgCpuText(r)}`;
  // ★ V12 2026-11-24: background decoding stepping back for listeners — on purpose, said as such (not a warning)
  if (r?.ease?.note) head += ` · ${r.ease.note}`;
  // ★★★ beta94 (Stuart 11-25: "if nobody is using the pi then the full cpu can be allowed for background use")
  const fc = bgFullCpuText(r?.fullCpu);
  if (fc) head += ` · ${fc}`;
  // ★★★ beta94 (Stuart: "the depth drops before dropping entire bands"): said only while it is below full
  if (r && typeof r.depth === 'number' && r.depth > 0) head += ` · Band decode depth reduced (${r.depthName || `step ${r.depth} of 5`}) — every band still decoding`;
  const rows = r ? vfos.map(bgVfoLine) : [];
  if (r && running && typeof r.channelsCpuPct === 'number') rows.push(`channels (one FFT for every VFO) · ${pctShort(r.channelsCpuPct)}`);
  return { head: `Background decoding · ${head}`, rows, warn };
}

/* ── ★★★ beta94 THE UNATTENDED SATELLITE LINE (adminStatusJson → "satPass", Impl::satPassStatusJson) ────────────────────
 *  What runs and why, in the server's own words ("now"): "METEOR-M2 4 LRPT pass + ORBCOMM monitoring (sky plot) together —
 *  one capture holds both", "ORBCOMM monitoring (sky plot) — no Meteor pass for 1 h 12 min", "METEOR-M2 4 LRPT pass —
 *  ORBCOMM paused (priority; …)". Rows: the pass's LOS, the sky plot's dots this stretch, and the last of each. */
export interface SatPassStatus {
  state: 'off' | 'waiting' | 'blocked' | 'running';
  why?: string; now?: string; birds?: number; minEl?: number;
  meteor?: boolean; orbcomm?: boolean; first?: 'meteor' | 'orbcomm'; both?: boolean; centre?: number;
  name?: string; label?: string; decoder?: string; los?: number; maxEl?: number; pausedBg?: boolean;
  orbSky?: { since: number; dots: number };
  last?: string; orbLast?: string;
}
export function satPassLines(sp: SatPassStatus | null | undefined, nowSec = Date.now() / 1000): { head: string; rows: string[]; warn: boolean } | null {
  if (!sp || (sp.state === 'off' && !sp.meteor && !sp.orbcomm && !sp.birds)) return null;
  let head: string;
  if (sp.state === 'off') head = 'off';
  else if (sp.state === 'running' || sp.state === 'waiting') head = sp.now || (sp.state === 'running' ? 'running' : sp.why || 'waiting');
  else head = `cannot run — ${sp.why || sp.now || 'not on this radio now'}`;
  const rows: string[] = [];
  if (sp.state === 'running' && sp.name) {
    rows.push(`${sp.name} ${sp.label ?? ''} · max ${Math.round(sp.maxEl ?? 0)}° · ${sp.decoder || 'no decoder'}`
      + (typeof sp.los === 'number' && sp.los > nowSec ? ` · LOS in ${bgDuration(sp.los - nowSec)}` : '')
      + (sp.pausedBg ? ' · background decoding paused' : ''));
  }
  if (sp.orbSky) rows.push(`ORBCOMM sky plot · ${sp.orbSky.dots} dot${sp.orbSky.dots === 1 ? '' : 's'} since ${hhmm(sp.orbSky.since)} (positions + SNR, no packets)`);
  if (typeof sp.centre === 'number' && sp.centre > 0 && sp.state === 'running') rows.push(`capture held at ${(sp.centre / 1e6).toFixed(4)} MHz`);
  if (sp.last) rows.push(`last pass: ${sp.last}`);
  if (sp.orbLast) rows.push(`last: ${sp.orbLast}`);
  return { head: `Unattended satellites · ${head}`, rows, warn: sp.state === 'blocked' };
}
