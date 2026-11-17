// test_tones_app.ts — V12 DTMF + 5-tone, the client half (src/services/tones.ts — shared by the app and the web client): the
// server's status sanitised, the attach parameters for AUTO and the manual controls, the words, and the keys wired in both
// clients and both setup screens (only where the server offers "tones"). Run: npx tsx scripts/test_tones_app.ts
import { readFileSync } from 'node:fs';
import { FIVE_TONE_STANDARDS, TONES_DEFAULTS, TONES_HINT, tonesParams, tonesSanitise, tonesStateText } from '../src/services/tones';
import { bandsOfDecoder, decoderNear, DECODER_PLACES } from '../src/utils/decoderBands';
import { modeIconFor } from '../src/utils/modeIcons';

let pass = 0, fail = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) pass++; else { fail++; console.log(`FAIL ${what}\n   got  ${g}\n   want ${w}`); }
};
const ok = (what: string, c: boolean) => eq(what, c, true);
const src = (q: string) => readFileSync(new URL('../' + q, import.meta.url), 'utf8');

// ── the standards: the server's table order and ids (decoders/tones_fivetone.h toneTables) ──
const five = src('android/app/src/main/cpp/decoders/tones_fivetone.h');
const serverIds = [...five.matchAll(/\{ "([a-z0-9]+)",\s+"([A-Z0-9-]+)",\s+\d+,/g)].map((m) => [m[1], m[2]]);
eq('the standards are the server\'s, in its order (AUTO first)', FIVE_TONE_STANDARDS.slice(1).map((s) => [s.id, s.label]), serverIds);
eq('AUTO first', FIVE_TONE_STANDARDS[0], { id: 'auto', label: 'AUTO' });

// ── the attach parameters ──
eq('both on, AUTO: nothing', tonesParams(TONES_DEFAULTS), {});
eq('null: nothing', tonesParams(null), {});
eq('DTMF off', tonesParams({ ...TONES_DEFAULTS, dtmf: false }), { dtmf: false });
eq('5-tone off', tonesParams({ ...TONES_DEFAULTS, five: false }), { five: false });
eq('a pin', tonesParams({ ...TONES_DEFAULTS, standard: 'dzvei' }), { standard: 'dzvei' });
eq('an unknown pin is AUTO', tonesParams({ ...TONES_DEFAULTS, standard: 'golay' }), {});

// ── the status ──
const s = tonesSanitise({ amateur: true, band: '2 m amateur', dtmf: true, five: true, pin: 'ccir', digits5: false })!;
eq('sanitised', s, { amateur: true, band: '2 m amateur', dtmf: true, five: true, pin: 'ccir', digits5: false });
eq('junk is not a status', tonesSanitise('x'), null);
eq('an unknown pin reads AUTO, a non-string band empty', tonesSanitise({ pin: 'x', band: 7 })!, { amateur: false, band: '', dtmf: true, five: true, pin: 'auto', digits5: false });
eq('amateur: digits shown', tonesStateText(0, s), 'listening for DTMF and 5-tone · 2 m amateur: digits shown');
eq('not amateur: no digits', tonesStateText(1, { ...s, amateur: false, band: '' }), 'tones… · not an amateur frequency: what was heard, no digits');
eq('before the status', tonesStateText(0, null), 'listening for DTMF and 5-tone');
ok('the hint says where digits are shown', TONES_HINT.includes('amateur bands') && TONES_HINT.includes('DTMF heard') && TONES_HINT.includes('ZVEI'));

// ── where the key sits ──
eq('VHF and UHF tabs', bandsOfDecoder('tones'), ['vhf', 'uhf']);
ok('…under Amateur', DECODER_PLACES.vhf.some((g) => g.group === 'amateur' && g.ids.includes('tones')) && DECODER_PLACES.uhf.some((g) => g.group === 'amateur' && g.ids.includes('tones')));
ok('near on 2 m and PMR446, not on 7 MHz', decoderNear('tones', 145.5e6) && decoderNear('tones', 446.1e6) && !decoderNear('tones', 7.1e6));
ok('it has a glyph', modeIconFor('tones') === 'keyboard');

// ── both clients wired, and only where the server offers it ──
const html = src('web/client/index.html'), main = src('web/client/src/main.ts'), dec = src('web/client/src/decoders.ts');
ok('web: the key, hidden until the server says so', /data-dec="tones" hidden/.test(html));
ok('web: shown only where "tones"', main.includes('tonesCap = j.tones === true;'));
ok('web: the options under the key', html.includes('id="tonesSettings"') && html.includes('id="tonesWhich"') && html.includes('id="tonesStd"')
   && src('web/client/src/decoderBandsDom.ts').includes("tones: 'tonesSettings'"));
ok('web: the choices are what is attached', main.includes("if (mode === 'tones') return tonesParams(tonesSet);"));
ok('web: the status JSON and the lines routed', dec.includes("msg.type === 'tones'") && dec.includes("tones: 'tones'") && dec.includes("case 'tones':"));
const ms = src('src/components/ModeSelector.tsx'), sd = src('src/screens/SDRScreen.tsx'), dc = src('src/services/DecoderClient.ts');
ok('app: the key only where offered', ms.includes("const tonesShown = !!decoderControls?.tonesAvail && !isBlocked('tones')") && sd.includes('tonesAvail: tonesCapable'));
ok('app: DECODE and 5-TONE STANDARD under the key', ms.includes('<SubLabel label="5-TONE STANDARD" />') && sd.includes('tonesSettings, onTonesSettings'));
ok('app: a change re-attaches', sd.includes("decoderClient.current.start('tones')"));
ok('app: attach + JSON + state', dc.includes("case 'tones': return { extension_name: 'tones', params: tonesParams(this.tonesSet) };")
   && dc.includes("m.type === 'tones'") && dc.includes("name === 'tones' && t === 0x03"));
ok('app: the capability read', src('src/services/sdrTypes.ts').includes('tones:     j.tones === true'));

// ── both setup screens: the switch and the owner's opt-in under it, with the same notice ──
const page = src('android/app/src/main/cpp/vibe_setup_page.h'), sms = src('src/screens/ServerModeScreen.tsx');
ok('setup page: the switch and the opt-in under it', page.includes('{ id: "tones", label: "DTMF and 5-tone selective calling" }')
   && page.includes('{ id: "tones+digits", label: "5-tone: show the called numbers off the amateur bands (to you only)", parent: "tones", on: true }'));
ok('app: the same pair', sms.includes("{ id: 'tones', label: 'DTMF and 5-tone selective calling' }")
   && sms.includes("{ id: 'tones+digits', label: '5-tone: show the called numbers off the amateur bands (to you only)', parent: 'tones', on: true }"));
const notice = (t: string) => (/TONES_DIGITS_NOTICE = ([\s\S]*?);\n/.exec(t)?.[1] ?? '').replace(/\\u2019|’|'/g, "'").replace(/["'\s+]/g, '');
ok('…with the same notice on both', notice(page).length > 100 && notice(page) === notice(sms));

// ── the copy that says where the key is ──
ok('About: the V12 note', src('src/components/AboutOverlay.tsx').includes('DTMF AND 5-TONE SELECTIVE CALLING'));
// ★ the tour's mode card is an overview now (Stuart 2026-11-16) — it names no key's place; About and the web tutorial do
ok('the tour\'s mode card stays an overview (no DTMF place to go stale); About says where the key is', !(() => { const t = sd; const i = t.indexOf("{ id: 'mode', title:"); return i < 0 ? '' : t.slice(i, t.indexOf("{ id: '", i + 5)); })().includes('DTMF') && src('src/components/AboutOverlay.tsx').includes('VHF and UHF tabs of the decoders) reads keypad tones'));
ok('the web tutorial says where the key is', src('web/client/src/tutorial.ts').includes('on the VHF and UHF tabs DTMF / 5-TONE'));

console.log(`${fail ? 'FAIL' : 'ok '}  test_tones_app — ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
