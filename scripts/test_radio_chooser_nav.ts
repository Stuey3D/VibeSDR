// The radio chooser by pad and keyboard (src/utils/radioChooserNav.ts) — controller audit §1, 2026-10-20.
import { chooserItems, chooserRowOf, chooserStart, chooserActivate, type ChooserRadio } from '../src/utils/radioChooserNav';

let fails = 0;
const ok = (c: boolean, what: string) => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fails++; };
const R = (id: string, o: Partial<ChooserRadio> = {}): ChooserRadio => {
  const r = { id, busy: false, unsupported: false, gated: false, inUseApp: false, blocked: false, ...o };
  r.blocked = o.blocked ?? (r.busy || r.unsupported || r.gated || r.inUseApp);
  return r;
};

console.log('test_radio_chooser_nav');
const radios = [R('a', { inUseApp: true }), R('b', { gated: true }), R('c', { busy: true }), R('d'), R('e', { unsupported: true })];
const items = chooserItems(radios, true);
ok(items.map((x) => x.k).join(',') === 'back,radio,radio,radio,radio,radio,pin,pinGo,admin,adminGo', 'order: back, radios, PIN row, admin row');
ok(chooserItems(radios, false).every((x) => x.k !== 'pin' && x.k !== 'pinGo'), 'no PIN row when nothing is PIN-locked');
ok(chooserRowOf(items, 0) === 0 && chooserRowOf(items, 1) === 1 && chooserRowOf(items, 5) === 5, 'back and each radio a row of its own');
ok(chooserRowOf(items, 6) === chooserRowOf(items, 7), 'PIN field and its UNLOCK share a row');
ok(chooserRowOf(items, 8) === chooserRowOf(items, 9) && chooserRowOf(items, 8) === chooserRowOf(items, 6) + 1, 'admin field and its UNLOCK share the next row');
ok(chooserStart(items, radios) === 4, 'starts on the first radio that can be chosen (d)');
ok(chooserStart(chooserItems([R('x', { busy: true })], false), [R('x', { busy: true })]) === 1, 'all blocked: starts on the first radio, not ‹ SERVERS');
ok(chooserStart(chooserItems([], false), []) === 0, 'no radios: ‹ SERVERS');
const act = (i: number) => chooserActivate(items, radios, i);
ok(act(0).do === 'back', 'Enter on ‹ SERVERS goes back');
ok(JSON.stringify(act(4)) === '{"do":"choose","id":"d"}', 'Enter on a free radio chooses it');
ok(JSON.stringify(act(2)) === '{"do":"focus","index":6}', 'Enter on a PIN-locked radio moves to the PIN field');
ok(JSON.stringify(act(3)) === '{"do":"focus","index":8}', 'Enter on an occupied radio moves to the admin field');
ok(act(1).do === 'none' && act(5).do === 'none', 'in use by another app / unsupported: nothing opens them, Enter does nothing');
ok(JSON.stringify(act(6)) === '{"do":"field","which":"pin"}' && JSON.stringify(act(9)) === '{"do":"unlock","which":"admin"}', 'fields and UNLOCKs');
const noPin = chooserItems(radios, false);
ok(chooserActivate(noPin, radios, 2).do === 'none', 'PIN-locked radio with no PIN row on screen: nothing to move to');
ok(chooserActivate(items, radios, 99).do === 'none', 'out of range: nothing');
process.exit(fails ? 1 : 0);
