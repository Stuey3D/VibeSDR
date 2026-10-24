/**
 * The ADS-B map's live list (src/services/adsbList.ts): row text, nearest-first order that does not shuffle, and the
 * geometry that keeps it in the bottom corner — clear of the zoom stack, never more than half the map.
 *
 * Run: node --no-warnings scripts/test_adsb_list.ts   (run-tests.sh does)
 */
import { adsbListRows, adsbListRowsFit, adsbListWidth, altText, rangeText, parseListOpen,
         MAP_TOPBAR_H, LIST_HEAD_H, LIST_ROW_H } from '../src/services/adsbList.ts';
import { adsbToAircraft } from '../src/services/adsbTypes.ts';

let fails = 0, passes = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  if (JSON.stringify(got) === JSON.stringify(want)) { passes++; return; }
  fails++; console.error(`FAIL ${what}\n   got  ${JSON.stringify(got)}\n   want ${JSON.stringify(want)}`);
};

// ── text ────────────────────────────────────────────────────────────────────────────────────────
eq('feet, always', altText({ icao: 'a', altitude: 35_000 }), '35,000 ft');
eq('feet below', altText({ icao: 'a', altitude: 4_321 }), '4,300 ft');
eq('ground', altText({ icao: 'a', altitude: 0, onGround: true }), 'GND');
eq('unknown altitude', altText({ icao: 'a' }), '');
eq('NM + compass', rangeText({ icao: 'a', distKm: 22.224, bearing: 44 }), '12 NM NE');
eq('under 10 NM: one decimal', rangeText({ icao: 'a', distKm: 9.26, bearing: 359 }), '5.0 NM N');
eq('no bearing', rangeText({ icao: 'a', distKm: 185.2 }), '100 NM');
eq('no position', rangeText({ icao: 'a' }), '');

// ── order ───────────────────────────────────────────────────────────────────────────────────────
const rx = { lat: 51.5, lon: -0.1 };
const list = adsbToAircraft([
  { hex: '400002', flight: 'FAR', lat: 52.5, lon: -0.1, alt_baro: 36000, gs: 451 },
  { hex: '400001', flight: 'NEAR', lat: 51.6, lon: -0.1, alt_baro: 3000, gs: 180 },
  { hex: '400003', rssi: -10 },                       // no position, strong
  { hex: '400004', rssi: -30 },                       // no position, weak
  { hex: '400005', flight: 'EMG', lat: 51.55, lon: -0.1, alt_baro: 'ground', emergency: 'general' },
], rx);
const rows = adsbListRows(list);
eq('nearest first, unplaced by signal last', rows.map((r) => r.label), ['EMG', 'NEAR', 'FAR', '400003', '400004']);
eq('row text', rows[2], { hex: '400002', label: 'FAR', alt: '36,000 ft', kt: '451 kt', range: '60 NM N', placed: true, emergency: false, badge: '', military: false, bars: 0, trend: '●', altCol: 'hsl(265,90%,62%)' });
// ★ Signal bars (adsbBars): 3 from -12 dBFS, 2 from -22, else 1; none without a level.
{ const { adsbBars } = await import('../src/services/adsbList.ts');
  eq('bars', [adsbBars(-3), adsbBars(-12), adsbBars(-15), adsbBars(-22), adsbBars(-30), adsbBars(undefined)], [3, 3, 2, 2, 1, 0]); }
eq('ground + emergency', [rows[0].alt, rows[0].emergency], ['GND', true]);
eq('unplaced cannot be focused', rows[3].placed, false);
eq('hex label is capitals', adsbListRows([{ icao: 'abc123' }])[0].label, 'ABC123');
// ★ Two equal rows must keep their order whichever way they arrive, or the list shuffles under the finger.
const tie = [{ icao: 'bbbbbb', distKm: 10 }, { icao: 'aaaaaa', distKm: 10 }];
eq('ties are stable by address', adsbListRows(tie).map((r) => r.hex), adsbListRows([...tie].reverse()).map((r) => r.hex));
eq('input not mutated', tie.map((t) => t.icao), ['bbbbbb', 'aaaaaa']);

// ── geometry ────────────────────────────────────────────────────────────────────────────────────
eq('a tall phone: 8 rows', adsbListRowsFit(800), 8);
eq('a phone held sideways: 4', adsbListRowsFit(320), 4);
eq('a short window: still 3', adsbListRowsFit(200), 3);
for (const h of [500, 640, 700]) {
  const n = adsbListRowsFit(h);
  const listH = LIST_HEAD_H + n * LIST_ROW_H;
  eq(`never more than half the map at ${h}`, listH <= (h - MAP_TOPBAR_H) / 2 || n === 3, true);
}
eq('SE in Display Zoom (320): clear of the zoom stack', adsbListWidth(320), 240);
eq('320 leaves the right 60 + margins', 320 - 10 - adsbListWidth(320) >= 60 + 10, true);
eq('wide: capped at 300', adsbListWidth(1366), 300);
eq('never below 160', adsbListWidth(100), 160);

// ── collapse state ──────────────────────────────────────────────────────────────────────────────
eq('open unless shut', [parseListOpen(null), parseListOpen('1'), parseListOpen('0'), parseListOpen('x')], [true, true, false, true]);

// ★ The order (Stuart, 2026-10-24): emergencies first, then military, then the nearest.
{
  const { adsbListRows: rowsOf } = await import('../src/services/adsbList.ts');
  const order = rowsOf([
    { icao: '400001', flight: 'NEAR1', distKm: 2, lat: 52, lon: -1 },
    { icao: '43c001', flight: 'RRR123', distKm: 90, lat: 52, lon: -1 },
    { icao: '400002', flight: 'SQK77', distKm: 150, lat: 52, lon: -1, squawk: '7700' },
    { icao: '400003', flight: 'MID', distKm: 40, lat: 52, lon: -1 },
  ] as never).map((r: { label: string }) => r.label);
  eq('emergency, then military, then the nearest', order, ['SQK77', 'RRR123', 'NEAR1', 'MID']);
  const em = rowsOf([{ icao: '400002', flight: 'X', squawk: '7600' }] as never)[0];
  eq('a radio-failure squawk is badged as one', [em.badge, em.emergency], ['7600 RADIO FAIL', true]);
}
console.log(fails ? `\n${fails} FAILED, ${passes} passed` : `adsb list: all ${passes} passed`);
process.exit(fails ? 1 : 0);
