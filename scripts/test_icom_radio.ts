// test_icom_radio.ts — an Icom as the server describes it (src/services/icomRadio.ts): hwinfo.radio and the
// `{"type":"icom"}` status line parsed exactly as radioCapsJsonCore()/icomStatusJson() build them, the hide
// list against the contract (docs/ICOM-SUPPORT-PLAN.md "Client controls that do NOT apply on an Icom"), the
// owner lock / RF-admin decisions, the span ladder, the S-meter, and an icom_control that can only ever carry
// values the radio listed — and no transmit field at all.
import {
  parseIcomRadio, parseIcomStatus, mergeIcomStatus, isIcom, icomHides, ICOM_HIDDEN_CONTROLS,
  icomMayTune, icomMayRf, icomFollowNote, icomModeLabel, icomClientMode, icomWidthFamily, icomFilterWidths,
  fmtWidth, fmtSpan, nearestSpanIndex, spanStep, sMeter, baudSweepsEstimate, baudNote, icomControlMsg,
  icomBanner, fmtAtt, fmtPreamp, rfGainPct,
} from '../src/services/icomRadio.ts';

let pass = 0, fail = 0;
const eq = (a: unknown, b: unknown, m: string) => {
  const ok = JSON.stringify(a) === JSON.stringify(b);
  if (ok) pass++; else { fail++; console.log(`  FAIL ${m}: ${JSON.stringify(a)} ≠ ${JSON.stringify(b)}`); }
};
const yes = (c: boolean, m: string) => eq(c, true, m);

// ★ The contract's own example, verbatim (the server's field order and types).
const HW = JSON.parse(`{"driver":"icom","model":"IC-7300","civ":"94","link":"usb","transport":"cp210x","linesHeldLow":true,
 "baud":115200,"noIq":true,"sharedOnly":true,"canTransmit":false,
 "modes":["lsb","usb","am","cw","rtty","fm","cwr","rttyr","usbd","lsbd"],"modeNow":"usbd",
 "filters":[1,2,3],"filNow":1,
 "filterWidths":{"ssb":[3000,2400,1800],"cw":[1200,500,250],"rtty":[2400,500,250],"am":[9000,6000,3000],"fm":[15000,10000,7000]},
 "att":[0,20],"attNow":0,"preamp":[0,1,2],"preampNow":1,"agc":["fast","mid","slow"],"agcNow":"mid",
 "rfGain":[0,255],"rfGainNow":255,"nb":true,"nbNow":false,"nr":true,"nrNow":false,"anf":true,"anfNow":false,
 "scope":{"points":475,"spans":[5000,10000,20000,50000,100000,200000,500000,1000000],"span":50000,
          "modes":["centre","fixed"],"mode":"centre","speeds":["fast","mid","slow"],"fps":19.9},
 "ranges":[[30000,74800000]],"tx":false,"linkUp":true,"ownerLock":true,"rfOpen":false,
 "audio":"ALSA hw:2,0 (CODEC)","audioProblem":"",
 "preflight":{"passed":true,"verified":false,"summary":"Safe to receive",
              "items":[{"name":"USB SEND","value":"OFF","ok":true,"gate":true,"where":"MENU > SET > Connectors > USB SEND"}]},
 "refusedFrames":0}`);

// ── parse ──
const r = parseIcomRadio(HW)!;
yes(!!r, 'the contract example parses');
eq(r.model, 'IC-7300', 'model');
eq(r.civ, '94', 'CI-V address');
eq(r.baud, 115200, 'baud');
eq(r.modes.length, 10, 'modes from the radio');
eq(r.modeNow, 'usbd', 'modeNow');
eq(r.filterWidths.ssb, [3000, 2400, 1800], 'FIL widths, ssb');
eq(r.att, [0, 20], 'ATT list in dB');
eq(r.preamp, [0, 1, 2], 'preamp list');
eq(r.rfGain, [0, 255], 'RF gain range');
eq(r.scope.spans.length, 8, 'eight span rungs');
eq(r.scope.fps, 19.9, 'scope fps');
eq(r.ranges, [[30000, 74800000]], 'ranges');
eq(r.ownerLock, true, 'owner lock');
eq(r.preflight?.items[0].where, 'MENU > SET > Connectors > USB SEND', 'preflight names the menu item');
eq(r.canTransmit, false, 'canTransmit false');
eq(parseIcomRadio({ ...HW, canTransmit: true })?.canTransmit, false, '★ canTransmit is false whatever arrives');
eq(parseIcomRadio({ driver: 'rtl', model: 'x' }), null, 'an RTL is not an Icom');
eq(parseIcomRadio(null), null, 'null');
eq(parseIcomRadio({ driver: 'icom' })?.ownerLock, true, '★ an absent ownerLock stays LOCKED (the server default)');
eq(parseIcomRadio({ driver: 'icom' })?.linkUp, true, 'absent linkUp is not "down"');
eq(parseIcomRadio({ driver: 'icom', rfGain: 'x', modes: [1, 'usb', null], att: ['20', 0] })?.modes, ['usb'], 'junk list entries dropped');
eq(parseIcomRadio({ driver: 'icom', rfGain: 'x' })?.rfGain, null, 'a junk rfGain = none');
eq(parseIcomRadio({ driver: 'icom', att: ['20', 0] })?.att, [0], 'a string ATT step is not trusted');
yes(isIcom(HW) && !isIcom({ driver: 'airspyhf' }) && !isIcom(undefined), 'isIcom');

// ── status line ──
const S = parseIcomStatus(JSON.parse('{"type":"icom","freq":7074000,"mode":"usbd","fil":1,"sRaw":48,"sDbOverS9":-30.4,"tx":false,"link":true,"fps":19.9,"span":50000,"scopeMode":"centre"}'))!;
eq(S.freq, 7074000, 'status freq');
eq(S.sDbOverS9, -30.4, 'status S-meter');
eq(parseIcomStatus({ type: 'icom', sRaw: -1, sDbOverS9: -999 })?.sDbOverS9, null, 'an unread meter is null, not S0');
eq(parseIcomStatus({ type: 'config' }), null, 'not a status line');
const m = mergeIcomStatus(r, { ...S, mode: 'cw', fil: 2, tx: true, link: true, span: 10000, scopeMode: 'fixed', fps: 3 });
eq([m.modeNow, m.filNow, m.tx, m.scope.span, m.scope.mode, m.scope.fps], ['cw', 2, true, 10000, 'fixed', 3], 'the status line updates the radio');
eq(r.modeNow, 'usbd', 'merge does not mutate');

// ── what is hidden (the contract's list, every item) ──
for (const c of ['sampleRate', 'gainSlider', 'vibeAgc', 'ifFilter', 'directSampling', 'biasT', 'ppm', 'radioPanels',
                 'squelch', 'noiseBlanker', 'rds', 'stereo', 'deemphasis', 'nfmVoice', 'dab', 'rawIq', 'iqRecording',
                 'vibeIq', 'multiVfo', 'fpsTiers', 'spatialise', 'binaural', 'digitalZoom', 'iqDecoders', 'wfmMode'])
  yes(icomHides(r, c), `hidden on an Icom: ${c}`);
eq(icomHides({ driver: 'rtl' }, 'sampleRate'), false, 'an RTL hides nothing here');
eq(icomHides(r, 'decoders'), false, 'audio decoders stay (FT8/RTTY/CW/WEFAX/SSTV work on the radio audio)');
eq(icomHides(r, 'nr'), false, 'client-side NR stays (it works on the radio audio)');
eq(new Set(ICOM_HIDDEN_CONTROLS).size, ICOM_HIDDEN_CONTROLS.length, 'no duplicate ids');

// ── owner lock / RF admin ──
eq(icomMayTune(r, false), false, 'owner lock ON: a listener may not tune');
eq(icomMayTune(r, true), true, 'owner lock ON: the signed-in admin may');
eq(icomMayTune({ ...r, ownerLock: false }, false), true, 'owner lock OFF: anyone tunes');
eq(icomMayRf(r, false), false, 'RF admin-only by default');
eq(icomMayRf({ ...r, rfOpen: true }, false), true, 'RF opened by the owner');
eq(icomMayRf(r, true), true, 'admin always has RF');
yes(icomFollowNote(r, false).startsWith('Everyone hears what the radio is tuned to'), 'the follow note for a listener');
eq(icomFollowNote(r, true), '', 'no follow note for the admin');
eq(icomFollowNote({ ...r, ownerLock: false }, false), '', 'no follow note when unlocked');

// ── labels ──
eq(['usbd', 'lsbd', 'cwr', 'rttyr', 'rtty', 'am', 'fm', 'usb'].map(icomModeLabel),
   ['USB-D', 'LSB-D', 'CW-R', 'RTTY-R', 'RTTY', 'AM', 'FM', 'USB'], 'mode labels as the radio writes them');
eq(['usb', 'usbd', 'rttyr', 'lsb', 'lsbd', 'rtty', 'am', 'cw', 'cwr', 'fm'].map(icomClientMode),
   ['usb', 'usb', 'usb', 'lsb', 'lsb', 'lsb', 'am', 'cwu', 'cwl', 'nfm'], 'client vocabulary = the server mapping');
eq(['usbd', 'lsb', 'cwr', 'rttyr', 'am', 'fm'].map(icomWidthFamily), ['ssb', 'ssb', 'cw', 'rtty', 'am', 'fm'], 'width families');
eq(icomFilterWidths(r), [3000, 2400, 1800], 'FIL widths for USB-D');
eq(icomFilterWidths(r, 'cwr'), [1200, 500, 250], 'FIL widths for CW-R');
eq([fmtWidth(2400), fmtWidth(500), fmtWidth(15000), fmtWidth(0)], ['2.4k', '500', '15k', ''], 'width text');
eq([fmtSpan(50000), fmtSpan(5000), fmtSpan(1000000)], ['±25k', '±2.5k', '±500k'], 'span labels (half width, like the radio)');
eq([fmtAtt(0), fmtAtt(20), fmtPreamp(0), fmtPreamp(2)], ['OFF', '20 dB', 'OFF', 'P.AMP2'], 'ATT / preamp labels');
eq(rfGainPct(255, [0, 255]), 100, 'RF gain %');

// ── span ladder (zoom) ──
const sp = r.scope.spans;
eq(nearestSpanIndex(sp, 50000), 3, 'exact rung');
eq(nearestSpanIndex(sp, 70000), 3, '70 kHz is nearest 50 kHz (log)');
eq(nearestSpanIndex(sp, 80000), 4, '80 kHz is nearest 100 kHz (log)');
eq(spanStep(sp, 50000, +1), 20000, 'zoom in one rung');
eq(spanStep(sp, 50000, -1), 100000, 'zoom out one rung');
eq(spanStep(sp, 5000, +1), 5000, 'the narrowest rung stays');
eq(spanStep(sp, 1000000, -1), 1000000, 'the widest rung stays');
eq(spanStep([], 50000, 1), 0, 'no ladder');

// ── S-meter ──
eq(sMeter(0)?.text, 'S9', 'S9');
eq(sMeter(-30.4)?.text, 'S4', '-30 dB = S4 (6 dB per unit)');
eq(sMeter(-54)?.text, 'S0', 'S0');
eq(sMeter(20)?.text, 'S9+20', 'S9+20');
eq(sMeter(60)?.frac, 1, 'S9+60 = full scale');
eq(sMeter(-54)?.frac, 0, 'S0 = empty');
eq(Math.round(sMeter(0)!.frac * 100), 60, 'S9 at 60 % of the bar');
eq(sMeter(null), null, 'no reading, no meter');
yes(sMeter(10)!.over && !sMeter(-1)!.over, 'over S9 flagged');

// ── baud ──
eq(baudSweepsEstimate(115200), 19, '115200 ≈ 19/s');
eq(baudSweepsEstimate(19200), 3, '19200 ≈ 3/s');
yes(baudNote(115200).includes('19200 only about 3'), 'the baud note names both figures');
yes(baudNote(19200).includes('set the radio and the server to 115200'), 'a slow baud is told how to fix it');

// ── what a client sends ──
eq(icomControlMsg(r, { mode: 'rtty' }), { type: 'icom_control', mode: 'rtty' }, 'a mode the radio has');
eq(icomControlMsg(r, { mode: 'wfm' }), null, 'a mode it has not: nothing sent');
eq(icomControlMsg(r, { fil: 2, span: 20000 }), { type: 'icom_control', fil: 2, span: 20000 }, 'FIL + span');
eq(icomControlMsg(r, { span: 30000 }), null, 'a span that is not a rung');
eq(icomControlMsg(r, { att: 10 }), null, 'an ATT step the radio does not have');
eq(icomControlMsg(r, { att: 20, preamp: 0, agc: 'slow' }), { type: 'icom_control', att: 20, preamp: 0, agc: 'slow' }, 'RF set');
eq(icomControlMsg(r, { rfGain: 300 }), { type: 'icom_control', rfGain: 255 }, 'RF gain clamped');
eq(icomControlMsg(r, { nb: true, nr: false, anf: true }), { type: 'icom_control', nb: true, nr: false, anf: true }, "the RADIO's NB/NR/ANF");
eq(icomControlMsg(r, { scopeMode: 'fixed', scopeSpeed: 'slow' }), { type: 'icom_control', scopeMode: 'fixed', scopeSpeed: 'slow' }, 'scope');
eq(icomControlMsg(null, { mode: 'usb' }), null, 'no radio, nothing');
// ★★★ RECEIVE ONLY: nothing outside the receive fields survives, whatever a caller passes.
const sneaky = icomControlMsg(r, { mode: 'usb', ...({ tx: true, ptt: 1, power: 100, civ: 'FEFE941C0001FD' } as object) });
eq(sneaky, { type: 'icom_control', mode: 'usb' }, '★★★ no tx/ptt/power/raw CI-V field can be built');

// ── banner ──
eq(icomBanner({ ...r, tx: true }), 'The radio is transmitting — receive paused', 'TX banner');
yes(icomBanner({ ...r, linkUp: false }).startsWith('The radio is not answering'), 'link-down banner');
eq(icomBanner({ ...r, tx: true, linkUp: false }), 'The radio is transmitting — receive paused', 'TX outranks link-down');
eq(icomBanner(r), '', 'no banner normally');

console.log(`icom radio (client contract): ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
