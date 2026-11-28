// test_air_sea_tune.ts — beta96 the AIR & SEA map's TUNE list (src/services/airSeaTune.ts) and the full-screen maps' SHARED
// TUNER line (src/services/dialChat.ts mapSharedLine). Per radio shapes, from what each radio's vibeserver.json says (the
// server's gates decide what is offered; the list only reads it): an RTL at 2.4 MS/s (split rows), an SDRplay RSP / HackRF at
// 8+ MS/s (the covered families collapse to one row), an Airspy HF+ (0.5 kHz–31 MHz + 60–260 MHz, ~768 kS/s: NDB, ACARS, VDL2,
// AIS, the VOR scan — never ADS-B or sondes, which have no capability field there).
//   npx tsx scripts/test_air_sea_tune.ts
import { airSeaTuneRows, airSeaLiveModes, airSeaTuneChip, avBandsShown, sondeBandsShown, mhz, tuneRowMode, TUNE_ICON_PATH, type AirSeaTuneSource } from '../src/services/airSeaTune';
import { mapSharedLine } from '../src/services/dialChat';
import { parseAvBands } from '../src/services/aviation';
import { readFileSync } from 'node:fs';

let pass = 0, fail = 0;
const ok = (c: unknown, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const Y = { ok: true, why: '' };

// ── the server's tables, as vibeserver.json sends them (vibeavi::bands, vibers41::bands) ──
const ACARS = { id: 'acars', name: 'ACARS', region: 'Europe and the worldwide 131.550', hz: 131.675e6, channels: 4, chLo: 131.525e6, chHi: 131.825e6 };
const VDL2 = { id: 'vdl2', name: 'VDL2', region: 'Worldwide 136.975', hz: 136.8625e6, channels: 12, chLo: 136.7e6, chHi: 136.975e6 };
const BOTH = { id: 'acars+vdl2', name: 'ACARS + VDL2', region: 'Both bands at once', hz: 134.25e6, channels: 16, chLo: 131.525e6, chHi: 136.975e6 };
const SLO = { id: 'lo', name: '400–402 MHz', hz: 401.005e6, lo: 400e6, hi: 402e6 };
const SMID = { id: 'mid', name: '402–404 MHz', hz: 403.005e6, lo: 402e6, hi: 404e6 };
const SHI = { id: 'hi', name: '404–406 MHz', hz: 405.005e6, lo: 404e6, hi: 406e6 };
const SALL = { id: 'all', name: '400–406 MHz', hz: 403.005e6, lo: 400e6, hi: 406e6 };

console.log('── an RTL at 2.4 MS/s: every family split ──');
const rtl: AirSeaTuneSource = { adsb: Y, avdata: Y, avBands: [ACARS, VDL2], vormap: Y, vormapLo: 108e6, vormapHi: 117.95e6, ndb: Y, ndbLo: 190e3, ndbHi: 535e3,
                                ais: Y, aisDsc: false, sondes: Y, sondeBands: [SLO, SMID, SHI] };
const r1 = airSeaTuneRows(rtl, { mode: 'avdata', avBand: 'vdl2' });
ok(r1.map((r) => r.id).join(' ') === 'adsb av:acars av:vdl2 vormap ndb ais sd:lo sd:mid sd:hi', `rows: ${r1.map((r) => r.id).join(' ')}`);
ok(r1.map((r) => `${r.name} · ${r.freq}`).join(' | ') === 'ADS-B · 1090 MHz | ACARS · 131.525–131.825 MHz | VDL2 · 136.7–136.975 MHz | VOR / ILS scan · 108–117.95 MHz | NDB · 190–535 kHz | AIS · 161.975 + 162.025 MHz | Sondes · 400–402 MHz | Sondes · 402–404 MHz | Sondes · 404–406 MHz',
   `names and frequencies from the tables: ${r1.map((r) => `${r.name} · ${r.freq}`).join(' | ')}`);
ok(r1.map((r) => r.group).join(',') === 'aviation,aviation,aviation,aviation,aviation,maritime,balloons,balloons,balloons', 'grouped AVIATION / MARITIME / WEATHER BALLOONS, in that order');
ok(r1.find((r) => r.id === 'adsb')!.icon === 'aircraft' && r1.find((r) => r.id === 'ndb')!.icon === 'beacon' && r1.find((r) => r.id === 'ais')!.icon === 'ship' && r1.find((r) => r.id === 'sd:lo')!.icon === 'balloon',
   'icons: aircraft, beacon (NDB), ship, balloon');
ok(r1.filter((r) => r.live).map((r) => r.id).join() === 'av:vdl2' && airSeaTuneChip(r1) === 'TUNE · VDL2 136.7–136.975 MHz', `the live row and the chip: "${airSeaTuneChip(r1)}"`);
ok(Object.values(TUNE_ICON_PATH).every((p) => /^M[\d.]/.test(p)), 'every icon is a path');

console.log('── an RSP / HackRF at 8 MS/s: the covered families are ONE row each ──');
const rsp: AirSeaTuneSource = { ...rtl, avBands: [ACARS, VDL2, BOTH], aisDsc: true, sondeBands: [SLO, SMID, SHI, SALL] };
const r2 = airSeaTuneRows(rsp, { mode: 'ais', aisDsc: true });
ok(r2.map((r) => r.id).join(' ') === 'adsb av:acars+vdl2 vormap ndb maritime sd:all', `rows: ${r2.map((r) => r.id).join(' ')}`);
ok(r2.find((r) => r.id === 'av:acars+vdl2')!.freq === 'both · 131.525–136.975 MHz' && r2.find((r) => r.id === 'sd:all')!.freq === '400–406 MHz', 'ACARS + VDL2 · both, Sondes · 400–406 MHz');
ok(airSeaTuneChip(r2) === 'TUNE · MARITIME AIS + DSC ch 70', `chip "${airSeaTuneChip(r2)}"`);
// a LIVE narrow mode on a wide radio keeps its row (the chip must be able to name it)
const r2b = airSeaTuneRows(rsp, { mode: 'ais', aisDsc: false });
ok(r2b.some((r) => r.id === 'ais' && r.live) && r2b.some((r) => r.id === 'maritime'), 'AIS alone live on a wide radio: its row stays, beside MARITIME');
const r2c = airSeaTuneRows(rsp, { mode: 'sondes', sdBand: 'mid' });
ok(r2c.filter((r) => r.group === 'balloons').map((r) => r.id).join() === 'sd:mid,sd:all', 'a live sub-band on a wide radio stays beside the covering band');
ok(avBandsShown([ACARS, VDL2, BOTH]).length === 1 && avBandsShown([ACARS, VDL2]).length === 2 && sondeBandsShown([SLO, SALL]).length === 1, 'the cover rule alone');

console.log('── an Airspy HF+ (no 1090 / 400 MHz reach: no caps for them) ──');
const hfp: AirSeaTuneSource = { avdata: Y, avBands: [ACARS, VDL2], vormap: Y, vormapLo: 108e6, vormapHi: 117.95e6, ndb: Y, ndbLo: 255e3, ndbHi: 526.5e3, ais: Y };
const r3 = airSeaTuneRows(hfp, { mode: '' });
ok(r3.map((r) => r.id).join(' ') === 'av:acars av:vdl2 vormap ndb ais', `rows: ${r3.map((r) => r.id).join(' ')} — never ADS-B or sondes`);
ok(r3.find((r) => r.id === 'ndb')!.freq === '255–527 kHz', `NDB's window from the server's plan (ndbLo/ndbHi): ${r3.find((r) => r.id === 'ndb')!.freq}`);
ok(airSeaTuneChip(r3) === 'TUNE', 'nothing live: the chip says TUNE');

console.log('── greyed with the server\'s reason; the only mode ──');
const r4 = airSeaTuneRows({ ...rtl, sondes: { ok: false, why: 'The owner switched radiosondes off' } }, { mode: 'adsb' });
ok(r4.filter((r) => r.group === 'balloons').every((r) => r.why === 'The owner switched radiosondes off'), 'a capability false: its rows greyed with the server\'s words');
const r5 = airSeaTuneRows({ ...rtl, only: 'adsb' }, { mode: 'adsb' });
ok(r5.map((r) => r.id).join() === 'adsb', 'an ADS-B-only server: one row (the map draws no chip for one row)');
ok(tuneRowMode('av:vdl2') === 'avdata' && tuneRowMode('maritime') === 'ais' && tuneRowMode('sd:') === 'sondes', 'row → mode');
ok(mhz(117.95e6) === '117.95' && mhz(400e6) === '400' && mhz(136.8625e6) === '136.8625', 'MHz formatting');
ok(parseAvBands([{ id: 'acars', name: 'ACARS', region: '', hz: 1, channels: 4, chLo: 131525000, chHi: 131825000 }])[0].chLo === 131525000
   && parseAvBands([{ id: 'acars', name: 'ACARS', region: '', hz: 1, channels: 4 }])[0].chLo === undefined, 'parseAvBands reads chLo / chHi (an older server: absent)');

console.log('── the SHARED TUNER line (mapSharedLine) ──');
const L = (o: Partial<Parameters<typeof mapSharedLine>[0]>) => mapSharedLine({ shared: true, listeners: 1, max: 10, exitMoves: true, ...o });
ok(L({}).text === 'SHARED TUNER · FREE TO EXIT' && L({}).show && !L({}).dialLocked, `alone: "${L({}).text}"`);
ok(L({ listeners: 2 }).text === 'SHARED TUNER · ASK BEFORE EXITING · 2/10 👤', `company: "${L({ listeners: 2 }).text}"`);
const lk = L({ listeners: 2, ask: { askFirst: true, tuneLocked: true, askState: '', askUntil: 0 } });
ok(lk.text === 'SHARED TUNER · LOCKED · ASK IN CHAT TO EXIT · 2/10 👤' && lk.dialLocked && /chat/i.test(lk.why), `locked: "${lk.text}" — why "${lk.why}"`);
const aw = L({ listeners: 2, ask: { askFirst: true, tuneLocked: true, askState: 'open', askUntil: 0 } });
ok(aw.text === 'SHARED TUNER · AWAITING A REPLY · 2/10 👤' && aw.dialLocked, `asked: "${aw.text}"`);
ok(L({ listeners: 2, ask: { askFirst: true, tuneLocked: false, askState: '', askUntil: 0 } }).text === 'SHARED TUNER · ASK BEFORE EXITING · 2/10 👤', 'the first listener (in force but free): ASK BEFORE EXITING, not locked');
ok(L({ listeners: 0 }).text === 'SHARED TUNER · ASK BEFORE EXITING', 'before the count: the cautious wording, never "free" on a guess');
ok(L({ exitMoves: false, listeners: 3 }).text === 'SHARED TUNER · ASK TO TUNE · 3/10 👤', 'a map opened from a key (CLOSE MAP leaves the radio alone): the bar\'s own words');
ok(!L({ shared: false }).show && !L({ standalone: true }).show && !L({ max: 1 }).show, 'not shown: an unshared dial, the standalone / viewer map, a one-listener server');
ok(L({ scanLocked: true, said: 'ADMIN TRIGGERED A SCAN' }).text === 'ADMIN TRIGGERED A SCAN' && L({ scanLocked: true, said: 'x' }).dialLocked, 'the owner\'s scan: its line, and locked');

console.log('── every language has the exit words ──');
for (const l of ['en', 'de', 'es', 'fr', 'it', 'nl', 'pl', 'pt-BR']) {
  const j = JSON.parse(readFileSync(new URL(`../i18n/chat/${l}.json`, import.meta.url), 'utf8'));
  ok(['bannerFreeExit', 'bannerAskExit', 'bannerLockedExit'].every((k) => typeof j.chrome?.[k] === 'string' && j.chrome[k].length > 2), `${l}: FREE TO EXIT / ASK BEFORE EXITING / LOCKED words`);
}

console.log('── ★★★ beta97 AIR & SEA LIVE in TUNES THE RADIO ITSELF: the app == the web for the same radio ──');
{
  // the web's rule, read from its source (main.ts airSeaModes / asAllowed): the same modes, the same order, the same gates
  const web = readFileSync(new URL('../web/client/src/main.ts', import.meta.url), 'utf8');
  const wm = /function airSeaModes\(\): string\[\] \{([^]*?)\n\}/.exec(web)?.[1] ?? '';
  const webOrder = [...wm.matchAll(/out\.push\('([a-z]+)'\)/g)].map((m) => m[1]);
  ok(webOrder.join() === 'adsb,av,vormap,ndb,ais,maritime,sd', `the web's order: ${webOrder.join(' · ')}`);
  ok(/return !!cap\?\.ok && \(!only \|\| only === m\);/.test(web) && /return airSeaModes\(\)\.length \|\| airSeaOn\(\) \? \{ ok: true, why: '' \} : null;/.test(web),
     'the web offers the key when any mode is ok under the only-mode rule (or a mode is on)');
  // the Lenovo's SDRplay RSP1A as its vibeserver.json says (8 MS/s: ADS-B, the datalink, the VOR scan, NDB, AIS + DSC, sondes)
  const rsp: AirSeaTuneSource = { adsb: { ok: true, why: '' }, avdata: { ok: true, why: '' }, vormap: { ok: true, why: '' }, ndb: { ok: true, why: '' },
                                  ais: { ok: true, why: '' }, aisDsc: true, sondes: { ok: true, why: '' }, only: '' };
  ok(airSeaLiveModes(rsp).join() === 'adsb,av,vormap,ndb,ais,maritime,sd', `RSP1A: every air / sea mode, in the web's order (${airSeaLiveModes(rsp).join(' · ')})`);
  const hf: AirSeaTuneSource = { avdata: { ok: true, why: '' }, vormap: { ok: true, why: '' }, ndb: { ok: true, why: '' }, ais: { ok: true, why: '' }, aisDsc: false, only: '' };
  ok(airSeaLiveModes(hf).join() === 'av,vormap,ndb,ais', 'Airspy HF+: no ADS-B, no sondes (no capability field there)');
  ok(airSeaLiveModes({ ...rsp, adsb: { ok: false, why: 'too slow' }, avdata: undefined, vormap: undefined, ndb: undefined, ais: undefined, sondes: undefined }).length === 0,
     'every mode refused: no key (a refused mode is not drawn unless on)');
  ok(airSeaLiveModes({ ...rsp, only: 'ais' }).join() === 'ais,maritime', 'an only-mode server: only its mode');
  // the app offers the key by this rule, never by a list computed while the client was missing (the build-586 fault)
  const sdr = readFileSync(new URL('../src/screens/SDRScreen.tsx', import.meta.url), 'utf8');
  ok(/airSeaAvail: asMenuOne && \(airSeaLiveModes\(/.test(sdr) && /const vibeHere = !IS_APPLE_TV && \(\(route\.params\.serverType \?\? 'ubersdr'\) === 'vibeserver' \|\| !!\(client\.current as any\)\?\.isVibe\);/.test(sdr)
     && /\[vibeHere, adsbCap, avCap, vmCap, ndbCap, aisCap, sdCap,/.test(sdr), 'the app: the key from the web\'s rule; the TUNE rows recomputed once it is known to be a VibeServer');
}

console.log(`test_air_sea_tune: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
