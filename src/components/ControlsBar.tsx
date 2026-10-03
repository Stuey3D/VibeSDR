/**
 * ControlsBar — scales from 320dp (iPhone SE Display Zoom) to 430dp+
 *
 * PORTRAIT — 4 rows (locked, do not change):
 *   Row 1: signal bar + freq/mode pill
 *   Row 2: [STEP] [MENU] [CHAT] [SHARE]
 *   Row 3: [VFO drum flex:1] [Zoom drum flex:1]
 *   Row 4: clock · rec timer
 *
 * LANDSCAPE — single row:
 *   [VFO drum] [STEP/MENU col] [sig bar + pill flex:2] [CHAT/SHARE col] [Zoom drum]
 *
 * Scaling: useUiScale() — port of computeUiScale() from skin
 *   Portrait:  scale = clamp(0.75, W/390, 1.45)  → 320dp = 0.82
 *   Landscape: scale = clamp(0.58, W/926, 1.45)  → 568dp = 0.61
 */

import React, {
  useCallback, useEffect, useMemo, useRef, useState,
} from 'react';
import {
  Animated,
  AppState,
  NativeModules,
  Platform,
  Share,
  StyleSheet,
  Text,
  TouchableOpacity,
  View, ViewStyle, type LayoutChangeEvent,} from 'react-native';
import { useRegionHandback, NAV_FOCUS } from './PanelNav';
import { BlurView } from 'expo-blur';
import SectionIcon from './SectionIcon';
import {
  Canvas,
  Group,
  LinearGradient,
  Path,
  Rect,
  Skia,
  vec,
} from '@shopify/react-native-skia';
import DrumWheel from './DrumWheel';
import { DomeKey, DomeText, DomeIcon, type IconStroke } from './DomeKey';
import ChassisPlate, { GlossPanel, RecessedWindow } from './ChassisPlate';
import type { SharedValue } from 'react-native-reanimated';
import TunerKeys from './TunerKeys';
import { keyLightReach } from '../constants/keyLight';
import { useMacSilenced } from '../services/macAudio';
import NixieTubes, { nixieNaturalWidth } from './NixieTubes';
import LedVu from './LedVu';
import EdgeMeter from './EdgeMeter';
import { GhostGrid, SegDigits, VfdFilaments } from './VfdParts';
import { TUBE_DESIGN, type NixieLayout } from '../constants/nixie';
import { composeModeLabel, modeBoxFit, MODE_BOX } from '../constants/modeBox';
import { FONT_DOTO, FONT_HYPER, rgba, NO_DROP_SHADOW } from '../constants/faceplate';
import { DECK, portraitDeck, landscapeDeck, compactKeyHitSlop, sqlClosedOf, type MeterKind, type DeckLayout,
  type LandscapeLayout, METER_SCALES, formatReading, meterReading, meterUnitOf, scaleMeterValues,
  makeScaledMeterState, type MeterUnit } from '../constants/meters';
import { statusGainParts, statusGainText, statusFit, statusState, vfdFreqLayout, type StatusItem, type StatusRowSpec } from '../constants/displayText';
import Svg, { Path as SvgPath } from 'react-native-svg';

/**
 * ★★ THE STATUS DISPLAY (§8.1) on silver / black: a recessed sub-display in Doto 900 12 pt, in the
 *   TEXT colour, over the ghost-dot grid. The default chassis keeps today's footer exactly — the
 *   context is null there and every status piece draws as it always has. A context rather than props
 *   because the pieces (ClockRow, LinkIndicator, DspBadges, the bars' own inline texts) are spread
 *   through both bars.
 */
interface StatusDisplay { font: string; color: string; glow: string; rgb: string; size: number }
const StatusDisplayContext = React.createContext<StatusDisplay | null>(null);

/** A status text: today's style on the default deck; Doto in the text colour inside the display.
 *  `keepColor` for meaning colours (the recording red) that no faceplate colour may replace. */
function StatusText({ style, keepColor = false, children, ...rest }:
    React.ComponentProps<typeof Text> & { keepColor?: boolean }) {
  const sd = React.useContext(StatusDisplayContext);
  if (!sd) return <Text style={style} {...rest}>{children}</Text>;
  const flat = StyleSheet.flatten(style) ?? {};
  return (
    <Text {...rest} style={[flat, {
      fontFamily: sd.font, fontSize: sd.size, fontWeight: 'normal', letterSpacing: 0.4,
      color: keepColor ? flat.color : sd.color,
      textShadowColor: keepColor ? undefined : sd.glow, textShadowRadius: 4, textShadowOffset: { width: 0, height: 0 },
    }]}>{children}</Text>
  );
}

/** The gain arrow, DRAWN (Doto has no arrow glyph) — Deck.mockup's 7 × 8 stroke. */
function GainArrow({ dir, color, size }: { dir: 'up' | 'down'; color: string; size: number }) {
  const k = size / 12;
  return (
    <Svg width={7 * k} height={8 * k} viewBox="0 0 7 8" style={{ marginLeft: 3 * k, marginRight: 1 * k,
         transform: dir === 'up' ? [{ rotate: '180deg' }] : undefined }}>
      <SvgPath d="M3.5 0v6M1 3.5l2.5 3 2.5-3" fill="none" stroke={color} strokeWidth={1.4} />
    </Svg>
  );
}

/**
 * ★ What the TUBES need that the formatted string does not carry: the frequency as a number, the
 *   unit (which bulb lights), and the radio's fixed tube row. A context, not three more props,
 *   because this file's bars keep hand-written copies of their prop lists and a name missing from
 *   one of them has already been fatal twice (see the note on `readOnly` in ControlsBar).
 */
interface FreqReadout { hz: number; unit: FreqUnit; layout: NixieLayout }
const FreqReadoutContext = React.createContext<FreqReadout>({ hz: 0, unit: 'khz', layout: 'hf' });

/** Guard for the keys' handler — never expected to run. */
const noStep = (_d: -1 | 1) => {};

export type Rect = { x: number; y: number; w: number; h: number };

/**
 * Wraps a control slot and reports its SCREEN rect, so a pointer scroll landing on
 * it can drive that control (BRIEF-inputs §3: "hover decides the target"). Measured
 * in WINDOW coordinates because that is what the native scroll event reports; a
 * layout-relative box would be wrong the moment anything above it moved.
 */
function ControlSlot({ report, style, children }: {
  report?: (r: Rect) => void; style?: ViewStyle; children: React.ReactNode;
}) {
  const ref = useRef<View | null>(null);
  const measure = useCallback(() => {
    ref.current?.measureInWindow((x, y, w, h) => {
      if (w > 0 && h > 0) report?.({ x, y, w, h });
    });
  }, [report]);

  return (
    <View ref={ref} style={style} onLayout={measure} collapsable={false}>
      {children}
    </View>
  );
}
import { useTheme } from '../contexts/ThemeContext';
import { useFaceplate, useFaceplateOnTrial, useFaceplateSettings } from '../contexts/FaceplateContext';
import { explainMeterFault } from '../services/faceplateGuard';
import type { ChassisTokens, PlateTokens } from '../constants/faceplate';
import { useUiScale } from '../hooks/useUiScale';
import { STEPS, stepsForFreq, type SDRMode } from '../services/sdrTypes';
import { STEP_833, type AirChannel } from '../utils/airband';
import { tourRef, mergeRefs } from './Coachmark';
import { IS_TV } from '../utils/tv';
import { minuteKey } from '../services/renderChurn';

// ── Helpers ───────────────────────────────────────────────────────────────────

export type FreqUnit = 'hz' | 'khz' | 'mhz';

// Display follows the user's chosen unit (FreqModal selection) and always
// shows full Hz resolution — never silently truncates digits.
function formatHz(hz: number, unit: FreqUnit): string {
  if (unit === 'hz')  return Math.round(hz).toLocaleString('en-US');
  if (unit === 'mhz') return (hz / 1e6).toFixed(6);
  return (hz / 1_000).toFixed(3);
}
function freqUnitLabel(unit: FreqUnit): string {
  return unit === 'hz' ? 'Hz' : unit === 'mhz' ? 'MHz' : 'kHz';
}

function formatStep(s: number): string {
  if (s === STEP_833) return '8.33k';        // the airband raster, 25/3 kHz — see utils/airband.ts
  return s >= 1_000_000 ? s / 1_000_000 + 'M'
       : s >= 1_000     ? s / 1_000 + 'k'
       :                  s + 'Hz';
}

// ── Signal gradient — port of sigGradient() ──────────────────────────────────
// ★ The stops are the faceplate's chassis tokens (meterGrad*); the positions stay here with the
//   geometry that needs them.
function sigGradColors(sig: number, ct: ChassisTokens): string[] {
  if (sig < 0.20) return ct.meterGradLow;
  if (sig < 0.58) return ct.meterGradMid;
  return ct.meterGradHigh;
}
function sigGradPos(sig: number): number[] {
  if (sig < 0.20) return [0, 1];
  if (sig < 0.58) return [0, 0.20 / sig, 1];
  return [0, 0.15, 0.45, 1];
}

// ── SNR text — port of snrToDisplay() ────────────────────────────────────────
// ── Meter bus ─────────────────────────────────────────────────────────────────
// Meter values arrive ~7×/s; routing them through screen-level React state
// re-rendered the entire SDRScreen tree per update (CPU profile: React task
// execution ≈ a third of all JS time). The bus lets ONLY the two leaf widgets
// that display them (SignalCanvas, FreqModePill) subscribe and re-render.
/** link: 0=disconnected, 1=poor(red), 2=fluctuating(yellow), 3=good(green) */
export interface MeterValues {
  level: number; peak: number; snr: number;
  /** ★ The level BEFORE the meter smoothing (same 0..1 bar scale as `level`). The analogue needle
   *  springs from it (§4.5 TRAP: smoothing first and then springing doubles the lag) and the LED VU's
   *  σ is its spread (§4.4). Absent on a backend that only has the smoothed one → use `level`. */
  raw?: number;
  /** Peak power in the passband, dBFS — feeds the S-meter / dBFS readouts. */
  dbfs: number;
  active: boolean; link: 0|1|2|3;
  /** Squelch threshold as a bar-normalised position (0..1), in the SAME scale the bar draws, so the
   *  red squelch line sits on the shown meter. -1 = squelch off / not applicable (no line, no SQL). */
  sql?: number;
  /** ★★★ The same squelch threshold in the READOUT's own quantity — the unit meterReading() reads for
   *  the current signal readout (dB in S-meter / dBFS mode, SNR dB in SNR mode), visual trim included.
   *  The LED ring and the analogue red hand are placed from it on the calibrated scale, so they sit
   *  under the label the mode box would name at the gate. Absent = not stated: no ring, never a wrong one. */
  sqlVal?: number;
  /** Is the gate ACTUALLY closed (muting) right now? Computed from the same quantity the gate
   *  itself compares — not from bar geometry. In S-meter/dBFS mode the bar is a smoothed dBFS fill
   *  while the UberSDR gate compares raw SNR, so "fill < line" could redden while audio flowed (and
   *  miss real mutes). undefined = this backend can't say; fall back to geometry. */
  gate?: boolean;
  /** Incoming SPECTRUM data rate (KB/s) and frame rate (fps) — the connection-meter readout. Audio
   *  bytes are decoded natively on the phone, so this is the JS-visible (spectrum) rate. */
  kbps?: number;
  /**
   * ★★★ WHAT VIBEAGC IS DOING, SHORT ENOUGH FOR A PHONE. The server says it in full — "OVERLOAD:
   *     GAIN ↓ 8.7 dB · pk −4 dBFS" — which is right on a browser status line and far too much on
   *     a handset, where it would push the rate readout off the row. Stuart asked for the short
   *     form: "GAIN ↑ 8.7 dB" (2026-08-22).
   *  ★ Empty when the server has no such loop, so nothing appears on an Airspy or an RSP, which
   *    manage their own gain and have nothing to report here.
   */
  agcText?: string;
  /** ★ THE TUNER'S IF FILTER, in the same short form as the gain — "IF 1430k auto" or "IF 700k".
   *  ★★ It matters BECAUSE IT IS INVISIBLE: zoomed in, a narrowed filter is doing the most useful
   *     thing on the receiver and nothing on screen would otherwise say so. Stuart, of the web
   *     client's version: "so a user knows its working". Empty on a radio that has no such filter. */
  ifText?: string;
  /** ★ hwinfo's `ds`: the direct-sampling mode the radio is in NOW (0 tuner, 1 I, 2 Q). Above 0 the
   *  gain item reads "Direct Sample" instead of agcText — see statusGainText. */
  dsLive?: number;
  fps?:  number;
}
export interface MeterBus {
  value: MeterValues;
  subs:  Set<(v: MeterValues) => void>;
  emit:  (v: MeterValues) => void;
}
export function createMeterBus(): MeterBus {
  const bus: MeterBus = {
    value: { level: 0, peak: 0, snr: 0, dbfs: -120, active: false, link: 0, sql: -1, kbps: 0, fps: 0,
             agcText: '' },
    subs:  new Set(),
    emit(v: MeterValues) { bus.value = v; bus.subs.forEach(f => f(v)); },
  };
  return bus;
}
export function useMeters(bus?: MeterBus): MeterValues | null {
  const [v, setV] = useState<MeterValues | null>(bus ? bus.value : null);
  useEffect(() => {
    if (!bus) return;
    const f = (nv: MeterValues) => setV(nv);
    bus.subs.add(f);
    return () => { bus.subs.delete(f); };
  }, [bus]);
  return bus ? v : null;
}

/** Exported so the WATCH can mirror the phone's meter verbatim rather than picking
 *  its own metric. It used to render SNR specifically — which OWRX, Kiwi and FM-DX
 *  do not have (they send an absolute S-meter / dBf and no noise reference), so the
 *  wrist showed a permanent "—" on those backends while the bar moved fine beneath
 *  it. Sending the TEXT means the watch can never disagree with the phone, and a
 *  future backend with some other metric works for free. Same reasoning as shipping
 *  the palette as a LUT instead of reimplementing the colour maps in Swift. */
/* ★★★ ONE CHAIN (constants/meters.ts): the reading, its text, and — on the LED / analogue meters — its
 *  position on the printed scale all come from meterReading(unit, m), so the three cannot disagree.
 *  The S-meter is the classic HF one from passband dBFS (6 dB/S-unit, S9 ≈ −73). */
export function meterText(mode: MeterUnit, m: Pick<MeterValues, 'dbfs' | 'snr'> & Partial<MeterValues>): string {
  return formatReading(mode, meterReading(mode, m));
}

// ── Clock — port of tick() ────────────────────────────────────────────────────
/* ★★★ THE RECEIVER'S CLOCK, NOT THE PHONE'S.
 *
 *  This row read "17:13 UTC · 18:13 BST", where the second half was the LISTENER's time — the one
 *  number the phone is already showing in its own status bar, two centimetres above. What it could
 *  not tell you is the time AT THE AERIAL, which is what explains the band: whether the receiver is
 *  in daylight, on greyline, or deep in its night.
 *  ★★ Stuart, 2026-09-21: "we already do for the local time anyway which when every computer and
 *     phone has a clock visible is a bit redundant. Knowing the time of the server is important."
 *     He then demonstrated the gap himself — he had Kiko's receiver in Paraná down as US East Coast
 *     time, two hours out, and nothing on screen was ever going to put him right.
 *  ★★★ THE ZONE ABBREVIATION IS THE LABEL. The row already ended in one, so swapping the listener's
 *      for the receiver's costs NO extra width — which is what kills the "Server 18:13" idea that
 *      would clip this line. On a UK receiver it still reads "18:13 BST"; on Kiko's it reads
 *      "14:13 -03", which is unmistakably not your own clock.
 *  ★★★ AND IT NEEDS THE GLYPH AFTER ALL. This note used to argue the opposite — "no glyph needed:
 *      a symbol has to be learnt". The abbreviation only distinguishes the two clocks when the two
 *      zones DIFFER: on a UK listener with a UK receiver the row reads "22:07 UTC · 23:07 BST" and
 *      nothing says which half is whose, and on a receiver whose box is set to UTC it reads
 *      "22:07 UTC · 22:07 UTC" and looks simply broken (Stuart, 2026-09-24, on the Lenovo — whose
 *      zone really was Etc/UTC). The symbol does not have to be learnt here: it is the SAME node
 *      mark already sitting in the stats row of this very bar, next to the phone glyph, where it
 *      means "the server end" (it was the old rack box here until 2026-09-29).
 *  ★ Falls back to the phone's clock when the server has not said (an older build), so the row is
 *    never blank — but it is then labelled with the PHONE's zone, which is the honest reading. */
function useClock(tzOffsetMin?: number | null, tzAbbr?: string) {
  const [now, setNow] = useState(() => new Date());
  useEffect(() => {
    // Background audio keeps JS alive when locked — don't re-render the
    // controls every second behind a screen nobody can see.
    /* ★★ AND ONLY WHEN THE MINUTE TURNS. Both clocks draw HH:MM, but a fresh Date every second
     *  re-rendered the whole controls bar sixty times a minute to draw the same pixels — plus two
     *  Intl formatting calls a second for the phone-clock fallback (power audit, 2026-10-01).
     *  Still ticked every second so the minute lands within a second of turning. */
    const id = setInterval(() => {
      if (AppState.currentState !== 'active') return;
      const t = Date.now();
      setNow((prev) => (minuteKey(prev.getTime()) === minuteKey(t) ? prev : new Date(t)));
    }, 1000);
    return () => clearInterval(id);
  }, []);
  const utc = now.toUTCString().slice(17, 22);
  if (tzOffsetMin === null || tzOffsetMin === undefined) {
    const local = now.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', hour12: false });
    const tz    = now.toLocaleDateString([], { timeZoneName: 'short' }).split(', ')[1] || '';
    // ★ The PHONE's clock — so no server glyph: claiming this came from the receiver would be a lie.
    return { utc: `${utc} UTC`, srv: `${local} ${tz}`, fromServer: false };
  }
  /* ★ Shift UTC by the receiver's offset and read it back in UTC: that gives its wall clock without
   *  needing an IANA zone name or the phone's tz database, and it is right for the half-hour and
   *  three-quarter-hour zones too (India +330, the Chathams +765). */
  const at  = new Date(now.getTime() + tzOffsetMin * 60_000);
  const hhmm = at.toISOString().slice(11, 16);
  const mins = Math.abs(tzOffsetMin);
  const label = tzAbbr || (tzOffsetMin === 0 ? 'UTC'
    : (tzOffsetMin > 0 ? '+' : '-') + String(Math.floor(mins / 60)).padStart(2, '0')
      + (mins % 60 ? ':' + String(mins % 60).padStart(2, '0') : ''));
  return { utc: `${utc} UTC`, srv: `${hhmm} ${label}`, fromServer: true };
}

/** The clock row: UTC, then the RECEIVER's wall clock behind the node mark that means "server end"
 *  everywhere else in this bar (the connection meter beneath it).
 *  ★ Row 9 (§8.2): `hide` drops `utc` / `localTime` in the landscape row; `onUnit` is the measuring
 *    row's hook — each item wrapped and reporting its natural width (see LandscapeStatus). */
function ClockRow({ clock, color, font, size, hide, onUnit }:
    { clock: { utc: string; srv: string; fromServer: boolean }; color: string; font?: string; size: number;
      hide?: Partial<Record<StatusItem, boolean>>; onUnit?: OnUnit }) {
  const sd = React.useContext(StatusDisplayContext);
  if (sd) {
    // §8.1: `08:37 UTC 09:37 BST` — Doto, text colour, one run; the node mark stays (it means
    // "the receiver's clock"), drawn in the display's colour.
    return (
      <View style={pm.clockRow}>
        {!hide?.utc && <Unit id="utc" onUnit={onUnit}><StatusText numberOfLines={1}>{clock.utc}</StatusText></Unit>}
        {!hide?.localTime && <Unit id="localTime" onUnit={onUnit} gap={4}>
          {clock.fromServer ? <SectionIcon name="instance" size={Math.round(sd.size * 1.1)} color={sd.color} /> : null}
          <StatusText numberOfLines={1}>{clock.srv}</StatusText>
        </Unit>}
      </View>
    );
  }
  return (
    <View style={pm.clockRow}>
      {!hide?.utc && <Unit id="utc" onUnit={onUnit}>
        <Text numberOfLines={1} style={{ color, fontFamily: font, fontSize: size }}>{clock.utc}</Text>
      </Unit>}
      {!hide?.localTime && <Unit id="localTime" onUnit={onUnit} gap={4}>
      <Text numberOfLines={1} style={{ color, fontFamily: font, fontSize: size, opacity: 0.6 }}>·</Text>
      {/* ★★ THE NODE, NOT THE RACK (Stuart, 2026-09-29: "wrong server icon next to the clock").
          The connection meter directly beneath this row draws the server end as the network-NODE
          mark (SectionIcon 'instance' — the same server mark as the menu and the watch); this row
          still drew the old server-rack box, so one bar used two symbols for one thing. Same
          component now, in this row's own colour, sized to the clock text. */}
      {clock.fromServer ? <SectionIcon name="instance" size={Math.max(11, Math.round(size * 1.45))} color={color} /> : null}
      <Text numberOfLines={1} style={{ color, fontFamily: font, fontSize: size }}>{clock.srv}</Text>
      </Unit>}
    </View>
  );
}

/** Row 9's measuring hook: (unit id, natural width in pt). */
type OnUnit = (id: string, w: number) => void;

/**
 * ★ One item of the landscape status row. In the real row it is its children and nothing else — no
 *   extra View, so every row lays out exactly as it did (the siblings keep their parent's gap). In the
 *   MEASURING row (`onUnit` given) it wraps them in a View that reports their natural width; `gap` is
 *   the parent's gap between them, so the wrapper measures what the row would have laid out.
 */
function Unit({ id, onUnit, gap = 0, children }:
    { id: string; onUnit?: OnUnit; gap?: number; children: React.ReactNode }) {
  if (!onUnit) return <>{children}</>;
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', gap }}
          onLayout={(e) => onUnit(id, e.nativeEvent.layout.width)}>
      {children}
    </View>
  );
}

// ── SVG paths (from mockup HTML) ──────────────────────────────────────────────
const CHAT_PATH   = Skia.Path.MakeFromSVGString('M3 4.5A1.5 1.5 0 0 1 4.5 3h11A1.5 1.5 0 0 1 17 4.5v8A1.5 1.5 0 0 1 15.5 14H7l-4 3V4.5Z')!;
const SHARE_LINES = Skia.Path.MakeFromSVGString('M13.3 5L6.7 9M13.3 15L6.7 11')!;
const SHARE_C1    = Skia.Path.MakeFromSVGString('M15 4m-1.8 0a1.8 1.8 0 1 0 3.6 0a1.8 1.8 0 1 0 -3.6 0')!;
const SHARE_C2    = Skia.Path.MakeFromSVGString('M15 16m-1.8 0a1.8 1.8 0 1 0 3.6 0a1.8 1.8 0 1 0 -3.6 0')!;
const SHARE_C3    = Skia.Path.MakeFromSVGString('M5 10m-1.8 0a1.8 1.8 0 1 0 3.6 0a1.8 1.8 0 1 0 -3.6 0')!;
// Speaker: cone body (fill) + two concentric sound-wave arcs (authored 20×20)
const SPEAKER_BODY = Skia.Path.MakeFromSVGString('M3 8H6L10 4V16L6 12H3Z')!;
const SPEAKER_W1   = Skia.Path.MakeFromSVGString('M12.5 8a3 3 0 0 1 0 4')!;
const SPEAKER_W2   = Skia.Path.MakeFromSVGString('M14.5 6a6 6 0 0 1 0 8')!;
/* ★★ MUTED (the Mac VOLUME / MUTE — services/macAudio.ts): a PROHIBITION SIGN — a ring with a slash from
 *   top-left to bottom-right, the "no sound" road sign — round the SAME speaker, scaled 0.7 about its own
 *   centre so the cone and both waves sit inside the ring. Drawn in the key's LEGEND colour like every other
 *   legend: never red, because a user who chose red controls would never see it (Stuart). The waves are a
 *   touch thinner than the ring so the two arcs stay apart at the deck's 20 pt. */
const MUTE_RING    = Skia.Path.MakeFromSVGString('M10 10m-8.9 0a8.9 8.9 0 1 0 17.8 0a8.9 8.9 0 1 0 -17.8 0')!;
const MUTE_SLASH   = Skia.Path.MakeFromSVGString('M3.707 3.707L16.293 16.293')!;
const MUTE_BODY    = Skia.Path.MakeFromSVGString('M5.45 8.6H7.55L10.35 5.8V14.2L7.55 11.4H5.45Z')!;
const MUTE_W1      = Skia.Path.MakeFromSVGString('M12.1 8.6a2.1 2.1 0 0 1 0 2.8')!;
const MUTE_W2      = Skia.Path.MakeFromSVGString('M13.5 7.2a4.2 4.2 0 0 1 0 5.6')!;
// Record disc: outline ring + solid inner dot (authored 20×20)
const RECORD_RING  = Skia.Path.MakeFromSVGString('M10 10m-7 0a7 7 0 1 0 14 0a7 7 0 1 0 -14 0')!;
const RECORD_DOT   = Skia.Path.MakeFromSVGString('M10 10m-4 0a4 4 0 1 0 8 0a4 4 0 1 0 -8 0')!;

// ── Props ─────────────────────────────────────────────────────────────────────

export interface ControlsBarProps {
  /** ★ The RECEIVER's clock — signed minutes from UTC and its zone name, from hwinfo. Undefined on
   *  a server too old to say, and the row then falls back to the phone's own time. See useClock. */
  srvTzOffsetMin?: number | null;
  srvTzAbbr?: string;
  frequency:     number;
  mode:          SDRMode;
  step:          number;
  connected:     boolean;
  signalLevel?:  number;
  peakLevel?:    number;
  snrDb?:        number;
  signalActive?: boolean;
  /** Meter bus — values bypass React screen state; only the meter leaves
   *  subscribe. Preferred over the 4 legacy props above. */
  meterBus?:     MeterBus;
  /** Readout mode for the pill text (menu SIGNAL METER toggles). */
  signalMode?:   'snr' | 'smeter' | 'dbfs';
  /** WFM stereo pilot detected (local hardware) → "ST" badge on the mode pill. */
  fmStereo?:     boolean;
  /** Active client decoder id (rtty/wefax/…) — composes the mode readout as
   *  `<mode>: <decoder>` (e.g. USB: RTTY) so the running decoder is visible (§5.1). */
  activeDecoder?: string | null;
  bottomInset:   number;
  onVfoDelta:    (px: number) => void;
  onBwDelta:     (px: number) => void;
  onMode:        (m: SDRMode) => void;
  onStep:        (s: number)  => void;
  onMenu:        () => void;
  onChat?:       () => void;
  /** Opens the AUDIO sheet (NR/NB/squelch/notch/REC + server NR). */
  onAudio?:      () => void;
  /** ★★★ THE AUDIO CHAIN'S STANDING STATE — noise reduction, noise blanker, auto-notch. Shown only
   *  when ON (see DspBadges), because the case worth reporting is the one the listener has
   *  forgotten about. These exist so the settings can be REMEMBERED across sessions: NR was never
   *  persisted precisely because an invisible one sounds like a broken receiver. */
  dspNr?:        boolean;
  dspNb?:        boolean;
  dspAn?:        boolean;
  /** FM-DX: the AUDIO sheet is REC-only, so show a record glyph (not a speaker). */
  audioAsRecord?: boolean;
  /** Deep-link share (instance URL + freq/mode params). Falls back to text. */
  onShare?:      () => void;
  onFreqTap?:    () => void;
  onModeTap?:    () => void;
  freqUnit?:     FreqUnit;
  instanceHost?: string;
  isRecording?:  boolean;
  recSeconds?:   number;
  chatUnread?:   boolean;
  /** Grey out chat + share (local hardware has no server chat / shareable URL). */
  chatShareDisabled?: boolean;
  /** Grey out chat only (e.g. KiwiSDR has no chat, but still has a shareable URL). */
  chatDisabled?: boolean;
  /** FM-DX tuner: no bandwidth/zoom — render only the full-width VFO drum. */
  singleDrum?: boolean;
  /** Disable VFO-drum fling inertia (FM-DX shared tuner: lift = stop). */
  vfoNoInertia?: boolean;
  /** Override the STEP-button cycle list (Hz). FM-DX locks this to FM steps
   *  instead of the freq-derived HF/VHF defaults. */
  stepList?: number[];
  /** Static text shown under the mode label (the meter-text slot) when there's
   *  no meter bus — FM-DX puts its "26.2 dBf" reading here. */
  meterLabel?: string;
  /** Render the MENU button as a Back (‹) button — FM-DX has no menu; onMenu
   *  becomes the back action. */
  menuAsBack?: boolean;
  /** Override the frequency string formatting (FM-DX shows 3-dp MHz instead of
   *  the unit-based full-resolution format HF needs). */
  freqFormat?: (hz: number) => string;
  /** SpyServer: another client owns the tuner — grey the drums, disable tuning. */
  readOnly?: boolean;
  /** ★★★ WHO ELSE IS ON THIS DIAL — the answer to the only question that matters before you turn
   *  it. On a shared-VFO receiver anybody may tune and the server stops nobody, so the etiquette is
   *  the whole mechanism; but asking in the chat every time would be absurd when you are the only
   *  person here. Stuart, 2026-08-20: *"the app needs a user counter on the shared tuner so that
   *  you know if its safe to tune without asking the chat."*
   *  ★★ ALONE IS THE LOAD-BEARING STATE, so it gets words rather than a number: "Only you" is
   *     instantly readable as permission, where "1 listening" makes you count. */
  sharedDial?: { listeners: number; max: number; alone: boolean; tuning: string;
                 /** ★ FM-DX: one tuner fanned out, no session cap — say "You+N" rather than a
                  *  total over a denominator it does not have (Stuart's wording, 2026-09-22). */
                 youPlus?: boolean } | null;
  /** ★ STORMS — sferics about, decided by the SERVER on the wide FFT (rate per minute, seconds
   *  since the last flash). Answers "what are those lines across the waterfall?" before it is
   *  asked; the number rides in the accessibility label, as the web's tooltip. */
  storms?: { rate: number; ago: number } | null;
  /** ★ In DAB the pill says DAB — the server's demodulator is idle and its name is a lie there. */
  dabOn?: boolean;
  /** ★★ THE AIRBAND CHANNEL, when tuned on one (118–137 MHz, AM) — computed by the parent from
   *  utils/airband.ts, null everywhere else. Like an aviation radio the readout then shows the
   *  channel NAME ("118.010"), with the spacing and the true frequency ("8.33 · 118.0083") small
   *  above the unit. Only in MHz: a listener who chose kHz or Hz keeps their digits, and the name
   *  moves into the small line instead. */
  airChannel?: AirChannel | null;
  /** ★ The Nixie display's FIXED tube row for this radio (§7): `hf` network radios (8 tubes),
   *  `wide` a local radio to 2 GHz (10), `fm` the FM tuner screen (3 + bulb + 3). Never per frequency. */
  tubeLayout?: NixieLayout;
  /** ★★ ADMIN SESSIONS ARE NOT TIMED, so this slot says WHY rather than counting down. An admin is
   *  exempt from the session limit, and the honest thing to show where a countdown would be is
   *  what is actually true of this session (Stuart, 2026-08-12). Takes precedence over
   *  `sessionLeft`: an admin who is also inside a limit is still not going to be disconnected. */
  adminMode?: boolean;
  /** Control mode per control — the drums are the default and stay untouched;
   *  these swap EITHER control independently for the HiFi tuner keys. Four
   *  combinations, two settings, no global switch (BRIEF-inputs §2). */
  vfoKeys?:  boolean;
  zoomKeys?: boolean;
  /** One step in `dir`, for the keys. Only needed when the keys are shown. */
  onVfoStep?:  (dir: -1 | 1) => void;
  onZoomStep?: (dir: -1 | 1) => void;
  /** Per-tick zoom while sweeping — finer than a tap, see TunerKeys. */
  onZoomSweep?: (dir: -1 | 1) => void;
  /** Steps/sec ceiling for the VFO sweep — derived from step size + visible span
   *  so signals cross the screen at a consistent rate. */
  vfoSweepRate?: () => number;
  /** Screen rects of the two control slots, so a pointer scroll can be
   *  HOVER-SCOPED to whichever control it is over. */
  onControlRects?: (r: { vfo?: Rect; zoom?: Rect }) => void;
  /** ★ Landscape only: the window x of the VFO drum's + and the zoom drum's −, the station strip's text anchors
   *  (Stuart, 2026-10-03). null when there is no landscape pair (portrait, a single drum). */
  onDrumAnchors?: (a: { left: number; right: number } | null) => void;
}

// ── Signal bar canvas ─────────────────────────────────────────────────────────

function SignalCanvas({ width, height, signal: sigProp = 0, peak: peakProp = 0, bus }:
  { width: number; height: number; signal?: number; peak?: number; bus?: MeterBus }) {
  const m = useMeters(bus);
  const ct = useFaceplate().chassis;
  const signal = m ? m.level : sigProp;
  const peak   = m ? m.peak  : peakProp;

  // Direct rendering at the real data rate (~10Hz) — interpolation removed by
  // request; updates cost only this small canvas re-render via the meter bus.
  if (width < 4) return null;
  const fillW  = width * Math.min(1, Math.max(0, signal));
  const peakX  = width * Math.min(1, Math.max(0, peak));
  const colors = signal > 0.001 ? sigGradColors(signal, ct) : [];
  const pos    = signal > 0.001 ? sigGradPos(signal) : [];
  // Squelch: red threshold line at its bar position; while the signal is BELOW it the gate is closed
  // (muting) and the fill dims a touch — noticeable but still readable. Above it, full brightness.
  const sql      = m ? (m.sql ?? -1) : -1;
  const sqlOn    = sql >= 0;
  const sqlX     = width * Math.min(1, Math.max(0, sql));
  // Prefer the gate's OWN verdict; bar geometry is only a fallback (see MeterValues.gate).
  const sqlClosed = sqlOn && (m?.gate ?? (signal < sql));
  return (
    <Canvas style={StyleSheet.absoluteFill}>
      <Rect x={0} y={0} width={width} height={height} color={ct.meterTrack} />
      {fillW > 1 && colors.length > 0 && (
        <Rect x={0} y={0} width={fillW} height={height} opacity={sqlClosed ? 0.55 : 1}>
          <LinearGradient start={vec(0,0)} end={vec(fillW,0)} colors={colors} positions={pos} />
        </Rect>
      )}
      {peakX > 2 && (
        <Rect x={peakX - 1} y={0} width={2} height={height} color={ct.peakLine} />
      )}
      {sqlOn && (
        <>
          {/* White halo so the red squelch line stays visible over any fill colour. */}
          <Rect x={sqlX - 2} y={0} width={4} height={height} color={ct.sqlHalo} />
          <Rect x={sqlX - 1} y={0} width={2} height={height} color={ct.sqlLine} />
        </>
      )}
    </Canvas>
  );
}

// ── Link quality bars (replaces the old connection dot) ───────────────────────
// Mobile-signal style: 3 green = solid link, 2 yellow = jitter/some drops,
// 1 red = stalling/reconnecting, all dim = disconnected.
function LinkBars({ q }: { q: 0 | 1 | 2 | 3 }) {
  const ct = useFaceplate().chassis;
  const sd = React.useContext(StatusDisplayContext);
  // Disconnected (q=0) → a clear red ✕ rather than ambiguous dim bars.
  if (q === 0) {
    return (
      <View style={pm.linkWrap}>
        <Text style={{ color: ct.linkBad, fontSize: 14, fontWeight: '900', lineHeight: 14 }}>✕</Text>
      </View>
    );
  }
  // ★ In the status display the bars are the display's own segments (Deck.mockup: currentColor, the
  //   unlit ones at α .28) — the COUNT carries the quality. The ✕ above stays red on every chassis.
  const litColor = sd ? sd.color : q === 3 ? ct.linkGood : q === 2 ? ct.linkFair : ct.linkBad;
  const unlit = sd ? rgba(sd.rgb, 0.28) : ct.linkUnlit;
  return (
    <View style={pm.linkWrap}>
      {[0, 1, 2].map(i => (
        <View key={i} style={[pm.linkBar, {
          height: 4 + i * 3,
          backgroundColor: i < q ? litColor : unlit,
        }]} />
      ))}
    </View>
  );
}

// ── Link indicator cluster: 📱 ⇄ bars ⇄ server ───────────────────────────────
// Lives in the bottom row so the metaphor is explicit: quality of the path
// between THIS phone and the SDR server.
function PhoneGlyph({ color }: { color: string }) {
  return (
    <View style={[pm.phoneGlyph, { borderColor: color }]}>
      <View style={[pm.phoneDot, { backgroundColor: color }]} />
    </View>
  );
}
/** ★★★ WHAT IS BEING DONE TO THE AUDIO — shown ONLY when something is.
 *
 *  Noise reduction is the reason this exists. It was deliberately never saved across sessions,
 *  because a listener who has forgotten it is on hears the artefacts and concludes the RECEIVER is
 *  broken (Stuart, 2026-09-24: "if a user forgets theyve enabled it they dont know its on and
 *  wonders why the audio sounds funny"). That reasoning is right, and it is also what has kept the
 *  setting from persisting — which is what an Airspy owner reported as a bug on the same day.
 *  Both are answered by making the state VISIBLE: once you can see it, remembering it is safe.
 *
 *  ★★ NOTHING IS DRAWN WHEN NOTHING IS ON, and that is the whole design. A row of greyed
 *     placeholders would cost permanent space on every screen to describe the case nobody needs
 *     telling about; appearing only in the exception is what makes it affordable at all — and the
 *     status rows are already full enough to be truncating on a 17 Pro Max.
 *  ★ Tapping opens the audio sheet, so the badge is the way to the thing it is warning about
 *    rather than a dead ornament.
 */
export function DspBadges({ nr, nb, an, onPress, font, color }:
    { nr?: boolean; nb?: boolean; an?: boolean; onPress?: () => void;
      font?: string; color?: string }) {
  const ct = useFaceplate().chassis;
  const sd = React.useContext(StatusDisplayContext);
  const on: string[] = [];
  if (nr) on.push('NR');
  if (nb) on.push('NB');
  if (an) on.push('AN');
  if (!on.length) return null;                 // ★ the ordinary case costs nothing
  const body = (
    <View style={pm.dspRow}>
      {on.map((k) => (
        <Text key={k} style={[pm.dspTag, sd
          ? { fontFamily: sd.font, fontWeight: 'normal', color: sd.color, borderColor: rgba(sd.rgb, 0.55),
              backgroundColor: rgba(sd.rgb, 0.12), textShadowColor: sd.glow, textShadowRadius: 4 }
          : { fontFamily: font, color: color ?? ct.dspTagText,
              borderColor: ct.dspTagBorder, backgroundColor: ct.dspTagBg }]}>{k}</Text>
      ))}
    </View>
  );
  return onPress
    ? <TouchableOpacity onPress={onPress} activeOpacity={0.7} hitSlop={8}>{body}</TouchableOpacity>
    : body;
}

/** What the connection meter shows, as the few values that are actually DRAWN. */
interface LinkReadout { q: 0 | 1 | 2 | 3; showRate: boolean; kbps: number; fps: number; agcText: string; ifText: string }

/**
 * The connection meter's readout from the bus. ★ It re-renders only when something DRAWN changes —
 * the bus ticks ~7×/s, the rounded rate about once a second — so the landscape row that reads it
 * (and its measuring twin, row 9) is not re-laid-out seven times a second for identical text.
 */
function useLinkReadout(bus?: MeterBus): LinkReadout {
  // ★ LATCH, don't gate on the live value. Requiring fps > 0 to show the readout
  // meant a single second with no counted frames BLANKED it — so on a backend
  // whose frames arrive unevenly (Kiwi) it flashed on and off once a second.
  // ★★ And hiding is the wrong response anyway: a stall is precisely when
  // "0k/s · 0fps" is worth seeing. Blanking turns the most informative moment
  // into no information at all. Show it once a rate has ever arrived, then keep
  // showing it — zeros included.
  const everHadRate = useRef(false);
  const derive = (m: MeterValues | null): LinkReadout => {
    const q = m ? m.link : 0;
    if ((m?.fps ?? 0) > 0 || (m?.kbps ?? 0) > 0) everHadRate.current = true;
    if (q === 0) everHadRate.current = false;      // disconnected — start clean again
    // Incoming rate readout — spectrum KB/s (the phone's audio is decoded natively, so JS can't see
    // its bytes) + frame rate, the same "what's actually arriving" cue the web client shows. Only once
    // a link exists, so a disconnected meter stays clean.
    return { q, showRate: !!m && q > 0 && everHadRate.current,
             kbps: Math.round(m?.kbps ?? 0), fps: Math.round(m?.fps ?? 0),
             // ★ "Direct Sample" in place of the gain while the tuner is bypassed — HERE, the one
             //   place the drawn readout is derived, so the row, its measuring twin and statusFit's
             //   widths all see the same text (statusGainText).
             agcText: statusGainText(m?.agcText ?? '', m?.dsLive), ifText: m?.ifText ?? '' };
  };
  const [r, setR] = useState<LinkReadout>(() => derive(bus ? bus.value : null));
  useEffect(() => {
    if (!bus) return;
    const f = (v: MeterValues) => {
      const n = derive(v);
      setR((p) => (p.q === n.q && p.showRate === n.showRate && p.kbps === n.kbps && p.fps === n.fps
                   && p.agcText === n.agcText && p.ifText === n.ifText) ? p : n);
    };
    bus.subs.add(f);
    f(bus.value);
    return () => { bus.subs.delete(f); };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [bus]);
  return r;
}

export function LinkIndicator({ bus, hide, noNode = false, readout, onUnit }: { bus?: MeterBus;
    /** §8.1 / §9: on a shared server SHARED TUNER sits in the landscape status row's centre, REPLACING
     *  the node icon here (the phone ⇄ bars stay: the connection meter is never dropped). */
    noNode?: boolean;
    /** ★ Row 9 (§8.2): items the landscape status row has dropped to fit, by STATUS_DROP_ORDER.
     *  The bars are never hidden — the connection meter is never dropped. */
    hide?: Partial<Record<StatusItem, boolean>>;
    /** The readout, when the caller already reads it (the landscape row and its measuring twin share
     *  one); otherwise this reads the bus itself. */
    readout?: LinkReadout;
    /** Row 9's MEASURING row: every unit reports its natural width, and no tour ref is claimed. */
    onUnit?: OnUnit }) {
  const own = useLinkReadout(readout ? undefined : bus);
  const r = readout ?? own;
  const ct = useFaceplate().chassis;
  const sd = React.useContext(StatusDisplayContext);
  const q = r.q;
  const dim = ct.linkDim;
  const showRate = r.showRate;
  const rateTxt  = showRate ? `${r.kbps}k/s · ${r.fps}fps` : '';
  return (
    // ★ collapsable={false} so the tour can measure it — a plain View can be flattened
    //   away by RN and then measureInWindow has nothing to report. Same as the other
    //   tour targets.
    <View ref={onUnit ? undefined : tourRef('linkMeter')} collapsable={false}
          style={[pm.linkRow, onUnit && { flexWrap: 'nowrap', justifyContent: 'flex-start' }]}>
      {sd ? (<>
        {/* §8.1: `[bars][node] 6k/s 5fps · GAIN ↓25.4dB · IF 2800k` — the phone and ⇄ go (they are
            the first icons row 9 drops anyway), the arrow is DRAWN. */}
        <Unit id="meter" onUnit={onUnit}><LinkBars q={q} /></Unit>
        {!hide?.linkIcons && !noNode && <Unit id="linkIconsB" onUnit={onUnit}>
          <SectionIcon name="instance" size={Math.round(sd.size * 1.1)} color={sd.color} />
        </Unit>}
        {showRate && !hide?.rate ? <Unit id="rate" onUnit={onUnit}><StatusText>{`${r.kbps}k/s ${r.fps}fps`}</StatusText></Unit> : null}
        {r.agcText && !hide?.gain ? <Unit id="gain" onUnit={onUnit}>{(() => {
          const g = statusGainParts(r.agcText);
          if (!g) return <StatusText>{`· ${r.agcText}`}</StatusText>;
          return (
            <View style={{ flexDirection: 'row', alignItems: 'center' }}>
              <StatusText>{`· ${g.label}`}</StatusText>
              {g.dir ? <GainArrow dir={g.dir} color={sd.color} size={sd.size} /> : <StatusText> </StatusText>}
              <StatusText>{g.value + g.tail}</StatusText>
            </View>
          );
        })()}</Unit> : null}
        {r.ifText && !hide?.if ? <Unit id="if" onUnit={onUnit}><StatusText>{`· ${r.ifText}`}</StatusText></Unit> : null}
      </>) : (<>
      {!hide?.linkIcons && <Unit id="linkIconsA" onUnit={onUnit} gap={4}>
        <PhoneGlyph color={dim} />
        <Text style={[pm.linkArrows, { color: ct.linkDim }]}>⇄</Text>
      </Unit>}
      <Unit id="meter" onUnit={onUnit}><LinkBars q={q} /></Unit>
      {/* The network-NODE triangle — the same server mark used everywhere else (menu, watch), not
          the old server-rack box. ★ Replaced by SHARED TUNER on a shared server in landscape (§8.1). */}
      {!hide?.linkIcons && !noNode && <Unit id="linkIconsB" onUnit={onUnit} gap={4}>
        <Text style={[pm.linkArrows, { color: ct.linkDim }]}>⇄</Text>
        <SectionIcon name="instance" size={13} color={dim} />
      </Unit>}
      {showRate && !hide?.rate ? <Unit id="rate" onUnit={onUnit}><Text style={[pm.linkRate, { color: ct.linkRate }]}>{rateTxt}</Text></Unit> : null}
      {/* ★ After the rate, because it changes rarely — a value that moves once a minute beside one
             that moves every second reads as part of the same reading if it comes first. */}
      {r.agcText && !hide?.gain ? <Unit id="gain" onUnit={onUnit}><Text style={[pm.linkRate, { color: ct.linkRate }]}>{`· ${r.agcText}`}</Text></Unit> : null}
      {/* ★ Last: it moves least of all — it changes only when somebody zooms. */}
      {r.ifText && !hide?.if ? <Unit id="if" onUnit={onUnit}><Text style={[pm.linkRate, { color: ct.linkRate }]}>{`· ${r.ifText}`}</Text></Unit> : null}
      </>)}
    </View>
  );
}

// ── Freq + mode pill ──────────────────────────────────────────────────────────
// All sizes passed as props from parent so they scale with useUiScale()

// Classic interlocking-rings stereo symbol (two overlapping ring outlines),
// shown on the mode pill when a WFM stereo pilot is locked.
function StereoIcon({ size, color }: { size: number; color: string }) {
  const bw = Math.max(1.2, size * 0.13);
  const ring = { position: 'absolute' as const, top: 0, width: size, height: size,
                 borderRadius: size / 2, borderWidth: bw, borderColor: color, backgroundColor: 'transparent' };
  return (
    <View style={{ width: size * 1.62, height: size, marginLeft: 5, justifyContent: 'center' }}>
      <View style={[ring, { left: 0 }]} />
      <View style={[ring, { left: size * 0.62 }]} />
    </View>
  );
}

/**
 * The frequency window for the tube / dot-matrix / segment displays (§7). ★ Exactly the pill's
 * height under Hyperlegible (the text's line height + its vertical padding), so switching Display
 * never changes the deck's height; the tubes and cells shrink into it, the window does not grow.
 * ★ The unit label has a FIXED width, so kHz / MHz / Hz cannot shift the digits beside it (§7 TRAP).
 */
function DisplayFreq({ freqStr, unit, chanTag, freqFontSize, freqWidth, unitFontSize, pillPadH, pillPadV, gap, shared,
  winH, land = false }: {
  freqStr: string; unit: string; chanTag: string | null; freqFontSize: number; freqWidth: number;
  unitFontSize: number; pillPadH: number; pillPadV: number; gap: number; shared: boolean;
  /** ★ The LED / analogue frequency window (§4.1): its height, which the deck's fixed block decided
   *  (48 / 38 / 46 / 35 at scale 1). Absent = the bar's pill, sized from the text as before. The
   *  compact window takes the §4.1 digit / tube sizes and FILLS its width. */
  winH?: number;
  /** The landscape LED / analogue window (§9): Deck.mockup `fvL` — tubes 15 × 27, Doto 22, 7-segment 23. */
  land?: boolean;
}) {
  const dk = useFaceplate().deck;
  const s = useUiScale();
  const ro = React.useContext(FreqReadoutContext);
  const compact = winH != null;
  const H = compact ? winH : Math.round(freqFontSize * 1.12) + 2 * pillPadV;
  const unitW = Math.round(unitFontSize * 2.6);
  // ★★ A FIXED WIDTH for the airband channel label — sized for its longest form ("8.33 · 128.5917", 15
  //    characters) — so tuning between 25 kHz and 8.33 kHz channels never changes the room the digits
  //    sit in, and the CENTRED digits never move (B11, Stuart: centred, there is room for the details).
  const tagW = chanTag ? Math.round(Math.max(unitFontSize * 0.72 * 0.62 * Math.max(chanTag.length, 15), unitW)) : 0;
  const labelW = Math.max(unitW, tagW);
  const label = (
    // ★ flexShrink 0 + one line: in a narrow Mac window the column was squeezed and "MHz" broke as "MH" / "z"
    //   (Stuart, 2026-10-03). The digits beside it shrink to fit; the unit never does.
    <View style={[pm.chanCol, { width: labelW, flexShrink: 0, height: H, paddingBottom: Math.max(2, pillPadV), paddingRight: 3,
                                position: dk.style === 'nixie' ? 'absolute' : 'relative', right: 0, bottom: 0 }]}>
      {chanTag ? (
        <Text style={[pm.chanTag, { color: dk.unit, fontFamily: dk.unitFont,
                      fontSize: Math.max(8, Math.round(unitFontSize * 0.72)) }]} numberOfLines={1}>
          {chanTag}
        </Text>
      ) : null}
      <Text style={[pm.unit, { color: dk.unit, fontFamily: dk.unitFont, fontSize: unitFontSize, paddingBottom: 0 }]} numberOfLines={1}>
        {unit}
      </Text>
    </View>
  );
  if (dk.style === 'nixie' && compact) {
    // ★★★ The LED / analogue window: 22 × 36 tubes (shared 18 × 29), bar = false, the window's full
    //   width. The TUBE shrinks to the window, never the other way (§7 TRAP) — the smallest case is
    //   analogue + shared = 35 pt against a 37 pt design stack (test_faceplate_meters.ts).
    return (
      <NixieTubes hz={ro.hz} unit={ro.unit} layout={ro.layout}
        design={land ? TUBE_DESIGN.meterLand : shared ? TUBE_DESIGN.meterShared : TUBE_DESIGN.meter} bar={false} scale={s.scale}
        radius={8} reserveRight={labelW} style={{ flex: 1, height: H, minWidth: 0 }}>
        {label}
      </NixieTubes>
    );
  }
  if (dk.style === 'nixie') {
    // ★ Bar-meter window: 16 pt tubes, 1 pt gaps; the SHARED banner and landscape take the mockup's
    //   smaller designs. The LED / analogue windows take the branch above.
    const design = shared ? TUBE_DESIGN.barShared : s.isLandscape ? TUBE_DESIGN.barLand : TUBE_DESIGN.bar;
    const want = Math.ceil(nixieNaturalWidth(ro.layout, design, true, s.scale)) + labelW + 4;
    return (
      <NixieTubes hz={ro.hz} unit={ro.unit} layout={ro.layout} design={design} bar scale={s.scale}
        radius={5} reserveRight={labelW} style={{ width: want, height: H, flexShrink: 1, minWidth: 0 }}>
        {label}
      </NixieTubes>
    );
  }
  // dot / seg: a black VFD window. Doto over the ghost-dot grid, or the DRAWN 7-segment cells.
  const winW = Math.round(freqWidth * 1.1);
  // ★ Compact (LED / analogue) window: Deck.mockup `fv0` — Doto 27 (shared 23), 7-segment 29 (25) —
  //   centred in the full-width window; the bar pill keeps its own sizes.
  const cellBox: ViewStyle = compact ? { flex: 1, minWidth: 0, alignItems: 'center' }
                                     : { width: winW, flexShrink: 1, minWidth: 0 };
  const dotSize = compact ? s.r(land ? 22 : shared ? 23 : 27) : s.r(shared ? 24 : 28);
  const segH = compact ? Math.min(s.r(land ? 23 : shared ? 25 : 29), H - 4) : s.r(shared ? 27 : 30);
  return (
    <View style={[{ flexDirection: 'row', alignItems: 'stretch', height: H, paddingHorizontal: pillPadH, gap,
                    flexShrink: 1, minWidth: 0 }, compact && { flex: 1 }]}>
      {dk.style === 'dot' ? (
        // ★ Centred, like the 7-segment digits below (the airband label's width is fixed — tagW).
        <View style={[cellBox, { justifyContent: 'center', alignItems: 'center' }]}>
          <GhostGrid rgb={dk.rgb} pitch={3.4} dot={0.8} />
          <Text style={[pm.freq, {
            color: dk.freq, fontFamily: dk.freqFont, letterSpacing: dk.freqSpacing,
            textShadowColor: dk.freqGlow, textShadowRadius: 5,
            fontSize: Math.min(dotSize, Math.floor((H - 2) / 1.1)),
            lineHeight: H, includeFontPadding: false,
          }]} numberOfLines={1} adjustsFontSizeToFit>
            {freqStr}
          </Text>
        </View>
      ) : (
        // ★★ CENTRED, airband included (Stuart): the airband label's width is FIXED above (tagW), so it
        //    can no longer push the digits about as you tune.
        // ★ vfdFreqLayout: the Hz digits stay dark until they are in use, with as many dark cells on the
        //   left, so 104.200 sits centred — not 104.200000 — and those cells are drawn ¾ size on the
        //   baseline, so the readout never crowds the airband label (Stuart, 2026-10-02).
        <SegDigits {...vfdFreqLayout(freqStr, unit)} rgb={dk.rgb} core={dk.core} glow={dk.glow}
          designH={segH} style={cellBox} align="center" />
      )}
      {label}
    </View>
  );
}

type SharedTuner = NonNullable<ControlsBarProps['sharedDial']>;

/** The SHARED TUNER banner's words — one copy for the bar's banner and the LED / analogue one (§4.1). */
function sharedBannerText(st: SharedTuner, tight: boolean): string {
  return st.alone ? 'SHARED TUNER · FREE TO TUNE'
    : st.youPlus
      /* ★★★ "You+N": the count includes us, so N = total − 1 and nobody has to work out
       *  whether they are in it. No "/max" — FM-DX has no cap to report. */
      //  ★ On the narrowest layouts (SE in Display Zoom) the prefix goes, never the words
      //    that matter — truncating mid-word is the one outcome that is not allowed.
      ? `${tight ? '' : 'Shared Tuner - '}Ask Before Tuning (You+${Math.max(1, st.listeners - 1)})`
      : `SHARED TUNER · ASK TO TUNE · ${st.listeners}${st.max > 1 ? `/${st.max}` : ''} 👤`;
}
function sharedBannerLabel(st: SharedTuner): string {
  return st.alone ? 'Shared tuner. Nobody else is listening — free to tune.'
    : st.youPlus
      ? `Shared tuner. You and ${Math.max(1, st.listeners - 1)} other${st.listeners - 1 === 1 ? '' : 's'} listening — ask before tuning.`
      : `Shared tuner. ${st.listeners}${st.max > 1 ? ` of ${st.max}` : ''} listening — ask before tuning.`;
}

interface ModeReading { text: string; active: boolean; sqlClosed: boolean; breathe: Animated.Value }

/**
 * The mode box's live reading, shared by every meter (§4.6): the S-reading, or — while the squelch
 * is MUTING — a breathing "SQL" in `dk.sqlClosed` (red; neon under Nixie, the rule outranks red).
 * ★ Squelch: when the live signal is BELOW the threshold the gate is closed (muting NOW) — the
 *   readout flips to "SQL" (no extra screen space), and the meter dims.
 */
function useModeReading(bus: MeterBus | undefined, snrText: string | undefined, meterMode: MeterUnit | undefined,
                        signalActive: boolean | undefined): ModeReading {
  // Skin parity (lsvSnrDisp): plain "NNdb", not a synthetic S-meter reading.
  const m = useMeters(bus);
  /* ★★★ The LIVE reading comes off the bus through the meters' own chain (meterText), so the needle and
   *  the LEDs stand under the label this text names. The screen's static label (FM-DX "28 dBf", from
   *  its 5 Hz state) only shows while the bus has nothing live — paused, or before the first frame. */
  const text = m && (m.active || !snrText) ? meterText(meterMode ?? 'snr', m) : (snrText ?? '');
  const active = m ? m.active : !!signalActive;
  const sqlClosed = sqlClosedOf(m ? (m.sql ?? -1) : -1, m?.gate, m ? m.level : 0);
  const breathe = useRef(new Animated.Value(1)).current;
  useEffect(() => {
    if (!sqlClosed) { breathe.setValue(1); return; }
    const loop = Animated.loop(Animated.sequence([
      Animated.timing(breathe, { toValue: 0.3, duration: 650, useNativeDriver: true }),
      Animated.timing(breathe, { toValue: 1.0, duration: 650, useNativeDriver: true }),
    ]));
    loop.start();
    return () => loop.stop();
  }, [sqlClosed, breathe]);
  return { text, active, sqlClosed, breathe };
}

/** The mode label (+ stereo rings) over the reading / breathing SQL — the mode box's contents. */
/** ★ Room for the stereo rings: whenever they are lit, and always in WFM (where they come and go). */
function stereoSlot(modeLabel: string, fmStereo: boolean | undefined): boolean {
  return !!fmStereo || /^WFM\b/i.test(modeLabel);
}

function ModeReadout({ reading, modeLabel, fmStereo, modeFontSize, modeLs, snrWidth, readingFontSize, oneLine = false }: {
  reading: ModeReading; modeLabel: string; fmStereo: boolean; modeFontSize: number; modeLs: number;
  snrWidth?: number;
  /** LED / analogue window: the mockup's 11 pt reading. Absent = the bar's (today's) sizing. */
  readingFontSize?: number;
  /** ★★★ The LED / analogue box (CompactDisplay): the label is ONE line, whatever happens — the box is
   *  sized to it (constants/modeBox.ts), and if a label nobody foresaw still does not fit, it shrinks
   *  rather than wrap ("USB:" / "RTTY" / "S9+1" was three lines). Absent = the bar's pill, which grows
   *  to its content as it always has. */
  oneLine?: boolean;
}) {
  const dk = useFaceplate().deck;
  const rf = readingFontSize ?? Math.max(9, Math.round(modeFontSize * 0.75));
  const rl = readingFontSize ? Math.round(readingFontSize * 1.15) : Math.round(Math.max(9, modeFontSize * 0.75) * 1.15);
  const dot = dk.modeFont === FONT_DOTO;
  return (<>
    <View style={[{ flexDirection: 'row', alignItems: 'center', justifyContent: 'center' }, oneLine && { maxWidth: '100%' }]}>
      <Text style={[pm.modeLbl, {
        color: dk.mode, fontSize: modeFontSize, letterSpacing: modeLs, fontFamily: dk.modeFont,
        textShadowColor: dk.modeGlow,
        // ★ Doto is ONE weight (the Black cut is the file); asking it for bold makes Android
        //   fall back to the system font.
        ...(dot ? { fontWeight: 'normal' as const } : null),
        lineHeight: Math.round(modeFontSize * 1.15), includeFontPadding: false,
      }, oneLine && { flexShrink: 1 }]}
        {...(oneLine ? { numberOfLines: 1, adjustsFontSizeToFit: true, minimumFontScale: 0.6 } : null)}>
        {modeLabel}
      </Text>
      {/* WFM stereo: V5's pilot-PLL lock (+ blend) is reliable, so the icon
          is back — shows the interlocking-rings symbol when stereo is active. */}
      {/* ★★ THE RINGS' ROOM IS ALWAYS KEPT IN WFM — they go INVISIBLE, never away. On a weak station
          the pilot locks and unlocks several times a second, and a box that grew and shrank with the
          rings made the frequency beside it wobble (Stuart, 2026-10-02). Same width with or without. */}
      {stereoSlot(modeLabel, fmStereo) && (
        <View style={{ opacity: fmStereo ? 1 : 0 }} accessibilityElementsHidden={!fmStereo}
              importantForAccessibility={fmStereo ? 'auto' : 'no-hide-descendants'}>
          <StereoIcon size={Math.round(modeFontSize * 0.95)} color={dk.mode} />
        </View>
      )}
    </View>
    {reading.sqlClosed ? (
      <Animated.Text style={[pm.snr, {
        color: dk.sqlClosed, fontFamily: dk.modeFont, width: snrWidth,
        fontSize: rf, lineHeight: rl,
        includeFontPadding: false, fontWeight: dot ? 'normal' : '800', opacity: reading.breathe,
        // §4.6: neon under Nixie (`#ff9a55`, the rule outranks red), red elsewhere, with the mockup's
        // glow; the default deck keeps today's unglowing SQL.
        ...(dk.sqlGlow ? { textShadowColor: dk.sqlGlow, textShadowRadius: 4, textShadowOffset: { width: 0, height: 0 } } : null),
      }]}>
        SQL
      </Animated.Text>
    ) : (
      <Text style={[pm.snr, {
        color: dk.reading, fontFamily: dk.modeFont, width: snrWidth,
        fontSize: rf, lineHeight: rl,
        includeFontPadding: false,
        fontWeight: dot ? 'normal' : '700',
        opacity: reading.active ? 1.0 : 0.65,
      }]}>
        {reading.text}
      </Text>
    )}
  </>);
}

function FreqModePill({ freqStr, unit, chanTag = null, chanMain = false, modeLabel, snrText, connected, signalActive,
  onFreqTap, onModeTap, freqFontSize, freqWidth, unitFontSize, modeFontSize,
  modeLs, snrWidth, pillPadH, pillPadV, modePadH, modePadV, gap, bus, meterMode,
  tight = false, fmStereo = false, wide = false, sharedTuner = null,
}: any) {
  /* ★★ THE TEXT ROLES ARE THE FACEPLATE'S (fp.deck): frequency, unit, mode, reading, banner — each
   *  with its own font, because under the Nixie display (§2) the frequency and banner are Nixie One
   *  in neon while the mode box is not. Default settings resolve to today's white-theme values. */
  const fp = useFaceplate();
  const ct = fp.chassis;
  const dk = fp.deck;
  /* ★★ A SHARED DIAL SAYS SO WHERE YOU TUNE (Stuart, 2026-09-19) — the web client's #mShared, here. A box of its
   *  own above the frequency and mode boxes, spanning both; the text steps down ~20 % so the pill still fits the
   *  meter frame. Shared-VFO radios only. */
  /* ★★ IT GROWS WITH THE PILL, NOT WITH A GUESS (Stuart, 2026-09-20: "room to increase the font by a tiny
   *  amount" on a Mac, then "not enough room" on the phone — both true of the same fixed number). The pill's
   *  own frequency size already knows how much width this layout has, so the banner takes a share of it and
   *  is clamped at both ends: never smaller than the 9 it shipped at, never bigger than a phone can hold. */
  const sharedFontSize = Math.max(9, Math.min(13, Math.round(freqFontSize * 0.34)));
  if (sharedTuner) {
    freqFontSize = Math.round(freqFontSize * 0.8); modeFontSize = Math.round(modeFontSize * 0.8);
    unitFontSize = Math.round(unitFontSize * 0.85); pillPadV = Math.max(1, Math.round(pillPadV * 0.6));
    modePadV = Math.max(1, Math.round(modePadV * 0.6));
  }
  const reading = useModeReading(bus, snrText, meterMode, signalActive);
  return (
    // maxWidth cap: the pill must NEVER swallow the signal bar — on narrow
    // screens (SE / Moto G35) and with Android font metrics the fixed dp
    // widths overflow the frame; the freq text's adjustsFontSizeToFit
    // absorbs the squeeze (meter stays visible ≥13% each side).
    <View style={{ maxWidth: tight ? '66%' : '74%', alignSelf: 'center', alignItems: 'stretch' }}>
    {sharedTuner && (
      /* ★★ CONTEXT-AWARE (noobish via Stuart, 2026-09-19): alone, you may just tune; with company, ask — and
       *    the room's count lives HERE, where the question is asked, not in a corner badge. */
      <View style={[pm.sharedBox, { backgroundColor: ct.pillBg, borderColor: ct.sharedBorder }]}
            accessibilityRole="text"
            accessibilityLabel={sharedBannerLabel(sharedTuner)}>
        <Text style={[pm.sharedTxt, { fontFamily: dk.bannerFont, fontSize: sharedFontSize,
                      color: sharedTuner.alone ? dk.bannerFree : dk.bannerAsk }]}
              numberOfLines={1} adjustsFontSizeToFit minimumFontScale={0.7}>
          {sharedBannerText(sharedTuner, tight)}
        </Text>
      </View>
    )}
    <View style={pm.row}>
      <TouchableOpacity
        ref={tourRef('freqBox')}
        style={[pm.freqBox, dk.style === 'hyper'
          ? { backgroundColor: ct.pillBg, paddingHorizontal: pillPadH, paddingVertical: pillPadV, gap }
          // ★ The display windows draw their own glass (the Nixie recess, the VFD's black) edge to edge.
          : { backgroundColor: dk.style === 'nixie' ? '#060403' : '#050505', overflow: 'hidden' }]}
        onPress={onFreqTap} activeOpacity={0.80} hitSlop={8}
      >
        {dk.style === 'hyper' ? (<>
        <Text style={[pm.freq, {
          // ★ A channel name is seven characters where the frequency is ten, so the digits give up
          //   the room the small spacing/true-frequency line needs — the pill does not grow.
          color: dk.freq, fontSize: freqFontSize,
          width: chanTag && chanMain ? Math.round(freqWidth * 0.74) : freqWidth,
          fontFamily: dk.freqFont, textShadowColor: dk.freqGlow, letterSpacing: dk.freqSpacing,
          // Tight line metrics — Atkinson's tall default line-height (and
          // Android's extra font padding) inflated the pill to fill the
          // whole meter frame, hiding the signal ring around it
          lineHeight: Math.round(freqFontSize * 1.12),
          includeFontPadding: false,
        }]} numberOfLines={1} adjustsFontSizeToFit>
          {freqStr}
        </Text>
        {chanTag ? (
          <View style={pm.chanCol}>
            <Text style={[pm.chanTag, { color: dk.unit, fontFamily: dk.freqFont,
                          fontSize: Math.max(8, Math.round(unitFontSize * 0.72)) }]}
                  numberOfLines={1}>
              {chanTag}
            </Text>
            <Text style={[pm.unit, { color: dk.unit, fontFamily: dk.freqFont, fontSize: unitFontSize }]} numberOfLines={1}>
              {unit}
            </Text>
          </View>
        ) : (
          <Text style={[pm.unit, { color: dk.unit, fontFamily: dk.freqFont, fontSize: unitFontSize }]} numberOfLines={1}>
            {unit}
          </Text>
        )}
        </>) : (
          <DisplayFreq freqStr={freqStr} unit={unit} chanTag={chanTag} freqFontSize={freqFontSize}
            freqWidth={freqWidth} unitFontSize={unitFontSize} pillPadH={pillPadH} pillPadV={pillPadV}
            gap={gap} shared={!!sharedTuner} />
        )}
        {/* ★ The VFD glass's filament wires, frontmost (lighting brief §1) — dot / seg only. */}
        {(dk.style === 'dot' || dk.style === 'seg') && <VfdFilaments radius={5} />}
      </TouchableOpacity>
      <TouchableOpacity
        ref={tourRef('modeBtn')}
        style={[pm.modeBtn, { backgroundColor: ct.pillBg, borderLeftColor: ct.modeDivider, paddingHorizontal: modePadH, paddingVertical: modePadV, minWidth: tight ? 72 : 84 }]}
        onPress={onModeTap} activeOpacity={0.80} hitSlop={8}
      >
        {/* ★ The bar's button grows to its label (minWidth + content) — the rule the LED / analogue box
            now copies. Under any faceplate but the untouched default it is also held to ONE line, so a
            pill squeezed by the meter frame shrinks the label rather than wrap it; the default deck
            keeps today's exactly (§13.1). */}
        <ModeReadout reading={reading} modeLabel={modeLabel} fmStereo={fmStereo}
          modeFontSize={modeFontSize} modeLs={modeLs} snrWidth={snrWidth} oneLine={!!ct.plate || dk.style !== 'hyper'} />
      </TouchableOpacity>
    </View>
    </View>
  );
}

/**
 * ★★★ THE LED / ANALOGUE DISPLAY (§4.1, Deck.mockup `isVu`): [SHARED TUNER banner] / frequency window
 * with the mode box inside it / the meter housing — stacked in a column of FIXED height (the deck's
 * block less the keys), in which only the frequency window flexes. So neither the meter type nor a
 * shared server can change the deck's height: the window gives the room back (48 → 38, 46 → 35).
 * The bar deck keeps FreqModePill inside the bar, exactly as today (§4.2).
 */
function CompactDisplay({ dl, land, meterKind, freqStr, unit, chanTag, chanMain, modeLabel, snrText, signalActive, bus,
  meterMode, fmStereo = false, onFreqTap, onModeTap, sharedTuner = null, tight = false, freqWidth }: any) {
  /* ★ `land` (a LandscapeLayout, §9): the same column in the landscape band — the mockup's `fvL`
   *  sizes (digits 25, tubes 15 × 27, Doto 22, 7-segment 23) and the mode box's type shrunk to the
   *  window, the LED strip at 9 / 6.5 (or unlabelled), the edgewise card in its 24 pt window. */
  const L = land as LandscapeLayout | undefined;
  const fp = useFaceplate();
  const dk = fp.deck;
  const s = useUiScale();
  const reading = useModeReading(bus, snrText, meterMode, signalActive);
  const shared = !!sharedTuner;
  const lip = fp.chassis.plate?.windowLip ?? 'rgba(255,255,255,0.06)';
  const winBg = dk.style === 'nixie' ? '#060403' : dk.style === 'hyper' ? '#0a0807' : '#050505';
  const unitFont = L ? Math.min(s.r(11), Math.max(8, Math.floor(dl.freqH * 0.42))) : s.r(11);
  // §4.1 "digits 32 (shared 27)" — capped by the window it has to sit in. Landscape: 25 (§9).
  const digit = L ? L.digit : Math.min(s.r(shared ? 27 : 32), Math.floor((dl.freqH - 4) / 1.12));
  /* ★★★ THE MODE BOX FITS ITS LABEL ON ONE LINE (Stuart, 2026-10-01: "USB:" / "RTTY" / "S9+1" in a
   *  fixed 70 pt box beside a window with lots of spare width). The default deck's rule, applied here:
   *  70 pt is the MINIMUM and the box grows to the label, out of the frequency window (which flexes —
   *  the tubes narrow, §7), up to MODE_BOX_MAX_SHARE of the window. Only past that does the label's
   *  spacing tighten, and only past THAT does the type shrink. constants/modeBox.ts; the longest label
   *  the app can compose is proven to fit at every size by test_faceplate_meters / _landscape. */
  const [winW, setWinW] = useState(0);
  const modeFont0 = L ? L.modeFont : s.r(15);
  const readingFont = L ? L.readingFont : s.r(11);
  const mb = useMemo(() => modeBoxFit({
    label: modeLabel, stereo: stereoSlot(modeLabel, fmStereo), face: dk.modeFont === FONT_DOTO ? 'doto' : 'hyper',
    fontSize: modeFont0, letterSpacing: L ? 1.5 : 2, readingFont,
    minW: s.r(MODE_BOX.minW), padH: s.r(MODE_BOX.padH), windowW: winW,
  }), [modeLabel, fmStereo, dk.modeFont, modeFont0, L, readingFont, s, winW]);
  return (
    <View style={{ height: dl.displayH }}>
      {sharedTuner && (
        <View style={[cd.banner, { height: dl.bannerH, marginBottom: dl.bannerGap }]}
              accessibilityRole="text" accessibilityLabel={sharedBannerLabel(sharedTuner)}>
          <Text style={[cd.bannerTxt, { fontFamily: dk.bannerFont, fontSize: s.f(10.5),
                        color: sharedTuner.alone ? dk.bannerFree : dk.bannerAsk,
                        textShadowColor: fp.chassis.plate ? dk.glow : 'transparent' }]}
                numberOfLines={1} adjustsFontSizeToFit minimumFontScale={0.7}>
            {sharedBannerText(sharedTuner, tight)}
          </Text>
          <View pointerEvents="none" style={[cd.lip, { backgroundColor: 'rgba(255,255,255,0.18)' }]} />
        </View>
      )}
      <View style={[cd.window, { height: dl.freqH, backgroundColor: winBg }]}
            onLayout={(e: LayoutChangeEvent) => {
              const w = Math.round(e.nativeEvent.layout.width);
              setWinW(p => (p === w ? p : w));
            }}>
        <TouchableOpacity ref={tourRef('freqBox')} onPress={onFreqTap} activeOpacity={0.80} hitSlop={8}
          style={[cd.freqArea, dk.style === 'hyper' && { paddingHorizontal: s.r(8), gap: s.r(8) }]}>
          {dk.style === 'hyper' ? (<>
            <Text style={[pm.freq, {
              color: dk.freq, fontSize: digit, fontFamily: dk.freqFont, textShadowColor: dk.freqGlow,
              letterSpacing: dk.freqSpacing, lineHeight: Math.round(digit * 1.12), includeFontPadding: false,
              flexShrink: 1,
            }]} numberOfLines={1} adjustsFontSizeToFit>
              {freqStr}
            </Text>
            <View style={[pm.chanCol, { paddingBottom: L ? Math.max(2, Math.round(dl.freqH * 0.16)) : s.r(7), alignSelf: 'stretch' }]}>
              {chanTag ? (
                <Text style={[pm.chanTag, { color: dk.unit, fontFamily: dk.freqFont,
                              fontSize: Math.max(8, Math.round(unitFont * 0.72)) }]} numberOfLines={1}>
                  {chanTag}
                </Text>
              ) : null}
              <Text style={[pm.unit, { color: dk.unit, fontFamily: dk.freqFont, fontSize: unitFont, paddingBottom: 0 }]} numberOfLines={1}>
                {unit}
              </Text>
            </View>
          </>) : (
            <DisplayFreq freqStr={freqStr} unit={unit} chanTag={chanTag} freqFontSize={digit}
              freqWidth={freqWidth} unitFontSize={unitFont} pillPadH={s.r(6)}
              pillPadV={L ? Math.max(2, Math.round(dl.freqH * 0.16)) : s.r(7)}
              gap={s.r(6)} shared={shared} winH={dl.freqH} land={!!L} />
          )}
        </TouchableOpacity>
        <TouchableOpacity ref={tourRef('modeBtn')} onPress={onModeTap} activeOpacity={0.80} hitSlop={8}
          style={[cd.modeBox, { width: mb.width, paddingHorizontal: s.r(MODE_BOX.padH),
                                borderLeftColor: 'rgba(255,255,255,0.10)' }]}>
          <ModeReadout reading={reading} modeLabel={modeLabel} fmStereo={fmStereo} oneLine
            modeFontSize={mb.fontSize} modeLs={mb.letterSpacing} readingFontSize={readingFont} />
        </TouchableOpacity>
        {/* The glass's inner shadow at the top and the lip below (`inset 0 2px 7px`, `0 1px 0 .25`). */}
        {/* ★ Not over the tubes: the Nixie recess draws its own lip shadows (§7), and a second
            shade would darken the domes' tips in the 35 pt window where they sit closest to it. */}
        {dk.style !== 'nixie' && <View pointerEvents="none" style={cd.shade} />}
        <View pointerEvents="none" style={[cd.lip, { backgroundColor: 'rgba(255,255,255,0.25)' }]} />
        {/* ★ The VFD glass's filament wires, frontmost (lighting brief §1) — dot / seg only. */}
        {(dk.style === 'dot' || dk.style === 'seg') && <VfdFilaments radius={8} />}
      </View>
      <View style={{ height: dl.meterGap }} />
      <MeterHousing kind={meterKind} height={dl.housingH} shared={shared} lip={lip} bus={bus} land={L}
        unit={meterMode ?? 'snr'} />
    </View>
  );
}

/**
 * ★★★ THE METER BUS ON THE CALIBRATED SCALE. The LED VU and the analogue meter take a bus of their own
 * whose level / raw / sql are the readout's POSITION on the printed scale (constants/meters.ts
 * scaleMeterValues) — the same reading the mode box shows, in the same unit — so their uniform table
 * lands on the labels. The bar keeps the screen's bus untouched (§4.2: today's meter, unchanged).
 * ★ The unit is read live from a ref, so switching the signal readout re-scales the next frame without
 *   re-subscribing the meters.
 */
function useScaledMeterBus(bus: MeterBus | undefined, unit: MeterUnit): MeterBus | undefined {
  const unitRef = useRef(unit);
  unitRef.current = unit;
  const out = useMemo(() => (bus ? createMeterBus() : undefined), [bus]);
  useEffect(() => {
    if (!bus || !out) return;
    const st = makeScaledMeterState();
    const f = (m: MeterValues) => out.emit(scaleMeterValues(m, unitRef.current, st));
    f(bus.value);
    bus.subs.add(f);
    return () => { bus.subs.delete(f); };
  }, [bus, out]);
  return out;
}

/** The LED strip's scale — the readout's own labels (S1 … +60, or dB), under the segments. LedVu's
 *  built-in row prints the S-meter's only, so the housing draws it (LedVu runs with labelH 0). Same
 *  geometry and type as LedVu's: padX, the 4 pt gap, one flex cell per LED. */
function VuScaleLabels({ unit, padX, top, labelH }: { unit: MeterUnit; padX: number; top: number; labelH: number }) {
  const s = useUiScale();
  return (
    <View style={{ position: 'absolute', left: padX, right: padX, top, height: labelH, flexDirection: 'row',
                   gap: s.r(4) }} pointerEvents="none">
      {METER_SCALES[unit].labels.map(l => (
        <Text key={l} style={{ flex: 1, textAlign: 'center', fontFamily: FONT_HYPER, fontSize: labelH,
                               lineHeight: labelH, fontWeight: '600', letterSpacing: 0.3,
                               color: 'rgba(255,255,255,0.45)', includeFontPadding: false }}
              numberOfLines={1} adjustsFontSizeToFit>{l}</Text>
      ))}
    </View>
  );
}

/** The LED strip's / edgewise meter's black housing (§4.3 / §4.5): `#030303 → #0b0b0b`, inset shadow,
 *  the chassis lip below, and the meter in it. */
function MeterHousing({ kind, height, shared, lip, bus, land, unit }: {
  kind: MeterKind; height: number; shared: boolean; lip: string; bus?: MeterBus;
  /** Landscape (§9): the strip's / card's own geometry from landscapeDeck. */
  land?: LandscapeLayout;
  /** The signal readout's unit: the scale printed, and the one the needle / LEDs read. */
  unit: MeterUnit;
}) {
  const s = useUiScale();
  const scaled = useScaledMeterBus(bus, unit);
  // ★★★ A METER CANNOT TAKE THE APP DOWN: a throw in its frame callback is caught there and lands
  //   here once — the BAR goes back, and the user is told (a crash on launch: faceplate.ts CRASH SAFETY).
  const { set: setFaceplate } = useFaceplateSettings();
  const onFault = useCallback((message: string) => {
    setFaceplate({ meter: 'bar' });
    explainMeterFault(kind, message);
  }, [kind, setFaceplate]);
  // LedVu's own geometry (its defaults in portrait, landscapeDeck's in landscape) — with ITS label row
  // off; VuScaleLabels draws the readout's labels in the same place.
  const g = land ? { padTop: land.ledPadTop, padX: land.ledPadX, ledH: land.ledH, labelH: land.labelH, labelGap: land.labelGap }
    : { padTop: s.r(shared ? 3 : 6), padX: s.r(7), ledH: s.r(13), labelH: s.r(DECK.ledLabel), labelGap: s.r(DECK.ledLabelGap) };
  const ledGeom = useMemo(() => ({ padTop: g.padTop, padX: g.padX, ledH: g.ledH, labelH: 0, labelGap: g.labelGap }),
    [g.padTop, g.padX, g.ledH, g.labelGap]);
  return (
    <View style={[cd.housing, { height }]}>
      <View pointerEvents="none" style={cd.housingShade} />
      <View pointerEvents="none" style={[cd.lip, { backgroundColor: kind === 'vu' ? 'rgba(255,255,255,0.22)' : lip }]} />
      {kind === 'vu' && <LedVu bus={scaled} height={height} shared={shared} geom={ledGeom} onFault={onFault} />}
      {kind === 'vu' && g.labelH > 0 && (
        <VuScaleLabels unit={unit} padX={g.padX} top={g.padTop + g.ledH + g.labelGap} labelH={g.labelH} />
      )}
      {kind === 'edge' && (land ? (
        // §9: a 24 pt window, padding 2, the 28 pt print shown 2 pt up (Deck.mockup `svgTop: -2px`).
        <View style={{ padding: land.edgePad }}>
          <EdgeMeter bus={scaled} unit={unit} height={land.edgeWindow} printH={land.edgePrintH} printTop={land.edgePrintTop}
            onFault={onFault} />
        </View>
      ) : (
        // §4.5: a 28 pt window in the 34 pt housing (padding 3; 2 with the shared banner).
        <View style={{ padding: s.r(shared ? DECK.edgePadShared : DECK.edgePad) }}>
          <EdgeMeter bus={scaled} unit={unit} height={s.r(DECK.edgeWindow)} onFault={onFault} />
        </View>
      ))}
    </View>
  );
}

const cd = StyleSheet.create({
  banner:    { borderRadius: 6, backgroundColor: '#070605', alignItems: 'center', justifyContent: 'center',
               paddingHorizontal: 8 },
  bannerTxt: { letterSpacing: 1.8, fontWeight: '600', textShadowOffset: { width: 0, height: 0 }, textShadowRadius: 5 },
  window:    { flexDirection: 'row', alignItems: 'stretch', borderRadius: 8 },
  freqArea:  { flex: 1, minWidth: 0, flexDirection: 'row', alignItems: 'center', justifyContent: 'center',
               borderTopLeftRadius: 8, borderBottomLeftRadius: 8, overflow: 'hidden' },
  modeBox:   { borderLeftWidth: 1, alignItems: 'center', justifyContent: 'center', gap: 1 },
  shade:     { position: 'absolute', left: 0, right: 0, top: 0, height: 4, borderTopLeftRadius: 8,
               borderTopRightRadius: 8, backgroundColor: 'rgba(0,0,0,0.55)' },
  lip:       { position: 'absolute', left: 6, right: 6, bottom: -1, height: 1 },
  housing:   { borderRadius: 6, backgroundColor: '#070707' },
  housingShade: { position: 'absolute', left: 0, right: 0, top: 0, height: 3, borderTopLeftRadius: 6,
                  borderTopRightRadius: 6, backgroundColor: 'rgba(0,0,0,0.6)' },
});

const pm = StyleSheet.create({
  row:      { flexDirection: 'row', alignItems: 'stretch', justifyContent: 'center' },
  sharedBox:{ borderRadius: 5, paddingHorizontal: 8, paddingVertical: 2, marginBottom: 3, alignItems: 'center',
              borderWidth: 1,   // colour: ct.sharedBorder
              shadowColor: '#000', shadowOpacity: 0.6, shadowRadius: 4, shadowOffset: { width: 0, height: 1 }, elevation: 3 },
  /* ★ The SIZE comes from the pill (see sharedFontSize) — a fixed 11 fitted a Mac and crowded a phone, and a
   *  fixed 9 wasted the room a Mac has. Only the constants that do not depend on width live here. */
  sharedTxt:{ letterSpacing: 1.1, fontWeight: '700' },
  linkWrap: { flexDirection: 'row', alignItems: 'flex-end', gap: 1.5, alignSelf: 'center', flexShrink: 0 },
  linkBar:  { width: 3, borderRadius: 1 },
  /* ★★★ IT MUST WRAP, OR IT TRUNCATES — and it was truncating on a 17 PRO MAX, which is the
     biggest phone Apple sells: "IF 2800k au" with the rest simply gone (Stuart, 2026-09-24).
     Portrait has a full-width row and never showed it; LANDSCAPE puts this same indicator in the
     narrow column under the zoom keys, where a no-wrap row has nowhere to put the overflow.
     ★★ The content is already conditional (rate, AGC, IF each appear only when known), so the row
        is usually short — wrapping costs a second line only in the case that was previously
        losing information altogether.
     ★ It also protects the SE in Display Zoom, which is the narrowest layout we support and has
       caught this class of fault before. */
  linkRow:    { flexDirection: 'row', alignItems: 'center', gap: 4,
                flexWrap: 'wrap', justifyContent: 'center' },
  linkArrows: { fontSize: 9, lineHeight: 11 },   // colour: ct.linkDim
  /* ★ Same size and rhythm as the link stats beside them — these are a reading, not a button, and
     should not shout. The colour is the amber the rest of the active state uses. */
  dspRow:     { flexDirection: 'row', alignItems: 'center', gap: 6 },
  /* ★★★ AN ACTIVE BADGE MUST LOOK ACTIVE. These render ONLY when the treatment is on, so the
   *  reading "grey = inactive" is exactly backwards — and at 9 px they were, in Stuart's words,
   *  "a couple of grey initials [that] could mean anything" (2026-09-24). They are the only clue
   *  that the audio is being processed, which is the whole reason NR is allowed to persist across
   *  sessions: an invisible NR sounds like a broken receiver.
   *  ★ A tinted pill, not just coloured text: two letters at the edge of a dense status row need a
   *    shape to be found at a glance. Non-interactive in appearance, but still opens AUDIO. */
  dspTag:     { fontSize: 11, lineHeight: 13, letterSpacing: 1, fontWeight: '700',
                paddingHorizontal: 5, paddingVertical: 1, borderRadius: 4, overflow: 'hidden',
                borderWidth: 1 },   // ★ border + fill colours: the faceplate's dspTag tokens
  linkRate:   { fontSize: 9, lineHeight: 11, marginLeft: 4, fontVariant: ['tabular-nums'] },
  phoneGlyph: { width: 8, height: 13, borderWidth: 1, borderRadius: 2,
                alignItems: 'center', justifyContent: 'flex-end', paddingBottom: 1.5 },
  phoneDot:   { width: 2.5, height: 1.5, borderRadius: 1 },
  clockRow:   { flexDirection: 'row', alignItems: 'center', gap: 4, flexShrink: 1, minWidth: 0 },
  freqBox: { flexDirection: 'row', alignItems: 'flex-end', borderTopLeftRadius: 5, borderBottomLeftRadius: 5, flexShrink: 1 },
  freq:    { letterSpacing: 1.5, textAlign: 'center', textShadowOffset: { width: 0, height: 0 }, textShadowRadius: 6, flexShrink: 1 },
  unit:    { letterSpacing: 1, alignSelf: 'flex-end', paddingBottom: 2, flexShrink: 0 },
  /* ★ The airband channel's small line sits ABOVE the unit, so "MHz" keeps its baseline beside the digits. */
  chanCol: { alignItems: 'flex-end', justifyContent: 'flex-end', flexShrink: 0 },
  chanTag: { letterSpacing: 0.5, fontWeight: '700', includeFontPadding: false },
  modeBtn: { borderTopRightRadius: 5, borderBottomRightRadius: 5,
             borderLeftWidth: 1,   // colour: ct.modeDivider
             alignItems: 'center', justifyContent: 'center', gap: 1, flexShrink: 0 },
  modeLbl: { fontWeight: 'bold',   // glow colour: ct.modeGlow
             textShadowOffset: { width:0,height:0 }, textShadowRadius: 5 },
  snr:     { fontSize: 9, textAlign: 'center' },
});

// ── Cog icon ──────────────────────────────────────────────────────────────────
// Replaces the hamburger on the menu button. Once the ServersChip owns "leaving",
// this button is honestly just settings — and a cog says so, where the hamburger
// read as "exit" and hid the way back to the instance list. Colour matches the
// row's other glyphs (the faceplate's key legend), not a hard white.
// Authored in a 24×24 space (Feather "settings"); scale to the canvas.
// ★★ THE REAL FEATHER `settings` PATH, exactly as docs/faceplates/Deck.mockup.dc.html draws it. Ours had been
//   a corrupted copy since the start (8 19.4 for 9 19.4, H2 for H3, 3.6 8 for 4.6 9, H8 for 9 4.68 …) — the
//   left and top teeth pulled out of true: the "lopsided cog" Stuart saw in every version (B9).
const COG_GEAR   = Skia.Path.MakeFromSVGString('M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 1 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 1 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 1 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.68 15a1.65 1.65 0 0 0-1.51-1H3a2 2 0 1 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 1 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.68a1.65 1.65 0 0 0 1-1.51V3a2 2 0 1 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 1 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 1 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z')!;
const COG_CENTER = Skia.Path.MakeFromSVGString('M15 12a3 3 0 1 1-6 0 3 3 0 0 1 6 0z')!;

// ★ The key legends are DomeIcons: the same paths, drawn in the resolved legend colour with the
//   §5 flare (and, on silver/black, the controls-colour glow and the engraving's shadow).
const COG_STROKES:   IconStroke[] = [{ path: COG_GEAR, width: 1.7 }, { path: COG_CENTER, width: 1.7 }];
const AUDIO_STROKES: IconStroke[] = [{ path: SPEAKER_BODY, fill: true }, { path: SPEAKER_W1 }, { path: SPEAKER_W2 }];
const AUDIO_MUTED_STROKES: IconStroke[] = [
  { path: MUTE_BODY, fill: true }, { path: MUTE_W1, width: 1.2 }, { path: MUTE_W2, width: 1.2 },
  { path: MUTE_RING, width: 1.5 }, { path: MUTE_SLASH, width: 1.5 },
];
const CHAT_STROKES:  IconStroke[] = [{ path: CHAT_PATH }];

function Cog({ size, progress }: { size: number; progress?: SharedValue<number> }) {
  return <DomeIcon size={size} k={size / 24} strokes={COG_STROKES} progress={progress} />;
}

// ── Share icon canvas ─────────────────────────────────────────────────────────

function ShareIcon({ size, color }: { size: number; color: string }) {
  // Paths are authored in a 20×20 space — scale to the canvas, otherwise
  // small buttons (SE landscape) clip the icon edges
  const k = size / 20;
  return (
    <Canvas pointerEvents="none" style={{ width: size, height: size }}>
      <Group transform={[{ scale: k }]}>
        <Path path={SHARE_LINES} color={color} strokeWidth={1.6 / k} style="stroke" strokeCap="round" />
        <Path path={SHARE_C1}    color={color} strokeWidth={1.6 / k} style="stroke" />
        <Path path={SHARE_C2}    color={color} strokeWidth={1.6 / k} style="stroke" />
        <Path path={SHARE_C3}    color={color} strokeWidth={1.6 / k} style="stroke" />
      </Group>
    </Canvas>
  );
}

function ChatIcon({ size, progress, legend }: { size: number; progress?: SharedValue<number>; legend?: typeof LAMP_OFF_LEGEND }) {
  return <DomeIcon size={size} k={size / 20} strokes={CHAT_STROKES} progress={progress} legend={legend} />;
}

/**
 * ★★ A KEY WHOSE LAMP IS OFF (B10, Stuart): on a server with no chat the CHAT key stays a real key — it
 *   presses, sinks and clicks like the rest — but nothing lights it: no gap light, no flood, and its legend
 *   is the unlit engraving. That reads as "this does nothing here" the way a real radio's dark button does,
 *   where the old 40 % grey-out read as a broken or absent control.
 */
const LAMP_OFF_LEGEND = { color: 'rgba(128,120,110,0.55)', hot: 'rgba(128,120,110,0.55)',
                          glow: 'rgba(0,0,0,0)', shade: 'rgba(0,0,0,0.35)' };
const NOOP = () => {};

function AudioIcon({ size, progress, muted }: { size: number; progress?: SharedValue<number>; muted?: boolean }) {
  // Speaker cone (filled) + two sound-wave arcs; muted = the same speaker inside a prohibition sign.
  return <DomeIcon size={size} k={size / 20} strokes={muted ? AUDIO_MUTED_STROKES : AUDIO_STROKES} progress={progress} />;
}

/** The AUDIO key's legend. ★ The Mac MUTE (or its fader at zero) wins even over FM-DX's record disc: the
 *  key opens the popup that holds the MUTE key, so it is where a silent Mac has to say why. Only the
 *  LEGEND changes — the key's lighting, its gap light and the recording pulse overlay are untouched. */
function AudioKeyLegend({ size, progress, asRecord, muted }: {
  size: number; progress?: SharedValue<number>; asRecord?: boolean; muted: boolean;
}) {
  if (muted) return <AudioIcon size={size} progress={progress} muted />;
  return asRecord ? <RecordIcon size={size} progress={progress} /> : <AudioIcon size={size} progress={progress} />;
}

// FM-DX audio button = REC panel; a filled record disc reads clearer than a speaker.
function RecordIcon({ size, progress }: { size: number; progress?: SharedValue<number> }) {
  const dot = useFaceplate().chassis.recordDot;
  const strokes = useMemo<IconStroke[]>(() => [
    { path: RECORD_RING }, { path: RECORD_DOT, fill: true, color: dot },
  ], [dot]);
  return <DomeIcon size={size} k={size / 20} strokes={strokes} progress={progress} />;
}

/** The status rows: bare on the default glass deck, in a recessed window on a metal plate. */
function StatusWell({ plate, gap, style, children }: {
  plate: PlateTokens | null; gap: number; style?: ViewStyle; children: React.ReactNode;
}) {
  const fp = useFaceplate();
  const s = useUiScale();
  const sd = useMemo<StatusDisplay | null>(() => plate ? {
    // ★ Doto 900 12 pt, with §8.2's 10 pt floor (dot matrix falls apart below it; what does not fit
    //   is dropped by row 9, never squeezed). Text colour — neon under Nixie (the rule outranks it).
    font: FONT_DOTO, color: fp.text.core, glow: fp.text.glow, rgb: fp.text.rgb, size: Math.max(10, s.f(12)),
  } : null, [plate, fp.text, s]);
  if (!plate || !sd) return <>{children}</>;
  return (
    <StatusDisplayContext.Provider value={sd}>
      <RecessedWindow lip={plate.windowLip} style={{ gap, ...style }}>
        <GhostGrid rgb={sd.rgb} pitch={3} dot={0.7} />
        {children}
        {/* ★ The status display on silver / black is always dot-matrix — so it is glass, with wires
            (lighting brief §1). 4 = the window's 5 pt corner less its 1 pt border. */}
        <VfdFilaments radius={4} />
      </RecessedWindow>
    </StatusDisplayContext.Provider>
  );
}

// ── PORTRAIT ──────────────────────────────────────────────────────────────────

// Android: exclude the drum band from the system back-edge swipe so a horizontal
// drag on the VFO/zoom drum doesn't trigger (and animate, blocking the drum) the
// in-app-handled back gesture. Returns a ref + onLayout for the drum container.
function useDrumSwipeGuard() {
  const ref = useRef<View>(null);
  const setExcl = (NativeModules.VibePowerModule as { setSwipeExclusion?: (t: number, h: number) => void } | undefined)?.setSwipeExclusion;
  const onLayout = useCallback((e: { nativeEvent: { layout: { height: number } } }) => {
    if (Platform.OS !== 'android' || !setExcl) return;
    const lh = e.nativeEvent.layout.height;
    // Set the exclusion immediately (the drum sits high in the portrait controls,
    // so top=0 covers it) — measureInWindow's callback proved unreliable here and
    // often never fires. Still try to refine the absolute Y when measure works.
    setExcl(0, lh);
    ref.current?.measureInWindow((_x, y, _w, h) => { if (h > 0) setExcl(y, h); });
  }, [setExcl]);
  useEffect(() => () => { if (Platform.OS === 'android') setExcl?.(0, 0); }, [setExcl]);
  return { ref, onLayout };
}

/**
 * The handback flash, shared by the portrait and landscape bars.
 *
 * ★ Stuart: when focus moves from the decoder box back to tune/zoom, the drums or keys should
 * flash too. The decoder box's departure flash says "leaving"; without this nothing says
 * "arriving here", and on a TV across the room the controls that just became live are exactly
 * what you need to find.
 */
function useHandbackFlash() {
  const handback = useRegionHandback();
  const value = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    if (!handback) return;                    // not on mount, only on a real handback
    value.setValue(1);
    Animated.timing(value, { toValue: 0, duration: 450, useNativeDriver: true }).start();
  }, [handback, value]);
  return value;
}

function PortraitBar({ freqStr, unit, chanTag, chanMain, modeLabel, snrText, connected, signalActive, bus, meterMode, fmStereo = false,
  signal, peak, stepLabel, onFreqTap, onModeTap, onStep, onChat, onMenu, onAudio, audioAsRecord,
  dspNr, dspNb, dspAn,
  onVfoDelta, onBwDelta, clock, isRecording, recTime, chatUnread, csDisabled, chatOff, singleDrum, menuAsBack, vfoNoInertia,
  readOnly, sharedDial, storms, adminMode, vfoKeys, zoomKeys, onVfoStep, onZoomStep, onZoomSweep, vfoSweepRate,
  onControlRects, plateInset }: any) {
  const handbackFlash = useHandbackFlash();

  const { theme: t } = useTheme();
  // ★ Colours are the faceplate's: the chassis for keys, glass and status; the key LEGENDS resolve
  //   separately (§2 — white, or neon when the controls are neon, and Nixie One only when neon).
  const fp = useFaceplate();
  const ct = fp.chassis;
  // (The key legends — colour, font, glow, flare — are DomeText / DomeIcon's, from fp.keyLegend.)
  const s = useUiScale();
  const [sigW, setSigW] = useState(0);

  // Recording / chat-unread pulses. These drive an OVERLAY border's OPACITY with
  // the NATIVE driver (UI thread) — NOT borderColor with useNativeDriver:false.
  // The JS driver fires setNativeProps ~60fps, and under the New Architecture each
  // one forces a full-tree Yoga layout; on the FM-DX tuner (a large dial + many
  // absolutely-positioned tick/label nodes) that pegged the JS thread and iOS
  // killed the app for exceeding its background-CPU limit. Native opacity costs
  // nothing on the JS thread.
  const recPulse = useRef(new Animated.Value(0)).current;
  const macMuted = useMacSilenced();   // ★ the Mac MUTE's legend — false on anything but a Mac
  useEffect(() => {
    if (isRecording) {
      const a = Animated.loop(Animated.sequence([
        Animated.timing(recPulse, { toValue: 1, duration: 2500, useNativeDriver: true }),
        Animated.timing(recPulse, { toValue: 0, duration: 2500, useNativeDriver: true }),
      ]));
      a.start();
      return () => { a.stop(); recPulse.setValue(0); };
    }
    recPulse.setValue(0);
  }, [isRecording, recPulse]);

  const chatPulse = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    if (chatUnread) {
      const a = Animated.loop(Animated.sequence([
        Animated.timing(chatPulse, { toValue: 1, duration: 2500, useNativeDriver: true }),
        Animated.timing(chatPulse, { toValue: 0, duration: 2500, useNativeDriver: true }),
      ]));
      a.start();
      return () => { a.stop(); chatPulse.setValue(0); };
    }
    chatPulse.setValue(0);
  }, [chatUnread, chatPulse]);

  // All dp values go through s.r() — port of applyUiScale()'s r() function
  // (The bar's height — 40, taller on tablet so the meter shows above/below the tall two-line pill —
  //  is the deck's: dl.displayH below.)
  const DRUM_H     = s.r(60);
  const ROW_GAP    = s.r(7);
  const COL_GAP    = s.r(8);
  const BAR_PAD_H  = s.r(12);
  // ★ Silver/black keys sit in the mockup's 58 pt slot with a 54 pt cap (§4.1, bar meter); the
  //   default key stays today's 44 pt minimum (a11y minimum touch target — was 36, misses).
  const isCap      = ct.dome.look === 'cap';
  const gloss      = !!ct.plate?.gloss && !!plateInset;
  /* ★★★ ONE DECK HEIGHT (§4.1). The display area and the keys come out of ONE fixed block per
   *  chassis (constants/meters.ts portraitDeck): on the LED / analogue deck the frequency window
   *  flexes and the keys take the §4.1 slot, so neither the meter type nor a shared server can grow
   *  or shrink the deck. The bar keeps today's sizes (§4.2); on silver / black its frame is the
   *  mockup's 72 pt so the bar deck is the same height as the other two. */
  const meterKind  = fp.settings.meter;
  const dl         = portraitDeck({ cap: isCap, meter: meterKind, shared: !!sharedDial, tablet: s.isTablet,
                                    rowGap: ROW_GAP, r: s.r });
  const KEY_SLOT   = dl.keySlot;
  const pulseR     = !isCap ? 4 : dl.compact ? s.r(8) : 10;
  const ICON_SZ    = Math.round(s.r(20) * dl.legendScale);
  // ★ §4.1 TRAP: a 34 pt key is below 44 pt — its hitSlop reaches into the gaps round it.
  const keySlop    = dl.compact ? compactKeyHitSlop(KEY_SLOT, ROW_GAP, COL_GAP) : undefined;
  // ★★ THE PANEL-GAP LIGHT (constants/keyLight.ts): each key's light may take its share of the tightest
  //   gap round it — the row gap to the display above and the drums below, the column gap to its
  //   neighbours — so two keys' lights never run together into one glowing bar.
  const lightReach = keyLightReach(Math.min(ROW_GAP, COL_GAP));
  const keyProps   = dl.compact
    ? { height: KEY_SLOT, radius: s.r(8), lightReach, hitSlop: keySlop && { top: keySlop.top, bottom: keySlop.bottom,
                                                               left: keySlop.left, right: keySlop.right } }
    : { height: KEY_SLOT, minHeight: true, lightReach };
  // Freq/mode sizing — read from theme so white mode can increase them
  // Pill sized to leave the signal bar visible around it (the white theme's
  // 28pt/168w pill covered the whole frame — screenshots 2026-06-11; 23/138
  // was still too wide on a 390pt screen). Android renders the same dp
  // sizes WIDER (font metrics) and the pill swallowed the meter on the G35
  // AND the iPhone SE — tighter sizes on Android and on narrow screens
  // (screenshots 2026-06-12).
  const tight      = Platform.OS === 'android' || s.isSmall;
  const FREQ_FONT  = s.r(tight ? 19 : 22);
  const FREQ_W     = s.r(tight ? 112 : 130);

  const { ref: drumRowRef, onLayout: guardDrums } = useDrumSwipeGuard();
  const UNIT_FONT  = s.r(9);
  const MODE_FONT  = s.r(tight ? 12 : 13);
  const MODE_LS    = s.f(t.modeLs);
  const SNR_W      = s.r(tight ? 50 : 58);
  const PILL_PAD_H = s.r(7);
  // Slim vertical paddings — the pill must float INSIDE the meter frame
  // with the signal ring visible above and below (boxes were touching the
  // frame edges on SE/G35, screenshots 2026-06-12 eve)
  const PILL_PAD_V = s.r(3);
  const MODE_PAD_H = s.r(10);
  const MODE_PAD_V = s.r(3);
  const PILL_GAP   = s.r(5);
  const BTN_FONT   = s.f(t.btnSize);
  const CLOCK_FONT = s.f(8);

  return (
    <View style={{ gap: ROW_GAP }}>

      {/* Row 1 — signal bar.
          ★ On black it sits on the GLOSS ACRYLIC PANEL, which runs to the plate's top and side edges
            (Deck.mockup `displayPanel`: margin −14, padding 14 14 12) with its trim line beneath. */}
      <View style={gloss ? { marginHorizontal: -plateInset.h, marginTop: -plateInset.top,
                             paddingHorizontal: plateInset.h, paddingTop: plateInset.top,
                             paddingBottom: s.r(12) } : undefined}>
      {gloss && <GlossPanel radius={plateInset.radius} squareBottom />}
      {dl.compact ? (
        <CompactDisplay dl={dl} meterKind={meterKind}
          freqStr={freqStr} unit={unit} chanTag={chanTag} chanMain={chanMain} modeLabel={modeLabel} snrText={snrText}
          signalActive={signalActive} bus={bus} meterMode={meterMode} fmStereo={fmStereo}
          onFreqTap={onFreqTap} onModeTap={onModeTap} sharedTuner={sharedDial ?? null} tight={tight}
          freqWidth={FREQ_W} />
      ) : (
      <View style={[por.sigFrame, { height: dl.displayH }]}
            onLayout={(e: any) => setSigW(e.nativeEvent.layout.width)}>
        <SignalCanvas width={sigW} height={dl.displayH} signal={signal} peak={peak} bus={bus} />
        <FreqModePill
          freqStr={freqStr} unit={unit} chanTag={chanTag} chanMain={chanMain} modeLabel={modeLabel} snrText={snrText}
          connected={connected} signalActive={signalActive} bus={bus} meterMode={meterMode} fmStereo={fmStereo}
          onFreqTap={onFreqTap} onModeTap={onModeTap}
          freqFontSize={FREQ_FONT} freqWidth={FREQ_W} unitFontSize={UNIT_FONT}
          modeFontSize={MODE_FONT} modeLs={MODE_LS} snrWidth={SNR_W}
          pillPadH={PILL_PAD_H} pillPadV={PILL_PAD_V}
          modePadH={MODE_PAD_H} modePadV={MODE_PAD_V} gap={PILL_GAP}
          tight={tight} sharedTuner={sharedDial ?? null}
        />
      </View>
      )}
      </View>

      {/* Row 2 — 4 equal buttons */}
      <View style={{ flexDirection: 'row', gap: COL_GAP }}>

        {/* ★★★ THE FOUR MAIN KEYS ARE DOME KEYS (§5) on every chassis — they had no haptics at all
            before. Default keeps today's outline key at rest; it now snaps and clicks. The ACTION
            is on release; the depress, click and flare are on touch-down. */}
        {/* STEP */}
        <DomeKey ref={tourRef('stepBtn')} style={por.key} {...keyProps}
          onPress={onStep} accessibilityLabel="Tuning step">
          {p => <DomeText progress={p} style={[por.btnTxt, { fontSize: BTN_FONT * dl.legendScale }]}>{stepLabel}</DomeText>}
        </DomeKey>

        {/* AUDIO — opens the audio sheet; breathes red↔white while recording
            (REC lives inside the sheet, so this is the tap target to stop it). */}
        <DomeKey style={por.key} {...keyProps} onPress={onAudio}
          accessibilityLabel={`${audioAsRecord ? 'Record' : 'Audio'}${macMuted ? ', muted' : ''}`}
          overlay={<Animated.View pointerEvents="none"
            style={[StyleSheet.absoluteFill, { borderRadius: pulseR, borderWidth: 1, borderColor: ct.keyPulseRec, opacity: recPulse }]} />}>
          {p => <AudioKeyLegend size={ICON_SZ} progress={p} asRecord={audioAsRecord} muted={macMuted} />}
        </DomeKey>

        {/* MENU */}
        <DomeKey ref={tourRef('menuBtn')} style={por.key} {...keyProps} onPress={onMenu}
          accessibilityLabel={menuAsBack ? 'Back' : 'Settings'}>
          {p => menuAsBack
            ? <DomeText progress={p} style={{ fontSize: s.f(t.btnSize) * dl.legendScale }}>‹ Back</DomeText>
            : <Cog size={ICON_SZ} progress={p} />}
        </DomeKey>

        {/* CHAT */}
        <DomeKey style={por.key} {...keyProps} lightReach={chatOff ? 0 : keyProps.lightReach}
          onPress={chatOff ? NOOP : onChat} accessibilityLabel="Chat"
          accessibilityHint={chatOff ? 'Chat is not available on this server' : undefined}
          overlay={<Animated.View pointerEvents="none"
            style={[StyleSheet.absoluteFill, { borderRadius: pulseR, borderWidth: 1, borderColor: ct.keyPulseChat, opacity: chatPulse }]} />}>
          {p => <ChatIcon size={ICON_SZ} progress={p} legend={chatOff ? LAMP_OFF_LEGEND : undefined} />}
        </DomeKey>

      </View>

      {/* Row 3 — drums (single full-width VFO for FM-DX; vfo+zoom otherwise).
          Greyed and inert on a read-only receiver: another client owns the tuner,
          so the drums would spin and change nothing.
          ★★★ ABSENT ON APPLE TV. A drum is a DRAG control: you throw it and it spins with
          inertia. There is nothing on a Siri Remote to drag it with — the ring TUNES directly and
          the touch surface moves the highlight — so on a TV it would be a control that cannot be
          operated at all. Removed rather than disabled, per AGENTS.md: an inert control reads as
          a broken FEATURE, not a missing one. Tuning is not lost; the ring does it, always. */}
      {!IS_TV && <View ref={mergeRefs(drumRowRef, tourRef('vfoDrum'))} onLayout={guardDrums}
            pointerEvents={readOnly ? 'none' : 'auto'}
            style={{ flexDirection: 'row', gap: COL_GAP, opacity: readOnly ? 0.35 : 1 }}>
        {/* ★ HANDBACK FLASH. When the decoder box gives the keyboard back, the controls that
            just became live announce themselves — the same arrival/departure idea, applied to
            the other end of the move. Drawn as a glow OVER the row rather than inside the two
            controls, so it works for the drums and the keys without either knowing about it.
            pointerEvents none: a signal, never a target. */}
        <Animated.View pointerEvents="none"
          style={{
            position: 'absolute', left: -4, right: -4, top: -4, bottom: -4,
            borderRadius: 12, borderWidth: 2, borderColor: NAV_FOCUS,
            backgroundColor: ct.handbackBg,
            opacity: handbackFlash, zIndex: 3,
          }} />
        <ControlSlot style={{ flex: 1 }} report={r => onControlRects?.({ vfo: r })}>
          {vfoKeys
            ? <TunerKeys type="vfo" height={DRUM_H} onStep={onVfoStep ?? noStep} sweepRate={vfoSweepRate} />
            : <DrumWheel type="vfo" height={DRUM_H} onDelta={onVfoDelta} noInertia={vfoNoInertia} />}
        </ControlSlot>
        {!singleDrum && (
          <ControlSlot style={{ flex: 1 }} report={r => onControlRects?.({ zoom: r })}>
            {zoomKeys
              ? <TunerKeys type="zoom" height={DRUM_H} onStep={onZoomStep ?? noStep} onSweepStep={onZoomSweep} />
              : <DrumWheel type="zoom" height={DRUM_H} onDelta={onBwDelta} />}
          </ControlSlot>
        )}
      </View>}

      {/* Rows 4–5 — the status. ★ On silver / black it is the recessed STATUS DISPLAY (§8.1): Doto in
          the text colour over the ghost grid, two lines in portrait — the clocks, then the link. The
          default deck keeps today's two-line footer untouched. */}
      <StatusWell plate={ct.plate} gap={ROW_GAP}>
      {/* Row 4 — clock · link quality · rec */}
      <View style={por.clockRow}>
        {/* ★★ minWidth 0 + shrink, OR THE CLOCK RUNS UNDER THE LINK ICONS. A row child's default
            minWidth is its content, so `flex: 1` alone does not let this group get smaller than the
            clock plus whatever else is in it — it overflowed instead, and on the phone the time was
            printed straight through the icons and the rate ("the clock is clipping the status
            icons", Stuart, 2026-09-20). Shrinking is what should give when the row is tight. */}
        <View style={{ flex: 1, minWidth: 0, flexShrink: 1, flexDirection: 'row', alignItems: 'center', gap: 8 }}>
          <ClockRow clock={clock} color={ct.clock} font={t.font} size={CLOCK_FONT} />
          {/* Time-limited receiver: how long before the server drops us. */}
          {adminMode ? (
            <StatusText style={{ color: ct.clock, fontFamily: t.font, fontSize: CLOCK_FONT,
                           opacity: 0.9 }}
                  accessibilityLabel="Admin mode — this session is not time limited">
              ⚿ Admin Mode
            </StatusText>
          ) : null}
          {/* ★★★ NO SECOND COUNTDOWN HERE. The session timer already has a large, legible card over
              the spectrum ("GUARANTEED TIME ENDS IN 29:13"), and repeating it as a 9pt shield in
              the status row was both redundant and the thing that pushed this row over its width —
              the clock printed straight through the link icons because of it.
              ★★ Stuart, 2026-09-20: "that super tiny shield and countdown in the clock confused me and
                 that was what caused the earlier clipping and isnt needed as there is a large easier
                 to see clock in the spectrum anyway."
              ★ The ADMIN chip above STAYS: an admin session shows no card over the spectrum
                (see SDRScreen's rxClock, which is suppressed when adminOk), so this is its only
                indication — removing it would leave nothing at all. */}
          {/* The room, beside the clock — the same kind of fact as "how long have I got", and read
              at the same moment. ★ Green when you are alone: a colour you can take in without
              reading, because the point is to answer "may I just tune?" at a glance. */}
          {/* ★ Who is moving the dial. The room's COUNT moved to the shared-tuner banner (2026-09-19). */}
          {!!sharedDial?.tuning && (
            <StatusText style={{ color: ct.clock, fontFamily: t.font, fontSize: CLOCK_FONT, opacity: 0.9 }}
                  numberOfLines={1}>
              {sharedDial.tuning}
            </StatusText>
          )}
          {!!storms && (
            <StatusText style={{ color: ct.srvClock, fontFamily: t.font, fontSize: CLOCK_FONT, opacity: 0.9, letterSpacing: 1 }}
                  numberOfLines={1}
                  accessibilityLabel={`Lightning nearby — the broadband lines across the spectrum are sferics, not a fault (about ${Math.round(storms.rate)} a minute`
                    + (storms.ago >= 0 && storms.ago < 90 ? `, last ${Math.round(storms.ago)} seconds ago)` : ')')}>
              ⚡ STORMS
            </StatusText>
          )}
        </View>
        {/* ★★ THE RECORDING TIMER BELONGS BESIDE THE CLOCK, not out on the right. Pinned to the
            right-hand end it collided with the connection stats, which have grown as the AGC
            readouts were added — "now the recording clock clips" (Stuart, 2026-09-20), with
            0:00:10 printed through "IF wide auto".
            ★★ And it is the same shape as landscape: CLOCK ON THE LEFT, STATUS ON THE RIGHT. The two
               orientations now read the same way round, which is what he asked for.
            ★ It is a time, so it sits with the other times — the grouping was always wrong; it was
              only invisible while the stats were short enough to leave a gap. */}
        {isRecording && (
          <View style={[por.recRow, { flexShrink: 0 }]}>
            <View style={[por.recDot, { backgroundColor: ct.recRed }]} />
            <StatusText keepColor style={[por.recTime, { color: ct.recRed, fontFamily: t.font, fontSize: CLOCK_FONT }]}>{recTime}</StatusText>
          </View>
        )}
        {/* ★★★ THE AUDIO CHAIN, ON THE END OF THE TIMES ROW — and the STATS get a line of their
            own below. The two were sharing one line and the stats were losing: "IF 2800k au" with
            the rest cut off, on a 17 Pro Max (Stuart, 2026-09-24). A row that truncates the thing
            it exists to report is not a status row.
            ★★ The pill grows DOWNWARD into space that was dead anyway — non-interactive text, so
               it may sit close to the bottom, but it stays clear of the home indicator (the safe
               area inset is applied by the screen, not here). */}
        <DspBadges nr={dspNr} nb={dspNb} an={dspAn} onPress={onAudio}
                   font={t.font} />
      </View>

      {/* Row 5 — the connection stats, on their own line so they can no longer be truncated. */}
      <View style={por.statsRow}><LinkIndicator bus={bus} /></View>
      </StatusWell>

    </View>
  );
}

const por = StyleSheet.create({
  sigFrame: { borderRadius: 7, overflow: 'hidden', justifyContent: 'center' },   // track: SignalCanvas draws ct.meterTrack
  // ★ The key's outline and tint are DomeKey's (today's values on the default chassis); the bar
  //   only shares the row out.
  key:      { flex: 1 },
  btnTxt:   { letterSpacing: 0.5, textAlign: 'center' },
  clockRow: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center', paddingHorizontal: 2 },
  /* ★ Its own line, centred like landscape's. The stats are the widest thing in the bar and the
     only one that was being cut off; given a row to themselves they simply fit. */
  statsRow: { flexDirection: 'row', justifyContent: 'center', alignItems: 'center',
              paddingHorizontal: 2, marginTop: 1 },
  clock:    { letterSpacing: 1 },
  recRow:   { flexDirection: 'row', alignItems: 'center', gap: 4 },
  recDot:   { width: 6, height: 6, borderRadius: 3 },   // colour: ct.recRed
  recTime:  { letterSpacing: 1 },
});

// ── LANDSCAPE ─────────────────────────────────────────────────────────────────

function LandscapeBar({ freqStr, unit, chanTag, chanMain, modeLabel, snrText, connected, signalActive, bus, meterMode, fmStereo = false,
  signal, peak, stepLabel, onFreqTap, onModeTap, onStep, onChat, onMenu, onAudio, audioAsRecord,
  dspNr, dspNb, dspAn,
  onVfoDelta, onBwDelta, clock, isRecording, recTime, chatUnread, chatOff, singleDrum, menuAsBack, vfoNoInertia,
  /* ★★★ sharedDial WAS MISSING FROM THIS LIST AND USED IN THE BODY. The props arrive as {...shared}, so the
   *  name simply was not in scope and the landscape bar threw "Property 'sharedDial' doesn't exist" the moment
   *  it rendered — the whole app bounced back to the server list (Stuart, 2026-09-20). PortraitBar destructures
   *  it and worked; its twin did not, which is why it survived review: the same JSX, one bar broken.
   *  ★★ A destructured prop list is a hand-maintained copy of the props — anything the body uses must be in it. */
  sharedDial,
  vfoKeys, zoomKeys, onVfoStep, onZoomStep, onZoomSweep, vfoSweepRate, onDrumAnchors }: any) {
  const handbackFlash = useHandbackFlash();
  const { theme: t } = useTheme();
  /* ★ The station strip anchors its text between the VFO drum's + and the zoom drum's − (DrumWheel draws them
   *  max(3, 5 % of the drum) in from its edges). Measured in WINDOW coordinates, like onControlRects. */
  const drumRects = useRef<{ vfo?: Rect; zoom?: Rect }>({});
  const reportAnchors = useCallback(() => {
    const { vfo, zoom } = drumRects.current;
    if (!vfo || !zoom) return;
    onDrumAnchors?.({ left: vfo.x + vfo.w - Math.max(3, vfo.w * 0.05), right: zoom.x + Math.max(3, zoom.w * 0.05) });
  }, [onDrumAnchors]);
  const onVfoRect = useCallback((r: Rect) => { drumRects.current.vfo = r; reportAnchors(); }, [reportAnchors]);
  const onZoomRect = useCallback((r: Rect) => { drumRects.current.zoom = r; reportAnchors(); }, [reportAnchors]);
  useEffect(() => () => onDrumAnchors?.(null), [onDrumAnchors]);
  useEffect(() => { if (singleDrum) { drumRects.current.zoom = undefined; onDrumAnchors?.(null); } }, [singleDrum, onDrumAnchors]);
  // ★ Colours are the faceplate's: the chassis for keys, glass and status; the key LEGENDS resolve
  //   separately (§2 — white, or neon when the controls are neon, and Nixie One only when neon).
  const fp = useFaceplate();
  const ct = fp.chassis;
  // (The key legends — colour, font, glow, flare — are DomeText / DomeIcon's, from fp.keyLegend.)
  const s = useUiScale();
  const [sigW, setSigW] = useState(0);
  const macMuted = useMacSilenced();   // ★ the Mac MUTE's legend — false on anything but a Mac

  /* ★★★ THE LANDSCAPE DECK (§9) — constants/meters.ts landscapeDeck, pure and tested
   *  (scripts/test_faceplate_landscape.ts). ONE band for every meter × shared state. On the SE it is
   *  TODAY's band (the taller of the 44 pt drum and the bar frame; 62 on a tablet); on a taller phone it
   *  GROWS with the window's height in points to the mockup's 62 pt (landscapeBand — build 356, "the
   *  Nixies are tiny in landscape on a 17 Pro Max"). The LED / analogue column is fitted into it — the
   *  frequency window flexes, then the labels go, and on the SE the analogue card becomes the bar.
   *  ★ The default chassis's BAR keeps today's band everywhere (§3.1 pixel for pixel). */
  const isCap     = ct.dome.look === 'cap';
  const lay       = landscapeDeck({ plate: ct.plate ? { screws: ct.plate.screws, gloss: ct.plate.gloss } : null,
                                    meter: fp.settings.meter, tablet: s.isTablet, W: s.W, scale: s.scale, r: s.r,
                                    singleDrum: !!singleDrum, H: s.H, display: fp.settings.display });
  const BAND_H    = lay.bandH;
  const SIG_H     = lay.barH;                // the bar frame (bar meter only)
  const GAP       = lay.rowGap;
  const COL_GAP   = lay.colGap;
  const BTN_W     = lay.keyW;
  const { ref: drumRowRef, onLayout: guardDrums } = useDrumSwipeGuard();
  // Pill enlarged toward portrait proportions — at 20pt in a 48pt frame the
  // signal bar visually swallowed it (screenshots 2026-06-11).
  const FREQ_FONT = s.r(24);
  const FREQ_W    = s.r(148);
  const UNIT_FONT = s.r(9);
  const MODE_FONT = s.r(15);
  const MODE_LS   = s.f(t.modeLs > 1.5 ? 1.2 : 1.0);
  const SNR_W     = s.r(74);
  const PILL_PAD_H = s.r(5);
  const PILL_PAD_V = s.r(3);
  const MODE_PAD_H = s.r(7);
  const MODE_PAD_V = s.r(4);
  const PILL_GAP  = s.r(4);
  const CLOCK_FONT = s.f(7);
  /* ★★★ EVERY LANDSCAPE KEY IS KEY_H TALL, SET, NOT NEGOTIATED (brief §11). The keys used to be
     `flex: 1` in their column, and a flex item's share is argued out against its CONTENT: the step
     key's Text (scaled lineHeight, adjustsFontSizeToFit) and the cog's Skia Canvas report different
     intrinsic heights, so on a live screenshot "1k" sat SHORTER than the cog beneath it and the
     audio/chat pair disagreed too. Now each key is exactly half the band less the gap (the mockup's
     28 / 28 rows on a tablet) — in every step size and in the menu-as-back state.
     ★ The icon is capped to the key rather than the key grown to the icon: a key must never be the
       thing that makes the bar taller. On a cap key (silver / black) the legends are the mockup's
       78 % of the portrait legend, inside the cap (the slot less its 2 pt inset top and bottom). */
  const KEY_H     = lay.keyH;
  // ★★ The panel-gap light (constants/keyLight.ts) — its share of the tightest gap round a landscape key:
  //   the row gap between the two stacked keys, the column gap to the drum and the display.
  const lightReach = keyLightReach(Math.min(GAP, COL_GAP));
  const ICON_SZ   = isCap ? Math.max(8, Math.min(Math.round(s.r(20) * lay.legendScale), KEY_H - 6))
                          : Math.min(s.r(18), KEY_H - 2);   // − the 1 pt border top and bottom
  const KEY_FONT  = isCap ? s.f(t.btnSize) * lay.legendScale : s.f(11);
  const KEY_LH    = isCap ? Math.round(KEY_FONT * 1.27) : s.f(14);   // default: today's exactly
  const compact   = lay.meter !== 'bar';
  /* The LED / analogue column as CompactDisplay reads it: the band, no banner (§9: the SHARED TUNER
     banner lives in the status row in landscape, so the display column never grows). */
  const dl        = useMemo<DeckLayout>(() => ({
    compact: true, displayH: BAND_H, keySlot: KEY_H, legendScale: lay.legendScale, bannerH: 0, bannerGap: 0,
    meterGap: lay.meterGap, housingH: lay.housingH, freqH: lay.freqH, blockH: BAND_H,
  }), [BAND_H, KEY_H, lay.legendScale, lay.meterGap, lay.housingH, lay.freqH]);
  const dispH     = compact ? BAND_H : SIG_H;

  return (
    /* ★ A COLUMN NOW: the controls in one row, the status in another beneath it. This function's
       root used to BE the drum row, which is why the clock and the stats had to be tucked inside
       the drum columns — there was nowhere else for them to go. */
    <View>
    <View ref={drumRowRef} onLayout={guardDrums} style={{ flexDirection: 'row', alignItems: 'stretch', justifyContent: 'center', gap: COL_GAP, height: BAND_H }}>

      {/* ★ Handback flash — see useHandbackFlash. The landscape bar has its own drum row, so
          without this the announcement simply vanished on rotation. */}
      <Animated.View pointerEvents="none"
        style={{
          position: 'absolute', left: -4, right: -4, top: -4, bottom: -4,
          borderRadius: 12, borderWidth: 2, borderColor: NAV_FOCUS,
          backgroundColor: ct.handbackBg,
          opacity: handbackFlash, zIndex: 3,
        }} />

      {/* VFO drum. ★ §9: the well stretches to its column and is sized to the BAND, not flex-stretched —
          `flex: 1` on the drum let its view grow past the height its canvas was drawn at (a tablet's
          44 pt drum sat in a 62 pt band). The trapezoid is 40 % of it (25 pt of the mockup's 62). */}
      <View ref={tourRef('vfoDrum')} style={{ flex: 1, minWidth: s.r(80) }}>
        <ControlSlot style={{ flex: 1 }} report={onVfoRect}>
        {vfoKeys
          ? <TunerKeys type="vfo" height={BAND_H} onStep={onVfoStep ?? noStep} sweepRate={vfoSweepRate} landscape />
          : <DrumWheel type="vfo" height={BAND_H} onDelta={onVfoDelta} noInertia={vfoNoInertia} />}
        </ControlSlot>
      </View>

      {/* STEP + MENU column */}
      <View style={{ width: BTN_W, gap: GAP, justifyContent: 'center' }}>
        {/* ★ Dome keys (§5), KEY_H tall exactly (§11). Under 44 pt, so hitSlop reaches into the gaps. */}
        <DomeKey ref={tourRef('stepBtn')} style={lnd.lsKey} height={KEY_H} radius={6} lightReach={lightReach}
          onPress={onStep} accessibilityLabel="Tuning step">
          {/* ★ ONE line: "100k" / "500Hz" / "8.33k" SHRINK to fit the key; two lines let the text
              ask for a taller box, which is the bug this key had. */}
          {p => <DomeText progress={p} style={[lnd.lsTxt, { fontSize: KEY_FONT, lineHeight: KEY_LH }]}
                  numberOfLines={1} adjustsFontSizeToFit minimumFontScale={0.6}>{stepLabel}</DomeText>}
        </DomeKey>
        <DomeKey ref={tourRef('menuBtn')} style={lnd.lsKey} height={KEY_H} radius={6} lightReach={lightReach}
          onPress={onMenu} accessibilityLabel={menuAsBack ? 'Back' : 'Settings'}>
          {p => menuAsBack
            ? <DomeText progress={p} style={{ fontSize: KEY_FONT, lineHeight: KEY_LH }} numberOfLines={1}>‹</DomeText>
            : <Cog size={ICON_SZ} progress={p} />}
        </DomeKey>
      </View>

      {/* ★★★ THE DISPLAY COLUMN — frequency on top, meter underneath (§9), or today's bar.
          TOP-ALIGNED, NOT CENTRED: justifyContent:'center' inside a row whose alignItems is
          'stretch' floated this box in the middle of the tallest column, leaving black padding
          above AND below it — "Landscape is wasting space, there is black padding above the
          frequency/signal meter box" (Stuart, 2026-09-24). */}
      <View style={{ width: lay.dispW, justifyContent: 'flex-start' }}
            onLayout={(e: any) => setSigW(e.nativeEvent.layout.width)}>
        {/* ★ Black: the gloss panel wraps the frequency AND the meter together (§9; Deck.mockup
            landscape `gloss`, radius 10). Drawn 4 pt OUTSIDE the column, in the gaps, so it adds no
            height to the band (the mockup pads 4 INSIDE a 62 pt column; ours is today's band, and
            the window cannot spare 8 pt). */}
        {ct.plate?.gloss && <GlossPanel radius={10} trim={false}
          style={{ top: -lay.glossOut, left: -lay.glossOut, right: -lay.glossOut, bottom: 'auto',
                   height: dispH + 2 * lay.glossOut }} />}
        {compact ? (
          <CompactDisplay dl={dl} land={lay} meterKind={lay.meter}
            freqStr={freqStr} unit={unit} chanTag={chanTag} chanMain={chanMain} modeLabel={modeLabel} snrText={snrText}
            signalActive={signalActive} bus={bus} meterMode={meterMode} fmStereo={fmStereo}
            onFreqTap={onFreqTap} onModeTap={onModeTap} sharedTuner={null} tight={false}
            freqWidth={FREQ_W} />
        ) : (
        <View style={[lnd.sigFrame, { height: SIG_H }]}>
          <SignalCanvas width={sigW} height={SIG_H} signal={signal} peak={peak} bus={bus} />
          {/* ★ No banner in the pill: in landscape the SHARED TUNER banner lives in the status row
              (§9, Deck.mockup `sharedInBar: false`), so the frequency keeps its size on a shared dial. */}
          <FreqModePill
            freqStr={freqStr} unit={unit} chanTag={chanTag} chanMain={chanMain} modeLabel={modeLabel} snrText={snrText}
            connected={connected} signalActive={signalActive} bus={bus} meterMode={meterMode} fmStereo={fmStereo}
            onFreqTap={onFreqTap} onModeTap={onModeTap}
            freqFontSize={FREQ_FONT} freqWidth={FREQ_W} unitFontSize={UNIT_FONT}
            modeFontSize={MODE_FONT} modeLs={MODE_LS} snrWidth={SNR_W}
            pillPadH={PILL_PAD_H} pillPadV={PILL_PAD_V}
            modePadH={MODE_PAD_H} modePadV={MODE_PAD_V} gap={PILL_GAP}
            sharedTuner={null}
          />
        </View>
        )}
      </View>

      {/* AUDIO + CHAT column */}
      <View style={{ width: BTN_W, gap: GAP, justifyContent: 'center' }}>
        {/* ★ Recording / unread chat: the outline turns red / blue on the default key (today's
            signal); a cap has no outline, so there it is a ring round the slot. */}
        <DomeKey style={lnd.lsKey} height={KEY_H} radius={6} lightReach={lightReach} onPress={onAudio}
          outline={isRecording ? ct.keyBorderRec : undefined}
          overlay={isCap && isRecording ? <View pointerEvents="none" style={[StyleSheet.absoluteFill,
            { borderRadius: 6, borderWidth: 1, borderColor: ct.keyBorderRec }]} /> : undefined}
          accessibilityLabel={`${audioAsRecord ? 'Record' : 'Audio'}${macMuted ? ', muted' : ''}`}>
          {p => <AudioKeyLegend size={ICON_SZ} progress={p} asRecord={audioAsRecord} muted={macMuted} />}
        </DomeKey>
        <DomeKey style={lnd.lsKey} height={KEY_H} radius={6} lightReach={chatOff ? 0 : lightReach}
          onPress={chatOff ? NOOP : onChat}
          accessibilityHint={chatOff ? 'Chat is not available on this server' : undefined}
          outline={chatUnread ? ct.keyBorderChat : undefined}
          overlay={isCap && chatUnread ? <View pointerEvents="none" style={[StyleSheet.absoluteFill,
            { borderRadius: 6, borderWidth: 1, borderColor: ct.keyBorderChat }]} /> : undefined}
          accessibilityLabel="Chat">
          {p => <ChatIcon size={ICON_SZ} progress={p} legend={chatOff ? LAMP_OFF_LEGEND : undefined} />}
        </DomeKey>
      </View>

      {/* Zoom drum (omitted for FM-DX single-drum tuner) — sized to the band, like the VFO's. */}
      {!singleDrum && (
        <ControlSlot style={{ flex: 1, minWidth: s.r(80) }} report={onZoomRect}>
          {zoomKeys
            ? <TunerKeys type="zoom" height={BAND_H} onStep={onZoomStep ?? noStep} onSweepStep={onZoomSweep} landscape />
            : <DrumWheel type="zoom" height={BAND_H} onDelta={onBwDelta} />}
        </ControlSlot>
      )}

      </View>

      {/* ★★★ ONE FULL-WIDTH STATUS ROW, BENEATH EVERYTHING — Stuart's layout, 2026-09-24:
          "have a clear row at the bottom … Server time/UTC Recording Timer | NR/NB/AN | sig 25KB
          20FPS Gain 29db IF Wide".
          ★★ It replaces two cramped half-rows tucked under the drums, and it is why the stats can
             stop truncating: they had the width of ONE COLUMN and now have the bar.
          ★ Times left, audio chain centre, link right — and the recording slot keeps its reserved
            space so nothing resizes under a thumb when recording starts (the reason it was pulled
            out of the tuning column in the first place).
          ★★ §8.1 / §9: ONE line in landscape, and on a shared dial SHARED TUNER sits in the CENTRE
             (replacing the node icon at the link's end) — the banner moved here so the display
             column never grows. */}
      <LandscapeStatus plate={ct.plate} marginTop={GAP} clock={clock} font={t.font} clockFont={CLOCK_FONT}
        isRecording={isRecording} recTime={recTime} sharedDial={sharedDial}
        dspNr={dspNr} dspNb={dspNb} dspAn={dspAn} onAudio={onAudio} bus={bus} />

    </View>
  );
}

/**
 * ★ SHARED TUNER in the landscape status row (§8.1 / §9): the banner's own words and colours (free /
 * ask), on one line. On the status display (silver / black) it is Doto in the text colour's role; on
 * the default deck it is today's banner font at the row's size.
 * ★★ Row 9 (§8.2): it is never SQUEEZED (dot matrix and segments fall apart below 10 pt) — when the
 *    row is short of room it shortens to `SHARED` (`short`), and after that it is dropped.
 *    The accessibility label keeps the whole sentence either way.
 */
function SharedStatus({ st, size, short = false }: { st: SharedTuner; size: number; short?: boolean }) {
  const dk = useFaceplate().deck;
  const colour = st.alone ? dk.bannerFree : dk.bannerAsk;
  return (
    <View style={{ flexShrink: 1, minWidth: 0 }} accessibilityRole="text" accessibilityLabel={sharedBannerLabel(st)}>
      <SharedText text={short ? SHARED_SHORT : sharedBannerText(st, true)} colour={colour} size={size} />
    </View>
  );
}
const SHARED_SHORT = 'SHARED';
function SharedText({ text, colour, size }: { text: string; colour: string; size: number }) {
  const dk = useFaceplate().deck;
  const sd = React.useContext(StatusDisplayContext);
  return (
    <StatusText keepColor numberOfLines={1}
      style={[lnd.sharedTxt, sd
        ? { color: colour, textShadowColor: sd.glow, textShadowRadius: 4 }
        : { color: colour, fontFamily: dk.bannerFont, fontSize: Math.max(9, size * 1.15), fontWeight: '700' }]}>
      {text}
    </StatusText>
  );
}

/**
 * ★★★ THE LANDSCAPE STATUS ROW (§8.1 / §8.2 / §9) — one line: times left, SHARED TUNER + the DSP
 * badges centre, the link right; on the default deck today's footer, on silver / black the status
 * display. Row 9 makes it FIT rather than squeeze:
 *
 *  ★★ MEASURED, NEVER BY DEVICE MODEL. A measuring twin (StatusMeasure) lays every item out at its
 *     natural width, invisibly and out of the layout; the row's own width comes from its onLayout.
 *     statusFit() (constants/displayText.ts, tested) turns the two into what to hide: first the sides
 *     PACK (so nothing goes where it fits at all), then items drop strictly in STATUS_DROP_ORDER —
 *     IF, GAIN, the phone ⇄ node icons, local time, DSP, rate, SHARED TUNER (→ `SHARED` first), UTC,
 *     the recording timer. The CONNECTION METER is never dropped.
 *  ★ No thrash: hiding an item changes neither the twin (it always holds everything) nor the row's
 *    width (it is stretched by its parent), so a decision cannot feed back into its own inputs. The
 *    twin re-lays out only when its text changes, and onLayout fires only when a width does.
 *  ★ No flapping: bringing an item back needs 8 pt to spare (statusFit's hysteresis), so a rate that
 *    ticks from 9k/s to 10k/s at the edge cannot make IF blink.
 *  ★ Portrait never drops — it does not use this row; it is the full two-line readout.
 */
function LandscapeStatus({ plate, marginTop, clock, font, clockFont, isRecording, recTime, sharedDial,
    dspNr, dspNb, dspAn, onAudio, bus }: {
  plate: PlateTokens | null; marginTop: number;
  clock: { utc: string; srv: string; fromServer: boolean }; font?: string; clockFont: number;
  isRecording?: boolean; recTime?: string; sharedDial?: SharedTuner | null;
  dspNr?: boolean; dspNb?: boolean; dspAn?: boolean; onAudio?: () => void; bus?: MeterBus;
}) {
  const ct = useFaceplate().chassis;
  const s = useUiScale();
  const link = useLinkReadout(bus);
  const dspOn = !!(dspNr || dspNb || dspAn);
  // ★ The reserved slot holds the timer's own shape, so it does not grow when recording starts.
  const recText = isRecording && recTime ? recTime : '0:00:00';
  const sharedFull = sharedDial ? sharedBannerText(sharedDial, true) : null;

  const [measured, setMeasured] = useState<Record<string, number>>({});
  const onUnit = useCallback((id: string, w: number) => {
    // ★ Rounded UP: ten items each rounded down could sum 2 pt short and truncate the last one.
    const v = Math.ceil(w * 2) / 2;
    setMeasured((p) => (p[id] === v ? p : { ...p, [id]: v }));
  }, []);
  const [avail, setAvail] = useState(0);
  const onRowLayout = useCallback((e: LayoutChangeEvent) => {
    // The row's content box: its width less lnd.statusRow's 4 pt padding each side.
    const v = Math.floor(e.nativeEvent.layout.width) - 2 * ROW_PAD;
    setAvail((p) => (p === v ? p : v));
  }, []);

  const w = (id: string) => measured[id] ?? 0;
  const spec: StatusRowSpec = {
    sectionGap: SECTION_GAP,
    sharedShort: sharedDial ? w('sharedShort') : undefined,
    // leads = the gaps the row lays out: pm.clockRow 4, lnd.statusSide 8, lnd.statusCentre 8, pm.linkRow 4
    left:   [{ item: 'utc', width: w('utc'), lead: 0 }, { item: 'localTime', width: w('localTime'), lead: 4 },
             { item: 'rec', width: w('rec'), lead: 8 }],
    centre: [{ item: 'shared', width: sharedDial ? w('shared') : 0, lead: 8 },
             { item: 'dsp', width: dspOn ? w('dsp') : 0, lead: 8 }],
    right:  [{ item: 'linkIcons', width: plate ? 0 : w('linkIconsA'), lead: 4 },
             { item: null, width: w('meter'), lead: 4 },                     // the meter: never dropped
             { item: 'linkIcons', width: sharedDial ? 0 : w('linkIconsB'), lead: 4 },
             { item: 'rate', width: link.showRate ? w('rate') : 0, lead: 4 },
             { item: 'gain', width: link.agcText ? w('gain') : 0, lead: 4 },
             { item: 'if', width: link.ifText ? w('if') : 0, lead: 4 }],
  };
  const stepRef = useRef<number | undefined>(undefined);
  let fit = statusState(0);
  if (avail > 0) {
    fit = statusFit(avail, spec, { prevStep: stepRef.current, hysteresis: s.r(8) });
    stepRef.current = fit.step;
  }
  const hide: Partial<Record<StatusItem, boolean>> = {};
  fit.hidden.forEach((it) => { hide[it] = true; });
  const showShared = !!sharedDial && !hide.shared;
  const showDsp = dspOn && !hide.dsp;
  const side = fit.packed ? lnd.statusSidePacked : lnd.statusSide;

  return (
    <StatusWell plate={plate} gap={0} style={{ marginTop }}>
      <View style={[lnd.statusRow, plate && { marginTop: 0 }]} onLayout={onRowLayout}>
        <View style={side}>
          {(!hide.utc || !hide.localTime) &&
            <ClockRow clock={clock} color={ct.clock} font={font} size={clockFont} hide={hide} />}
          {/* ★ The reserved slot: always laid out, only its opacity changes — nothing resizes. */}
          {!hide.rec && <RecSlot recording={!!isRecording} text={recText} font={font} size={clockFont} />}
        </View>
        {/* ★ Only when there is something to centre — an empty box would add a gap to today's row. */}
        {(showShared || showDsp) && <View style={lnd.statusCentre}>
          {showShared && <SharedStatus st={sharedDial!} size={clockFont} short={fit.sharedShort} />}
          {showDsp && <DspBadges nr={dspNr} nb={dspNb} an={dspAn} onPress={onAudio} font={font} />}
        </View>}
        <View style={[side, { justifyContent: 'flex-end' }]}>
          <LinkIndicator readout={link} hide={hide} noNode={!!sharedDial} />
        </View>
      </View>
      <StatusMeasure onUnit={onUnit} utc={clock.utc} srv={clock.srv} fromServer={clock.fromServer}
        clockColor={ct.clock} font={font} clockFont={clockFont} recText={recText}
        sharedFull={sharedFull} dspNr={!!dspNr} dspNb={!!dspNb} dspAn={!!dspAn}
        link={link} noNode={!!sharedDial} />
    </StatusWell>
  );
}
const ROW_PAD = 4;       // lnd.statusRow paddingHorizontal
const SECTION_GAP = 8;   // lnd.statusRow gap

/** The recording timer's reserved slot (§8.1): laid out whether or not recording, so nothing moves. */
function RecSlot({ recording, text, font, size }: { recording: boolean; text: string; font?: string; size: number }) {
  const ct = useFaceplate().chassis;
  return (
    <View style={[lnd.recRow, !recording && { opacity: 0 }]} pointerEvents="none">
      <View style={[lnd.recDot, { backgroundColor: ct.recRed }]} />
      <StatusText keepColor style={[lnd.recTime, { color: ct.recRed, fontFamily: font, fontSize: size }]}>{text}</StatusText>
    </View>
  );
}

/**
 * ★ Row 9's MEASURING TWIN: every status item at its natural width, invisible, out of the layout
 * (absolute), in the same context — so the same fonts, the same 10 pt floor — and never hidden, so
 * a dropped item's width is still known when there is room for it again. Memoised on the TEXT, so it
 * re-renders (and re-measures) only when something it draws changes.
 */
const StatusMeasure = React.memo(function StatusMeasure({ onUnit, utc, srv, fromServer, clockColor, font,
    clockFont, recText, sharedFull, dspNr, dspNb, dspAn, link, noNode }: {
  onUnit: OnUnit; utc: string; srv: string; fromServer: boolean; clockColor: string; font?: string;
  clockFont: number; recText: string; sharedFull: string | null;
  dspNr: boolean; dspNb: boolean; dspAn: boolean; link: LinkReadout; noNode: boolean;
}) {
  const clock = useMemo(() => ({ utc, srv, fromServer }), [utc, srv, fromServer]);
  return (
    <View style={lnd.statusGhost} pointerEvents="none"
          accessibilityElementsHidden importantForAccessibility="no-hide-descendants">
      <ClockRow clock={clock} color={clockColor} font={font} size={clockFont} onUnit={onUnit} />
      <Unit id="rec" onUnit={onUnit}><RecSlot recording text={recText} font={font} size={clockFont} /></Unit>
      {sharedFull !== null && <Unit id="shared" onUnit={onUnit}><SharedText text={sharedFull} colour={clockColor} size={clockFont} /></Unit>}
      {sharedFull !== null && <Unit id="sharedShort" onUnit={onUnit}><SharedText text={SHARED_SHORT} colour={clockColor} size={clockFont} /></Unit>}
      {(dspNr || dspNb || dspAn) && <Unit id="dsp" onUnit={onUnit}><DspBadges nr={dspNr} nb={dspNb} an={dspAn} font={font} /></Unit>}
      <LinkIndicator readout={link} noNode={noNode} onUnit={onUnit} />
    </View>
  );
});

const lnd = StyleSheet.create({
  /* ★ The bar's own bottom row. `flex: 1` on each side with the badges in the middle keeps the
     audio chain centred regardless of how long the clock or the stats are. */
  statusRow:  { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between',
                gap: 8, paddingHorizontal: 4, marginTop: 3 },
  statusSide: { flex: 1, minWidth: 0, flexShrink: 1, flexDirection: 'row',
                alignItems: 'center', gap: 8 },
  /* ★ Row 9 (§8.2): PACKED — the sides take their content's width (space-between spreads them) when
     the equal halves above would not hold them. Still shrinkable, as the last resort of all.
     ★ A whole style, NOT an override of statusSide: Yoga reads `flex: 1` as basis 0 whenever the
       basis is 'auto', so `{ flex: 1, flexGrow: 0, flexBasis: 'auto' }` is a side of width ZERO. */
  statusSidePacked: { flexShrink: 1, minWidth: 0, flexDirection: 'row', alignItems: 'center', gap: 8 },
  /* The measuring twin: out of the layout, invisible, wide enough that nothing in it ever wraps. */
  statusGhost: { position: 'absolute', left: 0, top: 0, width: 4000, opacity: 0,
                 flexDirection: 'column', alignItems: 'flex-start' },
  /* The centre: SHARED TUNER on a shared dial, then the DSP badges — shrinks before the sides do. */
  statusCentre: { flexShrink: 1, minWidth: 0, flexDirection: 'row', alignItems: 'center', gap: 8 },
  sharedTxt:  { letterSpacing: 1.1 },
  sigFrame: { borderRadius: 7, overflow: 'hidden', justifyContent: 'center', alignSelf: 'stretch' },   // track: ct.meterTrack
  // ★ No flex: the height is KEY_H at the use site (see LandscapeBar). overflow hidden so nothing
  //   inside can push the key taller than its neighbours.
  // ★ The outline, tint and overflow are DomeKey's now; the step legend carries the 4 pt side
  //   padding itself (lsTxt), because a cap's absolute layers must not be inset by it.
  lsKey:    {},
  // ★★ NO FIXED lineHeight HERE — it is set at the use site, SCALED, alongside fontSize.
  // A constant 14 lived here while the font is s.f(11), which scales: on a Mac window (and any
  // iPad wide enough to clamp the scale at 1.45) the text renders at ~16pt inside a 14pt line
  // box, and the tops of the glyphs are sliced off — "500Hz" and "100k" both showed it. The
  // portrait button next door has never had the fault because it sets no lineHeight at all.
  // ★ A length that must track fontSize cannot be a constant when fontSize is not one.
  lsTxt:    { letterSpacing: 0.5, textAlign: 'center', paddingHorizontal: 4 },
  clock:    { letterSpacing: 1, marginTop: 3, textAlign: 'center' },
  recRow:   { flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: 3, marginTop: 1 },
  recDot:   { width: 5, height: 5, borderRadius: 2.5 },   // colour: ct.recRed
  recTime:  { letterSpacing: 1 },
});

// ── Root ──────────────────────────────────────────────────────────────────────

function ControlsBar({
  srvTzOffsetMin = null, srvTzAbbr = '',
  frequency, mode, step, connected, bottomInset,
  signalLevel, peakLevel, snrDb = 40, signalActive, meterBus, signalMode = 'snr',
  fmStereo = false, activeDecoder = null, dabOn = false,
  onVfoDelta, onBwDelta, onMode, onStep,
  onMenu, onChat, onAudio, audioAsRecord = false, onFreqTap, onModeTap,
  // ★ The audio chain's standing state — drawn only when ON, see DspBadges.
  dspNr = false, dspNb = false, dspAn = false,
  instanceHost = 'ubersdr',
  isRecording = false, recSeconds = 0, chatUnread = false,
  freqUnit = 'khz',
  onShare: onShareProp,
  chatShareDisabled = false,
  chatDisabled = false,
  singleDrum = false,
  vfoNoInertia = false,
  stepList,
  meterLabel,
  menuAsBack = false,
  freqFormat,
  vfoKeys = false,
  zoomKeys = false,
  onVfoStep,
  onZoomStep,
  onZoomSweep,
  vfoSweepRate,
  onControlRects, onDrumAnchors,
  /* ★★★ AND THE FIVE THE BARS NEED. They are declared in ControlsBarProps and were arriving from SDRScreen,
   *  but this destructure never took them — so `shared` could not pass them on, and my first attempt at that
   *  fix referenced names that were not in scope, which threw "Property 'readOnly' doesn't exist" and bounced
   *  the app to the server list (Stuart, 2026-09-20). Three layers each keep their own hand-written copy of
   *  the prop list; a name has to appear in ALL of them or it is either dead or fatal. */
  readOnly,
  sharedDial,
  storms,
  adminMode,
  airChannel = null,
  tubeLayout = 'hf',
}: ControlsBarProps) {
  // ★★★ Entering a receiver is where the faceplate is really drawn: on trial from this first render
  //   (constants/faceplate.ts CRASH SAFETY) — a crash in the next few seconds brings the next launch up safe.
  useFaceplateOnTrial();
  // ★ Flashes when a captured region hands the keyboard back — see useRegionHandback.
  const handback = useRegionHandback();
  const handbackFlash = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    if (!handback) return;                       // not on first mount, only on a real handback
    handbackFlash.setValue(1);
    Animated.timing(handbackFlash, { toValue: 0, duration: 450, useNativeDriver: true }).start();
  }, [handback, handbackFlash]);

  const { theme: t } = useTheme();
  const fpTheme = useFaceplate();
  const ct = fpTheme.chassis;
  /* ★★★ TRANSPARENCY EFFECTS OFF (default chassis): the island made opaque — no BlurView, no tint
   *  layer. Silver / black are already an opaque plate; OFF only takes their drop shadow (below). */
  const solidDeck = fpTheme.opaque && !ct.plate;
  const s = useUiScale();

  /* ★★ THE CHANNEL NAME IS THE READOUT ON THE RASTER (airband only — airChannel is null elsewhere). An
   *  8.33 kHz channel's name is not its frequency (118.010 tunes 118.0083 MHz), and the name is what a
   *  pilot, a controller and every frequency list quote, so it is what an aviation radio displays. The
   *  true frequency is not hidden: it rides in `chanTag`, small, above the unit. */
  const chanMain  = !!airChannel && freqUnit === 'mhz' && !freqFormat;
  const freqStr   = useMemo(() => chanMain ? airChannel!.name
    : freqFormat ? freqFormat(frequency) : formatHz(frequency, freqUnit),
    [frequency, freqUnit, freqFormat, chanMain, airChannel]);
  const chanTag   = useMemo(() => {
    if (!airChannel) return null;
    const sp = airChannel.spacing === 833 ? '8.33' : '25 kHz';
    if (!chanMain) return `CH ${airChannel.name} · ${sp}`;
    return airChannel.spacing === 833 ? `8.33 · ${airChannel.trueText}` : sp;
  }, [airChannel, chanMain]);
  const unit      = useMemo(() => freqUnitLabel(freqUnit),       [freqUnit]);
  /* The tubes read the NUMBER. On the airband raster the channel name IS the readout (see chanMain),
   * so the tubes show the name's digits; a custom format (the FM tuner screen) is MHz. */
  const readout   = useMemo<FreqReadout>(() => ({
    hz: chanMain ? Math.round(parseFloat(airChannel!.name) * 1e6) || frequency : frequency,
    unit: freqFormat ? 'mhz' : freqUnit,
    layout: tubeLayout,
  }), [chanMain, airChannel, frequency, freqFormat, freqUnit, tubeLayout]);
  const stepLabel = useMemo(() => formatStep(step),      [step]);
  const snrText   = meterLabel ?? ''; // FM-DX static reading; live text comes from the bus + meterText()
  /* ★★★ THE READOUT'S UNIT, for the mode box AND the LED / analogue scales: the signal readout setting,
   *  or FM-DX's dBf (the tuner screen passes its own label and has no such setting). */
  const meterUnit = meterUnitOf(signalMode, meterLabel !== undefined);
  const clock     = useClock(srvTzOffsetMin, srvTzAbbr);

  const cycleStep = useCallback(() => {
    const list = stepList ?? stepsForFreq(frequency);
    const idx = list.indexOf(step);
    onStep(list[(idx + 1) % list.length] ?? list[0]);
  }, [step, onStep, frequency, stepList]);

  // Parent supplies the deep-link share (instance URL + freq/mode/bw/zoom
  // params — tappable straight into the station); plain text is the fallback
  const handleShare = useCallback(async () => {
    if (onShareProp) { onShareProp(); return; }
    await Share.share({ message: `VibeSDR — ${freqStr} ${unit} ${mode.toUpperCase()} — ${instanceHost}` });
  }, [onShareProp, freqStr, unit, mode, instanceHost]);

  const hh = Math.floor(recSeconds / 3600);
  const mm = Math.floor((recSeconds % 3600) / 60);
  const ss = recSeconds % 60;
  const recTime = `${hh}:${String(mm).padStart(2,'0')}:${String(ss).padStart(2,'0')}`;

  // Bar padding scales with screen
  // ★ A metal plate (silver / black) takes the mockup's padding and 16 pt corners: 14 all round in
  //   portrait; landscape 10 top and bottom, 24 at the sides on silver so the screws clear the drums
  //   (§9), 12 on black. The default island keeps today's numbers.
  const plate   = ct.plate;
  const PAD_H   = !plate ? s.r(12) : s.isLandscape && !IS_TV ? s.r(plate.screws ? 24 : 12) : s.r(14);
  const PAD_TOP = !plate ? s.r(8)  : s.isLandscape && !IS_TV ? s.r(10) : s.r(14);
  const RADIUS  = !plate ? s.r(18) : s.r(plate.radius);

  // ★★ MAX-WIDTH CAP — the Mac app IS the iPad app, so on a Mac window (and
  // especially an ultrawide) the landscape bar's edge-to-edge thumb-reach
  // layout becomes four enormous near-empty buttons spanning the glass. The
  // web client fixes this with no detection at all: content-sized island,
  // capped and centred. Same trick here.
  // ★ Why no platform branch is needed: an iPad tops out at 1366 pt wide in
  // landscape (13" Pro), so a cap ABOVE that can only ever be hit by a Mac
  // window or a genuinely huge display — hand-held iPad ergonomics are
  // untouched by construction. Deliberately NOT pointer detection
  // (isiOSAppOnMac / GCMouse): both need native code, and with a Magic
  // Keyboard the screen is still touchable, so auto-narrowing would be a
  // guess about intent.
  // ★ Bonus of being WINDOW-driven rather than device-driven: an iPad pushed
  // onto an external display gets the same cap for free — no extra code.
  const MAX_BAR_W = 1400;

  /* ★ Broadcast FM, and DAB is not it (DAB borrows the WFM demod internally). See the badge note
   *  in `shared` below. */
  const bcastFm = !dabOn && String(mode).toLowerCase() === 'wfm';
  const shared = {
    freqStr, unit, chanTag, chanMain,
    // §5.1: compose the running decoder onto the demod — USB → USB: RTTY (wefax reads FAX); a standalone
    // OWRX digimode is its own mode (MESHCORE, not "MESHCORE: MESHCORE") — constants/modeBox.ts.
    modeLabel: composeModeLabel(String(mode), activeDecoder, dabOn),
    snrText, fmStereo,
    connected, signalActive, bus: meterBus, meterMode: meterUnit,
    signal: signalLevel, peak: peakLevel,
    stepLabel, onFreqTap, onModeTap,
    onStep: cycleStep, onChat, onMenu, onAudio, audioAsRecord, onShare: handleShare,
    /* ★★★ NOT ON BROADCAST FM — and computed HERE, once, not in each bar. Stuart, 2026-09-25:
     *  "the broadcast FM NB/NR dont need indicators in the control bar, the only ones that need it
     *  are the ones that make a much larger noticeable difference so the MW/HF etc NR/NB/AN."
     *  ★★ A BADGE EARNS ITS PLACE BY BEING SOMETIMES ABSENT. On WFM these treatments are on by
     *  default for everyone and do something subtle, so a permanently-lit badge carries no
     *  information and is noise at the end of a dense row. On MW and HF the same treatments are a
     *  large, audible choice the listener made — that is worth reporting. The earlier fix made the
     *  badges read the RIGHT controls; this one asks whether they should be drawn at all.
     *  ★ Narrow FM keeps them: a weak-signal mode like the rest, not a broadcast one.
     *  ★★★ IN THE PARENT BECAUSE THIS FILE HAS THE SCAR: sharedDial was handled in PortraitBar and
     *  not in its landscape twin, and landscape THREW and bounced the app back to the server list.
     *  One computation, spread to both, cannot drift. */
    dspNr: dspNr && !bcastFm, dspNb: dspNb && !bcastFm, dspAn: dspAn && !bcastFm,
    onVfoDelta, onBwDelta,
    clock, isRecording, recTime, chatUnread,
    csDisabled: chatShareDisabled,
    chatOff: chatShareDisabled || chatDisabled,
    singleDrum, menuAsBack, vfoNoInertia,
    vfoKeys, zoomKeys, onVfoStep, onZoomStep, onZoomSweep, vfoSweepRate, onControlRects, onDrumAnchors,
    /* ★★★ FIVE PROPS THE BARS DESTRUCTURE AND NEVER RECEIVED (Stuart, 2026-09-20: "no shared dial notification
     *  above the frequency"). ControlsBar took them, the bars declared them, and NOTHING carried them across
     *  this object — so `sharedDial` was undefined in both bars and the shared-tuner banner could not draw on
     *  any phone, tablet or Mac. The web client showed it because it has its own code; the app's box has been
     *  dead since the day it was added, and in landscape it did not merely fail, it THREW (see LandscapeBar).
     *  ★★ The same silence covers the bar's session clock, the lightning badge, read-only and admin: the props
     *     exist, are typed `any`, and go nowhere. A prop list written twice is a fact stored twice. */
    readOnly, sharedDial, storms, adminMode,
    /** The plate's padding and corner, for the panels that run to its edge (black's gloss panel). */
    plateInset: { top: PAD_TOP, h: PAD_H, radius: RADIUS },
  };

  return (
    <View style={[
      root.bar,
      {
        paddingTop: PAD_TOP,
        paddingHorizontal: PAD_H,
        paddingBottom: Math.max(bottomInset, s.r(10)),
        borderRadius: RADIUS,
        maxWidth: MAX_BAR_W,
        // ★ Opaque from the first frame on a metal plate — the plate's base colour until the
        //   texture has decoded, never a see-through gap over the waterfall.
        ...(plate ? { backgroundColor: plate.base } : null),
        // ★★★ Transparency OFF: today's tint composited over black, alpha 1.0 EXACTLY, on THIS view
        //   rather than a fill child (one layer, not three). And no drop shadow on ANY chassis: a
        //   12 pt blurred shadow over the live spectrum is exactly the blend OFF exists to remove —
        //   and on the glass island, which has no background of its own, iOS could not even
        //   precompute its path (RN only does for an own background > 0.999 alpha,
        //   RCTViewComponentView "Stage 1. Shadow Path"), so it was drawn per pixel, offscreen.
        ...(solidDeck ? { backgroundColor: ct.deckSolid } : null),
        ...(fpTheme.opaque ? NO_DROP_SHADOW : null),
        alignSelf: 'center',
        width: '100%',
      },
    ]}>
      {/* ★★ THE TINT IS THE SAME ON BOTH PLATFORMS — only BlurView differs, so only BlurView is
          adjusted. On iOS it is a real UIVisualEffect and at 80 it stacked with the tint into a
          near-opaque slab; Android's is far weaker, which is why identical code showed the
          waterfall through the island on the Moto and blanked it on the iPhone.
          ★ ANDROID IS THE TARGET, NOT THE THING TO CHANGE (Stuart, 2026-07-28): "android cannot
          get any clearer… iOS just needs to get to android level". Hence iOS-only. */}
      {plate ? (
        /* ★★ SILVER / BLACK ARE OPAQUE METAL: no BlurView behind them (§3.4) — the blur was the
           expensive part of the glass deck on iOS. The plate is one cached Skia layer. */
        <ChassisPlate plate={plate} radius={RADIUS} />
      ) : solidDeck ? (
        /* Transparency OFF: the root's own background is the tint (above); only the ring stays, at
           its colour over black, so the island's edge reads exactly as it did on a dark band. */
        <View style={[root.border, { borderRadius: RADIUS, borderColor: ct.barBorderSolid }]}
              pointerEvents="none" />
      ) : (<>
        <BlurView intensity={Platform.OS === 'ios' ? 35 : 80} tint="dark" style={StyleSheet.absoluteFill} />
        {/* Tinted overlay — semi-transparent so blur shows; NOT fully opaque */}
        <View style={[StyleSheet.absoluteFill, root.tint, { backgroundColor: ct.deckTint, borderRadius: RADIUS }]}
              pointerEvents="none" />
        {/* Border ring */}
        <View style={[root.border, { borderRadius: RADIUS, borderColor: ct.barBorder }]}
              pointerEvents="none" />
      </>)}
      {/* ★★★ APPLE TV USES THE PORTRAIT CLUSTER, on a 16:9 screen (Stuart, 2026-08-04).
          Not a cosmetic choice — it is the one that matches the remote. Portrait STACKS the
          sections, so the four buttons sit one swipe DOWN from the frequency, on the same
          vertical axis the highlight already travels to reach the servers chip. Landscape FLANKS
          the frequency left and right, which turns a single directional move into horizontal
          hunting. It also puts the clock and status rows where they already belong, so this is a
          row DELETION (see PortraitBar) rather than a hand-built hybrid of the two layouts.
          See briefs/BRIEF-tvos-app.md §2. */}
      <FreqReadoutContext.Provider value={readout}>
      {s.isLandscape && !IS_TV
        ? <LandscapeBar {...shared} />
        : <PortraitBar  {...shared} />
      }
      </FreqReadoutContext.Provider>
    </View>
  );
}

const root = StyleSheet.create({
  bar: {
    overflow: 'hidden',
    shadowColor: '#000',
    shadowOffset: { width: 0, height: -4 },
    shadowOpacity: 0.85,
    shadowRadius: 12,
    elevation: 12,
  },
  // Semi-transparent tint: waterfall colours show through but content is legible
  tint: {
    // colour: ct.deckTint
    inset: 1,              // keeps tint inside the border ring visually             // keeps tint inside the border ring visually
  },
  border: {
    ...StyleSheet.absoluteFill,
    borderWidth: 1,
  },
});

// Memo wall — clock/meters live in internal state or the bus, so screen
// renders with stable props skip this whole subtree.
export default React.memo(ControlsBar);
