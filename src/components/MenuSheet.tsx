/**
 * MenuSheet — slide-up panel matching VibeSDR_Mockup_SAVE.html exactly.
 *
 * Sections (in order):
 *   Nearby Station · Spectrum/Waterfall · Audio · Server Maps
 *   Client Decoders · Server Extensions · Controls · Instance
 *   Reset Interface Settings
 */

import StationLogo from './StationLogo';
import { NavCtx, NavRow, usePanelNav, useNavButton, useNavRange, useListNav, noteTouchInteraction, revealIn, useKeyboardMode, useFullKeyboardAccessSuspected } from './PanelNav';
import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import {
  Alert,
  Animated,
  Share,
  Dimensions,
  Image,
  Modal,
  ScrollView,
  StyleSheet,
  Text,
  TextInput,
  TouchableOpacity,
  TouchableWithoutFeedback,
  View,
  useWindowDimensions,

  NativeEventEmitter,
  NativeModules,} from 'react-native';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import { BlurView } from 'expo-blur';
import Slider from '@react-native-community/slider';
import { COLORMAP_NAMES } from '../assets/colormapUtils';
import { RTTY_PRESETS, type RttySettings } from '../services/DecoderClient';
import {
  searchStations, fmtFreq, fmtRange, grpAbbr,
  type ServerBookmark, type ServerBand, type SearchResult,
} from '../services/stations';
import { type UserBookmark } from '../services/userBookmarks';
import {
  getSyncStatus, loadSyncEnabled, onSyncStatus, setSyncEnabled, resetCloudToThisDevice,
  syncDiagnostic, type SyncStatus,
} from '../services/cloudSync';
import { linkDebug } from '../services/linkManager';
import { APP_VERSION } from '../constants/version';
import UsbSdrIcon from './UsbSdrIcon';
import VfoLockIcon from './VfoLockIcon';
import {
  CONVERTER_PRESETS, ConverterProfile, HF_CEILING_HZ, V4_WARNING, fromPreset, isIdentity,
  presetById,
} from '../services/converter';
import SectionIcon, { type SectionIconName } from './SectionIcon';
import { isKiwiProtocol, kiwiFamilyLabel } from '../services/sdrTypes';

// ── Types ──────────────────────────────────────────────────────────────────────

export interface MenuSheetProps {
  visible:     boolean;
  serverName:  string;
  serverUrl:   string;

  colormap:    string;
  dbMin:       number;
  dbMax:       number;
  onColormap:  (n: string) => void;
  onDbMin:     (v: number) => void;
  onDbMax:     (v: number) => void;

  filterLow:    number;
  filterHigh:   number;
  /** Per-edge passband half-width cap (Hz) for the active mode/backend.
      Drives the bandwidth sliders' range — never hardcode it. Default 6000. */
  bwEdgeMax?:   number;
  onFilterLow:  (v: number) => void;
  onFilterHigh: (v: number) => void;
  /** Atomic both-edges setter — used by the mirrored sliders + SYNC. */
  onFilterBoth?: (low: number, high: number) => void;
  nr?:          boolean;
  onNr?:        (mode: 'off'|'nr'|'nr2') => void;
  onZoomIn?:    () => void;
  onZoomOut?:   () => void;
  onZoomMin?:   () => void;   // full span out
  onZoomMax?:   () => void;   // full zoom in
  onSetDefault?: () => void;
  isDefaultInstance?: boolean;
  /** Favourite the current instance (network receivers only — hidden for local
   *  USB / RTL-TCP / SpyServer, which favourite via the picker instead). */
  isFavourite?: boolean;
  onToggleFavourite?: () => void;
  /** Client decoders — skin semantics: toggle start/stop, menu stays open. */
  decMode?:        'rtty'|'navtex'|'wefax'|'sstv'|'morse'|'whisper'|'time'|null;
  decOn?:          boolean;
  onDecToggle?:    (m: 'rtty'|'navtex'|'wefax'|'sstv'|'morse'|'whisper'|'time') => void;
  /** Digital/CW spots feeds (skin lsvSpots). */
  spotsKind?:      'digi'|'cw'|null;
  onSpotsToggle?:  (k: 'digi'|'cw') => void;
  /** Server map overlays (skin lsv-hfdl / lsv-digmap / lsv-cwmap). */
  /** Local/Kiwi: open the on-device-decoder FT8 spots map. */
  onSpotsMap?:     () => void;
  rttySettings?:   RttySettings;
  onRttySettings?: (s: RttySettings) => void;
  wefaxLpm?:       number;
  onWefaxLpm?:     (lpm: number) => void;
  nb?:          boolean;
  onNb?:        (on: boolean) => void;
  recording?:   boolean;
  onRec?:       () => void;
  recSeconds?:  number;

  // SNR squelch — value ≤ -999 = off/open
  snrSquelch?:    number;
  onSnrSquelch?:  (v: number) => void;
  /** Adaptive waterfall-rate policy. 'adaptive' follows the link, 'full' never throttles,
   *  'lowData' pins the floor by choice (so it must never read as a poor link). */
  linkMode?:      'full' | 'adaptive' | 'lowData';
  onLinkMode?:    (m: 'full' | 'adaptive' | 'lowData') => void;
  // Local SDR power-based squelch (dBFS, -100 = off). Replaces the (dead) SNR
  // squelch slider on local instances.
  localSquelch?:   number;
  onLocalSquelch?: (db: number) => void;
  // Local SDR audio noise reduction strength (0=off..20).
  localNR?:        number;
  onLocalNR?:      (level: number) => void;
  // Automatic notch (adaptive line enhancer) on/off — all backends.
  notchOn?:        boolean;
  onNotch?:        (on: boolean) => void;
  // EiBi on-device shortwave schedule (fallback bookmarks) on/off.
  eibiEnabled?:    boolean;
  onEibiToggle?:   (on: boolean) => void;
  // Kiwi server-side squelch (0=off..99).
  kiwiSquelch?:    number;
  onKiwiSquelch?:  (v: number) => void;
  // FM squelch — only shown for fm/nfm modes
  fmSquelch?:     number;
  onFmSquelch?:   (v: number) => void;
  isFmMode?:      boolean;
  // OWRX squelch (dB, -150=off) + NR (threshold dB, 0=off) — replace the UberSDR
  // SNR/FM squelch + NR/NB controls (which do nothing on OWRX).
  serverLabel?:   string | null;
  onOwrxSquelch?: (db: number) => void;
  onOwrxNr?:      (threshold: number) => void;
  // OWRX server/profile preset DSP defaults — seeds the squelch/NR sliders on
  // connect + each profile switch (seq forces a re-sync even on an identical value).
  owrxDspDefaults?: { squelchDb?: number; nrEnabled?: boolean; nrThreshold?: number; seq: number };

  // Server DSP
  serverDspEnabled?:   boolean;
  serverDspFilter?:    string;
  serverDspParams?:    Record<string, string>;
  dspFilters?:         DspFilterDesc[];
  dspError?:           string | null;
  onServerDsp?:        (enabled: boolean) => void;
  onServerDspFilter?:  (name: string) => void;
  onServerDspParam?:   (name: string, value: string) => void;

  signalMode?:     'snr' | 'smeter' | 'dbfs';
  onSignalMode?:   (m: 'snr' | 'smeter' | 'dbfs') => void;
  displayStyle?:   'amber' | 'white';
  onDisplayStyle?: (s: 'amber' | 'white') => void;
  drumMode?:       'normal' | 'precise';
  onDrumMode?:     (m: 'normal' | 'precise') => void;
  mediaSkip?:      'step' | 'bookmark';
  onMediaSkip?:    (m: 'step' | 'bookmark') => void;
  /** Recentre the spectrum view on the tuned frequency (skin parity). */
  onCentreVfo?:    () => void;
  /** VFO lock: true = view follows VFO (default). Unlocked = waterfall pans. */
  vfoLocked?:      boolean;
  onToggleVfoLock?: () => void;
  /** Hide the controls bar for a full-screen waterfall (chevron restores). */
  onHideControls?: () => void;
  /** Opens the full keyboard reference — see KeyboardShortcuts.tsx. */
  onKeyHelp?: () => void;
  onDispReset?:      () => void;
  onDispSaveServer?: () => void;
  onDispSaveGlobal?: () => void;
  hapticsEnabled?: boolean;
  onHaptics?:      (on: boolean) => void;
  hapticsHardware?: boolean;   // false → device has no haptic motor, hide toggle
  /** Per-control drum-or-keys. Two independent settings — the drums are the
   *  default, and either control can be swapped for the HiFi tuner keys. */
  vfoKeys?:    boolean;
  zoomKeys?:   boolean;
  onVfoKeys?:  (on: boolean) => void;
  onZoomKeys?: (on: boolean) => void;
  /** The ONE pointing-device question: what the vertical scroll wheel does. The
   *  opposite control falls to whatever orthogonal axis the hardware has. */
  wheelAction?:   'zoom' | 'tune';
  onWheelAction?: (m: 'zoom' | 'tune') => void;

  vtsName?:    string;
  vtsFreq?:    number;
  onVtsNext?:  () => void;
  onVtsPrev?:  () => void;
  // OWRX profiles (hidden unless a backend reports them)
  profiles?:        { id: string; name: string }[];
  activeProfileId?: string;
  sdrUsage?:        Record<string, { name: string; inUse: boolean; activeProfileId?: string }>;  // OWRX: per-SDR usage
  clientCount?:     number;     // OWRX: live users online
  onSelectProfile?: (id: string) => void;
  // OWRX DAB ensemble — programme picker (hidden unless a DAB ensemble is tuned)
  dabProgrammes?:   { id: number; name: string }[];
  activeDabId?:     number;
  onSelectDab?:     (id: number) => void;
  dabSpeed?:        number;            // DAB speed-correction factor (1 = off)
  onDabSpeed?:      (scale: number) => void;
  serverType?:      string;   // 'ubersdr' | 'owrx' | 'kiwi' | 'web888' — picks the footer logo
  /** ★★★ A VibeServer SPEAKS THE UberSDR PROTOCOL, SO serverType SAYS 'ubersdr' — and the SERVER
   *  ADMIN buttons below therefore offered UberSDR's pages (/admin.html, /noisefloor.html …),
   *  which a VibeServer does not serve. The result was a section of links that 404, and no way at
   *  all to reach the admin or setup pages that DO exist (Stuart, 2026-08-12: "no admin/setup
   *  buttons when admin password is entered").
   *  ★ Supplied only when we are admin: the pages refuse without the credential, and offering a
   *    button that cannot work is worse than not offering one. */
  vibeAdminUrl?:    string;
  vibeSetupUrl?:    string;
  searchBookmarks?: ServerBookmark[];
  searchBands?:     ServerBand[];
  onSearchTune?:    (hz: number, mode?: string | null, isBand?: boolean) => void;
  userBookmarks?:     UserBookmark[];
  currentFreq?:       number;
  currentMode?:       string;
  onAddBookmark?:     (name: string, allInstances: boolean) => void;
  onDeleteBookmark?:  (bm: UserBookmark) => void;
  onExportBookmarks?: () => void;
  onImportBookmarks?: (text: string, allInstances: boolean) => string;
  onPickImportFile?: (allInstances: boolean) => Promise<string>;

  onClose:          () => void;
  onBack?:          () => void;
  /** The connected radio's model, from the server — names the controls button. */
  radioModel?: string;
  /** V4 local hardware: opens the radio's controls submenu (Android only). */
  onLocalHardware?: () => void;
  /** ★★★ THE CONVERTER IN FRONT OF THE DONGLE — configuration AND its on/off, both here.
   *  Present only on the paths where the listener owns the converter (local USB, rtl_tcp,
   *  SpyServer); absent on VibeServer, whose owner sets one once on the radio's setup page and
   *  whose server then publishes true frequencies, and absent on every other backend for the same
   *  reason. See converter.ts — correcting a second time is the failure mode. */
  converter?:   ConverterProfile;
  onConverter?: (c: ConverterProfile) => void;
  /** RTL-TCP session — footer shows the RTL-TCP icon + label (vs USB for direct). */
  isTcp?:          boolean;
  onAdminLink?:     (path: string, title: string) => void;
  /** ★★★ THE ADMIN PASSWORD BELONGS WHERE THE PAGES IT UNLOCKS ARE. It lived only in the hardware
   *  panel, so an owner looking for the admin page found no way in from here — and unlocking in
   *  the hardware panel had no visible effect on this menu at all (Stuart, 2026-08-13). */
  adminSet?:        boolean;
  adminOk?:         boolean;
  adminRefused?:    boolean;
  onAdminUnlock?:   (pw: string) => void;
  onResetSettings?: () => void;
  onReplayTour?:    () => void;
  onDisplaySettings?: () => void;
  /** UberSDR server software version (/api/description) — footer right side. */
  serverVersion?:   string | null;
  /** ★★★ Is this a VibeServer, rather than a radio in THIS device? MenuSheet decides "local" from
   *  `!!onLocalHardware`, which was sound when only on-device USB had hardware controls — but a
   *  VibeServer offers them remotely, so the footer branded a Pi across the room as "Local
   *  Hardware — via VibeDSP (native)" (Stuart, 2026-08-12). The parent knows which it is; asking
   *  a callback's existence never could. */
  isVibeServer?:    boolean;
  /** Opens the About VibeSDR overlay (footer left side). */
  onAbout?:         () => void;
  /** Opens the saved-recordings browser. */
  onRecordings?:    () => void;

  // Display settings panel props
  vfoNeedle?:         string;
  onVfoNeedle?:       (hex: string) => void;
  /** Needle/glow brightness 1–10 (5 = original look). */
  vfoIntensity?:      number;
  onVfoIntensity?:    (v: number) => void;
  /** Frosted backing 0–10 (0 = off) — dims the waterfall behind the needle. */
  vfoFrost?:          number;
  onVfoFrost?:        (v: number) => void;
  /** Instance spectrum backdrop opacity 0–10 (server-supplied image). */
  bgOpacity?:         number;
  onBgOpacity?:       (v: number) => void;
  hasBgImage?:        boolean;
  wfCoarse?:          'auto' | 'manual';
  onWfCoarse?:        (v: 'auto' | 'manual') => void;
  /** UberSDR auto-range symmetric contrast 0–20 (web calibration = 10). */
  autoContrast?:      number;
  onAutoContrast?:    (v: number) => void;
  /** M9PSY 5-tap spatial waterfall smooth. */
  spatialSmooth?:     boolean;
  onSpatialSmooth?:   (v: boolean) => void;
  wfBrightness?:      number;
  onWfBrightness?:    (v: number) => void;
  wfContrast?:        number;
  onWfContrast?:      (v: number) => void;
  wfSharpness?:       number;
  onWfSharpness?:     (v: number) => void;
  specShow?:          boolean;
  onSpecShow?:        (v: boolean) => void;
  specSmoothing?:     number;
  onSpecSmoothing?:   (v: number) => void;
  avgFrames?:         number;
  onAvgFrames?:       (v: number) => void;
  specFloor?:         number;
  onSpecFloor?:       (v: number) => void;
  specPeakScale?:     number;
  onSpecPeakScale?:   (v: number) => void;
  peakHold?:          boolean;
  onPeakHold?:        (v: boolean) => void;
  frameRate?:         '10fps' | '20fps' | '30fps';
  onFrameRate?:       (v: '10fps' | '20fps' | '30fps') => void;
  wfScroll?:          'sharp' | 'default' | 'smooth';
  onWfScroll?:        (v: 'sharp' | 'default' | 'smooth') => void;
  smoothTune?:        boolean;
  onSmoothTune?:      (v: boolean) => void;
  idleSlow?:          boolean;
  onIdleSlow?:        (v: boolean) => void;
  onSpecRatio?:       () => void;
}

// ── Constants ─────────────────────────────────────────────────────────────────

// Server-software logos for the menu footer — one per supported backend so the
// user can see what they're connected to (KiwiSDR/OpenWebRX to come).
const SERVER_LOGOS: Record<string, any> = {
  ubersdr: require('../../assets/logo_ubersdr.png'),
  owrx:    require('../../assets/logo_owrx.png'),
  kiwi:    require('../../assets/logo_kiwi.png'),
  // ★ No Web-888 asset ships, so it borrows the Kiwi mark — it runs Kiwi firmware, so this is
  //   nearly right. The ?? fallback below is the UBERSDR logo, which would be plainly wrong:
  //   an unmapped backend must never be branded as a different vendor's product.
  web888:  require('../../assets/logo_kiwi.png'),
  // ★ VibeServer had no logo, so our OWN server fell back to a generic radio glyph
  //   in the very list where every other backend is branded (Stuart, 2026-07-29).
  vibeserver: require('../../assets/logo_vibeserver.png'),
};

// Accessibility skin (reference body.lsv-a11y) — the single style going
// forward: white Atkinson Hyperlegible text, larger touch targets, neutral
// white borders. Gold survives only as the ACTIVE-state accent.
const C = {
  bg:           'rgba(6,4,2,0.99)',        // a11y #lsv-menu-panel
  border:       'rgba(255,255,255,0.30)',
  divider:      'rgba(255,255,255,0.12)',  // a11y section rules
  gold:         '#ffe566',                 // active text
  goldDim:      'rgba(255,229,102,0.70)',  // active border
  focus:        '#7CFF9B',                 // keyboard/D-pad focus — matches btnFocused
  muted:        'rgba(255,255,255,0.92)',  // base button/value text — white
  btnBg:        'rgba(20,18,14,0.85)',
  active:       'rgba(255,200,0,0.12)',
  danger:       'rgba(160,30,30,0.80)',
  dangerBorder: 'rgba(220,60,60,0.60)',
  text:         '#ffffff',
  sectionC:     'rgba(180,190,210,0.80)',  // a11y .lsv-mp-section
  sliderLabel:  'rgba(200,210,225,0.90)',
};

const { height: SCREEN_H } = Dimensions.get('window');
const SHEET_H = Math.min(SCREEN_H * 0.88, 700);

// Bookmark scope colours (key shown above the saved list)
const BM_GLOBAL_C = 'rgba(110,200,255,0.95)';  // all instances — cyan
const BM_LOCAL_C  = '#ffe566';                 // this instance — gold


// ── Helpers ───────────────────────────────────────────────────────────────────

function fmtHz(hz: number) {
  return hz >= 1000 ? (hz / 1000).toFixed(1) + ' kHz' : hz + ' Hz';
}

function fmtRecTime(s: number) {
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60;
  return `${h}:${String(m).padStart(2,'0')}:${String(sec).padStart(2,'0')}`;
}

// ── Server-side NR (DSP insert) descriptors — shapes match the server's
//    dsp_filters paramInfo/filterInfo JSON (all values are strings on the wire)
export interface DspParamDesc {
  name:          string;
  type?:         string;   // 'float' | 'int' | 'bool' | free text
  default?:      string;
  min?:          string;
  max?:          string;
  description?:  string;
  runtime_safe?: boolean;
}
export interface DspFilterDesc {
  name:         string;
  description?: string;
  params?:      DspParamDesc[];
}

function fmtParamName(n: string) {
  return n.replace(/_/g, ' ').toUpperCase();
}
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

function StepSlider({
  value, min, max, step, format, onChange,
}: {
  value: number; min: number; max: number; step: number;
  format: (v: number) => string; onChange: (v: number) => void;
}) {
  const clamp = (v: number) => Math.min(max, Math.max(min, v));
  return (
    <View style={styles.stepSlider}>
      <TouchableOpacity style={styles.stepSliderBtn} hitSlop={8}
        onPress={() => onChange(clamp(value - step))}>
        <Text style={styles.stepSliderBtnTxt}>−</Text>
      </TouchableOpacity>
      <Text style={styles.stepSliderVal}>{format(value)}</Text>
      <TouchableOpacity style={styles.stepSliderBtn} hitSlop={8}
        onPress={() => onChange(clamp(value + step))}>
        <Text style={styles.stepSliderBtnTxt}>+</Text>
      </TouchableOpacity>
    </View>
  );
}

// ── Sub-components ─────────────────────────────────────────────────────────────

function SectionLabel({ label, icon, first }: { label: string; icon?: SectionIconName; first?: boolean }) {
  return (
    <View style={[styles.sectionBar, first && styles.sectionBarFirst]}>
      <View style={styles.sectionRow}>
        {icon && <SectionIcon name={icon} size={16} color={C.sectionC} />}
        <Text style={styles.sectionLabel}>{label}</Text>
      </View>
    </View>
  );
}

// ── Keyboard navigation (BRIEF-inputs §6, in-panel layer) ─────────────────────
//
// ★ The primitives learn about focus; the 1000-line sheet is not restructured.
// Every Btn registers itself on mount, so the navigable grid is derived from the
// JSX that already exists — mount order IS reading order — and new menu rows are
// automatically navigable without anyone remembering to wire them up.
//
// Rows come from BtnRow via context, which gives the natural 2-D shape: up/down
// moves between rows, left/right within one.
// ★ The machinery itself now lives in PanelNav.tsx — it is shared with StepPicker,
// AudioSheet and ModeSelector, and is what a game controller's D-pad will drive
// (briefs/BRIEF-controls-keyboard-and-gamepad.md). MenuSheet keeps only the wiring.
function BtnRow({ children, col }: { children: React.ReactNode; col?: boolean }) {
  return (
    <NavRow>
      <View style={[styles.btnRow, col && styles.btnRowCol]}>{children}</View>
    </NavRow>
  );
}

function Btn({ label, active, danger, onPress, full, style, icon, skipNav }: {
  label: string; active?: boolean; danger?: boolean;
  onPress?: () => void; full?: boolean; style?: object; icon?: SectionIconName;
  /** Keep OUT of the keyboard focus order — used for buttons that open the receiver's own
   *  pages, where none of our shortcuts apply. See useNavButton. */
  skipNav?: boolean;
}) {
  // ★ Scroll-into-view is now MEASURED against the ScrollView's content node
  // (revealIn, inside useNavButton) rather than estimated as `row * 46`, which
  // assumed a uniform row height and was wrong for anything nested or unevenly
  // sized.
  const { focused, viewRef } = useNavButton(onPress, skipNav);
  return (
    <TouchableOpacity
      ref={viewRef as any}
      style={[styles.btn, active && styles.btnActive, danger && styles.btnDanger, full && styles.btnFull,
              icon && { flexDirection: 'row', gap: 7 }, style,
              focused && styles.btnFocused]}
      onPress={onPress} hitSlop={4} activeOpacity={0.7}
    >
      {icon && <SectionIcon name={icon} size={15} color={active ? C.gold : C.muted} />}
      <Text style={[styles.btnText, active && styles.btnTextActive, danger && styles.btnTextDanger]}>
        {label}
      </Text>
    </TouchableOpacity>
  );
}

function SwatchBtn({ hex, active, onPress }: { hex: string; active: boolean; onPress: () => void }) {
  const { focused, viewRef } = useNavButton(onPress);
  return (
    <TouchableOpacity ref={viewRef as any} hitSlop={4}
      style={[styles.swatch, { backgroundColor: hex },
              active && styles.swatchActive,
              // Focus outranks the active ring, or you cannot see where you are on the
              // colour you already have selected.
              focused && { borderColor: C.focus, borderWidth: 3 }]}
      onPress={onPress}
    />
  );
}

function CmapHeaderBtn({ open, onPress, children }: {
  open: boolean; onPress: () => void; children: React.ReactNode;
}) {
  const { focused, viewRef } = useNavButton(onPress);
  return (
    <View ref={viewRef} style={focused ? styles.dropHeaderFocused : undefined}>{children}</View>
  );
}

// ── Keyboard-reachable slider ────────────────────────────────────────────────
// ★ Sliders used to be SKIPPED by keyboard focus entirely — only Btn registered, so
// the caret jumped straight past every slider and they were unreachable, not merely
// awkward. NavSlider registers as a value control: focus lands on it, left/right
// nudges it by one `step`, up/down leaves.
//
// ★ No wrapper View, deliberately. These sliders are laid out with `flex:1` inside
// rows, and wrapping them would change the layout of all twelve. Focus is shown by
// re-tinting the thumb and track instead, which is layout-free — and on a slider the
// thumb IS where the eye already is.
function NavSlider(props: React.ComponentProps<typeof Slider>) {
  const { minimumValue = 0, maximumValue = 1, step, value = 0, onValueChange } = props;
  // A slider with no explicit step still has to move by SOMETHING; a twentieth of
  // the range is a sane nudge and matches how these read on screen.
  const nudge = step && step > 0 ? step : (maximumValue - minimumValue) / 20;
  // ★ ATTACH THE REF. Without it reveal cannot measure the slider and falls back to the old
  // row-height ESTIMATE, which is why a slider landed barely in view at the foot of the sheet
  // while every button centred correctly — the buttons attach theirs and the sliders did not.
  const { focused, viewRef } = useNavRange((dir) => {
    const next = Math.max(minimumValue, Math.min(maximumValue, value + dir * nudge));
    if (next !== value) onValueChange?.(next);
  });
  return (
    <Slider
      ref={viewRef as any}
      {...props}
      minimumTrackTintColor={focused ? C.focus : props.minimumTrackTintColor}
      thumbTintColor={focused ? C.focus : props.thumbTintColor}
    />
  );
}

// VFO Lock toggle — padlock-over-spectrum glyph + state label. Disabled (dimmed)
// on local hardware until native panning lands (Phase 2).
function VfoLockBtn({ locked, disabled, onPress, full }: {
  locked: boolean; disabled?: boolean; onPress?: () => void; full?: boolean;
}) {
  return (
    <TouchableOpacity
      style={[styles.btn, { flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: 4 }, full && styles.btnFull, disabled && { opacity: 0.4 }]}
      onPress={disabled ? undefined : onPress} disabled={disabled} hitSlop={4} activeOpacity={0.7}
    >
      <VfoLockIcon size={20} locked={locked} />
      <Text style={[styles.btnText, !locked && { color: '#3ddc84' }]}>{locked ? 'LOCKED' : 'FREE'}</Text>
    </TouchableOpacity>
  );
}

function SubLabel({ label, small }: { label: string; small?: boolean }) {
  return <Text style={[styles.subLabel, small && styles.subLabelSmall]}>{label}</Text>;
}

/**
 * iCloud sync: the on/off switch and, more importantly, whether it is WORKING.
 *
 * ★ A sync that has silently stopped is worse than one that never started —
 * the user believes their favourites and bookmarks are covered. So this reports
 * the last error rather than only the happy state, and says nothing at all on a
 * platform where iCloud does not exist rather than showing a dead control.
 */
function ICloudRow() {
  const [s, setS] = useState<SyncStatus>(getSyncStatus());
  const [resetting, setResetting] = useState(false);
  useEffect(() => {
    void loadSyncEnabled();
    return onSyncStatus(setS);
  }, []);
  // ★ Destructive and remote, so it CONFIRMS. Everything else in this menu is a
  // toggle you can flip back; this reaches other devices and cannot be undone.
  const onReset = useCallback(() => {
    Alert.alert(
      'Replace iCloud with this device?',
      'iCloud is replaced by what is on this iPhone. Nothing here is deleted.\n\n'
      + 'Anything NOT on this iPhone is deleted everywhere — including favourites or '
      + 'bookmarks added on the watch that never reached this device. Your other devices '
      + 'will remove them when they next sync.\n\n'
      + 'Use this to clear entries left behind by an old build or a device you no longer have.',
      [
        { text: 'Cancel', style: 'cancel' },
        { text: 'Replace', style: 'destructive', onPress: () => {
          setResetting(true);
          resetCloudToThisDevice().finally(() => setResetting(false));
        } },
      ],
    );
  }, []);
  if (!s.supported) return null;
  const when = s.lastSyncAt
    ? new Date(s.lastSyncAt).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
    : null;
  return (<>
    <SectionLabel label="iCLOUD SYNC" icon="instance" />
    <BtnRow>
      <Btn label="ON"  active={s.enabled}  onPress={() => { void setSyncEnabled(true); }} />
      <Btn label="OFF" active={!s.enabled} onPress={() => { void setSyncEnabled(false); }} />
    </BtnRow>
    <SubLabel small label={
      !s.enabled          ? 'Off — favourites and bookmarks stay on this device.'
      : s.lastError       ? `⚠ ${s.lastError}`
      : !s.available      ? 'Waiting for iCloud — sign in to iCloud in Settings.'
      : when              ? `Synced ${when}.`
      :                     'Syncing…'
    } />
    {/* Sync MERGES, so anything already in iCloud keeps coming back — including
        entries from a build or a device that is long gone. This is the only way
        to say "throw that away", and it needs a person to mean it. */}
    {s.enabled && (<>
      <BtnRow>
        <Btn label={resetting ? 'REPLACING…' : 'REPLACE iCLOUD WITH THIS DEVICE'}
             active={false} onPress={resetting ? () => {} : onReset} />
      </BtnRow>
      <SubLabel small label="Clears leftovers from an old build or a device you no longer have." />
      {/* ★ Evidence, not inference. A resurrecting entry has several possible
          causes that look identical from the outside; this shows which one it
          is — every item on both sides with the key it resolves to and its
          timestamp, plus the tombstones and the deletion snapshot. */}
      <BtnRow>
        <Btn label="SHARE SYNC DIAGNOSTIC" active={false} onPress={() => {
          syncDiagnostic()
            .then((text) => Share.share({ message: text }))
            .catch((e) => Alert.alert('Diagnostic failed', String(e?.message ?? e)));
        }} />
      </BtnRow>
    </>)}
  </>);
}

function OptRow({ children }: { children: React.ReactNode }) {
  return <View style={[styles.btnRow, styles.optRow]}>{children}</View>;
}

function SegBtn({ label, active, onPress }: { label: string; active: boolean; onPress: () => void; key?: React.Key }) {
  return (
    <TouchableOpacity style={[styles.btn, active && styles.btnActive]} onPress={onPress} hitSlop={4} activeOpacity={0.7}>
      <Text style={[styles.btnText, active && styles.btnTextActive]}>{label}</Text>
    </TouchableOpacity>
  );
}

// ── Decoder settings (skin v6.3.2 lsv-mp-dec-settings-panel, REAL wiring) ─────
// RTTY: preset/shift/baud/encoding/invert; WEFAX: LPM. Settings live in the
// menu (not the decoder panel); changing one while running re-attaches.

function RttySettingsRows({ s, onChange }:
  { s: RttySettings; onChange: (s: RttySettings) => void }) {
  const presetKey = Object.entries(RTTY_PRESETS).find(([, p]) =>
    p.shift === s.shift && p.baud === s.baud &&
    p.encoding === s.encoding && p.inverted === s.inverted)?.[0] ?? '';
  return (
    <>
      <SubLabel label="Preset" />
      <OptRow>
        {([['ham','HAM'],['weather','WX'],['sitor-b','SITOR-B']] as const).map(([k, l]) => (
          <SegBtn key={k} label={l} active={presetKey === k}
                  onPress={() => onChange({ ...RTTY_PRESETS[k] })} />
        ))}
      </OptRow>
      <SubLabel label="Shift (Hz)" />
      <OptRow>{[170, 200, 425, 450, 850].map(v => (
        <SegBtn key={v} label={String(v)} active={s.shift === v}
                onPress={() => onChange({ ...s, shift: v })} />
      ))}</OptRow>
      <SubLabel label="Baud" />
      <OptRow>{[45.45, 50, 75, 100].map(v => (
        <SegBtn key={v} label={String(v)} active={s.baud === v}
                onPress={() => onChange({ ...s, baud: v })} />
      ))}</OptRow>
      <SubLabel label="Encoding" />
      <OptRow>{(['ITA2', 'ASCII', 'CCIR476'] as const).map(v => (
        <SegBtn key={v} label={v} active={s.encoding === v}
                onPress={() => onChange({ ...s, encoding: v })} />
      ))}</OptRow>
      <OptRow>
        <Btn label={s.inverted ? 'INVERT: ON' : 'INVERT: OFF'} active={s.inverted}
             onPress={() => onChange({ ...s, inverted: !s.inverted })} />
      </OptRow>
    </>
  );
}

// ── Main component ─────────────────────────────────────────────────────────────

export default function MenuSheet({
  visible, serverName, serverUrl,
  colormap, dbMin, dbMax, onColormap, onDbMin, onDbMax,
  filterLow, filterHigh, bwEdgeMax = 6000, onFilterLow, onFilterHigh, onFilterBoth,
  nr = false, onNr, nb = false, onNb, recording = false, onRec, recSeconds = 0,
  snrSquelch = -999, onSnrSquelch,
  linkMode = 'adaptive', onLinkMode,
  localSquelch = -100, onLocalSquelch,
  localNR = 0, onLocalNR, notchOn = false, onNotch, eibiEnabled = true, onEibiToggle, kiwiSquelch = 0, onKiwiSquelch,
  fmSquelch  = -999, onFmSquelch, isFmMode = false,
  serverLabel = null, onOwrxSquelch, onOwrxNr, owrxDspDefaults,
  serverDspEnabled = false, serverDspFilter = '', serverDspParams = {},
  dspFilters = [], dspError = null, onServerDsp, onServerDspFilter, onServerDspParam,
  signalMode = 'snr', onSignalMode,
  displayStyle = 'amber', onDisplayStyle,
  drumMode = 'normal', onDrumMode, onCentreVfo, vfoLocked = true, onToggleVfoLock, onHideControls, onKeyHelp,
  mediaSkip = 'step', onMediaSkip,
  onDispReset, onDispSaveServer, onDispSaveGlobal,
  hapticsEnabled = false, onHaptics, hapticsHardware = true,
  vfoKeys = false, zoomKeys = false, onVfoKeys, onZoomKeys,
  wheelAction = 'zoom', onWheelAction,
  vtsName = '', vtsFreq,
  onVtsNext, onVtsPrev,
  profiles = [], activeProfileId, sdrUsage = {}, clientCount = 0, onSelectProfile, serverType = 'ubersdr',
  vibeAdminUrl, vibeSetupUrl,
  dabProgrammes = [], activeDabId, onSelectDab, dabSpeed = 1, onDabSpeed,
  searchBookmarks = [], searchBands = [], onSearchTune,
  userBookmarks = [], currentFreq = 0, currentMode = '',
  onAddBookmark, onDeleteBookmark, onExportBookmarks, onImportBookmarks, onPickImportFile,
  onClose, onBack, onLocalHardware, converter, onConverter, radioModel, isTcp, onAdminLink,
  adminSet = false, adminOk = false, adminRefused = false, onAdminUnlock,
  onResetSettings, onReplayTour, onDisplaySettings,
  serverVersion = null, isVibeServer = false, onAbout, onRecordings,
  onZoomIn, onZoomOut, onZoomMin, onZoomMax, onSetDefault, isDefaultInstance = false,
  isFavourite = false, onToggleFavourite,
  decMode = null, decOn = false, onDecToggle,
  spotsKind = null, onSpotsToggle, onSpotsMap,
  rttySettings, onRttySettings,
  wefaxLpm = 120, onWefaxLpm,
  vfoNeedle = '#ffffff', onVfoNeedle,
  vfoIntensity = 5, onVfoIntensity,
  vfoFrost = 0, onVfoFrost,
  bgOpacity = 3, onBgOpacity, hasBgImage = false,
  wfCoarse = 'auto', onWfCoarse,
  autoContrast = 5, onAutoContrast,
  spatialSmooth = true, onSpatialSmooth,
  wfBrightness = 0, onWfBrightness,
  wfContrast = 0, onWfContrast,
  wfSharpness = 5, onWfSharpness,
  specShow = true, onSpecShow,
  specSmoothing = 5, onSpecSmoothing,
  avgFrames = 1, onAvgFrames,
  specFloor = 0, onSpecFloor,
  specPeakScale = 10, onSpecPeakScale,
  peakHold = false, onPeakHold,
  frameRate = '20fps', onFrameRate, wfScroll = 'sharp', onWfScroll,
  smoothTune = true, onSmoothTune, idleSlow = true, onIdleSlow,
  onSpecRatio,
}: MenuSheetProps) {

  const translateY = useRef(new Animated.Value(SHEET_H)).current;
  const [cmapOpen, setCmapOpen] = useState(false);
  /** ★ Cleared the moment it is submitted — this gets typed at a club night with people behind
   *  you, the same reason the server screen's box is dots by default. */
  const [menuAdminPw, setMenuAdminPw] = useState('');
  /** Custom converter LO, as TYPED (MHz). Kept as text so a half-entered "1" is not read as
   *  1 MHz and applied on every keystroke; committed by the UP/DOWN buttons, which is also where
   *  the sign is decided. Seeded from the stored profile so reopening the menu shows what is set. */
  const [convLo, setConvLo] = useState(
    () => converter && converter.offsetHz ? String(Math.abs(converter.offsetHz) / 1e6) : '');

  // Responsive sheet geometry — SHEET_H is a module constant measured in
  // PORTRAIT, so in landscape a bottom-anchored 700pt sheet pokes past the top
  // of the screen (unscrollable) and full-bleed width runs under the Dynamic
  // Island. Recompute per orientation and inset-clear in landscape.
  const { width: winW, height: winH } = useWindowDimensions();
  const sheetInsets = useSafeAreaInsets();
  const isLandscape = winW > winH;
  // Tablets (iPad) have the vertical room for the decoder panel + menu section
  // in landscape; phones don't, so client decoders stay portrait-only there.
  const isTablet = Math.min(winW, winH) >= 768;
  const sheetH = Math.min(winH * 0.88, 700);
  // Inner dropdown scroll lists (search results, profile/DAB/colormap) carry a
  // fixed maxHeight that fits portrait but overflows the much shorter landscape
  // sheet, clipping the bottom of the list. Cap them to the live sheet height.
  const dropMaxH = Math.min(300, Math.max(140, Math.round(sheetH - 180)));
  const sheetW = isLandscape
    ? Math.min(520, winW - sheetInsets.left - sheetInsets.right - 24)
    : undefined;
  const sheetGeom = isLandscape
    ? { height: sheetH, width: sheetW, left: (winW - (sheetW ?? winW)) / 2,
        right: undefined, borderTopLeftRadius: 16, borderTopRightRadius: 16 }
    : { height: sheetH };
  const backdropOp = useRef(new Animated.Value(0)).current;
  const [profileOpen, setProfileOpen] = useState(false);   // OWRX profile dropdown
  const [dabOpen, setDabOpen] = useState(false);           // OWRX DAB programme dropdown
  const isOwrx = serverType === 'owrx';
  const isKiwi = isKiwiProtocol(serverType);   // Web-888 is a Kiwi for every purpose here
  // Local hardware: no server-side maps/admin/CW-skimmer/STT (those are network
  // server features). FT8/FT4 digital spots are decoded locally, so they stay.
  const isLocal = !!onLocalHardware;
  const [dispSettingsOpen, setDispSettingsOpen] = useState(false);

  // ── Keyboard navigation of the sheet ────────────────────────────────────────
  // Up/down between rows, left/right within a row, Enter to activate. The machinery
  // is shared (PanelNav.tsx); attach `scrollRef` to the ScrollView and the buttons
  // register themselves. Esc is NOT handled here — SDRScreen already closes panels
  // on Esc, and one owner for that rule is what keeps the precedence honest.
  // ★ BACKSPACE = ‹ BACK. The sub-panels (Display Settings, Bookmarks, the colour-map
  // and profile dropdowns) each have a BACK row, but a keyboard-only user had no way to
  // reach it — you could get INTO a sub-panel and not out. Read through a ref because the
  // pane states are declared further down; the ref is refreshed every render, so the
  // closure never sees a stale one.
  const paneBack = useRef<() => void>(() => {});
  const kbInUse = useKeyboardMode();
  const fkaSuspected = useFullKeyboardAccessSuspected();
  const { navCtx, scrollProps, scrollRef, scrollY: scrollYRef } = usePanelNav(visible, {
    onBack: () => paneBack.current(),
    onTimeout: onClose,
  });

  // Palette list alphabetised (it ships in table order); profiles are LEFT in
  // server order on purpose — they're SDR-type ordered and re-sorting risks the
  // user tapping the wrong profile and disturbing an SDR in active use.
  const cmapSorted = useMemo(
    () => [...COLORMAP_NAMES].sort((a, b) => a.localeCompare(b, undefined, { sensitivity: 'base' })),
    [],
  );
  // OWRX profiles grouped per SDR (id = `sdrId|profileId`). The SDR name + in-use
  // flag come from /status.json (sdrUsage); profile order is preserved (server
  // order). Each profile label has the SDR-name prefix stripped (the wire name is
  // "{sdrName} {profileName}") so the dropdown reads cleanly under its header.
  const sdrGroups = useMemo(() => {
    const order: string[] = [];
    const byId = new Map<string, { id: string; name: string }[]>();
    for (const p of profiles) {
      const sid = p.id.includes('|') ? p.id.split('|')[0] : p.id;
      if (!byId.has(sid)) { byId.set(sid, []); order.push(sid); }
      byId.get(sid)!.push(p);
    }
    const lcp = (a: string[]) => {
      if (!a.length) return '';
      let pre = a[0];
      for (let i = 1; i < a.length; i++) { let k = 0; while (k < pre.length && k < a[i].length && pre[k] === a[i][k]) k++; pre = pre.slice(0, k); }
      return pre;
    };
    return order.map((sid) => {
      const items = byId.get(sid)!;
      const info = sdrUsage[sid];
      const sdrName = info?.name || lcp(items.map((i) => i.name)).replace(/\s+\S*$/, '').trim() || sid;
      const strip = (n: string) => (sdrName && n.startsWith(sdrName + ' ') ? n.slice(sdrName.length + 1) : n);
      return { sid, sdrName, inUse: !!info?.inUse, activeProfileId: info?.activeProfileId, items: items.map((i) => ({ id: i.id, label: strip(i.name) })) };
    });
  }, [profiles, sdrUsage]);
  // Compact dropdowns: each is a bounded, internally-scrollable box. While one is
  // open the OUTER menu scroll is DISABLED (see scrollEnabled below) so the inner
  // box gets all the gestures — that's what fixes the "outer steals the drag"
  // fight. On open we scroll the inner box to the current row (measureLayout of
  // the active row against the inner ScrollView → content-relative y, works
  // through the per-SDR group wrappers).
  const profScroll = useRef<ScrollView | null>(null);
  const dabScroll  = useRef<ScrollView | null>(null);
  const cmapScroll = useRef<ScrollView | null>(null);
  // content-relative y of each active row (captured via onLayout); the rows are
  // flat children of their ScrollView so onLayout y == scroll offset.
  const profY = useRef<Record<string, number>>({});
  const dabY  = useRef<Record<string, number>>({});
  const cmapY = useRef<Record<string, number>>({});
  // Read the position INSIDE the delay (not before) so onLayout has populated it.
  const openAt = (sv: ScrollView | null, y?: number) => {
    if (sv == null) return;
    setTimeout(() => { if (y != null) sv.scrollTo({ y: Math.max(0, y - 8), animated: false }); }, 60);
  };
  useEffect(() => { if (profileOpen) openAt(profScroll.current, activeProfileId != null ? profY.current[activeProfileId] : undefined); }, [profileOpen, activeProfileId]);
  useEffect(() => { if (dabOpen)     openAt(dabScroll.current,  activeDabId != null ? dabY.current[String(activeDabId)] : undefined); }, [dabOpen, activeDabId]);
  useEffect(() => { if (cmapOpen)    openAt(cmapScroll.current, cmapY.current[colormap]); }, [cmapOpen, colormap]);

  // Bookmarks pane (replaces menu content like DISPLAY SETTINGS)
  const [bookmarksOpen,  setBookmarksOpen]  = useState(false);
  const [bmName,         setBmName]         = useState('');
  const [bmAll,          setBmAll]          = useState(false);
  const [bmImportOpen,   setBmImportOpen]   = useState(false);

  // ★ The colour-map dropdown gets its OWN list nav. Because PanelNav's owner stack is
  // last-in-wins, opening the dropdown automatically takes the arrows away from the menu
  // behind it and gives them back on close — no mode flag, no precedence rule to maintain.
  // Reveal uses the y each row already records via onLayout, so it needs no measurement.
  // ★ When the dropdown OPENS, scroll the menu so the list is actually visible. It expands
  // downward, so opening one near the foot of the sheet left a single row showing with the
  // rest below the fold — you were navigating a list you could not see. (Stuart.)
  const cmapAnchor = useRef<View | null>(null);
  useEffect(() => {
    if (!cmapOpen) return;
    const id = setTimeout(() => revealIn(scrollRef, cmapAnchor, -1, 40, scrollYRef), 80);
    return () => clearTimeout(id);
  }, [cmapOpen]);   // eslint-disable-line react-hooks/exhaustive-deps

  const cmapFocus = useListNav(
    cmapOpen,
    cmapSorted.length,
    (i) => { const n = cmapSorted[i]; if (n) { onColormap(n); setCmapOpen(false); } },
    (i) => {
      const y = cmapY.current[cmapSorted[i]];
      if (y != null) cmapScroll.current?.scrollTo({ y: Math.max(0, y - 40), animated: true });
    },
    undefined,
    // Backspace leaves the list WITHOUT applying a colour; Enter picks and closes.
    () => setCmapOpen(false),
  );

  // Deepest-first, so Backspace peels one layer at a time rather than jumping to the top.
  paneBack.current = () => {
    if (bmImportOpen)     { setBmImportOpen(false); return; }
    if (cmapOpen)         { setCmapOpen(false); return; }
    if (profileOpen)      { setProfileOpen(false); return; }
    if (dabOpen)          { setDabOpen(false); return; }
    if (bookmarksOpen)    { setBookmarksOpen(false); return; }
    if (dispSettingsOpen) { setDispSettingsOpen(false); return; }
    // Nothing nested open — leave the menu itself to Esc, which SDRScreen owns.
  };

  const [bmImportText,   setBmImportText]   = useState('');
  const [bmImportMsg,    setBmImportMsg]    = useState('');
  useEffect(() => {
    if (!visible) {
      setBookmarksOpen(false); setBmImportOpen(false); setBmImportMsg('');
      // Collapse the dropdowns on close — MenuSheet stays mounted (returns null),
      // so an open dropdown would persist and reopen scrolled to the top instead
      // of the current item (the open-at-current effect only fires on open).
      setProfileOpen(false); setDabOpen(false); setCmapOpen(false);
    }
  }, [visible]);

  // Search bookmarks & band plan (skin lsv-mp-bm-input)
  const [searchQuery, setSearchQuery] = useState('');
  const searchResults = useMemo(
    () => searchStations(searchBookmarks, searchBands, searchQuery),
    [searchBookmarks, searchBands, searchQuery],
  );
  useEffect(() => { if (!visible) setSearchQuery(''); }, [visible]);

  useEffect(() => {
    if (visible) {
      Animated.parallel([
        Animated.timing(backdropOp, { toValue: 1, duration: 220, useNativeDriver: true }),
        Animated.spring(translateY, { toValue: 0, damping: 22, stiffness: 200, useNativeDriver: true }),
      ]).start();
    } else {
      Animated.parallel([
        Animated.timing(backdropOp, { toValue: 0, duration: 180, useNativeDriver: true }),
        Animated.timing(translateY, { toValue: SHEET_H, duration: 200, useNativeDriver: true }),
      ]).start();
    }
  }, [visible, backdropOp, translateY]);

  if (!visible) return null;

  return (
    <Modal visible={visible} transparent animationType="none" onRequestClose={onClose}
           supportedOrientations={['portrait', 'landscape', 'landscape-left', 'landscape-right']}>
      <View style={StyleSheet.absoluteFill} onTouchStart={noteTouchInteraction}>
        <TouchableWithoutFeedback onPress={onClose}>
          <Animated.View style={[StyleSheet.absoluteFill, styles.backdrop, { opacity: backdropOp }]} />
        </TouchableWithoutFeedback>

        <Animated.View style={[styles.sheet, sheetGeom, { transform: [{ translateY }] }]}>
          <BlurView intensity={55} tint="dark" style={StyleSheet.absoluteFill} />
          {/* a11y panel is near-opaque (reference bg rgba(6,4,2,0.99)) */}
          <View style={[StyleSheet.absoluteFill, { backgroundColor: 'rgba(6,4,2,0.60)' }]} />
          <View style={styles.handle} />

          <NavCtx.Provider value={navCtx}>
            {/* ★★ Shown when iOS Full Keyboard Access appears to be on — it takes the arrows and
                Tab before they reach us, so nothing here can be navigated. It has to sit at the
                TOP and outside the focus order, because the one thing the user cannot do in this
                state is move a highlight to find an explanation. (Tested on device 2026-07-26.) */}
            {fkaSuspected && (
              <View style={styles.fkaNote}>
                <Text style={styles.fkaNoteTitle}>HOLD SHIFT WITH THE ARROW KEYS</Text>
                <Text style={styles.fkaNoteBody}>
                  iOS Full Keyboard Access looks to be switched on. It claims the plain arrow keys
                  and Tab for its own navigation, so VibeSDR never sees them.{'\n'}
                  Use <Text style={styles.fkaNoteKey}>{'<'}</Text> <Text style={styles.fkaNoteKey}>{'>'}</Text> to
                  tune and <Text style={styles.fkaNoteKey}>-</Text> <Text style={styles.fkaNoteKey}>+</Text> to
                  zoom — they are the arrow keys by another name and work in these menus too. In a
                  text box, or for menus, hold <Text style={styles.fkaNoteKey}>Shift</Text> with an
                  arrow — and <Text style={styles.fkaNoteKey}>Alt/Option (⌥)</Text> with Tab. You do not need
                  to turn anything off.
                </Text>
              </View>
            )}
          <ScrollView {...scrollProps} style={styles.scroll}
            contentContainerStyle={[styles.scrollContent,
              { paddingBottom: sheetInsets.bottom + 16 }]}
            keyboardShouldPersistTaps="handled"
            scrollEnabled={!(profileOpen || dabOpen || cmapOpen)}
            showsVerticalScrollIndicator={false}>

            {/* Display settings is its OWN view — it REPLACES the main menu
                content instead of expanding inline over it (inline blended in
                and was confusing to read). */}
            {!dispSettingsOpen && !bookmarksOpen && (<>

            {/* ── LOCAL HARDWARE (V4 Android — RTL-SDR controls submenu) ── */}
            {onLocalHardware && (<>
              <SectionLabel label="LOCAL HARDWARE" icon="hardware" first />
              {/* ★ The BUTTON has to name the radio too, not just the panel it opens. The
                  header was fixed and this was not, so the menu still announced an RTL-SDR
                  while the panel behind it said Airspy (Stuart, 2026-07-27). */}
              <Btn label={`${radioModel || 'Local SDR'} Controls  \u203a`} full onPress={onLocalHardware} />
            </>)}

            {/* ── CONVERTER (up/down-converter in front of a LOCAL dongle) ────────────────
                ★★★ CONFIGURATION AND STATE IN ONE PLACE, but not collapsed into one control.
                  The preset is set once per dongle and the ENGAGE toggle is flipped whenever the
                  lead moves — an up-converter is inline and people genuinely swap between HF
                  through it and direct VHF by unplugging it. The row always NAMES the profile so
                  that "Converter: on" is never a mystery to someone who set it up weeks ago.
                ★★ Only rendered on the paths where the listener owns the converter — the parent
                  gates it, see converter.ts on why correcting twice is the failure mode. */}
            {converter && onConverter && (<>
              <SectionLabel label="CONVERTER" icon="hardware" />
              <BtnRow>
                {CONVERTER_PRESETS.map(pr => (
                  <Btn key={pr.id} label={pr.label} active={converter.id === pr.id}
                       onPress={() => onConverter(pr.id === 'custom'
                         ? { ...converter, id: 'custom', label: 'Custom' }
                         : fromPreset(pr, converter))} />
                ))}
              </BtnRow>
              <Text style={styles.kbSkipNote}>{presetById(converter.id)?.hint ?? 'Custom converter'}</Text>

              {converter.id === 'custom' && (<>
                {/* ★★ ASK FOR A POSITIVE LO AND A DIRECTION, never for a signed number. The stored
                    field is signed (an up-converter is a down-converter with a negative LO, which
                    is what makes this one transform instead of two features) — but "enter
                    −125000000" is a sign error waiting to happen, and the box in the user's hands
                    is labelled 125 MHz. The sign is applied here, once. */}
                <View style={styles.adminUnlockRow}>
                  <TextInput
                    value={convLo}
                    onChangeText={setConvLo}
                    placeholder="LO frequency in MHz, e.g. 125"
                    placeholderTextColor={C.muted}
                    keyboardType="decimal-pad"
                    style={styles.adminUnlockInput}
                  />
                </View>
                <BtnRow>
                  {/* ★★ THE DIRECTION ALSO SETS WHAT THE CONVERTER PASSES, because that is the
                      half that decides how far the VFO may go. An up-converter takes HF in and
                      carries a low-pass filter to keep everything else out, so the app's own HF
                      ceiling is the honest default. A DOWN-converter's input is its own high band
                      and we cannot guess it — so it declares none, and the range is merely
                      shifted rather than clamped to something invented. */}
                  <Btn label="CONVERTS UP" active={converter.offsetHz <= 0}
                       onPress={() => onConverter({ ...converter,
                         offsetHz: -Math.abs(parseFloat(convLo) || 0) * 1e6, label: 'Custom',
                         inputLoHz: 0, inputHiHz: HF_CEILING_HZ })} />
                  <Btn label="CONVERTS DOWN" active={converter.offsetHz > 0}
                       onPress={() => onConverter({ ...converter,
                         offsetHz: Math.abs(parseFloat(convLo) || 0) * 1e6, label: 'Custom',
                         inputLoHz: 0, inputHiHz: 0 })} />
                </BtnRow>
                <Text style={styles.kbSkipNote}>{V4_WARNING}</Text>
              </>)}

              {/* ★★★ THE ENGAGE TOGGLE, and it is disabled with no profile set rather than hidden:
                  a switch that does nothing is confusing, a missing one reads as a missing
                  feature. `active` shows the CURRENT state, not the action. */}
              {!isIdentity(converter) && (<>
                <BtnRow>
                  <Btn label={converter.engaged ? 'ENGAGED' : 'BYPASSED'} full
                       active={converter.engaged}
                       onPress={() => onConverter({ ...converter, engaged: !converter.engaged })} />
                </BtnRow>
                <Text style={styles.kbSkipNote}>
                  {converter.engaged
                    ? `Tuning is offset by ${(Math.abs(converter.offsetHz) / 1e6)} MHz. Bypass this when you unplug the converter.`
                    : 'Bypassed — the app tunes the dongle directly. Engage this when the converter is plugged in.'}
                </Text>
              </>)}
            </>)}

            {/* PROFILE moved to the FREQUENCY popup: on OWRX a profile IS a frequency
                choice (it's how you pick the band), so it belongs where you change
                frequency — and the menu sheet sitting over the decoder panel made a
                decoding profile look like it produced nothing. See ProfilePicker. */}

            {/* DAB PROGRAMME list + speed control relocated to the DECODER BOX (§4.5/§5.2) —
               it's the "what's on this signal" output, like the other decoders. */}

            {/* NEARBY STATION / VTS skip relocated to FreqModal (§4.1) — it belongs with
               the frequency you're tuning, not in settings. */}


            {/* ── SPECTRUM / WATERFALL ───────────────────────────── */}
            <SectionLabel label="SPECTRUM / WATERFALL" icon="spectrum" />
            {/* Zoom row — flex:1 buttons so they share the width evenly and the
                MAX button never wraps to its own row (the VFO-lock label width
                used to push it over). MIN = full span out, MAX = full zoom in. */}
            <BtnRow>
              <Btn label="MIN"    onPress={onZoomMin} style={{ flex: 1 }} />
              <Btn label="− ZOOM" onPress={onZoomOut} style={{ flex: 1 }} />
              <Btn label="+ ZOOM" onPress={onZoomIn}  style={{ flex: 1 }} />
              <Btn label="MAX"    onPress={onZoomMax} style={{ flex: 1 }} />
            </BtnRow>
            <BtnRow>
              <VfoLockBtn locked={vfoLocked} onPress={onToggleVfoLock} full />
            </BtnRow>
            <BtnRow>
              <Btn label="DISPLAY SETTINGS" icon="monitor" full active={dispSettingsOpen}
                onPress={() => setDispSettingsOpen((p: boolean) => !p)} />
            </BtnRow>
            <BtnRow>
              <Btn label="⌨ KEYBOARD SHORTCUTS" full onPress={onKeyHelp} />
            </BtnRow>
            <BtnRow>
              <Btn label="▼ HIDE CONTROLS" full onPress={onHideControls} />
            </BtnRow>
            </>)}


            {dispSettingsOpen && (
              <View style={styles.subPanel}>

                {/* Back header — this panel replaces the main menu */}
                <TouchableOpacity style={styles.backRow}
                  onPress={() => setDispSettingsOpen(false)} activeOpacity={0.7}>
                  <Text style={styles.backRowChevron}>‹  BACK</Text>
                  <Text style={styles.backRowTitle}>DISPLAY SETTINGS</Text>
                </TouchableOpacity>

                {/* Save row */}
                <BtnRow>
                  <Btn label="↺ RESET"       onPress={onDispReset} />
                  <Btn label="💾 THIS SERVER" onPress={onDispSaveServer} />
                  <Btn label="🌐 GLOBAL"      onPress={onDispSaveGlobal} />
                </BtnRow>

                {/* Layout — spectrum/waterfall ratio */}
                <SubLabel label="Layout" />
                <BtnRow>
                  <Btn label="📐 SPECTRUM / WATERFALL RATIO" full
                    onPress={() => onSpecRatio?.()} />
                </BtnRow>

                {/* Colour Map — dropdown over the FULL palette list. (The old
                    pill strip hardcoded names like 'sonar'/'green' that never
                    existed in the tables → silent gqrx fallback.) */}
                <SubLabel label="Colour Map" />
                {/* ★ The OPENER must be registered too. It was a bare TouchableOpacity, so the
                    arrows walked straight past it and there was no keyboard way to open the
                    colour list at all — which read as "it skips the dropdown". */}
                <View ref={cmapAnchor} collapsable={false}>
                <NavRow><CmapHeaderBtn open={cmapOpen} onPress={() => setCmapOpen((o: boolean) => !o)}>
                <TouchableOpacity style={styles.dropHeader}
                  onPress={() => setCmapOpen((o: boolean) => !o)} activeOpacity={0.7}>
                  <Text style={styles.dropHeaderText}>
                    {colormap === 'gqrx' ? 'GQRX' : colormap.charAt(0).toUpperCase() + colormap.slice(1)}
                  </Text>
                  <Text style={styles.dropChevron}>{cmapOpen ? '▴' : '▾'}</Text>
                </TouchableOpacity>
                </CmapHeaderBtn></NavRow>
                {cmapOpen && (
                  <ScrollView ref={cmapScroll} style={[styles.dropList, { maxHeight: dropMaxH }]} nestedScrollEnabled
                              keyboardShouldPersistTaps="handled">
                    {cmapSorted.map(name => (
                      <TouchableOpacity key={name}
                        style={[styles.dropItem, name === colormap && styles.dropItemActive,
                                cmapFocus === cmapSorted.indexOf(name) && styles.dropItemFocused]}
                        onLayout={e => { cmapY.current[name] = e.nativeEvent.layout.y; }}
                        onPress={() => { onColormap(name); setCmapOpen(false); }}
                        activeOpacity={0.7}>
                        <Text style={[styles.dropItemText, name === colormap && styles.dropItemTextActive]}>
                          {name === 'gqrx' ? 'GQRX' : name.charAt(0).toUpperCase() + name.slice(1)}
                        </Text>
                      </TouchableOpacity>
                    ))}
                  </ScrollView>
                )}
                </View>

                {/* VFO Needle Colour — colour swatches */}
                <SubLabel label="VFO Needle Colour" />
                {/* ★ The swatches are a ROW of choices, so they register as one NavRow and
                    left/right walks them — they were bare Touchables that focus skipped. */}
                <NavRow><View style={styles.swatchRow}>
                  {([
                    {hex:'#ff2020',label:'Red'},    {hex:'#00ff44',label:'Green'},
                    {hex:'#4499ff',label:'Blue'},   {hex:'#ffdd00',label:'Yellow'},
                    {hex:'#00eeff',label:'Cyan'},   {hex:'#ff8800',label:'Orange'},
                    {hex:'#ffffff',label:'White'},  {hex:'#cc44ff',label:'Purple'},
                  ]).map(c => (
                    <SwatchBtn key={c.hex} hex={c.hex} active={vfoNeedle === c.hex}
                               onPress={() => onVfoNeedle?.(c.hex)} />
                  ))}
                </View></NavRow>

                {/* VFO Intensity — needle + glow brightness; bright palettes
                    can swallow the needle whatever colour it is */}
                <View style={styles.bwRow}>
                  <Text style={styles.bwLabel}>VFO GLOW</Text>
                  <NavSlider style={styles.bwSlider}
                    minimumValue={1} maximumValue={10} step={1}
                    value={vfoIntensity}
                    onValueChange={(v: number) => onVfoIntensity?.(v)}
                    minimumTrackTintColor={C.gold} maximumTrackTintColor={C.muted}
                    thumbTintColor={C.gold} />
                  <Text style={styles.bwVal}>{vfoIntensity}</Text>
                </View>

                {/* Frosted backing — dims the waterfall across the passband
                    so the needle keeps contrast on bright palettes */}
                <View style={styles.bwRow}>
                  <Text style={styles.bwLabel}>VFO FROST</Text>
                  <NavSlider style={styles.bwSlider}
                    minimumValue={0} maximumValue={10} step={1}
                    value={vfoFrost}
                    onValueChange={(v: number) => onVfoFrost?.(v)}
                    minimumTrackTintColor={vfoFrost > 0 ? C.gold : C.muted}
                    maximumTrackTintColor={C.muted}
                    thumbTintColor={C.gold} />
                  <Text style={styles.bwVal}>{vfoFrost === 0 ? 'Off' : vfoFrost}</Text>
                </View>

                {/* Instance spectrum backdrop — only shown when the server
                    actually serves one (/api/spectrum-bg-image) */}
                {hasBgImage && (
                  <View style={styles.bwRow}>
                    <Text style={styles.bwLabel}>BACKDROP</Text>
                    <NavSlider style={styles.bwSlider}
                      minimumValue={0} maximumValue={10} step={1}
                      value={bgOpacity}
                      onValueChange={(v: number) => onBgOpacity?.(v)}
                      minimumTrackTintColor={bgOpacity > 0 ? C.gold : C.muted}
                      maximumTrackTintColor={C.muted}
                      thumbTintColor={C.gold} />
                    <Text style={styles.bwVal}>{bgOpacity === 0 ? 'Off' : bgOpacity}</Text>
                  </View>
                )}

                {/* Waterfall — Coarse */}
                <SubLabel label="Waterfall — Coarse" />
                <BtnRow>
                  <Btn label="AUTO"   active={wfCoarse==='auto'}   onPress={() => onWfCoarse?.('auto')} />
                  <Btn label="MANUAL" active={wfCoarse==='manual'} onPress={() => onWfCoarse?.('manual')} />
                </BtnRow>
                {wfCoarse === 'auto' && (
                  <View style={styles.sliderWrap}>
                    <Text style={styles.sliderLabel}>Auto Range</Text>
                    <NavSlider style={{flex:1}} minimumValue={0} maximumValue={20} step={1}
                      value={autoContrast} onValueChange={onAutoContrast ?? (() => {})}
                      minimumTrackTintColor={C.gold} maximumTrackTintColor={C.muted} thumbTintColor={C.gold} />
                    <Text style={styles.sliderVal}>{autoContrast}</Text>
                  </View>
                )}
                {wfCoarse === 'manual' && (
                  <>
                    {/* Manual dB window — floor/ceiling kept ≥5dB apart */}
                    <View style={styles.sliderWrap}>
                      <Text style={styles.sliderLabel}>Floor</Text>
                      <NavSlider style={{flex:1}} minimumValue={-160} maximumValue={-60} step={1}
                        value={Math.min(dbMin, dbMax - 5)}
                        onValueChange={(v: number) => onDbMin?.(Math.min(v, dbMax - 5))}
                        minimumTrackTintColor={C.gold} maximumTrackTintColor={C.muted} thumbTintColor={C.gold} />
                      <Text style={styles.sliderVal}>{dbMin} dB</Text>
                    </View>
                    <View style={styles.sliderWrap}>
                      <Text style={styles.sliderLabel}>Ceiling</Text>
                      <NavSlider style={{flex:1}} minimumValue={-100} maximumValue={0} step={1}
                        value={Math.max(dbMax, dbMin + 5)}
                        onValueChange={(v: number) => onDbMax?.(Math.max(v, dbMin + 5))}
                        minimumTrackTintColor={C.gold} maximumTrackTintColor={C.muted} thumbTintColor={C.gold} />
                      <Text style={styles.sliderVal}>{dbMax} dB</Text>
                    </View>
                  </>
                )}

                {/* Waterfall — Fine */}
                <SubLabel label="Waterfall — Fine" />
                <View style={styles.sliderWrap}>
                  <Text style={styles.sliderLabel}>Brightness</Text>
                  <NavSlider style={{flex:1}} minimumValue={-20} maximumValue={20} step={1}
                    value={wfBrightness} onValueChange={onWfBrightness ?? (() => {})}
                    minimumTrackTintColor={C.gold} maximumTrackTintColor={C.muted} thumbTintColor={C.gold} />
                  <Text style={styles.sliderVal}>{(wfBrightness > 0 ? '+' : '') + wfBrightness} dB</Text>
                </View>
                <View style={styles.sliderWrap}>
                  <Text style={styles.sliderLabel}>Contrast</Text>
                  <NavSlider style={{flex:1}} minimumValue={-10} maximumValue={10} step={1}
                    value={wfContrast} onValueChange={onWfContrast ?? (() => {})}
                    minimumTrackTintColor={C.gold} maximumTrackTintColor={C.muted} thumbTintColor={C.gold} />
                  <Text style={styles.sliderVal}>{(wfContrast > 0 ? '+' : '') + wfContrast}</Text>
                </View>
                <View style={styles.sliderWrap}>
                  <Text style={styles.sliderLabel}>Sharpness</Text>
                  <NavSlider style={{flex:1}} minimumValue={0} maximumValue={10} step={1}
                    value={wfSharpness} onValueChange={onWfSharpness ?? (() => {})}
                    minimumTrackTintColor={C.gold} maximumTrackTintColor={C.muted} thumbTintColor={C.gold} />
                  <Text style={styles.sliderVal}>{wfSharpness}</Text>
                </View>
                <BtnRow>
                  <Btn label="SPATIAL SMOOTH" active={spatialSmooth}
                       onPress={() => onSpatialSmooth?.(!spatialSmooth)} />
                </BtnRow>

                {/* Spectrum Trace */}
                <SubLabel label="Spectrum Trace" />
                <BtnRow>
                  <Btn label="SHOW" active={specShow}  onPress={() => onSpecShow?.(!specShow)} />
                  <Btn label="HIDE" active={!specShow} onPress={() => onSpecShow?.(false)} />
                </BtnRow>
                <View style={styles.sliderWrap}>
                  <Text style={styles.sliderLabel}>Smoothing</Text>
                  <NavSlider style={{flex:1}} minimumValue={1} maximumValue={10} step={1}
                    value={specSmoothing} onValueChange={onSpecSmoothing ?? (() => {})}
                    minimumTrackTintColor={C.gold} maximumTrackTintColor={C.muted} thumbTintColor={C.gold} />
                  <Text style={styles.sliderVal}>{specSmoothing}</Text>
                </View>
                <View style={styles.sliderWrap}>
                  <Text style={styles.sliderLabel}>Floor</Text>
                  <NavSlider style={{flex:1}} minimumValue={-20} maximumValue={20} step={1}
                    value={specFloor} onValueChange={onSpecFloor ?? (() => {})}
                    minimumTrackTintColor={C.gold} maximumTrackTintColor={C.muted} thumbTintColor={C.gold} />
                  <Text style={styles.sliderVal}>{(specFloor > 0 ? '+' : '') + specFloor} dB</Text>
                </View>
                <View style={styles.sliderWrap}>
                  <Text style={styles.sliderLabel}>Peak Scale</Text>
                  <NavSlider style={{flex:1}} minimumValue={1} maximumValue={30} step={1}
                    value={specPeakScale} onValueChange={onSpecPeakScale ?? (() => {})}
                    minimumTrackTintColor={C.gold} maximumTrackTintColor={C.muted} thumbTintColor={C.gold} />
                  <Text style={styles.sliderVal}>{(specPeakScale / 10).toFixed(1)}×</Text>
                </View>
                <BtnRow>
                  <Btn label="PEAK HOLD" active={peakHold} onPress={() => onPeakHold?.(!peakHold)} />
                </BtnRow>

                {/* ★★★ SCROLL SPEED AS A FIXED FRAME-GENERATION MULTIPLIER. Not a frame rate: the
                    number of rows drawn per RECEIVED frame, held CONSTANT. It maps all waterfall
                    history, so anything that changes it rescales the picture — deriving it from the
                    live data rate is what made the waterfall compress and expand. A constant cannot
                    do that. Speed therefore follows the data rate, which is what SHARP always did. */}
                <SubLabel label="Waterfall Speed" />
                <BtnRow>
                  <Btn label="SHARP"   active={wfScroll==='sharp'}   onPress={() => onWfScroll?.('sharp')} />
                  <Btn label="DEFAULT" active={wfScroll==='default'} onPress={() => onWfScroll?.('default')} />
                  <Btn label="SMOOTH"  active={wfScroll==='smooth'}  onPress={() => onWfScroll?.('smooth')} />
                </BtnRow>
                <Text style={{ color: 'rgba(200,210,225,0.55)', fontFamily: 'Atkinson Hyperlegible',
                               fontSize: 11, lineHeight: 15, paddingHorizontal: 4, marginTop: 4 }}>
                  {wfScroll === 'sharp'
                    ? 'One row per received frame — most detail, scrolls at the data rate.'
                    : wfScroll === 'default'
                    ? 'Two rows per frame — a little interpolation, twice the scroll speed.'
                    : 'Four rows per frame — continuous motion on a slow feed, less real detail.'}
                </Text>

                {/* Power saving — IDLE SAVER: ⅓ server frame rate after 30s
                    without touch. (Smooth tune is always on — the 120 Hz boost
                    while interacting; no toggle, to avoid confusion.) */}
                <SubLabel label="Power Saving" />
                <BtnRow>
                  <Btn label="IDLE SAVER"  active={idleSlow}   onPress={() => onIdleSlow?.(!idleSlow)} />
                </BtnRow>

                {/* SIGNAL METER unit — SNR / S-units / dBFS. A DISPLAY choice (it only changes the
                    frequency-pill readout), so it lives with the other display settings — moved here
                    from CONTROLS. Link Management moved OUT to the SERVER section (it's a server control). */}
                {onSignalMode && (<>
                  <SubLabel label="Signal meter" />
                  <BtnRow>
                    {(['snr','smeter','dbfs'] as const).map(sm => (
                      <Btn key={sm} label={sm==='smeter' ? 'S-METER' : sm.toUpperCase()}
                        active={signalMode===sm} onPress={() => onSignalMode?.(sm)} />
                    ))}
                  </BtnRow>
                </>)}

              </View>
            )}

            {!dispSettingsOpen && !bookmarksOpen && (<>


            {/* SERVER MAPS relocated to ModeSelector (§4.4) — same "what's on this
               signal" family as the decoders. */}

            {/* CLIENT DECODERS + SERVER EXTENSIONS / DECODED SPOTS relocated to ModeSelector
               (§4.3) — a decoder rides on the demod, so it belongs in the demodulator menu. */}

            {/* ── CONTROLS ───────────────────────────────────────── */}
            <SectionLabel label="CONTROLS" icon="controls" />
            {/* SIGNAL METER unit moved to DISPLAY SETTINGS (it's a display choice). */}
            {/* DISPLAY STYLE row removed — accessibility skin (white/Atkinson)
                is the single style now; amber/Nixie dropped for readability. */}
            <View style={styles.ctrlRow}>
              <Text style={styles.ctrlLabel}>DRUMS</Text>
              <BtnRow>
                <Btn label="NORMAL"    active={drumMode==='normal'}  onPress={() => onDrumMode?.('normal')} />
                <Btn label="PRECISE"   active={drumMode==='precise'} onPress={() => onDrumMode?.('precise')} />
                {hapticsHardware && (
                  <Btn label="✦ HAPTICS" active={hapticsEnabled}     onPress={() => onHaptics?.(!hapticsEnabled)} />
                )}
              </BtnRow>
            </View>
            {/* Drum or HiFi tuner keys, per control. Deliberately two rows rather
                than one switch: mixing them (keys to tune, drum to zoom) is a
                real preference, and it is also the accessibility route — a
                labelled target for anyone who cannot make a drag gesture. */}
            <View style={styles.ctrlRow}>
              <Text style={styles.ctrlLabel}>TUNE</Text>
              <BtnRow>
                <Btn label="DRUM" active={!vfoKeys} onPress={() => onVfoKeys?.(false)} />
                <Btn label="KEYS" active={vfoKeys}  onPress={() => onVfoKeys?.(true)} />
              </BtnRow>
            </View>
            <View style={styles.ctrlRow}>
              <Text style={styles.ctrlLabel}>ZOOM</Text>
              <BtnRow>
                <Btn label="DRUM" active={!zoomKeys} onPress={() => onZoomKeys?.(false)} />
                <Btn label="KEYS" active={zoomKeys}  onPress={() => onZoomKeys?.(true)} />
              </BtnRow>
            </View>
            {/* ★ ONE question for pointing devices, not a layout matrix. The vertical
                wheel is the only input every device has, so it is the only thing worth
                asking; the OTHER control lands automatically on whatever orthogonal
                axis exists (horizontal wheel, tilt wheel, trackpad left/right). */}
            <View style={styles.ctrlRow}>
              <Text style={styles.ctrlLabel}>WHEEL</Text>
              <BtnRow>
                <Btn label="ZOOM" active={wheelAction === 'zoom'} onPress={() => onWheelAction?.('zoom')} />
                <Btn label="TUNE" active={wheelAction === 'tune'} onPress={() => onWheelAction?.('tune')} />
              </BtnRow>
            </View>
            {/* Lock-screen / car-stereo skip buttons: tune by step, or jump
                bookmarks like the VTS arrows */}
            <View style={styles.ctrlRow}>
              <Text style={styles.ctrlLabel}>MEDIA ⏮⏭</Text>
              <BtnRow>
                <Btn label="TUNE STEP" active={mediaSkip==='step'}
                     onPress={() => onMediaSkip?.('step')} />
                <Btn label="BOOKMARK SKIP" active={mediaSkip==='bookmark'}
                     onPress={() => onMediaSkip?.('bookmark')} />
              </BtnRow>
            </View>

            {/* ── SERVER PAGES — in-app browser view. OWRX bundles map + files
                   gallery (SSTV/WEFAX/Navtex images) + settings, so we link those
                   rather than UberSDR's noise/conditions/listeners pages. ──── */}
            {serverType === 'owrx' ? (<>
              {/* MAP + FILES relocated to the demodulator menu (they're "what's on this
                 signal" content, with the other maps). ADMIN stays — it's server settings. */}
              <SectionLabel label="OPENWEBRX" icon="server" />
              {kbInUse && <Text style={styles.kbSkipNote}>Server pages are skipped on a keyboard — they are the receiver's own, and we cannot guarantee how a keyboard behaves there. Tap to open one.</Text>}
              <BtnRow>
                <Btn label="⚙ ADMIN" full skipNav={kbInUse} onPress={() => onAdminLink?.('/settings', 'Settings')} />
              </BtnRow>
            </>) : (isVibeServer && (adminSet || vibeAdminUrl || vibeSetupUrl)) ? (<>
              {/* ★ A VibeServer's OWN pages. A WebView keeps them automatically in step with the
                    server, which matters while those pages are still moving — rendering them
                    natively would be two implementations drifting apart. */}
              <SectionLabel label="VIBESERVER" icon="admin" />
              {/* ★★ THE WAY IN IS HERE TOO. The section used to appear only once you were ALREADY
                  admin, so the buttons were invisible to the one person who needed them and there
                  was nothing on screen to say a password would reveal them. */}
              {!adminOk && (
                <>
                  <View style={styles.adminUnlockRow}>
                    <TextInput
                      value={menuAdminPw}
                      onChangeText={setMenuAdminPw}
                      placeholder="Admin password"
                      placeholderTextColor="rgba(255,184,51,0.45)"
                      secureTextEntry autoCapitalize="none" autoCorrect={false}
                      style={styles.adminUnlockInput}
                      onSubmitEditing={() => { if (menuAdminPw) { onAdminUnlock?.(menuAdminPw); setMenuAdminPw(''); } }}
                    />
                    <TouchableOpacity
                      style={styles.adminUnlockBtn}
                      disabled={!menuAdminPw}
                      onPress={() => { onAdminUnlock?.(menuAdminPw); setMenuAdminPw(''); }}>
                      <Text style={styles.adminUnlockBtnTxt}>UNLOCK</Text>
                    </TouchableOpacity>
                  </View>
                  <Text style={styles.kbSkipNote}>
                    {adminRefused
                      ? 'That password was not accepted.'
                      : 'The owner\u2019s password unlocks the admin and setup pages, and the hardware controls.'}
                  </Text>
                </>
              )}
              {adminOk && (
                <BtnRow>
                  {!!vibeAdminUrl && (
                    <Btn label="ADMIN" skipNav={kbInUse}
                         onPress={() => onAdminLink?.(vibeAdminUrl, 'Admin')} />
                  )}
                  {!!vibeSetupUrl && (
                    <Btn label="SETUP" skipNav={kbInUse}
                         onPress={() => onAdminLink?.(vibeSetupUrl, 'Setup')} />
                  )}
                </BtnRow>
              )}
            </>) : isLocal || isKiwi ? null : (<>
              <SectionLabel label="SERVER ADMIN" icon="admin" />
              {kbInUse && <Text style={styles.kbSkipNote}>Server pages are skipped on a keyboard — they are the receiver's own, and we cannot guarantee how a keyboard behaves there. Tap to open one.</Text>}
              <BtnRow>
                <Btn label="ADMIN"      skipNav={kbInUse} onPress={() => onAdminLink?.('/admin.html', 'Admin')} />
                <Btn label="NOISE"      skipNav={kbInUse} onPress={() => onAdminLink?.('/noisefloor.html', 'Noise Floor')} />
              </BtnRow>
              <BtnRow>
                <Btn label="CONDITIONS" skipNav={kbInUse} onPress={() => onAdminLink?.('/bandconditions.html', 'Band Conditions')} />
                <Btn label="LISTENERS"  skipNav={kbInUse} onPress={() => onAdminLink?.('/session_stats.html', 'Listeners')} />
              </BtnRow>
            </>)}

            {/* TEMPORARY link-controller diagnostic. */}
            <SubLabel small label={`LINK: ${linkDebug.line}`} />

            <ICloudRow />

            {/* ── INSTANCE ───────────────────────────────────────── */}
            <SectionLabel label="SERVER" icon="instance" />
            <Text style={styles.instanceUrl} numberOfLines={1}>{serverName || serverUrl}</Text>
            {/* AUTO LINK MANAGEMENT — a SERVER control (it changes what the server sends): asks for
                fewer waterfall frames when the link can't carry them, climbs back when it recovers.
                AUTO default; FULL never throttles (may stutter); LOW DATA pins the floor for metered
                connections. The waterfall interpolates regardless, so a lower rate costs TIME
                RESOLUTION, not smooth scrolling. Moved here from Display settings. */}
            {onLinkMode && (<>
              <SubLabel label="Link Management" />
              <BtnRow>
                <Btn label="AUTO"     active={linkMode==='adaptive'} onPress={() => onLinkMode('adaptive')} />
                <Btn label="FULL"     active={linkMode==='full'}     onPress={() => onLinkMode('full')} />
                <Btn label="LOW DATA" active={linkMode==='lowData'}  onPress={() => onLinkMode('lowData')} />
              </BtnRow>
            </>)}
            {/* Back-to-list, Favourite and Set-default moved OUT to the ServersChip
                (top-left of the spectrum) — they're the "which server am I on / how
                do I leave" actions, and burying them here behind a settings-looking
                glyph was exactly what users couldn't find. Reset stays here,
                deliberately away from the quick exit. */}
            <BtnRow col>
              <Btn label="↺ RESET INTERFACE SETTINGS" full danger onPress={onResetSettings} />
            </BtnRow>
            {onRecordings && (
              <BtnRow col>
                <Btn label="⏺ RECORDINGS" full onPress={onRecordings} />
              </BtnRow>
            )}

            {onReplayTour && (
              <BtnRow col>
                <Btn label="❔ REPLAY TUTORIAL" full onPress={onReplayTour} />
              </BtnRow>
            )}

            {/* ── Footer — app version | server type + version. The logo
                identifies WHICH backend this instance runs (UberSDR today;
                KiwiSDR/OpenWebRX later), so it's keyed by server type. ── */}
            <View style={styles.footerRow}>
              <TouchableOpacity onPress={onAbout} hitSlop={8}>
                <Text style={styles.footerBrand}>VibeSDR v{APP_VERSION}</Text>
                <Text style={styles.footerAboutHint}>ABOUT</Text>
              </TouchableOpacity>
              <View style={styles.footerServer}>
                {/* OWRX's logo is a black antenna that vanishes on the dark UI —
                    give it a light chip so it reads. */}
                {isTcp ? (
                  <Image source={require('../../assets/rtltcp.png')}
                    style={[styles.footerLogo, { tintColor: '#cfe3ff' }]} resizeMode="contain" />
                ) : isVibeServer ? (
                  <Image source={SERVER_LOGOS.vibeserver} style={styles.footerLogo} resizeMode="contain" />
                ) : isLocal ? (
                  <View style={styles.footerLogo}>
                    <UsbSdrIcon size={30} color="#cfe3ff" strokeWidth={2.4} />
                  </View>
                ) : (
                  <View style={serverType === 'owrx' ? styles.footerLogoChip : undefined}>
                    <Image source={SERVER_LOGOS[serverType] ?? SERVER_LOGOS.ubersdr} style={styles.footerLogo} resizeMode="contain" />
                  </View>
                )}
                <View>
                  <Text style={styles.footerServerName}>{isTcp ? 'RTL-TCP' : isVibeServer ? 'VibeServer'
                    : isLocal ? 'Local Hardware'
                    : serverLabel ?? (isKiwi ? kiwiFamilyLabel(serverType) : isOwrx ? 'OpenWebRX' : 'UberSDR')}</Text>
                  {/* ★ A VibeServer states its OWN version, which is the number a bug report needs.
                      Silent when the server predates the field — better than a guess. */}
                  {isVibeServer ? (
                    serverVersion
                      ? <Text style={styles.footerServerVer}>v{serverVersion}</Text>
                      : null
                  ) : isLocal ? (
                    <Text style={styles.footerServerVer}>via VibeDSP (native)</Text>
                  ) : serverVersion ? (
                    <Text style={styles.footerServerVer}>v{serverVersion}</Text>
                  ) : null}
                </View>
              </View>
            </View>

            <View style={{ height: 24 }} />
            </>)}
          </ScrollView>
          </NavCtx.Provider>

          <TouchableOpacity
            style={[styles.closeBtn, { marginBottom: sheetInsets.bottom + 12 }]}
            onPress={onClose} hitSlop={8}>
            <Text style={styles.closeBtnText}>CLOSE  ✕</Text>
          </TouchableOpacity>
        </Animated.View>
      </View>
    </Modal>
  );
}

// ── Styles ─────────────────────────────────────────────────────────────────────

const styles = StyleSheet.create({
  adminUnlockRow:   { flexDirection: 'row', alignItems: 'center', gap: 8,
                      paddingHorizontal: 12, marginBottom: 6 },
  adminUnlockInput: { flex: 1, borderWidth: 1, borderColor: 'rgba(255,160,0,0.35)', borderRadius: 6,
                      paddingHorizontal: 10, paddingVertical: 8, color: '#ffb833', fontSize: 14 },
  adminUnlockBtn:   { borderWidth: 1, borderColor: 'rgba(255,160,0,0.55)', borderRadius: 6,
                      paddingHorizontal: 12, paddingVertical: 9 },
  adminUnlockBtnTxt:{ color: '#ffb833', fontSize: 12, letterSpacing: 0.5 },
  backdrop: { backgroundColor: 'rgba(0,0,0,0.55)' },
  sheet: {
    position: 'absolute', bottom: 0, left: 0, right: 0, height: SHEET_H,
    borderTopLeftRadius: 16, borderTopRightRadius: 16,
    overflow: 'hidden', borderTopWidth: 1, borderColor: C.border,
  },
  handle: {
    alignSelf: 'center', width: 40, height: 4, borderRadius: 2,
    backgroundColor: C.border, marginTop: 10, marginBottom: 2,
  },
  scroll:        { flex: 1 },
  scrollContent: { paddingHorizontal: 14, paddingTop: 4 },

  sectionBar: {
    borderTopWidth: StyleSheet.hairlineWidth, borderTopColor: C.divider,
    paddingTop: 12, paddingBottom: 6, marginTop: 6,
  },
  sectionBarFirst: { borderTopWidth: 0, marginTop: 2 },
  sectionRow: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  sectionLabel: {
    color: C.sectionC, fontFamily: 'Atkinson Hyperlegible', fontSize: 12,
    fontWeight: 'bold', letterSpacing: 2,
  },

  footerRow: {
    flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center',
    borderTopWidth: StyleSheet.hairlineWidth, borderTopColor: C.divider,
    marginTop: 14, paddingTop: 14, paddingHorizontal: 2,
  },
  footerBrand: {
    color: C.text, fontFamily: 'Atkinson Hyperlegible', fontSize: 15,
    fontWeight: 'bold', letterSpacing: 1.5,
  },
  footerAboutHint: {
    color: C.sectionC, fontFamily: 'Atkinson Hyperlegible', fontSize: 10,
    letterSpacing: 2, marginTop: 1,
  },
  footerServer: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  footerLogo:   { width: 30, height: 30 },
  footerLogoChip: { backgroundColor: 'rgba(235,235,235,0.92)', borderRadius: 7, padding: 3 },
  footerServerName: {
    color: C.text, fontFamily: 'Atkinson Hyperlegible', fontSize: 13,
    fontWeight: 'bold', letterSpacing: 1, textAlign: 'right',
  },
  footerServerVer: {
    color: C.sliderLabel, fontFamily: 'Atkinson Hyperlegible', fontSize: 11,
    letterSpacing: 1, textAlign: 'right', marginTop: 1,
  },

  btnRow:    { flexDirection: 'row', flexWrap: 'wrap', gap: 6, paddingVertical: 4 },
  btnRowCol: { flexDirection: 'column', gap: 6 },
  optRow:    { paddingTop: 2, paddingBottom: 0 },

  // a11y .lsv-mp-btn: 15px text, 11×16 padding
  btn: {
    backgroundColor: C.btnBg, borderWidth: 1, borderColor: C.border,
    borderRadius: 5, paddingHorizontal: 16, paddingVertical: 11,
    alignItems: 'center', justifyContent: 'center',
  },
  btnActive:     { backgroundColor: C.active, borderColor: C.goldDim },
  // Keyboard focus ring — deliberately distinct from ACTIVE (which means "this
  // setting is on"). Focus is where the keyboard is, not what is selected.
  btnFocused:    { borderColor: C.focus, borderWidth: 2 },
  // Amber rather than the focus green: this is an explanation of why the green is not moving.
  fkaNote:      { borderWidth: 1, borderColor: C.goldDim, borderRadius: 6,
                  backgroundColor: 'rgba(60,40,0,0.5)', padding: 10, marginBottom: 10 },
  fkaNoteTitle: { color: C.gold, fontSize: 11, letterSpacing: 1, marginBottom: 5 },
  fkaNoteBody:  { color: C.muted, fontSize: 11, lineHeight: 16 },
  fkaNoteKey:   { color: C.gold, fontWeight: '700' },
  // Dim and small: an explanation, not a warning — nothing has gone wrong.
  kbSkipNote:    { color: C.sectionC, fontSize: 10, lineHeight: 14, paddingHorizontal: 2, paddingBottom: 6, opacity: 0.85 },
  dropHeaderFocused: { borderWidth: 2, borderColor: C.focus, borderRadius: 5, margin: -2 },
  // Dropdown rows are a dense list with only a divider, so focus is a background tint —
  // a 2px border would shift every row as focus moved down it.
  dropItemFocused: { backgroundColor: 'rgba(124,255,155,0.18)' },
  btnSelected:   { borderColor: C.goldDim }, // selected but not running (skin)
  btnDanger:     { backgroundColor: C.danger, borderColor: C.dangerBorder },
  btnFull:       { flex: 1, alignSelf: 'stretch' },
  // Colour map dropdown
  dropHeader: {
    flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center',
    backgroundColor: C.btnBg, borderWidth: 1, borderColor: C.border,
    borderRadius: 4, paddingHorizontal: 12, paddingVertical: 9, marginVertical: 4,
  },
  dropHeaderText: { color: C.text, fontFamily: 'Atkinson Hyperlegible', fontSize: 15, fontWeight: 'bold', letterSpacing: 0.5 },
  dropChevron:    { color: C.muted, fontSize: 10 },
  dropList: {
    borderWidth: 1, borderColor: C.border, borderRadius: 4, maxHeight: 240,
    marginBottom: 6, overflow: 'hidden', backgroundColor: 'rgba(0,0,0,0.25)',
  },
  dropItem: {
    paddingHorizontal: 12, paddingVertical: 8,
    borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: C.border,
  },
  dropItemActive:     { backgroundColor: C.active },
  dropItemText:       { color: C.muted, fontFamily: 'Atkinson Hyperlegible', fontSize: 15, letterSpacing: 0.5 },
  dropItemTextActive: { color: C.gold, fontWeight: 'bold' },

  btnText:       { color: C.muted, fontFamily: 'Atkinson Hyperlegible', fontSize: 15, fontWeight: 'bold', letterSpacing: 0.5 },
  btnTextActive: { color: C.gold },
  btnTextDanger: { color: '#ff6666' },

  profileDrop: { paddingVertical: 6 },
  profileDropHead: {
    flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between',
    paddingHorizontal: 12, paddingVertical: 9, borderRadius: 8,
    borderWidth: 1, borderColor: C.border, backgroundColor: C.btnBg,
  },
  profileDropHeadText: { color: C.text, fontFamily: 'Atkinson Hyperlegible', fontSize: 14, flex: 1 },
  profileDropChevron: { color: C.muted, fontSize: 14, marginLeft: 8 },
  profileDropList: {
    marginTop: 4, borderRadius: 8, borderWidth: 1, borderColor: C.border, maxHeight: 300,
    backgroundColor: C.btnBg, overflow: 'hidden',
  },
  profileDropItem: { paddingHorizontal: 12, paddingVertical: 10, borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: C.divider },
  profileDropItemSub: { paddingLeft: 22, paddingRight: 12, paddingVertical: 10, borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: C.divider },
  profileDropItemText: { color: C.text, fontFamily: 'Atkinson Hyperlegible', fontSize: 14 },
  profileChipTextActive: { color: C.gold },
  profileItemInUse: { backgroundColor: 'rgba(255,184,77,0.10)' },
  profileTextInUse: { color: '#ffb84d' },
  sdrHeadRow: { flexDirection: 'row', alignItems: 'center', gap: 8, paddingHorizontal: 12, paddingTop: 11, paddingBottom: 6, backgroundColor: 'rgba(255,255,255,0.04)' },
  sdrHeadText: { flexShrink: 1, color: C.sectionC, fontFamily: 'Atkinson Hyperlegible', fontSize: 12, fontWeight: '700', letterSpacing: 0.4 },
  sdrBadge: { fontFamily: 'Atkinson Hyperlegible', fontSize: 9, fontWeight: '700', letterSpacing: 0.5, overflow: 'hidden', borderRadius: 3, paddingHorizontal: 5, paddingVertical: 2, color: '#0a0a0a' },
  sdrBadgeInUse: { backgroundColor: '#ffb84d' },     // amber — busy, switching may disturb
  sdrBadgeCurrent: { backgroundColor: '#52dc64' },   // green — this is where you are
  etiquette: { marginBottom: 8, paddingHorizontal: 10, paddingVertical: 8, borderRadius: 6, borderWidth: 1, borderColor: 'rgba(255,184,77,0.45)', backgroundColor: 'rgba(255,184,77,0.10)' },
  etiquetteText: { color: 'rgba(255,225,180,0.92)', fontFamily: 'Atkinson Hyperlegible', fontSize: 11.5, lineHeight: 16 },
  etiquetteLead: { color: '#ffb84d', fontWeight: '700' },
  dabSpeedLabel: { color: C.muted, fontFamily: 'Atkinson Hyperlegible', fontSize: 11, marginTop: 8, marginBottom: 4 },
  dabSpeedRow: { flexDirection: 'row', gap: 8 },
  dabSpeedChip: {
    flex: 1, alignItems: 'center', paddingVertical: 8, borderRadius: 8,
    borderWidth: 1, borderColor: C.border, backgroundColor: C.btnBg,
  },
  dabSpeedChipActive: { borderColor: C.active },
  dabSpeedChipText: { color: C.text, fontFamily: 'Atkinson Hyperlegible', fontSize: 13 },
  vtsRow: { flexDirection: 'row', alignItems: 'center', gap: 8, paddingVertical: 6 },
  vtsArrow: {
    backgroundColor: C.btnBg, borderWidth: 1, borderColor: C.border,
    borderRadius: 4, paddingHorizontal: 14, paddingVertical: 10,
    alignItems: 'center', justifyContent: 'center',
  },
  vtsArrowText: { color: C.gold, fontSize: 18 },
  vtsInfo:  { flex: 1, alignItems: 'center', gap: 3 },
  vtsName:  { color: C.text, fontFamily: 'Atkinson Hyperlegible', fontSize: 14, letterSpacing: 1 },
  vtsFreq:  { color: C.sectionC, fontFamily: 'Atkinson Hyperlegible', fontSize: 11, letterSpacing: 1 },
  searchWrap: { paddingTop: 6, paddingBottom: 2 },
  bmEmpty: {
    color: 'rgba(255,255,255,0.45)', fontFamily: 'Atkinson Hyperlegible',
    fontSize: 12, paddingVertical: 6,
  },
  bmRow: {
    flexDirection: 'row', alignItems: 'center', gap: 10,
    paddingVertical: 8,
    borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: 'rgba(255,255,255,0.12)',
  },
  bmTune: { flex: 1, flexDirection: 'row', alignItems: 'baseline', gap: 8 },
  bmName: { flexShrink: 1, color: C.text, fontFamily: 'Atkinson Hyperlegible', fontSize: 14 },
  bmKey: {
    flexDirection: 'row', alignItems: 'center', gap: 6,
    paddingBottom: 6,
  },
  bmKeyDot: { width: 9, height: 9, borderRadius: 4.5, flexShrink: 0 },
  bmKeyTxt: {
    color: 'rgba(255,255,255,0.55)', fontFamily: 'Atkinson Hyperlegible',
    fontSize: 11, marginRight: 10,
  },
  bmFreq: { flexShrink: 0, color: '#ffe566', fontFamily: 'Atkinson Hyperlegible', fontSize: 12 },
  bmDel:  { color: 'rgba(220,80,80,0.85)', fontSize: 16, paddingHorizontal: 6 },
  bmImportBox: { minHeight: 90, textAlignVertical: 'top', marginTop: 4 },
  bmImportMsg: {
    color: 'rgba(120,235,140,0.90)', fontFamily: 'Atkinson Hyperlegible',
    fontSize: 12, paddingVertical: 4,
  },
  searchInput: {
    backgroundColor: 'rgba(255,255,255,0.06)',
    borderWidth: 1, borderColor: 'rgba(255,255,255,0.30)', borderRadius: 8,
    color: C.text, fontFamily: 'Atkinson Hyperlegible', fontSize: 14,
    paddingHorizontal: 12, paddingVertical: 9,
  },
  searchDrop: {
    marginTop: 4, borderWidth: 1, borderColor: 'rgba(255,255,255,0.22)',
    borderRadius: 8, backgroundColor: 'rgba(0,0,0,0.45)', overflow: 'hidden',
  },
  searchScroll: { maxHeight: 240 },
  searchHint: {
    color: 'rgba(255,255,255,0.45)', fontFamily: 'Atkinson Hyperlegible',
    fontSize: 11, paddingHorizontal: 10, paddingVertical: 6,
  },
  searchMsg: {
    color: 'rgba(255,255,255,0.55)', fontFamily: 'Atkinson Hyperlegible',
    fontSize: 13, paddingHorizontal: 10, paddingVertical: 10,
  },
  searchRow: {
    flexDirection: 'row', alignItems: 'center', gap: 8,
    paddingHorizontal: 10, paddingVertical: 9,
    borderTopWidth: StyleSheet.hairlineWidth, borderTopColor: 'rgba(255,255,255,0.12)',
  },
  searchFreq: {
    color: '#ffe566', fontFamily: 'Atkinson Hyperlegible', fontSize: 12,
    width: 92,
  },
  searchMode: {
    color: 'rgba(255,255,255,0.50)', fontFamily: 'Atkinson Hyperlegible',
    fontSize: 11, width: 42,
  },
  searchName: {
    flex: 1, color: C.text, fontFamily: 'Atkinson Hyperlegible', fontSize: 13,
  },
  searchRpt: { fontSize: 11 },

  sliderRow:   { paddingVertical: 4, gap: 4 },
  sliderLabel: { color: C.sliderLabel, fontFamily: 'Atkinson Hyperlegible', fontSize: 13, letterSpacing: 1, width: 90, flexShrink: 0 },
  bwRow:    { flexDirection: 'row', alignItems: 'center', gap: 6, paddingVertical: 2 },
  bwMirrorRow:  { flexDirection: 'row', alignItems: 'center', gap: 4, paddingVertical: 2 },
  bwHalfSlider: { flex: 1, height: 32 },
  bwEdgeVal:    { color: C.gold, fontFamily: 'Atkinson Hyperlegible', fontSize: 10, minWidth: 44, textAlign: 'center' },
  bwLabel:  { color: C.sectionC, fontFamily: 'Atkinson Hyperlegible', fontSize: 11, letterSpacing: 1, width: 32 },
  bwSlider: { flex: 1, height: 32 },
  bwVal:    { color: C.gold, fontFamily: 'Atkinson Hyperlegible', fontSize: 11, minWidth: 68, textAlign: 'right' },
  sliderWrap:  { flexDirection: 'row', alignItems: 'center', gap: 8, flex: 1 },
  sliderVal:   { color: C.text, fontFamily: 'Atkinson Hyperlegible', fontSize: 14, minWidth: 72, textAlign: 'right' },

  stepSlider: { flexDirection: 'row', alignItems: 'center', gap: 8, paddingVertical: 4 },
  stepSliderBtn: {
    backgroundColor: C.btnBg, borderWidth: 1, borderColor: C.border,
    borderRadius: 4, width: 32, height: 32, alignItems: 'center', justifyContent: 'center',
  },
  stepSliderBtnTxt: { color: C.gold, fontSize: 18, fontWeight: 'bold', lineHeight: 22 },
  stepSliderVal: { color: C.gold, fontFamily: 'Atkinson Hyperlegible', fontSize: 12, flex: 1, textAlign: 'center' },

  subPanel: {
    backgroundColor: 'rgba(255,255,255,0.04)', borderRadius: 6,
    borderWidth: StyleSheet.hairlineWidth, borderColor: C.divider,
    padding: 10, marginBottom: 4,
  },
  subLabel:      { color: C.sectionC, fontFamily: 'Atkinson Hyperlegible', fontSize: 12, letterSpacing: 1, paddingTop: 8, paddingBottom: 3 },
  subLabelSmall: { fontSize: 10, opacity: 0.5 },

  // Display-settings back header
  backRow: {
    flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between',
    backgroundColor: C.btnBg, borderWidth: 1, borderColor: C.border,
    borderRadius: 5, paddingHorizontal: 14, paddingVertical: 11, marginBottom: 8,
  },
  backRowChevron: { color: C.gold, fontFamily: 'Atkinson Hyperlegible', fontSize: 15, fontWeight: 'bold' },
  backRowTitle:   { color: C.text, fontFamily: 'Atkinson Hyperlegible', fontSize: 15, fontWeight: 'bold', letterSpacing: 1 },

  recTimer: { flexDirection: 'row', alignItems: 'center', gap: 8, paddingVertical: 4 },
  recDot:   { width: 8, height: 8, borderRadius: 4, backgroundColor: '#cc2222' },
  recTime:  { color: C.gold, fontFamily: 'Atkinson Hyperlegible', fontSize: 13 },
  dspError: { color: 'rgba(220,53,69,0.95)', fontFamily: 'Atkinson Hyperlegible', fontSize: 13, paddingBottom: 6 },

  ctrlRow:   { paddingVertical: 4, gap: 4 },
  ctrlLabel: { color: C.sectionC, fontFamily: 'Atkinson Hyperlegible', fontSize: 10, letterSpacing: 1.5 },

  swatchRow:  { flexDirection: 'row', flexWrap: 'wrap', gap: 12, paddingVertical: 4 },
  swatch:     { width: 32, height: 32, borderRadius: 16, borderWidth: 3, borderColor: 'transparent' },
  swatchActive: { borderColor: '#fff' },
  cmapStrip:          { gap: 6, flexDirection: 'row', paddingBottom: 4 },
  cmapPill:           { backgroundColor: C.btnBg, borderWidth: 1, borderColor: C.border, borderRadius: 4, paddingHorizontal: 8, paddingVertical: 3 },
  cmapPillActive:     { backgroundColor: C.active, borderColor: C.gold },
  cmapPillText:       { color: C.muted, fontFamily: 'Atkinson Hyperlegible', fontSize: 11 },
  cmapPillTextActive: { color: C.gold },

  instanceUrl: { color: 'rgba(255,255,255,0.40)', fontFamily: 'Atkinson Hyperlegible', fontSize: 11, paddingBottom: 4 },

  closeBtn: {
    margin: 12, alignSelf: 'center', backgroundColor: C.btnBg,
    borderWidth: 1, borderColor: C.border, borderRadius: 6,
    paddingHorizontal: 24, paddingVertical: 8,
  },
  closeBtnText: { color: C.goldDim, fontFamily: 'Atkinson Hyperlegible', fontSize: 12, fontWeight: 'bold', letterSpacing: 1 },
});
