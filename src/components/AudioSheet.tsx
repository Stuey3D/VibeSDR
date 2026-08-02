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

// Local copy of the menu's accessibility palette so this sheet is self-contained
// (no shared-internals refactor of MenuSheet). Values mirror MenuSheet's `C`.
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
  return (
    <Slider ref={viewRef as any} {...props}
      minimumTrackTintColor={focused ? NAV_FOCUS : props.minimumTrackTintColor}
      thumbTintColor={focused ? NAV_FOCUS : props.thumbTintColor} />
  );
}

function SquelchBar({ level, pos, gate, onDrag, onDragEnd }: {
  level: number; pos: number; gate?: boolean;
  /** ★ A NORMALISED value: 0..1 along the bar, or -1 for off. NOT a coordinate — the
   *  parameter was called `x` and that misreading cost a real bug (see the nav note below). */
  onDrag?: (v: number) => void;
  onDragEnd?: () => void;
}) {
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

  const shown = held ?? pos;
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

  return (
    <View ref={(r: any) => { (bar as any).current = r; (navViewRef as any).current = r; }}
          style={[st.sqlBarWrap, navFocused && st.sqlBarFocused]}
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
      <View pointerEvents="none" style={[st.sqlNeedle, { left: handleX },
                    off && { backgroundColor: 'rgba(255,255,255,0.35)' }]} />
      <View pointerEvents="none" style={[st.sqlBall,   { left: handleX },
                    off && { backgroundColor: 'rgba(255,255,255,0.35)' }]} />
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
  return (
    <View style={st.sectionBar}>
      <View style={st.sectionRow}>
        {icon && <SectionIcon name={icon} size={16} color={C.sectionC} />}
        <Text style={st.sectionLabel}>{label}</Text>
      </View>
    </View>
  );
}
function BtnRow({ children }: { children: React.ReactNode }) {
  return <NavRow><View style={st.btnRow}>{children}</View></NavRow>;
}
function Btn({ label, active, onPress, full, style }: {
  label: string; active?: boolean; onPress?: () => void; full?: boolean; style?: object;
}) {
  const { focused, viewRef } = useNavButton(onPress);
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
  return <Text style={st.subLabel}>{label}</Text>;
}
function SegBtn({ label, active, onPress }: { label: string; active: boolean; onPress: () => void }) {
  const { focused, viewRef } = useNavButton(onPress);
  return (
    <TouchableOpacity ref={viewRef as any}
      style={[st.btn, active && st.btnActive, focused && st.btnFocused]}
      onPress={onPress} hitSlop={4} activeOpacity={0.7}>
      <Text style={[st.btnText, active && st.btnTextActive]}>{label}</Text>
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
  serverType?: string;         // 'ubersdr' | 'owrx' | 'kiwi'
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
  deemph?: number;             // FM de-emphasis tau, SECONDS (0 = off, 50e-6, 75e-6)
  onDeemph?: (tau: number) => void;
  stereo?: boolean;            // WFM stereo on, vs forced mono
  onStereo?: (on: boolean) => void;

  // OWRX server-side squelch (dB) + NR (threshold dB)
  onOwrxSquelch?: (db: number) => void;
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
  nr = false, onNr, nb = false, onNb,
  recording = false, onRec, recSeconds = 0, onRecordings,
  snrSquelch = -999, onSnrSquelch,
  localSquelch = -100, onLocalSquelch,
  localNR = 0, onLocalNR,
  kiwiSquelch = 0, onKiwiSquelch, onSquelchDrag, onSquelchDragEnd,
  fmSquelch = -999, onFmSquelch, isFmMode = false,
  notchOn = false, onNotch,
  deemph = 50e-6, onDeemph, stereo = true, onStereo,
  rawAudio = false, onRawAudio,
  onOwrxSquelch, onOwrxNr, owrxDspDefaults,
  serverDspEnabled = false, serverDspFilter = '', serverDspParams = {},
  dspFilters = [], dspError = null, onServerDsp, onServerDspFilter, onServerDspParam,
}: AudioSheetProps) {
  const { theme: t } = useTheme();
  const insets = useSafeAreaInsets();
  const isOwrx = serverType === 'owrx';
  const isKiwi = serverType === 'kiwi';
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
      <Pressable style={st.backdrop} onPress={onClose} onTouchStart={noteTouchInteraction} />
      <View style={[st.sheet, {
        borderTopColor: t.barBorder,
        // Landscape: keep clear of the Dynamic Island and don't sprawl the full
        // (very wide) width — cap it and centre it.
        paddingLeft: 16 + insets.left, paddingRight: 16 + insets.right,
        paddingBottom: 40 + insets.bottom,
        alignSelf: 'center', width: '100%', maxWidth: 640,
      }]}>
        <View style={st.titleRow}>
          <SectionIcon name="audio" size={15} color={t.sectionColor} />
          <Text style={[st.sheetLabel, { color: t.sectionColor, fontFamily: t.font, marginBottom: 0 }]}>
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
            <Btn label="⏺ REC" active={recording} onPress={onRec} />
          </BtnRow>
          {recording && (
            <View style={st.recTimer}>
              <View style={st.recDot} />
              <Text style={st.recTime}>{fmtRecTime(recSeconds)}</Text>
            </View>
          )}
          {onRecordings && (
            <BtnRow>
              <Btn label="RECORDINGS" full onPress={onRecordings} />
            </BtnRow>
          )}

          {/* Live signal — set the gate against what you can SEE (this sheet hides the meter bar). */}
          {!recordingOnly && liveSig ? (
            <View style={st.bwRow}>
              <Text style={[st.bwLabel, st.sqlLabel]}>SIGNAL</Text>
              <View style={{ flex: 1 }} />
              <Text style={[st.bwVal, { color: C.gold, fontWeight: '700' }]}>{liveSig}</Text>
            </View>
          ) : null}

          {/* OWRX server-side squelch (dB) + NR (threshold dB). Squelch left =
              Off (open); NR left = Off, slides up for more reduction. */}
          {isOwrx && (<>
            <View style={st.bwRow}>
              <Text style={[st.bwLabel, st.sqlLabel]}>SQUELCH</Text>
              <NavSlider style={st.bwSlider}
                minimumValue={-130} maximumValue={-20} step={1}
                value={owrxSql <= -130 ? -130 : owrxSql}
                onValueChange={(v: number) => { const db = v <= -130 ? -150 : v; setOwrxSql(db); onOwrxSquelch?.(db); }}
                minimumTrackTintColor={owrxSql > -130 ? C.gold : C.muted}
                maximumTrackTintColor={C.muted} thumbTintColor={C.gold} />
              <Text style={st.bwVal}>{owrxSql <= -130 ? 'Off' : sqlDisp(owrxSql)}</Text>
            </View>
            <View style={st.bwRow}>
              <Text style={st.bwLabel}>NR</Text>
              <NavSlider style={st.bwSlider}
                minimumValue={0} maximumValue={30} step={1}
                value={owrxNr}
                onValueChange={(v: number) => { setOwrxNr(v); onOwrxNr?.(v); }}
                minimumTrackTintColor={owrxNr > 0 ? C.gold : C.muted}
                maximumTrackTintColor={C.muted} thumbTintColor={C.gold} />
              <Text style={st.bwVal}>{owrxNr <= 0 ? 'Off' : `${owrxNr}dB`}</Text>
            </View>
          </>)}

          {/* Local SDR: power-based squelch (dBFS). */}
          {onLocalSquelch ? (
            <View style={st.bwRow}>
              <Text style={[st.bwLabel, st.sqlLabel]}>SQUELCH</Text>
              <View style={{ flex: 1 }}>
                <SquelchBar level={liveM?.level ?? 0} pos={liveM?.sql ?? -1}
                             gate={liveM?.gate} onDrag={onSquelchDrag} onDragEnd={onSquelchDragEnd} />
                <Text style={st.sqlHint}>
                  Your squelch stays at the signal level you set. The needle drifts a little here
                  and on the live meter as the noise floor moves — that's normal, not the setting
                  changing.
                </Text>
              </View>
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
                minimumTrackTintColor={localNR > 0 ? C.gold : C.muted}
                maximumTrackTintColor={C.muted} thumbTintColor={C.gold} />
              <Text style={st.bwVal}>{localNR <= 0 ? 'Off' : String(localNR)}</Text>
            </View>
          )}

          {/* Automatic notch (adaptive line enhancer) — on/off, all backends. */}
          {onNotch && (
            <View style={st.bwRow}>
              <Text style={[st.bwLabel, { width: 78 }]}>AUTO NOTCH</Text>
              <View style={{ flex: 1 }} />
              <TouchableOpacity onPress={() => onNotch?.(!notchOn)} hitSlop={8}
                style={{ paddingHorizontal: 16, paddingVertical: 4, borderRadius: 6,
                         backgroundColor: notchOn ? C.gold : 'transparent',
                         borderWidth: 1, borderColor: notchOn ? C.gold : C.muted }}>
                <Text style={{ color: notchOn ? '#000' : C.muted,
                               fontFamily: 'Atkinson Hyperlegible', fontSize: 11, letterSpacing: 1 }}>
                  {notchOn ? 'ON' : 'OFF'}
                </Text>
              </TouchableOpacity>
            </View>
          )}

          {/* ★ UNCOMPRESSED AUDIO — only when the owner allowed the choice. Changing it reopens
              the audio socket, because the codec is a query parameter fixed at connect. */}
          {onRawAudio && (
            <View style={st.bwRow}>
              <Text style={[st.bwLabel, { width: 78 }]}>UNCOMP</Text>
              <View style={{ flex: 1 }} />
              <TouchableOpacity onPress={() => onRawAudio(!rawAudio)} hitSlop={8}
                style={{ paddingHorizontal: 16, paddingVertical: 4, borderRadius: 6,
                         backgroundColor: rawAudio ? C.gold : 'transparent',
                         borderWidth: 1, borderColor: rawAudio ? C.gold : C.muted }}>
                <Text style={{ color: rawAudio ? '#000' : C.muted,
                               fontFamily: 'Atkinson Hyperlegible', fontSize: 11, letterSpacing: 1 }}>
                  {rawAudio ? 'ON' : 'OFF'}
                </Text>
              </TouchableOpacity>
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
                <TouchableOpacity key={o.l} onPress={() => onDeemph(o.v)} hitSlop={6}
                  style={{ paddingHorizontal: 10, paddingVertical: 4, borderRadius: 6, marginLeft: 6,
                           backgroundColor: deemph === o.v ? C.gold : 'transparent',
                           borderWidth: 1, borderColor: deemph === o.v ? C.gold : C.muted }}>
                  <Text style={{ color: deemph === o.v ? '#000' : C.muted,
                                 fontFamily: 'Atkinson Hyperlegible', fontSize: 11, letterSpacing: 1 }}>
                    {o.l}
                  </Text>
                </TouchableOpacity>
              ))}
            </View>
          )}

          {/* ★ WFM STEREO — off forces mono, which is cleaner on a weak or noisy signal. */}
          {onStereo && (
            <View style={st.bwRow}>
              <Text style={[st.bwLabel, { width: 78 }]}>WFM STEREO</Text>
              <View style={{ flex: 1 }} />
              <TouchableOpacity onPress={() => onStereo(!stereo)} hitSlop={8}
                style={{ paddingHorizontal: 16, paddingVertical: 4, borderRadius: 6,
                         backgroundColor: stereo ? C.gold : 'transparent',
                         borderWidth: 1, borderColor: stereo ? C.gold : C.muted }}>
                <Text style={{ color: stereo ? '#000' : C.muted,
                               fontFamily: 'Atkinson Hyperlegible', fontSize: 11, letterSpacing: 1 }}>
                  {stereo ? 'ON' : 'OFF'}
                </Text>
              </TouchableOpacity>
            </View>
          )}

          {/* Kiwi squelch — client-side dBFS gate (dBm threshold, −130 = Off). */}
          {onKiwiSquelch && (
            <View style={st.bwRow}>
              <Text style={[st.bwLabel, st.sqlLabel]}>SQUELCH</Text>
              <View style={{ flex: 1 }}>
                <SquelchBar level={liveM?.level ?? 0} pos={liveM?.sql ?? -1}
                             gate={liveM?.gate} onDrag={onSquelchDrag} onDragEnd={onSquelchDragEnd} />
                <Text style={st.sqlHint}>
                  Your squelch stays at the signal level you set. The needle drifts a little here
                  and on the live meter as the noise floor moves — that's normal, not the setting
                  changing.
                </Text>
              </View>
            </View>
          )}

          {/* SNR Squelch — UberSDR audio gate (0–50 dB in our meter's units). */}
          {!recordingOnly && !onLocalSquelch && !onKiwiSquelch && !isOwrx && (
            <View style={st.bwRow}>
              <Text style={[st.bwLabel, st.sqlLabel]}>SQUELCH</Text>
              <View style={{ flex: 1 }}>
                <SquelchBar level={liveM?.level ?? 0} pos={liveM?.sql ?? -1}
                             gate={liveM?.gate} onDrag={onSquelchDrag} onDragEnd={onSquelchDragEnd} />
                <Text style={st.sqlHint}>
                  Your squelch stays at the signal level you set. The needle drifts a little here
                  and on the live meter as the noise floor moves — that's normal, not the setting
                  changing.
                </Text>
              </View>
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
                minimumTrackTintColor={fmSquelch > -999 ? C.gold : C.muted}
                maximumTrackTintColor={C.muted} thumbTintColor={C.gold} />
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
                          minimumTrackTintColor={C.gold} maximumTrackTintColor={C.muted}
                          thumbTintColor={C.gold} />
                        <Text style={st.bwVal}>{fmtDspVal(num, step)}</Text>
                      </View>
                    );
                  })}
              </View>
            )}
          </>)}

                </NavCtx.Provider>
        </ScrollView>

        <TouchableOpacity style={[st.closeBtn, { borderColor: t.btnBorder }]} onPress={onClose}>
          <Text style={[st.closeBtnText, { fontFamily: t.font, color: t.btnText }]}>CLOSE</Text>
        </TouchableOpacity>
      </View>
    </Modal>
  );
}

const st = StyleSheet.create({
  backdrop:   { flex: 1, backgroundColor: 'rgba(0,0,0,0.50)' },
  sheet: {
    backgroundColor: 'rgba(8,6,1,0.97)',
    borderTopWidth: 1, borderRadius: 14,
    padding: 16, paddingBottom: 40,
  },
  sheetLabel: { textAlign: 'center', fontSize: 10, letterSpacing: 3, marginBottom: 12 },
  titleRow: { flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: 7, marginBottom: 12 },
  sectionRow: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  scroll:     { maxHeight: 420 },

  sectionBar: {
    borderTopWidth: StyleSheet.hairlineWidth, borderTopColor: C.divider,
    paddingTop: 12, paddingBottom: 6, marginTop: 6,
  },
  sectionLabel: {
    color: C.sectionC, fontFamily: 'Atkinson Hyperlegible', fontSize: 12,
    fontWeight: 'bold', letterSpacing: 2,
  },

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
  btnActive:     { backgroundColor: C.active, borderColor: C.goldDim },
  btnFull:       { flex: 1, alignSelf: 'stretch' },
  btnText:       { color: C.muted, fontFamily: 'Atkinson Hyperlegible', fontSize: 15, fontWeight: 'bold', letterSpacing: 0.5 },
  btnTextActive: { color: C.gold },

  bwRow:    { flexDirection: 'row', alignItems: 'center', gap: 6, paddingVertical: 2 },
  bwLabel:  { color: C.sectionC, fontFamily: 'Atkinson Hyperlegible', fontSize: 11, letterSpacing: 1, width: 32 },
  bwSlider: { flex: 1, height: 32 },
  bwVal:    { color: C.gold, fontFamily: 'Atkinson Hyperlegible', fontSize: 11, minWidth: 68, textAlign: 'right' },
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
  sqlHint:     { color: 'rgba(255,255,255,0.45)', fontFamily: 'Atkinson Hyperlegible',
                 fontSize: 10, lineHeight: 13, paddingTop: 4, paddingBottom: 2 },
  sqlBarTrack: { height: 14, borderRadius: 7, backgroundColor: 'rgba(255,255,255,0.15)',
                 overflow: 'hidden' },
  sqlBarFill:  { position: 'absolute', left: 0, top: 0, bottom: 0 },
  // Needle + ball live OUTSIDE the track: the track clips its fill, and both must overhang it.
  sqlNeedle:   { position: 'absolute', top: 11, bottom: 0, width: 2, marginLeft: -1,
                 backgroundColor: '#3ddc84' },
  sqlBall:     { position: 'absolute', top: 0, width: 22, height: 22, borderRadius: 11,
                 marginLeft: -11, backgroundColor: '#3ddc84',
                 borderWidth: 2, borderColor: 'rgba(0,0,0,0.55)' },

  subPanel: {
    backgroundColor: 'rgba(255,255,255,0.04)', borderRadius: 6,
    borderWidth: StyleSheet.hairlineWidth, borderColor: C.divider,
    padding: 10, marginBottom: 4,
  },
  subLabel: { color: C.sectionC, fontFamily: 'Atkinson Hyperlegible', fontSize: 12, letterSpacing: 1, paddingTop: 8, paddingBottom: 3 },

  recTimer: { flexDirection: 'row', alignItems: 'center', gap: 8, paddingVertical: 4 },
  recDot:   { width: 8, height: 8, borderRadius: 4, backgroundColor: '#cc2222' },
  recTime:  { color: C.gold, fontFamily: 'Atkinson Hyperlegible', fontSize: 13 },
  dspError: { color: 'rgba(220,53,69,0.95)', fontFamily: 'Atkinson Hyperlegible', fontSize: 13, paddingBottom: 6 },

  closeBtn: {
    marginTop: 14, alignSelf: 'center', borderWidth: 1,
    borderRadius: 3, paddingVertical: 7, paddingHorizontal: 24,
  },
  closeBtnText: { fontSize: 11 },
});
