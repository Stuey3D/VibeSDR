/**
 * ★★★ GAME CONTROLLERS IN THE APP — the pure part (Stuart's mapping; memory: gamepad_mapping). Tested by
 * scripts/test_pad_actions.ts.
 *
 * A pad drives the app EXACTLY like the keyboard: this turns what a pad is doing into the KEYBOARD'S OWN KEYS (the DOM
 * names src/utils/keyActions.ts maps), and src/utils/hardwareKeys.ts feeds them into the same listener bus the
 * hardware keyboard uses. So the receiver screen's highlight (ScreenNav), every panel and menu (PanelNav), the decoder
 * box and the frequency card work with a pad with no code of their own — and can never disagree with the keyboard.
 *
 *  Pads WITHOUT analogue sticks — face buttons by POSITION ('buttons'):
 *    D-pad = move the highlight · R shoulder = enter · L shoulder = back
 *    top (Y / △) = zoom in · bottom (A / ✕) = zoom out · left (X / □) = tune down · right (B / ○) = tune up
 *  Pads WITH two analogue sticks ('sticks'):
 *    D-pad = move the highlight · ✕ (bottom) = enter · ○ (right) = back
 *    LEFT stick turned like a jog wheel = tune (clockwise forwards) · RIGHT stick turned = zoom (clockwise in)
 *  SELECT opens and closes the CONTROLLER GUIDE (src/components/PadGuide.tsx — the pad drawn with what each control does,
 *  Stuart approved 2026-10-19): it is not a keyboard key, so it comes out as its own { guide } and hardwareKeys hands it
 *  to the guide, with the profile in use so the guide opens on that tab.
 *  HOLD START (~1 s, never a tap) switches between the two; a notice says which and how to switch back; the choice is
 *  remembered on the device. Without a choice, a pad with sticks gets 'sticks' (padProfile, shared with the web page).
 *
 * ★ Buttons are reported by POSITION (south / east / west / north), never by letter: an Xbox pad's A and a Nintendo
 *   pad's B are both the bottom button. The native layers (GameController on Apple, KeyEvent/MotionEvent on Android)
 *   do that translation; everything after it is here.
 * ★ Press and release are separate keys, as the keyboard's are: movement repeats while held (PanelNav), tune and zoom
 *   keep the keys' tap-steps / hold-sweeps law (createHoldSweep). A jog-wheel step is one tap.
 * ★ A key goes UP as the key it went DOWN as — switching the controls with a button held, or a pad unplugged mid-press,
 *   can never leave a sweep running or a highlight walking on its own.
 */
import { Jog, padProfile } from './gamepadNav';

export type PadButton = 'up' | 'down' | 'left' | 'right' | 'south' | 'east' | 'west' | 'north' | 'l1' | 'r1' | 'start' | 'select';

/** The bits a native report packs its buttons into (the same on iOS and Android — see VibeGamepad.swift / .kt). */
export const PAD_BIT: Record<PadButton, number> = {
  up: 1, down: 2, left: 4, right: 8,
  south: 16, east: 32, west: 64, north: 128,
  l1: 256, r1: 512, start: 1024,
  select: 2048,                              // ★ 2026-10-19: the controller guide (VibeGamepad.swift buttonOptions / .kt BUTTON_SELECT)
};

/** One native report: the whole pad, every time something on it changes. Sticks: −1..1, x right, y DOWN. */
export interface PadReport {
  id: string;
  /** Has two analogue sticks — decides the profile until START has chosen one. */
  sticks?: boolean;
  /** PAD_BIT flags of the buttons held. */
  b?: number;
  lx?: number; ly?: number; rx?: number; ry?: number;
  /** The pad was disconnected. */
  gone?: boolean;
}

export type PadProfile = 'sticks' | 'buttons';
export type PadOut =
  | { kind: 'down' | 'up'; key: string }
  | { notice: string; profile: PadProfile }
  /** SELECT was pressed: open the controller guide on `profile`, or close it. */
  | { guide: true; profile: PadProfile }
  /** ★ The controller keyboard is open and a button with a keyboard meaning was pressed (kbPadAction). */
  | { kbButton: PadButton; profile: PadProfile }
  /** ★ The controller keyboard is open and START was TAPPED: submit (decided 10-20). */
  | { kbSubmit: true };

/**
 * ★★★ THE CONTROLLER KEYBOARD'S BUTTONS (Stuart, decided 2026-10-20 — brief § Button mapping, the proposed table
 * accepted). Each pad keeps its own scheme: enter and back stay where they are (✕ / ○ on stick pads, R1 / L1 on
 * no-stick pads — they arrive as the ordinary Enter / Back keys), and the TUNE action moves the caret. On stick pads the
 * spare buttons edit: west ⌫, north space, L1 shift, R1 symbols — and on the numeric pad L1 / R1 switch kHz / MHz.
 * No-stick pads have no spare button: they use the on-screen keys. A tap of START submits; HOLD START (the profile
 * switch) is ignored while the keyboard is open.
 */
export type KbPadAction = 'backspace' | 'space' | 'shift' | 'symbols' | 'unit';

export function kbPadAction(b: PadButton, prof: PadProfile, layout: 'numeric' | 'qwerty'): KbPadAction | null {
  if (prof !== 'sticks') return null;
  switch (b) {
    case 'west': return 'backspace';
    case 'north': return layout === 'qwerty' ? 'space' : null;
    case 'l1': return layout === 'qwerty' ? 'shift' : 'unit';
    case 'r1': return layout === 'qwerty' ? 'symbols' : 'unit';
    default: return null;
  }
}
/** The buttons that can carry a keyboard meaning (kbPadAction), on a stick pad. */
const KB_BUTTONS: PadButton[] = ['west', 'north', 'l1', 'r1'];

/** START held this long switches the controls (the web page's figure). */
export const SWITCH_HOLD_MS = 900;
const TUNE_DEG = 30, ZOOM_DEG = 45;          // a jog step: the web page's figures

const DPAD: [PadButton, string][] = [['up', 'ArrowUp'], ['down', 'ArrowDown'], ['left', 'ArrowLeft'], ['right', 'ArrowRight']];

/** Which keyboard key a button is, in a profile. null = nothing (START is the switch, SELECT the guide; a shoulder in
 *  'sticks'). */
export function padKey(b: PadButton, prof: PadProfile): string | null {
  const d = DPAD.find(([n]) => n === b);
  if (d) return d[1];
  if (prof === 'buttons') {
    switch (b) {
      case 'r1': return 'Enter';
      case 'l1': return 'Escape';             // back — and, in a text field, leave it (one layer), as Escape does
      case 'north': return '+';               // zoom in
      case 'south': return '-';               // zoom out
      case 'west': return ',';                // tune down
      case 'east': return '.';                // tune up
      default: return null;
    }
  }
  switch (b) {
    case 'south': return 'Enter';
    case 'east': return 'Escape';
    default: return null;
  }
}

export function profileNotice(p: PadProfile): string {
  return (p === 'sticks' ? 'Controls: two sticks — turn the left stick to tune, the right to zoom'
                         : 'Controls: buttons by position — face buttons tune and zoom, R enter, L back')
    + ' · hold START to switch back';
}

interface PadState {
  b: number;
  sticks: boolean;
  sent: Map<PadButton, string>;              // button → the key it went down as
  startAt: number | null;                    // when START went down; null = up; -1 = switched, wait for release
  tune: Jog; zoom: Jog;
}

export class PadMapper {
  private pads = new Map<string, PadState>();
  private kbOpen = false;
  /** ★ The controller keyboard opened / closed: START taps submit and HOLD START is ignored while it is open. */
  setKeyboardOpen(on: boolean) { this.kbOpen = on; }
  /** @param chosen what START last chose on this device (null = decide from the pad). @param save remembers a choice. */
  constructor(private chosen: string | null = null, private save: (p: PadProfile) => void = () => {}) {}

  setChosen(c: string | null) { this.chosen = c; }

  /** The profile in use now — what the guide opens on: the newest connected pad's, else what START chose, else sticks. */
  current(): PadProfile {
    let last: PadState | undefined;
    for (const st of this.pads.values()) last = st;
    return last ? this.profileOf(last.sticks) : this.chosen === 'buttons' ? 'buttons' : 'sticks';
  }
  profileOf(sticks: boolean): PadProfile { return padProfile(sticks ? 4 : 2, this.chosen); }

  feed(r: PadReport, now: number): PadOut[] {
    const out: PadOut[] = [];
    let st = this.pads.get(r.id);
    if (r.gone) {
      if (st) { this.releaseAll(st, out); this.pads.delete(r.id); }
      return out;
    }
    if (!st) {
      st = { b: 0, sticks: !!r.sticks, sent: new Map(), startAt: null, tune: new Jog(TUNE_DEG), zoom: new Jog(ZOOM_DEG) };
      this.pads.set(r.id, st);
    }
    st.sticks = !!r.sticks;
    const b = r.b ?? 0;
    const prev = st.b;
    st.b = b;

    // HOLD START: the clock starts on the press; tick() switches once it has been held long enough.
    const startNow = !!(b & PAD_BIT.start);
    if (startNow && !(prev & PAD_BIT.start)) st.startAt = now;
    // ★ A START TAP while the keyboard is open submits — released before the hold would have switched.
    if (!startNow && (prev & PAD_BIT.start) && this.kbOpen && st.startAt !== null && st.startAt >= 0
        && now - st.startAt < SWITCH_HOLD_MS) out.push({ kbSubmit: true });
    if (!startNow) st.startAt = null;
    out.push(...this.checkHold(st, now));

    const prof = this.profileOf(st.sticks);
    // Releases first, then presses — a roll from one button to the next is an up then a down, as on a keyboard.
    for (const [name, bit] of Object.entries(PAD_BIT) as [PadButton, number][]) {
      if ((prev & bit) && !(b & bit)) {
        const k = st.sent.get(name);
        if (k) { st.sent.delete(name); out.push({ kind: 'up', key: k }); }
      }
    }
    // ★ SELECT: the guide, once per press (never repeated — it is a toggle).
    if ((b & PAD_BIT.select) && !(prev & PAD_BIT.select)) out.push({ guide: true, profile: prof });
    for (const [name, bit] of Object.entries(PAD_BIT) as [PadButton, number][]) {
      if (!(prev & bit) && (b & bit)) {
        const k = padKey(name, prof);
        if (k) { st.sent.set(name, k); out.push({ kind: 'down', key: k }); }
        // ★ The keyboard's own buttons — stick pads only, where these four mean nothing else (kbPadAction).
        else if (this.kbOpen && prof === 'sticks' && KB_BUTTONS.includes(name)) out.push({ kbButton: name, profile: prof });
      }
    }

    if (prof === 'sticks') {
      const tap = (k: string) => { out.push({ kind: 'down', key: k }, { kind: 'up', key: k }); };
      // ★ With the time: the rate follows the stick's speed, like the drum (gamepadNav Jog) — slow to land, fast to cross.
      const t = st.tune.feed(r.lx ?? 0, r.ly ?? 0, now);
      for (let i = 0; i < Math.abs(t); i++) tap(t > 0 ? '.' : ',');
      const z = st.zoom.feed(r.rx ?? 0, r.ry ?? 0, now);
      for (let i = 0; i < Math.abs(z); i++) tap(z > 0 ? '+' : '-');
    }
    return out;
  }

  /** Call when the clock passes nextDeadline() — a held START reports nothing further, so the time must be checked. */
  tick(now: number): PadOut[] {
    const out: PadOut[] = [];
    for (const st of this.pads.values()) out.push(...this.checkHold(st, now));
    return out;
  }

  /** When a held START will switch the controls, or null if none is held. */
  nextDeadline(): number | null {
    let d: number | null = null;
    for (const st of this.pads.values()) {
      if (st.startAt !== null && st.startAt >= 0) { const t = st.startAt + SWITCH_HOLD_MS; d = d === null ? t : Math.min(d, t); }
    }
    return d;
  }

  private checkHold(st: PadState, now: number): PadOut[] {
    if (st.startAt === null || st.startAt < 0 || now - st.startAt < SWITCH_HOLD_MS) return [];
    // ★ HOLD START is ignored while the keyboard is open — switching mid-word would change what every button means.
    if (this.kbOpen) { st.startAt = -1; return []; }
    st.startAt = -1;                                      // once per hold
    const out: PadOut[] = [];
    // Anything held goes up as what it went down as, before its meaning changes.
    this.releaseAll(st, out);
    const next: PadProfile = this.profileOf(st.sticks) === 'sticks' ? 'buttons' : 'sticks';
    this.chosen = next;
    this.save(next);
    st.tune = new Jog(TUNE_DEG); st.zoom = new Jog(ZOOM_DEG);
    out.push({ notice: profileNotice(next), profile: next });
    return out;
  }

  private releaseAll(st: PadState, out: PadOut[]) {
    for (const k of st.sent.values()) out.push({ kind: 'up', key: k });
    st.sent.clear();
  }
}
