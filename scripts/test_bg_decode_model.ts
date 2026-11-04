// test_bg_decode_model.ts — the model behind the Android server screen's Background Decoding editor
// (src/services/bgDecode.ts): RadioConfig::bgDecode's JSON read and written exactly as the Setup page's bgLoad /
// bgCollect do, the engine's caps read, and the window / NDB / Band / DSC rules the editor's lines come from.
import {
  parseBg, collectBg, parseBgCaps, bgNewProfile, bgHalfWindow, bgNdbSegment, bgNdbHint, bgBandReach, bgBandHint,
  bgDscWatch, bgLocal, bgTime, bgReach, bgDecoderSetsFreq, type BgSpotBand,
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

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
