// test_bg_decode_model.ts — the model behind the Android server screen's Background Decoding editor
// (src/services/bgDecode.ts): RadioConfig::bgDecode's JSON read and written exactly as the Setup page's bgLoad /
// bgCollect do, the engine's caps read, and the window / NDB / Band / DSC rules the editor's lines come from.
import { readFileSync } from 'node:fs';
import {
  parseBg, collectBg, parseBgCaps, bgNewProfile, bgHalfWindow, bgNdbSegment, bgNdbHint, bgBandReach, bgBandHint,
  bgDscWatch, bgLocal, bgTime, bgReach, bgDecoderSetsFreq, type BgSpotBand,
  bgSkimPoints, bgSkimPointAt, bgSkimPointLabel, bgSkimFits, bgSkimFirstFit, bgSkimHint, bgSkimDecoderChange, parseSkimBands, type BgVfo,
  bgFaxIn, bgFaxKhz, bgFaxDefault, bgFaxHint, BG_FAX_MAX, bgSatBothFits, bgUsbOnly, BG_HFDL_NOTE, type BgDecoderDef,
} from '../src/services/bgDecode.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
console.log('Background decoding model (Android server screen)');

// ── read / write ──
ok(collectBg(parseBg('')) === '', 'nothing stored + nothing set = nothing stored (the radio entry stays as before)');
ok(collectBg(parseBg('not json')) === '', 'an unreadable value reads as off with no profiles');
const nick = '{"on":true,"waitMin":5,"profiles":[{"centre":500000,"from":"00:00","to":"00:00","vfos":[{"hz":489500,"mode":"usb","dec":"navtex"},{"hz":517500,"mode":"usb","dec":"navtex"}]}]}';
ok(collectBg(parseBg(nick)) === nick, "NickB's NAVTEX profile round-trips byte for byte");
const m = parseBg(nick);
ok(m.keepInRange === true, 'keepInRange absent = on (as the engine reads it)');
m.keepInRange = false;
ok(JSON.parse(collectBg(m)).keepInRange === false, '…and off is written as false');
const ism = parseBg('{"on":true,"waitMin":10,"profiles":[{"ism":"433","centre":123,"vfos":[{"hz":1}],"from":"20:00","to":"06:00"}]}');
ok(ism.profiles[0].ism === '433' && ism.profiles[0].centre === 0 && ism.profiles[0].vfos.length === 0, 'an ISM profile drops its centre and VFOs');
ok(JSON.parse(collectBg(ism)).profiles[0].ism === '433', '…and keeps its band');
const ais = parseBg('{"on":true,"profiles":[{"ais":true}]}');
ok(ais.profiles[0].ais === true && ais.waitMin === 5, 'an AIS profile, and the 5-minute default wait');
const ndb = parseBg('{"on":true,"profiles":[{"ndb":true}]}');
ok(ndb.profiles[0].centre === 362750 && ndb.profiles[0].vfos[0].dec === 'ndb-skim', 'an older whole-radio NDB profile becomes the VFO profile it now is');
ok(parseBg('{"on":false,"profiles":[{"centre":0,"vfos":[{"hz":0,"mode":"usb","dec":"ndb-skim"}]}]}').profiles.length === 1
   && collectBg(parseBg('{"on":false,"profiles":[{"centre":0,"vfos":[{"hz":0,"mode":"usb","dec":"ndb-skim"}]}]}')) !== '',
   'off with an NDB skimmer row still saves (it is set up, just switched off)');
ok(parseBg('{"profiles":[{"vfos":[{"hz":1,"mode":"am","dec":"navtex"}]}]}').profiles[0].vfos[0].mode === 'usb', 'a demodulator no decoder works on reads as USB');
ok(bgNewProfile().from === '00:00' && bgNewProfile().to === '00:00', 'a new profile is all day');

// ── the engine's caps ──
ok(parseBgCaps('') === null && parseBgCaps('{"decoders":[]}') === null, 'no decoders from the engine = no editor (never drawn empty)');
const caps = parseBgCaps('{"decoders":[{"id":"navtex","label":"NAVTEX","modes":["usb","lsb"],"tone":500},{"id":"spots-band","label":"Band","modes":["usb"],"tone":0,"band":true}],"ismBands":["315","433"],"spotBands":[],"maxVfos":16,"maxProfiles":4,"decoderSlotsDefault":2}');
ok(!!caps && caps.decoders.length === 2 && caps.decoders[1].band === true && caps.decoderSlotsDefault === 2, "the engine's caps parse");

// ── the window rules (mirrors vibe::bg::halfWindowHz / ndbSegment) ──
ok(bgHalfWindow(2_048_000) === 1_024_000 - 50_000 - 3_000, 'the half window keeps clear of the capture edge');
const seg = bgNdbSegment(1_200_000, 3_000_000);
ok(!!seg && seg.lo === 190_000 && seg.hi === 535_000 && seg.ok, "Stuart's Lenovo RSP (1200 kHz, 3 MS/s) holds the whole NDB band");
ok(bgNdbHint(false, true, 0, 0, false).bad, 'NDB switched off says so');
ok(!bgNdbHint(true, false, 362_750, 2_048_000, false).bad, 'an NDB skimmer on the band centre is fine');
const b40: BgSpotBand = { id: '40m', name: '40 m', hz: 7_074_000, lo: 7_038_000, hi: 7_080_000, bandLo: 7_000_000, bandHi: 7_300_000, modes: ['FT8'] };
ok(bgBandReach(b40, 7_100_000, 2_048_000) && !bgBandReach(b40, 14_100_000, 2_048_000), 'a Band VFO reaches its band only from a centre that holds it');
ok(bgBandHint(true, [b40], 7_074_000, 14_100_000, 2_048_000).bad, '…and says why not');
ok(bgDecoderSetsFreq('ndb-skim') && bgDecoderSetsFreq('spots-band') && !bgDecoderSetsFreq('navtex'), 'the decoders that set the frequency');

// ── DSC watch ──
const p = bgNewProfile();
const msg = bgDscWatch(p, 2_400_000, false, 0, 16, 'rtl');
ok(p.centre > 0 && p.vfos.some((v) => v.dec === 'dsc') && /Added DSC/.test(msg), 'DSC watch picks a centre and adds what the window hears');
ok(p.vfos.every((v) => v.dec !== 'dsc' || v.mode === 'usb'), '…on USB, 1.7 kHz below');

// ── times ──
ok(bgLocal('23:30', 60) === '00:30' && bgLocal('00:15', -30) === '23:45', 'the server time beside UTC wraps the day');
ok(bgTime('2000') === '20:00' && bgTime('7:05') === '07:05' && bgTime('25:00') === null, 'typed times are read as HH:MM');

// ── reach by driver (vibeserver/main.cpp's save rules) ──
ok(!bgReach('airspyhf', false, 0).ismOk && bgReach('airspyhf', false, 0).ndbOk, 'an HF+: no ISM/AIS, NDB yes');
ok(!bgReach('rtl', false, 0).ndbOk && bgReach('rtl', true, 0).ndbOk && bgReach('rtl', false, 0).aisOk, 'an RTL: NDB only with direct sampling');
ok(!bgReach('icom', false, 0).ismOk && !bgReach('icom', false, 0).aisOk, 'an Icom: no whole-radio profiles');

// ── ★ V12 beta90 unattended satellite passes (the Setup page's satPasses, its own switch) ──
{
  const m = parseBg('{"on":false,"waitMin":5,"profiles":[],"satPasses":{"on":true,"minEl":30,"birds":[{"norad":57166,"label":"LRPT"},{"norad":-1,"label":"x"}]}}');
  // ★★★ beta94: the Meteor tick is the Meteor group now (stored as "meteor":true)
  ok(m.sat.on && m.sat.minEl === 30 && m.sat.birds.length === 0 && m.sat.meteor, 'satPasses read; a bad entry dropped; the Meteor tick = the group');
  ok(collectBg(m) === '{"on":false,"waitMin":5,"profiles":[],"satPasses":{"on":true,"minEl":30,"birds":[],"meteor":true}}', `stored with background off: ${collectBg(m)}`);
  const e = parseBg('');
  ok(!e.sat.on && e.sat.birds.length === 0 && collectBg(e) === '', 'untouched: nothing stored');
  // ★★★ beta94 the groups: an old Meteor tick becomes the Meteor group; ORBCOMM + the priority round-trip; absent = unchanged
  const g = parseBg('{"on":false,"waitMin":5,"profiles":[],"satPasses":{"on":true,"minEl":20,"birds":[{"norad":59051,"label":"LRPT"},{"norad":39444,"label":"Telemetry"}]}}');
  ok(g.sat.meteor && !g.sat.orbcomm && g.sat.first === 'meteor' && g.sat.birds.length === 1 && g.sat.birds[0].norad === 39444, 'an old Meteor tick = the Meteor group');
  ok(collectBg(g) === '{"on":false,"waitMin":5,"profiles":[],"satPasses":{"on":true,"minEl":20,"birds":[{"norad":39444,"label":"Telemetry"}],"meteor":true}}', `saved as the group: ${collectBg(g)}`);
  g.sat.orbcomm = true; g.sat.first = 'orbcomm';
  const g2 = parseBg(collectBg(g));
  ok(g2.sat.orbcomm && g2.sat.first === 'orbcomm' && collectBg(g2) === collectBg(g), `ORBCOMM + ORBCOMM first round-trip: ${collectBg(g2)}`);
  const capsB = parseBgCaps('{"decoders":[{"id":"navtex","label":"NAVTEX","modes":["usb"],"tone":500}],"satBoth":{"needHz":791000,"minRate":915000,"narrow":["airspyhf"]}}')!;
  ok(!!capsB.satBoth && bgSatBothFits(capsB.satBoth, 'rtlsdr', 2400000) === true && bgSatBothFits(capsB.satBoth, 'sdrplay', 2000000) === true
     && bgSatBothFits(capsB.satBoth, 'airspyhf', 912000) === false && bgSatBothFits(capsB.satBoth, 'rtlsdr', 900000) === false, 'satBoth: both on an RTL / RSP, the priority on an HF+ and a slow RTL');
  ok(bgSatBothFits(null, 'rtlsdr', 2400000) === null && parseBgCaps('{"decoders":[{"id":"navtex","label":"NAVTEX","modes":["usb"],"tone":500}]}')!.satBoth === null, 'an older engine: not known');
  const c2 = parseBgCaps('{"decoders":[{"id":"navtex","label":"NAVTEX","modes":["usb"],"tone":500}],"satPasses":[{"norad":57166,"label":"LRPT","name":"Meteor-M2-3","what":"weather pictures (LRPT)"}]}');
  ok(!!c2 && c2.satPasses.length === 1 && c2.satPasses[0].name === 'Meteor-M2-3', 'the engine\'s keepable downlinks reach the editor');
  ok(parseBgCaps('{"decoders":[{"id":"navtex","label":"NAVTEX","modes":["usb"],"tone":500}]}')!.satPasses.length === 0, 'an older engine: none (the section is not drawn)');
}

// ── ★★ V12 beta90 THE SKIMMERS BY BAND, SEVERAL WINDOWS EACH (the engine's skimBands points) ──
{
  const P = (...hz: (number | [number, string])[]) => hz.map((h) => (Array.isArray(h) ? { hz: h[0], note: h[1] } : { hz: h }));
  const caps = parseBgCaps(JSON.stringify({
    decoders: [{ id: 'cw-skim-24', label: 'CW skimmer, 24 kHz (summary)', modes: ['usb'], tone: 0, width: 24000, skim: 'cw' },
               { id: 'cw-skim-48', label: 'CW skimmer, 48 kHz (summary)', modes: ['usb'], tone: 0, width: 48000, skim: 'cw' },
               { id: 'cw-skim-96', label: 'CW skimmer, 96 kHz (summary)', modes: ['usb'], tone: 0, width: 96000, skim: 'cw' },
               { id: 'rtty-skim-24', label: 'RTTY skimmer, 24 kHz (summary)', modes: ['usb'], tone: 0, width: 24000, skim: 'rtty' },
               { id: 'navtex', label: 'NAVTEX', modes: ['usb'], tone: 500 }],
    skimBands: { region: 2,
      cw: [{ id: '40m', name: '40 m', lo: 7000000, hi: 7040000, points: { 24000: P(7012000, 7036000), 48000: P(7024000), 96000: P(7048000) } },
           { id: '20m', name: '20 m', lo: 14000000, hi: 14070000, points: { 24000: P(14012000, 14036000, 14060000), 48000: P(14024000, 14072000), 96000: P(14048000) } }],
      rtty: [{ id: '40m', name: '40 m', lo: 7040000, hi: 7050000, points: { 24000: P(7052000, [7092000, 'US RTTY'], [7116000, 'US RTTY']) } },
             { id: '60m', name: '60 m', lo: 5351500, hi: 5366500, points: { 24000: P([5363500, '']) } }], psk: [] },
  }))!;
  const sb = caps.skimBands!;
  ok(!!sb && sb.cw.length === 2 && caps.decoders[1].skim === 'cw' && caps.decoders[1].width === 48000 && !!sb.cw[0].points, 'the engine\'s skimBands (with each band\'s windows) and each skimmer\'s kind + width reach the editor');
  ok(parseSkimBands(undefined) === null && parseBgCaps('{"decoders":[{"id":"navtex","label":"N","modes":["usb"],"tone":0}]}')!.skimBands === null, 'an older engine: no skimBands (the typed box stays)');
  ok(bgSkimPoints(sb.cw, 24000).map((q) => q.hz / 1000).join() === '7012,7036,14012,14036,14060', '24 kHz: 40 m\'s two windows, 20 m\'s three, band by band');
  ok(bgSkimPoints(sb.cw, 48000).map((q) => q.hz / 1000).join() === '7024,14024,14072' && bgSkimPoints(sb.cw, 96000)[0].hz === 7048000, '48 kHz 7024, 96 kHz 7048 (Stuart\'s figures)');
  ok(bgSkimPointLabel(bgSkimPoints(sb.rtty, 24000)[1]) === '7080–7104 kHz · US RTTY', 'a practice window reads "7080–7104 kHz · US RTTY"');
  ok(bgSkimPointAt(sb.cw, 24000, 7036000)?.band.id === '40m' && bgSkimPointAt(sb.cw, 24000, 7020000) === null, 'a stored 7036 is 40 m\'s second window; an owner\'s 7020 is their own centre');
  ok(bgSkimPoints(parseSkimBands({ region: 1, cw: [{ id: '40m', name: '40 m', lo: 7000000, hi: 7040000 }] })!.cw, 48000)[0].hz === 7024000, 'an older engine (no points): one window per band, lo + width/2');
  ok(bgSkimFits(7024000, 48000, 7000000, 2400000) && !bgSkimFits(7048000, 96000, 6900000, 250000), 'capture fit: 7024/48 inside 2.4 MS/s at 7.0; 7048/96 outside 250 kS/s at 6.9');
  ok(bgSkimFirstFit(sb.cw, 48000, 14100000, 2400000)?.hz === 14024000, 'a new row at 14.1 MHz lands on 20 m\'s first window (the first that fits)');
  const d24 = caps.decoders[0], d48 = caps.decoders[1], d96 = caps.decoders[2], dr = caps.decoders[3], nav = caps.decoders[4];
  const h = bgSkimHint(d24, true, false, sb.cw, 7036000, false, 7000000, 2400000);
  ok(!h.bad && h.text.startsWith('40 m CW 7024–7048 kHz — every CW signal in it') && h.text.includes('part of the band’s CW segment'), 'its line — the Setup page\'s words: ' + h.text);
  ok(bgSkimHint(dr, true, false, sb.rtty, 7092000, false, 7000000, 2400000).text.includes('US RTTY, outside the band plan’s RTTY segment'), 'a practice window says so');
  ok(!/licen[cs]e/i.test(bgSkimHint(dr, true, false, sb.rtty, 5363500, false, 6000000, 2400000).text), '60 m: no licence note — we only receive (Stuart 11-21)');
  const ho = bgSkimHint(d48, true, false, sb.cw, 7031500, true, 7000000, 2400000);
  ok(!ho.bad && ho.text.startsWith('Your own centre: 7007.5–7055.5 kHz'), 'an own centre says what it covers: ' + ho.text);
  ok(bgSkimHint(d48, true, false, sb.cw, 7024000, false, 14100000, 2400000).bad, 'outside the capture: said in red');
  ok(bgSkimHint(dr, false, true, sb.rtty, 7052000, false, 7000000, 2400000).text.includes('an Icom'), 'cannot run (an Icom): says why');
  const v: BgVfo = { hz: 7036000, mode: 'usb', dec: 'cw-skim-24' };
  bgSkimDecoderChange(v, d24, d96, caps.skimBands, 7000000, 2400000);
  ok(v.hz === 7048000, 'CW 24 → 96 kHz keeps the band: 7036 → 40 m\'s one 96 kHz window, 7048');
  const v2: BgVfo = { hz: 14060000, mode: 'usb', dec: 'cw-skim-24' };
  bgSkimDecoderChange(v2, d24, d48, caps.skimBands, 14000000, 2400000);
  ok(v2.hz === 14072000, '20 m 24 kHz 14060 → 48 kHz: the nearest of 20 m\'s windows, 14072');
  const o: BgVfo = { hz: 7031500, mode: 'usb', dec: 'cw-skim-48', own: true };
  bgSkimDecoderChange(o, d48, d96, caps.skimBands, 7000000, 2400000);
  ok(o.hz === 7031500 && o.own === true, '…an owner\'s own centre stays theirs');
  const n: BgVfo = { hz: 517500, mode: 'usb', dec: 'navtex' };
  bgSkimDecoderChange(n, nav, dr, caps.skimBands, 7000000, 2400000);
  ok(n.hz === 7052000, 'NAVTEX → RTTY skimmer 24 kHz lands on 40 m, 7052 (the first window that fits)');
  ok(JSON.parse(collectBg({ ...parseBg(''), on: true, profiles: [{ centre: 7000000, from: '00:00', to: '00:00', vfos: [o] }] })).profiles[0].vfos[0].own === undefined,
     '"own" is the editor\'s, never stored');
}

// ── ★★ V12 2026-11-22 WEFAX stations (the Setup page's rules and words, test-setup-bg-wefax.mjs) ──
{
  const fx = '{"on":true,"waitMin":5,"profiles":[{"centre":6000000,"from":"00:00","to":"00:00","vfos":[{"hz":0,"mode":"usb","dec":"wefax-stations","fax":[4610000,7880000]},{"hz":4608100,"mode":"usb","dec":"wefax"}]}]}';
  ok(collectBg(parseBg(fx)) === fx, '★ a WEFAX stations row round-trips byte for byte ("fax" on that row only)');
  ok(collectBg(parseBg('{"on":false,"profiles":[{"centre":0,"vfos":[{"hz":0,"mode":"usb","dec":"wefax-stations","fax":[4610000]}]}]}')) !== '', 'a stations row alone counts as something set up (its hz is 0)');
  ok(bgDecoderSetsFreq('wefax-stations'), 'its row has no frequency of its own');
  const caps = parseBgCaps('{"decoders":[{"id":"wefax","label":"WEFAX (120 LPM)","modes":["usb"],"tone":1900},{"id":"wefax-stations","label":"WEFAX stations (several in one slot)","modes":["usb"],"tone":1900,"fax":true}],'
    + '"wefaxStations":[{"id":"gya","call":"GYA","name":"Northwood","country":"United Kingdom","freqs":[2618500,4610000,8040000,11086500]},{"id":"dwd","call":"DDH3 / DDK3 / DDK6","name":"Pinneberg (DWD)","country":"Germany","freqs":[3855000,7880000,13882500]}]}')!;
  ok(caps.decoders[1].fax === true && caps.wefaxStations.length === 2 && caps.wefaxStations[0].freqs[1] === 4610000, 'the engine\'s caps carry the decoder (fax) and the stations');
  ok(parseBgCaps('{"decoders":[{"id":"navtex","modes":["usb"]}]}')!.wefaxStations.length === 0, 'an older engine: no stations (the decoder is not offered)');
  ok(bgFaxIn(4610000, 4400000, 2000000) && !bgFaxIn(8040000, 4400000, 2000000), '4610 is inside a 2 MS/s window at 4.4 MHz, 8040 is not');
  ok(bgFaxDefault(caps.wefaxStations, 4400000, 2000000).join() === '4610000,3855000', 'a row becoming WEFAX stations ticks what the window holds: 4610, 3855');
  ok(bgFaxKhz(4610000) === '4610.0' && bgFaxKhz(12629250) === '12629.25', 'kHz as the schedule writes it');
  ok(bgFaxHint(caps.wefaxStations, [4610000, 8040000], 4400000, 2000000).text.startsWith('Decodes 1 of the 2 ticked in one decoder slot (the rest are outside this window)'), 'its line');
  ok(bgFaxHint(caps.wefaxStations, [8040000], 4400000, 2000000).bad && bgFaxHint([], [4610000], 4400000, 2000000).bad, 'none in the window, or no station reachable: said in red');
  ok(BG_FAX_MAX === 8, 'at most 8 ticks (the engine\'s kMaxFaxFreqs)');
}

console.log('── ★★★ beta97 the satellite digital-mode skimmer in satPasses ──');
{
  const m = parseBg(JSON.stringify({ on: false, satPasses: { on: true, minEl: 20, birds: [{ norad: 43803, label: 'Transponder' }], skim: true, skimMode: 'live', skimIq: true } }));
  ok(m.sat.skim && m.sat.skimMode === 'live' && m.sat.skimIq, 'read: skim, its speed, keep-the-IQ');
  const out = collectBg(m) || '';
  ok(/"skim":true/.test(out) && /"skimMode":"live"/.test(out) && /"skimIq":true/.test(out), 'written back (the Setup page\'s keys)');
  const old = parseBg(JSON.stringify({ on: false, satPasses: { on: true, minEl: 20, birds: [] } }));
  ok(!old.sat.skim && old.sat.skimMode === 'auto' && !old.sat.skimIq && !/skim/.test(collectBg(old) || ''), 'an older setting: off / auto, written back without them');
  ok(parseBg(JSON.stringify({ satPasses: { skimMode: 'warp' } })).sat.skimMode === 'auto', 'an unknown speed reads as auto');
}

// ── ★★ beta98 SSTV ON LSB (Stuart 11-28: 7171 kHz LSB is 40 m's SSTV calling frequency) — the engine's own list ──
{
  const cfgH = readFileSync(new URL('../android/app/src/main/cpp/vibe_bgdecode_config.h', import.meta.url), 'utf8');
  const decs: BgDecoderDef[] = [...cfgH.matchAll(/\{ "([a-z0-9-]+)",\s+"([^"]+)",\s+"[^"]*",\s+"[^"]*",\s+"([a-z,]+)"/g)]
    .map((x) => ({ id: x[1], label: x[2], modes: x[3].split(','), tone: 0 }));
  const lsbRuns = decs.filter((d) => d.modes.includes('lsb')).map((d) => d.id);
  ok(lsbRuns.includes('sstv') && decs.find((d) => d.id === 'sstv')!.modes.join() === 'usb,lsb,nfm', `the engine offers SSTV on USB, LSB and NFM (LSB: ${lsbRuns.join(' ')})`);
  const grey = bgUsbOnly(decs, 'lsb').map((d) => d.id);
  ok(grey.includes('wefax') && grey.includes('ft8') && !grey.includes('sstv') && !grey.includes('navtex'), `an LSB row greys the USB-only ones, "USB only" (${grey.join(' ')})`);
  ok(bgUsbOnly(decs, 'usb').length === 0 && bgUsbOnly(decs, 'nfm').length === 0, '…and only on an LSB row');
  const j = '{"on":true,"waitMin":5,"profiles":[{"centre":7100000,"from":"00:00","to":"00:00","vfos":[{"hz":7171000,"mode":"lsb","dec":"sstv"}]}]}';
  ok(collectBg(parseBg(j)) === j, 'a 7171 kHz LSB SSTV VFO round-trips byte for byte');
  // ★★ beta98 HFDL as a background VFO: in the engine's list (USB only), greyed on LSB, its profile round-trips
  const hf = decs.find((d) => d.id === 'hfdl');
  ok(!!hf && hf.label === 'HFDL' && hf.modes.join() === 'usb' && bgUsbOnly(decs, 'lsb').some((d) => d.id === 'hfdl'), 'the engine offers HFDL on USB (greyed "USB only" on LSB)');
  const h = '{"on":true,"waitMin":5,"profiles":[{"centre":8900000,"from":"00:00","to":"00:00","vfos":[{"hz":8977000,"mode":"usb","dec":"hfdl"},{"hz":8942000,"mode":"usb","dec":"hfdl"}]}]}';
  ok(collectBg(parseBg(h)) === h, 'two HFDL VFOs (8977 + 8942 kHz) round-trip byte for byte');
  ok(/published HFDL frequency/.test(BG_HFDL_NOTE) && /1440 Hz above it/.test(BG_HFDL_NOTE), 'an HFDL row says the dial is the published frequency');
}
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
