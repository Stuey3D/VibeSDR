/**
 * DecoderPanel — floating panel above the control bar.
 *
 * Appears when a decoder is active. Positioned dynamically:
 *   bottom = pillBottom + 8  (passed as prop from SDRScreen)
 *
 * Header row: status dot · decoder title · decoder type buttons (scrollable) · status text · ✕
 * Body: scrollable text output, character-drip style from decoder service.
 * Tap header to minimise/restore. ✕ to close.
 *
 * Matches VibeSDR_Mockup_SAVE.html #lsv-decoder-panel exactly.
 */

import React, { useCallback, useEffect, useRef, useState } from 'react';
import {
  Animated,
  FlatList,
  ScrollView,
  StyleSheet,
  Text,
  TouchableOpacity,
  View,
} from 'react-native';
import { useTheme } from '../contexts/ThemeContext';
import { NativeEventEmitter, NativeModules } from 'react-native';
import { NAV_FOCUS, captureRegion, useAnnounce, useKeyboardMode, noteTouchInteraction, useRepeatingKeys, NAV_REPEAT_KEYS, PANEL_IDLE_MS } from './PanelNav';
import DecoderImageCanvas, { type DecoderImageHandle } from './DecoderImageCanvas';
import { type MorseQuality, type SpotRow, type SpotsKind } from '../services/DecoderClient';
import { abbrCountry } from '../assets/countryAbbr';
import AircraftPanel from './AircraftPanel';
import StationLogo from './StationLogo';
import type { Aircraft } from '../services/SDRBackend';

// ── Types ──────────────────────────────────────────────────────────────────────

export type DecoderType = 'rtty' | 'navtex' | 'wefax' | 'sstv' | 'morse' | 'whisper' | 'ft8' | null;
const IMAGE_DECODERS: DecoderType[] = ['wefax', 'sstv'];

export interface DecoderPanelProps {
  activeDecoder: DecoderType;
  decoderText:   string;
  /** ADS-B: the structured aircraft table. When present it REPLACES the text body —
   *  the records carry far more than the flattened "CALLSIGN 39700ft 434kt -25dB"
   *  line ever could (registry country, vertical trend, range and bearing). */
  aircraft?:     Aircraft[];
  decoderStatus: string;   // 'listening…' | 'decoding…' | custom
  decoding:      boolean;  // true = green dot
  bottomOffset:  number;   // distance from bottom of screen (pillTop - 8)
  /** Clear the text output (skin CLR — text decoders only). */
  onClear?:      () => void;
  onClose:       () => void;
  /** Image canvas (WEFAX/SSTV) — SDRScreen drives lines via this ref. */
  imageRef?:      React.RefObject<DecoderImageHandle | null>;
  /** Canvas status messages ("done — tap SAVE") → SDRScreen decoderStatus. */
  onImageStatus?: (s: string) => void;
  /** Morse quality filter (skin header dropdown — cycles ALL/LOW+/MED+/HIGH). */
  morseQuality?:   MorseQuality;
  onMorseQuality?: (q: MorseQuality) => void;
  /** Digital/CW spots mode — when set, the panel shows the spots table. */
  spotsKind?:      SpotsKind | null;
  spots?:          SpotRow[];
  onTuneHz?:       (hz: number) => void;
  /** OWRX DAB (§5.2): when an ensemble is tuned the panel shows the service list with logos;
   *  the speed-correction control (§4.5) rides in the header. NOT a decoder. */
  dabProgrammes?:  { id: number; name: string }[];
  /** Ensemble (multiplex) label from OWRX's `ensemble_label`. Shown in the header
   *  beside DAB, mirroring the watch's DabView. Empty until the ensemble decodes. */
  dabEnsemble?:    string;
  activeDabId?:    number;
  onSelectDab?:    (id: number) => void;
  dabSpeed?:       number;
  onDabSpeed?:     (v: number) => void;
}

const DAB_SPEEDS = [
  { v: 1, l: 'Off' }, { v: 0.6667, l: '×0.67' }, { v: 0.5, l: '×0.5' },
  { v: 0.3333, l: '×0.33' }, { v: 0.25, l: '×0.25' },
];

const MORSE_QUALITIES: MorseQuality[] = ['all', 'low', 'medium', 'high'];
const MORSE_QUALITY_LABELS: Record<MorseQuality, string> = {
  all: 'ALL', low: 'LOW+', medium: 'MED+', high: 'HIGH',
};

// Spots filters (skin lsv-dec-sf-mode / sf-band / sf-age)
const SF_MODES = ['ALL', 'FT8', 'FT4', 'WSPR', 'JS8'];
const SF_BANDS = ['ALL', '160m', '80m', '60m', '40m', '30m', '20m', '17m', '15m', '12m', '10m'];
const SF_AGES: Array<{ label: string; minutes: number }> = [
  { label: 'AGE', minutes: 0 }, { label: '15m', minutes: 15 },
  { label: '30m', minutes: 30 }, { label: '1h', minutes: 60 },
];

function fmtSpotTime(t: number): string {
  const d = new Date(t);
  return String(d.getUTCHours()).padStart(2, '0') + ':' +
         String(d.getUTCMinutes()).padStart(2, '0');
}

// Expanded line 2 wants the exact instant — FT8 lives on 15-second slots, so minutes alone
// cannot tell two transmissions in the same minute apart.
function fmtSpotTimeSec(t: number): string {
  const d = new Date(t);
  return String(d.getUTCHours()).padStart(2, '0') + ':' +
         String(d.getUTCMinutes()).padStart(2, '0') + ':' +
         String(d.getUTCSeconds()).padStart(2, '0') + 'z';
}

// Memoized spot row — with FlatList virtualization only the ~12 visible rows
// render, and unchanged rows skip re-render entirely when new spots flush in.
const SpotRowView = React.memo(function SpotRowView({ s, isCW, font, callColor, onTuneHz, expanded }: {
  s: SpotRow; isCW: boolean; font: string; callColor: string; expanded: boolean;
  onTuneHz?: (hz: number) => void;
}) {
  // Line 1: Time · Call · Band · Mode · SNR · Country · Distance.
  //
  // Call moved from fifth to second (2026-07-22): it is the IDENTITY of the row, and reading it
  // after three metadata cells is backwards.
  //
  // Line 2 (expanded only) carries the MESSAGE. The original columns were chosen for DX hunting —
  // distance, country, grid — and the message was left out, but a user watching FT8 pointed out
  // that the message is what tells you whether you are seeing a CQ, a signal report, or a QSO in
  // progress. Distance answers "how far"; the message answers "what is happening".
  return (
    <TouchableOpacity style={expanded ? dp.spotRowTall : dp.spotRow}
      onPress={() => s.freqHz && onTuneHz?.(s.freqHz)} activeOpacity={0.6}>
      <View style={dp.spotLine}>
        <Text style={[dp.spotCell, dp.spotTime, { fontFamily: font }]}>
          {fmtSpotTime(s.time)}
        </Text>
        <Text style={[dp.spotCell, dp.spotCall, { color: callColor, fontFamily: font }]}
              numberOfLines={1}>
          {s.call}
        </Text>
        <Text style={[dp.spotCell, dp.spotBand, { fontFamily: font }]}>{s.band}</Text>
        <Text style={[dp.spotCell, dp.spotMode, { fontFamily: font }]}>
          {isCW ? (s.wpm ? Math.round(s.wpm) + 'w' : 'CW') : s.mode}
        </Text>
        <Text style={[dp.spotCell, dp.spotSnr,
          { color: (s.snr ?? -99) >= 0 ? '#55d98d' : 'rgba(255,160,0,0.65)', fontFamily: font }]}>
          {s.snr !== undefined ? s.snr : ''}
        </Text>
        <Text style={[dp.spotCell, dp.spotCountry, { fontFamily: font }]} numberOfLines={1}>
          {abbrCountry(s.country)}
        </Text>
        {/* Distance-to-receiver (FT8: from the TX grid). Its own cell so country +
            distance both show; blank when unknown.
            ★ ROUND IT. UberSDR sends distance_km as a raw float, so this printed
            "8231.437291841km" and blew the column apart. Sub-kilometre precision is
            meaningless anyway — a Maidenhead grid square is ~100 km across, so every
            digit after the point is invented. */}
        <Text style={[dp.spotCell, dp.spotDist, { fontFamily: font }]} numberOfLines={1}>
          {s.distKm != null ? `${Math.round(s.distKm)}km` : ''}
        </Text>
      </View>
      {expanded && (
        // Dimmer than line 1 on purpose: this is context to READ, not a scan target. Parts are
        // joined with · and any missing piece simply drops out — a report or a 73 carries no
        // locator, so the row must not leave a gap where the grid would have been.
        <Text style={[dp.spotDetail, { fontFamily: font }]} numberOfLines={1}>
          {[
            s.msg,
            s.grid,
            s.bearing != null ? `${Math.round(s.bearing)}°` : undefined,
            fmtSpotTimeSec(s.time),
          ].filter(Boolean).join(' · ')}
        </Text>
      )}
    </TouchableOpacity>
  );
});

const DECODER_LABELS: Record<NonNullable<DecoderType>, string> = {
  rtty:    'RTTY',
  navtex:  'NAVTEX',
  wefax:   'WEFAX',
  sstv:    'SSTV',
  morse:   'CW/MORSE',
  whisper: 'SPEECH',
  ft8:     'FT8',
};

const C = {
  bg:       'rgba(10,8,4,0.95)',
  border:   'rgba(255,160,0,0.28)',
  gold:     '#ffb833',
  goldDim:  'rgba(255,160,0,0.70)',
  muted:    'rgba(255,160,0,0.38)',
  hdrBdr:   'rgba(255,160,0,0.12)',
  btnBdr:   'rgba(255,160,0,0.28)',
  btnAct:   'rgba(255,160,0,0.12)',
  dotIdle:  'rgba(255,160,0,0.35)',
  dotOn:    '#55d98d',
  outputCl: '#ffe566',
  closeCl:  'rgba(255,100,100,0.70)',
};
const FONT = 'Atkinson Hyperlegible';

// ── Component ──────────────────────────────────────────────────────────────────

export default function DecoderPanel({
  activeDecoder, decoderText, aircraft, decoderStatus, decoding,
  bottomOffset, onClear, onClose,
  imageRef, onImageStatus,
  morseQuality = 'all', onMorseQuality,
  spotsKind = null, spots = [], onTuneHz,
  dabProgrammes = [], dabEnsemble = '', activeDabId, onSelectDab, dabSpeed = 1, onDabSpeed,
}: DecoderPanelProps) {
  const isDabMode = dabProgrammes.length > 0;
  const isSpotsMode = !isDabMode && spotsKind !== null;
  const isImageMode = !isSpotsMode && !isDabMode && IMAGE_DECODERS.includes(activeDecoder);
  const isAircraftMode = !isSpotsMode && !isDabMode && !!aircraft?.length;

  // Spots filters — header cyclers (skin sf-mode/sf-band/sf-age)
  const [sfMode, setSfMode] = useState('ALL');
  const [sfBand, setSfBand] = useState('ALL');
  const [sfAge,  setSfAge]  = useState(0);
  // Collapsed is the DEFAULT and stays the one-line row it has always been: on an SE that is the
  // difference between a dozen spots on screen and five. Expanding opens every row to two lines at
  // once — a per-row disclosure would mean hunting for the arrow on the row you care about.
  const [spotsExpanded, setSpotsExpanded] = useState(false);
  const visibleSpots = React.useMemo(() => {
    if (!isSpotsMode) return [];
    const cutoff = sfAge > 0 ? Date.now() - sfAge * 60_000 : 0;
    return spots.filter(s =>
      (spotsKind === 'cw' || sfMode === 'ALL' || s.mode === sfMode) &&
      (sfBand === 'ALL' || s.band === sfBand) &&
      (cutoff === 0 || s.time >= cutoff));
  }, [isSpotsMode, spots, spotsKind, sfMode, sfBand, sfAge]);

  const { theme: themeForRows } = useTheme();
  const renderSpot = useCallback(({ item }: { item: SpotRow }) => (
    <SpotRowView s={item} isCW={spotsKind === 'cw'} font={themeForRows.font}
                 callColor={C.gold}
                 onTuneHz={onTuneHz} expanded={spotsExpanded} />
  ), [spotsKind, themeForRows, onTuneHz, spotsExpanded]);
  // Canvas header state — fed by DecoderImageCanvas callbacks (skin parity)
  const [imageInfo,   setImageInfo]   = useState('');
  const [hasPrev,     setHasPrev]     = useState(false);
  const [viewingPrev, setViewingPrev] = useState(false);
  const onTogglePrev = () => {
    if (viewingPrev) imageRef?.current?.showLive();
    else             imageRef?.current?.showPrev();
  };
  const onSave = () => { imageRef?.current?.save(); };
  const { theme: t } = useTheme();
  const isWhite = t.name === 'white';
  const [minimised, setMinimised] = useState(false);
  const [dabSpeedOpen, setDabSpeedOpen] = useState(false);   // DAB speed-fix popup

  // ★ Expanding changes every row's height at once, so any preserved offset points somewhere
  // different afterwards. Going to the top is a DEFINED position rather than a guessed one —
  // and spots are newest-first, so the top is where you would want to be anyway.
  const didMountSpots = useRef(false);
  useEffect(() => {
    if (!didMountSpots.current) { didMountSpots.current = true; return; }
    bodyScrollY.current = 0;
    requestAnimationFrame(() => spotsRef.current?.scrollToOffset?.({ offset: 0, animated: false }));
  }, [spotsExpanded]);
  const opacity  = useRef(new Animated.Value(0)).current;
  const slideY   = useRef(new Animated.Value(20)).current;
  const outputRef = useRef<ScrollView>(null);
  const aircraftRef = useRef<ScrollView | null>(null);
  const spotsRef = useRef<any>(null);
  const bodyScrollY = useRef(0);
  // ★ Offset tracked in a REF, never state. The removed green bar kept it in state and set it
  // on every scroll frame — and onContentSizeChange fired when a spot row expanded, which is
  // the likely source of the list scrolling wildly on EXPAND. A ref costs nothing and cannot
  // feed back into a render.
  //
  // Reading the real offset also keeps arrow-scrolling honest: a counter of our own drifts the
  // moment the list is flicked by hand or clamps at its end, and then the arrows appear dead
  // until you press them back through the difference.
  const bodyScroll = {
    scrollEventThrottle: 32,
    onScroll: (e: any) => { bodyScrollY.current = e?.nativeEvent?.contentOffset?.y ?? 0; },
  };

  const dc = {
    border:  isWhite ? 'rgba(255,255,255,0.25)' : C.border,
    hdrBdr:  isWhite ? 'rgba(255,255,255,0.10)' : C.hdrBdr,
    title:   isWhite ? 'rgba(255,255,255,0.65)' : C.goldDim,
    status:  isWhite ? 'rgba(255,255,255,0.38)' : C.muted,
    btnBdr:  isWhite ? 'rgba(255,255,255,0.25)' : C.btnBdr,
    btnAct:  isWhite ? 'rgba(255,255,255,0.12)' : C.btnAct,
    btnTxt:  isWhite ? 'rgba(255,255,255,0.55)' : C.muted,
    btnActT: isWhite ? '#ffffff' : C.gold,
    output:  isWhite ? '#ffffff' : C.outputCl,
    close:   isWhite ? 'rgba(255,180,180,0.70)' : C.closeCl,
  };

  // Appear / disappear
  const panelOn = !!activeDecoder || isSpotsMode || isDabMode;
  useEffect(() => {
    if (panelOn) {
      setMinimised(false);
      // The panel is never unmounted when it closes, so without this it reopens still expanded
      // from a previous session.
      setSpotsExpanded(false);
      Animated.parallel([
        Animated.timing(opacity, { toValue: 1, duration: 200, useNativeDriver: true }),
        Animated.spring(slideY, { toValue: 0, damping: 22, stiffness: 200, useNativeDriver: true }),
      ]).start();
    } else {
      Animated.timing(opacity, { toValue: 0, duration: 150, useNativeDriver: true }).start();
    }
  }, [panelOn, opacity, slideY]);

  // CW spots have no message or grid to reveal, so an expanded state carried across the switch
  // would just be tall empty rows.
  useEffect(() => { setSpotsExpanded(false); }, [spotsKind]);

  // Scroll to bottom when text grows
  useEffect(() => {
    if (!minimised) {
      setTimeout(() => outputRef.current?.scrollToEnd({ animated: false }), 40);
    }
  }, [decoderText, minimised]);

  // ── Keyboard: the decoder box takes the keyboard on TAB ─────────────────────
  //
  // ★ This box floats above a LIVE screen, so it cannot simply listen: while it holds the
  // keyboard the main screen must stop acting on keys, or up/down would retune the radio
  // underneath the list you are reading. Tab hands it over and Tab hands it back.
  //
  // ★★ TWO AXES, NO SUB-MODES. Left/right move along the header controls, up/down move
  // through the list, and whichever you last used is what SPACE activates. That avoids a
  // nested "now you are in the header" state, which would be one more invisible mode.
  //
  // ★★★ SPACE, NOT ENTER, everywhere in here. Enter is the tune box across the whole app and
  // Stuart flagged the exception himself — a key that means something different depending on
  // where you are is the thing that has caused most of the confusion in this work. Space is
  // free, and "space activates the focused thing" is a convention rather than a rule to learn.
  const [kbZone, setKbZone] = useState<null | 'header' | 'list' | 'speed'>(null);
  const [speedIdx, setSpeedIdx] = useState(0);
  const [hdrIdx, setHdrIdx] = useState(0);
  const [listIdx, setListIdx] = useState(0);
  const hdrSlots = useRef<Array<() => void>>([]);
  hdrSlots.current = [];
  const [hdrCount, setHdrCount] = useState(0);
  useEffect(() => {
    if (hdrSlots.current.length !== hdrCount) setHdrCount(hdrSlots.current.length);
  });

  // The list this box is showing, if any. ADS-B and spots have nothing to select — Stuart:
  // "same kinda thing with ADSB except nothing to select just scroll" — so they navigate but
  // Space does nothing rather than pretending to.
  const listLen = isDabMode ? dabProgrammes.length : 0;

  // ★ The flash. Tab-in is otherwise invisible, and invisible focus has looked like a broken
  // keyboard three times over in this work. It announces itself once and then gets out of the
  // way, which is what a real control does when it lights up. (Stuart's idea.)
  const { value: flash, flash: announce, flashThen } = useAnnounce();

  const leave = useCallback(() => { setKbZone(null); captureRegion(null); }, []);
  // ★ On a TIMEOUT, flash once more and let it be seen before closing — an announcement of
  // departure, so the box handing the keyboard back is deliberate rather than mysterious.
  const leaveAnnounced = useCallback(() => { flashThen(leave); }, [flashThen, leave]);
  const leaveAnnouncedRef = useRef(leaveAnnounced); leaveAnnouncedRef.current = leaveAnnounced;

  useEffect(() => () => { captureRegion(null); }, []);   // never leave it captured on unmount

  // ★★ DAB TAKES THE KEYBOARD WITHOUT TAB. On DAB the VFO is LOCKED to the multiplex, so
  // there is nothing to tune and the box IS the interface — the programme list is the only
  // thing on screen worth moving through. Making the user press Tab first would be asking
  // them to hand over a keyboard the main screen has no use for. (Stuart, 2026-07-26.)
  //
  // ★ Once per appearance, so Tab still releases it and does not immediately snatch it back:
  // a user who wants the waterfall's zoom on a DAB ensemble can still have it.
  // ★ The list SCROLLS, so focus has to drag it — the same fault the profile list had. Uses
  // the y each row reports on layout rather than a measurement API: it is the position within
  // the scroll content, which is exactly what scrollTo wants, and it cannot silently no-op the
  // way measureLayout did three times over.
  const dabScroll = useRef<ScrollView | null>(null);
  const dabY = useRef<Record<number, number>>({});
  useEffect(() => {
    if (kbZone !== 'list') return;
    const p = dabProgrammes[listIdx];
    const y = p ? dabY.current[p.id] : undefined;
    if (y != null) dabScroll.current?.scrollTo({ y: Math.max(0, y - 60), animated: true });
  }, [listIdx, kbZone, dabProgrammes]);

  // ★★ A TOUCH HANDS THE KEYBOARD BACK. The box was staying in keyboard mode when Stuart went
  // back to fingers — capture is a mode, and a mode you cannot leave by doing the obvious
  // thing is a trap. Any touch drops it; the next key press takes it again, so switching
  // between hand and keyboard needs no thought and no gesture of its own.
  const kbActive = useKeyboardMode();
  // ★★ WHO OWNS THE ARROWS BY DEFAULT. On DAB and ADS-B the box IS the screen — a locked
  // multiplex or an aircraft table — so it takes the arrows outright. On the HF decoders the
  // waterfall is in ACTIVE USE and tune/zoom are the primary controls, so the box only
  // borrows them when you deliberately Tab in. Stuart's framing, and it is the right split:
  // secondary functions should take the controls only while you are actually looking at them.
  const autoOwn = isDabMode || isAircraftMode;
  const autoTaken = useRef(false);
  useEffect(() => {
    if (kbActive) return;
    autoTaken.current = false;    // let DAB re-take it on the next key press
    if (kbZoneRef.current) leave();
  }, [kbActive, leave]);
  useEffect(() => {
    const want = autoOwn && panelOn && !minimised;
    if (!want) { autoTaken.current = false; return; }
    if (autoTaken.current) return;
    autoTaken.current = true;
    captureRegion('decoder');
    setKbZone('list');
    setListIdx(0);
    announce();
  }, [autoOwn, panelOn, minimised, announce]);

  useEffect(() => { if (!panelOn) leave(); }, [panelOn, leave]);

  useRepeatingKeys(panelOn, (k: string) => {
    {
      // Any key the box acts on counts as activity — see armIdle.
      if (kbZoneRef.current) armIdleRef.current();
      if (k === 'Tab') {
        // ★★ ON DAB, TAB MOVES BETWEEN LIST AND HEADER — it never releases the keyboard.
        // Zooming a multiplex only zooms into a wall of signals (Stuart), so handing the
        // arrows back to the main screen gains nothing. But the header still holds SPEED FIX,
        // which is very much wanted, so Tab has to reach it rather than leave.
        //
        // ★ I first removed Tab here entirely, having conflated "leaving the box" with
        // "reaching the header". They are different things, and on DAB only one of them is
        // useful.
        if (autoOwnRef.current) {
          // Owned outright: Tab moves between the list and the header, never out.
          setKbZone(z => (z === 'header' ? 'list' : 'header'));
          setHdrIdx(0);
          announce();
          return;
        }
        // ★ Borrowed: Tab once into the LIST, again into the HEADER, again to hand it back.
        // The list first because scrolling what you are reading is the common case; the
        // header controls are the occasional one. (Stuart.)
        setKbZone(z => {
          if (z === null) { captureRegion('decoder'); announce(); return 'list'; }
          if (z === 'list') { setHdrIdx(0); announce(); return 'header'; }
          captureRegion(null);
          return null;
        });
        return;
      }
      if (!kbZoneRef.current) return;               // not ours until Tab says so
      if (k === 'Escape' || k === 'Backspace') {
        // Deepest first: close the speed popup before anything else, without changing it.
        if (kbZoneRef.current === 'speed') { setDabSpeedOpenRef.current(false); return; }
        // Where the box OWNS the arrows there is nothing to hand them back to, so these step
        // back to the list rather than leaving them tuning a locked VFO.
        if (autoOwnRef.current) { setKbZone('list'); return; }
        leave();
        return;
      }
      // The popup is a dropdown: it owns every arrow while open, Space picks, Backspace leaves.
      if (kbZoneRef.current === 'speed') {
        if (k === 'ArrowLeft' || k === 'ArrowUp') { setSpeedIdx(i => Math.max(0, i - 1)); return; }
        if (k === 'ArrowRight' || k === 'ArrowDown') { setSpeedIdx(i => Math.min(DAB_SPEEDS.length - 1, i + 1)); return; }
        if (k === 'Space') { onDabSpeedRef.current(speedIdxRef.current); return; }
        if (k === 'Tab') { setDabSpeedOpenRef.current(false); return; }
        return;
      }
      if (k === 'ArrowLeft' || k === 'ArrowRight') {
        setKbZone('header');
        setHdrIdx(i => Math.max(0, Math.min(hdrSlots.current.length - 1, i + (k === 'ArrowRight' ? 1 : -1))));
        return;
      }
      if (k === 'ArrowUp' || k === 'ArrowDown') {
        // ★ No selectable list (ADS-B, spots, a text decoder) — Stuart: "nothing to select,
        // just scroll". So the arrows move the body itself rather than doing nothing.
        if (listLenRef.current <= 0) {
          const sv: any = aircraftRef.current ?? spotsRef.current ?? outputRef.current;
          if (!sv) return;
          setKbZone('list');
          bodyScrollY.current = Math.max(0, bodyScrollY.current + (k === 'ArrowDown' ? 90 : -90));
          if (sv.scrollToOffset) sv.scrollToOffset({ offset: bodyScrollY.current, animated: true });
          else sv.scrollTo?.({ y: bodyScrollY.current, animated: true });
          return;
        }
        setKbZone('list');
        setListIdx(i => Math.max(0, Math.min(listLenRef.current - 1, i + (k === 'ArrowDown' ? 1 : -1))));
        return;
      }
      if (k === 'Space') {
        if (kbZoneRef.current === 'header') hdrSlots.current[hdrIdxRef.current]?.();
        else if (listLenRef.current > 0) onSelectDabRef.current?.(listIdxRef.current);
      }
    }
  }, NAV_REPEAT_KEYS);

  // Refs so the listener above, installed once, never reads a stale value.
  const kbZoneRef = useRef(kbZone);   kbZoneRef.current = kbZone;
  const hdrIdxRef = useRef(hdrIdx);   hdrIdxRef.current = hdrIdx;
  const listIdxRef = useRef(listIdx); listIdxRef.current = listIdx;
  const listLenRef = useRef(listLen); listLenRef.current = listLen;
  const isDabModeRef = useRef(isDabMode); isDabModeRef.current = isDabMode;
  const autoOwnRef = useRef(autoOwn); autoOwnRef.current = autoOwn;
  const speedIdxRef = useRef(speedIdx); speedIdxRef.current = speedIdx;
  const setDabSpeedOpenRef = useRef(setDabSpeedOpen); setDabSpeedOpenRef.current = setDabSpeedOpen;
  const onDabSpeedRef = useRef((i: number) => {});
  onDabSpeedRef.current = (i: number) => {
    const o = DAB_SPEEDS[i];
    if (o) { onDabSpeed?.(o.v); setDabSpeedOpen(false); }
  };
  const onSelectDabRef = useRef((i: number) => {
    const p = dabProgrammes[i];
    if (p) onSelectDab?.(p.id);
  });
  onSelectDabRef.current = (i: number) => {
    const p = dabProgrammes[i];
    if (p) onSelectDab?.(p.id);
  };

  // Idle timeout, matching the menus: a stray Tab must not leave the box holding the keyboard
  // while the user has walked away from it. Resets on every key it handles.
  const idleRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  // ★★ RESET ON EVERY KEY THE BOX HANDLES, not on a changed index. The timer used to restart
  // when the focused INDEX moved — fine on a selectable list, useless on ADS-B, spots or a
  // text decoder, where the arrows scroll the BODY and no index exists to change. So the one
  // case where you scroll continuously was the one case where scrolling did not count as
  // activity, and the box released the keyboard mid-scroll.
  const armIdle = useCallback(() => {
    if (idleRef.current) clearTimeout(idleRef.current);
    idleRef.current = null;
    if (autoOwnRef.current || !kbZoneRef.current) return;
    idleRef.current = setTimeout(() => leaveAnnouncedRef.current(), PANEL_IDLE_MS);
  }, []);
  const armIdleRef = useRef(armIdle); armIdleRef.current = armIdle;
  // ★ The SPEED FIX popup takes the arrows the moment it opens. The button EXPANDS the header
  // to show the presets, but nothing could move the selector into them — Stuart: "the button
  // expands the header for them but I cannot move the selector down to get to them." A
  // control that opens a list has to hand the list the keys, or it has only half worked.
  useEffect(() => {
    if (!dabSpeedOpen) { setKbZone(z => (z === 'speed' ? 'header' : z)); return; }
    const cur = DAB_SPEEDS.findIndex(o => Math.abs((dabSpeed ?? 1) - o.v) < 0.001);
    setSpeedIdx(cur >= 0 ? cur : 0);
    setKbZone('speed');
    announce();
  }, [dabSpeedOpen]);   // eslint-disable-line react-hooks/exhaustive-deps

  // Minimising hands the keyboard back — the list is not on screen to be walked.
  useEffect(() => { if (minimised && kbZone) leave(); }, [minimised]);   // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => {
    if (kbZone === null) { if (idleRef.current) clearTimeout(idleRef.current); return; }
    if (idleRef.current) clearTimeout(idleRef.current);
    // ★ Where the box OWNS the arrows they were never borrowed, so timing out would strand
    // the user with arrows that tune a locked VFO and a list they can no longer reach. Where
    // it BORROWED them, the timeout is the point: on an HF waterfall you want tune and zoom
    // back as soon as you stop reading the decoder.
    if (autoOwn) return;
    idleRef.current = setTimeout(() => leaveAnnounced(), PANEL_IDLE_MS);
    return () => { if (idleRef.current) clearTimeout(idleRef.current); };
  }, [kbZone, hdrIdx, listIdx, autoOwn, leaveAnnounced]);

  /** A header control that takes part in the left/right order. */
  const HBtn = ({ onPress, style, children, ...rest }: any) => {
    const i = hdrSlots.current.length;
    hdrSlots.current.push(onPress ?? (() => {}));
    const on = kbZone === 'header' && hdrIdx === i;
    return (
      <TouchableOpacity onPress={onPress}
        style={[style, on && { borderColor: NAV_FOCUS, borderWidth: 2 }]} {...rest}>
        {children}
      </TouchableOpacity>
    );
  };

  if (!panelOn) return null;

  const title = isDabMode
    ? 'DAB'
    : isSpotsMode
    ? (spotsKind === 'cw' ? 'CW SPOTS' : 'DIGITAL SPOTS')
    : (DECODER_LABELS[activeDecoder!] ?? String(activeDecoder).toUpperCase());

  return (
    <Animated.View
      style={[dp.wrap, { bottom: bottomOffset, opacity, transform: [{ translateY: slideY }] }]}
    >
      <View style={[dp.inner, { borderColor: kbZone ? NAV_FOCUS : dc.border }]}
            onTouchStart={noteTouchInteraction}>

        {/* ★ Arrival / departure flash. A border that brightens once and fades, so taking the
            keyboard and handing it back are both announced. pointerEvents none — it is a
            signal, never a target. */}
        <Animated.View pointerEvents="none"
          style={[dp.flash, { opacity: flash }]} />

        {/* Header */}
        <TouchableOpacity
          style={[dp.header, { borderBottomColor: dc.hdrBdr }]}
          onPress={() => setMinimised((p: boolean) => !p)}
          activeOpacity={0.85}
        >
          {/* Status dot */}
          <View style={[dp.dot, decoding && dp.dotOn]} />

          {/* Title */}
          <Text style={[dp.title, { color: dc.title, fontFamily: t.font }, minimised && dp.titleMin]}>
            {title}
          </Text>

          {/* Status text — directly after title (skin layout).
              ★ DAB has no "listening…" state to report: it is not hunting for a signal,
              the multiplex either decodes or it does not. So that slot carries the
              MULTIPLEX NAME instead, matching the watch's DabView, which is the one
              thing you actually want to read there. */}
          <Text style={[dp.status, dp.statusGrow, { color: dc.status, fontFamily: t.font }]}
                numberOfLines={1}>
            {kbZone
              // ★ Shown for EVERY decoder, not just DAB. On RTTY the box took the keyboard
              // and said nothing about it — the CLR button responded to space, so it worked,
              // but nothing told you it would. A box that has the keyboard should say so
              // whatever it is showing. (Stuart, 2026-07-25.)
              // ★ Always says where Tab goes NEXT, so the cycle is discoverable by using it
              // rather than by being remembered. The wording differs by zone AND by whether
              // the box owns the arrows or merely borrowed them.
              ? (kbZone === 'speed'  ? 'space to set · backspace to cancel'
                 : kbZone === 'header' ? (autoOwn ? (isDabMode ? 'space to press · tab for stations'
                                                               : 'space to press · tab for the list')
                                                  : 'space to press · tab to leave')
                 : listLen > 0        ? 'space to select · tab for controls'
                 : 'scroll with ↑↓ · tab for controls')
              : isDabMode ? (dabEnsemble || 'reading multiplex…')
              : decoderStatus}
          </Text>

          {/* EXPAND LEADS, and stays outside the filter run. It changes how each row is DRAWN;
              MODE/BAND/AGE change WHICH rows are listed. Sitting it between two cyclers read as
              a fourth filter. */}
          {isSpotsMode && spotsKind === 'digi' && (
            <HBtn hitSlop={6} style={[dp.hbtn, { borderColor: dc.btnBdr }]}
              onPress={(e: any) => { e?.stopPropagation(); setSpotsExpanded(v => !v); }}>
              <Text style={[dp.hbtnTxt, {
                color: spotsExpanded ? dc.btnActT : dc.btnTxt, fontFamily: t.font }]}>
                {spotsExpanded ? 'COLLAPSE' : 'EXPAND'}
              </Text>
            </HBtn>
          )}
          {/* Spots filter cyclers (skin sf-mode / sf-band / sf-age dropdowns) */}
          {isSpotsMode && spotsKind === 'digi' && (
            <HBtn hitSlop={6} style={[dp.hbtn, { borderColor: dc.btnBdr }]}
              onPress={(e: any) => {
                e?.stopPropagation();
                setSfMode(SF_MODES[(SF_MODES.indexOf(sfMode) + 1) % SF_MODES.length]);
              }}>
              <Text style={[dp.hbtnTxt, {
                color: sfMode !== 'ALL' ? dc.btnActT : dc.btnTxt, fontFamily: t.font }]}>
                {sfMode === 'ALL' ? 'MODE' : sfMode}
              </Text>
            </HBtn>
          )}
          {isSpotsMode && (
            <HBtn hitSlop={6} style={[dp.hbtn, { borderColor: dc.btnBdr }]}
              onPress={(e: any) => {
                e?.stopPropagation();
                setSfBand(SF_BANDS[(SF_BANDS.indexOf(sfBand) + 1) % SF_BANDS.length]);
              }}>
              <Text style={[dp.hbtnTxt, {
                color: sfBand !== 'ALL' ? dc.btnActT : dc.btnTxt, fontFamily: t.font }]}>
                {sfBand === 'ALL' ? 'BAND' : sfBand}
              </Text>
            </HBtn>
          )}
          {isSpotsMode && (
            <HBtn hitSlop={6} style={[dp.hbtn, { borderColor: dc.btnBdr }]}
              onPress={(e: any) => {
                e?.stopPropagation();
                const i = SF_AGES.findIndex(a => a.minutes === sfAge);
                setSfAge(SF_AGES[(i + 1) % SF_AGES.length].minutes);
              }}>
              <Text style={[dp.hbtnTxt, {
                color: sfAge > 0 ? dc.btnActT : dc.btnTxt, fontFamily: t.font }]}>
                {SF_AGES.find(a => a.minutes === sfAge)?.label ?? 'AGE'}
              </Text>
            </HBtn>
          )}

          {/* CLR — text decoders (skin _clearB) */}
          {!isImageMode && !isSpotsMode && !isDabMode && (
            <HBtn hitSlop={6}
              style={[dp.hbtn, { borderColor: dc.btnBdr }]}
              onPress={(e: any) => { e?.stopPropagation(); onClear?.(); }}>
              <Text style={[dp.hbtnTxt, { color: dc.btnTxt, fontFamily: t.font }]}>CLR</Text>
            </HBtn>
          )}

          {/* DAB speed correction (§4.5) — opens the SPEED FIX popup (separate preset
              buttons), a popup like the spots filters. Highlighted when not Off. */}
          {isDabMode && onDabSpeed && (
            <HBtn hitSlop={6} style={[dp.hbtn, { borderColor: dc.btnBdr }]}
              onPress={(e: any) => { e?.stopPropagation(); setDabSpeedOpen(o => !o); }}>
              <Text style={[dp.hbtnTxt, {
                color: Math.abs((dabSpeed ?? 1) - 1) > 0.001 ? dc.btnActT : dc.btnTxt, fontFamily: t.font }]}>
                SPEED FIX
              </Text>
            </HBtn>
          )}

          {/* Morse quality filter (skin lsv-dec-sf-quality) */}
          {activeDecoder === 'morse' && (
            <HBtn hitSlop={6}
              style={[dp.hbtn, { borderColor: dc.btnBdr }]}
              onPress={(e: any) => {
                e?.stopPropagation();
                const i = MORSE_QUALITIES.indexOf(morseQuality);
                onMorseQuality?.(MORSE_QUALITIES[(i + 1) % MORSE_QUALITIES.length]);
              }}>
              <Text style={[dp.hbtnTxt, { color: dc.btnActT, fontFamily: t.font }]}>
                {MORSE_QUALITY_LABELS[morseQuality]}
              </Text>
            </HBtn>
          )}

          {/* PREV/LIVE + SAVE — image decoders (skin _prevB/_saveB) */}
          {isImageMode && hasPrev && (
            <HBtn hitSlop={6}
              style={[dp.hbtn, { borderColor: dc.btnBdr }]}
              onPress={(e: any) => { e?.stopPropagation(); onTogglePrev?.(); }}>
              <Text style={[dp.hbtnTxt, { color: dc.btnTxt, fontFamily: t.font }]}>
                {viewingPrev ? 'LIVE' : 'PREV'}
              </Text>
            </HBtn>
          )}
          {isImageMode && (
            <HBtn hitSlop={6}
              style={[dp.hbtn, { borderColor: dc.btnBdr }]}
              onPress={(e: any) => { e?.stopPropagation(); onSave?.(); }}>
              <Text style={[dp.hbtnTxt, { color: dc.btnActT, fontFamily: t.font }]}>SAVE</Text>
            </HBtn>
          )}
          {isImageMode && !!imageInfo && (
            <Text style={[dp.status, { color: dc.status, fontFamily: t.font }]} numberOfLines={1}>
              {imageInfo}
            </Text>
          )}

          {/* Minimise / restore (skin _minB: − / □) */}
          <HBtn
            hitSlop={8}
            style={[dp.hbtn, { borderColor: dc.btnBdr }]}
            onPress={(e: any) => { e?.stopPropagation(); setMinimised((p: boolean) => !p); }}
          >
            <Text style={[dp.hbtnTxt, { color: dc.btnTxt, fontFamily: t.font }]}>
              {minimised ? '□' : '−'}
            </Text>
          </HBtn>

          {/* Close — stops the decoder and dismisses the panel (see dismissDecoderPanel).
              ★ NOT shown for DAB or ADS-B: there the whole profile IS the decoder, so closing the
              box would leave the receiver in a mode with nothing to show and no obvious way back.
              Every other decoder is something layered ON a mode you can happily return to, on
              every server type — hence no backend condition here. */}
          {!isDabMode && !isAircraftMode && (
            <HBtn
              hitSlop={8}
              style={[dp.hbtn, { borderColor: dc.btnBdr }]}
              onPress={(e: any) => { e?.stopPropagation(); onClose(); }}
            >
              <Text style={[dp.hbtnTxt, { color: dc.close, fontFamily: t.font }]}>×</Text>
            </HBtn>
          )}
        </TouchableOpacity>

        {/* Body — hidden when minimised; image canvas for WEFAX/SSTV */}
        {!minimised && isImageMode && imageRef && (
          <View style={dp.bodyContent}>
            <DecoderImageCanvas
              ref={imageRef}
              maxHeight={200}
              decoderName={activeDecoder ?? 'image'}
              onInfo={setImageInfo}
              onStatus={(s: string) => onImageStatus?.(s)}
              onPrevState={(hp: boolean, vp: boolean) => { setHasPrev(hp); setViewingPrev(vp); }}
            />
          </View>
        )}
        {/* ADS-B gets a real table rather than the text blob.
            HEIGHT, not maxHeight: dp.body only caps the height, and AircraftPanel is
            flex-based — so with nothing to flex INSIDE it collapsed to zero and the
            box rendered empty. The text body doesn't hit this because a ScrollView
            sizes to its content. */}
        {!minimised && isAircraftMode && (
          <View style={[dp.bodyContent, { height: 200 }]}>
            <AircraftPanel aircraft={aircraft!} scrollRef={aircraftRef} />
          </View>
        )}
        {!minimised && !isImageMode && !isSpotsMode && !isAircraftMode && !isDabMode && (
          <ScrollView
            ref={outputRef}
            style={dp.body}
            contentContainerStyle={dp.bodyContent}
            {...bodyScroll}
            showsVerticalScrollIndicator
          >
            <Text style={[dp.output, { color: dc.output, fontFamily: t.font }]} selectable>
              {decoderText}
            </Text>
          </ScrollView>
        )}

        {/* DAB service list (§5.2) — logos resolve cleanly because DAB programme names are
            EXACT ensemble strings (no RDS guessing like FM). Tap a row to switch service. */}
        {/* SPEED FIX popup (§4.5) — separate preset buttons; fixes the dablin/OWRX
            chipmunk misread. Remembered per station. */}
        {!minimised && isDabMode && dabSpeedOpen && (
          <View style={[dp.dabSpeedPop, { borderBottomColor: dc.hdrBdr }]}>
            <Text style={[dp.dabSpeedTitle, { color: dc.status, fontFamily: t.font }]}>SPEED FIX · remembered per station</Text>
            <View style={dp.dabSpeedRow}>
              {DAB_SPEEDS.map((o) => {
                const on = Math.abs((dabSpeed ?? 1) - o.v) < 0.001;
                return (
                  <TouchableOpacity key={o.l}
                    style={[dp.dabSpeedBtn, { borderColor: on ? dc.btnActT : dc.btnBdr }, on && { backgroundColor: dc.btnAct },
                            kbZone === 'speed' && speedIdx === DAB_SPEEDS.indexOf(o)
                              && { borderColor: NAV_FOCUS, borderWidth: 2 }]}
                    onPress={() => { onDabSpeed?.(o.v); setDabSpeedOpen(false); }} activeOpacity={0.7}>
                    <Text style={[dp.dabSpeedBtnTxt, { color: on ? dc.btnActT : dc.btnTxt, fontFamily: t.font }]}>{o.l}</Text>
                  </TouchableOpacity>
                );
              })}
            </View>
          </View>
        )}
        {!minimised && isDabMode && (
          <ScrollView ref={dabScroll} style={dp.body} showsVerticalScrollIndicator {...bodyScroll}>
            {dabProgrammes.map((p, pi) => {
              const active = p.id === activeDabId;
              const navOn = kbZone === 'list' && listIdx === pi;
              return (
                <TouchableOpacity key={p.id}
                  onLayout={(e) => { dabY.current[p.id] = e.nativeEvent.layout.y; }}
                  style={[dp.dabRow, { borderBottomColor: dc.hdrBdr },
                          navOn && { backgroundColor: 'rgba(124,255,155,0.16)' }]}
                  onPress={() => onSelectDab?.(p.id)} activeOpacity={0.7}>
                  <StationLogo name={p.name} />
                  <Text style={[dp.dabName, { color: active ? dc.btnActT : dc.output, fontFamily: t.font }]}
                        numberOfLines={1}>
                    {active ? '✓ ' : ''}{p.name}
                  </Text>
                </TouchableOpacity>
              );
            })}
          </ScrollView>
        )}

        {/* Spots table — virtualized; newest first; tap frequency to tune */}
        {!minimised && isSpotsMode && (
          <FlatList
            ref={spotsRef}
            style={dp.body}
            {...bodyScroll}
            data={visibleSpots}
            keyExtractor={(s: SpotRow, i: number) => `${s.time}-${s.call}-${s.freqHz}-${i}`}
            renderItem={renderSpot}
            initialNumToRender={12}
            maxToRenderPerBatch={12}
            windowSize={5}
            // ★★ removeClippedSubviews is OFF, deliberately. Spot rows change height when
            // EXPAND is toggled, and with clipping on, iOS mis-estimates the content size,
            // corrects it, the correction moves the offset, and the list oscillates — Stuart:
            // "scrolling rapidly and getting stuck bouncing up and down". It is a documented
            // problem with dynamic row heights, and the memory it saves on a list this short is
            // not worth a list that cannot be read.
            removeClippedSubviews={false}
            // A stable key per spot rather than one including the index, so a row keeps its
            // identity across the re-render that expanding causes.
            extraData={spotsExpanded}
            ListEmptyComponent={
              <Text style={[dp.output, dp.spotEmpty, { color: dc.status, fontFamily: t.font }]}>
                waiting for spots…
              </Text>
            }
          />
        )}

      </View>
    </Animated.View>
  );
}

// ── Styles ────────────────────────────────────────────────────────────────────

const dp = StyleSheet.create({
  // Sits over the whole box; only ever an opacity animation, so it stays on the native driver.
  flash: {
    position: 'absolute', left: 0, right: 0, top: 0, bottom: 0,
    borderWidth: 2, borderColor: NAV_FOCUS, borderRadius: 8,
  },
  wrap: {
    position: 'absolute', left: 8, right: 8,
    zIndex: 200,
  },
  inner: {
    backgroundColor: C.bg,
    borderWidth: 1, borderColor: C.border,
    borderRadius: 14,
    shadowColor: '#000', shadowOffset: { width: 0, height: 4 },
    shadowOpacity: 0.80, shadowRadius: 14, elevation: 16,
    overflow: 'hidden',
  },
  header: {
    flexDirection: 'row', alignItems: 'center', gap: 6,
    paddingHorizontal: 12, paddingVertical: 8,
    borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: C.hdrBdr,
  },
  dot:       { width: 6, height: 6, borderRadius: 3, backgroundColor: C.dotIdle, flexShrink: 0 },
  dotOn:     { backgroundColor: C.dotOn, shadowColor: '#55d98d', shadowOpacity: 0.60, shadowRadius: 4, shadowOffset: { width:0, height:0 } },
  title:     { fontSize: 10, letterSpacing: 2, color: C.goldDim, fontFamily: FONT, flexShrink: 0 },
  titleMin:  { color: 'rgba(255,160,0,0.40)' },
  btnScroll: { flexShrink: 1 },
  btnScrollContent: { flexDirection: 'row', gap: 5, alignItems: 'center' },
  hbtn: {
    borderWidth: 1, borderColor: C.btnBdr, borderRadius: 4,
    paddingHorizontal: 8, paddingVertical: 3,
  },
  hbtnActive:    { backgroundColor: C.btnAct, borderColor: 'rgba(255,160,0,0.55)' },
  hbtnTxt:       { fontFamily: FONT, fontSize: 11, color: 'rgba(255,160,0,0.60)' },
  hbtnTxtActive: { color: C.gold },
  status:     { fontSize: 9, letterSpacing: 1, color: C.muted, flexShrink: 1, overflow: 'hidden' },
  statusGrow: { flex: 1 },
  // Spots table
  // The cells now live in the inner `spotLine`, so the row box itself must NOT be a row — a nested
  // row would shrink to its content and the right-hand cells would lose their alignment.
  spotRow: {
    paddingHorizontal: 10, paddingVertical: 4,
    borderBottomWidth: StyleSheet.hairlineWidth,
    borderBottomColor: 'rgba(255,160,0,0.08)',
  },
  // Expanded rows keep the same horizontal padding and divider; only the vertical box grows.
  spotRowTall: {
    paddingHorizontal: 10, paddingVertical: 4,
    borderBottomWidth: StyleSheet.hairlineWidth,
    borderBottomColor: 'rgba(255,160,0,0.08)',
  },
  spotLine:    { flexDirection: 'row', alignItems: 'center', gap: 6 },
  // Indented to the width of the time cell so it reads as belonging to the row above rather than
  // as a row of its own, and dimmer so a collapsed-style scan still skims past it.
  // Line 2 is prose to READ, not a column to scan, so it gets a readable size rather than a
  // decorative one. Still stepped back from line 1, but by opacity alone now.
  spotDetail:  { fontSize: 11, letterSpacing: 0.2, color: 'rgba(255,255,255,0.62)',
                 marginLeft: 44, marginTop: 3 },
  spotEmpty:   { padding: 12, textAlign: 'center' },
  // 7-column layout (Time·Call·Band·Mode·SNR·Country·Distance) — fixed widths sized
  // for the SE's ~340pt panel; call + country flex the remainder
  // ★ Data WHITE, callsign AMBER — the reverse of the original. Dim amber on black at 10pt was
  // unreadable on a 17 Pro Max even at arm's length: the colour was doing the work of a hierarchy
  // that weight and size should do. Now the data reads plainly and the amber marks the one field
  // you scan for.
  spotCell:    { fontSize: 11, letterSpacing: 0.3, color: 'rgba(255,255,255,0.88)' },
  // Widened with the font step from 10pt to 11pt — the old widths were cut for 10pt and clip
  // "20:14"/"FT8" at the larger size. Check on the SE in Display Zoom before trimming these.
  spotTime:    { width: 42 },
  spotBand:    { width: 38 },
  spotMode:    { width: 40 },
  spotSnr:     { width: 30, textAlign: 'right' },
  spotCall:    { flex: 1.2, fontSize: 13, fontWeight: '700', marginLeft: 6 },
  spotCountry: { flex: 0.9, textAlign: 'right' },
  spotDist:    { width: 56, textAlign: 'right', marginLeft: 4 },
  settingsRow: {
    flexDirection: 'row', alignItems: 'center', flexWrap: 'wrap', gap: 5,
    paddingHorizontal: 12, paddingVertical: 6,
    borderTopWidth: StyleSheet.hairlineWidth,
  },
  settingsGap: { width: 6 },
  closeBtn: { color: C.closeCl, fontSize: 16, paddingHorizontal: 2, flexShrink: 0 },
  body:        { maxHeight: 200 },
  bodyContent: { padding: 12 },
  dabRow:      { flexDirection: 'row', alignItems: 'center', gap: 10, paddingVertical: 9, paddingHorizontal: 12, borderBottomWidth: StyleSheet.hairlineWidth },
  dabName:     { flex: 1, fontSize: 14 },
  dabSpeedPop: { paddingVertical: 8, paddingHorizontal: 12, borderBottomWidth: StyleSheet.hairlineWidth },
  dabSpeedTitle: { fontSize: 10, letterSpacing: 0.5, marginBottom: 6 },
  dabSpeedRow: { flexDirection: 'row', gap: 6 },
  dabSpeedBtn: { flex: 1, borderWidth: 1, borderRadius: 4, paddingVertical: 8, alignItems: 'center' },
  dabSpeedBtnTxt: { fontSize: 11, fontWeight: '600' },
  output: {
    fontSize: 12, letterSpacing: 0.8, lineHeight: 20,
    color: C.outputCl, fontFamily: FONT,
    textShadowColor: 'rgba(255,220,100,0.35)',
    textShadowOffset: { width: 0, height: 0 },
    textShadowRadius: 4,
  },
});
