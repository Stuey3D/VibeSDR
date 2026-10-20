// appAlert's rules (src/utils/appAlertModel.ts) — controller audit root cause D, 2026-10-20.
import { alertButtons, alertStartIndex, alertBack, alertMove } from '../src/utils/appAlertModel';

let fails = 0;
const ok = (c: boolean, what: string) => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fails++; };
const J = JSON.stringify;

console.log('test_app_alert');
ok(alertButtons(undefined).length === 1 && (alertButtons(undefined)[0] as any).text === 'OK', 'no buttons: one OK, as Alert.alert');
const takeOver = [{ text: 'Cancel', style: 'cancel' as const }, { text: 'Take over', style: 'destructive' as const }];
ok(alertStartIndex(takeOver) === 0, 'Take over?: starts on Cancel — a destructive key is never the default');
ok(J(alertBack(takeOver, true)) === '{"press":0}', 'Back presses Cancel');
const compat = [{ text: 'Back' }, { text: 'Continue' }];
ok(alertStartIndex(compat) === 1, 'two plain keys: starts on the last (the default side)');
ok(J(alertBack(compat, false)) === '{"none":true}', 'no cancel key and not cancelable: Back does nothing');
ok(J(alertBack(compat, true)) === '{"dismiss":true}', 'no cancel key, cancelable: Back dismisses');
ok(alertStartIndex([{ text: 'OK' }]) === 0 && J(alertBack([{ text: 'OK' }], false)) === '{"press":0}', 'a lone OK: start on it, Back presses it');
const del = [{ text: 'Delete', style: 'destructive' as const }, { text: 'Cancel', style: 'cancel' as const }];
ok(alertStartIndex(del) === 1, 'Delete / Cancel: starts on Cancel even when it comes second');
ok(J(alertBack([{ text: 'Reset', style: 'destructive' as const }], true)) === '{"dismiss":true}', 'a lone destructive key is never Back');
const three = [{ text: 'Cancel', style: 'cancel' as const }, { text: 'Remove', style: 'destructive' as const }, { text: 'Keep' }];
ok(alertStartIndex(three) === 2, 'three keys: starts on the plain one');
ok(alertMove(0, -1, 3) === 0 && alertMove(2, 1, 3) === 2 && alertMove(1, 1, 3) === 2, 'movement clamps at the ends');
ok(alertMove(0, 1, 0) === -1, 'no keys: nowhere');
process.exit(fails ? 1 : 0);
