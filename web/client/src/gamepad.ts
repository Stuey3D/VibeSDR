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
 *    LEFT stick turned like a jog wheel = tune (clockwise forwards) · RIGHT stick turned = zoom (clockwise in)
 *  START switches between the two (Stuart: a pad with one stick may be reported as having two) — shown on screen and
 *  remembered on this device. ?pad=buttons|sticks forces one for a session.
 *
 * ★ Nothing runs until a pad is connected: no polling loop, no listener work, no cost to anyone without one.
 * ★ Enter on a slider or a drop-down ADJUSTS it: D-pad left/right changes the value, enter or back finishes.
 */
import { pickNext, Jog, padProfile, type Box, type Dir } from '../../../src/utils/gamepadNav';

export interface GamepadHooks {
  tune(dir: 1 | -1): void;           // one step of the current tuning step
  zoom(dir: 1 | -1): void;           // one zoom step in (+1) or out (−1)
  back(): boolean;                   // close the top panel/menu; false if there was nothing to close
}

// Standard-layout button indices.
const B_SOUTH = 0, B_EAST = 1, B_WEST = 2, B_NORTH = 3, B_L1 = 4, B_R1 = 5, B_START = 9;
const PROFILE_KEY = 'vibe.padProfile';
const B_UP = 12, B_DOWN = 13, B_LEFT = 14, B_RIGHT = 15;
const FOCUSABLE = 'button, input:not([type=hidden]), select, a[href], [role="button"], [tabindex]:not([tabindex="-1"])';
const REPEAT_DELAY = 380, REPEAT_EVERY = 110;

let hooks: GamepadHooks | null = null;
let running = false;
let current: HTMLElement | null = null;
let adjusting = false;
const held = new Map<string, number>();        // action → when it next repeats (ms)
const tuneJog = new Jog(30), zoomJog = new Jog(45);

function visible(el: HTMLElement): boolean {
  if ((el as HTMLButtonElement).disabled || el.hidden) return false;
  const r = el.getBoundingClientRect();
  if (r.width < 2 || r.height < 2) return false;
  const cs = getComputedStyle(el);
  return cs.visibility !== 'hidden' && cs.display !== 'none' && cs.pointerEvents !== 'none';
}

/** What the highlight moves within: the START screen, else the top open panel or menu, else the whole page. */
function scope(): HTMLElement {
  for (const id of ['audioGate', 'splash']) {
    const el = document.getElementById(id);
    if (el && visible(el)) return el;
  }
  const open = [...document.querySelectorAll<HTMLElement>('.open')].filter(visible);
  if (open.length) {
    // The one drawn on top: highest z-index, later in the page on a tie.
    return open.reduce((a, b) => ((+getComputedStyle(b).zIndex || 0) >= (+getComputedStyle(a).zIndex || 0) ? b : a));
  }
  return document.body;
}

function candidates(root: HTMLElement): HTMLElement[] {
  return [...root.querySelectorAll<HTMLElement>(FOCUSABLE)].filter(visible);
}

function box(el: HTMLElement): Box { const r = el.getBoundingClientRect(); return { x: r.left, y: r.top, w: r.width, h: r.height }; }

function highlight(el: HTMLElement | null) {
  current?.classList.remove('gpFocus', 'gpAdjust');
  current = el;
  adjusting = false;
  if (el) { el.classList.add('gpFocus'); el.scrollIntoView({ block: 'nearest', inline: 'nearest' }); }
}

function move(dir: Dir) {
  const root = scope();
  if (adjusting && current && (dir === 'left' || dir === 'right')) { adjust(dir === 'right' ? 1 : -1); return; }
  const list = candidates(root);
  const cur = current && root.contains(current) && visible(current) ? current : null;
  const i = pickNext(cur ? box(cur) : null, list.map(box), dir);
  if (i >= 0) highlight(list[i]);
  else if (!cur && list.length) highlight(list[0]);
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
  const root = scope();
  if (!current || !root.contains(current) || !visible(current)) { move('down'); return; }
  const el = current;
  if ((el instanceof HTMLInputElement && el.type === 'range') || el instanceof HTMLSelectElement) {
    adjusting = !adjusting;
    el.classList.toggle('gpAdjust', adjusting);
    return;
  }
  if (el instanceof HTMLInputElement && !['button', 'submit', 'checkbox', 'radio'].includes(el.type)) { el.focus(); return; }
  el.click();
}

function back() {
  if (adjusting && current) { adjusting = false; current.classList.remove('gpAdjust'); return; }
  if (hooks?.back()) highlight(null);
}

/** Press, hold-to-repeat: fires on the press, then after a pause, then steadily while held. */
function press(action: string, down: boolean, fire: () => void, repeat = true) {
  const now = performance.now();
  if (!down) { held.delete(action); return; }
  const next = held.get(action);
  if (next === undefined) { fire(); held.set(action, repeat ? now + REPEAT_DELAY : Infinity); return; }
  if (now >= next) { fire(); held.set(action, now + REPEAT_EVERY); }
}

/** The choice: ?pad= for this session, else what START last chose on this device, else decided from the pad. */
let chosen: string | null = (() => {
  try { const u = new URLSearchParams(location.search).get('pad'); if (u) return u; } catch { /* no URL */ }
  try { return localStorage.getItem(PROFILE_KEY); } catch { return null; }
})();
function forcedProfile(): string | null { return chosen; }

function toggleProfile(axes: number) {
  chosen = padProfile(axes, chosen) === 'sticks' ? 'buttons' : 'sticks';
  try { localStorage.setItem(PROFILE_KEY, chosen); } catch { /* private window: this session only */ }
  toast(chosen === 'sticks' ? 'Controls: two sticks — turn the left stick to tune, the right to zoom'
                            : 'Controls: buttons by position — face buttons tune and zoom, R enter, L back');
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
  for (const gp of pads) {
    const btn = (i: number) => !!gp.buttons[i]?.pressed;
    const prof = padProfile(gp.axes.length, forcedProfile());
    const id = `${gp.index}:`;
    press(id + 'start', btn(B_START), () => toggleProfile(gp.axes.length), false);
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
      const t = tuneJog.feed(gp.axes[0] ?? 0, gp.axes[1] ?? 0);
      for (let k = 0; k < Math.abs(t); k++) hooks?.tune(t > 0 ? 1 : -1);
      const z = zoomJog.feed(gp.axes[2] ?? 0, gp.axes[3] ?? 0);
      for (let k = 0; k < Math.abs(z); k++) hooks?.zoom(z > 0 ? 1 : -1);
    }
  }
  requestAnimationFrame(poll);
}

export function initGamepad(h: GamepadHooks): void {
  hooks = h;
  const start = () => { if (!running) { running = true; requestAnimationFrame(poll); } };
  window.addEventListener('gamepadconnected', start);
  // ★ A pad already connected before the page loaded announces itself only on its first press — poll once to see.
  if (navigator.getGamepads && [...navigator.getGamepads()].some(Boolean)) start();
}
