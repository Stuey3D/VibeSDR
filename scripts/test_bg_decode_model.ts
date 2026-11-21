// test_bg_decode_model.ts — the model behind the Android server screen's Background Decoding editor
// (src/services/bgDecode.ts): RadioConfig::bgDecode's JSON read and written exactly as the Setup page's bgLoad /
// bgCollect do, the engine's caps read, and the window / NDB / Band / DSC rules the editor's lines come from.
import {
  parseBg, collectBg, parseBgCaps, bgNewProfile, bgHalfWindow, bgNdbSegment, bgNdbHint, bgBandReach, bgBandHint,
  bgDscWatch, bgLocal, bgTime, bgReach, bgDecoderSetsFreq, type BgSpotBand,
  bgSkimCentre, bgSkimBandAt, bgSkimFits, bgSkimFirstFit, bgSkimHint, bgSkimDecoderChange, parseSkimBands, type BgVfo,
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
  ok(m.sat.on && m.sat.minEl === 30 && m.sat.birds.length === 1 && m.sat.birds[0].label === 'LRPT', 'satPasses read; a bad entry dropped');
  ok(collectBg(m) === '{"on":false,"waitMin":5,"profiles":[],"satPasses":{"on":true,"minEl":30,"birds":[{"norad":57166,"label":"LRPT"}]}}', `stored with background off: ${collectBg(m)}`);
  const e = parseBg('');
  ok(!e.sat.on && e.sat.birds.length === 0 && collectBg(e) === '', 'untouched: nothing stored');
  const c2 = parseBgCaps('{"decoders":[{"id":"navtex","label":"NAVTEX","modes":["usb"],"tone":500}],"satPasses":[{"norad":57166,"label":"LRPT","name":"Meteor-M2-3","what":"weather pictures (LRPT)"}]}');
  ok(!!c2 && c2.satPasses.length === 1 && c2.satPasses[0].name === 'Meteor-M2-3', 'the engine\'s keepable downlinks reach the editor');
  ok(parseBgCaps('{"decoders":[{"id":"navtex","label":"NAVTEX","modes":["usb"],"tone":500}]}')!.satPasses.length === 0, 'an older engine: none (the section is not drawn)');
}

// ── ★★ V12 beta90 THE SKIMMERS BY BAND (the engine's skimBands; centre = segment bottom + width / 2) ──
{
  const caps = parseBgCaps(JSON.stringify({
    decoders: [{ id: 'cw-skim-24', label: 'CW skimmer, 24 kHz (summary)', modes: ['usb'], tone: 0, width: 24000, skim: 'cw' },
               { id: 'cw-skim-48', label: 'CW skimmer, 48 kHz (summary)', modes: ['usb'], tone: 0, width: 48000, skim: 'cw' },
               { id: 'cw-skim-96', label: 'CW skimmer, 96 kHz (summary)', modes: ['usb'], tone: 0, width: 96000, skim: 'cw' },
               { id: 'rtty-skim-24', label: 'RTTY skimmer, 24 kHz (summary)', modes: ['usb'], tone: 0, width: 24000, skim: 'rtty' },
               { id: 'navtex', label: 'NAVTEX', modes: ['usb'], tone: 500 }],
    skimBands: { region: 1, cw: [{ id: '40m', name: '40 m', lo: 7000000, hi: 7040000 }, { id: '20m', name: '20 m', lo: 14000000, hi: 14070000 }],
                 rtty: [{ id: '40m', name: '40 m', lo: 7040000, hi: 7050000 }], psk: [] },
  }))!;
  ok(!!caps.skimBands && caps.skimBands.cw.length === 2 && caps.decoders[1].skim === 'cw' && caps.decoders[1].width === 48000, 'the engine\'s skimBands and each skimmer\'s kind + width reach the editor');
  ok(parseSkimBands(undefined) === null && parseBgCaps('{"decoders":[{"id":"navtex","label":"N","modes":["usb"],"tone":0}]}')!.skimBands === null, 'an older engine: no skimBands (the typed box stays)');
  const b40 = caps.skimBands!.cw[0];
  ok(bgSkimCentre(b40, 24000) === 7012000 && bgSkimCentre(b40, 48000) === 7024000 && bgSkimCentre(b40, 96000) === 7048000, '40 m CW: 24 → 7012, 48 → 7024, 96 → 7048 (Stuart\'s figures)');
  ok(bgSkimBandAt(caps.skimBands!.cw, 48000, 7024000)?.id === '40m' && bgSkimBandAt(caps.skimBands!.cw, 48000, 7020000) === null, 'a stored 7024 is 40 m; an owner\'s 7020 is their own centre');
  ok(bgSkimFits(7024000, 48000, 7000000, 2400000) && !bgSkimFits(7048000, 96000, 6900000, 250000), 'capture fit: 7024/48 inside 2.4 MS/s at 7.0; 7048/96 outside 250 kS/s at 6.9');
  ok(bgSkimFirstFit(caps.skimBands!.cw, 48000, 14100000, 2400000)?.id === '20m', 'a new row at 14.1 MHz lands on 20 m (the first band whose window fits)');
  const d48 = caps.decoders[1], d96 = caps.decoders[2], dr = caps.decoders[3], nav = caps.decoders[4];
  const h = bgSkimHint(d48, true, false, caps.skimBands!.cw, 7024000, false, 7000000, 2400000);
  ok(!h.bad && h.text.startsWith('40 m CW 7000–7048 kHz — every CW signal in it'), 'its line — the Setup page\'s words: ' + h.text);
  const ho = bgSkimHint(d48, true, false, caps.skimBands!.cw, 7031500, true, 7000000, 2400000);
  ok(!ho.bad && ho.text.startsWith('Your own centre: 7007.5–7055.5 kHz'), 'an own centre says what it covers: ' + ho.text);
  ok(bgSkimHint(d48, true, false, caps.skimBands!.cw, 7024000, false, 14100000, 2400000).bad, 'outside the capture: said in red');
  ok(bgSkimHint(dr, false, true, caps.skimBands!.rtty, 7052000, false, 7000000, 2400000).text.includes('an Icom'), 'cannot run (an Icom): says why');
  const v: BgVfo = { hz: 7024000, mode: 'usb', dec: 'cw-skim-48' };
  bgSkimDecoderChange(v, d48, d96, caps.skimBands, 7000000, 2400000);
  ok(v.hz === 7048000, 'CW 48 → 96 kHz keeps the band: 7024 → 7048');
  const o: BgVfo = { hz: 7031500, mode: 'usb', dec: 'cw-skim-48', own: true };
  bgSkimDecoderChange(o, d48, d96, caps.skimBands, 7000000, 2400000);
  ok(o.hz === 7031500 && o.own === true, '…an owner\'s own centre stays theirs');
  const n: BgVfo = { hz: 517500, mode: 'usb', dec: 'navtex' };
  bgSkimDecoderChange(n, nav, dr, caps.skimBands, 7000000, 2400000);
  ok(n.hz === 7052000, 'NAVTEX → RTTY skimmer 24 kHz lands on 40 m, 7052 (7040 + 12)');
  ok(JSON.parse(collectBg({ ...parseBg(''), on: true, profiles: [{ centre: 7000000, from: '00:00', to: '00:00', vfos: [o] }] })).profiles[0].vfos[0].own === undefined,
     '"own" is the editor\'s, never stored');
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
