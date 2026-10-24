/**
 * Two rules with two readers each (2026-10-24) — held to the same cases here so they cannot part:
 *   1. The ADS-B capture warning: src/services/adsbTypes.ts adsbCaptureWarning (the app's map top bar) and
 *      web/client/src/adsb.ts adsbCaptureWarning (the web client's).
 *   2. The boosted-rate guard's red line: src/services/rateGuard.ts rateGuardLine (the app's server screen) and
 *      vibe_setup_page.h rateGuardLine (the browser setup page) — and the server's own JSON as both read it.
 * The web module and the setup page cannot be imported by Node (bundler paths; a C++ raw string), so their function
 * is cut out of the source and run — the real text, not a copy.
 *
 * Run: node --no-warnings scripts/test_capture_and_rate_guard.ts   (run-tests.sh does)
 */
import { readFileSync } from 'node:fs';
import { stripTypeScriptTypes } from 'node:module';
import vm from 'node:vm';
import { adsbCaptureWarning, parseAdsbMessage } from '../src/services/adsbTypes.ts';
import { rateGuardLine } from '../src/services/rateGuard.ts';

let fails = 0, passes = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  if (JSON.stringify(got) === JSON.stringify(want)) { passes++; return; }
  fails++; console.error(`FAIL ${what}\n   got  ${JSON.stringify(got)}\n   want ${JSON.stringify(want)}`);
};
/** The source from `start` to the end of the function that follows — its closing brace in column 0. */
function cut(src: string, start: string): string {
  const a = src.indexOf(start);
  if (a < 0) throw new Error(`not found: ${start}`);
  const z = src.indexOf('\n}', src.indexOf('function', a));
  return src.slice(a, z + 2);
}

// ── 1. the capture warning ──
const web = readFileSync(new URL('../web/client/src/adsb.ts', import.meta.url), 'utf8');
const numSrc = web.slice(web.indexOf('function num('), web.indexOf('\n', web.indexOf('function num(')));
const webWarnSrc = stripTypeScriptTypes(numSrc + '\n' + cut(web, 'export const ADSB_CAPTURE_WARN_PCT').replace(/export /g, ''));
const webCtx: any = {};
vm.runInNewContext(webWarnSrc + '\nthis.f = adsbCaptureWarning;', webCtx);
const cases: [string, any][] = [
  ['unknown (-1)', { capture: -1, rate: 2.4e6 }], ['absent', { rate: 2.4e6 }], ['97 is fine', { capture: 97, rate: 2.4e6 }],
  ['96 warns', { capture: 96, rate: 2.4e6 }], ['74 on an RSP', { capture: 74, rate: 3e6 }], ['0', { capture: 0 }],
];
for (const [what, r] of cases) {
  const app = adsbCaptureWarning(r);
  eq(`capture warning, the two readers agree: ${what}`, webCtx.f({ aircraft: [], ...r }), app);
}
eq('74 % says so', adsbCaptureWarning({ capture: 74, rate: 2.4e6 })?.short, 'capture 74 %');
eq('97 % is quiet', adsbCaptureWarning({ capture: 97, rate: 2.4e6 }), null);
eq('null report is quiet', adsbCaptureWarning(null), null);
// ★ through the app's parser, as a report arrives off the socket
eq('the report parser carries capture', parseAdsbMessage({ type: 'adsb', now: 1, messages: 0, aircraft: [], capture: 63, rate: 2400000 }).capture, 63);
eq('a report without it parses to undefined', parseAdsbMessage({ type: 'adsb', aircraft: [] }).capture, undefined);

// ── 2. the rate guard's line ──
const page = readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
const pageCtx: any = {};
vm.runInNewContext(cut(page, 'function rateGuardLine(st)') + '\nthis.f = rateGuardLine;', pageCtx);
const fmt = (d: Date) => d.toLocaleString();
const say = 'SDR crashed at 2.4 MS/s during ADS-B — the server came back at its own rate, and the rate switch is off until you clear this.';
const states: [string, any][] = [
  ['nothing', null], ['empty', { fault: '', blocked: false, say: '' }],
  ['crashed', { fault: 'crashed', at: 1761300200, rate: 2400000, mode: 'ADS-B', blocked: true, say }],
  ['stalled, no time', { fault: 'stalled', at: 0, rate: 2048000, mode: 'DAB', blocked: true, say: 'SDR stopped responding at 2.048 MS/s during DAB' }],
];
for (const [what, st] of states) eq(`rate guard line, the two readers agree: ${what}`, pageCtx.f(st), rateGuardLine(st, fmt));
eq('crashed line starts with the server\'s sentence', rateGuardLine(states[2][1], fmt).startsWith(say + ' ('), true);
eq('no fault, no line', rateGuardLine({ fault: '' }), '');

console.log(`${passes}/${passes + fails} checks passed`);
process.exit(fails ? 1 : 0);
