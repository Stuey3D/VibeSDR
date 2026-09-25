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
  View, ViewStyle,} from 'react-native';
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
import TunerKeys from './TunerKeys';

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
import { useUiScale } from '../hooks/useUiScale';
import { STEPS, stepsForFreq, type SDRMode } from '../services/sdrTypes';
import { tourRef, mergeRefs } from './Coachmark';
import { IS_TV } from '../utils/tv';

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

// Mode pill label: there's a single CW button (the sideband id cwu/cwl is an
// internal demod detail), so show it as plain "CW" to match the button.
function modeDisplay(mode: string): string {
  const m = mode.toLowerCase();
  return (m === 'cwu' || m === 'cwl' || m === 'cw') ? 'CW' : mode.toUpperCase();
}
function formatStep(s: number): string {
  return s >= 1_000_000 ? s / 1_000_000 + 'M'
       : s >= 1_000     ? s / 1_000 + 'k'
       :                  s + 'Hz';
}

// ── Signal gradient — port of sigGradient() ──────────────────────────────────
function sigGradColors(sig: number): string[] {
  if (sig < 0.20) return ['#bb1100', '#ff4400'];
  if (sig < 0.58) return ['#bb1100', '#ff4400', '#ffaa00'];
  return ['#bb1100', '#ff4400', '#ffaa00', '#00dd44'];
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
  /** Peak power in the passband, dBFS — feeds the S-meter / dBFS readouts. */
  dbfs: number;
  active: boolean; link: 0|1|2|3;
  /** Squelch threshold as a bar-normalised position (0..1), in the SAME scale the bar draws, so the
   *  red squelch line sits on the shown meter. -1 = squelch off / not applicable (no line, no SQL). */
  sql?: number;
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

// Real S-meter from passband dBFS (classic 6dB/S-unit, S9 ≈ −73) — replaces
// the old synthetic conversion built on upstream's broken +30dB SNR offset.
function dbfsToSMeter(dbfs: number): string {
  if (dbfs >= -73) {
    const over = Math.round(dbfs + 73);
    return over > 0 ? `S9+${over}` : 'S9';
  }
  const s = Math.max(1, 9 - Math.ceil((-73 - dbfs) / 6));
  return `S${s}`;
}

/** Exported so the WATCH can mirror the phone's meter verbatim rather than picking
 *  its own metric. It used to render SNR specifically — which OWRX, Kiwi and FM-DX
 *  do not have (they send an absolute S-meter / dBf and no noise reference), so the
 *  wrist showed a permanent "—" on those backends while the bar moved fine beneath
 *  it. Sending the TEXT means the watch can never disagree with the phone, and a
 *  future backend with some other metric works for free. Same reasoning as shipping
 *  the palette as a LUT instead of reimplementing the colour maps in Swift. */
export function meterText(mode: 'snr' | 'smeter' | 'dbfs', m: MeterValues): string {
  if (mode === 'smeter') return dbfsToSMeter(m.dbfs);
  if (mode === 'dbfs')   return `${Math.round(m.dbfs)}dB`;
  return isFinite(m.snr) ? `${Math.round(m.snr)}db` : '';
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
 *      zone really was Etc/UTC). The symbol does not have to be learnt here: it is the SAME rack
 *      glyph already sitting in the stats row of this very bar, next to the phone glyph, where it
 *      has always meant "the server end".
 *  ★ Falls back to the phone's clock when the server has not said (an older build), so the row is
 *    never blank — but it is then labelled with the PHONE's zone, which is the honest reading. */
function useClock(tzOffsetMin?: number | null, tzAbbr?: string) {
  const [now, setNow] = useState(() => new Date());
  useEffect(() => {
    // Background audio keeps JS alive when locked — don't re-render the
    // controls every second behind a screen nobody can see.
    const id = setInterval(() => {
      if (AppState.currentState === 'active') setNow(new Date());
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

/** The clock row: UTC, then the RECEIVER's wall clock behind the rack glyph that means "server end"
 *  everywhere else in this bar. */
function ClockRow({ clock, color, font, size }:
    { clock: { utc: string; srv: string; fromServer: boolean }; color: string; font?: string; size: number }) {
  return (
    <View style={pm.clockRow}>
      <Text numberOfLines={1} style={{ color, fontFamily: font, fontSize: size }}>{clock.utc}</Text>
      <Text numberOfLines={1} style={{ color, fontFamily: font, fontSize: size, opacity: 0.6 }}>·</Text>
      {clock.fromServer ? <ServerGlyph color={color} /> : null}
      <Text numberOfLines={1} style={{ color, fontFamily: font, fontSize: size }}>{clock.srv}</Text>
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
}

// ── Signal bar canvas ─────────────────────────────────────────────────────────

function SignalCanvas({ width, height, signal: sigProp = 0, peak: peakProp = 0, bus }:
  { width: number; height: number; signal?: number; peak?: number; bus?: MeterBus }) {
  const m = useMeters(bus);
  const signal = m ? m.level : sigProp;
  const peak   = m ? m.peak  : peakProp;

  // Direct rendering at the real data rate (~10Hz) — interpolation removed by
  // request; updates cost only this small canvas re-render via the meter bus.
  if (width < 4) return null;
  const fillW  = width * Math.min(1, Math.max(0, signal));
  const peakX  = width * Math.min(1, Math.max(0, peak));
  const colors = signal > 0.001 ? sigGradColors(signal) : [];
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
      <Rect x={0} y={0} width={width} height={height} color="rgba(105,98,82,0.30)" />
      {fillW > 1 && colors.length > 0 && (
        <Rect x={0} y={0} width={fillW} height={height} opacity={sqlClosed ? 0.55 : 1}>
          <LinearGradient start={vec(0,0)} end={vec(fillW,0)} colors={colors} positions={pos} />
        </Rect>
      )}
      {peakX > 2 && (
        <Rect x={peakX - 1} y={0} width={2} height={height} color="rgba(255,245,200,0.92)" />
      )}
      {sqlOn && (
        <>
          {/* White halo so the red squelch line stays visible over any fill colour. */}
          <Rect x={sqlX - 2} y={0} width={4} height={height} color="rgba(255,255,255,0.90)" />
          <Rect x={sqlX - 1} y={0} width={2} height={height} color="rgba(255,50,50,1)" />
        </>
      )}
    </Canvas>
  );
}

// ── Link quality bars (replaces the old connection dot) ───────────────────────
// Mobile-signal style: 3 green = solid link, 2 yellow = jitter/some drops,
// 1 red = stalling/reconnecting, all dim = disconnected.
function LinkBars({ q }: { q: 0 | 1 | 2 | 3 }) {
  // Disconnected (q=0) → a clear red ✕ rather than ambiguous dim bars.
  if (q === 0) {
    return (
      <View style={pm.linkWrap}>
        <Text style={{ color: '#e04040', fontSize: 14, fontWeight: '900', lineHeight: 14 }}>✕</Text>
      </View>
    );
  }
  const litColor = q === 3 ? '#33cc44' : q === 2 ? '#e0b020' : '#e04040';
  return (
    <View style={pm.linkWrap}>
      {[0, 1, 2].map(i => (
        <View key={i} style={[pm.linkBar, {
          height: 4 + i * 3,
          backgroundColor: i < q ? litColor : 'rgba(255,255,255,0.18)',
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
function ServerGlyph({ color }: { color: string }) {
  return (
    <View style={[pm.serverGlyph, { borderColor: color }]}>
      <View style={[pm.serverLine, { backgroundColor: color }]} />
      <View style={[pm.serverLine, { backgroundColor: color }]} />
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
  const on: string[] = [];
  if (nr) on.push('NR');
  if (nb) on.push('NB');
  if (an) on.push('AN');
  if (!on.length) return null;                 // ★ the ordinary case costs nothing
  const body = (
    <View style={pm.dspRow}>
      {on.map((k) => (
        <Text key={k} style={[pm.dspTag, { fontFamily: font, color: color ?? '#ffb833' }]}>{k}</Text>
      ))}
    </View>
  );
  return onPress
    ? <TouchableOpacity onPress={onPress} activeOpacity={0.7} hitSlop={8}>{body}</TouchableOpacity>
    : body;
}

export function LinkIndicator({ bus }: { bus?: MeterBus }) {
  const m = useMeters(bus);
  const q = m ? m.link : 0;
  // ★ LATCH, don't gate on the live value. Requiring fps > 0 to show the readout
  // meant a single second with no counted frames BLANKED it — so on a backend
  // whose frames arrive unevenly (Kiwi) it flashed on and off once a second.
  // ★★ And hiding is the wrong response anyway: a stall is precisely when
  // "0k/s · 0fps" is worth seeing. Blanking turns the most informative moment
  // into no information at all. Show it once a rate has ever arrived, then keep
  // showing it — zeros included.
  const everHadRate = useRef(false);
  if ((m?.fps ?? 0) > 0 || (m?.kbps ?? 0) > 0) everHadRate.current = true;
  if (q === 0) everHadRate.current = false;      // disconnected — start clean again
  const dim = 'rgba(255,255,255,0.40)';
  // Incoming rate readout — spectrum KB/s (the phone's audio is decoded natively, so JS can't see
  // its bytes) + frame rate, the same "what's actually arriving" cue the web client shows. Only once
  // a link exists, so a disconnected meter stays clean.
  const showRate = !!m && q > 0 && everHadRate.current;
  const rateTxt  = showRate ? `${Math.round(m!.kbps ?? 0)}k/s · ${Math.round(m!.fps ?? 0)}fps` : '';
  return (
    // ★ collapsable={false} so the tour can measure it — a plain View can be flattened
    //   away by RN and then measureInWindow has nothing to report. Same as the other
    //   tour targets.
    <View ref={tourRef('linkMeter')} collapsable={false} style={pm.linkRow}>
      <PhoneGlyph color={dim} />
      <Text style={pm.linkArrows}>⇄</Text>
      <LinkBars q={q} />
      <Text style={pm.linkArrows}>⇄</Text>
      {/* The network-NODE triangle — the same server mark used everywhere else (menu, watch), not
          the old server-rack box. */}
      <SectionIcon name="instance" size={13} color={dim} />
      {showRate ? <Text style={pm.linkRate}>{rateTxt}</Text> : null}
      {/* ★ After the rate, because it changes rarely — a value that moves once a minute beside one
             that moves every second reads as part of the same reading if it comes first. */}
      {m?.agcText ? <Text style={pm.linkRate}>{`· ${m.agcText}`}</Text> : null}
      {/* ★ Last: it moves least of all — it changes only when somebody zooms. */}
      {m?.ifText ? <Text style={pm.linkRate}>{`· ${m.ifText}`}</Text> : null}
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

function FreqModePill({ freqStr, unit, modeLabel, snrText, connected, signalActive,
  onFreqTap, onModeTap, freqFontSize, freqWidth, unitFontSize, modeFontSize,
  modeLs, snrWidth, pillPadH, pillPadV, modePadH, modePadV, gap, bus, meterMode,
  tight = false, fmStereo = false, wide = false, sharedTuner = null,
}: any) {
  const { theme: t } = useTheme();
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
  // Skin parity (lsvSnrDisp): plain "NNdb", not a synthetic S-meter reading.
  const m = useMeters(bus);
  // An explicit snrText (FM-DX "28 dBf") wins over the bus-computed text.
  const liveSnrText = snrText ? snrText : (m ? meterText(meterMode ?? 'snr', m) : '');
  const liveActive  = m ? m.active : signalActive;
  // Squelch: when the live signal is BELOW the threshold the gate is closed (muting NOW) — the
  // readout flips to a breathing red "SQL" (no extra screen space), and the bar dims (SignalCanvas).
  const sqlNorm   = m ? (m.sql ?? -1) : -1;
  const sqlClosed = sqlNorm >= 0 && (m?.gate ?? ((m ? m.level : 0) < sqlNorm));
  const breathe   = useRef(new Animated.Value(1)).current;
  useEffect(() => {
    if (!sqlClosed) { breathe.setValue(1); return; }
    const loop = Animated.loop(Animated.sequence([
      Animated.timing(breathe, { toValue: 0.3, duration: 650, useNativeDriver: true }),
      Animated.timing(breathe, { toValue: 1.0, duration: 650, useNativeDriver: true }),
    ]));
    loop.start();
    return () => loop.stop();
  }, [sqlClosed, breathe]);
  return (
    // maxWidth cap: the pill must NEVER swallow the signal bar — on narrow
    // screens (SE / Moto G35) and with Android font metrics the fixed dp
    // widths overflow the frame; the freq text's adjustsFontSizeToFit
    // absorbs the squeeze (meter stays visible ≥13% each side).
    <View style={{ maxWidth: tight ? '66%' : '74%', alignSelf: 'center', alignItems: 'stretch' }}>
    {sharedTuner && (
      /* ★★ CONTEXT-AWARE (noobish via Stuart, 2026-09-19): alone, you may just tune; with company, ask — and
       *    the room's count lives HERE, where the question is asked, not in a corner badge. */
      <View style={[pm.sharedBox, { backgroundColor: t.pillBg }]}
            accessibilityRole="text"
            accessibilityLabel={sharedTuner.alone ? 'Shared tuner. Nobody else is listening — free to tune.'
              : sharedTuner.youPlus
                ? `Shared tuner. You and ${Math.max(1, sharedTuner.listeners - 1)} other${sharedTuner.listeners - 1 === 1 ? '' : 's'} listening — ask before tuning.`
                : `Shared tuner. ${sharedTuner.listeners}${sharedTuner.max > 1 ? ` of ${sharedTuner.max}` : ''} listening — ask before tuning.`}>
        <Text style={[pm.sharedTxt, { fontFamily: t.font, fontSize: sharedFontSize,
                      color: sharedTuner.alone ? '#7bd88f' : t.snrColor }]}
              numberOfLines={1} adjustsFontSizeToFit minimumFontScale={0.7}>
          {sharedTuner.alone ? 'SHARED TUNER · FREE TO TUNE'
            : sharedTuner.youPlus
              /* ★★★ "You+N": the count includes us, so N = total − 1 and nobody has to work out
               *  whether they are in it. No "/max" — FM-DX has no cap to report. */
              //  ★ On the narrowest layouts (SE in Display Zoom) the prefix goes, never the words
              //    that matter — truncating mid-word is the one outcome that is not allowed.
              ? `${tight ? '' : 'Shared Tuner - '}Ask Before Tuning (You+${Math.max(1, sharedTuner.listeners - 1)})`
              : `SHARED TUNER · ASK TO TUNE · ${sharedTuner.listeners}${sharedTuner.max > 1 ? `/${sharedTuner.max}` : ''} 👤`}
        </Text>
      </View>
    )}
    <View style={pm.row}>
      <TouchableOpacity
        ref={tourRef('freqBox')}
        style={[pm.freqBox, { backgroundColor: t.pillBg, paddingHorizontal: pillPadH, paddingVertical: pillPadV, gap }]}
        onPress={onFreqTap} activeOpacity={0.80} hitSlop={8}
      >
        <Text style={[pm.freq, {
          color: t.freqColor, fontSize: freqFontSize, width: freqWidth,
          fontFamily: t.font, textShadowColor: t.freqGlowColor,
          // Tight line metrics — Atkinson's tall default line-height (and
          // Android's extra font padding) inflated the pill to fill the
          // whole meter frame, hiding the signal ring around it
          lineHeight: Math.round(freqFontSize * 1.12),
          includeFontPadding: false,
        }]} numberOfLines={1} adjustsFontSizeToFit>
          {freqStr}
        </Text>
        <Text style={[pm.unit, { color: t.unitColor, fontFamily: t.font, fontSize: unitFontSize }]}>
          {unit}
        </Text>
      </TouchableOpacity>
      <TouchableOpacity
        ref={tourRef('modeBtn')}
        style={[pm.modeBtn, { backgroundColor: t.pillBg, paddingHorizontal: modePadH, paddingVertical: modePadV, minWidth: tight ? 72 : 84 }]}
        onPress={onModeTap} activeOpacity={0.80} hitSlop={8}
      >
        <View style={{ flexDirection: 'row', alignItems: 'center', justifyContent: 'center' }}>
          <Text style={[pm.modeLbl, {
            color: t.modeColor, fontSize: modeFontSize, letterSpacing: modeLs, fontFamily: t.font,
            lineHeight: Math.round(modeFontSize * 1.15), includeFontPadding: false,
          }]}>
            {modeLabel}
          </Text>
          {/* WFM stereo: V5's pilot-PLL lock (+ blend) is reliable, so the icon
              is back — shows the interlocking-rings symbol when stereo is active. */}
          {fmStereo && <StereoIcon size={Math.round(modeFontSize * 0.95)} color={t.modeColor} />}
        </View>
        {sqlClosed ? (
          <Animated.Text style={[pm.snr, {
            color: '#ff4040', fontFamily: t.font, width: snrWidth,
            fontSize: Math.max(9, Math.round(modeFontSize * 0.75)),
            lineHeight: Math.round(Math.max(9, modeFontSize * 0.75) * 1.15),
            includeFontPadding: false, fontWeight: '800', opacity: breathe,
          }]}>
            SQL
          </Animated.Text>
        ) : (
          <Text style={[pm.snr, {
            color: t.snrColor, fontFamily: t.font, width: snrWidth,
            fontSize: Math.max(9, Math.round(modeFontSize * 0.75)),
            lineHeight: Math.round(Math.max(9, modeFontSize * 0.75) * 1.15),
            includeFontPadding: false,
            fontWeight: '700',
            opacity: liveActive ? 1.0 : 0.65,
          }]}>
            {liveSnrText}
          </Text>
        )}
      </TouchableOpacity>
    </View>
    </View>
  );
}

const pm = StyleSheet.create({
  row:      { flexDirection: 'row', alignItems: 'stretch', justifyContent: 'center' },
  sharedBox:{ borderRadius: 5, paddingHorizontal: 8, paddingVertical: 2, marginBottom: 3, alignItems: 'center',
              borderWidth: 1, borderColor: 'rgba(255,255,255,0.30)',
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
  linkArrows: { color: 'rgba(255,255,255,0.40)', fontSize: 9, lineHeight: 11 },
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
                borderWidth: 1, borderColor: 'rgba(255,184,51,0.55)',
                backgroundColor: 'rgba(255,184,51,0.16)' },
  linkRate:   { color: 'rgba(255,255,255,0.55)', fontSize: 9, lineHeight: 11, marginLeft: 4, fontVariant: ['tabular-nums'] },
  phoneGlyph: { width: 8, height: 13, borderWidth: 1, borderRadius: 2,
                alignItems: 'center', justifyContent: 'flex-end', paddingBottom: 1.5 },
  phoneDot:   { width: 2.5, height: 1.5, borderRadius: 1 },
  clockRow:   { flexDirection: 'row', alignItems: 'center', gap: 4, flexShrink: 1, minWidth: 0 },
  serverGlyph:{ width: 13, height: 11, borderWidth: 1, borderRadius: 2,
                justifyContent: 'space-evenly', paddingHorizontal: 2 },
  serverLine: { height: 1, borderRadius: 0.5 },
  dot:     { width: 7, height: 7, borderRadius: 3.5, marginRight: 5, alignSelf: 'center', flexShrink: 0 },
  dotOn:   { backgroundColor: '#00cc44' },
  dotOff:  { backgroundColor: '#333' },
  freqBox: { flexDirection: 'row', alignItems: 'flex-end', borderTopLeftRadius: 5, borderBottomLeftRadius: 5, flexShrink: 1 },
  freq:    { letterSpacing: 1.5, textAlign: 'center', textShadowOffset: { width: 0, height: 0 }, textShadowRadius: 6, flexShrink: 1 },
  unit:    { letterSpacing: 1, alignSelf: 'flex-end', paddingBottom: 2, flexShrink: 0 },
  modeBtn: { borderTopRightRadius: 5, borderBottomRightRadius: 5,
             borderLeftWidth: 1, borderLeftColor: 'rgba(70,60,45,0.45)',
             alignItems: 'center', justifyContent: 'center', gap: 1, flexShrink: 0 },
  modeLbl: { fontWeight: 'bold', textShadowColor: 'rgba(255,160,0,0.6)',
             textShadowOffset: { width:0,height:0 }, textShadowRadius: 5 },
  snr:     { fontSize: 9, textAlign: 'center' },
});

// ── Cog icon ──────────────────────────────────────────────────────────────────
// Replaces the hamburger on the menu button. Once the ServersChip owns "leaving",
// this button is honestly just settings — and a cog says so, where the hamburger
// read as "exit" and hid the way back to the instance list. Colour matches the
// row's other glyphs (t.btnText), not a hard white, so both themes stay coherent.
// Authored in a 24×24 space (Feather "settings"); scale to the canvas.
const COG_GEAR   = Skia.Path.MakeFromSVGString('M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 1 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 8 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 1 1-2.83-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H2a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 3.6 8a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 1 1 2.83-2.83l.06.06a1.65 1.65 0 0 0 1.82.33H8a1.65 1.65 0 0 0 1-1.51V2a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 1 1 2.83 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H22a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z')!;
const COG_CENTER = Skia.Path.MakeFromSVGString('M15 12a3 3 0 1 1-6 0 3 3 0 0 1 6 0z')!;

function Cog({ size, color }: { size: number; color: string }) {
  const k = size / 24;
  return (
    <Canvas pointerEvents="none" style={{ width: size, height: size }}>
      <Group transform={[{ scale: k }]}>
        <Path path={COG_GEAR}   color={color} strokeWidth={1.7 / k} style="stroke" strokeCap="round" strokeJoin="round" />
        <Path path={COG_CENTER} color={color} strokeWidth={1.7 / k} style="stroke" strokeCap="round" strokeJoin="round" />
      </Group>
    </Canvas>
  );
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

function ChatIcon({ size, color }: { size: number; color: string }) {
  const k = size / 20;
  return (
    <Canvas pointerEvents="none" style={{ width: size, height: size }}>
      <Group transform={[{ scale: k }]}>
        <Path path={CHAT_PATH} color={color} strokeWidth={1.6 / k} style="stroke" strokeCap="round" strokeJoin="round" />
      </Group>
    </Canvas>
  );
}

function AudioIcon({ size, color }: { size: number; color: string }) {
  const k = size / 20;
  return (
    <Canvas pointerEvents="none" style={{ width: size, height: size }}>
      <Group transform={[{ scale: k }]}>
        {/* Speaker cone (filled) + two sound-wave arcs */}
        <Path path={SPEAKER_BODY} color={color} style="fill" />
        <Path path={SPEAKER_W1}   color={color} strokeWidth={1.6 / k} style="stroke" strokeCap="round" />
        <Path path={SPEAKER_W2}   color={color} strokeWidth={1.6 / k} style="stroke" strokeCap="round" />
      </Group>
    </Canvas>
  );
}

// FM-DX audio button = REC panel; a filled record disc reads clearer than a speaker.
function RecordIcon({ size, color }: { size: number; color: string }) {
  const k = size / 20;
  return (
    <Canvas pointerEvents="none" style={{ width: size, height: size }}>
      <Group transform={[{ scale: k }]}>
        <Path path={RECORD_RING} color={color} strokeWidth={1.6 / k} style="stroke" />
        <Path path={RECORD_DOT}  color="#e23b3b" style="fill" />
      </Group>
    </Canvas>
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

function PortraitBar({ freqStr, unit, modeLabel, snrText, connected, signalActive, bus, meterMode, fmStereo = false,
  signal, peak, stepLabel, onFreqTap, onModeTap, onStep, onChat, onMenu, onAudio, audioAsRecord,
  dspNr, dspNb, dspAn,
  onVfoDelta, onBwDelta, clock, isRecording, recTime, chatUnread, csDisabled, chatOff, singleDrum, menuAsBack, vfoNoInertia,
  readOnly, sharedDial, storms, adminMode, vfoKeys, zoomKeys, onVfoStep, onZoomStep, onZoomSweep, vfoSweepRate,
  onControlRects }: any) {
  const handbackFlash = useHandbackFlash();

  const { theme: t } = useTheme();
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
  const SIG_H      = s.r(s.isTablet ? 62 : 40); // taller on tablet so the meter shows above/below the tall two-line pill
  const DRUM_H     = s.r(60);
  const ROW_GAP    = s.r(7);
  const COL_GAP    = s.r(8);
  const BAR_PAD_H  = s.r(12);
  const BTN_H      = s.r(44); // a11y minimum touch target (was 36 — misses)
  const ICON_SZ    = s.r(20);
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

      {/* Row 1 — signal bar */}
      <View style={[por.sigFrame, { height: SIG_H }]}
            onLayout={(e: any) => setSigW(e.nativeEvent.layout.width)}>
        <SignalCanvas width={sigW} height={SIG_H} signal={signal} peak={peak} bus={bus} />
        <FreqModePill
          freqStr={freqStr} unit={unit} modeLabel={modeLabel} snrText={snrText}
          connected={connected} signalActive={signalActive} bus={bus} meterMode={meterMode} fmStereo={fmStereo}
          onFreqTap={onFreqTap} onModeTap={onModeTap}
          freqFontSize={FREQ_FONT} freqWidth={FREQ_W} unitFontSize={UNIT_FONT}
          modeFontSize={MODE_FONT} modeLs={MODE_LS} snrWidth={SNR_W}
          pillPadH={PILL_PAD_H} pillPadV={PILL_PAD_V}
          modePadH={MODE_PAD_H} modePadV={MODE_PAD_V} gap={PILL_GAP}
          tight={tight} sharedTuner={sharedDial ?? null}
        />
      </View>

      {/* Row 2 — 4 equal buttons */}
      <View style={{ flexDirection: 'row', gap: COL_GAP }}>

        {/* STEP */}
        <TouchableOpacity
          ref={tourRef('stepBtn')}
          style={[por.btn, { minHeight: BTN_H, borderColor: t.btnBorder }]}
          onPress={onStep} activeOpacity={0.75} hitSlop={10}
        >
          <Text style={[por.btnTxt, { color: t.btnText, fontFamily: t.font, fontSize: BTN_FONT }]}>
            {stepLabel}
          </Text>
        </TouchableOpacity>

        {/* AUDIO — opens the audio sheet; breathes red↔white while recording
            (REC lives inside the sheet, so this is the tap target to stop it). */}
        <View style={[por.btn, { minHeight: BTN_H, borderColor: t.btnBorder, borderWidth: 1 }]}>
          <Animated.View pointerEvents="none"
            style={[StyleSheet.absoluteFill, { borderRadius: 4, borderWidth: 1, borderColor: 'rgba(255,60,60,1)', opacity: recPulse }]} />
          <TouchableOpacity
            style={{ flex: 1, alignSelf: 'stretch', alignItems: 'center', justifyContent: 'center' }}
            onPress={onAudio} activeOpacity={0.75} hitSlop={10}
          >
            {audioAsRecord
              ? <RecordIcon size={ICON_SZ} color={t.btnText} />
              : <AudioIcon size={ICON_SZ} color={t.btnText} />}
          </TouchableOpacity>
        </View>

        {/* MENU */}
        <TouchableOpacity
          ref={tourRef('menuBtn')}
          style={[por.btn, { minHeight: BTN_H, borderColor: t.btnBorder, borderWidth: 1 }]}
          onPress={onMenu} activeOpacity={0.75} hitSlop={10}
        >
          {menuAsBack
            ? <Text style={{ color: t.btnText, fontFamily: t.font, fontSize: s.f(t.btnSize) }}>‹ Back</Text>
            : <Cog size={ICON_SZ} color={t.btnText} />}
        </TouchableOpacity>

        {/* CHAT */}
        <View style={[por.btn, { minHeight: BTN_H, borderColor: t.btnBorder, borderWidth: 1, opacity: chatOff ? 0.4 : 1 }]}>
          <Animated.View pointerEvents="none"
            style={[StyleSheet.absoluteFill, { borderRadius: 4, borderWidth: 1, borderColor: 'rgba(100,180,255,1)', opacity: chatPulse }]} />
          <TouchableOpacity
            style={{ flex: 1, alignSelf: 'stretch', alignItems: 'center', justifyContent: 'center' }}
            onPress={chatOff ? undefined : onChat} disabled={chatOff} activeOpacity={0.75} hitSlop={10}
          >
            {/* decorative — don't let the Skia view contest the touch */}
            <ChatIcon size={ICON_SZ} color={t.btnText} />
          </TouchableOpacity>
        </View>

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
            backgroundColor: 'rgba(124,255,155,0.10)',
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

      {/* Row 4 — clock · link quality · rec */}
      <View style={por.clockRow}>
        {/* ★★ minWidth 0 + shrink, OR THE CLOCK RUNS UNDER THE LINK ICONS. A row child's default
            minWidth is its content, so `flex: 1` alone does not let this group get smaller than the
            clock plus whatever else is in it — it overflowed instead, and on the phone the time was
            printed straight through the icons and the rate ("the clock is clipping the status
            icons", Stuart, 2026-09-20). Shrinking is what should give when the row is tight. */}
        <View style={{ flex: 1, minWidth: 0, flexShrink: 1, flexDirection: 'row', alignItems: 'center', gap: 8 }}>
          <ClockRow clock={clock} color={t.clockColor} font={t.font} size={CLOCK_FONT} />
          {/* Time-limited receiver: how long before the server drops us. */}
          {adminMode ? (
            <Text style={{ color: t.clockColor, fontFamily: t.font, fontSize: CLOCK_FONT,
                           opacity: 0.9 }}
                  accessibilityLabel="Admin mode — this session is not time limited">
              ⚿ Admin Mode
            </Text>
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
            <Text style={{ color: t.clockColor, fontFamily: t.font, fontSize: CLOCK_FONT, opacity: 0.9 }}
                  numberOfLines={1}>
              {sharedDial.tuning}
            </Text>
          )}
          {!!storms && (
            <Text style={{ color: '#9fd0ff', fontFamily: t.font, fontSize: CLOCK_FONT, opacity: 0.9, letterSpacing: 1 }}
                  numberOfLines={1}
                  accessibilityLabel={`Lightning nearby — the broadband lines across the spectrum are sferics, not a fault (about ${Math.round(storms.rate)} a minute`
                    + (storms.ago >= 0 && storms.ago < 90 ? `, last ${Math.round(storms.ago)} seconds ago)` : ')')}>
              ⚡ STORMS
            </Text>
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
            <View style={por.recDot} />
            <Text style={[por.recTime, { fontFamily: t.font, fontSize: CLOCK_FONT }]}>{recTime}</Text>
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

    </View>
  );
}

const por = StyleSheet.create({
  sigFrame: { borderRadius: 7, overflow: 'hidden', backgroundColor: 'rgba(105,98,82,0.30)', justifyContent: 'center' },
  btn:      { flex: 1, backgroundColor: 'rgba(20,10,0,0.75)', borderWidth: 1, borderRadius: 4, alignItems: 'center', justifyContent: 'center' },
  btnTxt:   { letterSpacing: 0.5, textAlign: 'center' },
  clockRow: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center', paddingHorizontal: 2 },
  /* ★ Its own line, centred like landscape's. The stats are the widest thing in the bar and the
     only one that was being cut off; given a row to themselves they simply fit. */
  statsRow: { flexDirection: 'row', justifyContent: 'center', alignItems: 'center',
              paddingHorizontal: 2, marginTop: 1 },
  clock:    { letterSpacing: 1 },
  recRow:   { flexDirection: 'row', alignItems: 'center', gap: 4 },
  recDot:   { width: 6, height: 6, borderRadius: 3, backgroundColor: '#e05050' },
  recTime:  { letterSpacing: 1, color: '#e05050' },
});

// ── LANDSCAPE ─────────────────────────────────────────────────────────────────

function LandscapeBar({ freqStr, unit, modeLabel, snrText, connected, signalActive, bus, meterMode, fmStereo = false,
  signal, peak, stepLabel, onFreqTap, onModeTap, onStep, onChat, onMenu, onAudio, audioAsRecord,
  dspNr, dspNb, dspAn,
  onVfoDelta, onBwDelta, clock, isRecording, recTime, chatUnread, chatOff, singleDrum, menuAsBack, vfoNoInertia,
  /* ★★★ sharedDial WAS MISSING FROM THIS LIST AND USED IN THE BODY. The props arrive as {...shared}, so the
   *  name simply was not in scope and the landscape bar threw "Property 'sharedDial' doesn't exist" the moment
   *  it rendered — the whole app bounced back to the server list (Stuart, 2026-09-20). PortraitBar destructures
   *  it and worked; its twin did not, which is why it survived review: the same JSX, one bar broken.
   *  ★★ A destructured prop list is a hand-maintained copy of the props — anything the body uses must be in it. */
  sharedDial,
  vfoKeys, zoomKeys, onVfoStep, onZoomStep, onZoomSweep, vfoSweepRate }: any) {
  const handbackFlash = useHandbackFlash();

  const { theme: t } = useTheme();
  const s = useUiScale();
  const [sigW, setSigW] = useState(0);

  const DRUM_H    = s.r(44);   // landscape drum height from skin BASE_LSV_DH=44
  // Tablet: the two-line pill (mode + SNR) is tall enough to fill a 40dp frame,
  // hiding the meter fill above/below the freq box — give it more height so the
  // meter shows top and bottom like it does on phones.
  const SIG_H     = s.r(s.isTablet ? 62 : 40);  // was 48 — frame dwarfed the small pill
  const GAP       = s.r(6);
  const BTN_W     = s.r(56);
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
  const ICON_SZ   = s.r(18);
  const CLOCK_FONT = s.f(7);

  return (
    /* ★ A COLUMN NOW: the controls in one row, the status in another beneath it. This function's
       root used to BE the drum row, which is why the clock and the stats had to be tucked inside
       the drum columns — there was nowhere else for them to go. */
    <View>
    <View ref={drumRowRef} onLayout={guardDrums} style={{ flexDirection: 'row', alignItems: 'stretch', justifyContent: 'center', gap: GAP }}>

      {/* ★ Handback flash — see useHandbackFlash. The landscape bar has its own drum row, so
          without this the announcement simply vanished on rotation. */}
      <Animated.View pointerEvents="none"
        style={{
          position: 'absolute', left: -4, right: -4, top: -4, bottom: -4,
          borderRadius: 12, borderWidth: 2, borderColor: NAV_FOCUS,
          backgroundColor: 'rgba(124,255,155,0.10)',
          opacity: handbackFlash, zIndex: 3,
        }} />

      {/* VFO drum + clock */}
      <View ref={tourRef('vfoDrum')} style={{ flex: 1, minWidth: s.r(80) }}>
        {vfoKeys
          ? <TunerKeys type="vfo" height={DRUM_H} onStep={onVfoStep ?? noStep} sweepRate={vfoSweepRate} style={{ flex: 1 }} />
          : <DrumWheel type="vfo" height={DRUM_H} onDelta={onVfoDelta} style={{ flex: 1 }} noInertia={vfoNoInertia} />}

        {/* ★★ THE SLOT IS ALWAYS THERE, EMPTY OR NOT — and that is the whole point of putting it
            back deliberately rather than just reverting. The recording row used to APPEAR, which
            grew this column and resized the tuning keys under the user's thumb mid-gesture
            (Stuart: "the controls dont have to grow and shrink when recording is happening"). A
            reserved row keeps the timer beside the dial where he wants it AND keeps the keys
            still: the height is identical whether it is recording or not. */}
      </View>

      {/* STEP + MENU column */}
      <View style={{ width: BTN_W, gap: GAP }}>
        <TouchableOpacity ref={tourRef('stepBtn')} style={[lnd.lsBtn, { borderColor: t.btnBorder }]} onPress={onStep} activeOpacity={0.75} hitSlop={10}>
          <Text style={[lnd.lsTxt, { color: t.btnText, fontFamily: t.font,
                                     fontSize: s.f(11), lineHeight: s.f(14) }]}
                numberOfLines={2} adjustsFontSizeToFit>
            {stepLabel}
          </Text>
        </TouchableOpacity>
        <TouchableOpacity
          ref={tourRef('menuBtn')}
          style={[lnd.lsBtn, { borderColor: t.btnBorder }]}
          onPress={onMenu} activeOpacity={0.75} hitSlop={10}
        >
          {menuAsBack
            ? <Text style={{ color: t.btnText, fontFamily: t.font, fontSize: s.f(11) }}>‹</Text>
            : <Cog size={ICON_SZ} color={t.btnText} />}
        </TouchableOpacity>
      </View>

      {/* Signal bar + pill — flex so small screens (SE) get a shorter bar with
          everything still fitting; maxWidth caps the stretch on big panels. */}
      {/* ★★★ TOP-ALIGNED, NOT CENTRED. justifyContent:'center' inside a row whose alignItems is
          'stretch' floated this box in the middle of the tallest column, leaving black padding
          above AND below it — "Landscape is wasting space, there is black padding above the
          frequency/signal meter box" (Stuart, 2026-09-24). The drums and the button columns start
          at the top; this now starts there too, so the row reads as one band of controls. */}
      <View style={{ width: s.r(340), justifyContent: 'flex-start' }}
            onLayout={(e: any) => setSigW(e.nativeEvent.layout.width)}>
        <View style={[lnd.sigFrame, { height: SIG_H }]}>
          <SignalCanvas width={sigW} height={SIG_H} signal={signal} peak={peak} bus={bus} />
          <FreqModePill
            freqStr={freqStr} unit={unit} modeLabel={modeLabel} snrText={snrText}
            connected={connected} signalActive={signalActive} bus={bus} meterMode={meterMode} fmStereo={fmStereo}
            onFreqTap={onFreqTap} onModeTap={onModeTap}
            freqFontSize={FREQ_FONT} freqWidth={FREQ_W} unitFontSize={UNIT_FONT}
            modeFontSize={MODE_FONT} modeLs={MODE_LS} snrWidth={SNR_W}
            pillPadH={PILL_PAD_H} pillPadV={PILL_PAD_V}
            modePadH={MODE_PAD_H} modePadV={MODE_PAD_V} gap={PILL_GAP}
            sharedTuner={sharedDial ?? null}
          />
        </View>
      </View>

      {/* AUDIO + CHAT column */}
      <View style={{ width: BTN_W, gap: GAP }}>
        <TouchableOpacity
          style={[lnd.lsBtn, { borderColor: isRecording ? 'rgba(220,40,40,0.90)' : t.btnBorder }]}
          onPress={onAudio} activeOpacity={0.75} hitSlop={10}
        >
          {audioAsRecord
            ? <RecordIcon size={ICON_SZ} color={t.btnText} />
            : <AudioIcon size={ICON_SZ} color={t.btnText} />}
        </TouchableOpacity>
        <TouchableOpacity
          style={[lnd.lsBtn, { borderColor: chatUnread ? 'rgba(40,140,255,0.85)' : t.btnBorder, opacity: chatOff ? 0.4 : 1 }]}
          onPress={chatOff ? undefined : onChat} disabled={chatOff} activeOpacity={0.75} hitSlop={10}
        >
          <ChatIcon size={ICON_SZ} color={t.btnText} />
        </TouchableOpacity>
      </View>

      {/* Zoom drum (omitted for FM-DX single-drum tuner) */}
      {!singleDrum && (
        <View style={{ flex: 1, minWidth: s.r(80) }}>
          {zoomKeys
            ? <TunerKeys type="zoom" height={DRUM_H} onStep={onZoomStep ?? noStep} onSweepStep={onZoomSweep} style={{ flex: 1 }} />
            : <DrumWheel type="zoom" height={DRUM_H} onDelta={onBwDelta} style={{ flex: 1 }} />}
          {/* ★★★ THE READOUTS LIVE UNDER THE ZOOM KEYS, NOT UNDER THE VFO — and the reason is not
              tidiness. They were in the VFO column, so the recording row APPEARED AND DISAPPEARED
              inside the group that holds the tuning keys, and the keys resized under the thumb
              while you were using them. Stuart: "also means the controls dont have to grow and
              shrink when recording is happening."
              ★★ The zoom column is the right home on its own terms too: it is the one group whose
                 height nothing else depends on, and these are STATUS, not controls — so they sit
                 with the control you are least likely to be holding.
              ★ Portrait is untouched: it has a full-width row of its own (por.clockRow) and never
                had the problem. */}
        </View>
      )}

      </View>

      {/* ★★★ ONE FULL-WIDTH STATUS ROW, BENEATH EVERYTHING — Stuart's layout, 2026-09-24:
          "have a clear row at the bottom … Server time/UTC Recording Timer | NR/NB/AN | sig 25KB
          20FPS Gain 29db IF Wide".
          ★★ It replaces two cramped half-rows tucked under the drums, and it is why the stats can
             stop truncating: they had the width of ONE COLUMN and now have the bar.
          ★ Times left, audio chain centre, link right — and the recording slot keeps its reserved
            space so nothing resizes under a thumb when recording starts (the reason it was pulled
            out of the tuning column in the first place). */}
      <View style={lnd.statusRow}>
        <View style={lnd.statusSide}>
          <ClockRow clock={clock} color={t.clockColor} font={t.font} size={CLOCK_FONT} />
          <View style={[lnd.recRow, !isRecording && { opacity: 0 }]} pointerEvents="none">
            <View style={lnd.recDot} />
            <Text style={[lnd.recTime, { fontFamily: t.font, fontSize: CLOCK_FONT }]}>
              {isRecording ? recTime : '0:00'}
            </Text>
          </View>
        </View>
        <DspBadges nr={dspNr} nb={dspNb} an={dspAn} onPress={onAudio}
                   font={t.font} />
        <View style={[lnd.statusSide, { justifyContent: 'flex-end' }]}>
          <LinkIndicator bus={bus} />
        </View>
      </View>

    </View>
  );
}

const lnd = StyleSheet.create({
  /* ★ The bar's own bottom row. `flex: 1` on each side with the badges in the middle keeps the
     audio chain centred regardless of how long the clock or the stats are. */
  statusRow:  { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between',
                gap: 8, paddingHorizontal: 4, marginTop: 3 },
  statusSide: { flex: 1, minWidth: 0, flexShrink: 1, flexDirection: 'row',
                alignItems: 'center', gap: 8 },
  sigFrame: { borderRadius: 7, overflow: 'hidden', backgroundColor: 'rgba(105,98,82,0.30)', justifyContent: 'center', alignSelf: 'stretch' },
  lsBtn:    { flex: 1, backgroundColor: 'rgba(20,10,0,0.75)', borderWidth: 1, borderRadius: 4, alignItems: 'center', justifyContent: 'center', paddingHorizontal: 4 },
  // ★★ NO FIXED lineHeight HERE — it is set at the use site, SCALED, alongside fontSize.
  // A constant 14 lived here while the font is s.f(11), which scales: on a Mac window (and any
  // iPad wide enough to clamp the scale at 1.45) the text renders at ~16pt inside a 14pt line
  // box, and the tops of the glyphs are sliced off — "500Hz" and "100k" both showed it. The
  // portrait button next door has never had the fault because it sets no lineHeight at all.
  // ★ A length that must track fontSize cannot be a constant when fontSize is not one.
  lsTxt:    { letterSpacing: 0.5, textAlign: 'center' },
  clock:    { letterSpacing: 1, marginTop: 3, textAlign: 'center' },
  recRow:   { flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: 3, marginTop: 1 },
  recDot:   { width: 5, height: 5, borderRadius: 2.5, backgroundColor: '#e05050' },
  recTime:  { letterSpacing: 1, color: '#e05050' },
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
  onControlRects,
  /* ★★★ AND THE FIVE THE BARS NEED. They are declared in ControlsBarProps and were arriving from SDRScreen,
   *  but this destructure never took them — so `shared` could not pass them on, and my first attempt at that
   *  fix referenced names that were not in scope, which threw "Property 'readOnly' doesn't exist" and bounced
   *  the app to the server list (Stuart, 2026-09-20). Three layers each keep their own hand-written copy of
   *  the prop list; a name has to appear in ALL of them or it is either dead or fatal. */
  readOnly,
  sharedDial,
  storms,
  adminMode,
}: ControlsBarProps) {
  // ★ Flashes when a captured region hands the keyboard back — see useRegionHandback.
  const handback = useRegionHandback();
  const handbackFlash = useRef(new Animated.Value(0)).current;
  useEffect(() => {
    if (!handback) return;                       // not on first mount, only on a real handback
    handbackFlash.setValue(1);
    Animated.timing(handbackFlash, { toValue: 0, duration: 450, useNativeDriver: true }).start();
  }, [handback, handbackFlash]);

  const { theme: t } = useTheme();
  const s = useUiScale();

  const freqStr   = useMemo(() => freqFormat ? freqFormat(frequency) : formatHz(frequency, freqUnit), [frequency, freqUnit, freqFormat]);
  const unit      = useMemo(() => freqUnitLabel(freqUnit),       [freqUnit]);
  const stepLabel = useMemo(() => formatStep(step),      [step]);
  const snrText   = meterLabel ?? ''; // FM-DX static reading; live text comes from the bus + meterText()
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
  const PAD_H   = s.r(12);
  const PAD_TOP = s.r(8);
  const RADIUS  = s.r(18);

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
    freqStr, unit,
    // §5.1: compose the running decoder onto the demod — USB → USB: RTTY (wefax reads FAX).
    modeLabel: dabOn ? 'DAB' : modeDisplay(mode) + (activeDecoder ? `: ${(activeDecoder === 'wefax' ? 'fax' : activeDecoder).toUpperCase()}` : ''),
    snrText, fmStereo,
    connected, signalActive, bus: meterBus, meterMode: signalMode,
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
    vfoKeys, zoomKeys, onVfoStep, onZoomStep, onZoomSweep, vfoSweepRate, onControlRects,
    /* ★★★ FIVE PROPS THE BARS DESTRUCTURE AND NEVER RECEIVED (Stuart, 2026-09-20: "no shared dial notification
     *  above the frequency"). ControlsBar took them, the bars declared them, and NOTHING carried them across
     *  this object — so `sharedDial` was undefined in both bars and the shared-tuner banner could not draw on
     *  any phone, tablet or Mac. The web client showed it because it has its own code; the app's box has been
     *  dead since the day it was added, and in landscape it did not merely fail, it THREW (see LandscapeBar).
     *  ★★ The same silence covers the bar's session clock, the lightning badge, read-only and admin: the props
     *     exist, are typed `any`, and go nowhere. A prop list written twice is a fact stored twice. */
    readOnly, sharedDial, storms, adminMode,
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
      <BlurView intensity={Platform.OS === 'ios' ? 35 : 80} tint="dark" style={StyleSheet.absoluteFill} />
      {/* Tinted overlay — semi-transparent so blur shows; NOT fully opaque */}
      <View style={[StyleSheet.absoluteFill, root.tint, { borderRadius: RADIUS }]}
            pointerEvents="none" />
      {/* Border ring */}
      <View style={[root.border, { borderRadius: RADIUS, borderColor: t.barBorder }]}
            pointerEvents="none" />
      {/* ★★★ APPLE TV USES THE PORTRAIT CLUSTER, on a 16:9 screen (Stuart, 2026-08-04).
          Not a cosmetic choice — it is the one that matches the remote. Portrait STACKS the
          sections, so the four buttons sit one swipe DOWN from the frequency, on the same
          vertical axis the highlight already travels to reach the servers chip. Landscape FLANKS
          the frequency left and right, which turns a single directional move into horizontal
          hunting. It also puts the clock and status rows where they already belong, so this is a
          row DELETION (see PortraitBar) rather than a hand-built hybrid of the two layouts.
          See briefs/BRIEF-tvos-app.md §2. */}
      {s.isLandscape && !IS_TV
        ? <LandscapeBar {...shared} />
        : <PortraitBar  {...shared} />
      }
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
    backgroundColor: 'rgba(8,6,2,0.55)',
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
