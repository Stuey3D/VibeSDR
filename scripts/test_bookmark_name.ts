// The pre-filled bookmark name (src/utils/bookmarkName.ts) — Stuart, decided 2026-10-20.
import { bookmarkDefaultName, bookmarkFreqLabel } from '../src/utils/bookmarkName';
let fails = 0;
const ok = (c: boolean, what: string) => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fails++; };
console.log('test_bookmark_name');
ok(bookmarkDefaultName({ rds: 'BBC R4 ', station: 'Radio 4', hz: 93.5e6 }) === 'BBC R4', 'the RDS name first, tidied');
ok(bookmarkDefaultName({ rds: '   ', station: 'Radio Caroline', hz: 648e3 }) === 'Radio Caroline', 'then the station name');
ok(bookmarkDefaultName({ hz: 99.8e6 }) === '99.8 MHz', 'then the frequency (99.8 MHz)');
ok(bookmarkFreqLabel(198e3) === '198 kHz' && bookmarkFreqLabel(7.074e6) === '7.074 MHz' && bookmarkFreqLabel(0) === '', 'frequency labels');
ok(bookmarkDefaultName({ rds: 'A'.repeat(80), hz: 1e6 }).length === 60, 'never longer than the name box allows');
process.exit(fails ? 1 : 0);
