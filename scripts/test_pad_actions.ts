// Game controllers in the app (src/utils/padActions.ts) — Stuart's mapping, fed into the keyboard's own keys.
import { PadMapper, PAD_BIT, padKey, SWITCH_HOLD_MS, kbPadAction, type PadOut } from '../src/utils/padActions';
import { toHwKey } from '../src/utils/keyActions';

let fails = 0;
const ok = (c: boolean, what: string) => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fails++; };
const keys = (o: PadOut[]) => o.filter((x): x is { kind: 'down' | 'up'; key: string } => 'kind' in x).map((x) => `${x.kind}:${x.key}`).join(' ');
const action = (k: string | null) => (k ? toHwKey({ key: k })?.key ?? null : null);

console.log('test_pad_actions');

// ── Every button lands on the keyboard key Stuart's scheme gives it ──
const want: Record<string, [string | null, string | null]> = {   // button: [buttons profile, sticks profile]
  up: ['ArrowUp', 'ArrowUp'], down: ['ArrowDown', 'ArrowDown'], left: ['ArrowLeft', 'ArrowLeft'], right: ['ArrowRight', 'ArrowRight'],
  r1: ['Enter', null], l1: ['Back', null],
  north: ['ZoomIn', null], south: ['ZoomOut', 'Enter'], west: ['TuneDown', null], east: ['TuneUp', 'Back'],
  start: [null, null], select: [null, null],
};
for (const [b, [bp, sp]] of Object.entries(want)) {
  ok(action(padKey(b as any, 'buttons')) === bp, `buttons: ${b} -> ${bp}`);
  ok(action(padKey(b as any, 'sticks')) === sp, `sticks: ${b} -> ${sp}`);
}

// ── Profile choice: sticks decide until START has chosen ──
{
  const m = new PadMapper(null);
  ok(m.profileOf(true) === 'sticks' && m.profileOf(false) === 'buttons', 'no choice: a pad with sticks gets sticks, one without gets buttons');
  m.setChosen('buttons');
  ok(m.profileOf(true) === 'buttons', 'a remembered choice wins over the pad');
}

// ── Press / release ──
{
  const m = new PadMapper(null);
  ok(keys(m.feed({ id: 'p', sticks: true, b: PAD_BIT.south }, 0)) === 'down:Enter', 'sticks: ✕ down is Enter down');
  ok(keys(m.feed({ id: 'p', sticks: true, b: 0 }, 50)) === 'up:Enter', '…and up is Enter up');
  ok(keys(m.feed({ id: 'p', sticks: true, b: PAD_BIT.right }, 60)) === 'down:ArrowRight', 'D-pad right');
  ok(keys(m.feed({ id: 'p', sticks: true, b: PAD_BIT.right }, 70)) === '', 'a repeat report of the same state sends nothing (PanelNav repeats a held key)');
  ok(keys(m.feed({ id: 'p', sticks: true, b: PAD_BIT.down }, 80)) === 'up:ArrowRight down:ArrowDown', 'a roll: up, then the new down');
  ok(keys(m.feed({ id: 'p', gone: true }, 90)) === 'up:ArrowDown', 'unplugged mid-press: the held key goes up');
  ok(keys(m.feed({ id: 'p', sticks: true, b: PAD_BIT.l1 | PAD_BIT.r1 }, 100)) === '', 'sticks: the shoulders do nothing');
}
{
  const m = new PadMapper(null);
  ok(keys(m.feed({ id: 'q', sticks: false, b: PAD_BIT.east }, 0)) === 'down:.', 'buttons: right face = tune up (held = the keys\' sweep)');
  ok(keys(m.feed({ id: 'q', sticks: false, b: PAD_BIT.east | PAD_BIT.r1 }, 10)) === 'down:Enter', 'buttons: R = Enter, with tune still held');
  ok(keys(m.feed({ id: 'q', sticks: false, b: 0 }, 20)) === 'up:. up:Enter', 'both released');
}

// ── HOLD START switches, never a tap ──
{
  const saved: string[] = [];
  const m = new PadMapper(null, (p) => saved.push(p));
  m.feed({ id: 'p', sticks: true, b: PAD_BIT.start }, 1000);
  ok(m.nextDeadline() === 1000 + SWITCH_HOLD_MS, 'START down: a deadline is armed');
  m.feed({ id: 'p', sticks: true, b: 0 }, 1300);
  ok(m.nextDeadline() === null && saved.length === 0 && m.tick(5000).length === 0, 'a TAP of START switches nothing');
  m.feed({ id: 'p', sticks: true, b: PAD_BIT.start | PAD_BIT.south }, 2000);   // ✕ held as well (Enter down)
  ok(m.tick(2000 + SWITCH_HOLD_MS - 1).length === 0, 'not before the hold time');
  const o = m.tick(2000 + SWITCH_HOLD_MS);
  const n = o.find((x) => 'notice' in x) as any;
  ok(!!n && n.profile === 'buttons' && /hold START to switch back/.test(n.notice), 'held: switches to buttons, the notice says how to switch back');
  ok(keys(o) === 'up:Enter', 'a button held across the switch goes up as what it went down as');
  ok(saved.join() === 'buttons', 'the choice is saved');
  ok(m.tick(9999).length === 0 && m.nextDeadline() === null, 'once per hold');
  ok(keys(m.feed({ id: 'p', sticks: true, b: PAD_BIT.start }, 10000)) === '', 'releasing ✕ after the switch sends nothing more');
  m.feed({ id: 'p', sticks: true, b: 0 }, 10100);
  ok(keys(m.feed({ id: 'p', sticks: true, b: PAD_BIT.south }, 10200)) === 'down:-', 'now ✕ (bottom) is zoom out, by position');
  m.feed({ id: 'p', sticks: true, b: 0 }, 10300);
  m.feed({ id: 'p', sticks: true, b: PAD_BIT.start }, 11000);
  const back = m.tick(11000 + SWITCH_HOLD_MS).find((x) => 'notice' in x) as any;
  ok(back?.profile === 'sticks' && saved.join() === 'buttons,sticks', 'held again: back to sticks');
}

// ── The jog wheels ──
{
  const m = new PadMapper('sticks');
  // ★ A ticking clock: 5° every 16 ms is ~310 °/s, a NORMAL turn (gamepadNav's jog follows the speed — a frozen clock
  //   reads as an infinitely fast spin).
  let clock = 1000;
  const ring = (stick: 'l' | 'r', fromDeg: number, toDeg: number, step = 5) => {
    const out: PadOut[] = [];
    const s = Math.sign(toDeg - fromDeg);
    for (let a = fromDeg; s > 0 ? a <= toDeg : a >= toDeg; a += s * step) {
      const x = Math.cos(a * Math.PI / 180), y = Math.sin(a * Math.PI / 180);   // y DOWN: growing angle = clockwise
      clock += 16;
      out.push(...m.feed(stick === 'l' ? { id: 'j', sticks: true, lx: x, ly: y } : { id: 'j', sticks: true, rx: x, ry: y }, clock));
    }
    m.feed({ id: 'j', sticks: true }, (clock += 16));   // back to the middle
    return keys(out);
  };
  const cw = ring('l', 0, 90);
  ok(cw === 'down:. up:. down:. up:. down:. up:.', `left stick clockwise a quarter turn = three tune-up taps (got ${cw})`);
  const acw = ring('l', 90, 0);
  ok(acw === 'down:, up:, down:, up:, down:, up:,', `anticlockwise = tune down (got ${acw})`);
  const z = ring('r', 0, 90);
  ok(z === 'down:+ up:+ down:+ up:+', `right stick clockwise = zoom in (got ${z})`);
  const wrap = ring('l', 150, 210);
  ok(wrap === 'down:. up:. down:. up:.', `passing through west is not a 360° jump (got ${wrap})`);
  ok(keys(m.feed({ id: 'j', sticks: true, lx: 0.2, ly: 0.1 }, 0)) === '', 'a stick near the middle never drifts the dial');
  const mb = new PadMapper('buttons');
  let out: PadOut[] = [];
  for (let a = 0; a <= 180; a += 5) out = out.concat(mb.feed({ id: 'k', sticks: true, lx: Math.cos(a * Math.PI / 180), ly: Math.sin(a * Math.PI / 180) }, 0));
  ok(keys(out) === '', 'buttons profile: the sticks do nothing');
}

// ── SELECT: the controller guide (2026-10-19) — once per press, with the profile in use; never a keyboard key ──
{
  const m = new PadMapper(null);
  ok(m.current() === 'sticks', 'no pad and no choice: the guide opens on two sticks');
  let o = m.feed({ id: 's', sticks: true, b: PAD_BIT.select }, 0);
  const g = o.find((x) => 'guide' in x) as any;
  ok(!!g && g.profile === 'sticks' && keys(o) === '', 'SELECT on a two-stick pad: the guide, on sticks, and no key');
  o = m.feed({ id: 's', sticks: true, b: PAD_BIT.select }, 50);
  ok(!o.some((x) => 'guide' in x), 'held: once per press, not repeated');
  m.feed({ id: 's', sticks: true, b: 0 }, 100);
  o = m.feed({ id: 's', sticks: true, b: PAD_BIT.select }, 150);
  ok(o.some((x) => 'guide' in x), 'pressed again: again (the guide toggles)');
  const mb = new PadMapper('buttons');
  ok(mb.current() === 'buttons', 'no pad, buttons chosen: the guide opens on buttons');
  const gb = mb.feed({ id: 't', sticks: true, b: PAD_BIT.select }, 0).find((x) => 'guide' in x) as any;
  ok(gb?.profile === 'buttons' && mb.current() === 'buttons', 'a remembered choice decides the tab');
}

console.log(fails ? `test_pad_actions: ${fails} FAILED` : 'test_pad_actions: all ok');
// ★★★ THE CONTROLLER KEYBOARD'S BUTTONS (Stuart, decided 2026-10-20).
{
  ok(kbPadAction('west', 'sticks', 'qwerty') === 'backspace' && kbPadAction('north', 'sticks', 'qwerty') === 'space'
     && kbPadAction('l1', 'sticks', 'qwerty') === 'shift' && kbPadAction('r1', 'sticks', 'qwerty') === 'symbols',
     'stick pads: west ⌫, north space, L1 shift, R1 symbols');
  ok(kbPadAction('l1', 'sticks', 'numeric') === 'unit' && kbPadAction('r1', 'sticks', 'numeric') === 'unit' && kbPadAction('north', 'sticks', 'numeric') === null,
     'stick pads on the numeric pad: L1 / R1 switch kHz / MHz; no space');
  ok((['west', 'north', 'l1', 'r1', 'south', 'east'] as const).every((b) => kbPadAction(b, 'buttons', 'qwerty') === null),
     'no-stick pads: no spare button — on-screen keys only (enter / back stay R1 / L1)');
  ok(padKey('south', 'sticks') === 'Enter' && padKey('east', 'sticks') === 'Escape' && padKey('r1', 'buttons') === 'Enter' && padKey('l1', 'buttons') === 'Escape',
     'enter and back stay each profile\'s own');
  const m = new PadMapper('sticks');
  const press = (b: number, t: number) => m.feed({ id: 'p', sticks: true, b }, t);
  press(0, 0);
  m.setKeyboardOpen(true);
  let o = press(PAD_BIT.west, 10);
  ok(o.some((x) => 'kbButton' in x && x.kbButton === 'west'), 'open: west reports a keyboard button');
  press(0, 20);
  o = [...press(PAD_BIT.start, 100), ...press(0, 300)];
  ok(o.some((x) => 'kbSubmit' in x), 'open: a START TAP submits');
  o = [...press(PAD_BIT.start, 1000), ...m.tick(1000 + SWITCH_HOLD_MS + 50), ...press(0, 1000 + SWITCH_HOLD_MS + 100)];
  ok(!o.some((x) => 'notice' in x) && !o.some((x) => 'kbSubmit' in x), 'open: HOLD START neither switches the controls nor submits');
  m.setKeyboardOpen(false);
  o = press(PAD_BIT.west, 5000);
  ok(!o.some((x) => 'kbButton' in x), 'closed: west means nothing again on a stick pad');
  press(0, 5010);
  o = [...press(PAD_BIT.start, 6000), ...press(0, 6100)];
  ok(!o.some((x) => 'kbSubmit' in x), 'closed: a START tap does nothing, as before');
  o = [...press(PAD_BIT.start, 7000), ...m.tick(7000 + SWITCH_HOLD_MS + 10)];
  ok(o.some((x) => 'notice' in x), 'closed: HOLD START still switches the controls');
}
process.exit(fails ? 1 : 0);
