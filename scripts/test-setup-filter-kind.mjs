// test-setup-filter-kind.mjs — the setup page's "Filters fitted" rows: the filter TYPE is four tick boxes, none ticked on a
// new row, exactly one ticked after a choice, and a row with no type is never saved (Stuart, 2026-11-13: "most of our users
// seem to accidentally set their antennas to band stop not bandpass … put tick boxes and make the user choose one").
// The page's own code, rendered in jsdom and clicked.   node scripts/test-setup-filter-kind.mjs
import fs from 'node:fs';
import { JSDOM } from 'jsdom';

const src = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
let fail = 0;
const ok = (c, what) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fail++; };

const a = src.indexOf('// ANTBANDS-BEGIN');
const r0 = src.indexOf('function renderAntBands()');
const r1 = src.indexOf('\n}\n', r0);
if (a < 0 || r0 < a || r1 < 0) { console.log('   FAIL the aerial editor was not found in the page'); process.exit(1); }
const code = src.slice(a, r1 + 2);

const dom = new JSDOM('<div id="antRanges"></div><div id="antFilters"></div>');
const { window } = dom;
const doc = window.document;
const radioObj = { antennas: [], antennaRanges: '', antennaFilters: '' };
const api = new Function('document', '$', 'esc', 'radio', 'antPortNamesOf', 'antPortLabel',
  `let curRadio = 0;\n${code}\nreturn { antBandAdd, antRowsFor, renderAntBands, antFormatFilters };`)(
  doc, (id) => doc.getElementById(id), (s) => String(s), () => radioObj, () => ({}), (p) => p);

const host = () => doc.getElementById('antFilters');
const boxes = () => Array.from(host().querySelectorAll('input[type=checkbox][data-kind]'));
const ticked = () => boxes().filter((b) => b.checked).map((b) => b.getAttribute('data-kind'));
const click = (kind) => { const b = boxes().find((x) => x.getAttribute('data-kind') === kind); b.checked = !b.checked; b.dispatchEvent(new window.Event('change')); };

console.log('setup page — the filter type is four tick boxes, and the owner must choose one');
ok(!/data-f="kind"/.test(code), 'no filter-type dropdown left');
api.antBandAdd(radioObj, 'filters'); api.renderAntBands();
ok(boxes().length === 4, 'a new filter row has four tick boxes');
ok(boxes().map((b) => b.getAttribute('data-kind')).join() === 'bandpass,bandstop,highpass,lowpass', '…band-pass first, then band-stop, high-pass, low-pass');
ok(ticked().length === 0, '★ none is ticked on a new row (it used to open on Band-stop)');
ok(/Tick the type of filter first/.test(host().textContent) && !host().querySelector('input[type=number]'),
   '…the row asks for a type and shows no frequency boxes until one is ticked');
ok(radioObj.antennaFilters === '', 'a row with no type is not saved');

click('bandpass');
ok(ticked().join() === 'bandpass', 'ticking Band-pass ticks it alone');
ok(host().querySelectorAll('input[type=number]').length === 2, '…and the from/to boxes appear');
const nums = host().querySelectorAll('input[type=number]');
nums[0].value = '174'; nums[0].dispatchEvent(new window.Event('input'));
nums[1].value = '240'; nums[1].dispatchEvent(new window.Event('input'));
ok(radioObj.antennaFilters === 'bandpass 174-240MHz', `the finished row saves as a band-pass (${radioObj.antennaFilters})`);

click('bandstop');
ok(ticked().join() === 'bandstop', 'ticking Band-stop moves the tick — never two at once');
ok(radioObj.antennaFilters === 'bandstop 174-240MHz', '…and the saved row follows');
click('highpass');
ok(ticked().join() === 'highpass' && host().querySelectorAll('input[type=number]').length === 1, 'High-pass: one box (passes above)');
click('highpass');
ok(ticked().length === 0 && radioObj.antennaFilters === '', 'unticking the ticked box leaves no type — and nothing saved');

ok(api.antFormatFilters([{ kind: '', from: '1', to: '2', unit: 'MHz', name: 'x', port: '' }]) === '', 'the writer drops a row with no type');
// a filter saved before this change reopens with its own type ticked (a fresh radio object re-reads the stored text)
const saved = { antennas: [], antennaRanges: '', antennaFilters: 'highpass 1.7MHz MW high-pass' };
const api2 = new Function('document', '$', 'esc', 'radio', 'antPortNamesOf', 'antPortLabel',
  `let curRadio = 0;\n${code}\nreturn { renderAntBands };`)(
  doc, (id) => doc.getElementById(id), (x) => String(x), () => saved, () => ({}), (q) => q);
api2.renderAntBands();
ok(ticked().join() === 'highpass' && host().querySelector('input[type=number]').value === '1.7',
   'a high-pass saved before the change reopens with High-pass ticked and its 1.7 MHz');

console.log(fail ? `   ${fail} FAILED` : '   all passed');
process.exit(fail ? 1 : 0);
