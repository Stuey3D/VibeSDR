// One overlay rule (src/utils/overlayStack.ts) — controller audit root causes A and B, 2026-10-20.
import { OverlayStack } from '../src/utils/overlayStack';
import { KeyBus } from '../src/utils/keyActions';

let fails = 0;
const ok = (c: boolean, what: string) => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fails++; };

console.log('test_overlay_stack');
const st = new OverlayStack();
let counts: number[] = [];
const unsub = st.subscribe((n) => counts.push(n));
const a = {}, b = {}, c = {};
let backs: string[] = [];
st.push(a, { name: 'chooser', onBack: () => backs.push('a') });
ok(st.count() === 1 && st.isTop(a), 'one overlay: it is the top');
st.push(b, { name: 'tour', onBack: () => backs.push('b'), onEnter: () => backs.push('b-enter') });
ok(st.count() === 2 && st.isTop(b) && !st.isTop(a), 'the newest is the top');
ok(st.decide(a, 'Back', {}, false) === 'pass', 'an overlay beneath the top never acts');
ok(st.decide(b, 'Back', {}, false) === 'back', 'Back on the top runs its onBack');
ok(st.decide(b, 'Enter', {}, false) === 'enter', 'Enter on a card with one action runs it');
ok(st.decide(a, 'Enter', {}, false) === 'pass', 'Enter on a covered overlay: not ours');
ok(st.decide(b, 'ArrowDown', {}, false) === 'swallow', 'arrows are swallowed so nothing beneath moves');
ok(st.decide(b, 'TuneUp', {}, false) === 'pass', 'tune passes by default (works from anywhere)');
st.push(c, { name: 'card', blockTuneZoom: true });
ok(st.decide(c, 'ZoomIn', {}, false) === 'swallow', 'blockTuneZoom swallows tune and zoom');
ok(st.decide(c, 'Back', {}, false) === 'swallow' && st.decide(c, 'Enter', {}, false) === 'swallow', 'no handlers: swallowed, never passed beneath');
ok(st.decide(c, 'Back', { typing: true }, false) === 'pass' && st.decide(c, 'Enter', { typing: true }, false) === 'pass', 'a field\'s own Enter/Escape are the field\'s');
st.remove(c);
ok(st.isTop(b) && st.count() === 2, 'closing the top hands the keys to the one beneath');
// Suppressed (a third-party page): nothing, except Back where asked.
const br = {};
st.push(br, { name: 'browser', onBack: () => backs.push('br'), backWhenSuppressed: true });
ok(st.decide(br, 'Back', {}, true) === 'back', 'BrowserOverlay: Back still closes it while shortcuts are off');
ok(st.decide(br, 'ArrowUp', {}, true) === 'pass' && st.decide(br, 'Enter', {}, true) === 'pass', 'everything else stays the page\'s');
st.remove(br);
ok(st.decide(b, 'Back', {}, true) === 'pass', 'an ordinary overlay is silent while a third-party page is up');
// Re-push moves to the top without duplicating.
st.push(a, { name: 'chooser', onBack: () => backs.push('a2') });
ok(st.count() === 2 && st.isTop(a), 're-push: to the top, no duplicate');
st.update(a, { name: 'chooser', onBack: () => backs.push('a3') });
ok(st.isTop(a), 'update keeps the place');
// Levels: a tour (level 1) stays on top of the menu it opens behind itself.
const st3 = new OverlayStack();
const tour = {}, menu = {};
st3.push(tour, { name: 'tour', level: 1, onEnter: () => {} });
st3.push(menu, { name: 'menu', onBack: () => {} });
ok(st3.isTop(tour) && !st3.isTop(menu), 'a level-1 tour stays on top of a menu opened after it');
st3.remove(tour);
ok(st3.isTop(menu), 'tour gone: the menu is the top');
ok(counts.length > 0 && counts[counts.length - 1] === 2, 'subscribers hear the count');
unsub();

// ── End to end on the real bus: a panel at priority 0 first, the overlay at -0.5, the screen at -1. ──
const bus = new KeyBus();
const st2 = new OverlayStack();
const tok = {};
const log: string[] = [];
let subPage = true;
st2.push(tok, { onBack: () => log.push('overlay-back') });
const overlayListener = (e: any) => {
  const d = st2.decide(tok, e.key, e, false);
  if (d === 'back') { log.push('overlay-back-run'); return true; }
  if (d === 'enter' || d === 'swallow') return true;
};
bus.add('down', (e) => { log.push('screen:' + e.key); return true; }, -1);
bus.add('down', overlayListener, -0.5);
bus.add('down', (e) => { if (e.key === 'Back' && subPage) { subPage = false; log.push('panel-peel'); return true; } if (e.key === 'Enter') { log.push('panel-enter'); return true; } }, 0);
bus.dispatch('down', { key: 'Escape' });
bus.dispatch('down', { key: 'Escape' });
bus.dispatch('down', { key: 'ArrowDown' });
bus.dispatch('down', { key: 'Enter' });
bus.dispatch('down', { key: '>' });
ok(log.join(',') === 'panel-peel,overlay-back-run,panel-enter,screen:TuneUp',
   'Back peels the panel, then closes the overlay; arrows never reach the screen; Enter is the panel\'s; tune passes (' + log.join(',') + ')');
st2.remove(tok);
log.length = 0;
bus.dispatch('down', { key: 'ArrowDown' });
ok(log.join(',') === 'screen:ArrowDown', 'overlay gone: the screen has the arrows again');
process.exit(fails ? 1 : 0);
