// test_admin_bg.ts — the admin page's background-decoding line (web/client/src/adminBg.ts, V12 beta 4).
import { bgLines, bgVfoLine, bgDuration, bgCpuText } from '../web/client/src/adminBg.ts';
let pass = 0, fail = 0;
const ok = (c: boolean, m: string) => { if (c) pass++; else { fail++; console.log('  FAIL ' + m); } };

ok(bgLines(null) === null && bgLines({ state: 'off', profiles: 0 }) === null, 'not set up → nothing shown');
ok(bgLines({ state: 'off', profiles: 1 })!.head === 'Background decoding \u00b7 off', 'switched off');
ok(bgLines({ state: 'idle', profiles: 1, profile: 0 })!.head.includes('no profile covers this time of day'), 'idle');
ok(bgLines({ state: 'waiting', profiles: 1, profile: 1, startsIn: 200 })!.head.includes('profile 1 starts in 3 min 20 s unless somebody arrives'), 'waiting countdown');
const b = bgLines({ state: 'busy', profiles: 1, profile: 1, retryIn: 45 })!;
ok(b.head.includes('another program has the radio') && b.head.includes('45 s') && b.warn, 'busy (shared by time), warned');
const run = {
  state: 'running' as const, profiles: 1, profile: 1,
  runner: { running: true, cpuPct: 3.14, channelsCpuPct: 0.8, vfos: [
    { hz: 490000, dec: 'navtex', label: 'NAVTEX', state: 'running' as const, lock: 'locked', last: 1792707180, cpuPct: 1.4 },
    { hz: 518000, dec: 'navtex', label: 'NAVTEX', state: 'paused' as const, why: 'a listener needed a decoder slot', cpuPct: 0 },
    { hz: 4610000, dec: 'rtty', label: 'RTTY', state: 'out' as const },
  ] } };
const l = bgLines(run)!;
ok(l.head === 'Background decoding \u00b7 profile 1 \u00b7 1 of 3 VFOs running \u00b7 3.1 % of one core', 'running head: ' + l.head);
ok(/^490\.0 NAVTEX · locked · last message \d\d:\d\d · 1\.4 %$/.test(l.rows[0]), 'running VFO: ' + l.rows[0]);
ok(l.rows[1] === '518.0 NAVTEX \u00b7 paused (a listener needed a decoder slot)', 'paused VFO: ' + l.rows[1]);
ok(l.rows[2] === "4610.0 RTTY \u00b7 out of range (outside the listener's window)", 'out of range VFO');
ok(l.rows[3] === 'channels (one FFT for every VFO) \u00b7 0.8 %', 'the shared FFT, said as such');
const lis = bgLines({ ...run, state: 'listener' })!;
ok(lis.head.includes('carrying on with a listener on \u00b7 1 of 3 VFOs running'), 'carrying on with a listener');
ok(bgLines({ state: 'paused', profiles: 1, profile: 1 })!.head.includes('paused \u2014 a listener is on'), 'paused for a listener');
ok(bgLines({ ...run, runner: { ...run.runner, cpuPct: null } })!.head.endsWith('measuring\u2026'), 'no CPU yet → measuring…');
ok(bgVfoLine({ hz: 490000, dec: 'navtex', label: 'NAVTEX', state: 'running', last: 0 }).includes('nothing yet'), 'nothing decoded yet');
ok(bgVfoLine({ hz: 1840000, dec: 'ft8', label: 'FT8', state: 'running', spots: 142, countries: 23, cpuPct: 0.9 })
   === '1840.0 FT8 \u00b7 running \u00b7 142 spots, 23 countries so far \u00b7 0.9 %', 'FT8 live count');
ok(bgVfoLine({ hz: 1840000, dec: 'ft8', label: 'FT8', state: 'running', spots: 1, countries: 1 }).includes('1 spot, 1 country so far'), 'FT8 singulars');
// ★ V12 CW: a skimmer's VFO carries spots too, and is named for what it is (it read "FT8")
ok(bgVfoLine({ hz: 7025000, dec: 'cwskim', label: 'CW skimmer, 48 kHz (summary)', state: 'running', spots: 37, countries: 12, cpuPct: 0.4 })
   === '7025.0 CW skimmer \u00b7 running \u00b7 37 spots, 12 countries so far \u00b7 0.4 %', 'CW skimmer live count');
// ★ V12 PSK: the same for a PSK skimmer
ok(bgVfoLine({ hz: 14071500, dec: 'pskskim', label: 'PSK skimmer, 12 kHz (summary)', state: 'running', spots: 9, countries: 5, cpuPct: 0.1 })
   === '14071.5 PSK skimmer \u00b7 running \u00b7 9 spots, 5 countries so far \u00b7 0.1 %', 'PSK skimmer live count');
// ★ V12 2026-10-27: a Band digital-spots VFO, named for its band
ok(bgVfoLine({ hz: 7074000, dec: 'spots-band', label: 'Band skimmer (summary)', band: '40 m', state: 'running', spots: 212, countries: 31, cpuPct: 2.1 })
   === '7074.0 Band skimmer 40 m \u00b7 running \u00b7 212 spots, 31 countries so far \u00b7 2.1 %', 'Band VFO live count');
ok(bgVfoLine({ hz: 7074000, dec: 'spots-band', label: 'Band skimmer (summary)', band: '40 m', state: 'paused', why: 'the server owner has switched the band skimmer off on this receiver' })
   .includes('paused (the server owner has switched the band skimmer off'), 'Band VFO switched off here: paused, with why');
// ★ V12 2026-10-27: an NDB skimmer VFO — no frequency of its own, the part of the band it covers and its beacons
ok(bgVfoLine({ hz: 0, dec: 'ndb-skim', label: 'NDB skimmer (summary)', ndbLo: 190000, ndbHi: 535000, ndb: '12 beacons identified of 41 carriers', state: 'running', cpuPct: 3.2 })
   === 'NDB skimmer 190–535 kHz · running · 12 beacons identified of 41 carriers · 3.2 %', 'NDB skimmer VFO line');
ok(bgVfoLine({ hz: 0, dec: 'ndb-skim', label: 'NDB skimmer (summary)', state: 'paused', why: 'a listener is running NDB mode' })
   === 'NDB skimmer · paused (a listener is running NDB mode)', 'NDB skimmer VFO paused, with why');
ok(bgDuration(45) === '45 s' && bgDuration(120) === '2 min', 'durations');
// ★ V12 2026-10-25: an ISM profile — no VFOs, the band and the day instead
const ism = bgLines({ state: 'running', profiles: 1, profile: 1, ism: { band: '433', name: '433.92 MHz ISM', devices: 6, today: '14 devices \u00b7 212 readings today' } })!;
ok(ism.head === 'Background decoding \u00b7 profile 1 \u00b7 ISM 433.92 MHz ISM \u00b7 6 devices listed \u00b7 14 devices \u00b7 212 readings today' && ism.rows.length === 0 && !ism.warn,
   `ISM running: ${ism.head}`);
ok(bgLines({ state: 'waiting', profiles: 1, profile: 1, startsIn: 60, ism: { band: '868', name: '868 MHz SRD' } })!.head.includes('profile 1 (ISM 868 MHz SRD) starts in 1 min unless somebody arrives'), 'ISM waiting');
const blk = bgLines({ state: 'blocked', profiles: 1, profile: 1, why: 'the server owner has switched ISM off on this receiver', retryIn: 50, ism: { band: '433', name: '433.92 MHz ISM' } })!;
ok(blk.warn && blk.head.includes('profile 1 (ISM 433.92 MHz ISM) cannot run \u2014 the server owner has switched ISM off on this receiver') && blk.head.includes('50 s'), `ISM blocked, warned: ${blk.head}`);

// ★ V12 2026-11-22: a WEFAX stations VFO — its frequencies and what each is doing, one slot
{
  const fx = bgVfoLine({ hz: 0, dec: 'wefax-stations', label: 'WEFAX stations (several in one slot)', state: 'running', last: 0, cpuPct: 0.4,
    fax: [{ hz: 4610000, label: 'Northwood GYA 4610.0 kHz', state: 'chart' }, { hz: 7880000, label: 'Pinneberg (DWD) 7880.0 kHz', state: 'running' },
          { hz: 8040000, label: 'Northwood GYA 8040.0 kHz', state: 'out' }] });
  ok(fx === 'WEFAX stations · running, one decoder slot · Northwood GYA 4610.0 kHz receiving a chart, Pinneberg (DWD) 7880.0 kHz standing by, Northwood GYA 8040.0 kHz out of range · no chart yet · 0.4 %', `WEFAX stations: ${fx}`);
  ok(bgVfoLine({ hz: 0, dec: 'wefax-stations', label: '', state: 'paused', why: 'a listener needed a decoder slot', fax: [] }) === 'WEFAX stations · paused (a listener needed a decoder slot)', 'WEFAX stations paused');
}

// ★★ V12 2026-11-24: the CPU figure says what it is — a sum over threads, spread over cores — and easing is said as such
{
  const t = bgCpuText({ cpuPct: 87, threads: 41, cores: 4, hottestPct: 33.2, hottestName: 'vibe-bgdsp' });
  ok(t === '87 % of one core in all — spread over 41 threads on 4 cores (22 % of this machine) · busiest thread 33 % (vibe-bgdsp)', `the spread: ${t}`);
  ok(bgCpuText({ cpuPct: 3.14 }) === '3.1 % of one core', 'an older server: the bare figure');
  ok(bgCpuText({ cpuPct: 12, cores: 4 }) === '12 % of one core in all — spread over its threads on 4 cores (3.0 % of this machine)', 'cores but no thread count (not Linux)');
  ok(bgCpuText({ cpuPct: null, threads: 3, cores: 4 }) === 'measuring…', 'measuring');
  const e = bgLines({ ...run, state: 'listener', runner: { ...run.runner, cpuPct: 87, threads: 41, cores: 4, hottestPct: 33, hottestName: 'vibe-bgdsp',
    ease: { paused: 2, chips: 1, excused: true, note: 'eased for listeners (2 of 9 paused, extra modes off on 1 band)' },
    vfos: [{ hz: 7074000, dec: 'spots-band', label: 'Band', band: '40 m', state: 'running' as const, chipsOff: true, spots: 10, countries: 3, cpuPct: 4 },
           { hz: 10100800, dec: 'rtty-weather', label: 'RTTY weather', state: 'paused' as const, why: 'eased for listeners', eased: true }] } })!;
  ok(e.head.endsWith('busiest thread 33 % (vibe-bgdsp) · eased for listeners (2 of 9 paused, extra modes off on 1 band)') && !e.warn, `eased head, not a warning: ${e.head}`);
  ok(e.rows[0].includes('Band skimmer 40 m · CW / RTTY / PSK eased off for listeners · running'), `chips eased: ${e.rows[0]}`);
  ok(e.rows[1] === '10100.8 RTTY weather · paused (eased for listeners)', `paused for listeners: ${e.rows[1]}`);
}

console.log(`admin bg: ${pass} passed, ${fail} failed`);
if (fail) process.exit(1);
