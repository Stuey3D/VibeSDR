/**
 * beta91 SATELLITE LABELS ON THE WATERFALL (src/utils/knownSignals.ts parseSatLabels / satLabelSignals — Stuart,
 * 2026-11-22: "we dont even need to be in satellite mode either, i could simply be listening to 2m band and if it appears
 * we can label it"). The server (vibe_sat_engine.h skyLabels) sends `sat_labels`; this proves the client half, which the
 * app (WaterfallView via SDRScreen) and the web client (waterfall.ts via main.ts) share:
 *   • the message checked: bad numbers, no name, a silly band, too many — dropped; modes kept to the listening ones;
 *   • ONE TEXT: "ISS FM", "Meteor-M2-4 LRPT", "SO-50 FM", "RS-44 linear", "ORBCOMM FM114";
 *   • the mark at the Doppler-shifted frequency, carried along the rate between messages (≤ 8 s);
 *   • label-only ORBCOMM birds over the band merged into one label; one with its own channel stays its own;
 *   • priorities, the sky-blue colour, the hint (ORBCOMM: decoded from a SATELLITE pick, subscriber messages never); a tap
 *     tunes, never opens a decoder;
 *   • the layout takes them with the other labels (two Meteors on 137.9 stack, not overlap).
 * Run: node --no-warnings scripts/test_sat_labels.ts
 */
import { parseSatLabels, satLabelSignals, satLabelText, satShortName, satTxWord, isSatLabel, satLabelHint, withSatLabels,
         knownLabelColour, SAT_LABEL_COLOUR, DX_LABEL_COLOUR, KNOWN_LABEL_COLOUR, PRIO_SAT, PRIO_EIBI, PRIO_TABLE, PRIO_PERSONAL,
         SATLABEL_MAX, knownTapPlan, signalsInView, labelLayout, estimateLabelWidth, KNOWN_SIGNALS, type KnownSignal } from '../src/utils/knownSignals.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };

// A message as vibe_sat_engine.h skyLabelsJson writes it (an ISS pass over São Paulo, Meteor and ORBCOMM over IO92)
const msg = {
  type: 'sat_labels', v: 1, t: 1795609669,
  sats: [
    { id: 'ISS', n: 'ISS', norad: 25544, tx: 'FM repeater', hz: 437806532, r: -61.2, el: 37.9, m: 'nfm' },
    { id: 'ISS', n: 'ISS', norad: 25544, tx: 'Voice / SSTV', hz: 145802254, r: -20.4, el: 37.9, m: 'nfm' },
    { id: 'SO-50', n: 'SO-50 (SaudiSat-1C)', norad: 27607, tx: 'FM repeater', hz: 436791000, r: 40, el: 12, m: 'nfm' },
    { id: 'RS-44', n: 'RS-44', norad: 44909, tx: 'Transponder', hz: 435641000, lo: 435611000, hi: 435671000, r: 10, el: 20, m: 'usb' },
    { id: 'METEOR-M2-4', n: 'Meteor-M2-4', norad: 59051, tx: 'LRPT', hz: 137902100, r: -12, el: 44, m: 'nfm' },
    { id: 'METEOR-M2-3', n: 'Meteor-M2-3', norad: 57166, tx: 'LRPT', hz: 137897400, r: 9, el: 8, m: 'nfm' },
    { id: 'ORBCOMM-FM114', n: 'ORBCOMM FM114', norad: 41185, tx: 'ORBCOMM', hz: 137501000, lo: 137201000, hi: 137801000, r: -9, el: 30, m: 'nfm', b: true },
    { id: 'ORBCOMM-FM116', n: 'ORBCOMM FM116', norad: 41187, tx: 'ORBCOMM', hz: 137498000, lo: 137198000, hi: 137798000, r: 8, el: 10, m: 'nfm', b: true },
    { id: 'ORBCOMM-FM108', n: 'ORBCOMM FM108', norad: 41179, tx: 'ORBCOMM', hz: 137711500, r: 4, el: 22, m: 'nfm', b: true },
    // refused:
    { id: 'BAD', n: 'Bad', norad: 1, tx: 'x', hz: Number.NaN, r: 0, el: 0, m: 'nfm' },
    { id: 'NONAME', n: '  ', norad: 2, tx: 'x', hz: 145e6, r: 0, el: 0, m: 'nfm' },
    { id: 'LOW', n: 'Low', norad: 3, tx: 'x', hz: 500e3, r: 0, el: 0, m: 'nfm' },
  ],
};

console.log('the message checked');
const es = parseSatLabels(msg);
ok(es.length === 9, `9 good entries kept, 3 refused (${es.length})`);
ok(parseSatLabels(null).length === 0 && parseSatLabels({ sats: 'x' }).length === 0 && parseSatLabels({}).length === 0, 'not a message: nothing');
ok(parseSatLabels({ sats: Array.from({ length: 40 }, (_, i) => ({ n: 'S' + i, hz: 145e6 + i * 1e4 })) }).length === SATLABEL_MAX, `at most ${SATLABEL_MAX}`);
const weird = parseSatLabels({ sats: [{ n: 'W', hz: 145e6, lo: 140e6, hi: 150e6, m: 'lrpt', r: 'x' }] })[0];
ok(weird.lo == null && weird.hi == null && weird.m === 'nfm' && weird.r === 0, 'a 10 MHz "band" dropped, an unknown mode → NFM, a bad rate → 0');
ok(es.find((e) => e.id === 'RS-44')!.lo === 435611000 && es.filter((e) => e.b).length === 3, 'a passband kept; label-only flags kept');

console.log('ONE TEXT');
ok(satShortName('SO-50 (SaudiSat-1C)') === 'SO-50' && satShortName('ISS') === 'ISS' && satShortName('AO-73 (FUNcube-1)') === 'AO-73', 'the short name drops the long form');
ok(satTxWord('FM repeater') === 'FM' && satTxWord('Mode B transponder') === 'linear' && satTxWord('CW beacon') === 'beacon'
   && satTxWord('SSTV events') === 'SSTV' && satTxWord('LRPT') === 'LRPT', 'the downlink in a word');
ok(satLabelText('ISS', 'FM repeater') === 'ISS FM' && satLabelText('Meteor-M2-4', 'LRPT') === 'Meteor-M2-4 LRPT'
   && satLabelText('ORBCOMM FM114', 'ORBCOMM') === 'ORBCOMM FM114' && satLabelText('RS-44', 'Transponder') === 'RS-44 linear',
   'Stuart\'s words: "ISS FM", "Meteor-M2-4 LRPT", "ORBCOMM FM114"');

console.log('labels');
const sigs = satLabelSignals(es);
const by = (l: string) => sigs.find((s) => s.label === l);
const iss = by('ISS FM');
ok(!!iss && iss.hz === 437806532 && iss.dialHz === 437806532 && iss.mode === 'nfm' && iss.dec === '' && iss.src === 'sat' && isSatLabel(iss),
   'ISS FM at its Doppler-shifted 437.8065 MHz, NFM, no decoder');
ok(!!by('ISS Voice/SSTV') && !!by('SO-50 FM') && !!by('Meteor-M2-4 LRPT') && !!by('Meteor-M2-3 LRPT'), 'every bird named');
const rs = by('RS-44 linear');
ok(!!rs && rs.lo === 435611000 && rs.hi === 435671000 && rs.mode === 'usb', 'a linear transponder: its shifted passband as the bar, USB');
const orb = sigs.filter((s) => s.sat?.labelOnly);
ok(orb.length === 2, `ORBCOMM: the two band birds merged into one label, FM108 (own channel) its own (${orb.map((s) => s.label).join(' / ')})`);
const band = orb.find((s) => s.lo != null)!, fm108 = orb.find((s) => s.lo == null)!;
ok(band.label === 'ORBCOMM FM114 +1' && band.sat!.names.join() === 'ORBCOMM FM114,ORBCOMM FM116' && band.lo === 137201000 && band.hi === 137801000,
   'the band label named after the highest (FM114 +1), over the band');
ok(fm108.label === 'ORBCOMM FM108' && fm108.hz === 137711500 && fm108.prio === PRIO_EIBI && fm108.dec === '', 'FM108 at its own 137.7115 MHz, low priority, no decoder');
ok(sigs.every((s, i) => i === 0 || sigs[i - 1].hz <= s.hz), 'sorted by frequency');
const later = satLabelSignals(es, 4);
ok(later.find((s) => s.label === 'ISS FM')!.hz === Math.round(437806532 - 61.2 * 4), '4 s after the message: carried along its rate');
ok(satLabelSignals(es, 60).find((s) => s.label === 'ISS FM')!.hz === Math.round(437806532 - 61.2 * 8), '…never more than 8 s of it');
ok(satLabelSignals([]).length === 0 && satLabelSignals(null).length === 0, 'nothing in, nothing out');

console.log('priority, colour, words, tap');
ok(iss!.prio === PRIO_SAT && PRIO_SAT < PRIO_TABLE && PRIO_SAT > PRIO_PERSONAL, 'below the curated table, above every bookmark');
ok(knownLabelColour(iss) === SAT_LABEL_COLOUR && SAT_LABEL_COLOUR !== DX_LABEL_COLOUR && SAT_LABEL_COLOUR !== KNOWN_LABEL_COLOUR
   && knownLabelColour(KNOWN_SIGNALS[0]) === KNOWN_LABEL_COLOUR, 'its own sky blue; a curated label keeps the lilac');
ok(/SATELLITE pick/.test(satLabelHint(band)) && /subscriber messages are never decoded/.test(satLabelHint(band)) && !/named only/.test(satLabelHint(band))
   && /ORBCOMM FM116/.test(satLabelHint(band)) && /Doppler/.test(satLabelHint(iss!)) && /38° up/.test(satLabelHint(iss!)),
   'the hint: ORBCOMM decoded from a SATELLITE pick (subscriber messages never); the ISS how high, Doppler-shifted');
const tap = knownTapPlan(iss!, { rtty: true, wefax: true, navtex: true, dsc: true, spots: true, time: true, hfdl: true, cw: true });
ok(tap.hz === 437806532 && tap.mode === 'nfm' && tap.dec === '', 'a tap tunes to where it is now, NFM — no decoder opened');
ok(knownTapPlan(band, {}).dec === '' && knownTapPlan(band, {}).mode === 'nfm', 'an ORBCOMM tap only tunes (never a decoder)');

console.log('with the other labels');
const merged = withSatLabels(KNOWN_SIGNALS as KnownSignal[], sigs);
ok(merged.length === KNOWN_SIGNALS.length + sigs.length && merged.every((s, i) => i === 0 || merged[i - 1].hz <= s.hz), 'one sorted list');
ok(withSatLabels(KNOWN_SIGNALS as KnownSignal[], []) === (KNOWN_SIGNALS as unknown), 'no satellites: the same list (no re-layout)');
const view = signalsInView(merged, 137.0e6, 138.0e6);
ok(view.filter(isSatLabel).length === 4, `a 137–138 MHz view: both Meteors and the two ORBCOMM labels (${view.filter(isSatLabel).map((s) => s.label).join(', ')})`);
const measure = (l: string) => estimateLabelWidth(l, 9);
const placed = labelLayout(view, 137.0e6, 1.0e6, 400, measure, 137.9e6);
const pm = placed.filter((p) => /Meteor/.test(p.sig.label));
const overlaps = (a: typeof placed[0], b: typeof placed[0]) => a.row === b.row && a.lx < b.lx + b.w && b.lx < a.lx + a.w;
ok(pm.length === 2 && pm[0].row !== pm[1].row, `the two Meteors 4.7 kHz apart stack on two rows (rows ${pm.map((p) => p.row).join(', ')})`);
ok(placed.every((a, i) => placed.every((b, k) => k <= i || !overlaps(a, b))), 'no two boxes overlap');
const v2m = signalsInView(merged, 144e6, 146e6);
ok(v2m.filter(isSatLabel).map((s) => s.label).join() === 'ISS Voice/SSTV', 'a 2 m view (not SATELLITE mode): the ISS named where it is');

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
