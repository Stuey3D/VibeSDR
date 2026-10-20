/**
 * ★★★ THE CONTROLLER KEYBOARD'S EDITING RULES — a pure reducer (brief §Component). Tested by
 * scripts/test_controller_keyboard.ts.
 *
 *  - a caret that moves (the pad's TUNE action moves it — Stuart, decided 10-20), insert at the caret, ⌫ before it;
 *  - ⇧ = one capital, a double ⇧ = caps lock, a third = off; #+= the symbols layer; ÀÉ the accents layer;
 *  - numeric: digits and ONE point, a digit limit for the unit, kHz ↔ MHz converts the value; `.` is the point
 *    whatever the locale (we parse our own numbers — Brazil and much of Europe write a comma);
 *  - pin: digits only; url: no spaces.
 *
 * ★★ NEVER LOGGED (brief, password mode TRAP): this module imports nothing at run time and logs nothing — no crumb, no console, no
 *    diagnostics. The test checks there is no import and no console call here at all.
 */
import type { KbMode, KbLayer } from './kbLayouts';   // types only — erased; this module has no runtime import
export type { KbMode, KbLayer };
export type KbUnit = 'hz' | 'khz' | 'mhz';

export interface KbState {
  value: string;
  caret: number;
  layer: KbLayer;
  mode: KbMode;
  unit?: KbUnit;
  /** password: the characters are shown. */
  reveal: boolean;
}

export type KbAction =
  | { t: 'ch'; ch: string }
  | { t: 'bs' } | { t: 'left' } | { t: 'right' }
  | { t: 'shift' } | { t: 'symbols' } | { t: 'accents' } | { t: 'abc' }
  | { t: 'space' } | { t: 'clear' } | { t: 'unit' } | { t: 'reveal' };

export const PIN_MAX = 12;
export const TEXT_MAX = 256;
/** Whole and decimal digits allowed per unit (the frequency box's own sense of a sensible number). */
export const UNIT_DIGITS: Record<KbUnit, { int: number; dec: number }> = {
  hz: { int: 10, dec: 0 }, khz: { int: 7, dec: 3 }, mhz: { int: 5, dec: 6 },
};

export function kbInit(mode: KbMode, value = '', opts: { unit?: KbUnit; caret?: number } = {}): KbState {
  let v = value ?? '';
  if (mode === 'frequency') v = v.replace(/,/g, '.').replace(/[^0-9.]/g, '');
  if (mode === 'pin') v = v.replace(/[^0-9]/g, '').slice(0, PIN_MAX);
  const caret = opts.caret == null ? v.length : Math.max(0, Math.min(v.length, opts.caret));
  return { value: v, caret, layer: 'lower', mode, unit: mode === 'frequency' ? opts.unit ?? 'mhz' : undefined, reveal: false };
}

/** Our own number parse: digits and one `.`, nothing locale-aware. NaN when it is not a number. */
export function kbParseNumber(v: string): number {
  if (!/^\d*\.?\d*$/.test(v) || v === '' || v === '.') return NaN;
  return Number(v);
}

/** A number back to text without float noise: at most `dec` decimals, trailing zeros trimmed. */
export function kbFormatNumber(n: number, dec: number): string {
  if (!Number.isFinite(n)) return '';
  let s = n.toFixed(dec);
  if (s.includes('.')) s = s.replace(/0+$/, '').replace(/\.$/, '');
  return s;
}

function numericInsertOk(value: string, caret: number, c: string, unit: KbUnit): boolean {
  if (c === '.') {
    if (UNIT_DIGITS[unit].dec === 0) return false;
    return !value.includes('.');
  }
  if (!/^\d$/.test(c)) return false;
  const next = value.slice(0, caret) + c + value.slice(caret);
  const [i, d = ''] = next.split('.');
  return i.length <= UNIT_DIGITS[unit].int && d.length <= UNIT_DIGITS[unit].dec;
}

function insert(s: KbState, c: string): KbState {
  const { value, caret, mode } = s;
  if (mode === 'frequency') { if (!numericInsertOk(value, caret, c, s.unit ?? 'mhz')) return s; }
  else if (mode === 'pin') { if (!/^\d$/.test(c) || value.length >= PIN_MAX) return s; }
  else {
    if (value.length >= TEXT_MAX) return s;
    if (mode === 'url' && /\s/.test(c)) return s;
  }
  const v = value.slice(0, caret) + c + value.slice(caret);
  // ★ One shifted letter, then back to lower case (caps lock stays).
  const layer = s.layer === 'upper' && /\p{L}/u.test(c) ? 'lower' : s.layer;
  return { ...s, value: v, caret: caret + c.length, layer };
}

export function kbReduce(s: KbState, a: KbAction): KbState {
  switch (a.t) {
    case 'ch': return insert(s, a.ch);
    case 'space': return s.mode === 'frequency' || s.mode === 'pin' || s.mode === 'url' ? s : insert(s, ' ');
    case 'bs':
      if (s.caret <= 0) return s;
      return { ...s, value: s.value.slice(0, s.caret - 1) + s.value.slice(s.caret), caret: s.caret - 1 };
    case 'left': return { ...s, caret: Math.max(0, s.caret - 1) };
    case 'right': return { ...s, caret: Math.min(s.value.length, s.caret + 1) };
    case 'clear': return { ...s, value: '', caret: 0 };
    case 'shift':
      if (s.mode === 'url' || s.layer === 'symbols') return s;
      if (s.layer === 'accents' || s.layer === 'accentsUp') return { ...s, layer: s.layer === 'accents' ? 'accentsUp' : 'accents' };
      return { ...s, layer: s.layer === 'lower' ? 'upper' : s.layer === 'upper' ? 'caps' : 'lower' };
    case 'symbols': return { ...s, layer: s.layer === 'symbols' ? 'lower' : 'symbols' };
    case 'accents': return { ...s, layer: s.layer === 'accents' || s.layer === 'accentsUp' ? 'lower' : 'accents' };
    case 'abc': return { ...s, layer: 'lower' };
    case 'reveal': return s.mode === 'password' ? { ...s, reveal: !s.reveal } : s;
    case 'unit': {
      if (s.mode !== 'frequency') return s;
      const from = s.unit ?? 'mhz';
      const to: KbUnit = from === 'mhz' ? 'khz' : 'mhz';
      const n = kbParseNumber(s.value);
      if (!Number.isFinite(n)) return { ...s, unit: to };
      const hz = n * (from === 'mhz' ? 1e6 : from === 'khz' ? 1e3 : 1);
      const v = kbFormatNumber(hz / (to === 'mhz' ? 1e6 : 1e3), UNIT_DIGITS[to].dec);
      return { ...s, unit: to, value: v, caret: v.length };
    }
  }
}

/** The value as the field's TEXT should show it (password: dots unless revealed). Never logged. */
export function kbDisplay(s: KbState): string {
  return s.mode === 'password' && !s.reveal ? '•'.repeat(s.value.length) : s.value;
}
