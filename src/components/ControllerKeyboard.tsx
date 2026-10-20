/**
 * ★★★ CONTROLLER KEYBOARD — text entry for a game controller or TV remote (docs/v12/BRIEF-controller-keyboard.md,
 * decided by Stuart 2026-10-20). Stuart, on device: "right now even with the on screen keyboard there is no way of
 * controlling the system keyboard with the controller."
 *
 * ★★ AN ADDITION, NEVER A REPLACEMENT. Touch → the system keyboard, always (VoiceOver / TalkBack, sizing, dictation,
 *    other languages). A physical keyboard → types in place. Only a pad or remote gets this. Decided by the source at
 *    the moment Enter opens the field AND followed live while editing (src/utils/kbHandover.ts): a touch closes this
 *    and opens the system keyboard on the same field; a physical key closes it and you type in place; a pad press in a
 *    field with the system keyboard up opens this. The value, the caret and password mode survive every swap.
 *
 * Drawn as a full-screen pop-up (it mirrors cleanly over AirPlay) with the value in a <Text> and a drawn caret —
 * never a TextInput, so the system keyboard cannot appear. Layouts are data (src/utils/kbLayouts.ts), editing is a
 * pure reducer (src/utils/controllerKeyboard.ts). Keys in:
 *   D-pad / arrows ........... move the highlight (wraps at the edges)
 *   enter (✕ / R1 / Enter) ... press the highlighted key
 *   back (○ / L1) ............ one layer: symbols / accents / shift first, then cancel (the field keeps its value)
 *   tune (left stick / □○ on no-stick pads / < >) ... move the caret
 *   stick pads: west ⌫ · north space · L1 shift · R1 symbols (numeric: L1 / R1 kHz ↔ MHz) · START tap = submit
 *   On screen, on BOTH layouts: ↵ (submit), Back (cancel) and ⌫ — so a no-stick pad or a remote can do everything.
 *
 * ★★ PASSWORD MODE (brief TRAPs): over AirPlay the highlight shows which key is pressed, so the ring is a neutral grey
 *    with no press animation, and a line offers the touchscreen instead. The value is NEVER logged — nothing here
 *    passes it to crumb, console or the diagnostics.
 */
import React, { useEffect, useMemo, useRef, useState } from 'react';
import { Keyboard, Modal, NativeModules, StyleSheet, Text, TouchableOpacity, View, useWindowDimensions,
         type TextInput } from 'react-native';
import { useOverlay, useRepeatingKeys, useNavFocus, holdIdleClose, NAV_REPEAT_KEYS } from './PanelNav';
import { usePopupTheme } from './PopupShell';
import { useFaceplate } from '../contexts/FaceplateContext';
import { FreqDigit } from './FreqDigits';
import { kbKeys, kbMove, kbStartKey, layoutFor, KB_SYSTEM_GLYPHS, type KbMode, type KbKey } from '../utils/kbLayouts';
import { kbInit, kbReduce, kbDisplay, type KbState, type KbUnit, type KbAction } from '../utils/controllerKeyboard';
import { carry, type EditCarry } from '../utils/kbHandover';
import { inputSource, inputSourceNow } from '../utils/inputSource';
import { addPadKbListener, addPadWhileTypingListener, setPadKeyboardOpen, padProfileNow } from '../utils/hardwareKeys';
import { kbPadAction } from '../utils/padActions';
import type { Dir } from '../utils/gamepadNav';

export interface KbRequest {
  mode: KbMode;
  title?: string;
  value: string;
  caret?: number;
  /** frequency: the unit the value is in, and whether the receiver locks it. */
  unit?: KbUnit;
  unitLocked?: boolean;
  /** Live, as it changes (search: results update while typing). */
  onChange?: (v: string) => void;
  /** ↵ / START. Return a message to refuse it and stay open (an out-of-range frequency). */
  onSubmit: (v: string, unit?: KbUnit) => string | void;
  /** Back with nothing left to peel — the field keeps the value it had. */
  onCancel?: () => void;
  /** A touch or a physical key took over: put this value and caret in the field and give it the cursor. */
  onHandover?: (c: EditCarry) => void;
}

let current: KbRequest | null = null;
const subs = new Set<(r: KbRequest | null) => void>();
const emit = () => subs.forEach((f) => f(current));

/** Open the controller keyboard (normally through usePadField). Lets go of any field's cursor first — a field with
 *  the cursor would make the bus deliver only Enter and Escape, and the keyboard would be deaf. */
export function openControllerKeyboard(r: KbRequest) {
  Keyboard.dismiss();
  NativeModules.VibePowerModule?.leaveTextField?.();
  current = r;
  emit();
}
export function closeControllerKeyboard() { current = null; emit(); }
export const controllerKeyboardOpen = () => current !== null;

/** Mount once near the root (App.tsx), inside the faceplate provider. */
export function ControllerKeyboardHost() {
  const [r, setR] = useState<KbRequest | null>(current);
  useEffect(() => { subs.add(setR); setR(current); return () => { subs.delete(setR); }; }, []);
  return r ? <ControllerKeyboardView key={String(r.title) + r.mode} req={r} /> : null;
}

function ControllerKeyboardView({ req }: { req: KbRequest }) {
  const pt = usePopupTheme();
  const fp = useFaceplate();
  const navRing = useNavFocus().ring;
  const { width: W, height: H } = useWindowDimensions();
  const [st, setSt] = useState<KbState>(() => kbInit(req.mode, req.value, { unit: req.unit, caret: req.caret }));
  const [err, setErr] = useState<string | null>(null);
  const keys = useMemo(() => kbKeys(req.mode, st.layer, {
    unitLocked: req.unitLocked,
    unitLabel: (st.unit ?? 'mhz') === 'mhz' ? 'kHz' : 'MHz',
    revealLabel: st.reveal ? 'HIDE' : 'SHOW',
  }), [req.mode, st.layer, st.unit, st.reveal, req.unitLocked]);
  const [focus, setFocus] = useState(() => Math.max(0, keys.findIndex((k) => k.id === kbStartKey(req.mode))));
  // A layer change swaps the keys: keep the highlight on the same id where it still exists, else in place.
  const lastId = useRef<string | null>(null);
  useEffect(() => {
    const id = lastId.current;
    if (id) { const i = keys.findIndex((k) => k.id === id); if (i >= 0) { setFocus(i); return; } }
    setFocus((f) => Math.min(f, keys.length - 1));
  }, [keys]);
  useEffect(() => { lastId.current = keys[focus]?.id ?? null; }, [focus, keys]);

  const password = req.mode === 'password';
  const layout = layoutFor(req.mode);
  const stRef = useRef(st); stRef.current = st;
  const reqRef = useRef(req); reqRef.current = req;
  const keysRef = useRef(keys); keysRef.current = keys;
  const focusRef = useRef(focus); focusRef.current = focus;

  // ── Open / close plumbing ──
  useOverlay(true, { name: 'controller keyboard', level: 3, blockTuneZoom: true });
  useEffect(() => {
    setPadKeyboardOpen(true);
    const release = holdIdleClose();
    return () => { setPadKeyboardOpen(false); release(); };
  }, []);

  const apply = (a: KbAction) => {
    const prev = stRef.current;
    const next = kbReduce(prev, a);
    if (next === prev) return;
    stRef.current = next;
    setSt(next);
    setErr(null);
    if (next.value !== prev.value) reqRef.current.onChange?.(next.value);
  };
  const submit = () => {
    const s = stRef.current;
    const refusal = reqRef.current.onSubmit(s.value, s.unit);
    if (typeof refusal === 'string' && refusal) { setErr(refusal); return; }
    closeControllerKeyboard();
  };
  const cancel = () => { closeControllerKeyboard(); reqRef.current.onCancel?.(); };
  /** Back, one layer at a time: a layer first, then away (the field keeps its old value). */
  const back = () => {
    const l = stRef.current.layer;
    if (l !== 'lower') { apply({ t: 'abc' }); return; }
    cancel();
  };
  const handover = () => {
    const s = stRef.current;
    const h = reqRef.current.onHandover;
    closeControllerKeyboard();
    h?.(carry({ value: s.value, caret: s.caret, secure: password }));
  };
  const press = (k: KbKey | undefined) => {
    if (!k) return;
    switch (k.act.t) {
      case 'enter': submit(); return;
      case 'back': back(); return;
      default: apply(k.act as KbAction);
    }
  };

  // ── Keys in. Above every panel and overlay; every key consumed — nothing beneath hears anything. ──
  useRepeatingKeys(true, (k, e) => {
    // ★ A PHYSICAL key: hand over — they type in place (kbHandover: controller + keyboard → inplace).
    if (!e.pad) { handover(); return true; }
    const dir: Dir | null = k === 'ArrowUp' ? 'up' : k === 'ArrowDown' ? 'down' : k === 'ArrowLeft' ? 'left' : k === 'ArrowRight' ? 'right' : null;
    if (dir) { const n = kbMove(keysRef.current, focusRef.current, dir); if (n >= 0) setFocus(n); return true; }
    if (k === 'Enter') { press(keysRef.current[focusRef.current]); return true; }
    if (k === 'Back') { back(); return true; }
    if (k === 'TuneDown') { apply({ t: 'left' }); return true; }
    if (k === 'TuneUp') { apply({ t: 'right' }); return true; }
    return true;   // zoom: nothing here
  }, [...NAV_REPEAT_KEYS, 'TuneDown', 'TuneUp'], { priority: 1000 });
  useEffect(() => {
    const sub = addPadKbListener((o) => {
      if ('kbSubmit' in o) { submit(); return; }
      const a = kbPadAction(o.kbButton, padProfileNow(), layout);
      if (a === 'backspace') apply({ t: 'bs' });
      else if (a === 'space') apply({ t: 'space' });
      else if (a === 'shift') apply({ t: 'shift' });
      else if (a === 'symbols') apply({ t: stRef.current.layer === 'symbols' ? 'abc' : 'symbols' });
      else if (a === 'unit') apply({ t: 'unit' });
    });
    return () => sub.remove();
  }, [layout]);   // eslint-disable-line react-hooks/exhaustive-deps

  // ── Layout: follow the WINDOW (Stuart's rule) — numeric side by side when wide; QWERTY full width. ──
  const cols = layout === 'numeric' ? 4.75 : 10;
  const rows = layout === 'numeric' ? 4 : 5;
  const wide = W > H;
  const sidePad = 16;
  const valueH = 96;
  const availW = layout === 'numeric' && wide ? (W - sidePad * 3) * 0.55 : W - sidePad * 2;
  const availH = layout === 'numeric' && wide ? H - 80 : H - valueH - 120;
  const unitPx = Math.max(24, Math.min(layout === 'numeric' ? 86 : 58, availW / cols, availH / rows));
  const gapPx = Math.max(3, Math.round(unitPx * 0.08));

  const ringColour = password ? '#9a9a9a' : navRing;
  const qwertyFont = fp.deck.style === 'nixie' ? 'Nixie One' : 'Atkinson Hyperlegible';
  const legend = pt.metal ? pt.legend : pt.ink.key;
  const legendLit = pt.metal ? pt.legendLit : pt.gold.sel;

  const keyFace = (k: KbKey, i: number) => {
    const w = k.w * unitPx - gapPx, h = k.h * unitPx - gapPx;
    const on = i === focus;
    const isDigit = layout === 'numeric' && /^[\d.]$/.test(k.label);
    const lit = k.act.t === 'enter' || (k.act.t === 'shift' && (st.layer === 'upper' || st.layer === 'caps' || st.layer === 'accentsUp'))
             || (k.act.t === 'reveal' && st.reveal);
    const sys = [...k.label].some((c) => KB_SYSTEM_GLYPHS.includes(c));
    return (
      <TouchableOpacity /* nav-registered: the keyboard's own highlight (focus) */ key={k.id} activeOpacity={password ? 1 : 0.7}
        accessibilityRole="button" accessibilityLabel={k.act.t === 'ch' ? k.label : k.id}
        onPress={() => press(k)}
        style={{ position: 'absolute', left: k.x * unitPx, top: k.y * unitPx, width: w, height: h,
                 borderRadius: 8, borderWidth: on ? 3 : 1, borderColor: on ? ringColour : (pt.metal ? '#55524c' : pt.ink.keyBorder),
                 backgroundColor: pt.metal ? (pt.silver ? '#c4c1ba' : '#151618') : 'rgba(255,255,255,0.05)',
                 alignItems: 'center', justifyContent: 'center', overflow: 'hidden' }}>
        {isDigit
          ? <FreqDigit ch={k.label} w={w - 6} h={h - 6} />
          : <Text numberOfLines={1} adjustsFontSizeToFit minimumFontScale={0.6}
                  style={{ color: lit ? legendLit : legend, fontSize: Math.round(Math.min(h * 0.42, 22)),
                           fontFamily: sys ? undefined : qwertyFont, fontWeight: lit ? '700' : '400' }}>{k.label}</Text>}
      </TouchableOpacity>
    );
  };

  const shown = kbDisplay(st);
  const before = shown.slice(0, st.caret), after = shown.slice(st.caret);
  const valueBox = (
    <View style={[s.valueBox, { borderColor: pt.ink.keyBorder }]}>
      {!!req.title && <Text style={[s.title, { color: pt.ink.textDim }]}>{req.title}</Text>}
      <View style={s.valueRow}>
        <Text style={[s.value, { color: pt.ink.text }]} numberOfLines={1}>{before}</Text>
        <View style={[s.caret, { backgroundColor: navRing }]} />
        <Text style={[s.value, { color: pt.ink.text }]} numberOfLines={1}>{after}</Text>
        {req.mode === 'frequency' && <Text style={[s.unit, { color: pt.ink.textDim }]}>{(st.unit ?? 'mhz') === 'mhz' ? 'MHz' : st.unit === 'khz' ? 'kHz' : 'Hz'}</Text>}
      </View>
      {!!err && <Text style={[s.err, { color: pt.ink.m.bad }]}>{err}</Text>}
      {password && <Text style={[s.hint, { color: pt.ink.textDim }]}>You can type this on the touchscreen instead — touch anywhere.</Text>}
    </View>
  );
  const grid = (
    <View style={{ width: cols * unitPx, height: rows * unitPx, alignSelf: 'center' }}>
      {keys.map(keyFace)}
    </View>
  );
  return (
    <Modal visible transparent animationType="fade" onRequestClose={back}
           supportedOrientations={['portrait', 'portrait-upside-down', 'landscape', 'landscape-left', 'landscape-right']}>
      {/* ★ ANY TOUCH hands over to the system keyboard on the same field (accessibility) — kbHandover. */}
      <View style={[s.root, { backgroundColor: 'rgba(0,0,0,0.88)' }]}
            onTouchStart={() => { inputSource.set('touch'); handover(); }}>
        {layout === 'numeric' && wide ? (
          <View style={[s.side, { paddingHorizontal: sidePad }]}>
            <View style={{ flex: 1, justifyContent: 'center', marginRight: sidePad }}>{valueBox}</View>
            <View style={{ justifyContent: 'center' }}>{grid}</View>
          </View>
        ) : (
          <View style={{ paddingHorizontal: sidePad, gap: 14 }}>
            {valueBox}
            {grid}
          </View>
        )}
      </View>
    </Modal>
  );
}

const s = StyleSheet.create({
  root:     { flex: 1, justifyContent: 'center' },
  side:     { flexDirection: 'row', alignItems: 'center' },
  valueBox: { borderWidth: 1, borderRadius: 10, paddingHorizontal: 14, paddingVertical: 10, backgroundColor: 'rgba(0,0,0,0.6)' },
  title:    { fontFamily: 'Atkinson Hyperlegible', fontSize: 13, marginBottom: 4 },
  valueRow: { flexDirection: 'row', alignItems: 'center', minHeight: 40 },
  value:    { fontFamily: 'Atkinson Hyperlegible', fontSize: 28, flexShrink: 1 },
  caret:    { width: 3, height: 30, marginHorizontal: 1, borderRadius: 1 },
  unit:     { fontFamily: 'Atkinson Hyperlegible', fontSize: 16, marginLeft: 8 },
  err:      { fontFamily: 'Atkinson Hyperlegible', fontSize: 13, marginTop: 6 },
  hint:     { fontFamily: 'Atkinson Hyperlegible', fontSize: 12, marginTop: 6 },
});

// ── usePadField: one text field, three editors ────────────────────────────────────────────────────────────────────
//
// ★ Wire a field once: its nav slot's Enter calls `open()`, and the TextInput takes `fieldProps` (and keeps its own
//   ref, value and onChangeText). `open()` follows the source at that moment: pad → ControllerKeyboard; keyboard or
//   touch → the cursor goes in the field exactly as before. While the field has the cursor, a pad press (not Back)
//   hands it to ControllerKeyboard with the text typed so far and the caret.
export interface PadFieldOpts {
  ref: React.RefObject<TextInput | null>;
  mode: KbMode;
  title?: string;
  /** Read at open time — the field's live value. */
  getValue: () => string;
  setValue: (v: string) => void;
  /** ↵ on the controller keyboard (a field's own commit: tune, unlock, connect). Return a message to stay open. */
  onSubmit?: (v: string, unit?: KbUnit) => string | void;
  /** search: push every change, so results follow the typing. */
  live?: boolean;
  unit?: () => KbUnit | undefined;
  unitLocked?: boolean;
  onUnit?: (u: KbUnit) => void;
}

export function usePadField(o: PadFieldOpts) {
  const optsRef = useRef(o); optsRef.current = o;
  const caretRef = useRef<number | null>(null);
  const focusedRef = useRef(false);

  const giveCursor = (c?: EditCarry) => {
    const f = optsRef.current;
    if (c) f.setValue(c.value);
    // After the value has rendered, then the caret where it was.
    setTimeout(() => {
      f.ref.current?.focus();
      if (c) setTimeout(() => { (f.ref.current as unknown as { setSelection?: (a: number, b: number) => void })?.setSelection?.(c.caret, c.caret); }, 30);
    }, 0);
  };

  const openKeyboard = (value: string, caret?: number) => {
    const f = optsRef.current;
    openControllerKeyboard({
      mode: f.mode, title: f.title, value, caret,
      unit: f.unit?.(), unitLocked: f.unitLocked,
      onChange: f.live ? (v) => optsRef.current.setValue(v) : undefined,
      onSubmit: (v, unit) => {
        const g = optsRef.current;
        if (unit && g.onUnit && unit !== g.unit?.()) g.onUnit(unit);
        g.setValue(v);
        return g.onSubmit?.(v, unit);
      },
      onHandover: (c) => giveCursor(c),
    });
  };

  /** Enter on the field's slot (or a press that should start editing). */
  const open = () => {
    if (inputSourceNow() === 'pad') openKeyboard(optsRef.current.getValue());
    else optsRef.current.ref.current?.focus();
  };

  // ★ Pad pressed while this field has the cursor → ControllerKeyboard takes it over (kbHandover: system/inplace + pad).
  useEffect(() => {
    const sub = addPadWhileTypingListener(() => {
      if (!focusedRef.current) return;
      const v = optsRef.current.getValue();
      const c = caretRef.current ?? v.length;
      openKeyboard(v, c);
    });
    return () => sub.remove();
  }, []);   // eslint-disable-line react-hooks/exhaustive-deps

  const fieldProps = {
    onFocus: () => { focusedRef.current = true; },
    onBlur: () => { focusedRef.current = false; },
    onSelectionChange: (e: { nativeEvent: { selection: { start: number; end: number } } }) => {
      caretRef.current = e.nativeEvent.selection.end;
    },
  };
  return { open, fieldProps };
}
