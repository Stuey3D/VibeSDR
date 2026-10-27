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
  /** FT8/FT4 only: the run's summary so far (rewritten in the gallery every 10 min). */
  spots?: number; countries?: number;
  /** ★ V12 2026-10-27: a Band digital-spots VFO — its band ("40 m") and what the governor runs now. */
  band?: string; modes?: string;
}
export interface BgStatus {
  state: 'off' | 'idle' | 'waiting' | 'running' | 'listener' | 'paused' | 'busy' | 'blocked';
  why?: string; profile?: number; profiles?: number; locked?: boolean;
  startsIn?: number; retryIn?: number;
  /** ★ V12 2026-10-25: an ISM profile — its band, and while it runs the devices listed now and the day so far. */
  ism?: { band: string; name: string; devices?: number; today?: string };
  runner?: { running: boolean; inRangeOnly?: boolean; listenerOn?: boolean; channelsCpuPct?: number | null;
             cpuPct?: number | null; vfos: BgVfo[] };
}

const pct = (v: number | null | undefined) =>
  typeof v === 'number' && Number.isFinite(v) ? `${v < 10 ? v.toFixed(1) : v.toFixed(0)} % of one core` : 'measuring…';
const pctShort = (v: number | null | undefined) =>
  typeof v === 'number' && Number.isFinite(v) ? `${v < 10 ? v.toFixed(1) : v.toFixed(0)} %` : '';
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
  const name = v.dec === 'spots-band' ? `Digital spots: Band${v.band ? ' ' + v.band : ''}` : v.dec === 'cwskim' ? 'CW skimmer' : v.dec === 'pskskim' ? 'PSK skimmer' : typeof v.spots === 'number' ? 'FT8' : v.label || v.dec.toUpperCase();
  const parts = [`${khz(v.hz)} ${name}`];
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
    case 'paused':   head = `${p} · paused — a listener is on`; break;
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
  if (r) head += ` · ${pct(r.cpuPct)}`;
  const rows = r ? vfos.map(bgVfoLine) : [];
  if (r && running && typeof r.channelsCpuPct === 'number') rows.push(`channels (one FFT for every VFO) · ${pctShort(r.channelsCpuPct)}`);
  return { head: `Background decoding · ${head}`, rows, warn };
}
