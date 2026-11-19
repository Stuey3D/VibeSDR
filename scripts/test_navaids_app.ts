// test_navaids_app.ts — V12 VOR / ILS and 406 MHz beacons, the client half (src/services/navaids.ts — shared by the app and the
// web client): the servers' statuses sanitised, the words, the radial's line and the FIX two radials make, the web's card, and
// the keys wired in both clients and both setup screens (only where the server offers "vor" / "sarsat").
// Run: npx tsx scripts/test_navaids_app.ts
import { readFileSync } from 'node:fs';
import { VOR_HINT, SARSAT_HINT, SARSAT_NOTE, vorSanitise, vorStateText, vorReadout, ilsWords, radialLine, radialFix, navCardSvg,
         sarsatSanitise, sarsatStateText } from '../src/services/navaids';
import { bandsOfDecoder, decoderNear } from '../src/utils/decoderBands';
import { modeIconFor } from '../src/utils/modeIcons';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { if (c) pass++; else { fail++; console.log(`FAIL ${what}`); } };
const near = (a: number, b: number, tol: number) => Math.abs(a - b) <= tol;

// ── the VOR status ──
const lon = { ident: 'LON', name: 'London', cc: 'GB', lat: 51.4872, lon: -0.46667, var: -3, dme: '083X', km10: 100 };
const v = vorSanitise({ chan: '113.60', expect: 'vor', kind: 'vor', rate: 37500, secs: 12, radial: 247.5, sigma: 0.2, trueBrg: 244.5,
                        ident: 'LON', reading: 'LON', wpm: 7.1, station: lon, listed: [lon, { lat: 'x' }], credit: 'OurAirports (public domain)' });
ok(!!v && v.kind === 'vor' && v.radial === 247.5 && v.trueBrg === 244.5 && v.station?.ident === 'LON' && v.station.variation === -3, 'vorSanitise keeps the fields');
ok(!!v && v.listed.length === 1, 'a broken listed station is dropped');
ok(vorSanitise({ kind: 'nonsense', radial: 999, ddm: 'x', station: { lat: 200, lon: 0 } })?.kind === 'none'
   && vorSanitise({ radial: 999 })?.radial === null && vorSanitise({ station: { lat: 200, lon: 0 } })?.station === null, 'out-of-range values become null');
ok(vorSanitise(null) === null && vorSanitise('x') === null, 'not an object: null');
ok(vorReadout(v) === '248° FROM LON', `readout "${vorReadout(v)}"`);
ok(/radial 247\.5° ±0\.2° · true 244\.5° · LON London/.test(vorStateText(1, v)), `status "${vorStateText(1, v)}"`);
ok(/searching/.test(vorStateText(0, vorSanitise({ chan: '109.50', expect: 'loc', kind: 'none', pair: '332.60' }))) && /glideslope 332\.60/.test(vorStateText(0, vorSanitise({ chan: '109.50', expect: 'loc', kind: 'none', pair: '332.60' }))),
   'a localiser channel, searching, names its glideslope');
ok(vorStateText(0, null) === 'listening for a VOR or ILS…', 'before the first status');

// ── ILS words: 90 Hz dominant on a localiser = left of the course = fly right; on a glideslope = above = fly down ──
ok(ilsWords('loc', 0.1).side === 'left of the course — fly right' && ilsWords('loc', -0.1).side === 'right of the course — fly left', 'localiser sides');
ok(ilsWords('gs', 0.1).side === 'above the glide path — fly down' && ilsWords('gs', -0.1).side === 'below the glide path — fly up', 'glideslope sides');
ok(ilsWords('loc', 0.155).pct === 100 && ilsWords('gs', 0.0875).pct === 50, 'per cent of full scale (0.155 localiser, 0.175 glideslope)');
ok(ilsWords('loc', 0.002).side === 'on the course line', 'on the line within 0.004 DDM');

// ── the radial's line, and the FIX ──
const line = radialLine(v, 300, 24);
ok(line.length === 25 && near(line[0][0], 51.4872, 1e-6) && near(line[0][1], -0.46667, 1e-6), 'the line starts at the station');
ok(line[24][1] < -4 && line[24][0] < 51.4872, `…and runs west-south-west (244.5° true): ends ${line[24][0].toFixed(2)}, ${line[24][1].toFixed(2)}`);
ok(radialLine(vorSanitise({ kind: 'vor', radial: 10 })).length === 0, 'no station or no true bearing: no line');
// a point P; two stations; the true bearing FROM each station TO P → the fix is P
const P = { lat: 52.20, lon: -1.10 };
const brgTo = (a: { lat: number; lon: number }, b: { lat: number; lon: number }) => {
  const r = Math.PI / 180, p1 = a.lat * r, p2 = b.lat * r, dl = (b.lon - a.lon) * r;
  return ((Math.atan2(Math.sin(dl) * Math.cos(p2), Math.cos(p1) * Math.sin(p2) - Math.sin(p1) * Math.cos(p2) * Math.cos(dl)) / r) + 360) % 360;
};
const DTY = { lat: 52.18, lon: -1.11 + 0.9 }, HON = { lat: 52.36, lon: -1.66 };
const fix = radialFix({ ...DTY, brg: brgTo(DTY, P) }, { ...HON, brg: brgTo(HON, P) });
ok(!!fix && near(fix[0], P.lat, 1e-6) && near(fix[1], P.lon, 1e-6), `two radials cross where they should (${fix?.map((x) => x.toFixed(4)).join(', ')})`);
ok(radialFix({ ...DTY, brg: (brgTo(DTY, P) + 180) % 360 }, { ...HON, brg: brgTo(HON, P) }) === null, 'a crossing BEHIND a station is no fix');
ok(radialFix({ lat: 51, lon: 0, brg: 90 }, { lat: 51, lon: 0, brg: 90 }) === null, 'the same line twice is no fix');

// ── the web's card ──
const svg = navCardSvg(v);
ok(svg.startsWith('<svg') && svg.includes('248° FROM LON') && svg.includes('true 244.5°') && !/NaN|undefined/.test(svg), 'the VOR card');
const loc = vorSanitise({ chan: '109.50', expect: 'loc', kind: 'loc', ddm: 0.031, sdm: 0.4, pair: '332.60', ident: 'ILL' });
const ls = navCardSvg(loc);
ok(ls.includes('LOCALISER +0.031') && ls.includes('left of the course — fly right') && ls.includes('paired glideslope 332.60') && !/NaN|undefined/.test(ls), 'the localiser card');
ok(navCardSvg(vorSanitise({ kind: 'none' })) === '' && navCardSvg(null) === '', 'nothing held: no card');
ok(navCardSvg(vorSanitise({ kind: 'vor', radial: 10, station: { ident: '<b>', name: '"x"&', cc: 'GB', lat: 50, lon: 0 } })).includes('&lt;b&gt;'), 'station text escaped');

// ── 406 MHz ──
const s = sarsatSanitise({ bursts: 3, tests: 1, rate: 48000, last: { rf: 406037000, country: 232, countryName: 'United Kingdom (232)', type: 'EPIRB', protocol: 'Standard location', test: false, selfTest: false, long: true } });
ok(!!s && s.bursts === 3 && s.last?.type === 'EPIRB', 'sarsatSanitise');
ok(sarsatStateText(0, s) === '3 bursts heard (1 test) · last: EPIRB · United Kingdom (232)', `status "${sarsatStateText(0, s)}"`);
ok(/listening for 406 MHz/.test(sarsatStateText(0, null)), 'before the first burst');
ok(/authorities/.test(SARSAT_NOTE) && SARSAT_HINT.includes(SARSAT_NOTE) && /owner only/.test(SARSAT_HINT), 'the note: authorities, not us; the owner-only rule said');
ok(/AM/.test(VOR_HINT) && /OurAirports/.test(VOR_HINT), 'the VOR hint says AM and the source');

// ── placement and glyphs ──
ok(bandsOfDecoder('vor').join() === 'vhf,uhf' && bandsOfDecoder('sarsat').join() === 'uhf', 'VOR / ILS VHF + UHF, 406 UHF');
ok(decoderNear('vor', 113.6e6) && decoderNear('sarsat', 406.04e6), 'near their frequencies');
ok(modeIconFor('vor') === 'beacon' && modeIconFor('sarsat') === 'ship', 'glyphs');

// ── both clients wired, both setup screens ──
const web = readFileSync('web/client/src/main.ts', 'utf8'), html = readFileSync('web/client/index.html', 'utf8'), dec = readFileSync('web/client/src/decoders.ts', 'utf8');
ok(/data-dec="vor" hidden/.test(html) && /data-dec="sarsat" hidden/.test(html) && /id="navCard"/.test(html), 'web: both keys (hidden until offered) and the card');
ok(/vorCap = j\.vor === true \|\| !!decWhy\.vor/.test(web) && /sarsatCap = j\.sarsat === true \|\| !!decWhy\.sarsat/.test(web), 'web: offered only where vibeserver.json says so (greyed with why)');
ok(/'vor', 'sarsat'\];/.test(web), 'web: both in the reach list');
ok(/vor: 'vor'/.test(dec) && /sarsat: 'sarsat'/.test(dec) && /msg\.type === 'vor'/.test(dec) && /msg\.type === 'sarsat'/.test(dec), 'web: attach + status JSON');
const app = readFileSync('src/screens/SDRScreen.tsx', 'utf8'), ms = readFileSync('src/components/ModeSelector.tsx', 'utf8'), dc = readFileSync('src/services/DecoderClient.ts', 'utf8');
ok(/setVorCapable\(o\?\.vor === true \|\| !!dw\.vor\)/.test(app) && /setSarsatCapable\(o\?\.sarsat === true \|\| !!dw\.sarsat\)/.test(app), 'app: the capability, greyed with why');
ok(/vorAvail: vorCapable/.test(app) && /sarsatAvail: sarsatCapable/.test(app), 'app: passed to the decoder keys');
ok(/case 'vor':   return vorShown \? srvKey\('vor', 'VOR \/ ILS'/.test(ms) && /case 'sarsat': return sarsatShown \? srvKey\('sarsat', '406 BEACONS'/.test(ms), 'app: both keys');
ok(/case 'vor':    return \{ extension_name: 'vor'/.test(dc) && /case 'sarsat': return \{ extension_name: 'sarsat'/.test(dc) && /m\.type === 'vor'/.test(dc), 'app: attach + status JSON');
const setup = readFileSync('android/app/src/main/cpp/vibe_setup_page.h', 'utf8'), sms = readFileSync('src/screens/ServerModeScreen.tsx', 'utf8');
ok(/id: "vor", label: "VOR bearing and ILS deviation"/.test(setup) && /id: 'vor', label: 'VOR bearing and ILS deviation'/.test(sms), 'the owner switch: setup page and app, the same words (VOR)');
ok(/id: "sarsat", label: "406 MHz distress beacons \(COSPAS-SARSAT\)"/.test(setup) && /id: 'sarsat', label: '406 MHz distress beacons \(COSPAS-SARSAT\)'/.test(sms), '…and 406');
// ── the copy that says where it is ──
const about = readFileSync('src/components/AboutOverlay.tsx', 'utf8'), tut = readFileSync('web/client/src/tutorial.ts', 'utf8');
ok(/VOR, ILS AND 406 MHz BEACONS/.test(about) && /under Aviation, on the VHF and UHF tabs/.test(about) && /under Maritime, on the UHF tab/.test(about), 'About: where both keys are');
ok(/VHF '\s*\+ 'and UHF tabs VOR \/ ILS/.test(tut) && /406 BEACONS/.test(tut), 'web tutorial: both');

console.log(`\n${fail ? '✗' : '✓'} test_navaids_app: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
