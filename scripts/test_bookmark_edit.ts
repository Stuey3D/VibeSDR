// The bookmark EDIT form's rules (src/utils/bookmarkEdit.ts) — V12, GitHub discussion #59. Shared by the app and the web.
import {
  checkBookmarkEdit, parseBookmarkFrequency, formatBookmarkFrequency, bookmarkEditForm, fillFromDial,
  serverEditQuery, serverLeftOldRow, serverEditError, applyPersonalEdit, bookmarkModeChoices, BOOKMARK_MODES,
  type BookmarkEditForm,
} from '../src/utils/bookmarkEdit';
import type { UserBookmark } from '../src/services/userBookmarks';
let fails = 0;
const ok = (c: boolean, what: string) => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fails++; };
console.log('test_bookmark_edit');

// ── frequency box ──
ok(parseBookmarkFrequency('96.1', 'mhz') === 96_100_000, '96.1 in MHz');
ok(parseBookmarkFrequency('100,5', 'mhz') === 100_500_000, 'a European comma is a decimal point');
ok(parseBookmarkFrequency('7150', 'khz') === 7_150_000 && parseBookmarkFrequency('7150k', 'mhz') === 7_150_000, 'kHz box, and a typed "k" beats the box');
ok(parseBookmarkFrequency('14.074 MHz', 'khz') === 14_074_000 && parseBookmarkFrequency('198 kHz', 'mhz') === 198_000, 'units typed out in full');
ok(parseBookmarkFrequency('', 'mhz') === null && parseBookmarkFrequency('abc', 'mhz') === null && parseBookmarkFrequency('-5', 'mhz') === null
   && parseBookmarkFrequency('0', 'mhz') === null, 'empty, words, negative and zero are not frequencies');
ok(formatBookmarkFrequency(96_100_000, 'mhz') === '96.1' && formatBookmarkFrequency(7_150_000, 'khz') === '7150'
   && formatBookmarkFrequency(14_074_500, 'mhz') === '14.0745', 'shown without trailing zeros');

// ── the rules ──
const base: BookmarkEditForm = { name: 'Radio 4', freqText: '198', unit: 'khz', mode: 'am', defaultPassband: true, lowText: '', highText: '' };
const r0 = checkBookmarkEdit(base);
ok(r0.ok && r0.value.frequency === 198_000 && r0.value.bandwidth_low === null && r0.value.bandwidth_high === null, 'a good edit, the mode default = no edges');
const r1 = checkBookmarkEdit({ ...base, defaultPassband: false, lowText: '-4500', highText: '+4500' });
ok(r1.ok && r1.value.bandwidth_low === -4500 && r1.value.bandwidth_high === 4500, 'a passband, signs allowed');
const bad = (f: Partial<BookmarkEditForm>, re: RegExp, what: string) => {
  const r = checkBookmarkEdit({ ...base, ...f });
  ok(!r.ok && re.test((r as any).error), `${what} → "${(r as any).error ?? 'accepted'}"`);
};
bad({ name: '   ' }, /name/, 'no name');
bad({ name: 'x'.repeat(61) }, /60/, 'a name past 60 characters');
bad({ freqText: '' }, /frequency/, 'no frequency');
bad({ freqText: '200 GHz' }, /100 GHz/, 'past 100 GHz');
bad({ mode: '' }, /mode/, 'no mode');
bad({ mode: 'wobble' }, /WOBBLE/, 'an unknown mode');
bad({ defaultPassband: false, lowText: '-4000', highText: '' }, /whole hertz/, 'one edge only');
bad({ defaultPassband: false, lowText: '4000', highText: '-4000' }, /low edge/, 'low above high');
bad({ defaultPassband: false, lowText: '0', highText: '0' }, /low edge/, 'low equal to high');
bad({ defaultPassband: false, lowText: '-600000', highText: '0' }, /500000/, 'an edge past ±500 kHz');
bad({ defaultPassband: false, lowText: '-4.5', highText: '4' }, /whole hertz/, 'a fractional edge');
ok(checkBookmarkEdit({ ...base, mode: 'USB' }).ok, 'the mode is case-blind');
ok(BOOKMARK_MODES.includes('dab') && BOOKMARK_MODES.includes('sam') && bookmarkModeChoices('fax').includes('fax')
   && bookmarkModeChoices('am').length === BOOKMARK_MODES.length, "the row's own odd mode is offered, nothing doubled");

// ── form from a bookmark, and "what I'm listening to now" ──
const f0 = bookmarkEditForm({ name: 'FT8', frequency: 14_074_000, mode: 'USB', bandwidth_low: 0, bandwidth_high: 3000 }, 'mhz');
ok(f0.freqText === '14.074' && f0.mode === 'usb' && !f0.defaultPassband && f0.lowText === '0' && f0.highText === '3000', 'prefilled from the bookmark');
ok(bookmarkEditForm({ name: 'X', frequency: 1e6, mode: 'am', bandwidth_low: 5, bandwidth_high: 5 }, 'mhz').defaultPassband, 'a nonsense saved passband opens as the mode default');
const f1 = fillFromDial(f0, { frequency: 14_075_500, mode: 'usb', bandwidthLow: 100, bandwidthHigh: 2900 });
ok(f1.name === 'FT8' && f1.freqText === '14.0755' && f1.lowText === '100' && f1.highText === '2900', 'the dial fills frequency/mode/passband and keeps the name');
ok(fillFromDial(f0, { frequency: 96.1e6, mode: 'wfm' }).defaultPassband, 'a dial with no passband = the mode default');

// ── the server edit ──
const v = (checkBookmarkEdit({ ...base, name: 'R4 & LW' }) as any).value;
ok(serverEditQuery({ frequency: 198_000 }, v) === 'was=198000&frequency=198000&name=R4%20%26%20LW&mode=am', `query: ${serverEditQuery({ frequency: 198_000 }, v)}`);
ok(serverEditQuery({ frequency: 222_064_000, sid: 0xC221 }, { ...v, mode: 'dab', bandwidth_low: -1, bandwidth_high: 1 }).includes('&was_sid=49697&')
   && serverEditQuery({ frequency: 222_064_000, sid: 0xC221 }, { ...v, bandwidth_low: -1, bandwidth_high: 1 }).endsWith('&bandwidth_low=-1&bandwidth_high=1'),
   'a DAB service carries was_sid; edges go when set');
const moved = { ...v, frequency: 199_000 };
ok(serverLeftOldRow([{ frequency: 198_000 }, { frequency: 199_000 }], { frequency: 198_000 }, moved), 'an older server that ADDED and kept the old row is spotted');
ok(!serverLeftOldRow([{ frequency: 199_000 }], { frequency: 198_000 }, moved), '…a current one (old gone) is not');
ok(!serverLeftOldRow([{ frequency: 198_000 }], { frequency: 198_000 }, v), '…nor an edit in place');
ok(!serverLeftOldRow([{ frequency: 222_064_000, sid: 2 }], { frequency: 222_064_000, sid: 1 }, { ...v, frequency: 225_648_000 }), '…nor another DAB service on the same block');
ok(serverEditError(409, '{"error":"There is already a bookmark at 7.15 MHz"}') === 'There is already a bookmark at 7.15 MHz', "the server's own words");
ok(/admin/.test(serverEditError(401, '')) && /HTTP 500/.test(serverEditError(500, 'oops')), 'a plain fallback when it sent none');

// ── your own list ──
const mine: UserBookmark[] = [
  { name: 'A', frequency: 1e6, mode: 'am', scope: '', synced: true, group: 'G' },
  { name: 'B', frequency: 2e6, mode: 'am', scope: '' },
  { name: 'A', frequency: 3e6, mode: 'am', scope: 'http://other' },
];
const e1 = applyPersonalEdit(mine, mine[0], { name: 'A2', frequency: 1.5e6, mode: 'usb', bandwidth_low: 300, bandwidth_high: 2700 }, 42);
ok(e1.length === 3 && e1[0].name === 'A2' && e1[0].frequency === 1.5e6 && e1[0].mode === 'usb' && e1[0].bandwidth_high === 2700
   && e1[0].synced === true && e1[0].group === 'G' && e1[0].updatedAt === 42, 'edited in place, group / iCloud / scope kept, stamped');
ok(e1[2].name === 'A' && e1[2].frequency === 3e6, 'a same-named row on another server is untouched');
const e2 = applyPersonalEdit(mine, mine[0], { name: 'B', frequency: 2e6, mode: 'am', bandwidth_low: null, bandwidth_high: null });
ok(e2.length === 2 && e2.filter((b) => b.name === 'B').length === 1, 'editing into an exact twin leaves one row');
ok(applyPersonalEdit(mine, { ...mine[0], name: 'Z' }, v) === mine, 'a row that has gone leaves the list as it was');

process.exit(fails ? 1 : 0);
