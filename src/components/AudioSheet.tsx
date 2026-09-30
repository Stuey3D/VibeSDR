import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import {
  Modal, PanResponder, Pressable, ScrollView, StyleSheet, Text, TouchableOpacity, View,
} from 'react-native';
import Slider from '@react-native-community/slider';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import { useTheme } from '../contexts/ThemeContext';
import type { DspFilterDesc, DspParamDesc } from './MenuSheet';
import { NavCtx, NavRow, usePanelNav, useNavButton, useNavRange, NAV_FOCUS, noteTouchInteraction } from './PanelNav';
import SectionIcon, { type SectionIconName } from './SectionIcon';
import { meterText, useMeters, type MeterBus } from './ControlsBar';
import { isKiwiProtocol } from '../services/sdrTypes';
import {
  usePopupStyles, usePopupTheme, usePopupSurface, usePopupFrame, onMetal, engraveText,
  PopupKey, PopupFader, PopupPlate, PopupHandle, PopupScrim, PopupWindow, type PopupTokens,
} from './PopupShell';
import { sqlClosedOf } from '../constants/meters';

// Local copy of the menu's accessibility palette so this sheet is self-contained
// (no shared-internals refactor of MenuSheet). Values mirror MenuSheet's `C`.
/** Today's dim behind the sheet, and the sheet's own glass — see PopupScrim / usePopupSurface for
 *  what Transparency OFF does with each. */
const BACKDROP = 'rgba(0,0,0,0.50)';
const SHEET_BG = 'rgba(8,6,1,0.97)';
/** The squelch fader's index line while the gate MUTES (§4.3's closed-ring red). */
const SQL_MUTING = '#ff3a2e';

const C = {
  gold:        '#ffe566',
  goldDim:     'rgba(255,229,102,0.70)',
  muted:       'rgba(255,255,255,0.92)',
  btnBg:       'rgba(20,18,14,0.85)',
  border:      'rgba(255,255,255,0.30)',
  active:      'rgba(255,200,0,0.12)',
  divider:     'rgba(255,255,255,0.12)',
  sectionC:    'rgba(180,190,210,0.80)',
};

/**
 * The squelch control: the LIVE SIGNAL METER IS the control. Signal fills the bar, a needle with a
 * grabbable ball marks the threshold, and the fill reddens while the gate is muting you.
 *
 * There used to be a slider and a number above this. Both are gone deliberately:
 *  - the slider was a second, static bar with its own ball, sitting directly above a live one — two
 *    bars, two balls, and only one of them meant anything;
 *  - the number ("≥27") was unreadable in the sense that matters: 27 of what, relative to a noise
 *    floor you can't see? You set squelch by pointing at noise, not by naming a figure.
 * What is left is the one gesture that was always the real interaction: drag the ball to just above
 * the noise. Drag it off the left end to turn squelch off.
 *
 * THE NEEDLE IS ALWAYS DRAWN, including when squelch is OFF — parked at the left end and dimmed.
 * A handle that only appears once the thing is already on is a handle you can never use to turn it
 * on, which is exactly how v2 shipped: with squelch off there was simply nothing to grab.
 *
 * `level` and `pos` are the meter bus's own 0..1 bar scale — the same numbers the main signal meter
 * draws, so this needle sits exactly where that red line sits. `pos` < 0 = squelch off.
 * `onDrag` receives a 0..1 position, or -1 for off; SDRScreen converts to the backend's native unit.
 */
// ── Keyboard-reachable slider (see PanelNav; same shape as MenuSheet's) ──────
function NavSlider(props: React.ComponentProps<typeof Slider>) {
  const { minimumValue = 0, maximumValue = 1, step, value = 0, onValueChange } = props;
  const nudge = step && step > 0 ? step : (maximumValue - minimumValue) / 20;
  // ★ ATTACH THE REF. Without it reveal cannot measure the slider and falls back to the old
  // row-height ESTIMATE, which is why a slider landed barely in view at the foot of the sheet
  // while every button centred correctly — the buttons attach theirs and the sliders did not.
  const { focused, viewRef } = useNavRange((dir) => {
    const next = Math.max(minimumValue, Math.min(maximumValue, value + dir * nudge));
    if (next !== value) onValueChange?.(next);
  });
  const pt = usePopupTheme();
  if (pt.metal) {
    // ★ §10.3: a slide fader; unlit exactly where the slider's track went muted ("Off").
    return (
      <PopupFader innerRef={viewRef as any} value={value} minimumValue={minimumValue} maximumValue={maximumValue}
        step={step} onValueChange={onValueChange} onSlidingComplete={props.onSlidingComplete}
        active={props.minimumTrackTintColor !== C.muted} focused={focused}
        style={[StyleSheet.flatten(props.style) as any, { height: 24 }]} />
    );
  }
  return (
    <Slider ref={viewRef as any} {...props}
      minimumTrackTintColor={focused ? NAV_FOCUS : props.minimumTrackTintColor}
      thumbTintColor={focused ? NAV_FOCUS : props.thumbTintColor} />
  );
}

function SquelchBar({ level, raw, pos, gate, auto = false, onDrag, onDragEnd }: {
  level: number; pos: number; gate?: boolean;
  /** The meter's RAW level (MeterValues.raw, before smoothing) — the silver / black fader draws the
   *  live signal from it. Absent on a backend without one → `level`. */
  raw?: number;
  /** Auto squelch is driving the threshold: the bar fades but stays visible, the ball becomes a red
   *  line that moves on its own, and the bar says so. It stays DRAGGABLE — the drag is how you take
   *  it back (SDRScreen switches auto off on the first touch). */
  auto?: boolean;
  /** ★ A NORMALISED value: 0..1 along the bar, or -1 for off. NOT a coordinate — the
   *  parameter was called `x` and that misreading cost a real bug (see the nav note below). */
  onDrag?: (v: number) => void;
  onDragEnd?: () => void;
}) {
  const st = usePopupStyles(makeSt);
  const pt = usePopupTheme();
  // The bar's position in WINDOW coordinates, measured on layout.
  //
  // ★ Do NOT use the touch's locationX. It is relative to whichever view actually received the
  // touch, and once the ball slides under your finger that view becomes the BALL — so locationX
  // collapses to 0..22 and the needle lurches back towards the left. That is what "not latching to
  // my finger" and the "ghost of a 2nd needle" both were: one value fighting another every frame.
  // pageX minus a measured origin is target-independent and cannot do that.
  const bar = useRef<View>(null);
  const geo = useRef({ x: 0, w: 1 });
  const measure = useCallback(() => {
    bar.current?.measureInWindow((x, _y, width) => { geo.current = { x, w: Math.max(1, width) }; });
  }, []);

  // `held` is the value THIS control owns while the user is interacting. It stays put after release
  // instead of reverting to `pos`: the gate value goes out to the backend and comes back through the
  // meter bus, and until that round-trip completes (or if it never confirms, as on a backend whose
  // line position we can't derive) reverting means the ball visibly snaps back out from under the
  // finger that just placed it. Cleared only once `pos` agrees, so external changes still win.
  const [held, setHeld] = useState<number | null>(null);
  useEffect(() => {
    if (held === null) return;
    if (Math.abs(pos - held) < 0.02 || (held < 0 && pos < 0)) setHeld(null);
  }, [pos, held]);

  /* ★ A leftover `held` must not survive the switch INTO auto: it is this control's memory of a
   *   finger, and under auto the threshold is no longer the finger's. Without this the red line
   *   would sit at wherever it was last dragged until the round-trip test happened to agree. */
  useEffect(() => { if (auto) setHeld(null); }, [auto]);

  const shown = auto ? pos : (held ?? pos);
  const off = shown < 0;
  // Red = the gate is REALLY muting. Prefer its own verdict over bar geometry; while dragging,
  // geometry is all we have (the new threshold hasn't round-tripped yet).
  const closed = !off && (held !== null ? level < shown : (gate ?? (level < shown)));
  // Parked at the left end when off — the handle stays on screen and in reach.
  const handleX = `${Math.max(0, Math.min(1, off ? 0 : shown)) * 100}%` as const;

  const apply = useCallback((pageX: number) => {
    const { x, w } = geo.current;
    const raw = (pageX - x) / w;
    // Dragging off the LEFT edge is how you turn it off — the same gesture as "no threshold at
    // all", rather than a separate control to hunt for.
    const v = raw < -0.04 ? -1 : Math.max(0, Math.min(1, raw));
    setHeld(v); onDrag?.(v);
  }, [onDrag]);

  // ★★ Keyboard / D-pad adjustment, in VALUES — not coordinates.
  //
  // The first attempt synthesised a fake pageX and pushed it through onDrag. That was
  // wrong twice over: `onDrag` is TYPED `(x: number)` but `apply` actually hands it the
  // NORMALISED value, so once the bar had been measured it received something like 182
  // where 0..1 was expected — clamped to maximum, squelching everything, with every later
  // press recomputing from that. Stuart: "it jumped to the right edge, squelched everything
  // and then I couldn't get back out." A misleading parameter name, believed rather than
  // checked against the one line that calls it.
  //
  // ★ LEFT AT ZERO TURNS IT OFF, mirroring the drag gesture (drag off the left edge = off).
  // Without it there is no keyboard way back out of a squelch you have just applied.
  const { focused: navFocused, viewRef: navViewRef } = useNavRange((dir) => {
    const cur = held ?? pos;
    if (dir < 0 && cur >= 0 && cur <= 0.001) { setHeld(-1); onDrag?.(-1); onDragEnd?.(); return; }
    const base = cur < 0 ? 0 : cur;
    const next = Math.max(0, Math.min(1, base + dir * 0.04));
    setHeld(next); onDrag?.(next); onDragEnd?.();
  });

  const pan = useMemo(() => PanResponder.create({
    // CAPTURE phase, not bubble. The sheet is inside a ScrollView, and a ScrollView claims a touch
    // the moment it moves more than a few pixels — so a drag that starts with any vertical
    // component gets stolen before the bubble-phase handlers are ever asked. Capturing is the
    // difference between "very difficult to get it latching" and grabbing it first time.
    onStartShouldSetPanResponderCapture: () => !!onDrag,
    onMoveShouldSetPanResponderCapture: () => !!onDrag,
    // And once claimed, never give it back mid-drag.
    onPanResponderTerminationRequest: () => false,
    onShouldBlockNativeResponder: () => true,
    onPanResponderGrant: (e) => { measure(); apply(e.nativeEvent.pageX); },
    onPanResponderMove: (e) => apply(e.nativeEvent.pageX),
    // Tell the owner the gesture is over so it can unfreeze the noise floor (see onSquelchDrag).
    onPanResponderRelease: () => onDragEnd?.(),
    onPanResponderTerminate: () => onDragEnd?.(),
  }), [onDrag, onDragEnd, apply, measure]);

  if (pt.metal) {
    // ★★ SILVER / BLACK: THE SAME CONTROL AS A SLIDE FADER (§10.3). Same gesture, same geometry
    //   (0..1 across the full width, off the left end = off), same keyboard nudge — only the drawing
    //   changes: a recessed slot; the live signal behind the fill at white α .22 (the RAW level, the
    //   one the analogue needle springs from, §4.5); the fill in the controls colour up to the
    //   threshold; a brushed cap whose index line is the controls colour while the gate is OPEN and
    //   red while it MUTES — decided by sqlClosedOf(), the one rule the deck's meters read.
    //   Under AUTO the cap goes (it is not yours to hold) and a red line marks the threshold, as today.
    const f = pt.fader;
    const x = Math.max(0, Math.min(1, off ? 0 : shown));
    const lvl = Math.max(0, Math.min(1, raw ?? level));
    const muting = held !== null ? (!off && level < shown) : sqlClosedOf(shown, gate, level);
    return (
      <View ref={(r: any) => { (bar as any).current = r; (navViewRef as any).current = r; }}
            style={[st.sqlBarWrap, navFocused && st.sqlBarFocused, auto && st.sqlBarAuto]}
            {...(onDrag ? pan.panHandlers : {})}
            hitSlop={{ top: 14, bottom: 14, left: 10, right: 10 }}
            onLayout={measure}>
        {auto && <Text pointerEvents="none" style={st.sqlAutoOverlayTxt}>AUTO SQUELCH ACTIVE</Text>}
        <View pointerEvents="none" style={st.fBox}>
          <View style={[st.fSlot, { backgroundColor: f.slot }]} />
          <View style={[st.fSlotLip, { backgroundColor: f.slotLip }]} />
          <View style={[st.fFill, { width: `${lvl * 100}%`, backgroundColor: f.level }]} />
          {!off && (
            <View style={[st.fFill, { width: `${x * 100}%`, backgroundColor: f.fill, shadowColor: f.fillGlow }]} />
          )}
          {auto ? (
            <View style={[st.fAutoLine, { left: `${x * 100}%` }]} />
          ) : (
            <View style={[st.fCap, { left: `${x * 100}%`, borderColor: f.capBorder, backgroundColor: f.capColors[1] }]}>
              <View style={[st.fCapTop, { backgroundColor: f.capColors[0] }]} />
              <View style={[st.fCapBot, { backgroundColor: f.capColors[2] }]} />
              <View style={[st.fCapHi, { backgroundColor: f.capHi }]} />
              <View style={[st.fCapLine, off
                ? { backgroundColor: pt.legend }
                : { backgroundColor: muting ? SQL_MUTING : f.lineColor, shadowColor: muting ? SQL_MUTING : f.lineGlow }]} />
            </View>
          )}
        </View>
      </View>
    );
  }

  return (
    <View ref={(r: any) => { (bar as any).current = r; (navViewRef as any).current = r; }}
          // ★ FADES BUT STAYS VISIBLE (and stays draggable): the live signal is still the thing worth
          //   watching while auto is on, and a bar that vanished would take the evidence with it.
          style={[st.sqlBarWrap, navFocused && st.sqlBarFocused, auto && st.sqlBarAuto]}
          {...(onDrag ? pan.panHandlers : {})}
          hitSlop={{ top: 14, bottom: 14, left: 10, right: 10 }}
          onLayout={measure}>
      <View style={st.sqlBarTrack}>
        <View style={[st.sqlBarFill, {
          width: `${Math.max(0, Math.min(1, level)) * 100}%`,
          backgroundColor: closed ? 'rgba(255,77,77,0.9)' : 'rgba(255,255,255,0.9)',
        }]} />
      </View>
      {/* pointerEvents none: the handles must never become the touch target — see the note above. */}
      {/* ★★ UNDER AUTO THE BALL BECOMES A RED LINE. The ball is a HANDLE and the threshold is no
             longer yours to hold: a grabbable-looking ball moving by itself invites a fight with the
             tracker. The line is the same indicator the main signal meter draws, and it is drawn from
             the SETTING, not from the momentarily-applied threshold — during the hang the applied
             value is at the bottom of the scale, so a line drawn from it would slam to the far left
             every time somebody spoke (see the tick in SDRScreen). */}
      <View pointerEvents="none" style={[st.sqlNeedle, { left: handleX },
                    auto && st.sqlNeedleAuto,
                    off && !auto && { backgroundColor: 'rgba(255,255,255,0.35)' }]} />
      {!auto && (
        <View pointerEvents="none" style={[st.sqlBall, { left: handleX },
                      off && { backgroundColor: 'rgba(255,255,255,0.35)' }]} />
      )}
      {auto && (
        <View pointerEvents="none" style={st.sqlAutoOverlay}>
          <Text style={st.sqlAutoOverlayTxt}>AUTO SQUELCH ACTIVE</Text>
        </View>
      )}
    </View>
  );
}

/* ── The whole squelch control: the live bar, its hint, and auto squelch ─────────────────────────
 * ★★ ONE COMPONENT FOR ALL THREE BACKENDS' BARS (local/VibeServer, Kiwi, SNR). They were three
 *    copies of the same JSX, which was survivable while it was a bar and a sentence; adding a
 *    toggle, a slider and a disabled-reason to each copy is how two of them come to disagree about
 *    where a control is or what it says. The backend difference is in the CONDITION that draws it,
 *    which stays where it was.                                                                    */
function SquelchControl({
  level, raw, pos, gate, onDrag, onDragEnd, auto, onAuto, margin, onMargin, autoOk,
}: {
  level: number; raw?: number; pos: number; gate?: boolean;
  onDrag?: (v: number) => void; onDragEnd?: () => void;
  auto: boolean; onAuto?: (on: boolean) => void;
  margin: number; onMargin?: (db: number) => void;
  autoOk: boolean;
}) {
  const st = usePopupStyles(makeSt);
  const pt = usePopupTheme();
  return (
    <View style={{ flex: 1 }}>
      <SquelchBar level={level} raw={raw} pos={pos} gate={gate} auto={auto}
                  onDrag={onDrag} onDragEnd={onDragEnd} />
      <Text style={st.sqlHint}>
        {auto
          /* ★ Under auto the old sentence would be a lie — the threshold is MEANT to move. So the
           *   hint changes to say what the movement is, and to say where the way out is: a drag.
           *   Stuart: "People love it so we need to get it right but I avoid it because I cannot get
           *   it right myself." Someone who cannot set it by hand needs to be told it is working. */
          ? 'The threshold tracks this channel\'s own quiet level and sits the chosen amount above it. Drag the bar to take over by hand.'
          : 'Your squelch stays at the signal level you set. The needle drifts a little here and on the live meter as the noise floor moves — that\'s normal, not the setting changing.'}
      </Text>
      {onAuto && (
        <NavRow>
          <View style={st.sqlAutoRow}>
            <Btn label="AUTO SQUELCH" active={auto}
                 onPress={autoOk ? () => onAuto(!auto) : undefined}
                 style={autoOk ? undefined : { opacity: 0.4 }} />
            {/* ★★ THE REASON, NOT JUST A GREY BUTTON. A toggle that cannot work must say why it
                   cannot, or the listener concludes the FEATURE is broken rather than unavailable
                   here — and the accessibility hint carries the same sentence for a reader who
                   never sees the grey. */}
            {!autoOk && (
              <Text style={st.sqlAutoWhy}
                    accessibilityHint="Auto squelch needs a channel signal reading this receiver does not send.">
                Needs a channel signal reading this receiver does not send.
              </Text>
            )}
          </View>
        </NavRow>
      )}
      {/* ★ SHOWN ONLY WHILE AUTO IS ON: off, it sets nothing. "ABOVE NOISE" rather than "margin" —
             margin is what the code and OpenWebRX call it and it means nothing to a listener. */}
      {onAuto && auto && (
        <NavRow>
          <View style={st.sqlAutoMarginRow}>
            <Text style={st.sqlAutoMarginCap}>ABOVE NOISE</Text>
            <NavSlider style={st.bwSlider}
              minimumValue={4} maximumValue={20} step={1}
              value={Math.max(4, Math.min(20, margin))}
              onValueChange={(v: number) => onMargin?.(v)}
              minimumTrackTintColor={pt.gold.fill}
              maximumTrackTintColor={C.muted} thumbTintColor={pt.gold.thumb} />
            <Text style={st.bwVal}>{`+${Math.round(margin)} dB`}</Text>
          </View>
        </NavRow>
      )}
    </View>
  );
}

// ── Helpers (local copies) ────────────────────────────────────────────────────
function fmtRecTime(s: number) {
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60;
  return `${h}:${String(m).padStart(2, '0')}:${String(sec).padStart(2, '0')}`;
}
function fmtParamName(n: string) { return n.replace(/_/g, ' ').toUpperCase(); }
function dspStep(min: number, max: number) {
  const r = max - min;
  if (r <= 1)   return 0.01;
  if (r <= 10)  return 0.1;
  if (r <= 100) return 1;
  return Math.pow(10, Math.floor(Math.log10(r)) - 2);
}
function fmtDspVal(v: number, step: number) {
  return v.toFixed(step < 0.1 ? 2 : step < 1 ? 1 : 0);
}

// ── Small primitives (local copies of MenuSheet's) ───────────────────────────
function SectionLabel({ label, icon }: { label: string; icon?: SectionIconName }) {
  const st = usePopupStyles(makeSt);
  const pt = usePopupTheme();
  return (
    <View style={st.sectionBar}>
      <View style={st.sectionRow}>
        {icon && <SectionIcon name={icon} size={16} color={pt.metal ? pt.label : C.sectionC} />}
        <Text style={st.sectionLabel}>{label}</Text>
      </View>
    </View>
  );
}
function BtnRow({ children }: { children: React.ReactNode }) {
  const st = usePopupStyles(makeSt);
  return <NavRow><View style={st.btnRow}>{children}</View></NavRow>;
}
function Btn({ label, active, onPress, full, style, pip }: {
  label: string; active?: boolean; onPress?: () => void; full?: boolean; style?: object;
  /** Silver / black: the LED pip. Defaults to "has an on/off state" (`active` given). */
  pip?: boolean;
}) {
  const st = usePopupStyles(makeSt);
  const pt = usePopupTheme();
  const { focused, viewRef } = useNavButton(onPress);
  if (pt.metal) {
    return (
      <PopupKey ref={viewRef as any} label={label} active={!!active} pip={pip ?? active !== undefined}
        onPress={onPress} disabled={!onPress} focused={focused} height={34} fontSize={12}
        // ★ A disabled key dims ONCE (PopupKey's own .45) — the caller's opacity would stack on it.
        style={[{ minWidth: 72 }, full && st.btnFull, style && { ...StyleSheet.flatten(style as any), opacity: undefined }]} />
    );
  }
  return (
    <TouchableOpacity
      ref={viewRef as any}
      style={[st.btn, active && st.btnActive, full && st.btnFull, style,
              focused && st.btnFocused]}
      onPress={onPress} hitSlop={4} activeOpacity={0.7}
    >
      <Text style={[st.btnText, active && st.btnTextActive]}>{label}</Text>
    </TouchableOpacity>
  );
}
function SubLabel({ label }: { label: string }) {
  const st = usePopupStyles(makeSt);
  return <Text style={st.subLabel}>{label}</Text>;
}
function SegBtn({ label, active, onPress }: { label: string; active: boolean; onPress: () => void }) {
  const st = usePopupStyles(makeSt);
  const pt = usePopupTheme();
  const { focused, viewRef } = useNavButton(onPress);
  if (pt.metal) {
    return <PopupKey ref={viewRef as any} label={label} active={active} pip onPress={onPress}
                     focused={focused} height={32} fontSize={11} style={{ minWidth: 54 }} />;
  }
  return (
    <TouchableOpacity ref={viewRef as any}
      style={[st.btn, active && st.btnActive, focused && st.btnFocused]}
      onPress={onPress} hitSlop={4} activeOpacity={0.7}>
      <Text style={[st.btnText, active && st.btnTextActive]}>{label}</Text>
    </TouchableOpacity>
  );
}

/**
 * One of the sheet's small ON / OFF (or pick-one) keys — AUTO NOTCH, NOISE BLANKER, NFM AUDIO,
 * UNCOMP, RAW IQ, DE-EMPH, WFM STEREO, the broadcast-FM row. Default chassis: today's pill, drawn
 * exactly as it was inline (gold fill when on, black legend). Silver / black: a dome key with its
 * LED pip (§10.3 — the pip replaces every gold fill).
 */
function Toggle({ label, on, onPress, padH = 16, marginLeft, a11y }: {
  label: string; on: boolean; onPress: () => void;
  /** Today's horizontal padding (16 for a lone switch, 10 / 9 in a row of choices). */
  padH?: number; marginLeft?: number; a11y?: string;
}) {
  const pt = usePopupTheme();
  if (pt.metal) {
    return (
      <PopupKey label={label} active={on} pip onPress={onPress} hitSlop={8} height={30} fontSize={10}
        accessibilityLabel={a11y} style={{ minWidth: padH >= 16 ? 58 : 44, marginLeft }} />
    );
  }
  return (
    <TouchableOpacity onPress={onPress} hitSlop={marginLeft != null ? 6 : 8} accessibilityLabel={a11y}
      style={{ paddingHorizontal: padH, paddingVertical: 4, borderRadius: 6,
               ...(marginLeft != null ? { marginLeft } : null),
               backgroundColor: on ? pt.gold.sel : 'transparent',
               borderWidth: 1, borderColor: on ? pt.gold.sel : C.muted }}>
      <Text style={{ color: on ? '#000' : C.muted,
                     fontFamily: 'Atkinson Hyperlegible', fontSize: 11, letterSpacing: 1 }}>
        {label}
      </Text>
    </TouchableOpacity>
  );
}

export interface AudioSheetProps {
  visible:  boolean;
  onClose:  () => void;
  /** iOS Modal onDismiss — fires after the sheet is fully gone. SDRScreen uses
   *  it to present the recording share sheet only once no RN modal is up (else
   *  the native share VC presents over this Modal and wedges touch handling). */
  onDismiss?: () => void;
  serverType?: string;         // 'ubersdr' | 'owrx' | 'kiwi' | 'web888'
  /** Meter display mode — squelch readouts follow it (S-units when 'smeter'), while the value SENT
   *  to the backend stays in its native unit. */
  signalMode?: 'snr' | 'smeter' | 'dbfs';
  /** Live meter bus — so the squelch controls can show the CURRENT signal (this sheet covers the
   *  signal bar, so you'd otherwise be setting the gate blind). */
  meterBus?: MeterBus;
  isLocal?:  boolean;          // V4 local hardware
  /** FM-DX: only REC + Recordings apply (no client DSP / squelch / notch). */
  recordingOnly?: boolean;

  // Client-side NR/NB (UberSDR only)
  nr?:   boolean;
  onNr?: (mode: 'off' | 'nr' | 'nr2') => void;
  nb?:   boolean;
  onNb?: (on: boolean) => void;
  /** ★ The audio-menu NOISE BLANKER (VibeServer, every mode but WFM) — the web's nbxBtn. */
  nbx?:   boolean;
  onNbx?: (on: boolean) => void;
  /** ★ NFM audio — true = VOICE (300 Hz-3 kHz), false = RAW. The web's nfmVoiceBtn. Passed only in
   *  NFM, on a server that has reported the setting. */
  nfmVoice?:   boolean;
  onNfmVoice?: (on: boolean) => void;

  // Recording
  recording?:   boolean;
  onRec?:       () => void;
  recSeconds?:  number;
  onRecordings?: () => void;

  // Squelch variants (gated by backend)
  snrSquelch?:   number;  onSnrSquelch?:   (v: number) => void;
  localSquelch?: number;  onLocalSquelch?: (db: number) => void;
  localNR?:      number;  onLocalNR?:      (level: number) => void;
  kiwiSquelch?:  number;  onKiwiSquelch?:  (v: number) => void;
  /** Squelch dragged to a 0..1 position on the meter (-1 = dragged off / Off). SDRScreen owns the
   *  position→native-unit conversion, since it owns the forward mapping the red line is drawn from. */
  /** Normalised 0..1 along the meter, or -1 for off. Not a coordinate. */
  onSquelchDrag?: (v: number) => void;
  /** The drag gesture ended — releases the frozen noise floor. */
  onSquelchDragEnd?: () => void;
  /* ── AUTO SQUELCH ─────────────────────────────────────────────────────────────────────────────
   * ★★★ THE ALGORITHM IS NOT HERE. SDRScreen owns it, because it owns the channel figure, the noise
   *     floor and the one per-backend threshold map — see briefs/BRIEF-auto-squelch.md. This sheet
   *     only draws the switch, the margin and what the tracker has decided. */
  sqlAuto?: boolean;
  onSqlAuto?: (on: boolean) => void;
  /** ABOVE NOISE, dB. "Margin" is what the code and OpenWebRX call it and it means nothing to a
   *  listener; 4-20, and the default is measured (see SQL_AUTO_MARGIN_DEFAULT). */
  sqlAutoMargin?: number;
  onSqlAutoMargin?: (db: number) => void;
  /** ★★ Does this backend give a channel figure the tracker can steer from? False = the toggle is
   *  drawn DISABLED WITH THE REASON. ✗ Never silently dead: a switch that does nothing reads as a
   *  broken feature, which is the same rule as AGENTS.md's control that works on one radio only. */
  sqlAutoOk?: boolean;
  fmSquelch?:    number;  onFmSquelch?:    (v: number) => void;
  isFmMode?:     boolean;

  // Auto-notch (all backends)
  notchOn?: boolean;
  onNotch?: (on: boolean) => void;

  /** ★★ FM DE-EMPHASIS AND WFM STEREO LIVE HERE, NOT IN THE HARDWARE PANEL. They are not
   *  properties of the radio — they act on OUR demodulator, which is why they applied to a
   *  SpyServer too where most of that panel does not. The web client has always grouped them
   *  with volume, uncompressed audio, NR and the auto-notch, and the app had them under the
   *  radio's cog; Stuart, 2026-08-02: "to better line up with the web client… in the web client
   *  these buttons are where they should be." Moved (from LocalHardwarePanel §FM DE-EMPHASIS). */
  /** ★★ UNCOMPRESSED AUDIO — SHOWN ONLY WHEN THE SERVER SAYS THE LISTENER MAY CHOOSE.
   *  The owner's policy is three-way ('off' / 'choice' / 'compat') and only 'choice' means a
   *  switch belongs here; 'compat' is an automatic fallback with no control, and 'off' never
   *  offers it at all. SDRScreen passes undefined for anything but 'choice', which HIDES the row
   *  rather than disabling it — a greyed control still reads as an offer, and this one spends the
   *  OWNER's uplink (~187 KB/s against Opus's ~8), not ours.
   *  ★ Hans identified Opus by ear on first listen, which is why this exists at all. */
  rawAudio?: boolean;
  onRawAudio?: (on: boolean) => void;
  /** ★★★ RAW IQ OUT — this session's channel as an rtl_tcp stream, for a decoder we do not carry.
   *  Offered only when the owner allows it (SDRScreen passes undefined otherwise). `iq` is the
   *  server's answer: the LAN address, or a pairing code for the VibeIQ bridge through the tunnel. */
  iq?: { on: boolean; rate?: number; host?: string; port?: number; code?: string; public?: boolean } | null;
  onIqOut?: (on: boolean, rate: number) => void;
  iqLocal?: boolean;           // we are on the owner's network: wider rates are on offer
  deemph?: number;             // FM de-emphasis tau, SECONDS (0 = off, 50e-6, 75e-6)
  onDeemph?: (tau: number) => void;
  stereo?: boolean;            // WFM stereo on, vs forced mono
  onStereo?: (on: boolean) => void;
  // ── The broadcast-FM treatments (VibeServer 3.1) ────────────────────────────────────────────
  // ★★★ FOUR FAULTS, FOUR SWITCHES. NR answers continuous NOISE, IMS a REFLECTION too weak for CEQ, CEQ a
  //     REFLECTION, NB IMPULSES — and measured on the server they want OPPOSITE actions, so one
  //     combined control would be wrong as well as unhelpful. All four default ON and each declines
  //     to act unless its own evidence says it will help, so the switches exist for A/B rather than
  //     for daily use (asked for by TGCFabian via the FM-DX community).
  // ★ Absent handlers = a backend without them (Kiwi, OWRX, FM-DX): the rows simply do not draw,
  //   rather than offering controls that cannot work.
  fmNr?: boolean;   onFmNr?: (on: boolean) => void;
  fmIms?: boolean;  onFmIms?: (on: boolean) => void;
  fmAutoBw?: boolean;  onFmAutoBw?: (on: boolean) => void;
  fmCeq?: boolean;  onFmCeq?: (on: boolean) => void;
  fmNb?: boolean;   onFmNb?: (on: boolean) => void;

  // OWRX server-side squelch (dB) + NR (threshold dB)
  onOwrxSquelch?: (db: number) => void;
  /** The display trim, dB — the OWRX squelch slider lives on the SAME trimmed scale as the
   *  SIGNAL readout above it, and the server is sent the raw figure (DL8LDN, 2026-09-14). */
  visualGain?: number;
  onOwrxNr?:      (threshold: number) => void;
  owrxDspDefaults?: { squelchDb?: number; nrEnabled?: boolean; nrThreshold?: number; seq: number };

  // UberSDR server-side NR (DSP insert)
  serverDspEnabled?:  boolean;
  serverDspFilter?:   string;
  serverDspParams?:   Record<string, string>;
  dspFilters?:        DspFilterDesc[];
  dspError?:          string | null;
  onServerDsp?:       (enabled: boolean) => void;
  onServerDspFilter?: (name: string) => void;
  onServerDspParam?:  (name: string, value: string) => void;
}

export default function AudioSheet({
  visible, onClose, onDismiss, serverType = 'ubersdr', signalMode = 'smeter', meterBus, isLocal = false, recordingOnly = false,
  nr = false, onNr, nb = false, onNb, nbx = false, onNbx, nfmVoice = true, onNfmVoice,
  recording = false, onRec, recSeconds = 0, onRecordings,
  snrSquelch = -999, onSnrSquelch,
  localSquelch = -100, onLocalSquelch,
  localNR = 0, onLocalNR,
  kiwiSquelch = 0, onKiwiSquelch, onSquelchDrag, onSquelchDragEnd,
  sqlAuto = false, onSqlAuto, sqlAutoMargin = 12, onSqlAutoMargin, sqlAutoOk = false,
  fmSquelch = -999, onFmSquelch, isFmMode = false,
  notchOn = false, onNotch,
  deemph = 50e-6, onDeemph, stereo = true, onStereo,
  fmNr, onFmNr, fmIms, onFmIms, fmCeq, onFmCeq, fmNb, onFmNb, fmAutoBw, onFmAutoBw,
  rawAudio = false, onRawAudio, iq = null, onIqOut, iqLocal = false,
  onOwrxSquelch, onOwrxNr, owrxDspDefaults, visualGain = 0,
  serverDspEnabled = false, serverDspFilter = '', serverDspParams = {},
  dspFilters = [], dspError = null, onServerDsp, onServerDspFilter, onServerDspParam,
}: AudioSheetProps) {
  const st = usePopupStyles(makeSt);
  const pt = usePopupTheme();
  const [iqRate, setIqRate] = useState(48000);   // ★ raw IQ out: the rate to ask for
  const { theme: t } = useTheme();
  const insets = useSafeAreaInsets();
  const surf = usePopupSurface();
  const metalFrame = usePopupFrame(16, true);
  const isOwrx = serverType === 'owrx';
  const isKiwi = isKiwiProtocol(serverType);   // Web-888 has the same DSP surface
  /* ★★★ BOTH PLATFORMS. I hid these on Android on the strength of a COMMENT that said the port was
   *  pending — "Android: accepted no-op (port pending)" beside the two handlers in SDRScreen. It
   *  is stale: VibeStreamModule.setNrMode/setNoiseBlanker set nrMode/nbOn on VibeStreamService,
   *  and the decode path applies them through VibeDSP.kt. The port landed and the comment did not
   *  move. Stuart caught it (2026-09-24) — "nr/nb should also work on android".
   *  ★ The lesson is the file's own: a comment is not evidence. The code two files away was. */
  const uberDsp = !recordingOnly && !isOwrx && !isLocal && !isKiwi;

  // Live signal reading — this sheet covers the signal bar, so show the CURRENT level next to the
  // squelch control (set the gate just above where speech sits / just below where noise shows).
  const liveM = useMeters(meterBus);
  const liveSig = liveM ? meterText(signalMode, liveM) : '';

  // Squelch readout in the DISPLAYED meter unit (S-units when the meter shows S-meter), while the
  // slider's value stays in the backend's NATIVE unit for the wire. dBm/dBFS → S (S9 = −73, 6 dB/S).
  const sqlDisp = (v: number) => {
    if (signalMode === 'smeter') {
      if (v >= -73) { const o = Math.round(v + 73); return o > 0 ? `S9+${o}` : 'S9'; }
      return `S${Math.max(1, 9 - Math.ceil((-73 - v) / 6))}`;
    }
    return `${Math.round(v)}dB`;
  };

  // OWRX squelch/NR sliders — seeded from the server/profile preset (keyed on
  // seq so a profile switch re-syncs even when the new preset equals the old).
  const [owrxSql, setOwrxSql] = useState(-150);
  const [owrxNr,  setOwrxNr]  = useState(0);
  useEffect(() => {
    if (!owrxDspDefaults) return;
    if (owrxDspDefaults.squelchDb !== undefined) setOwrxSql(owrxDspDefaults.squelchDb);
    if (owrxDspDefaults.nrThreshold !== undefined) {
      setOwrxNr(owrxDspDefaults.nrEnabled ? owrxDspDefaults.nrThreshold : 0);
    }
  }, [owrxDspDefaults?.seq]);   // eslint-disable-line react-hooks/exhaustive-deps

  // NR cycle — off→nr→nr2. SERV is locked while the server DSP section is on.
  const [nrMode, setNrMode] = useState<'off' | 'nr' | 'nr2' | 'serv'>(
    serverDspEnabled ? 'serv' : nr ? 'nr' : 'off'
  );
  const cycleNr = useCallback(() => {
    if (nrMode === 'serv') return;   // locked — server DSP section controls this
    const next = nrMode === 'off' ? 'nr' : nrMode === 'nr' ? 'nr2' : 'off';
    setNrMode(next);
    onNr?.(next);
  }, [nrMode, onNr]);
  useEffect(() => {
    if (serverDspEnabled) setNrMode('serv');
    else if (nrMode === 'serv') setNrMode('off');
  }, [serverDspEnabled]);   // eslint-disable-line react-hooks/exhaustive-deps

  // Keyboard / D-pad navigation — shared machinery (PanelNav). Buttons, sliders and
  // the squelch bar all register themselves; the game controller drives this unchanged.
  const { navCtx, scrollProps } = usePanelNav(visible, { onTimeout: onClose });

  return (
    <Modal visible={visible} transparent animationType="slide" onRequestClose={onClose}
           onDismiss={onDismiss}
           supportedOrientations={['portrait', 'landscape', 'landscape-left', 'landscape-right']}>
      {/* ★★★ Transparency OFF: the tap-to-close view stays, the dim goes (PopupScrim). */}
      <PopupScrim style={st.backdrop} color={BACKDROP} onPress={onClose} onTouchStart={noteTouchInteraction} />
      <View style={[st.sheet, {
        borderTopColor: t.barBorder,
        // Landscape: keep clear of the Dynamic Island and don't sprawl the full
        // (very wide) width — cap it and centre it.
        paddingLeft: 16 + insets.left, paddingRight: 16 + insets.right,
        paddingBottom: 40 + insets.bottom,
        alignSelf: 'center', width: '100%', maxWidth: 640,
      }, surf.opaque && !pt.metal && { backgroundColor: surf.fill(SHEET_BG) }, metalFrame,
         metalFrame && { paddingTop: 0 }]}>
        <PopupPlate />
        <PopupHandle />
        <View style={st.titleRow}>
          <SectionIcon name="audio" size={15} color={pt.metal ? pt.label : t.sectionColor} />
          <Text style={[st.sheetLabel, { color: t.sectionColor, fontFamily: t.font, marginBottom: 0 }, st.sheetLabelMetal]}>
            AUDIO
          </Text>
        </View>

        <ScrollView {...scrollProps} style={st.scroll} keyboardShouldPersistTaps="handled">
        <NavCtx.Provider value={navCtx}>

          {/* NR / NB (UberSDR client-side DSP) + REC — REC stays for all backends */}
          <BtnRow>
            {uberDsp && (
              <Btn
                label={nrMode === 'serv' ? 'SERV' : nrMode === 'nr2' ? 'NR2' : 'NR'}
                active={nrMode !== 'off'}
                style={nrMode === 'serv' ? { borderColor: 'rgba(50,210,100,0.60)', backgroundColor: 'rgba(50,210,100,0.10)' } : undefined}
                onPress={cycleNr}
              />
            )}
            {uberDsp && <Btn label="NB" active={nb} onPress={() => onNb?.(!nb)} />}
            {/* No pip (the mockup): REC is an action whose state the timer below reports. */}
            <Btn label="⏺ REC" active={recording} pip={false} onPress={onRec} />
          </BtnRow>
          {recording && (
            // ★ Silver / black: the timer is a readout, so it sits in a window (text colour).
            <PopupWindow style={st.recTimer} metalStyle={st.recTimerWin}>
              <View style={st.recDot} />
              <Text style={st.recTime}>{fmtRecTime(recSeconds)}</Text>
            </PopupWindow>
          )}
          {onRecordings && (
            <BtnRow>
              <Btn label="RECORDINGS" full onPress={onRecordings} />
            </BtnRow>
          )}

          {/* Live signal — set the gate against what you can SEE (this sheet hides the meter bar). */}
          {!recordingOnly && liveSig ? (pt.metal ? (
            // ★ §10.3: the SIGNAL readout in a recessed window, lit in the text colour.
            <PopupWindow metalStyle={st.sigWin}>
              <Text style={st.sigWinLabel}>SIGNAL</Text>
              <Text style={st.sigWinVal}>{liveSig}</Text>
            </PopupWindow>
          ) : (
            <View style={st.bwRow}>
              <Text style={[st.bwLabel, st.sqlLabel]}>SIGNAL</Text>
              <View style={{ flex: 1 }} />
              <Text style={[st.bwVal, { color: pt.gold.readout, fontWeight: '700' }]}>{liveSig}</Text>
            </View>
          )) : null}

          {/* OWRX server-side squelch (dB) + NR (threshold dB). Squelch left =
              Off (open); NR left = Off, slides up for more reduction. */}
          {isOwrx && (<>
            <View style={st.bwRow}>
              <Text style={[st.bwLabel, st.sqlLabel]}>SQUELCH</Text>
              {/* ★ Slider and label on the TRIMMED scale (raw + visualGain); the server gets raw. */}
              <NavSlider style={st.bwSlider}
                minimumValue={-130} maximumValue={-20} step={1}
                value={owrxSql <= -130 ? -130 : Math.max(-129, Math.min(-20, owrxSql + visualGain))}
                onValueChange={(v: number) => { const db = v <= -130 ? -150 : v - visualGain; setOwrxSql(db); onOwrxSquelch?.(db); }}
                minimumTrackTintColor={owrxSql > -130 ? pt.gold.fill : C.muted}
                maximumTrackTintColor={C.muted} thumbTintColor={pt.gold.thumb} />
              <Text style={st.bwVal}>{owrxSql <= -130 ? 'Off' : sqlDisp(owrxSql + visualGain)}</Text>
            </View>
            <View style={st.bwRow}>
              <Text style={st.bwLabel}>NR</Text>
              <NavSlider style={st.bwSlider}
                minimumValue={0} maximumValue={30} step={1}
                value={owrxNr}
                onValueChange={(v: number) => { setOwrxNr(v); onOwrxNr?.(v); }}
                minimumTrackTintColor={owrxNr > 0 ? pt.gold.fill : C.muted}
                maximumTrackTintColor={C.muted} thumbTintColor={pt.gold.thumb} />
              <Text style={st.bwVal}>{owrxNr <= 0 ? 'Off' : `${owrxNr}dB`}</Text>
            </View>
          </>)}

          {/* Local SDR: power-based squelch (dBFS). */}
          {onLocalSquelch ? (
            <View style={st.bwRow}>
              <Text style={[st.bwLabel, st.sqlLabel]}>SQUELCH</Text>
              <SquelchControl level={liveM?.level ?? 0} raw={liveM?.raw} pos={liveM?.sql ?? -1}
                              gate={liveM?.gate} onDrag={onSquelchDrag} onDragEnd={onSquelchDragEnd}
                              auto={sqlAuto} onAuto={onSqlAuto}
                              margin={sqlAutoMargin} onMargin={onSqlAutoMargin}
                              autoOk={sqlAutoOk} />
            </View>
          ) : null}

          {/* Local SDR audio noise reduction — strength slider (0=off..20). */}
          {onLocalNR && (
            <View style={st.bwRow}>
              <Text style={st.bwLabel}>NR</Text>
              <NavSlider style={st.bwSlider}
                minimumValue={0} maximumValue={20} step={1}
                value={localNR}
                onValueChange={(v: number) => onLocalNR?.(v)}
                minimumTrackTintColor={localNR > 0 ? pt.gold.fill : C.muted}
                maximumTrackTintColor={C.muted} thumbTintColor={pt.gold.thumb} />
              <Text style={st.bwVal}>{localNR <= 0 ? 'Off' : String(localNR)}</Text>
            </View>
          )}

          {/* Automatic notch (adaptive line enhancer) — on/off, all backends. */}
          {onNotch && (
            <View style={st.bwRow}>
              <Text style={[st.bwLabel, { width: 78 }]}>AUTO NOTCH</Text>
              <View style={{ flex: 1 }} />
              <Toggle label={notchOn ? 'ON' : 'OFF'} on={notchOn} onPress={() => onNotch?.(!notchOn)} />
            </View>
          )}

          {/* ★ NOISE BLANKER — impulse noise on your own channel (lightning crackle, power-line
              hash, ignition, switch-mode supplies), every mode but broadcast FM, which has its own
              NB in the row below. The web client has had this since the listener NB landed; the
              app did not (Stuart, 2026-09-15: "noise blanker missing"). Same word, same place. */}
          {onNbx && (
            <View style={st.bwRow}>
              <Text style={[st.bwLabel, { width: 78 }]}>NOISE BLANKER</Text>
              <View style={{ flex: 1 }} />
              <Toggle label={nbx ? 'ON' : 'OFF'} on={nbx} onPress={() => onNbx?.(!nbx)} />
            </View>
          )}

          {/* ★ NFM AUDIO — VOICE is how an NFM radio sounds (300 Hz-3 kHz: no CTCSS tone, softer
              hiss); RAW is the flat output for an external decoder that needs the low end. */}
          {onNfmVoice && (
            <View style={st.bwRow}>
              <Text style={[st.bwLabel, { width: 78 }]}>NFM AUDIO</Text>
              <View style={{ flex: 1 }} />
              <Toggle label={nfmVoice ? 'VOICE' : 'RAW'} on={nfmVoice} onPress={() => onNfmVoice?.(!nfmVoice)}
                a11y={nfmVoice ? 'NFM audio: voice filtered. Tap for raw.' : 'NFM audio: raw. Tap for voice filtered.'} />
            </View>
          )}

          {/* ★ UNCOMPRESSED AUDIO — only when the owner allowed the choice. Changing it reopens
              the audio socket, because the codec is a query parameter fixed at connect. */}
          {onRawAudio && (
            <View style={st.bwRow}>
              <Text style={[st.bwLabel, { width: 78 }]}>UNCOMP</Text>
              <View style={{ flex: 1 }} />
              <Toggle label={rawAudio ? 'ON' : 'OFF'} on={rawAudio} onPress={() => onRawAudio(!rawAudio)} />
            </View>
          )}

          {/* ★★★ RAW IQ OUT — a side channel of this session. See the note on the prop. */}
          {onIqOut && (
            <View style={{ marginTop: 6 }}>
              <View style={st.bwRow}>
                <Text style={[st.bwLabel, { width: 78 }]}>RAW IQ</Text>
                <View style={{ flex: 1, flexDirection: 'row', gap: 6 }}>
                  {(iqLocal ? [48000, 96000, 192000, 250000] : [48000]).map(r => (
                    pt.metal ? (
                    <PopupKey key={r} label={`${r / 1000}k`} active={(iq?.rate ?? iqRate) === r} pip hitSlop={6}
                      height={28} fontSize={10} disabled={!!iq?.on && iq.rate !== r}
                      onPress={() => { if (!iq?.on) setIqRate(r); }} />
                  ) : (
                    <TouchableOpacity key={r} onPress={() => { if (!iq?.on) setIqRate(r); }} hitSlop={6}
                      style={{ paddingHorizontal: 8, paddingVertical: 3, borderRadius: 5, borderWidth: 1,
                               borderColor: (iq?.rate ?? iqRate) === r ? pt.gold.sel : C.muted, opacity: iq?.on && iq.rate !== r ? 0.35 : 1 }}>
                      <Text style={{ color: (iq?.rate ?? iqRate) === r ? pt.gold.sel : C.muted, fontFamily: 'Atkinson Hyperlegible', fontSize: 10 }}>{r / 1000}k</Text>
                    </TouchableOpacity>
                  )
                  ))}
                </View>
                <Toggle label={iq?.on ? 'ON' : 'OFF'} on={!!iq?.on} onPress={() => onIqOut(!iq?.on, iqRate)} />
              </View>
              <Text selectable style={st.iqNote}>
                {iq?.on && !iq.public
                  ? `IQ out is on at ${(iq.rate ?? iqRate) / 1000} kHz. Connect your rtl_tcp app to ${iq.host}:${iq.port}. Tuning from that app moves this dial; audio here keeps playing.`
                  : iq?.on && iq.public
                  ? `IQ out is on at ${(iq.rate ?? 48000) / 1000} kHz. Open VibeIQ and enter the code ${iq.code}, then connect your rtl_tcp app to 127.0.0.1:1234.`
                  : 'Your channel as raw IQ, in rtl_tcp form, for a decoder this app does not carry — digital voice, say. 48 kHz covers every digital voice mode.'}
              </Text>
            </View>
          )}

          {/* ★ FM DE-EMPHASIS — ours, not the radio's. Order matches the web client: NR, notch,
              de-emphasis, stereo. 50µs Europe/UK, 75µs Americas/Korea. Tau is in SECONDS on the
              wire, which is easy to get wrong — see the wire-units note. */}
          {onDeemph && (
            <View style={st.bwRow}>
              <Text style={[st.bwLabel, { width: 78 }]}>DE-EMPH</Text>
              <View style={{ flex: 1 }} />
              {([{ l: 'OFF', v: 0 }, { l: '50µs', v: 50e-6 }, { l: '75µs', v: 75e-6 }]).map((o) => (
                <Toggle key={o.l} label={o.l} on={deemph === o.v} onPress={() => onDeemph(o.v)} padH={10} marginLeft={6} />
              ))}
            </View>
          )}

          {/* ★ WFM STEREO — off forces mono, which is cleaner on a weak or noisy signal. */}
          {onStereo && (
            <View style={st.bwRow}>
              <Text style={[st.bwLabel, { width: 78 }]}>WFM STEREO</Text>
              <View style={{ flex: 1 }} />
              <Toggle label={stereo ? 'ON' : 'OFF'} on={stereo} onPress={() => onStereo(!stereo)} />
            </View>
          )}

          {/* ★★ THE BROADCAST-FM TREATMENTS, on one row because they are one subject: what the
              receiver is doing about a difficult FM signal. Compact labels — the phone has no room
              for four full-width rows, and a DXer reading them knows the initials.
              ★ NR works in MONO too: the audio high-cut treats baseband hiss, which is there
                whether you are in stereo or not; only the stereo half needs L-R. */}
          {(onFmNr || onFmIms || onFmCeq || onFmNb || onFmAutoBw) && (
            <View style={st.bwRow}>
              {/* ★★ "BROADCAST FM", not "FM DSP" — and the row label is load-bearing. This sheet
                  ALREADY has NR and NB buttons a few rows up, and those are the app's OWN
                  client-side audio DSP; these are the RADIO's, they act only on broadcast FM, and
                  they are shared with every other listener on that receiver. Two controls sharing
                  a name and meaning different things is a trap, so the row says which family these
                  belong to. The names themselves match the web client deliberately: same product,
                  same words. */}
              <Text style={[st.bwLabel, { width: 78 }]}>BCAST FM</Text>
              <View style={{ flex: 1 }} />
              {([
                { l: 'NR',  on: fmNr !== false,  cb: onFmNr },
                { l: 'IMS', on: fmIms !== false, cb: onFmIms },
                { l: 'CEQ', on: fmCeq !== false, cb: onFmCeq },
                { l: 'NB',  on: fmNb !== false,  cb: onFmNb },
                /* ★ AUTO BW is SERVER-WIDE — it changes the demodulator for everybody, not this
                 *   listener's own processing like the four above. Same row because it is the same
                 *   family of FM treatments and the same words the web client uses. */
                { l: 'A-BW', on: fmAutoBw === true, cb: onFmAutoBw },
              ] as const).filter((o) => !!o.cb).map((o) => (
                <Toggle key={o.l} label={o.l} on={o.on} onPress={() => o.cb?.(!o.on)} padH={9} marginLeft={6} />
              ))}
            </View>
          )}

          {/* Kiwi squelch — client-side dBFS gate (dBm threshold, −130 = Off). */}
          {onKiwiSquelch && (
            <View style={st.bwRow}>
              <Text style={[st.bwLabel, st.sqlLabel]}>SQUELCH</Text>
              <SquelchControl level={liveM?.level ?? 0} raw={liveM?.raw} pos={liveM?.sql ?? -1}
                              gate={liveM?.gate} onDrag={onSquelchDrag} onDragEnd={onSquelchDragEnd}
                              auto={sqlAuto} onAuto={onSqlAuto}
                              margin={sqlAutoMargin} onMargin={onSqlAutoMargin}
                              autoOk={sqlAutoOk} />
            </View>
          )}

          {/* SNR Squelch — UberSDR audio gate (0–50 dB in our meter's units). */}
          {!recordingOnly && !onLocalSquelch && !onKiwiSquelch && !isOwrx && (
            <View style={st.bwRow}>
              <Text style={[st.bwLabel, st.sqlLabel]}>SQUELCH</Text>
              <SquelchControl level={liveM?.level ?? 0} raw={liveM?.raw} pos={liveM?.sql ?? -1}
                              gate={liveM?.gate} onDrag={onSquelchDrag} onDragEnd={onSquelchDragEnd}
                              auto={sqlAuto} onAuto={onSqlAuto}
                              margin={sqlAutoMargin} onMargin={onSqlAutoMargin}
                              autoOk={sqlAutoOk} />
            </View>
          )}

          {/* FM Squelch — only for fm/nfm. */}
          {!isOwrx && isFmMode && (
            <View style={st.bwRow}>
              <Text style={st.bwLabel}>FM SQL</Text>
              <NavSlider style={st.bwSlider}
                minimumValue={0} maximumValue={100} step={1}
                value={fmSquelch <= -999 ? 0 : Math.round((fmSquelch + 48) * 99 / 68 + 1)}
                onValueChange={(v: number) => {
                  const db = v === 0 ? -999 : -48 + (v - 1) * (68 / 99);
                  onFmSquelch?.(db);
                }}
                minimumTrackTintColor={fmSquelch > -999 ? pt.gold.fill : C.muted}
                maximumTrackTintColor={C.muted} thumbTintColor={pt.gold.thumb} />
              <Text style={st.bwVal}>{fmSquelch <= -999 ? 'Open' : `${fmSquelch.toFixed(1)}dB`}</Text>
            </View>
          )}

          {/* ── SERVER SIDE NR (DSP insert) — only when the server advertises
                 filters (UberSDR). Type selector + per-filter params. ── */}
          {dspFilters.length > 0 && (<>
            <SectionLabel label="SERVER SIDE NR" icon="nr" />
            <BtnRow>
              <Btn
                label={serverDspEnabled ? 'DISABLE SERVER NR' : 'ENABLE SERVER NR'}
                active={serverDspEnabled}
                full
                style={serverDspEnabled ? { borderColor: 'rgba(50,210,100,0.50)', backgroundColor: 'rgba(50,210,100,0.10)' } : undefined}
                onPress={() => onServerDsp?.(!serverDspEnabled)}
              />
            </BtnRow>
            {dspError != null && <Text style={st.dspError}>{dspError}</Text>}
            {serverDspEnabled && (
              <View style={st.subPanel}>
                <SubLabel label="DSP TYPE" />
                <View style={[st.btnRow, { paddingTop: 2, paddingBottom: 0 }]}>
                  {dspFilters.map((f: DspFilterDesc) => (
                    <SegBtn key={f.name} label={f.name.toUpperCase()}
                            active={serverDspFilter === f.name}
                            onPress={() => onServerDspFilter?.(f.name)} />
                  ))}
                </View>
                {(dspFilters.find((f: DspFilterDesc) => f.name === serverDspFilter)?.params ?? [])
                  .filter((p: DspParamDesc) => p.runtime_safe !== false)
                  .map((p: DspParamDesc) => {
                    const val = serverDspParams[p.name] ?? p.default ?? '';
                    if ((p.type ?? 'float').toLowerCase() === 'bool') {
                      return (
                        <BtnRow key={p.name}>
                          <Btn label={fmtParamName(p.name)} active={val === 'true'} full
                               onPress={() => onServerDspParam?.(p.name, val === 'true' ? 'false' : 'true')} />
                        </BtnRow>
                      );
                    }
                    const min = parseFloat(p.min ?? ''), max = parseFloat(p.max ?? '');
                    if (!Number.isFinite(min) || !Number.isFinite(max) || max <= min) return null;
                    const step = dspStep(min, max);
                    const num  = Number.isFinite(parseFloat(val)) ? parseFloat(val) : min;
                    return (
                      <View key={p.name} style={st.bwRow}>
                        <Text style={st.bwLabel} numberOfLines={1}>{fmtParamName(p.name)}</Text>
                        <NavSlider style={st.bwSlider}
                          minimumValue={min} maximumValue={max} step={step}
                          value={Math.max(min, Math.min(max, num))}
                          onValueChange={(v: number) => onServerDspParam?.(p.name, fmtDspVal(v, step))}
                          minimumTrackTintColor={pt.gold.fill} maximumTrackTintColor={C.muted}
                          thumbTintColor={pt.gold.thumb} />
                        <Text style={st.bwVal}>{fmtDspVal(num, step)}</Text>
                      </View>
                    );
                  })}
              </View>
            )}
          </>)}

                </NavCtx.Provider>
        </ScrollView>

        {pt.metal ? (
          <PopupKey label="CLOSE" onPress={onClose} height={32} style={{ alignSelf: 'center', width: 110, marginTop: 14 }} />
        ) : (
        <TouchableOpacity style={[st.closeBtn, { borderColor: t.btnBorder }]} onPress={onClose}>
          <Text style={[st.closeBtnText, { fontFamily: t.font, color: t.btnText }]}>CLOSE</Text>
        </TouchableOpacity>
        )}
      </View>
    </Modal>
  );
}

const makeSt = (pt: PopupTokens) => StyleSheet.create({
  backdrop:   { flex: 1 },
  sheet: {
    backgroundColor: SHEET_BG,
    borderTopWidth: 1, borderRadius: 14,
    padding: 16, paddingBottom: 40,
  },
  sheetLabel: { textAlign: 'center', fontSize: 10, letterSpacing: 3, marginBottom: 12 },
  titleRow: { flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: 7, marginBottom: 12 },
  sectionRow: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  scroll:     { maxHeight: 420 },

  sectionBar: onMetal(pt, {
    borderTopWidth: StyleSheet.hairlineWidth, borderTopColor: C.divider,
    paddingTop: 12, paddingBottom: 6, marginTop: 6,
  }, { borderTopColor: pt.rule }),
  sectionLabel: onMetal(pt, {
    color: C.sectionC, fontFamily: 'Atkinson Hyperlegible', fontSize: 12,
    fontWeight: 'bold', letterSpacing: 2,
  }, { ...engraveText(pt), fontSize: 11, letterSpacing: 2.2 }),

  btnFocused:    { borderColor: NAV_FOCUS, borderWidth: 2 },
  // The bar has no border of its own, so focus is a ring drawn around it rather than a
  // thickened edge — and it must be visible, since without it you cannot tell the arrows
  // are about to move the squelch rather than the focus.
  sqlBarFocused: { borderWidth: 2, borderColor: NAV_FOCUS, borderRadius: 6, margin: -2 },
  btnRow:  { flexDirection: 'row', flexWrap: 'wrap', gap: 6, paddingVertical: 4 },
  btn: {
    backgroundColor: C.btnBg, borderWidth: 1, borderColor: C.border,
    borderRadius: 5, paddingHorizontal: 16, paddingVertical: 11,
    alignItems: 'center', justifyContent: 'center',
  },
  btnActive:     { backgroundColor: C.active, borderColor: pt.gold.selBorder },
  btnFull:       { flex: 1, alignSelf: 'stretch' },
  btnText:       { color: C.muted, fontFamily: 'Atkinson Hyperlegible', fontSize: 15, fontWeight: 'bold', letterSpacing: 0.5 },
  btnTextActive: { color: pt.gold.sel },

  bwRow:    { flexDirection: 'row', alignItems: 'center', gap: 6, paddingVertical: 2 },
  bwLabel:  onMetal(pt, { color: C.sectionC, fontFamily: 'Atkinson Hyperlegible', fontSize: 11, letterSpacing: 1, width: 32 }, { ...engraveText(pt), fontSize: 10, letterSpacing: 1.4 }),
  bwSlider: { flex: 1, height: 32 },
  bwVal:    onMetal(pt, { color: pt.gold.value, fontFamily: 'Atkinson Hyperlegible', fontSize: 11, minWidth: 68, textAlign: 'right' }, { ...engraveText(pt, pt.value), fontWeight: '700' }),
  // The squelch meter IS the control, so it gets a slider's worth of height and touch target —
  // it replaced the slider rather than sitting under it.
  // 40 tall: a 22px ball on top, its needle dropping through the 14px bar parked at the bottom.
  // The ball has to sit ABOVE the bar or your finger covers the very signal you're aiming at.
  // marginTop buys the ball its own air: it overhangs the top of the bar, and without this it
  // collides with whatever row sits above (AUTO NOTCH).
  sqlBarWrap:  { height: 40, marginTop: 8, position: 'relative', justifyContent: 'flex-end' },
  // "SQUELCH" needs more than the 32pt the short labels use, or it wraps to "SQUE / LCH".
  sqlLabel:    { width: 62 },
  // Sets the expectation that a gate sitting ON the noise will chatter a little — otherwise that
  // reads as a bug rather than as physics.
  sqlHint:     onMetal(pt, { color: 'rgba(255,255,255,0.45)', fontFamily: 'Atkinson Hyperlegible',
                 fontSize: 10, lineHeight: 13, paddingTop: 4, paddingBottom: 2 }, engraveText(pt, pt.note)),
  sqlBarTrack: { height: 14, borderRadius: 7, backgroundColor: 'rgba(255,255,255,0.15)',
                 overflow: 'hidden' },
  sqlBarFill:  { position: 'absolute', left: 0, top: 0, bottom: 0 },
  // Needle + ball live OUTSIDE the track: the track clips its fill, and both must overhang it.
  sqlNeedle:   { position: 'absolute', top: 11, bottom: 0, width: 2, marginLeft: -1,
                 backgroundColor: '#3ddc84' },
  sqlBall:     { position: 'absolute', top: 0, width: 22, height: 22, borderRadius: 11,
                 marginLeft: -11, backgroundColor: '#3ddc84',
                 borderWidth: 2, borderColor: 'rgba(0,0,0,0.55)' },
  // ── Auto squelch ─────────────────────────────────────────────────────────────────────────────
  // ★ Faded, not hidden — the signal is still what you watch. 0.55 keeps the fill readable while
  //   saying clearly that the threshold is not yours at the moment.
  sqlBarAuto:  { opacity: 0.55 },
  // ★ RED, and it runs the full height of the bar: the same indicator the main signal meter uses
  //   for an automatic threshold, so the two screens read as one thing.
  sqlNeedleAuto: { top: 0, width: 2, backgroundColor: '#ff4b4b' },
  sqlAutoOverlay: { position: 'absolute', left: 0, right: 0, bottom: 0, height: 14,
                    alignItems: 'center', justifyContent: 'center' },
  sqlAutoOverlayTxt: onMetal(pt, { color: 'rgba(255,255,255,0.9)', fontFamily: 'Atkinson Hyperlegible',
                       fontSize: 9, letterSpacing: 1.2 }, { ...engraveText(pt, pt.note), position: 'absolute', top: 0, left: 0, right: 0, textAlign: 'center' }),
  sqlAutoRow:  { flexDirection: 'row', alignItems: 'center', gap: 8, paddingTop: 6 },
  // ★ The reason a disabled toggle is disabled, beside it — never a switch that silently does
  //   nothing. Wraps, because the sentence matters more than the row height.
  sqlAutoWhy:  onMetal(pt, { color: 'rgba(255,255,255,0.45)', fontFamily: 'Atkinson Hyperlegible',
                 fontSize: 10, lineHeight: 13, flex: 1 }, engraveText(pt, pt.note)),
  sqlAutoMarginRow: { flexDirection: 'row', alignItems: 'center', paddingTop: 2 },
  sqlAutoMarginCap: onMetal(pt, { color: C.sectionC, fontFamily: 'Atkinson Hyperlegible', fontSize: 10,
                      letterSpacing: 1, width: 84 }, engraveText(pt)),

  subPanel: onMetal(pt, {
    backgroundColor: 'rgba(255,255,255,0.04)', borderRadius: 6,
    borderWidth: StyleSheet.hairlineWidth, borderColor: C.divider,
    padding: 10, marginBottom: 4,
  }, { backgroundColor: 'transparent', borderColor: pt.rule }),
  subLabel: onMetal(pt, { color: C.sectionC, fontFamily: 'Atkinson Hyperlegible', fontSize: 12, letterSpacing: 1, paddingTop: 8, paddingBottom: 3 }, engraveText(pt)),

  recTimer: { flexDirection: 'row', alignItems: 'center', gap: 8, paddingVertical: 4 },
  recDot:   { width: 8, height: 8, borderRadius: 4, backgroundColor: '#cc2222' },
  recTime:  onMetal(pt, { color: pt.gold.readout, fontFamily: 'Atkinson Hyperlegible', fontSize: 13 }, { fontWeight: '700' }),
  dspError: onMetal(pt, { color: 'rgba(220,53,69,0.95)', fontFamily: 'Atkinson Hyperlegible', fontSize: 13, paddingBottom: 6 }, { color: pt.danger }),

  closeBtn: {
    marginTop: 14, alignSelf: 'center', borderWidth: 1,
    borderRadius: 3, paddingVertical: 7, paddingHorizontal: 24,
  },
  closeBtnText: { fontSize: 11 },
  sheetLabelMetal: onMetal(pt, {}, { ...engraveText(pt), fontSize: 11, letterSpacing: 2.2, fontWeight: '700' }),
  recTimerWin: { paddingHorizontal: 10, paddingVertical: 6, marginVertical: 4 },
  sigWin: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', paddingHorizontal: 10, paddingVertical: 6, marginVertical: 4 },
  sigWinLabel: { color: pt.winDim, fontFamily: 'Atkinson Hyperlegible', fontSize: 10, letterSpacing: 1.4 },
  sigWinVal: { color: pt.readout, fontFamily: 'Atkinson Hyperlegible', fontSize: 13, fontWeight: '700', textShadowColor: pt.readoutGlow, textShadowRadius: 5, textShadowOffset: { width: 0, height: 0 } },
  iqNote: onMetal(pt, { color: C.muted, fontFamily: 'Atkinson Hyperlegible', fontSize: 11, lineHeight: 15, marginTop: 4 }, engraveText(pt, pt.note)),
  fBox: { height: 24, justifyContent: 'center' },
  fSlot: { position: 'absolute', left: 0, right: 0, top: 9, height: 6, borderRadius: 3 },
  fSlotLip: { position: 'absolute', left: 3, right: 3, top: 15, height: 1 },
  fFill: { position: 'absolute', left: 1, top: 10, height: 4, borderRadius: 2, shadowOpacity: 1, shadowRadius: 3, shadowOffset: { width: 0, height: 0 } },
  fAutoLine: { position: 'absolute', top: 2, bottom: 2, width: 2, marginLeft: -1, backgroundColor: '#ff4b4b' },
  fCap: { position: 'absolute', top: 3, width: 20, height: 18, marginLeft: -10, borderRadius: 4, borderWidth: 1, overflow: 'hidden', shadowColor: '#000', shadowOpacity: 0.6, shadowRadius: 1, shadowOffset: { width: 0, height: 2 }, elevation: 2 },
  fCapTop: { position: 'absolute', left: 0, right: 0, top: 0, height: '45%' },
  fCapBot: { position: 'absolute', left: 0, right: 0, bottom: 0, height: '30%' },
  fCapHi: { position: 'absolute', left: 1, right: 1, top: 0, height: 1 },
  fCapLine: { position: 'absolute', left: 8, top: 3, bottom: 3, width: 2, shadowOpacity: 1, shadowRadius: 2, shadowOffset: { width: 0, height: 0 } },
});
