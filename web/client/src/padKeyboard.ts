/**
 * ★★★ THE CONTROLLER KEYBOARD ON THE WEB PAGE (docs/v12/BRIEF-controller-keyboard.md, decided by Stuart 2026-10-20) —
 * text entry for a game controller or a handheld. The handheld kiosk (an Anbernic / R36S running the page full screen)
 * has no on-screen keyboard at all, and a phone's own keyboard cannot be driven by a pad.
 *
 * SHARED, NOT FORKED: the layouts (src/utils/kbLayouts.ts), the editing rules (src/utils/controllerKeyboard.ts), the
 * hand-over rule (src/utils/kbHandover.ts) and the last-input tracker (src/utils/inputSource.ts) are the app's own
 * files, byte for byte. This file only draws them as DOM and wires them to the page's fields.
 *
 * ★★ AN ADDITION, NEVER A REPLACEMENT (Stuart's rules):
 *  - It shows ONLY when the last input was a game controller — a real button press or a stick past the dead zone
 *    (gamepad.ts). A pad that is merely connected never counts.
 *  - Touch or mouse on a field → the browser's / the phone's own keyboard, exactly as before.
 *  - A physical keyboard types in place, with no overlay.
 *  - LIVE hand-over mid-entry: a pad press in a field brings this up; a touch or a click closes this and gives the real
 *    field the cursor (the system keyboard); a physical key closes this and types in place. The text, the caret and
 *    password masking are carried every time (kbHandover.carry).
 * ★★ NEVER A FOCUSED <input> while this shows — the value is drawn as text with a drawn caret, and the field is blurred,
 *    so a phone never pops its own keyboard up underneath.
 * ★★ PASSWORD MODE: a neutral grey ring and no press flash (over a mirrored screen the highlight shows which key was
 *    pressed), the value drawn as dots, and NOTHING here logs — no console, no diagnostics. Checked by
 *    scripts/test_web_pad_keyboard.ts.
 *
 * Pad buttons (gamepad.ts routes them here while this is open — each pad profile keeps its own enter and back):
 *   two sticks: D-pad keys · ✕ press · ○ back one layer · left stick turned = caret · □ ⌫ · △ space ·
 *               L1 shift / R1 symbols (numeric: kHz ↔ MHz) · START tap = ↵ · HOLD START ignored while open
 *   buttons:    D-pad keys · R press · L back one layer · left / right face buttons = caret · the rest are on-screen keys
 */
import { kbKeys, kbMove, kbStartKey, layoutFor, KB_SYSTEM_GLYPHS,
         type KbMode, type KbKey, type KbLayoutName } from '../../../src/utils/kbLayouts';
import { kbInit, kbReduce, kbDisplay, type KbState, type KbUnit, type KbAction } from '../../../src/utils/controllerKeyboard';
import { handover, carry } from '../../../src/utils/kbHandover';
import { inputSource, type InputSource } from '../../../src/utils/inputSource';
import type { Dir } from '../../../src/utils/gamepadNav';

/** What a field needs beyond what its own attributes say. */
export interface KbFieldOpts {
  mode?: KbMode;
  title?: string;
  /** frequency: the unit the field is in now, and how to switch it (BEFORE the value is written). No setter → no unit key. */
  unit?: () => KbUnit | undefined;
  setUnit?: (u: KbUnit) => void;
  /** After ↵: a refusal message keeps the keyboard open showing it (an out-of-range frequency). */
  refusal?: () => string;
  /** ↵ presses Enter in the field (its own commit: tune, unlock, connect) — default; 'none' only writes the value. */
  commit?: 'enter' | 'none';
  /** What the field does when it takes the cursor that still matters with no cursor (search: fetch the server's list). */
  onOpen?: () => void;
}

const fields = new WeakMap<HTMLElement, KbFieldOpts>();
/** Tell the keyboard more about a field (main.ts / chat.ts). A field with no entry works from its attributes. */
export function kbField(el: HTMLElement | null, o: KbFieldOpts) { if (el) fields.set(el, o); }

type TextField = HTMLInputElement | HTMLTextAreaElement;
const TEXT_TYPES = ['text', 'search', 'password', 'number', 'tel', 'url', 'email', ''];
/** A box you type into (not a button, slider, checkbox, colour or file picker), and one that is not read-only. */
export function isTextField(el: unknown): el is TextField {
  if (el instanceof HTMLTextAreaElement) return !el.readOnly && !el.disabled;
  if (!(el instanceof HTMLInputElement)) return false;
  return TEXT_TYPES.includes((el.getAttribute('type') || '').toLowerCase()) && !el.readOnly && !el.disabled;
}

const MODES: KbMode[] = ['frequency', 'pin', 'search', 'text', 'password', 'url'];
function modeFor(el: TextField, o: KbFieldOpts): KbMode {
  if (o.mode) return o.mode;
  const d = el.dataset.kb as KbMode | undefined;
  if (d && MODES.includes(d)) return d;
  const type = el instanceof HTMLInputElement ? el.type : 'text';
  const im = el.getAttribute('inputmode') || '';
  if (type === 'password') return im === 'numeric' ? 'pin' : 'password';
  if (im === 'numeric' || type === 'number' || type === 'tel') return 'pin';
  if (im === 'decimal') return 'frequency';
  if (type === 'search') return 'search';
  if (type === 'url') return 'url';
  return 'text';
}

function titleFor(el: TextField, o: KbFieldOpts): string {
  if (o.title) return o.title;
  const lab = el.id ? document.querySelector(`label[for="${CSS.escape(el.id)}"]`)?.textContent?.trim() : '';
  return lab || el.getAttribute('aria-label') || el.placeholder || '';
}

interface Session {
  el: TextField;
  o: KbFieldOpts;
  mode: KbMode;
  st: KbState;
  keys: KbKey[];
  focus: number;
  secure: boolean;
  /** The field's value when it opened — Back leaves it there. */
  orig: string;
  err: string;
}

let s: Session | null = null;
let root: HTMLElement | null = null;
const closeSubs: Array<(el: TextField, why: 'submit' | 'cancel' | 'handover') => void> = [];
/** gamepad.ts: put the highlight back on the field when the keyboard closes (not after a hand-over). */
export function onPadKbClose(f: (el: TextField, why: 'submit' | 'cancel' | 'handover') => void) { closeSubs.push(f); }

export const padKbOpen = (): boolean => s !== null;
export const padKbLayout = (): KbLayoutName => (s ? layoutFor(s.mode) : 'qwerty');

/** Fields a touch, a click or a keystroke has actually gone into since they took the cursor — a pad press in one of
 *  these hands it to the controller keyboard. A field that took the cursor by itself (autofocus) is only let go. */
const edited = new WeakSet<Element>();
export const fieldEdited = (el: Element): boolean => edited.has(el);
let anyUserInput = false;

// ── Drawing ───────────────────────────────────────────────────────────────────────────────────────────────────────────

function build(): HTMLElement {
  if (root) return root;
  root = document.createElement('div');
  root.id = 'padKb';
  root.hidden = true;
  root.setAttribute('role', 'dialog');
  root.setAttribute('aria-label', 'On-screen keyboard');
  root.innerHTML =
    '<div class="kbSheet"><div class="kbValueBox"><div class="kbTitle"></div>' +
    '<div class="kbValue"><span class="kbBefore"></span><span class="kbCaret"></span><span class="kbAfter"></span>' +
    '<span class="kbUnit"></span></div><div class="kbErr"></div><div class="kbHint"></div></div>' +
    '<div class="kbGrid"></div></div>';
  document.body.appendChild(root);
  return root;
}

function keysFor(x: Session): KbKey[] {
  return kbKeys(x.mode, x.st.layer, {
    unitLocked: !x.o.setUnit,
    unitLabel: (x.st.unit ?? 'mhz') === 'mhz' ? 'kHz' : 'MHz',
    revealLabel: x.st.reveal ? 'HIDE' : 'SHOW',
  });
}

const q = <T extends Element>(sel: string) => root!.querySelector(sel) as T;

function render() {
  if (!s || !root) return;
  // A layer change swaps the keys: keep the highlight on the same key where it still exists, else in place.
  const was = s.keys[s.focus]?.id;
  s.keys = keysFor(s);
  const same = was ? s.keys.findIndex((k) => k.id === was) : -1;
  s.focus = same >= 0 ? same : Math.max(0, Math.min(s.focus, s.keys.length - 1));

  const numeric = layoutFor(s.mode) === 'numeric';
  root.classList.toggle('secure', s.secure);
  root.classList.toggle('numeric', numeric);
  // ★ The layout follows the WINDOW: the number pad sits beside its value when the window is wide and short.
  const side = numeric && innerWidth > innerHeight * 1.3 && innerHeight < 620;
  root.classList.toggle('kbSide', side);

  q<HTMLElement>('.kbTitle').textContent = titleFor(s.el, s.o);
  const shown = s.mode === 'pin' && s.secure && !s.st.reveal ? '•'.repeat(s.st.value.length) : kbDisplay(s.st);
  q<HTMLElement>('.kbBefore').textContent = shown.slice(0, s.st.caret);
  q<HTMLElement>('.kbAfter').textContent = shown.slice(s.st.caret);
  q<HTMLElement>('.kbUnit').textContent = s.mode === 'frequency'
    ? ((s.st.unit ?? 'mhz') === 'mhz' ? 'MHz' : s.st.unit === 'khz' ? 'kHz' : 'Hz') : '';
  q<HTMLElement>('.kbErr').textContent = s.err;
  q<HTMLElement>('.kbHint').textContent = s.secure
    ? 'You can type this on the touchscreen or a keyboard instead — touch anywhere, or just type.' : '';

  // Key size: as big as fits, both ways (numeric 4.75 × 4 units, QWERTY 10 × 5).
  const cols = numeric ? 4.75 : 10, rows = numeric ? 4 : 5;
  const availW = (side ? innerWidth * 0.55 : innerWidth) - 32;
  const availH = side ? innerHeight - 48 : innerHeight * 0.7 - 120;
  const u = Math.floor(Math.max(26, Math.min(numeric ? 84 : 60, availW / cols, availH / rows)));
  const gap = Math.max(3, Math.round(u * 0.08));
  const grid = q<HTMLElement>('.kbGrid');
  grid.style.width = `${cols * u}px`;
  grid.style.height = `${rows * u}px`;
  grid.style.setProperty('--kbU', `${u}px`);
  grid.innerHTML = '';
  s.keys.forEach((k, i) => {
    const d = document.createElement('div');
    const digit = numeric && /^[\d.]$/.test(k.label);
    const lit = k.act.t === 'enter'
      || (k.act.t === 'shift' && (s!.st.layer === 'upper' || s!.st.layer === 'caps' || s!.st.layer === 'accentsUp'))
      || (k.act.t === 'reveal' && s!.st.reveal);
    d.className = 'kbKey' + (i === s!.focus ? ' kbOn' : '') + (lit ? ' kbLit' : '') + (digit ? ' kbDigitKey' : '');
    d.dataset.id = k.id;
    d.style.left = `${k.x * u}px`;
    d.style.top = `${k.y * u}px`;
    d.style.width = `${k.w * u - gap}px`;
    d.style.height = `${k.h * u - gap}px`;
    const lab = document.createElement('span');
    // ★ The number pad's digits are the frequency readout's own (#mFreq and .kbDigit share one rule in index.html).
    lab.className = digit ? 'kbDigit' : [...k.label].some((c) => KB_SYSTEM_GLYPHS.includes(c)) ? 'kbSys' : '';
    lab.textContent = k.label;
    lab.style.fontSize = `${Math.round(Math.min((k.h * u - gap) * (digit ? 0.55 : 0.4), digit ? 44 : 22))}px`;
    d.appendChild(lab);
    grid.appendChild(d);
  });
}

/** The readout's colours, taken from the card where the page's colour settings put them, so the number pad's digits
 *  match the frequency display whatever the listener chose. */
function takeReadoutColours() {
  const card = document.getElementById('mcard');
  if (!card || !root) return;
  const cs = getComputedStyle(card);
  const fq = cs.getPropertyValue('--ctl-freq').trim();
  root.style.setProperty('--kb-freq', fq || '#fff');
  const bg = cs.backgroundColor;
  root.style.setProperty('--kb-card', bg && !/rgba\(0, 0, 0, 0\)|transparent/.test(bg) ? bg : '#000');
}

function flash(i: number) {
  if (!s || s.secure || !root) return;          // ★ password mode: no press animation, ever
  const el = root.querySelectorAll<HTMLElement>('.kbKey')[i];
  if (!el) return;
  el.classList.add('kbPress');
  setTimeout(() => el.classList.remove('kbPress'), 110);
}

// ── Opening and closing ───────────────────────────────────────────────────────────────────────────────────────────────

/** Open the controller keyboard on `el` (pad Enter on a field, or a pad press while typing in one). */
export function openPadKeyboard(el: TextField, caret?: number) {
  const o = fields.get(el) ?? {};
  const mode = modeFor(el, o);
  const st = kbInit(mode, el.value, { unit: o.unit?.(), caret });
  s = { el, o, mode, st, keys: [], focus: 0, secure: el instanceof HTMLInputElement && el.type === 'password', orig: el.value, err: '' };
  s.keys = keysFor(s);
  s.focus = Math.max(0, s.keys.findIndex((k) => k.id === kbStartKey(mode)));
  // ★ Let go of the cursor FIRST: a focused field is what makes a phone show its own keyboard.
  if (document.activeElement === el) el.blur();
  o.onOpen?.();
  build();
  takeReadoutColours();
  root!.hidden = false;
  document.body.classList.add('padKbOpen');
  render();
}

function close(why: 'submit' | 'cancel' | 'handover') {
  if (!s) return;
  const el = s.el;
  s = null;
  if (root) { root.hidden = true; q<HTMLElement>('.kbGrid').innerHTML = ''; q<HTMLElement>('.kbBefore').textContent = ''; q<HTMLElement>('.kbAfter').textContent = ''; }
  document.body.classList.remove('padKbOpen');
  for (const f of closeSubs) { try { f(el, why); } catch { /* a listener's fault */ } }
}

function writeField(el: TextField, v: string) {
  const max = el.maxLength > 0 ? el.maxLength : Infinity;
  const nv = v.slice(0, max);
  if (el.value === nv) return;
  el.value = nv;
  el.dispatchEvent(new Event('input', { bubbles: true }));
}

function live(x: Session) { return x.mode === 'search'; }

function apply(a: KbAction) {
  if (!s) return;
  const prev = s.st;
  const next = kbReduce(prev, a);
  if (next === prev) return;
  s.st = next;
  s.err = '';
  // Search follows the typing: the results update as you go.
  if (live(s) && next.value !== prev.value) writeField(s.el, next.value);
  render();
}

/** ↵ / a START tap: the value into the field, then the field's own commit. A refusal keeps the keyboard open. */
export function padKbSubmit() {
  if (!s) return;
  const x = s;
  if (x.o.setUnit && x.st.unit && x.st.unit !== x.o.unit?.()) x.o.setUnit(x.st.unit);
  writeField(x.el, x.st.value);
  const commit = x.o.commit ?? (live(x) ? 'none' : 'enter');
  if (commit === 'enter') {
    const ev = new KeyboardEvent('keydown', { key: 'Enter', code: 'Enter', bubbles: true, cancelable: true });
    x.el.dispatchEvent(ev);
    const form = (x.el as HTMLInputElement).form;
    if (!ev.defaultPrevented && form && s === x) form.requestSubmit();
  }
  if (s !== x) return;                          // the commit closed it (or opened another)
  const refused = x.o.refusal?.() ?? '';
  if (refused) { x.err = refused; render(); return; }
  close('submit');
}

/** Back, one layer at a time: symbols / accents / shift first, then away — the field keeps the value it had. */
export function padKbBack() {
  if (!s) return;
  if (s.st.layer !== 'lower') { apply({ t: 'abc' }); return; }
  if (live(s)) writeField(s.el, s.orig);
  close('cancel');
}

export function padKbMove(dir: Dir) {
  if (!s) return;
  const n = kbMove(s.keys, s.focus, dir);
  if (n >= 0 && n !== s.focus) { s.focus = n; render(); }
}

/** Press the highlighted key. */
export function padKbPress() {
  if (!s) return;
  const k = s.keys[s.focus];
  if (!k) return;
  flash(s.focus);
  if (k.act.t === 'enter') { padKbSubmit(); return; }
  if (k.act.t === 'back') { padKbBack(); return; }
  apply(k.act as KbAction);
}

/** A pad button's own editing action (⌫, space, shift, symbols, kHz/MHz, caret). */
export function padKbEdit(a: 'bs' | 'space' | 'shift' | 'symbols' | 'unit' | 'left' | 'right') {
  if (!s) return;
  if (a === 'symbols') { apply({ t: s.st.layer === 'symbols' ? 'abc' : 'symbols' }); return; }
  apply({ t: a } as KbAction);
}

/** The field went away under the keyboard (its panel closed, the page changed): close without touching it. */
export function padKbCheckField() {
  if (s && !s.el.isConnected) close('cancel');
}

/** A touch, a click or a physical key took over: the value and caret into the real field, and give it the cursor. */
function handOver(src: InputSource): TextField | null {
  if (!s) return null;
  if (!handover('controller', src)) return null;
  const c = carry({ value: s.st.value, caret: s.st.caret, secure: s.secure });
  const el = s.el;
  writeField(el, c.value);
  close('handover');
  el.focus({ preventScroll: true });
  try { el.setSelectionRange(c.caret, c.caret); } catch { /* type=number has no caret to set */ }
  edited.add(el);
  return el;
}

// ── The page's own input: touch / mouse, keyboard ─────────────────────────────────────────────────────────────────────

const MODIFIERS = ['Shift', 'Control', 'Alt', 'Meta', 'CapsLock', 'Fn', 'AltGraph'];

export function initPadKeyboard() {
  // ★ CAPTURE, on the window: seen before anything on the page can act on it.
  window.addEventListener('pointerdown', (e) => {
    anyUserInput = true;
    inputSource.set('touch');                    // a mouse counts as a hand, as far as typing goes
    if (isTextField(e.target)) edited.add(e.target as Element);
    if (!s) return;
    // ★ Ours is up: this press is for the real field (the system keyboard), never for what lies beneath.
    e.preventDefault();
    e.stopPropagation();
    const el = handOver('touch');
    if (!el) return;
    // The click that follows must not land on the page underneath either — and it is a user gesture, the one a phone
    // wants before it shows its keyboard, so the field takes the cursor again there.
    const fix = (ev: Event) => {
      window.removeEventListener('click', fix, true);
      ev.preventDefault(); ev.stopPropagation();
      el.focus({ preventScroll: true });
    };
    window.addEventListener('click', fix, true);
    setTimeout(() => window.removeEventListener('click', fix, true), 800);
  }, true);

  window.addEventListener('keydown', (e) => {
    if (!e.isTrusted || MODIFIERS.includes(e.key)) return;   // ours (the commit's Enter) and bare modifiers: not a hand-over
    anyUserInput = true;
    inputSource.set('keyboard');
    if (isTextField(e.target)) edited.add(e.target as Element);
    if (!s) return;
    // ★ Ours is up: they type in place. The field takes the cursor NOW, so the key's own character lands in it.
    //   Nothing else on the page hears this key — its target was the page, not the field, and the page's shortcuts
    //   (arrows tune, letters pick modes) must not fire while somebody is typing.
    e.stopPropagation();
    const el = handOver('keyboard');
    if (el && e.key === 'Enter') {
      e.preventDefault();
      el.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', code: 'Enter', bubbles: true, cancelable: true }));
    }
  }, true);

  // A field the person put the cursor in (a tap, a click, Tab) is being edited; one that took it by itself is not.
  document.addEventListener('focusin', (e) => {
    if (isTextField(e.target) && anyUserInput && inputSource.get() !== 'pad') edited.add(e.target as Element);
  });
  document.addEventListener('focusout', (e) => { if (e.target instanceof Element) edited.delete(e.target); });

  window.addEventListener('resize', () => { if (s) render(); });
}

/** A pad press counts as user input too (gamepad.ts). */
export function notePadInput() { anyUserInput = true; inputSource.set('pad'); }
export { inputSource };
