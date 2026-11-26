// test_admin_bg.ts — the admin page's background-decoding line (web/client/src/adminBg.ts, V12 beta 4).
import { bgLines, bgVfoLine, bgDuration, bgCpuText, satPassLines, bgFullCpuText, behindLines, keepRunLines } from '../web/client/src/adminBg.ts';
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
// ★ beta94 (the Lenovo, 11-25): the reason it is idle is said here — the health pill says nothing about it
ok(bgLines({ state: 'paused', why: 'a listener has the radio on a satellite pass', profiles: 1, profile: 1 })!.head
   === 'Background decoding \u00b7 profile 1 \u00b7 idle \u2014 a listener has the radio on a satellite pass', 'idle: a satellite pass, said why');
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

// ── ★★★ beta94 the unattended satellite line: which runs and why ──
{
  ok(satPassLines(null) === null && satPassLines({ state: 'off', birds: 0 }) === null, 'never set up: nothing');
  const now = 1795600000;
  const both = satPassLines({ state: 'running', now: 'METEOR-M2 4 LRPT pass + ORBCOMM monitoring (sky plot) together \u2014 one capture holds both',
    meteor: true, orbcomm: true, both: true, centre: 137992200, name: 'METEOR-M2 4', label: 'LRPT', decoder: 'lrpt', los: now + 400, maxEl: 61, pausedBg: true,
    orbSky: { since: now - 3600, dots: 412 } }, now)!;
  ok(both.head === 'Unattended satellites \u00b7 METEOR-M2 4 LRPT pass + ORBCOMM monitoring (sky plot) together \u2014 one capture holds both', `both: ${both.head}`);
  ok(both.rows[0] === 'METEOR-M2 4 LRPT · max 61° · lrpt · LOS in 6 min 40 s · background decoding paused', `the pass row: ${both.rows[0]}`);
  ok(/^ORBCOMM sky plot · 412 dots since \d\d:\d\d \(positions \+ SNR, no packets\)$/.test(both.rows[1]) && both.rows[2] === 'capture held at 137.9922 MHz', `the sky plot rows: ${both.rows.slice(1).join(' | ')}`);
  const orb = satPassLines({ state: 'running', now: 'ORBCOMM monitoring (sky plot) \u2014 no Meteor pass for 1 h 12 min', orbcomm: true, orbSky: { since: now - 60, dots: 3 } }, now)!;
  ok(orb.head.endsWith('ORBCOMM monitoring (sky plot) \u2014 no Meteor pass for 1 h 12 min') && !orb.warn, `ORBCOMM alone: ${orb.head}`);
  const pr = satPassLines({ state: 'running', now: 'METEOR-M2 3 LRPT pass \u2014 ORBCOMM paused (priority; this radio\'s capture is too narrow for both (768 kHz usable, 791 kHz needed))',
    meteor: true, orbcomm: true, name: 'METEOR-M2 3', label: 'LRPT', decoder: 'lrpt', los: now + 60, maxEl: 33, orbLast: 'ORBCOMM monitoring, 3 h 4 min \u2014 980 sky dots \u2014 stopped (paused for the METEOR-M2 3 LRPT pass)' }, now)!;
  ok(pr.head.includes('ORBCOMM paused (priority;') && pr.rows.some((r) => r.startsWith('last: ORBCOMM monitoring, 3 h 4 min')), `the priority on an HF+: ${pr.head}`);
  const bl = satPassLines({ state: 'blocked', why: 'ADS-B is kept running on this radio when nobody is connected', meteor: true }, now)!;
  ok(bl.warn && bl.head === 'Unattended satellites \u00b7 cannot run \u2014 ADS-B is kept running on this radio when nobody is connected', `blocked, warned: ${bl.head}`);
  // ★★ beta95 warn, don't block: a keep-running ADS-B radio still runs the passes, and says what that costs
  const kw = 'a satellite pass takes the radio: ADS-B pauses for the pass, about 10\u201315 min, and resumes after';
  const w = satPassLines({ state: 'waiting', now: 'waiting \u2014 next Meteor pass in 40 min', meteor: true, keepWarn: kw }, now)!;
  ok(w.warn && w.rows.includes(`Warning: ${kw}`) && !w.head.includes('cannot run'), `keep-running warning, not a block: ${w.rows.join(' | ')}`);
  const wp = satPassLines({ state: 'running', now: 'METEOR-M2 4 LRPT pass', meteor: true, name: 'METEOR-M2 4', label: 'LRPT', decoder: 'lrpt', los: now + 300, maxEl: 40, keepPaused: 'ADS-B', keepWarn: kw }, now)!;
  ok(wp.rows.includes('ADS-B paused for this pass \u2014 it resumes after LOS'), `ADS-B paused row: ${wp.rows.join(' | ')}`);
}
// ★★ beta95 the pill audit: decoders losing input are the owner's line
{
  ok(behindLines(undefined) === null && behindLines([]) === null, 'nothing behind: no line');
  const one = behindLines(['ADS-B'])!;
  ok(one.head === 'Decoders losing input \u00b7 ADS-B' && one.warn && one.rows[0].startsWith('it dropped'), `one: ${one.head} | ${one.rows[0]}`);
  const two = behindLines(['ADS-B', 'ORBCOMM (wide-IQ slice tap)'])!;
  ok(two.head.endsWith('ADS-B, ORBCOMM (wide-IQ slice tap)') && two.rows[0].includes('audio is not affected'), `two: ${two.head}`);
}
// ★★★ beta94 nobody listening — background may use the whole machine
{
  ok(bgFullCpuText(undefined) === '' && bgFullCpuText({ cores: 0 }) === '', 'today\'s budget: nothing said');
  ok(bgFullCpuText({ cores: 4, rttyWorkers: 4, extraSlots: 4 }) === 'nobody listening \u2014 using up to 4 cores (4 RTTY search workers, 4 extra decoder slots)', 'four cores');
  ok(bgFullCpuText({ cores: 1, rttyWorkers: 2, extraSlots: 1 }) === 'nobody listening \u2014 using up to 1 core (2 RTTY search workers, 1 extra decoder slot)', 'one core, singular');
  const f = bgLines({ ...run, runner: { ...run.runner, fullCpu: { cores: 2, rttyWorkers: 2, extraSlots: 2 } } })!;
  ok(f.head.endsWith(' · nobody listening \u2014 using up to 2 cores (2 RTTY search workers, 2 extra decoder slots)') && !f.warn, `full head: ${f.head}`);
  ok(!bgLines(run)!.head.includes('nobody listening'), 'absent on today\'s budget');
  const dd = bgLines({ ...run, runner: { ...run.runner, depth: 3, depthName: 'one pass a slot' } })!;
  ok(dd.head.endsWith(' · Band decode depth reduced (one pass a slot) — every band still decoding'), `depth: ${dd.head}`);
  ok(!bgLines({ ...run, runner: { ...run.runner, depth: 0, depthName: 'full' } })!.head.includes('depth'), 'full depth: nothing said');
}

// ★★★ beta96 the landing map / keep-running line, and the shared 137 MHz capture on the satellite line
ok(keepRunLines(null) === null && keepRunLines({ map: '' }) === null && keepRunLines({ bg: false, map: '', land: 'plain', landHz: 96.1e6 }) === null, 'keepRun: manual background → nothing (bgLines says that)');
{ // ★★★ b96-bgsplit the background line names where new listeners land
  const a = keepRunLines({ bg: true, map: 'avmar', mode: 'AIS', on: false, state: 'listeners', land: 'plain', landHz: 96_100_000, landDemod: 'wfm' })!;
  ok(/paused — listeners have the radio/.test(a.head) && a.rows.some((r) => r === 'new listeners land on 96.100 MHz WFM — AIS pauses while they are on'), 'bg AIS + FM landing: ' + a.head + ' / ' + a.rows[0]);
  const b = keepRunLines({ bg: true, map: 'avmar', mode: 'VDL2', on: true, state: 'running', land: 'map', landTitle: 'Aviation & Maritime', landMode: 'VDL2' })!;
  ok(b.rows[0] === 'new listeners land on the Aviation & Maritime map in VDL2 — they join it already populated', 'bg = landing: ' + b.rows[0]);
  const d = keepRunLines({ bg: true, map: 'avmar', mode: 'ADS-B', on: true, state: 'running', land: 'dab', landMode: 'DAB 12B' })!;
  ok(/new listeners land on DAB 12B/.test(d.rows[0]), 'bg ADS-B + DAB landing: ' + d.rows[0]);
}
{
  const k = keepRunLines({ map: 'avmar', title: 'Aviation & Maritime', mode: 'VDL2', keep: true, on: true, state: 'running',
                           cost: 'about 9 % of one core (setup benchmark)', orbRider: true, centre: 137179000 })!;
  ok(k.head === 'Background decoding \u00b7 VDL2 \u00b7 running (nobody needs to be connected)', 'keepRun head (b96-bgsplit: the background line): ' + k.head);
  ok(k.rows.some((r) => r.startsWith('cost: about 9 %')) && k.rows.some((r) => /137\.1790 MHz/.test(r)) && k.rows.some((r) => /ORBCOMM sky plot building/.test(r)) && !k.warn,
     'keepRun rows: the cost, the shared centre, the sky plot');
  const c = keepRunLines({ map: 'avmar', mode: 'ADS-B', keep: true, on: false, state: 'cannot', why: 'ADS-B needs a capture of 2.4 MS/s' })!;
  ok(c.warn && /cannot run now — ADS-B needs a capture of 2\.4 MS\/s/.test(c.head) && c.rows.some((r) => /idles as it would/.test(r)), 'keepRun cannot: why, warned, what happens instead');
  const p = keepRunLines({ map: 'avmar', mode: 'VDL2', keep: true, state: 'pass', shared: 'VDL2' })!;
  ok(/sharing the capture with a satellite pass/.test(p.head), 'keepRun during a shared pass');
  const h = keepRunLines({ map: 'avmar', mode: 'VOR / ILS map', keep: true, on: true, state: 'running', cost: 'about 72 % of one core (setup benchmark)', heavy: true })!;
  ok(h.warn && h.rows.some((r) => /never hides it/.test(r)), 'heavy: warned on the admin page, never hidden');
  const sp = satPassLines({ state: 'running', name: 'METEOR-M2 4', label: 'LRPT', keepShared: 'VDL2', keepPaused: 'VDL2', keepWarn: 'a satellite pass shares the 137 MHz capture with VDL2' })!;
  ok(sp.rows.some((r) => /VDL2 keeps decoding on this pass's capture/.test(r)) && !sp.rows.some((r) => /paused for this pass/.test(r)), 'satellites: shared, not paused');
  const sr = satPassLines({ state: 'waiting', now: 'VDL2 is kept running — waiting for a pass', orbRider: true, meteor: true })!;
  ok(sr.rows.some((r) => /riding the kept VDL2 capture/.test(r)), 'satellites: the sky plot rides kept VDL2');
}

console.log(`admin bg: ${pass} passed, ${fail} failed`);
if (fail) process.exit(1);
