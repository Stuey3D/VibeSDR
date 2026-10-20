/**
 * ★★★ GAME CONTROLLERS AND HANDHELDS (Stuart, 2026-10-10 — memory: gamepad_mapping). The receiver page driven by a
 * pad: a PS / Xbox controller on a computer, a Steam Deck, or an Anbernic / R36S-type console running the page in a
 * full-screen shell. The browser's Gamepad API, standard layout.
 *
 *  Pads WITHOUT analogue sticks — face buttons by POSITION:
 *    D-pad = move the highlight · R shoulder = enter · L shoulder = back
 *    top = zoom in · bottom = zoom out · left = tune down · right = tune up
 *  Pads WITH two analogue sticks:
 *    D-pad = move the highlight · ✕ (bottom) = enter · ○ (right) = back
 *    LEFT stick turned like a jog wheel = tune (clockwise forwards) · RIGHT stick turned = zoom (clockwise in) —
 *    the rate follows the speed, like the drum: slow turning is fine, a fast spin covers ground (gamepadNav Jog)
 *  SELECT opens and closes the CONTROLLER GUIDE (padGuide.ts — the pad drawn with what each control does, Stuart
 *  approved 2026-10-19); while it is open the D-pad left / right moves between its two tabs, up / down scroll it, and
 *  back closes it. It opens on the layout in use and follows a HOLD START switch.
 *  HOLD START (~1 s) switches between the two (Stuart: a pad with one stick may be reported as having two) — shown on
 *  screen and remembered on this device. ?pad=buttons|sticks forces one for a session.
 *  ★ HELD, never tapped (Stuart, 2026-10-18: "i pressed the l and r button by mistake and it switched … with no way
 *    back"): a pad whose browser numbering is not the standard layout can report a shoulder button as START, and a
 *    tap is too easy to make by accident. The toast always says how to switch back.
 *
 * ★★ THE DECODER BOX IS ONE PLACE, AS IN THE APP (Stuart, 2026-10-18: "move up to select it then it grabs the up down
 *    left and right with enter and backspace to select an item or to move the controls away from the decoder back to
 *    the main controls"). A [data-navbox] is a single stop for the highlight; enter on it hands it the D-pad (the
 *    highlight goes to its first [data-navstart] item, a DAB station), the D-pad then moves only inside it — up/down
 *    the list, left/right to the header's keys; with nothing selectable, up/down scroll its [data-navscroll] —
 *    enter presses, and back gives the D-pad back with the box itself highlighted. [data-navauto] (DAB, where the box
 *    IS the tuning) takes the D-pad by itself on the first press after it appears, once — back still releases it.
 *    The app's DecoderPanel does exactly this (enterBox / leave / autoOwn).
 * ★ Nothing runs until a pad is connected: no polling loop, no listener work, no cost to anyone without one.
 * ★ Enter on a slider or a drop-down ADJUSTS it: D-pad left/right changes the value, enter or back finishes.
 * ★★★ Enter on a TEXT BOX opens the CONTROLLER KEYBOARD (padKeyboard.ts — the app's layouts and rules, decided by
 *    Stuart 2026-10-20); while it is open every button goes to it (kbPoll). A pad press in a box somebody is typing in
 *    with the phone's keyboard or a real one hands it over with the text and caret; BACK in such a box lets go of it
 *    (the audit's "on the web, pad Back doesn't release a text field" — the highlight used to walk away while the box
 *    kept the cursor). Only a real press or a stick past STICK_USE counts as using the pad; a connected pad does not.
 */
import { pickNext, Jog, padProfile, type Box, type Dir } from '../../../src/utils/gamepadNav';
import { guideTabStep, type PadLayout } from '../../../src/utils/padGuide';
import { padGuideOpen, padGuideLayout, setPadGuideLayout, scrollPadGuide } from './padGuide';
import { isTextField, fieldEdited, openPadKeyboard, padKbOpen, padKbLayout, padKbMove, padKbPress, padKbBack, padKbEdit,
         padKbSubmit, padKbCheckField, onPadKbClose, notePadInput, inputSource } from './padKeyboard';

export interface GamepadHooks {
  tune(dir: 1 | -1): void;           // one step of the current tuning step
  zoom(dir: 1 | -1): void;           // one zoom step in (+1) or out (−1)
  back(): boolean;                   // close the top panel/menu; false if there was nothing to close
  guide(): void;                     // SELECT: open the controller guide on activePadLayout(), or close it
}

// Standard-layout button indices.
const B_SOUTH = 0, B_EAST = 1, B_WEST = 2, B_NORTH = 3, B_L1 = 4, B_R1 = 5, B_SELECT = 8, B_START = 9;
const PROFILE_KEY = 'vibe.padProfile';
const B_UP = 12, B_DOWN = 13, B_LEFT = 14, B_RIGHT = 15;
const FOCUSABLE = 'button, input:not([type=hidden]), select, a[href], [role="button"], [tabindex]:not([tabindex="-1"])';
const NAVBOX = '[data-navbox]';
const BOX_SCROLL = 90;                          // px the D-pad scrolls a box with nothing selectable (the app's step)
const REPEAT_DELAY = 380, REPEAT_EVERY = 110;
const SWITCH_HOLD_MS = 900;                     // START held this long switches the controls
// ★★ WHAT COUNTS AS USING THE PAD (Stuart, 2026-10-20: "only show the on-screen keyboard when the web client is actually
//    being driven by a control pad"): a button going DOWN, or a stick pushed past this — never a pad that is merely
//    connected, and never drift or axis noise. The jog itself engages at 0.6 (gamepadNav Jog).
const STICK_USE = 0.5;

let hooks: GamepadHooks | null = null;
let running = false;
let current: HTMLElement | null = null;
// ★ Where the highlight last stood. A panel that redraws itself (the admin panel's tables, every 2 s) replaces the
//   highlighted button with a new one; the next press carries on from here instead of jumping to the top.
let lastBox: Box | null = null;
let adjusting = false;
let grabbed: HTMLElement | null = null;         // the [data-navbox] that has the D-pad, if any
let autoTaken = '';                             // the data-navauto token already taken (once per appearance)
const held = new Map<string, number>();        // action → when it next repeats (ms)
const tuneJog = new Jog(30), zoomJog = new Jog(45);
const lastButtons = new Map<number, boolean[]>();   // per pad: what was down last frame (to see a press, not a hold)
const lastSticks = new Map<number, boolean[]>();    // per pad: which sticks were past STICK_USE last frame
// ★ Buttons that were down when the keyboard opened or closed: ignored until let go, so the press that opened it never
//   also types a key, and the ↵ that closed it never also presses the field again (which would reopen it).
const suppressed = new Set<string>();
function suppressHeld() {
  // ★ And forget every hold in progress: a button let go while the other side had the pad never told press() so, and a
  //   one-shot (enter, back) would otherwise stay "held" for ever and never fire again.
  held.clear();
  const pads = navigator.getGamepads ? [...navigator.getGamepads()].filter(Boolean) as Gamepad[] : [];
  for (const gp of pads) gp.buttons.forEach((b, i) => { if (b?.pressed) suppressed.add(`${gp.index}:${i}`); });
}

function visible(el: HTMLElement): boolean {
  if ((el as HTMLButtonElement).disabled || el.hidden) return false;
  const r = el.getBoundingClientRect();
  if (r.width < 2 || r.height < 2) return false;
  const cs = getComputedStyle(el);
  return cs.visibility !== 'hidden' && cs.display !== 'none' && cs.pointerEvents !== 'none';
}

/** What the highlight moves within: the admin panel or the START screen, else the top open panel or menu, else the whole page. */
function scope(): HTMLElement {
  if (grabbed && !(grabbed.isConnected && visible(grabbed))) release(false);   // it closed under us
  // ★ The admin panel first: it is drawn ABOVE the splash (where an owner most often opens it) and is shown by its
  //   `hidden` attribute, not .open — so the pad stayed on the page underneath (Stuart, 2026-10-18: "in the admin menu
  //   there is no way to navigate it with the controller").
  for (const id of ['adminPanel', 'audioGate', 'splash']) {
    const el = document.getElementById(id);
    if (el && visible(el)) return el;
  }
  // ★ A [data-navbox] is .open but DOCKED beside the radio, not over it (the decoder box) — never a scope of its own
  //   until enter hands it the D-pad (see the header).
  const open = [...document.querySelectorAll<HTMLElement>('.open:not([data-navbox])')].filter(visible);
  if (open.length) {
    // The one drawn on top: highest z-index, later in the page on a tie.
    return open.reduce((a, b) => ((+getComputedStyle(b).zIndex || 0) >= (+getComputedStyle(a).zIndex || 0) ? b : a));
  }
  // ★ A box that has the D-pad comes AFTER the panels and pop-ups: one opened from inside it (a header key's menu) or by
  //   the mouse over it is on top, and takes the D-pad until it closes.
  if (grabbed) return grabbed;
  return document.body;
}

function candidates(root: HTMLElement): HTMLElement[] {
  const all = [...root.querySelectorAll<HTMLElement>(FOCUSABLE)].filter(visible);
  if (root === grabbed) return all;
  // Outside a box, the box is ONE stop and what is inside it is not on the route.
  const boxes = [...root.querySelectorAll<HTMLElement>(NAVBOX)].filter(visible);
  return [...all.filter((el) => !boxes.some((b) => b.contains(el))), ...boxes];
}

/** Enter on a box: it takes the D-pad, the highlight goes to its first list item (else its first key). */
function grab(b: HTMLElement) {
  grabbed = b;
  b.classList.add('gpGrab');
  const list = candidates(b);
  const start = b.dataset.navstart;
  highlight((start && list.find((el) => el.matches(start))) || list[0] || null);
}

/** Back inside a box: the D-pad goes back to the page, the box itself highlighted. */
function release(highlightBox = true) {
  const b = grabbed;
  grabbed = null;
  b?.classList.remove('gpGrab');
  highlight(highlightBox && b && b.isConnected && visible(b) ? b : null);
}

/** DAB: the box takes the D-pad by itself on the first press after it appears (and only then). */
function autoGrab(): boolean {
  const b = document.querySelector<HTMLElement>('[data-navbox][data-navauto]');
  const token = b?.dataset.navauto || '';
  if (!b || !token || grabbed || token === autoTaken || !visible(b) || scope() !== document.body) return false;
  autoTaken = token;
  grab(b);
  return true;
}

function box(el: HTMLElement): Box { const r = el.getBoundingClientRect(); return { x: r.left, y: r.top, w: r.width, h: r.height }; }

function highlight(el: HTMLElement | null) {
  current?.classList.remove('gpFocus', 'gpAdjust');
  current = el;
  adjusting = false;
  if (el) { el.classList.add('gpFocus'); el.scrollIntoView({ block: 'nearest', inline: 'nearest' }); lastBox = box(el); }
}

function move(dir: Dir) {
  // ★ The guide is a picture, not a form: left / right are its two tabs, up / down scroll it (a phone shows the list
  //   below the pad). Checked first — it is drawn over everything, a grabbed decoder box included.
  if (padGuideOpen()) {
    if (dir === 'left' || dir === 'right') setPadGuideLayout(guideTabStep(padGuideLayout(), dir === 'right' ? 1 : -1));
    else scrollPadGuide(dir === 'down' ? 1 : -1);
    return;
  }
  if (autoGrab()) return;
  const root = scope();
  if (adjusting && current && (dir === 'left' || dir === 'right')) { adjust(dir === 'right' ? 1 : -1); return; }
  const list = candidates(root);
  const cur = current && root.contains(current) && visible(current) ? current : null;
  const from = cur ? box(cur) : current && !current.isConnected ? lastBox : null;   // redrawn under the highlight
  const i = pickNext(from, list.map(box), dir);
  if (i >= 0) highlight(list[i]);
  else if (!from && list.length) highlight(list[0]);
  else if (root === grabbed && (dir === 'up' || dir === 'down')) {
    // Nothing further that way inside the box: scroll it (a text decoder, spots — "nothing to select, just scroll").
    const sc = root.querySelector<HTMLElement>('[data-navscroll]') ?? root;
    sc.scrollTop += dir === 'down' ? BOX_SCROLL : -BOX_SCROLL;
  }
}

function adjust(d: 1 | -1) {
  const el = current;
  if (el instanceof HTMLInputElement && el.type === 'range') {
    d > 0 ? el.stepUp() : el.stepDown();
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
  } else if (el instanceof HTMLSelectElement) {
    const n = Math.max(0, Math.min(el.options.length - 1, el.selectedIndex + d));
    if (n !== el.selectedIndex) { el.selectedIndex = n; el.dispatchEvent(new Event('change', { bubbles: true })); }
  }
}

function enter() {
  if (padGuideOpen()) return;                     // nothing on it to press — the D-pad has the tabs
  if (autoGrab()) return;
  const root = scope();
  if (!current || !root.contains(current) || !visible(current)) { move('down'); return; }
  const el = current;
  if (el !== grabbed && el.matches(NAVBOX)) { grab(el); return; }
  if ((el instanceof HTMLInputElement && el.type === 'range') || el instanceof HTMLSelectElement) {
    adjusting = !adjusting;
    el.classList.toggle('gpAdjust', adjusting);
    return;
  }
  // ★★ A TEXT BOX: the controller keyboard (padKeyboard.ts) — never el.focus(), which on a phone pops up a keyboard
  //    the pad cannot drive, and on a handheld kiosk gives the cursor to a box nothing can type into.
  if (isTextField(el)) { openPadKeyboard(el); suppressHeld(); return; }
  el.click();
}

function back() {
  if (padGuideOpen()) { hooks?.back(); return; }   // closes the guide first, whatever is grabbed underneath
  if (adjusting && current) { adjusting = false; current.classList.remove('gpAdjust'); return; }
  if (grabbed) { release(); return; }
  if (hooks?.back()) highlight(null);
}

/** Press, hold-to-repeat: fires on the press, then after a pause, then steadily while held. */
function press(action: string, down: boolean, fire: () => void, repeat = true) {
  const now = performance.now();
  if (!down) { held.delete(action); return; }
  const next = held.get(action);
  // ★ Recorded BEFORE it fires: a press that opens or closes the controller keyboard clears every hold (suppressHeld),
  //   and must not be written back afterwards as a hold that never ends.
  if (next === undefined) { held.set(action, repeat ? now + REPEAT_DELAY : Infinity); fire(); return; }
  if (now >= next) { held.set(action, now + REPEAT_EVERY); fire(); }
}

/** The choice: ?pad= for this session, else what START last chose on this device, else decided from the pad. */
let chosen: string | null = (() => {
  try { const u = new URLSearchParams(location.search).get('pad'); if (u) return u; } catch { /* no URL */ }
  try { return localStorage.getItem(PROFILE_KEY); } catch { return null; }
})();
function forcedProfile(): string | null { return chosen; }

/** The layout in use now — what the guide opens on: the first connected pad's, else what START chose, else sticks. */
export function activePadLayout(): PadLayout {
  const gp = navigator.getGamepads ? [...navigator.getGamepads()].find(Boolean) : null;
  return gp ? padProfile(gp.axes.length, chosen) : chosen === 'buttons' ? 'buttons' : 'sticks';
}

function toggleProfile(axes: number) {
  chosen = padProfile(axes, chosen) === 'sticks' ? 'buttons' : 'sticks';
  try { localStorage.setItem(PROFILE_KEY, chosen); } catch { /* private window: this session only */ }
  setPadGuideLayout(chosen as PadLayout);         // the guide, if open, follows the switch
  toast((chosen === 'sticks' ? 'Controls: two sticks — turn the left stick to tune, the right to zoom'
                             : 'Controls: buttons by position — face buttons tune and zoom, R enter, L back')
        + ' · hold START to switch back');
}

function toast(text: string) {
  document.getElementById('gpToast')?.remove();
  const t = document.createElement('div');
  t.id = 'gpToast';
  t.textContent = text;
  t.style.cssText = 'position:fixed;left:50%;bottom:24px;transform:translateX(-50%);z-index:9600;max-width:90vw;' +
    'background:var(--ov-warn,rgba(40,10,0,0.94));color:#ffb833;border:1px solid rgba(255,120,0,0.6);border-radius:8px;' +
    'padding:8px 14px;font:13px ui-monospace,monospace;text-align:center;pointer-events:none';
  document.body.appendChild(t);
  setTimeout(() => t.remove(), 2600);
}

function poll() {
  if (!running) return;
  const pads = navigator.getGamepads ? [...navigator.getGamepads()].filter(Boolean) as Gamepad[] : [];
  if (!pads.length) { running = false; highlight(null); return; }
  padKbCheckField();
  for (const gp of pads) {
    const prof = padProfile(gp.axes.length, forcedProfile());
    const id = `${gp.index}:`;
    const down = gp.buttons.map((b) => !!b?.pressed);
    const was = lastButtons.get(gp.index) ?? [];
    lastButtons.set(gp.index, down);
    const rising = down.map((d, i) => d && !was[i]);
    const sticks = [Math.hypot(gp.axes[0] ?? 0, gp.axes[1] ?? 0) > STICK_USE, Math.hypot(gp.axes[2] ?? 0, gp.axes[3] ?? 0) > STICK_USE];
    const sWas = lastSticks.get(gp.index) ?? [false, false];
    lastSticks.set(gp.index, sticks);
    if (rising.some(Boolean) || sticks.some((v, i) => v && !sWas[i])) notePadInput();
    // ★★ A PAD PRESS WHILE A TEXT BOX HAS THE CURSOR (kbHandover: system / in place + pad).
    if (rising.some(Boolean) && padWhileTyping(rising[prof === 'buttons' ? B_L1 : B_EAST])) suppressHeld();
    const btn = (i: number) => {
      const k = id + i;
      if (suppressed.has(k)) { if (!down[i]) suppressed.delete(k); return false; }
      return down[i];
    };
    if (padKbOpen()) { kbPoll(gp, prof, id, btn); continue; }
    // HOLD to switch: fires once when START has been down SWITCH_HOLD_MS, never on a tap.
    if (btn(B_START)) {
      const since = held.get(id + 'start');
      if (since === undefined) held.set(id + 'start', performance.now());
      else if (since > 0 && performance.now() - since >= SWITCH_HOLD_MS) {
        toggleProfile(gp.axes.length); held.set(id + 'start', -1);   // -1: done until released
      }
    } else held.delete(id + 'start');
    press(id + 'select', btn(B_SELECT), () => hooks?.guide(), false);
    press(id + 'up', btn(B_UP), () => move('up'));
    press(id + 'down', btn(B_DOWN), () => move('down'));
    press(id + 'left', btn(B_LEFT), () => move('left'));
    press(id + 'right', btn(B_RIGHT), () => move('right'));
    if (prof === 'buttons') {
      press(id + 'enter', btn(B_R1), enter, false);
      press(id + 'back', btn(B_L1), back, false);
      press(id + 'zin', btn(B_NORTH), () => hooks?.zoom(1));
      press(id + 'zout', btn(B_SOUTH), () => hooks?.zoom(-1));
      press(id + 'tdn', btn(B_WEST), () => hooks?.tune(-1));
      press(id + 'tup', btn(B_EAST), () => hooks?.tune(1));
    } else {
      press(id + 'enter', btn(B_SOUTH), enter, false);
      press(id + 'back', btn(B_EAST), back, false);
      // ★ With the time, so the jog's rate follows the stick's speed — slow to land, fast to cross (gamepadNav Jog).
      const now = performance.now();
      const t = tuneJog.feed(gp.axes[0] ?? 0, gp.axes[1] ?? 0, now);
      for (let k = 0; k < Math.abs(t); k++) hooks?.tune(t > 0 ? 1 : -1);
      const z = zoomJog.feed(gp.axes[2] ?? 0, gp.axes[3] ?? 0, now);
      for (let k = 0; k < Math.abs(z); k++) hooks?.zoom(z > 0 ? 1 : -1);
    }
  }
  requestAnimationFrame(poll);
}

/** A pad press while a text box has the cursor. Back lets go of the box (the highlight stays on it); any other press in
 *  a box somebody was typing in hands it to the controller keyboard with the text and caret; a box that took the cursor
 *  by itself (autofocus) is only let go, and the press then does what it always does. True = the press is used up. */
function padWhileTyping(isBack: boolean): boolean {
  const el = document.activeElement;
  if (!isTextField(el) || padKbOpen()) return false;
  if (isBack) { el.blur(); highlight(el); return true; }
  if (fieldEdited(el)) {
    let caret: number | undefined;
    try { caret = el.selectionStart ?? undefined; } catch { /* no caret on this type */ }
    highlight(el);
    openPadKeyboard(el, caret);
    return true;
  }
  el.blur();
  highlight(el);
  return false;
}

/** The pad while the controller keyboard is open: every button goes to it, nothing reaches tuning, zoom or panels. */
function kbPoll(gp: Gamepad, prof: string, id: string, btn: (i: number) => boolean) {
  // START: a TAP submits (released before the hold time); a HOLD is ignored while open — switching the controls
  // mid-word would change what every button means.
  if (btn(B_START)) { if (!held.has(id + 'kbstart')) held.set(id + 'kbstart', performance.now()); }
  else {
    const t = held.get(id + 'kbstart');
    held.delete(id + 'kbstart');
    if (t !== undefined && performance.now() - t < SWITCH_HOLD_MS) { padKbSubmit(); return; }
  }
  press(id + 'kbup', btn(B_UP), () => padKbMove('up'));
  press(id + 'kbdown', btn(B_DOWN), () => padKbMove('down'));
  press(id + 'kbleft', btn(B_LEFT), () => padKbMove('left'));
  press(id + 'kbright', btn(B_RIGHT), () => padKbMove('right'));
  const now = performance.now();
  if (prof === 'buttons') {
    press(id + 'kbpress', btn(B_R1), padKbPress, false);
    press(id + 'kbback', btn(B_L1), padKbBack, false);
    press(id + 'kbcl', btn(B_WEST), () => padKbEdit('left'));      // the tune buttons move the caret
    press(id + 'kbcr', btn(B_EAST), () => padKbEdit('right'));
  } else {
    const numeric = padKbLayout() === 'numeric';
    press(id + 'kbpress', btn(B_SOUTH), padKbPress, false);
    press(id + 'kbback', btn(B_EAST), padKbBack, false);
    press(id + 'kbbs', btn(B_WEST), () => padKbEdit('bs'));
    press(id + 'kbsp', btn(B_NORTH), () => padKbEdit('space'));
    press(id + 'kbl1', btn(B_L1), () => padKbEdit(numeric ? 'unit' : 'shift'), false);
    press(id + 'kbr1', btn(B_R1), () => padKbEdit(numeric ? 'unit' : 'symbols'), false);
    // The left stick turned (the tune action) moves the caret; the right stick (zoom) does nothing here.
    const t = tuneJog.feed(gp.axes[0] ?? 0, gp.axes[1] ?? 0, now);
    for (let k = 0; k < Math.abs(t); k++) padKbEdit(t > 0 ? 'right' : 'left');
    zoomJog.feed(gp.axes[2] ?? 0, gp.axes[3] ?? 0, now);
  }
}

export function initGamepad(h: GamepadHooks): void {
  hooks = h;
  // When the keyboard closes by ↵ or Back, the highlight goes back to the box it was typing into.
  onPadKbClose((el, why) => { suppressHeld(); if (why !== 'handover' && el.isConnected && visible(el)) highlight(el); });
  // ★ A box that takes the cursor BY ITSELF while the pad is driving (a panel that focuses its field on opening): let
  //   go of it and highlight it instead — enter on it opens the controller keyboard. Otherwise a phone would pop up
  //   its own keyboard, and a kiosk would leave the cursor in a box nothing can type into.
  document.addEventListener('focusin', (e) => {
    if (!isTextField(e.target) || padKbOpen() || inputSource.get() !== 'pad') return;
    const el = e.target as HTMLElement;
    el.blur();
    if (running) highlight(el);
  });
  const start = () => { if (!running) { running = true; requestAnimationFrame(poll); } };
  window.addEventListener('gamepadconnected', start);
  // ★ A pad already connected before the page loaded announces itself only on its first press — poll once to see.
  if (navigator.getGamepads && [...navigator.getGamepads()].some(Boolean)) start();
}
