/* test_adsb_info.ts — V12 aircraft data on the CLIENTS: the adsb_info sanitiser and per-hex store
 * (src/services/adsbInfo.ts — the app and the web client both import it).
 * ★ Somebody else's server's JSON: markup, wrong types and path characters must never come through. */
import { cleanAdsbInfo, AdsbInfoStore, adsbInfoLine, routeShort, flagEmoji, adsbShowsData, adsbCreditHtml, ADSB_DATA_CREDIT } from '../src/services/adsbInfo.ts';

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
// ★ a registration-specific logo (VRS: operator-type+registration) is longer than 12 — it must arrive WHOLE (Jet2 G-JZBP)
eq('a 14-character logo key survives whole', cleanAdsbInfo({ logo: 'EXS-B738G-JZBP' }).logo, 'EXS-B738G-JZBP');
eq('a 40-character key survives', cleanAdsbInfo({ sil: 'A'.repeat(40) }).sil, 'A'.repeat(40));
eq('a 41-character key is refused, not cut to 40', cleanAdsbInfo({ logo: 'A'.repeat(41) }).logo, undefined);

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

// ★ V12 map purge (Stuart 11-27): the web's old ADS-B list (adsb.ts adsbListHtml) is gone — the map's LIST rows are the
//   aviation map's (trkRowHtml, scripts/test_aviation_map.ts), the licence words HELP's (test-web-avmap.mjs)

// ── beta91 the notice's rule and words (adsbInfo.ts — the ADS-B list, the aviation list and the layer card's text) ──
ok('shows data: a registration / type / operator', adsbShowsData([{ r: 'G-ABCD' }]) && adsbShowsData([null, { op: 'RYR' }]) && !adsbShowsData([{}, null]));
ok('the card\'s words', adsbCreditHtml().includes('contains information from Mictronics aircraft database, which is made available under the ')
   && adsbCreditHtml().includes('VRS standing data</a> (CC0)') && ADSB_DATA_CREDIT.odcUrl === 'https://opendatacommons.org/licenses/by/1-0/');

console.log(`test_adsb_info: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
