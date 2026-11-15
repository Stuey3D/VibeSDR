/**
 * THE TUNED STATION'S LABEL AT THE VFO (src/utils/tunedStation.ts) — Stuart, 2026-11-15: "When the VTS disappears can we
 * have a label on the VFO with the current station name … [EiBi] China Radio Int / [Server] Radio Caroline / [User] Flex FM".
 * Proves:
 *   • precedence — the listener's own bookmark beats the server's, which beats EiBi, even when the lower one is nearer;
 *     within a source the nearest wins, the first listed on a tie;
 *   • tolerance — on the dial is the VTS's own ±99 Hz (stations.ts VTS_ON_HZ is read from the file); beyond it the label
 *     matches anything INSIDE the receive passband (Stuart: a listener slightly off who still hears it gets the name) —
 *     the demodulator's filter, else a mode default, never wider than ±100 kHz;
 *   • a supplement, never a repeat — a station the spectrum's own labels already name gets no tuned label (and nothing
 *     else in its place); that spectrum label is highlighted instead (tunedPick.highlight);
 *   • EiBi only while ON AIR NOW ("HHMM-HHMM" in the comment, across midnight too; no time = on air);
 *   • suppression — no label while a live RDS / DAB / DRM name exists, while the VTS is showing the name, or while the
 *     dial moves; the tags are exactly [User] / [Server] / [EiBi]; a long name is cut with "…";
 *   • placement — right of the needle, flipped left at the right edge, kept on screen at both edges.
 * Run: node --no-warnings scripts/test_tuned_station.ts
 */
import { readFileSync } from 'node:fs';
import { tunedStation, tunedLabel, tunedPick, tunedLabelText, tunedLabelX, utcMinutes, markerCovers, markerIndex, tunedPassband, inPassband,
         TUNED_TOL_HZ, TUNED_MAX_CHARS, TUNED_TAG, type TunedRow } from '../src/utils/tunedStation.ts';

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
ok(tunedStation([{ frequency: 225_648_000, name: 'BBC Radio 1', source: 'server', mode: 'dab' }], 225_648_000, 0) === null,
   'a DAB service is no candidate (its block\'s frequency names none of them)');

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

console.log('the receive passband (the label only — the VTS keeps ±99 Hz)');
const am9 = tunedPassband('am', -4500, 4500);
const sw: TunedRow[] = [{ frequency: 9_580_000, name: 'China Radio Int', source: 'eibi', comment: '0000-2400 · E' }];
ok(tunedLabel(sw, 9_582_000, 600, { ...settled, passband: am9 }) === '[EiBi] China Radio Int', '2 kHz off on SW AM: still labelled');
ok(tunedLabel(sw, 9_582_000, 600, settled) === null, '…but not with no passband given (the ±99 Hz on-tune rule)');
ok(tunedLabel(sw, 9_590_000, 600, { ...settled, passband: am9 }) === null, 'the adjacent channel 10 kHz away, 9 kHz AM filter: not labelled');
const usb = tunedPassband('usb', 300, 2700);
const ssb: TunedRow[] = [{ frequency: 7_074_000, name: 'Below', source: 'user' }];
ok(tunedLabel(ssb, 7_075_000, 600, { ...settled, passband: usb }) === null, 'USB, the station just below the dial (outside the sideband): not labelled');
ok(tunedLabel(ssb, 7_074_000, 600, { ...settled, passband: usb }) === '[User] Below', 'USB, the bookmark ON the dial: labelled (outside its own audio sideband)');
ok(tunedLabel(ssb, 7_073_000, 600, { ...settled, passband: usb }) === '[User] Below', 'USB, the station inside the sideband above the dial: labelled');
const pair: TunedRow[] = [{ frequency: 9_583_000, name: 'Farther', source: 'eibi' }, { frequency: 9_581_000, name: 'Nearer', source: 'eibi' }];
ok(tunedStation(pair, 9_580_000, 0, am9)?.name === 'Nearer', 'two inside the passband: the nearer to the dial wins');
ok(tunedStation([...pair, { frequency: 9_584_000, name: 'Mine', source: 'user' }], 9_580_000, 0, am9)?.name === 'Mine',
   '…and the source order still comes first among them');
const dflt = (m: string) => { const p = tunedPassband(m); return `${p.lo}..${p.hi}`; };
ok(dflt('am') === '-5000..5000' && dflt('sam') === '-5000..5000' && dflt('wfm') === '-75000..75000' && dflt('nfm') === '-6000..6000'
   && dflt('usb') === '0..3000' && dflt('lsb') === '-3000..0' && dflt('cwu') === '-300..300' && dflt('ft8') === '-1500..1500',
   'mode defaults when the passband is unknown (AM ±5k, WFM ±75k, NFM ±6k, USB 0..3k, LSB −3k..0, CW ±300, data ±1.5k)');
const wide = tunedPassband('wfm', -400_000, 400_000);
ok(wide.lo === -100_000 && wide.hi === 100_000, 'never wider than ±100 kHz');
ok(tunedPassband('am', 3000, -3000).lo === -5000, 'a nonsense passband (high below low) falls back to the mode default');
ok(inPassband(1e6 + 99, 1e6, { lo: 0, hi: 0 }) && !inPassband(1e6 + 100, 1e6, { lo: 0, hi: 0 }), 'on the dial (±99 Hz) always counts');

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

console.log('a supplement, never a repeat (the spectrum\'s own labels)');
const flex: TunedRow[] = [{ frequency: 9_580_000, name: 'Flex FM', source: 'user' }];
ok(tunedLabel(flex, 9_580_000, 600, { ...settled, shown: [{ hz: 9_580_000, dialHz: 9_580_000, src: 'personal' }] }) === null,
   'your bookmark already labelled on the spectrum: no "[User] Flex FM" under "Flex FM"');
ok(tunedLabel(flex, 9_580_000, 600, { ...settled, shown: [] }) === '[User] Flex FM', 'labels off (none drawn): the label');
ok(tunedLabel(flex, 9_580_000, 600, { ...settled, shown: [{ hz: 9_590_000, src: 'personal' }] }) === '[User] Flex FM',
   'a different bookmark labelled elsewhere does not hide this one');
ok(tunedLabel(flex, 9_580_000, 600, { ...settled, shown: [{ hz: 9_580_000, src: 'eibi' }] }) === '[User] Flex FM',
   'an EiBi label at the same place does not hide a bookmark');
{ const pk = tunedPick(rows.slice(0, 2), 9_580_000, 600, { ...settled, shown: [{ hz: 7e6, src: 'personal' }, { hz: 9_580_050, src: 'server' }] });
  ok(pk.label === null && pk.highlight === 1,
     'the server\'s bookmark labelled: NO tuned label, no EiBi name in its place — that label (index 1) is highlighted'); }
ok(tunedPick(rows.slice(0, 2), 9_580_000, 600, { ...settled, shown: [{ hz: 9_580_050, src: 'learnt' }] }).highlight === 0,
   'a learnt FM name counts as the server\'s label');
{ const pk = tunedPick(flex, 9_580_000, 600, { ...settled, shown: [] });
  ok(pk.label === '[User] Flex FM' && pk.highlight === -1, 'no label drawn: the tuned label, nothing highlighted'); }
for (const [st, why] of [[{ vtsShowing: false, settled: false }, 'while the dial moves'], [{ vtsShowing: true, settled: true }, 'while the VTS shows the name'],
                         [{ ...settled, liveName: 'BBC R4' }, 'while a live name is held']] as const)
  ok(tunedPick(flex, 9_580_000, 600, { ...st, shown: [{ hz: 9_580_000, src: 'personal' }] }).highlight === -1, `no highlight ${why} (the same rules as the label)`);
ok(markerIndex({ frequency: 9e6, source: 'user' }, [{ hz: 9e6 + 80, src: 'personal' }, { hz: 9e6 + 10, src: 'server' }, { hz: 9e6 + 10, src: 'personal' }]) === 1,
   'the nearest drawn label is the one highlighted; the first on a tie');
ok(tunedPick(rows.slice(0, 1), 9_580_000, 600, { ...settled, shown: [{ hz: 9_581_000, dialHz: 9_580_000, src: 'eibi' }] }).highlight === 0,
   'an EiBi row already labelled (by its dial — HFDL / voice markers sit above it): no label, that one highlighted');
ok(markerCovers({ frequency: 7_000_000, source: 'server' }, [{ hz: 7_000_099, src: 'server' }])
   && !markerCovers({ frequency: 7_000_000, source: 'server' }, [{ hz: 7_000_100, src: 'server' }]), 'covered within the VTS\'s ±99 Hz');

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
