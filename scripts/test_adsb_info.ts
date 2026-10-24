/* test_adsb_info.ts — V12 aircraft data on the CLIENTS: the adsb_info sanitiser and per-hex store
 * (src/services/adsbInfo.ts — the app and the web client both import it), and the web list's rows with it
 * (web/client/src/adsb.ts adsbListHtml, bundled here with esbuild because it imports the page's map modules).
 * ★ Somebody else's server's JSON: markup, wrong types and path characters must never come through. */
import { build } from 'esbuild';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
const { cleanAdsbInfo, AdsbInfoStore, adsbInfoLine, routeShort, flagEmoji } = await import('../src/services/adsbInfo.ts');

let pass = 0, fail = 0;
function eq(what: string, got: unknown, want: unknown) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) pass++; else { fail++; console.error(`FAIL ${what}\n   got  ${g}\n   want ${w}`); }
}
function ok(what: string, c: boolean) { eq(what, c, true); }

// ── the sanitiser ──
const full = cleanAdsbInfo({ r: 'EI-DLJ', t: 'B738', desc: 'BOEING 737-800', wtc: 'M', eng: '2J', species: 'L', country: 'IE',
  countryName: 'Ireland', mil: 1, op: 'RYR', opName: 'Ryanair', logo: 'RYR-B738', sil: 'B738',
  route: { from: 'GCRR', to: 'EGBB', fromIata: 'ACE', toIata: 'BHX', fromName: 'Lanzarote', toName: 'Birmingham', plausible: true,
           stops: ['GCRR', 'EGBB'] } });
eq('a whole record survives', Object.keys(full).length, 14);
eq('two stops are not a multi-stop route', full.route?.stops, undefined);
const bad = cleanAdsbInfo({ r: '<img src=x onerror=alert(1)>', t: 'B7378X', wtc: 'Z', eng: '22J', country: 'gbr', mil: 'yes',
  op: 'R-R', logo: '../../etc/passwd', sil: 'a/b', opName: 42, route: { from: 'EGLL' }, desc: 'x'.repeat(200) });
eq('markup stripped, bad shapes dropped', bad, { r: 'img src=x on', desc: 'x'.repeat(60) });
eq('not an object', cleanAdsbInfo('nope'), {});

// ── the store ──
const st = new AdsbInfoStore();
st.take({ type: 'adsb_info', aircraft: { '4CA2D6': { r: 'EI-DLJ' }, 'zzzzzz': { r: 'X' }, '~123456': { r: 'Y' } } });
eq('hex keys normalised, junk keys refused', [st.size, st.get('4ca2d6')?.r], [1, 'EI-DLJ']);
const merged = st.apply([{ hex: '4ca2d6', flight: 'RYR8CK', r: 'FROM-REPORT' } as any, { hex: '400001' }]);
eq('merged, the report wins a clash', [merged[0].flight, merged[0].r], ['RYR8CK', 'FROM-REPORT']);
st.take({ aircraft: { '4ca2d6': {} } });
eq('an empty record clears it', st.get('4ca2d6'), undefined);
st.take({ aircraft: { '4ca2d6': { r: 'A' }, '400001': { r: 'B' } } });
st.apply([{ hex: '400001' }]);
eq('an aircraft that left the report is forgotten', [st.get('4ca2d6'), st.get('400001')?.r], [undefined, 'B']);
st.take({ full: 1, aircraft: { '500000': { r: 'C' } } });
eq('full replaces everything', [st.size, st.get('400001')], [1, undefined]);

// ── words ──
eq('route short', routeShort(full.route), 'ACE → BHX');
eq('implausible: no short route', routeShort({ from: 'EGLL', to: 'KJFK', plausible: false }), '');
eq('info line', adsbInfoLine(full), 'RYR · B738 · ACE → BHX · EI-DLJ');
eq('flag', [flagEmoji('IE'), flagEmoji('I'), flagEmoji(null)], ['\u{1F1EE}\u{1F1EA}', '', '']);

// ── the web list's rows (web/client/src/adsb.ts), bundled ──
{
  const root = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
  const out = await build({ entryPoints: [path.join(root, 'web/client/src/adsb.ts')], bundle: true, write: false, format: 'esm',
                            platform: 'neutral', logLevel: 'silent', loader: { '.ts': 'ts' } });
  const mod = await import('data:text/javascript;base64,' + Buffer.from(out.outputFiles[0].text).toString('base64'));
  const report = { aircraft: [
    { hex: '4ca2d6', flight: 'RYR8CK', lat: 52, lon: -2, alt_baro: 36000, ...full, mil: undefined },
    { hex: '3f0001', lat: 51, lon: 0, mil: 1, country: 'DE' },
    { hex: '400123', flight: 'BAW1', lat: 51.5, lon: -0.5 },
  ] };
  const html: string = mod.adsbListHtml(report, { lat: 52, lon: -1 }, 'http://srv/adsb/');
  ok('flag before the callsign', html.includes('<span class="flag" title="IE">\u{1F1EE}\u{1F1EA}</span>RYR8CK'));
  ok('the owner\'s logo replaces the badge', html.includes('<img class="oplogo" src="http://srv/adsb/logo/RYR-B738" alt="RYR">'));
  ok('type · route · registration on the second line', html.includes('B738 · ACE → BHX · EI-DLJ'));
  ok('server-military row is military', /data-hex="3f0001"/.test(html) && html.indexOf('3f0001') < html.indexOf('4ca2d6') && html.includes('adsbRow mil'));
  ok('no data: hex on the second line as before', html.includes('<small>400123</small>'));
  const noLogo: string = mod.adsbListHtml(report, null, '');
  ok('no image base: the ICAO code as a badge', noLogo.includes('<span class="op">RYR</span> B738'));
  const evil: string = mod.adsbListHtml({ aircraft: [{ hex: '400999', ...cleanAdsbInfo({ r: '"><script>x</script>', op: 'ABC' }) }] }, null, '');
  ok('escaped', !evil.includes('<script>'));
}

console.log(`test_adsb_info: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
