/**
 * "IS THIS ACTUALLY NEWS?" (src/services/renderChurn.ts) — the equality tests that stop a repeated
 * message re-rendering the whole radio screen (power audit, 2026-10-01).
 *
 * Proves:
 *   • an identical live-station label keeps the PREVIOUS object (so React skips the render), and a
 *     change to ANY drawn field — name, raw PS, RadioText, badge, country, PI, ECC — gets through;
 *   • DAB replacing RDS is a change even with the same name (the RDS identity must clear);
 *   • a bookmark list with the same entries is "same"; a renamed, retuned, reordered, added or
 *     removed entry is not;
 *   • the clock's minute key turns exactly on the minute and not between.
 *
 * Run: node --no-warnings scripts/test_renderChurn.ts
 */
import {
  sameLiveStation, keepIfSameStation, sameFlatList, minuteKey,
  type LiveStationLike,
} from '../src/services/renderChurn.ts';

let fails = 0, passes = 0;
function eq(what: string, got: unknown, want: unknown) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { passes++; return; }
  fails++;
  console.error(`FAIL ${what}\n   got  ${g}\n   want ${w}`);
}

// ── Live station ────────────────────────────────────────────────────────────────────────────────
const rds: LiveStationLike = { name: 'BBC R2', psRaw: 'BBC R2', text: 'Jo Whiley', badge: 'RDS',
                               countryIso: 'gb', pi: 'C202', ecc: 0xE1 };
const again = { ...rds };
eq('identical RDS is the same', sameLiveStation(rds, again), true);
eq('identical RDS keeps the PREVIOUS object', keepIfSameStation(rds, again) === rds, true);

const fields: Array<[keyof LiveStationLike, unknown]> = [
  ['name', 'BBC R3'], ['psRaw', 'BBC  R2'], ['text', 'Next: news'], ['badge', undefined],
  ['countryIso', 'ie'], ['pi', 'C203'], ['ecc', 0xE2],
];
for (const [k, v] of fields) {
  const changed = { ...rds, [k]: v } as LiveStationLike;
  eq(`a change to ${k} is news`, sameLiveStation(rds, changed), false);
  eq(`a change to ${k} hands over the NEW object`, keepIfSameStation(rds, changed) === changed, true);
}
// ★ The DAB path states its fields outright so the FM station's PI/ECC cannot leak into it.
const dab: LiveStationLike = { name: 'BBC R2', text: undefined, badge: 'DAB' };
eq('DAB after RDS with the same name is news', sameLiveStation(rds, dab), false);
eq('a repeated DAB report is not', sameLiveStation(dab, { name: 'BBC R2', text: undefined, badge: 'DAB' }), true);
eq('empty vs empty', sameLiveStation({}, {}), true);
eq('empty vs a name', sameLiveStation({}, { name: 'X' }), false);

// ── Bookmark lists ──────────────────────────────────────────────────────────────────────────────
const a = [{ name: 'Radio 4', frequency: 93_500_000, mode: 'wfm', source: 'server' },
           { name: 'Capital', frequency: 105_400_000, mode: 'wfm', source: 'server' }];
const b = a.map((x) => ({ ...x }));
eq('same entries, new objects', sameFlatList(a, b), true);
eq('same array', sameFlatList(a, a), true);
eq('renamed', sameFlatList(a, [{ ...a[0], name: 'Radio 4 FM' }, a[1]]), false);
eq('retuned', sameFlatList(a, [a[0], { ...a[1], frequency: 105_300_000 }]), false);
eq('reordered', sameFlatList(a, [a[1], a[0]]), false);
eq('added', sameFlatList(a, [...a, { name: 'Heart', frequency: 96_900_000, mode: 'wfm', source: 'server' }]), false);
eq('removed', sameFlatList(a, [a[0]]), false);
eq('a field gained', sameFlatList(a, [{ ...a[0], manual: true }, a[1]]), false);
eq('both empty', sameFlatList([], []), true);

// ── The clock's minute ──────────────────────────────────────────────────────────────────────────
const t0 = Date.UTC(2026, 9, 1, 12, 34, 0);
eq('same minute at :00 and :59.999', minuteKey(t0) === minuteKey(t0 + 59_999), true);
eq('the next minute at +60 s', minuteKey(t0 + 60_000) === minuteKey(t0) + 1, true);
eq('the previous minute 1 ms before', minuteKey(t0 - 1) === minuteKey(t0) - 1, true);

console.log(`\nrender churn: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
