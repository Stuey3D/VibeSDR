// test_spot_time.ts — every spot's time is HH:MM:SS UTC, the START of the slot it was heard in (src/utils/spotTime.ts,
// V12 2026-10-26, M7EIW via Stuart: "Showing the seconds lets me know … which transmission block a decode was received
// like in WSJTX"). The web client, the app's lists, its map card and the web map's popups all format with it.
import { fmtSpotTime, FMT_SPOT_TIME_JS } from '../src/utils/spotTime';

let fail = 0;
const ok = (c: boolean, what: string) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fail++; };
const at = (h: number, m: number, s: number) => Date.UTC(2026, 9, 26, h, m, 0) + s * 1000;

ok(fmtSpotTime(at(16, 46, 30)) === '16:46:30', `an FT8 spot stamped with its slot (16:46:30) → "${fmtSpotTime(at(16, 46, 30))}"`);
ok(fmtSpotTime(at(16, 46, 37.5)) === '16:46:37', `an FT4 slot at :37.5 → "16:46:37" (the second it began in): "${fmtSpotTime(at(16, 46, 37.5))}"`);
ok(fmtSpotTime(at(16, 46, 33.75)) === '16:46:33', `an FT2 slot at :33.75 → "16:46:33": "${fmtSpotTime(at(16, 46, 33.75))}"`);
ok(fmtSpotTime(at(9, 4, 6)) === '09:04:06', 'leading zeros: "09:04:06"');
ok(fmtSpotTime(at(0, 0, 0)) === '00:00:00' && fmtSpotTime(at(23, 59, 59)) === '23:59:59', 'midnight and the last second of the day');
ok(fmtSpotTime(NaN) === '' && fmtSpotTime(Infinity) === '', 'not a time → nothing (never "NaN:NaN")');
// ★ the WebView/popup copy says exactly the same thing
const js = new Function(`${FMT_SPOT_TIME_JS}; return fmtSpotTime;`)() as (t: number) => string;
let same = true;
for (const t of [at(16, 46, 30), at(16, 46, 37.5), at(0, 0, 0), at(23, 59, 59), at(12, 3, 45.9)]) same = same && js(t) === fmtSpotTime(t);
ok(same, "the maps' script copy (FMT_SPOT_TIME_JS) formats identically");

console.log(fail ? `\n✗ test_spot_time: ${fail} failed` : '\n✓ test_spot_time: all passed');
process.exit(fail ? 1 : 0);
