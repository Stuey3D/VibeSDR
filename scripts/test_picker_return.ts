// ★★★ Back from a radio lands where you left (Stuart, 2026-10-27: "in the main app when you back out of a radio it
// always takes you back to the top level rather than back to the directory you were in").
// The pure parts: src/utils/pickerReturn.ts (the server list's place) and src/utils/doorReturn.ts (a front door's radio
// list), plus chooserStart's return target — and source checks that the wiring says what the comments claim.
import { readFileSync } from 'node:fs';
import { pickerPlace, newPickerOwner, restorableDir, groupsToRestore, planReturn, type PickerPlace } from '../src/utils/pickerReturn';
import { doorVisit, backToDoor, doorReturnParams } from '../src/utils/doorReturn';
import { chooserItems, chooserStart, type ChooserRadio } from '../src/utils/radioChooserNav';

let fails = 0;
const ok = (c: boolean, what: string) => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fails++; };
const J = JSON.stringify;

console.log('test_picker_return');

// ── The place: saved, handed over by a reset, taken once ─────────────────────────────────────────────────────────────
const A = newPickerOwner(), B = newPickerOwner(), C = newPickerOwner();
ok(A !== B && B !== C, 'every picker instance has its own owner id');
const place = (o: Partial<PickerPlace<{ url: string }>> = {}): PickerPlace<{ url: string }> => ({
  owner: A, dir: 'vibeserver', filter: 'pi', openGroups: ['GB', 'DE'], showRestricted: true,
  instances: [{ url: 'https://a' }, { url: 'https://b' }], scrollY: 840, anchor: 'https://b', anchorIndex: 7, ...o,
});
pickerPlace.clear();
pickerPlace.save(place());
ok(pickerPlace.take(C) === null, 'a picker that did not save the place (a link, the watch) does not get it');
ok(pickerPlace.peek() !== null, '…and asking does not spend it');
const adopted = pickerPlace.adopt(B);
ok(adopted?.owner === B && adopted.dir === 'vibeserver' && adopted.filter === 'pi', 'the picker a reset built adopts the place — directory and search');
ok(pickerPlace.take(A) === null, 'once handed over, the old picker cannot take it back');
const got = pickerPlace.take(B);
ok(got?.anchor === 'https://b' && J(got?.openGroups) === '["GB","DE"]', 'the new picker takes it on return: groups and the receiver left');
ok(pickerPlace.take(B) === null, 'a return happens once — the next focus is an ordinary one');
pickerPlace.save(place({ owner: A, dir: null }));
ok(pickerPlace.take(A)?.dir === null, 'a picker that stayed mounted takes its own place back (the chooser)');
pickerPlace.clear();
ok(pickerPlace.adopt(B) === null, 'nothing saved (a cold launch): nothing to adopt — the top, as before');

// ── The directory must still exist; the fallback is never above the picker ──────────────────────────────────────────
const KNOWN = ['vibeserver', 'ubersdr', 'kiwisdr'];
ok(restorableDir('ubersdr', KNOWN) === 'ubersdr', 'a directory still offered is restored');
ok(restorableDir('gone', KNOWN) === null, 'a directory no longer offered: the chooser');
ok(restorableDir(null, KNOWN) === null, 'the chooser stays the chooser');

// ── Groups: what was open, plus the receiver's own ──────────────────────────────────────────────────────────────────
ok(J(groupsToRestore(['GB'], 'GB')) === '["GB"]', 'its group already open: unchanged');
ok(J(groupsToRestore(['GB'], 'FR')) === '["GB","FR"]', 'its group collapsed since: opened, so it is on screen');
ok(J(groupsToRestore([], null)) === '[]', 'no receiver (a favourite, a discovered server): just what was open');

// ── Where the list goes, and what is lit ────────────────────────────────────────────────────────────────────────────
const urls = [null, 'https://x', 'https://y', null, 'https://a', 'https://b'];
const P = (anchor: string | null, anchorIndex: number, scrollY = 840) => ({ anchor, anchorIndex, scrollY });
ok(J(planReturn(P('https://b', 5), urls, true)) === '{"scroll":{"kind":"offset","y":840},"focus":5}',
   'a NEW picker, receiver where it was: the same offset, receiver lit');
ok(J(planReturn(P('https://b', 5), urls, false)) === '{"scroll":{"kind":"stay"},"focus":5}',
   'a picker that stayed mounted kept its scroll: leave it, light the receiver');
ok(J(planReturn(P('https://x', 4), urls, false)) === '{"scroll":{"kind":"index","index":1},"focus":1}',
   'the receiver MOVED (a Most-Used favourite rises when opened): centre it where it is now');
ok(J(planReturn(P('https://gone', 5), urls, true)) === '{"scroll":{"kind":"top"},"focus":-1}',
   'the receiver is GONE (unfavourited, directory changed): the top of THIS list — never the app\'s top level');
ok(J(planReturn(P('https://gone', 5), urls, false)) === '{"scroll":{"kind":"top"},"focus":-1}',
   '…on a mounted picker too');
ok(J(planReturn(P(null, -1, 300), urls, true)) === '{"scroll":{"kind":"offset","y":300},"focus":-1}',
   'no receiver to find (a discovered server, local hardware): the offset alone');
ok(J(planReturn(P(null, -1, 300), urls, false)) === '{"scroll":{"kind":"stay"},"focus":-1}', '…and on a mounted picker, nothing moves');
ok(J(planReturn(P(null, -1, -12), urls, true)) === '{"scroll":{"kind":"offset","y":0}}'.replace('}}', '},"focus":-1}'),
   'an overscrolled (negative) offset is restored as 0');
ok(planReturn(P('https://a', 4), [], true).scroll.kind === 'top', 'an empty list (nothing matches the search now): top');

// ── The front door: back to its radio list, with this visit's PINs ──────────────────────────────────────────────────
ok(backToDoor({ hasDoor: true, choseFromList: true, radioId: 'rsp' }), 'chosen on the door\'s list: back to that list');
ok(backToDoor({ hasDoor: true, choseFromList: true, radioId: '' }), 'a door of one with a landing message, chosen there: back to it');
ok(!backToDoor({ hasDoor: true, choseFromList: false, radioId: null }), 'a door of one adopted silently: straight to the servers');
ok(!backToDoor({ hasDoor: false, choseFromList: false, radioId: null }), 'no door at all (UberSDR, Kiwi, OWRX, /r/<id>): the servers');
doorVisit.stash({ base: 'https://door', radioId: 'rsp', pins: { rsp: '1234' }, adminAuthQ: 'vs_admin_ticket=t' });
ok(doorVisit.take('https://other') === null, 'a visit is never handed to another door');
ok(doorVisit.take('https://door') === null, '…and a mismatched ask drops it rather than leaving a PIN lying about');
const pins = { rsp: '1234' };
doorVisit.stash({ base: 'https://door', radioId: 'rsp', pins, adminAuthQ: '' });
pins.rsp = 'changed';
const v = doorVisit.take('https://door');
ok(v?.radioId === 'rsp' && v.pins.rsp === '1234', 'the same door gets the visit: the radio left and its PIN (a copy)');
ok(doorVisit.take('https://door') === null, 'taken once');

let gen = 41;
const params = { baseUrl: 'https://door', viewMode: 'default', localGen: 7, deepLink: true, initialFreq: 7074000,
                 initialMode: 'usb', initialZoom: 3, autoTakeover: true, autoAdminPw: 'x', authSuffix: '&a=1' };
const np = doorReturnParams(params, () => ++gen) as Record<string, unknown>;
ok(np.doorReturn === true && np.baseUrl === 'https://door' && np.authSuffix === '&a=1', 'the door again, same address and auth');
ok(!('deepLink' in np) && !('initialFreq' in np) && !('initialMode' in np) && !('initialZoom' in np),
   'a link\'s tune belonged to the first connect: not replayed on the radio chosen next');
ok(!('autoTakeover' in np), 'a takeover is one-shot: never carried back to the list');
ok(np.localGen === 42, 'a NEW local generation, so the screen left cannot stop the shim under its replacement');
ok(!('localGen' in doorReturnParams({ baseUrl: 'u' }, () => 99)), 'no local generation to renew: none invented');
ok((params as Record<string, unknown>).deepLink === true, 'the old screen\'s params are not mutated');

// ── The chooser starts on the radio left ────────────────────────────────────────────────────────────────────────────
const R = (id: string, blocked = false): ChooserRadio => ({ id, busy: blocked, unsupported: false, gated: false, inUseApp: false, blocked });
const radios = [R('a'), R('b', true), R('c')];
const items = chooserItems(radios, false);
ok(chooserStart(items, radios, 'c') === 3, 'back from radio c: the highlight starts on c');
ok(chooserStart(items, radios, 'b') === 2, '…even if it has since been taken (that is where the listener was)');
ok(chooserStart(items, radios, 'gone') === 1, 'a radio no longer listed: the usual first free radio');
ok(chooserStart(items, radios) === 1, 'no return: unchanged');

// ── The wiring says what the comments claim ─────────────────────────────────────────────────────────────────────────
const picker = readFileSync('src/screens/InstancePickerScreen.tsx', 'utf8');
const resetAt = picker.indexOf("{ name: 'InstancePicker', params: { noAutoConnect: true, restorePlace: true } }");
ok(resetAt > 0, 'connectVibeServer\'s reset asks the new picker to restore the place');
const saveAt = picker.lastIndexOf('savePlaceRef.current();', resetAt);
ok(saveAt > 0 && resetAt - saveAt < 600 && picker.slice(saveAt, resetAt).includes('handedOffRef.current = true'),
   '…having saved it just before, and marked it handed off (the old picker\'s unmount must not save over it)');
ok((picker.match(/restorePlace: true/g) ?? []).length === 1, 'nothing else in the picker builds a restoring picker');
ok(/anchorRef\.current = favIn\.url/.test(picker) && /anchorRef\.current = inst\.url/.test(picker),
   'a favourite\'s press and a directory row\'s press both mark the receiver to come back to');
for (const f of ['App.tsx', 'src/linking/useDeepLinks.ts', 'src/services/crashGuard.ts']) {
  ok(!readFileSync(f, 'utf8').includes('restorePlace: true'), `${f}: its resets land at the top, as before (no restorePlace)`);
}
const sdr = readFileSync('src/screens/SDRScreen.tsx', 'utf8');
const back = sdr.slice(sdr.indexOf('const onBackToPicker = useCallback'), sdr.indexOf('const onBackToPicker = useCallback') + 700);
ok(back.includes("navigation.replace('SDR', doorReturnParams(") && back.includes('navigation.goBack()'),
   'the menu\'s ← BACK: the door\'s list when chosen there, else the server list');
ok(/chooseRadio = useCallback\(async \(id: string\) => \{\s*chosenFromListRef\.current = id;/.test(sdr),
   'only a radio CHOSEN on the list (chooseRadio) arms the return to it');

process.exit(fails ? 1 : 0);
