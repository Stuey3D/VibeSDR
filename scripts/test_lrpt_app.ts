// test_lrpt_app.ts — V12 METEOR-M LRPT, the clients' half (src/services/lrpt.ts): lrpt_status parsed defensively, the
// status line at each stage of a pass, the attach parameters, and the key's place (decoderBands: VHF, near 137.1 / 137.9).
// The server side is vibeserver/test-lrpt.cpp (the decoder) and test-lrpt-host.cpp (the host, preview, gallery, reach).
import { lrptAttachParams, lrptStatusText, parseLrptStatus } from '../src/services/lrpt';
import { REACH_DECODERS } from '../src/services/sdrTypes';
import { CLIENT_DECODERS } from '../src/constants/modeBox';
import { GAP_BAND_ROWS, INK, drawBand, lrptFitNow, lrptFits, lrptViewToggle, mapOutline, mapRow, parseLrptGaps, renderSquashed, squashPlan } from '../src/utils/lrptGaps';
import { createHash } from 'node:crypto';

let fail = 0;
const ok = (c: boolean, what: string) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fail++; };

// a real status frame's data, as vibe_lrpt_run.h sends it (from test-lrpt-host)
const real = { on: true, carrier: false, sync: true, mode: 'QPSK', diff: 0, symbolRate: 72000, lock: 63, offsetHz: -1067,
  frames: 47, bad: 1, packets: 603, lines: 112, channels: ['1 visible', '2 near-IR', '5 thermal'] };
const st = parseLrptStatus(real);
ok(!!st && st.frames === 47 && st.lines === 112 && st.channels.length === 3, 'a real status parses');
ok(lrptStatusText(st) === '112 lines · channels 1, 2, 5 · 47 frames', `the line during a pass: ${lrptStatusText(st)}`);
ok(lrptStatusText(parseLrptStatus({ ...real, sync: false })) === '112 lines · channels 1, 2, 5 · 47 frames · between frames, still listening', 'a gap in the frames is said as a gap (an older server: no reading)');
ok(lrptStatusText(parseLrptStatus({ ...real, sync: false, snrDb: 3.24, needDb: 5 })) === '112 lines · channels 1, 2, 5 · 47 frames · between frames, still listening · signal +3.2 dB over the noise (decodes from about +4, best above +5)', 'beta91: the gap with the signal reading');
ok(lrptStatusText(parseLrptStatus({ carrier: true, mode: 'OQPSK', offsetHz: 812 })) === 'carrier found (OQPSK, +812 Hz) · looking for frames', 'carrier found');
ok(lrptStatusText(parseLrptStatus({ sync: true, mode: 'QPSK', frames: 9, bad: 2 })) === 'QPSK locked · frames 9 (2 unreadable)', 'frames before any picture');
ok(lrptStatusText(parseLrptStatus({})) === 'listening for a METEOR satellite', 'nothing yet');
ok(lrptStatusText(parseLrptStatus({ on: true, lock: 73 })) === 'listening for a METEOR satellite', 'beta91: the lock % is never shown (an older server sends only that)');
ok(lrptStatusText(parseLrptStatus({ on: true, lock: 64, snrDb: 0.1, needDb: 5 })) === 'listening for a METEOR satellite · signal +0.1 dB over the noise (decodes from about +4, best above +5)', 'beta91: noise reads about 0 dB, and what decoding needs');
ok(lrptStatusText(parseLrptStatus({ on: true, snrDb: 4.26, needDb: 5, saved: 1 })) === 'last pass saved to the gallery · listening for a METEOR satellite · signal +4.3 dB over the noise (decodes from about +4, best above +5)', 'beta91: after a pass is filed the LIVE state shows, never a stale "picture complete"');
ok(lrptStatusText(parseLrptStatus({ on: true, snrDb: -0.4 })).includes('signal −0.4 dB'), 'a reading under the noise is signed');
{ const j = parseLrptStatus({ snrDb: 'x', needDb: Infinity, saved: -3, inRange: 7 }); ok(!!j && j.snrDb === undefined && j.needDb === undefined && j.saved === 0 && j.inRange === 1, 'beta91 fields bounded'); }
ok(lrptStatusText(null) === 'waiting for a METEOR pass', 'no status at all');
ok(lrptStatusText(parseLrptStatus({ why: 'the IQ slice runs at 50000 S/s — LRPT needs 1.8 to 8 times the symbol rate' })).startsWith('the IQ slice'), 'a reason is shown');
// defensive: junk never throws, never shows NaN, bounds lengths
const junk = parseLrptStatus({ frames: 'x', lines: NaN, channels: [1, 'a'.repeat(99), null, '2 near-IR', 'x', 'y', 'z', 'w'], mode: 'Q'.repeat(50) });
ok(!!junk && junk.frames === 0 && junk.lines === 0 && junk.channels.length <= 6 && junk.channels.every((c) => c.length <= 24) && junk.mode.length <= 8, 'junk is bounded');
ok(parseLrptStatus(null) === null && parseLrptStatus('x') === null, 'not an object: null');
ok(!/NaN|undefined/.test(lrptStatusText(junk)), `no NaN in the line: ${lrptStatusText(junk)}`);
// attach
ok(JSON.stringify(lrptAttachParams()) === '{"modulation":"auto","diff":"auto"}', 'attach: no rate named = AUTO (72k / 80k)');
ok(lrptAttachParams(80000).symbol_rate === 80000 && lrptAttachParams(79000).symbol_rate === 80000 && lrptAttachParams(72000).symbol_rate === 72000, 'attach: a satellite entry names its rate');
ok(lrptAttachParams(0).symbol_rate === undefined, 'attach: 0 = AUTO');
// registration
// ★★ beta95 (Stuart's 11-25 pass: it locked at ~+3.0 and decoded) — the wording, and decoded lines apart from the gap's
ok(lrptStatusText(parseLrptStatus({ on: true, snrDb: 3.04, needDb: 4 })) === 'listening for a METEOR satellite · signal +3.0 dB over the noise (decodes from about +3, best above +4)', 'beta95: decodes from about +3, best above +4');
ok(lrptStatusText(parseLrptStatus({ ...real, lines: 656, decoded: 320, gapLines: 336, channels: ['1 visible', '2 near-IR', '4 infrared'], frames: 40 })) === '320 lines decoded · 336 of no signal · channels 1, 2, 4 · 40 frames', 'beta95: the 656 "lines" of 11-25 said as 320 decoded and 336 of no signal');
ok(lrptStatusText(parseLrptStatus({ ...real, decoded: 112, gapLines: 0 })) === '112 lines · channels 1, 2, 5 · 47 frames', 'beta95: no gap — the plain count');
ok((REACH_DECODERS as readonly string[]).includes('lrpt'), 'greyed with its reason off 137 MHz (REACH_DECODERS)');
ok((CLIENT_DECODERS as readonly string[]).includes('lrpt'), 'a client decoder (CLIENT_DECODERS)');


// ★★★ beta96 THE GAPS SQUASHED in the live preview (Stuart 11-26, Meteor-M2-4: 616 lines of no signal took as much room as
// the picture; near LOS a label per short gap pushed the clean cloud out of view) — src/utils/lrptGaps.ts
{
  const W = 392, T0 = 14 * 3600_000 + 40 * 60_000;   // 14:40:00 satellite time
  // a pass as the server's preview has it: 100 rows of picture, a 616-line gap (154 rows), 98 rows of picture
  const H = 100 + 154 + 98;
  const src = new Uint8Array(W * H * 4);
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const i = (y * W + x) * 4, gap = y >= 100 && y < 254;
    src[i] = gap ? 26 : (y * 7) & 255; src[i + 1] = gap ? 26 : (x * 3) & 255; src[i + 2] = gap ? 36 : 200; src[i + 3] = 255;
  }
  const before = createHash('sha256').update(src).digest('hex');
  const status = { ...real, lines: H * 4, decoded: 792, gapLines: 616, gaps: [[100, 254, T0 + 30_000, T0 + 95_000, 616, 0]] };
  const st2 = parseLrptStatus(status)!;
  ok(st2.gaps.length === 1 && st2.gaps[0].lines === 616 && !st2.gaps[0].patchy, 'beta96: lrpt_status "gaps" parses');
  ok(lrptStatusText(st2).startsWith('792 lines decoded · 616 of no signal'), 'beta96: the status line stays (792 decoded · 616 of no signal)');
  ok(parseLrptStatus(real)!.gaps.length === 0, 'beta96: an older server (no "gaps"): none — drawn row for row as before');
  const plan = squashPlan(st2.gaps, H);
  ok(plan.bands === 1 && plan.h === 100 + GAP_BAND_ROWS + 98, `beta96: the 154-row gap is one ${GAP_BAND_ROWS}-row band (shown ${plan.h} rows, true ${H})`);
  const band = plan.segs.find((g) => g.gap)!;
  ok(band.dstH === GAP_BAND_ROWS && band.label[0] === 'NO SIGNAL 14:40-14:41' && band.label[1] === '616 LINES', `beta96: labelled "${band.label.join(' / ')}"`);
  const out = renderSquashed(src, W, plan);
  ok(out.length === W * plan.h * 4, 'beta96: the shown picture is the squashed height');
  let ink = 0, hatchOk = true;
  for (let y = 100; y < 100 + GAP_BAND_ROWS; y++) for (let x = 0; x < W; x++) {
    const i = (y * W + x) * 4;
    if (out[i] === INK[0] && out[i + 2] === INK[2]) ink++;
    else if (!((out[i] === 54 || out[i] === 26) && out[i + 2] === out[i] + 10)) hatchOk = false;
  }
  ok(ink > 300 && hatchOk, `beta96: the band is hatched with its label in it (${ink} label pixels)`);
  const rowEq = (a: Uint8Array, ay: number, b: Uint8Array, by: number) => Buffer.compare(Buffer.from(a.subarray(ay * W * 4, (ay + 1) * W * 4)), Buffer.from(b.subarray(by * W * 4, (by + 1) * W * 4))) === 0;
  ok(rowEq(src, 0, out, 0) && rowEq(src, 99, out, 99) && rowEq(src, 254, out, 100 + GAP_BAND_ROWS) && rowEq(src, H - 1, out, plan.h - 1), 'beta96: the decoded rows are shown exactly, before and after the band');
  ok(createHash('sha256').update(src).digest('hex') === before, 'beta96: the true-geometry picture (SAVE, what the gallery copy matches) is untouched, byte for byte');
  ok(mapRow(plan, 50) === 50 && mapRow(plan, 254) === 100 + GAP_BAND_ROWS && Math.abs(mapRow(plan, 177) - (100 + GAP_BAND_ROWS / 2)) < 1, 'beta96: rows map through the band');
  // the outline moves with the picture; the original is not changed
  const ol = { w: W, h: H, sat: 'METEOR-M2 4', coast: '', lake: '', border: '', grid: '', labels: [{ x: 10, y: 300, text: '50N' }],
    lines: { coast: [[0, 50, 100, 300]], lake: [], border: [], grid: [] }, centre: null, points: 2 };
  const m = mapOutline(ol, plan);
  ok(m.h === plan.h && m.lines.coast[0][1] === 50 && m.lines.coast[0][3] === 300 - 154 + GAP_BAND_ROWS && m.labels[0].y === 300 - 154 + GAP_BAND_ROWS && m.coast.startsWith('M0 50L100 '), 'beta96: the live outline is mapped through the squash');
  ok(ol.h === H && ol.lines.coast[0][3] === 300 && ol.labels[0].y === 300, 'beta96: …and the outline it was given (true geometry) is unchanged');
  // several gaps: a band each, their own labels; the same minute twice → seconds, never a repeated label
  const many = parseLrptGaps([[20, 60, T0, T0 + 40_000, 160, 0], [100, 140, T0 + 50_000, T0 + 58_000, 160, 0], [200, 210, T0 + 80_000, T0 + 85_000, 40, 0]]);
  const p2 = squashPlan(many, 300);
  const labels = p2.segs.filter((g) => g.gap).map((g) => g.label.join(' / '));
  ok(p2.bands === 3 && p2.h === 300 - 40 - 40 - 10 + GAP_BAND_ROWS * 2 + 10, `beta96: three gaps, three bands (a 10-row gap keeps its own height): ${p2.h} rows`);
  ok(labels[0] === 'NO SIGNAL 14:40 / 160 LINES' && labels[1] === 'NO SIGNAL 14:40:50-14:40:58 / 160 LINES' && labels[2] === 'NO SIGNAL 14:41 (40 LINES)', `beta96: their labels, none repeated: ${labels.join(' | ')}`);
  ok(new Set(p2.segs.filter((g) => g.gap).map((g) => g.label[0])).size === 3, 'beta96: no label repeats');
  // a fading tail: the server merges it (gapart::mergeGaps, test-lrpt) and sends one patchy gap → one band, one label
  const tail = squashPlan(parseLrptGaps([[300, 420, T0 + 120_000, T0 + 190_000, 288, 1]]), 430);
  const tb = tail.segs.filter((g) => g.gap);
  ok(tb.length === 1 && tb[0].label[0] === 'NO SIGNAL 14:42-14:43' && tb[0].label[1] === 'PATCHY, 288 LINES', `beta96: a fading tail is ONE band: "${tb[0]?.label.join(' / ')}"`);
  // FIT keeps the early clean part in view: a 15-minute pass squashed fits a phone's SATELLITE box whole
  const pass = parseLrptGaps([[200, 900, T0 + 300_000, T0 + 750_000, 2800, 0], [1100, 1500, T0 + 870_000, T0 + 990_000, 1600, 1]]);
  const pp = squashPlan(pass, 1560);
  ok(pp.h === 1560 - 700 - 400 + GAP_BAND_ROWS * 2, `beta96: a 1560-row pass with 1100 rows of gap shows as ${pp.h} rows`);
  ok(lrptFits(W, pp.h, 360, 440) && lrptFitNow('auto', W, pp.h, 360, 440), 'beta96: on a phone (360 × 440) it fits whole → the clean rows 0–199 stay in view');
  ok(!lrptFits(W, 1560, 360, 440), 'beta96: unsquashed it would not (the old behaviour: follow, the early picture scrolled away)');
  ok(!lrptFitNow('auto', W, 1600, 700, 440) && lrptFitNow('fit', W, 1600, 700, 440) && !lrptFitNow('follow', W, 100, 700, 440), 'beta96: a sliver follows; a tap overrides either way');
  ok(lrptViewToggle(true) === 'follow' && lrptViewToggle(false) === 'fit', 'beta96: a tap flips fit / follow');
  // ★★★ beta96 NORTH UP: a northbound pass ("rot":180) shown turned round; a southbound one as received
  ok(parseLrptStatus({ ...status, rot: 180 })!.rot === 180 && parseLrptStatus(status)!.rot === 0 && parseLrptStatus({ rot: 90 })!.rot === 0, 'beta96: "rot" parses (180 only; else 0)');
  const turned = renderSquashed(src, W, plan, true);
  const px = (a: Uint8Array, x: number, y: number) => Array.from(a.subarray((y * W + x) * 4, (y * W + x) * 4 + 4)).join(',');
  ok(px(turned, 0, 0) === px(src, W - 1, H - 1) && px(turned, W - 1, plan.h - 1) === px(src, 0, 0), 'beta96: turned — the top-left pixel is the newest row\'s last (the top row = the northernmost on a northbound pass)');
  ok(px(turned, 10, plan.h - 1 - 50) === px(src, W - 1 - 10, 50), 'beta96: every picture row turned (row 50 → its turned place, mirrored across)');
  // the band: at its turned place, its label UPRIGHT (the very pixels of the unturned band — never turned text)
  const bandAlone = new Uint8Array(W * GAP_BAND_ROWS * 4); drawBand(bandAlone, W, 0, GAP_BAND_ROWS, band.label);
  const tTop = plan.h - 100 - GAP_BAND_ROWS;
  ok(Buffer.compare(Buffer.from(turned.subarray(tTop * W * 4, (tTop + GAP_BAND_ROWS) * W * 4)), Buffer.from(bandAlone)) === 0, 'beta96: turned, the band sits at its turned place with its label upright (byte for byte the unturned band)');
  ok(Buffer.compare(Buffer.from(renderSquashed(src, W, plan, false)), Buffer.from(out)) === 0, 'beta96: southbound / not known: as received (unchanged)');
  ok(createHash('sha256').update(src).digest('hex') === before, 'beta96: turning the shown picture leaves the received one untouched');
  // the outline turned with it: points turned, label text unchanged at a turned anchor inside the picture
  const mt = mapOutline(ol, plan, true);
  ok(mt.lines.coast[0][0] === W - 0 && mt.lines.coast[0][1] === plan.h - 50 && mt.lines.coast[0][2] === W - 100 && mt.lines.coast[0][3] === plan.h - (300 - 154 + GAP_BAND_ROWS), 'beta96: the outline\'s points turned with the picture');
  ok(mt.labels[0].text === '50N' && mt.labels[0].x >= 0 && mt.labels[0].x <= W - 3 * 7 && mt.labels[0].y === plan.h - (300 - 154 + GAP_BAND_ROWS), `beta96: its label text unturned, at its turned anchor inside the picture (${mt.labels[0].x}, ${mt.labels[0].y})`);
  ok(mapOutline(ol, squashPlan([], H), false) === ol, 'beta96: no gaps and not turned: the outline as sent');
  // junk
  ok(parseLrptGaps([[5, 2, 0, 0, 1, 0], ['x'], null, [0, 10, -5, NaN, 4, 0], [8, 20, 1, 2, 3, 0]]).length === 1, 'beta96: junk gaps dropped, overlaps dropped');
}

console.log(fail ? `\n${fail} FAILED` : '\nall passed');
process.exit(fail ? 1 : 0);
