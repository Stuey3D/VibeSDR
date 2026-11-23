// test_log_radio.ts — the logs' RADIO filter rule (src/utils/logRadio.ts, V12 beta92, docs/v12/LOG-RADIO-FILTER.md).
import { logRadioChoices, entryRadios, radioPasses, keepRadioPick, radioQuery, heardByText, radioChipLabel } from '../src/utils/logRadio.ts';
let pass = 0, fail = 0;
const ok = (c: boolean, m: string) => { if (c) pass++; else { fail++; console.log('  FAIL ' + m); } };
const eq = (a: unknown, b: unknown, m: string) => ok(JSON.stringify(a) === JSON.stringify(b), `${m}: ${JSON.stringify(a)}`);

const two = [{ id: 'aa11bb22', name: 'RSP1A HF', n: 11 }, { id: 'cc33dd44', name: 'RTL VHF', n: 4 }];
eq(logRadioChoices(two)?.map((r) => r.id), ['aa11bb22', 'cc33dd44'], 'two radios with entries: a filter');
eq(logRadioChoices([two[0]]), null, 'one radio: no filter');
eq(logRadioChoices([two[0], { id: 'cc33dd44', name: 'RTL', n: 0 }]), null, 'the second radio holds nothing: no filter');
eq(logRadioChoices(undefined), null, 'an older server (no radios): no filter');
eq(logRadioChoices([two[0], { id: '../etc', name: 'x', n: 3 }, { id: 'aa11bb22', name: 'dup', n: 1 }]), null, 'junk ids and repeats dropped');
eq(logRadioChoices([two[0], { id: 'ee55', name: '', n: 2 }])?.[1].name, 'ee55', 'no name: its id');
eq(entryRadios('aa11bb22,cc33dd44'), ['aa11bb22', 'cc33dd44'], 'a gallery "ri"');
eq(entryRadios(['aa11bb22']), ['aa11bb22'], 'a station "radios"');
eq(entryRadios(undefined), [], 'none = unknown');
ok(radioPasses('aa11bb22', '') && radioPasses('aa11bb22', 'aa11bb22') && !radioPasses('aa11bb22', 'cc33dd44'), 'one radio\'s entry');
ok(radioPasses('aa11bb22,cc33dd44', 'cc33dd44') && radioPasses(['aa11bb22', 'cc33dd44'], 'aa11bb22'), 'heard by both: under both');
ok(radioPasses(undefined, 'cc33dd44') && radioPasses('', 'aa11bb22'), 'unknown: under every radio');
eq([keepRadioPick('cc33dd44', logRadioChoices(two)), keepRadioPick('ffff', logRadioChoices(two)), keepRadioPick('aa11bb22', null)], ['cc33dd44', '', ''], 'the pick kept only while offered');
eq([radioQuery(''), radioQuery('aa11bb22'), radioQuery('a&b=1')], ['', '&radio=aa11bb22', ''], 'the query');
eq(heardByText('aa11bb22,cc33dd44', logRadioChoices(two)), 'Heard by RSP1A HF and RTL VHF', 'heard by two');
eq(heardByText('aa11bb22', logRadioChoices(two)), null, 'one radio: nothing said');
eq(radioChipLabel(two[0]), 'RSP1A HF · 11', 'chip label');

console.log(`test_log_radio: ${pass} passed, ${fail} failed`);
if (fail) process.exit(1);
