// test_map_wf.ts — ★★★ V12 THE MAP DRAWER'S LIVE WATERFALL: what it shows (src/utils/mapWfModel.ts). The span fits the
// mode; one labelled marker per decoder channel from the server's table; a scan's window is fresh at each step (the key
// changes); a DECODED ident is stamped at its frequency when it is read (again on a later reading), never before the
// view opened, never a listed one.   npx tsx scripts/test_map_wf.ts
import { readFileSync } from 'node:fs';
import { mwfCrop, mwfMinSpan, MWF_MIN_BINS } from '../src/utils/mapChrome';
import { mwfStoreNew, pushRow, mwfPaint, mwfClampToCapture, LUT_STRIDE, LEVEL_IDX } from '../src/utils/mapWfPaint';
import { getColorLUT } from '../src/assets/colormapUtils';
import { ismSanitise } from '../src/services/ism';
import { avBandFor, wfModelIsm, wfModelVor, wfModelNdb, wfModelChannels, wfModelVfo, WF_SPAN_MIN, WF_SPAN_MAX, type WfSeen } from '../src/utils/mapWfModel';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); };

// ── a single channel: a slice a few times the filter, centred on the dial ──
{
  const m = wfModelVfo({ frequency: 145.5e6, mode: 'nfm', bandwidthLow: -6250, bandwidthHigh: 6250 });
  ok(m.hi - m.lo === 62500 && (m.lo + m.hi) / 2 === 145.5e6, `NFM 12.5 kHz → a 62.5 kHz slice centred on the dial (${m.hi - m.lo})`);
  ok(m.marks.length === 1 && m.marks[0].label === 'NFM 145.500' && m.marks[0].loHz === 145.5e6 - 6250, `one marker with its passband (${m.marks[0].label})`);
  const usb = wfModelVfo({ frequency: 7.1e6, mode: 'usb', bandwidthLow: 100, bandwidthHigh: 2800 });
  ok(usb.hi - usb.lo === 13500, `USB 2.7 kHz → 13.5 kHz (${usb.hi - usb.lo})`);
  const cw = wfModelVfo({ frequency: 7.0e6, mode: 'cw', bandwidthLow: -250, bandwidthHigh: 250 });
  ok(cw.hi - cw.lo === WF_SPAN_MIN, `a narrow CW filter → the floor, ${WF_SPAN_MIN / 1e3} kHz`);
  const wfm = wfModelVfo({ frequency: 96.1e6, mode: 'wfm', bandwidthLow: -100e3, bandwidthHigh: 100e3 });
  ok(wfm.hi - wfm.lo === 1e6 && wfm.hi - wfm.lo <= WF_SPAN_MAX, `WFM 200 kHz → 1 MHz, under the ceiling (${wfm.hi - wfm.lo})`);
}
// ── a multichannel decoder: every channel the server lists, a margin, one labelled marker each ──
{
  const vdl = [136.7e6, 136.725e6, 136.75e6, 136.775e6, 136.8e6, 136.825e6, 136.85e6, 136.875e6, 136.9e6, 136.925e6, 136.95e6, 136.975e6];
  const m = wfModelChannels({ id: 'vdl2', name: 'VDL2', chans: vdl })!;
  ok(m.marks.length === 12 && m.marks[0].label === 'Ch1 136.700' && m.marks[11].label === 'Ch12 136.975', `VDL2: 12 markers, Ch1…Ch12 (${m.marks[0].label} … ${m.marks[11].label})`);
  ok(m.lo < 136.7e6 && m.hi > 136.975e6 && m.lo >= 136.7e6 - 30e3, `the span covers every channel plus a margin (${m.lo}–${m.hi})`);
  const both = wfModelChannels({ id: 'both', name: 'ACARS+VDL2', chans: [131.525e6, 131.725e6, 136.975e6] })!;
  ok(both.marks.map((x) => x.label).join() === 'ACARS Ch1 131.525,ACARS Ch2 131.725,VDL2 Ch1 136.975', `the combined band names each channel by its kind (${both.marks.map((x) => x.label).join(' · ')})`);
  ok(wfModelChannels({ id: 'vdl2', name: 'VDL2' }) === null, 'an older server (no channel table) → null: the dial slice instead, nothing invented');
}
// ── the VOR / ILS scan: the window, fresh at each step; decoded idents stamped as read ──
{
  const seen: WfSeen = new Map();
  const wins: Array<[number, number]> = [[108e6, 110e6], [110e6, 112e6], [116e6, 118e6]];
  const base = { now: 100, windows: 3, wins, wlo: 0, whi: 0, dwell: 30 };
  const old = { hz: 116.4e6, ident: 'DTY', reads: 1, votes: 3, last: 60, fresh: true };
  // primed as it opens: an ident read BEFORE is not stamped as if now
  const p0 = wfModelVor({ ...base, window: 2, beacons: [old] }, seen, true);
  ok(p0.stamps.length === 0, 'opening: idents read before are not stamped');
  const m1 = wfModelVor({ ...base, window: 2, beacons: [old] }, seen, false);
  ok(m1.stamps.length === 0 && m1.lo === 116e6 && m1.hi === 118e6, `the window is the span (${m1.lo}–${m1.hi}); no new reading → no stamp`);
  ok(m1.marks[0].label === 'DTY 116.40', `the station's marker: "${m1.marks[0].label}"`);
  const m2 = wfModelVor({ ...base, now: 130, window: 2, beacons: [{ ...old, reads: 2, last: 128 }] }, seen, false);
  ok(m2.stamps.length === 1 && m2.stamps[0].hz === 116.4e6 && m2.stamps[0].text === 'DTY', `a new reading → "DTY" stamped at 116.400 (${JSON.stringify(m2.stamps)})`);
  const m3 = wfModelVor({ ...base, now: 131, window: 2, beacons: [{ ...old, reads: 2, last: 128 }] }, seen, false);
  ok(m3.stamps.length === 0, 'the same reading again → not stamped twice');
  const m4 = wfModelVor({ ...base, now: 190, window: 0, beacons: [{ ...old, reads: 2, last: 128 }, { hz: 109.5e6, ident: '', reads: 0, votes: 0, last: 185, fresh: true }] }, seen, false);
  ok(m4.key !== m3.key && m4.lo === 108e6 && m4.hi === 110e6, `the next window → a new key (the history breaks) and its own span (${m4.key})`);
  ok(m4.marks.length === 1 && m4.marks[0].hz === 109.5e6 && m4.marks[0].label === '', 'only the stations inside the window; an unidentified one is an unlabelled line');
  const m5 = wfModelVor({ ...base, now: 400, window: 2, beacons: [{ ...old, reads: 3, last: 395 }] }, seen, false);
  ok(m5.stamps.length === 1 && m5.stamps[0].hz === 116.4e6, 'back round the sweep, read again → stamped again at the same frequency');
}
// ── NDB: decoded idents only ──
{
  const seen: WfSeen = new Map();
  const r = { centre: 400e3, lo: 255e3, hi: 526e3, identified: 1, beacons: [{ id: 1, hz: 338e3, ident: '', active: true, lastId: 0 }] };
  wfModelNdb(r, seen, true);
  const a = wfModelNdb({ ...r, beacons: [{ id: 1, hz: 338e3, ident: 'BIA', active: true, lastId: 50 }] }, seen, false);
  ok(a.stamps.length === 1 && a.stamps[0].text === 'BIA' && a.marks[0].label === 'BIA 338.0', `NDB: decoded "BIA" stamped at 338 kHz, marker "${a.marks[0].label}"`);
  const b = wfModelNdb({ ...r, beacons: [{ id: 2, hz: 400e3, ident: '', active: true, lastId: 0 }] }, seen, false);
  ok(b.stamps.length === 0 && b.marks[0].label === '', 'an undecoded beacon: no stamp, no name (the list is never the judge)');
}
// ── ★★★ beta97 build 587 (Stuart, the Lenovo in VDL2: "USB 136.863 MHz · 13 kHz shown", one marker, noise bands) ──
{
  // the VDL2 band as a server sends it (its channel list), and as an older one does (first / last channel and the count)
  const vdl2 = { id: 'vdl2-eu', name: 'VDL2', region: 'EU', hz: 136.85e6, channels: 4, chans: [136.725e6, 136.775e6, 136.875e6, 136.975e6] };
  const old = { id: 'vdl2-eu', name: 'VDL2', region: 'EU', hz: 136.85e6, channels: 12, chLo: 136.7e6, chHi: 136.975e6 };
  const m1 = wfModelChannels(vdl2)!;
  ok(m1.marks.length === 4 && m1.marks[0].label === 'Ch1 136.725' && m1.lo < 136.725e6 && m1.hi > 136.975e6, `VDL2: one labelled marker per channel, the span the channels (${m1.info})`);
  const m2 = wfModelChannels(old)!;
  ok(!!m2 && m2.marks.length === 12 && m2.marks[11].hz === 136.975e6, 'an older band (no channel list): the even grid from first / last and the count');
  ok(avBandFor([vdl2], { band: 'vdl2', bandName: 'VDL2', hz: 136.875e6 }) === vdl2, 'the report\'s band found by name when the ids differ');
  ok(avBandFor([old], { band: 'x', hz: 136.85e6 }) === old, '…else by the frequency its channels hold');
  ok(avBandFor([vdl2], null) === undefined, 'no report → none (the VFO slice then)');
  const sdr = readFileSync(new URL('../src/screens/SDRScreen.tsx', import.meta.url), 'utf8');
  ok(/const band = avOn && avReport \? avBandFor\(avCap\?\.bands, avReport\) : undefined;/.test(sdr), 'the app looks the band up the forgiving way');
  // the crop: a 13 kHz view on a capture whose bins are ~3 kHz wide — interpolated, never four hard blocks
  const n = 2048, span = 6e6, bins = new Float32Array(n);
  for (let i = 0; i < n; i++) bins[i] = -100 + (i % 7);
  const centre = 136.85e6, hzPer = span / n;
  const row = mwfCrop(bins, centre, span, centre - 6500, centre + 6500, 200);
  let runs = 0; for (let i = 1; i < row.length; i++) if (row[i] !== row[i - 1]) runs++;
  ok(runs > 150 && row.every((v) => Number.isFinite(v)), `a column finer than a bin: interpolated (${runs} changes over 200 columns, not ~4 blocks)`);
  const one = mwfCrop(bins, centre, span, centre - 1, centre + 1, 1)[0];
  const x = (centre - (centre - span / 2)) / hzPer - 0.5, i0 = Math.floor(x);
  ok(one >= Math.min(bins[i0], bins[i0 + 1]) - 1e-3 && one <= Math.max(bins[i0], bins[i0 + 1]) + 1e-3, 'the value lies between its two bins');
  const wide = mwfCrop(bins, centre, span, centre - 1e6, centre + 1e6, 100);
  ok(wide.every((v) => v === -94 || v >= -100), 'a column wider than a bin keeps the strongest bin (unchanged)');
}

console.log('── ★★★ beta97 the swap needs a beta97 server ("specPause":1) — Stuart: none on an older one ──');
{
  const types = readFileSync(new URL('../src/services/sdrTypes.ts', import.meta.url), 'utf8');
  const sdr = readFileSync(new URL('../src/screens/SDRScreen.tsx', import.meta.url), 'utf8');
  ok(/specPause: j\.specPause === 1 \|\| j\.specPause === true,/.test(types), 'vibeserver.json\'s "specPause":1 read');
  ok(/setSpecPauseOk\(o\?\.specPause === true\);/.test(sdr) && /setSpecPauseOk\(false\);/.test(sdr), 'per radio, cleared first (an older server: false)');
  ok(/const wfOffered = !asViewer && isVibeServer && specPauseOk && /.test(sdr) && /wf: wfOffered \? \{ offered: true/.test(sdr),
     'no flag → wf null → no ≋ on the LIST chip and no folder tab (ListDrawer wfOk = p.wf?.offered); with it → offered');
  ok(/if \(!isVibeServer \|\| !specPauseOk\) return;   \/\/ ★★★ beta97 only a server that says it honours it/.test(sdr), 'spec on / off is sent only to a server that honours it');
}

console.log('── ★★★ beta97 hotfix the minimum span: never fewer than 16 of the frame\'s bins (the web\'s rule too) ──');
{
  const bin = 6e6 / 2048;   // a wide capture's bins (~2.9 kHz)
  const v = mwfMinSpan(869.5e6 - 6500, 869.5e6 + 6500, bin);
  ok(MWF_MIN_BINS === 16 && Math.abs(v.hi - v.lo - 16 * bin) < 1e-6 && Math.abs((v.lo + v.hi) / 2 - 869.5e6) < 1e-6, `a 13 kHz view on ${(bin / 1e3).toFixed(1)} kHz bins widens to 16 bins (${((v.hi - v.lo) / 1e3).toFixed(1)} kHz), about its centre`);
  const w = mwfMinSpan(136.6e6, 137.1e6, bin);
  ok(w.lo === 136.6e6 && w.hi === 137.1e6, 'a view already wider than 16 bins is untouched (VDL2\'s channels)');
  ok(mwfMinSpan(1, 2, 0).lo === 1, 'no bin width known: untouched');
  const mw = readFileSync(new URL('../src/components/MapMiniWaterfall.tsx', import.meta.url), 'utf8');
  ok(/const v = mwfClampToCapture\(mwfMinSpan\(m\.lo, m\.hi, f\.bins\.length \? f\.spanHz \/ f\.bins\.length : 0\), f\.centreHz, f\.spanHz\);/.test(mw) && /mwfCrop\(f\.bins, f\.centreHz, f\.spanHz, v\.lo, v\.hi, z\.cols\)/.test(mw)
     && /\{ \.\.\.model, lo: s\.vlo, hi: s\.vhi \}/.test(mw), 'the drawer crops to the widened span, and its markers and stamps are placed on it');
}

console.log('── ★★★ beta97 hotfix ISM / LoRa: the decoder\'s channels (lora.chs), else the capture — the web\'s rule, its module ──');
{
  // a type:"ism" report as the hotfix server sends it (868 band, three running channels)
  const raw = { type: 'ism', band: '868', bandName: '868 MHz', centre: 868.6e6, rate: 2.4e6, devices: [],
    lora: { chans: 3, plan: 8, dets: 0, frames: 0, badCrc: 0, texts: 0, mapped: 0, dropped: 0, found: 3, foundKnown: 3,
            chs: [{ hz: 869618000, bw: 62500, net: 'meshcore' }, { hz: 869525000, bw: 250000, net: 'meshtastic' }, { hz: 868100000, bw: 125000, net: 'lorawan' }, { hz: 'x' }, { hz: -5 }] } };
  const rep = ismSanitise(raw)!;
  ok(rep.lora?.chs?.length === 3 && rep.lora.chs[0].net === 'meshcore', `lora.chs read and checked (bad entries dropped: ${rep.lora?.chs?.length})`);
  const m = wfModelIsm(rep)!;
  ok(m.marks.length === 3 && m.marks.some((k) => k.label === 'MeshCore 869.618') && m.marks.some((k) => k.label === 'Meshtastic 869.525') && m.marks.some((k) => k.label === 'LoRaWAN 868.100'),
     `one labelled marker per running channel (${m.marks.map((k) => k.label).join(' · ')})`);
  ok(m.lo < 868.1e6 - 62.5e3 && m.hi > 869.525e6 + 125e3, `the span holds the whole channel set and a margin (${m.info})`);
  const old = ismSanitise({ ...raw, lora: { ...raw.lora, chs: undefined } })!;
  const mo = wfModelIsm(old)!;
  ok(mo.marks.length === 0 && mo.lo === 868.6e6 - 1.2e6 && mo.hi === 868.6e6 + 1.2e6, 'an older server (no list): the whole capture, centre ± rate / 2');
  const v = wfModelVfo({ frequency: 868.6e6, mode: 'usb', bandwidthLow: 0, bandwidthHigh: 2600 }, 16 * 2.4e6 / 2048);
  ok(v.hi - v.lo >= 16 * 2.4e6 / 2048 - 1, 'the dial slice is never narrower than 16 of the radio\'s bins');
  const sdr = readFileSync(new URL('../src/screens/SDRScreen.tsx', import.meta.url), 'utf8');
  const i1 = sdr.indexOf('?? (ismOn && ismReport ? wfModelIsm(ismReport) : null)'), i2 = sdr.indexOf("?? (adsbLiveHere ? { lo: 1089e6, hi: 1091e6, key: 'adsb'"), i3 = sdr.indexOf('status.binBandwidth > 0 ? status.binBandwidth * 16 : 0');
  ok(i1 > 0 && i2 > i1 && i3 > i2, 'the app picks in the web\'s order: VOR · NDB · datalink · ISM · ADS-B (1090 ± 1 MHz) · the dial slice (16-bin floor)');
  const shared = readFileSync(new URL('../src/utils/mapWfModel.ts', import.meta.url), 'utf8');
  ok(/export function wfModelIsm\(/.test(shared) && /export function wfModelVfo\(v: \{ frequency: number; mode: string; bandwidthLow: number; bandwidthHigh: number \}, minSpan = 0\)/.test(shared), 'one shared module (src/utils/mapWfModel.ts) for the app and the web');
}

console.log('── ★★★ beta97 hotfix REAL FRAMES (Stuart, TestFlight 589: rainbow speckle) — captured through the app\'s own client ──');
{
  const fx = JSON.parse(readFileSync(new URL('./fixtures/map_wf_frames.json', import.meta.url), 'utf8')) as { frames: Array<{ raw: string; bins: number[]; centreHz: number; spanHz: number; freq: number }> };
  ok(fx.frames.length >= 4, `${fx.frames.length} real frames (a beta97 server's binary8 spectrum, received by VibeServerClient)`);
  // ★ the documented binary8 rule, from the raw bytes: magic, version 2 (a 30-byte header with its Hz-per-bin), flags 3 = a
  //   full u8 frame, dBFS = byte − 256, then the FFT's halves swapped (negative frequencies first)
  let same = true, inRange = true;
  for (const f of fx.frames) {
    const raw = Buffer.from(f.raw, 'base64');
    const ver = raw[4], flags = raw[5], hdr = ver === 2 ? 30 : 22, n = raw.length - hdr, half = n >> 1;
    if (flags !== 3 || n !== f.bins.length) { same = false; continue; }
    for (let i = 0; i < n; i++) {
      const wrapped = i < half ? half + i : i - half;   // display index → wrapped (server) index
      const db = raw[hdr + wrapped] - 256;
      if (Math.abs(db - f.bins[i]) > 1e-6) same = false;
      if (!(db >= -160 && db <= 0)) inRange = false;
    }
  }
  ok(same, 'the bins the app hands its waterfall sinks == the raw frame decoded by the binary8 rule (dBFS = u8 − 256, halves unwrapped)');
  ok(inRange, 'every value is a dBFS level (−160 … 0) — never raw bytes, never another message read as bins');
  const sdr = readFileSync(new URL('../src/screens/SDRScreen.tsx', import.meta.url), 'utf8');
  ok(/wfFrameSink\.current\?\.\(newBins, s\);/.test(sdr) && /if \(mapWfSink\.current\) mapWfSink\.current\(\{ bins: newBins, centreHz: s\.trueCenterHz \?\? s\.centerHz, spanHz: s\.trueBwHz \?\? s\.bwHz \}\);/.test(sdr),
     'the drawer and the main waterfall get the SAME decoded array from the same onSpectrum call (one decode, never a second parser)');
  // ★ the drawer's own path on them: crop → level → paint, with the user's palette
  const st = mwfStoreNew(); st.cols = 240; st.rows = 400; st.ring = new Uint8Array(st.cols * st.rows);
  for (const f of fx.frames) pushRow(st, mwfCrop(Float32Array.from(f.bins), f.centreHz, f.spanHz, f.centreHz - 600e3, f.centreHz + 600e3, st.cols));
  const idx = Array.from(st.ring.subarray(0, st.cols * fx.frames.length));
  const lo = Math.min(...idx), hi = Math.max(...idx);
  ok(lo >= 1 && hi <= 254 && hi - lo <= 60, `the fake radio's flat noise (a few dB) stays a narrow band of the palette round the floor (indices ${lo}–${hi}, floor ${LEVEL_IDX})`);
  for (const name of ['gqrx', 'turbo', 'viridis']) {
    const lut = getColorLUT(name);
    ok(lut.length === 256 * LUT_STRIDE, `${name}: the palette is RGBA (${lut.length / 256} bytes an entry) — the drawer reads it ${LUT_STRIDE} at a time`);
    const px = mwfPaint(st, 40, lut);
    let exact = true;
    for (let y = 0; y < fx.frames.length; y++) for (let c = 0; c < st.cols; c++) {
      const v = st.ring[((st.seq - 1 - y) % st.rows) * st.cols + c], p = (y * st.cols + c) * 4;
      if (px[p] !== lut[v * 4] || px[p + 1] !== lut[v * 4 + 1] || px[p + 2] !== lut[v * 4 + 2]) exact = false;
    }
    ok(exact, `${name}: every pixel is EXACTLY its level's palette entry (the old 3-byte read straddled two entries: rainbow speckle)`);
    const tail = px.subarray(fx.frames.length * st.cols * 4, fx.frames.length * st.cols * 4 + 4);
    ok(tail[0] === 0 && tail[1] === 0 && tail[2] === 0 && tail[3] === 255, `${name}: below the history so far BLACK until it draws in (Stuart, TF 590)`);
  }
}

console.log('── ★★★ beta97 hotfix the left-edge strip (Stuart, TF 590): the drawer\'s span stays INSIDE the capture ──');
{
  const fx = JSON.parse(readFileSync(new URL('./fixtures/map_wf_frames.json', import.meta.url), 'utf8')) as { frames: Array<{ bins: number[]; centreHz: number; spanHz: number }> };
  const f = fx.frames[0], f0 = f.centreHz - f.spanHz / 2, f1 = f.centreHz + f.spanHz / 2;
  // a span that starts below the frame's lowest bin (a channel set near the capture's edge, plus its margin)
  const want = { lo: f0 - 120e3, hi: f0 + 1.0e6 };
  const before = mwfCrop(Float32Array.from(f.bins), f.centreHz, f.spanHz, want.lo, want.hi, 715);
  const blank = before.findIndex((v) => Number.isFinite(v));
  ok(blank > 40, `the old way: the first ${blank} of 715 columns had no data (the black strip down the left)`);
  const v = mwfClampToCapture(want, f.centreHz, f.spanHz);
  ok(v.lo === f0 && Math.abs(v.hi - v.lo - (want.hi - want.lo)) < 1e-6, 'slid inside the frame, its width kept');
  const after = mwfCrop(Float32Array.from(f.bins), f.centreHz, f.spanHz, v.lo, v.hi, 715);
  ok(after.every((x) => Number.isFinite(x)), 'every column now has data — no strip at either edge');
  ok(mwfClampToCapture({ lo: f1 - 1e5, hi: f1 + 3e5 }, f.centreHz, f.spanHz).hi === f1, 'the high edge the same way');
  const wide = mwfClampToCapture({ lo: f0 - 1e6, hi: f1 + 1e6 }, f.centreHz, f.spanHz);
  ok(wide.lo === f0 && wide.hi === f1, 'wider than the capture: exactly the capture');
  ok(mwfClampToCapture({ lo: f0 + 1e5, hi: f0 + 2e5 }, f.centreHz, f.spanHz).lo === f0 + 1e5, 'inside already: untouched');
  const mw = readFileSync(new URL('../src/components/MapMiniWaterfall.tsx', import.meta.url), 'utf8');
  ok(/const v = mwfClampToCapture\(mwfMinSpan\(m\.lo, m\.hi, f\.bins\.length \? f\.spanHz \/ f\.bins\.length : 0\), f\.centreHz, f\.spanHz\);/.test(mw),
     'the drawer crops to the clamped span (the markers are placed on it — s.vlo / s.vhi)');
  ok(!/Spectrum|specH/.test(mw), 'waterfall only — no spectrum in the drawer (Stuart: it would run too slowly at the map\'s rate)');
}

console.log(`test_map_wf: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
