/**
 * THE TUNED STATION'S LABEL AT THE VFO (src/utils/tunedStation.ts) — Stuart, 2026-11-15: "When the VTS disappears can we
 * have a label on the VFO with the current station name … [EiBi] China Radio Int / [Server] Radio Caroline / [User] Flex FM".
 * Proves:
 *   • precedence — the listener's own bookmark beats the server's, which beats EiBi, even when the lower one is nearer;
 *     within a source the nearest wins, the first listed on a tie;
 *   • tolerance — the VTS's own ±99 Hz (and it IS the VTS's: stations.ts VTS_ON_HZ is read from the file);
 *   • EiBi only while ON AIR NOW ("HHMM-HHMM" in the comment, across midnight too; no time = on air);
 *   • suppression — no label while a live RDS / DAB / DRM name exists, while the VTS is showing the name, or while the
 *     dial moves; the tags are exactly [User] / [Server] / [EiBi]; a long name is cut with "…";
 *   • placement — right of the needle, flipped left at the right edge, kept on screen at both edges.
 * Run: node --no-warnings scripts/test_tuned_station.ts
 */
import { readFileSync } from 'node:fs';
import { tunedStation, tunedLabel, tunedLabelText, tunedLabelX, utcMinutes, TUNED_TOL_HZ, TUNED_MAX_CHARS, TUNED_TAG,
         type TunedRow } from '../src/utils/tunedStation.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const settled = { vtsShowing: false, settled: true };

console.log('precedence');
const rows: TunedRow[] = [
  { frequency: 9_580_000, name: 'China Radio Int', source: 'eibi', comment: '0000-2400 · E' },
  { frequency: 9_580_050, name: 'Radio Caroline', source: 'server' },
  { frequency: 9_580_090, name: 'Flex FM', source: 'user' },
];
ok(tunedLabel(rows, 9_580_000, 600, settled) === '[User] Flex FM', 'user beats server and EiBi, though they are nearer');
ok(tunedLabel(rows.slice(0, 2), 9_580_000, 600, settled) === '[Server] Radio Caroline', 'server beats EiBi, though EiBi is dead on');
ok(tunedLabel(rows.slice(0, 1), 9_580_000, 600, settled) === '[EiBi] China Radio Int', 'EiBi alone');
const two: TunedRow[] = [
  { frequency: 100_000_080, name: 'Far', source: 'user' },
  { frequency: 100_000_010, name: 'Near', source: 'user' },
  { frequency: 100_000_010, name: 'Near twin', source: 'user' },
];
ok(tunedStation(two, 100_000_000, 0)?.name === 'Near', 'nearest within a source; the first listed on a tie');
ok(tunedStation([{ frequency: 1e6, name: 'x', source: 'heard' }, { frequency: 1e6, name: '  ', source: 'user' }], 1e6, 0) === null,
   'an unknown source or a blank name is no candidate');

console.log('tolerance');
let vtsOn = NaN;
{ const m = /export const VTS_ON_HZ\s*=\s*(\d+)/.exec(readFileSync(new URL('../src/services/stations.ts', import.meta.url), 'utf8')); if (m) vtsOn = +m[1]; }
ok(TUNED_TOL_HZ === vtsOn, `the tolerance is the VTS's own (stations.ts VTS_ON_HZ = ${vtsOn})`);
const one: TunedRow[] = [{ frequency: 7_200_000, name: 'On', source: 'server' }];
ok(tunedStation(one, 7_200_099, 0)?.name === 'On' && tunedStation(one, 7_199_901, 0)?.name === 'On', '±99 Hz is on it');
ok(tunedStation(one, 7_200_100, 0) === null && tunedStation(one, 7_199_900, 0) === null, '100 Hz off is not');
ok(tunedStation([{ frequency: 7_200_150, name: 'Off', source: 'user' }, ...one], 7_200_000, 0)?.source === 'server',
   'a higher-ranked row outside the tolerance does not win');
ok(tunedStation(one, 0, 0) === null && tunedStation(one, NaN, 0) === null, 'no dial, no station');

console.log('EiBi on air now');
const cri: TunedRow[] = [{ frequency: 6_175_000, name: 'BBC', source: 'eibi', comment: '1800-2000 · E' }];
ok(tunedStation(cri, 6_175_000, 18 * 60) !== null && tunedStation(cri, 6_175_000, 19 * 60 + 59) !== null, 'inside its hours');
ok(tunedStation(cri, 6_175_000, 20 * 60) === null && tunedStation(cri, 6_175_000, 17 * 60 + 59) === null, 'outside them: no label');
const night: TunedRow[] = [{ frequency: 5_975_000, name: 'Night', source: 'eibi', comment: '2300-0100 · R' }];
ok(tunedStation(night, 5_975_000, 23 * 60 + 30) !== null && tunedStation(night, 5_975_000, 30) !== null
   && tunedStation(night, 5_975_000, 2 * 60) === null, 'across midnight');
ok(tunedStation([{ frequency: 5e6, name: 'Pre-filtered', source: 'eibi', comment: 'E' }], 5e6, 777) !== null,
   'no time in the comment = on air (the app\'s list is already filtered to the hour)');
const offAirThenServer: TunedRow[] = [...cri, { frequency: 6_175_040, name: 'Heard here', source: 'server' }];
ok(tunedStation(offAirThenServer, 6_175_000, 3 * 60)?.name === 'Heard here', 'an off-air EiBi row never hides a server one');
const eibiOffThenOn: TunedRow[] = [{ frequency: 9e6, name: 'Off now', source: 'eibi', comment: '0000-0100' },
                                   { frequency: 9e6 + 50, name: 'On now', source: 'eibi', comment: '0000-2400' }];
ok(tunedStation(eibiOffThenOn, 9e6, 600)?.name === 'On now', 'the nearer EiBi row off air gives way to one on air');

console.log('suppression');
ok(tunedLabel(rows, 9_580_000, 600, { ...settled, liveName: 'BBC R4' }) === null, 'a live RDS / DAB / DRM name: no label');
ok(tunedLabel(rows, 9_580_000, 600, { ...settled, liveName: '   ' }) === '[User] Flex FM', 'a blank live name is no name');
ok(tunedLabel(rows, 9_580_000, 600, { vtsShowing: true, settled: true }) === null, 'while the VTS shows the name: no label');
ok(tunedLabel(rows, 9_580_000, 600, { vtsShowing: false, settled: false }) === null, 'while the dial moves: no label');
ok(tunedLabel([], 9_580_000, 600, settled) === null, 'no station: no label');

console.log('text');
ok(TUNED_TAG.user === '[User]' && TUNED_TAG.server === '[Server]' && TUNED_TAG.eibi === '[EiBi]', 'the tags, exactly');
const long = tunedLabelText({ source: 'eibi', name: 'Voice of the Islamic Republic of Iran External Service', hz: 1 });
ok(long.startsWith('[EiBi] Voice of the') && long.endsWith('…') && Array.from(long.slice(7)).length === TUNED_MAX_CHARS,
   `a long name is cut to ${TUNED_MAX_CHARS} characters with "…" (${long})`);
ok(tunedLabelText({ source: 'user', name: 'Радио Ретро', hz: 1 }) === '[User] Радио Ретро', 'non-Latin names whole');

console.log('placement');
ok(tunedLabelX(100, 80, 390) === 106, 'right of the needle');
ok(tunedLabelX(350, 80, 390) === 264, 'flipped left near the right edge');
ok(tunedLabelX(40, 80, 100) === 2, 'kept on screen at the left when it fits neither side');
ok(tunedLabelX(-30, 80, 390) === 2, 'a needle off the left edge: the box stays on screen');
ok(tunedLabelX(200, 500, 390) === 2, 'a box wider than the view starts at the margin');
ok(tunedLabelX(600, 120, 640) === 600 - 6 - 120, 'the handheld at 640 px');

console.log('clock');
ok(utcMinutes(new Date(Date.UTC(2026, 10, 15, 18, 13, 22))) === 18 * 60 + 13, 'minutes past 00:00 UTC');

console.log(`\n${pass} passed, ${fail} failed`);
if (fail) process.exit(1);
