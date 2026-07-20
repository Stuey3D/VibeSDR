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
  activeDabId?:    number;
  onSelectDab?:    (id: number) => void;
  dabSpeed?:       number;
  onDabSpeed?:     (v: number) => void;
}

const DAB_SPEEDS = [
  { v: 1, l: '1×' }, { v: 0.6667, l: '0.67×' }, { v: 0.5, l: '0.5×' },
  { v: 0.3333, l: '0.33×' }, { v: 0.25, l: '0.25×' },
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

// Memoized spot row — with FlatList virtualization only the ~12 visible rows
// render, and unchanged rows skip re-render entirely when new spots flush in.
const SpotRowView = React.memo(function SpotRowView({ s, isCW, font, callColor, onTuneHz }: {
  s: SpotRow; isCW: boolean; font: string; callColor: string;
  onTuneHz?: (hz: number) => void;
}) {
  // Columns: Time · Band · Mode · SNR · Call · Country (Stuart 2026-06-12 —
  // freq + distance dropped, they truncated everything on the SE; the row
  // tap still tunes to the spot's frequency)
  return (
    <TouchableOpacity style={dp.spotRow}
      onPress={() => s.freqHz && onTuneHz?.(s.freqHz)} activeOpacity={0.6}>
      <Text style={[dp.spotCell, dp.spotTime, { fontFamily: font }]}>
        {fmtSpotTime(s.time)}
      </Text>
      <Text style={[dp.spotCell, dp.spotBand, { fontFamily: font }]}>{s.band}</Text>
      <Text style={[dp.spotCell, dp.spotMode, { fontFamily: font }]}>
        {isCW ? (s.wpm ? Math.round(s.wpm) + 'w' : 'CW') : s.mode}
      </Text>
      <Text style={[dp.spotCell, dp.spotSnr,
        { color: (s.snr ?? -99) >= 0 ? '#55d98d' : 'rgba(255,160,0,0.65)', fontFamily: font }]}>
        {s.snr !== undefined ? s.snr : ''}
      </Text>
      <Text style={[dp.spotCell, dp.spotCall, { color: callColor, fontFamily: font }]}
            numberOfLines={1}>
        {s.call}
      </Text>
      <Text style={[dp.spotCell, dp.spotCountry, { fontFamily: font }]} numberOfLines={1}>
        {abbrCountry(s.country)}
      </Text>
      {/* Distance-to-receiver (FT8: from the TX grid). Its own cell so country +
          distance both show; blank when unknown. */}
      <Text style={[dp.spotCell, dp.spotDist, { fontFamily: font }]} numberOfLines={1}>
        {s.distKm != null ? `${s.distKm}km` : ''}
      </Text>
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
  dabProgrammes = [], activeDabId, onSelectDab, dabSpeed = 1, onDabSpeed,
}: DecoderPanelProps) {
  const isDabMode = dabProgrammes.length > 0;
  const isSpotsMode = !isDabMode && spotsKind !== null;
  const isImageMode = !isSpotsMode && !isDabMode && IMAGE_DECODERS.includes(activeDecoder);
  const isAircraftMode = !isSpotsMode && !isDabMode && !!aircraft?.length;

  // Spots filters — header cyclers (skin sf-mode/sf-band/sf-age)
  const [sfMode, setSfMode] = useState('ALL');
  const [sfBand, setSfBand] = useState('ALL');
  const [sfAge,  setSfAge]  = useState(0);
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
                 callColor={themeForRows.name === 'white' ? '#ffffff' : C.outputCl}
                 onTuneHz={onTuneHz} />
  ), [spotsKind, themeForRows, onTuneHz]);
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
  const opacity  = useRef(new Animated.Value(0)).current;
  const slideY   = useRef(new Animated.Value(20)).current;
  const outputRef = useRef<ScrollView>(null);

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
      Animated.parallel([
        Animated.timing(opacity, { toValue: 1, duration: 200, useNativeDriver: true }),
        Animated.spring(slideY, { toValue: 0, damping: 22, stiffness: 200, useNativeDriver: true }),
      ]).start();
    } else {
      Animated.timing(opacity, { toValue: 0, duration: 150, useNativeDriver: true }).start();
    }
  }, [panelOn, opacity, slideY]);

  // Scroll to bottom when text grows
  useEffect(() => {
    if (!minimised) {
      setTimeout(() => outputRef.current?.scrollToEnd({ animated: false }), 40);
    }
  }, [decoderText, minimised]);

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
      <View style={[dp.inner, { borderColor: dc.border }]}>

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

          {/* Status text — directly after title (skin layout) */}
          <Text style={[dp.status, dp.statusGrow, { color: dc.status, fontFamily: t.font }]}
                numberOfLines={1}>
            {decoderStatus}
          </Text>

          {/* Spots filter cyclers (skin sf-mode / sf-band / sf-age dropdowns) */}
          {isSpotsMode && spotsKind === 'digi' && (
            <TouchableOpacity hitSlop={6} style={[dp.hbtn, { borderColor: dc.btnBdr }]}
              onPress={(e: any) => {
                e?.stopPropagation();
                setSfMode(SF_MODES[(SF_MODES.indexOf(sfMode) + 1) % SF_MODES.length]);
              }}>
              <Text style={[dp.hbtnTxt, {
                color: sfMode !== 'ALL' ? dc.btnActT : dc.btnTxt, fontFamily: t.font }]}>
                {sfMode === 'ALL' ? 'MODE' : sfMode}
              </Text>
            </TouchableOpacity>
          )}
          {isSpotsMode && (
            <TouchableOpacity hitSlop={6} style={[dp.hbtn, { borderColor: dc.btnBdr }]}
              onPress={(e: any) => {
                e?.stopPropagation();
                setSfBand(SF_BANDS[(SF_BANDS.indexOf(sfBand) + 1) % SF_BANDS.length]);
              }}>
              <Text style={[dp.hbtnTxt, {
                color: sfBand !== 'ALL' ? dc.btnActT : dc.btnTxt, fontFamily: t.font }]}>
                {sfBand === 'ALL' ? 'BAND' : sfBand}
              </Text>
            </TouchableOpacity>
          )}
          {isSpotsMode && (
            <TouchableOpacity hitSlop={6} style={[dp.hbtn, { borderColor: dc.btnBdr }]}
              onPress={(e: any) => {
                e?.stopPropagation();
                const i = SF_AGES.findIndex(a => a.minutes === sfAge);
                setSfAge(SF_AGES[(i + 1) % SF_AGES.length].minutes);
              }}>
              <Text style={[dp.hbtnTxt, {
                color: sfAge > 0 ? dc.btnActT : dc.btnTxt, fontFamily: t.font }]}>
                {SF_AGES.find(a => a.minutes === sfAge)?.label ?? 'AGE'}
              </Text>
            </TouchableOpacity>
          )}

          {/* CLR — text decoders (skin _clearB) */}
          {!isImageMode && !isSpotsMode && !isDabMode && (
            <TouchableOpacity hitSlop={6}
              style={[dp.hbtn, { borderColor: dc.btnBdr }]}
              onPress={(e: any) => { e?.stopPropagation(); onClear?.(); }}>
              <Text style={[dp.hbtnTxt, { color: dc.btnTxt, fontFamily: t.font }]}>CLR</Text>
            </TouchableOpacity>
          )}

          {/* DAB speed correction (§4.5) — cycles the presets that fix the dablin/OWRX
              "chipmunk" sample-rate misread. Highlighted when not 1×. */}
          {isDabMode && onDabSpeed && (
            <TouchableOpacity hitSlop={6} style={[dp.hbtn, { borderColor: dc.btnBdr }]}
              onPress={(e: any) => {
                e?.stopPropagation();
                const i = DAB_SPEEDS.findIndex(o => Math.abs(o.v - (dabSpeed ?? 1)) < 0.001);
                onDabSpeed(DAB_SPEEDS[(i < 0 ? 0 : i + 1) % DAB_SPEEDS.length].v);
              }}>
              <Text style={[dp.hbtnTxt, {
                color: Math.abs((dabSpeed ?? 1) - 1) > 0.001 ? dc.btnActT : dc.btnTxt, fontFamily: t.font }]}>
                ⏩ {DAB_SPEEDS.find(o => Math.abs(o.v - (dabSpeed ?? 1)) < 0.001)?.l ?? '1×'}
              </Text>
            </TouchableOpacity>
          )}

          {/* Morse quality filter (skin lsv-dec-sf-quality) */}
          {activeDecoder === 'morse' && (
            <TouchableOpacity hitSlop={6}
              style={[dp.hbtn, { borderColor: dc.btnBdr }]}
              onPress={(e: any) => {
                e?.stopPropagation();
                const i = MORSE_QUALITIES.indexOf(morseQuality);
                onMorseQuality?.(MORSE_QUALITIES[(i + 1) % MORSE_QUALITIES.length]);
              }}>
              <Text style={[dp.hbtnTxt, { color: dc.btnActT, fontFamily: t.font }]}>
                {MORSE_QUALITY_LABELS[morseQuality]}
              </Text>
            </TouchableOpacity>
          )}

          {/* PREV/LIVE + SAVE — image decoders (skin _prevB/_saveB) */}
          {isImageMode && hasPrev && (
            <TouchableOpacity hitSlop={6}
              style={[dp.hbtn, { borderColor: dc.btnBdr }]}
              onPress={(e: any) => { e?.stopPropagation(); onTogglePrev?.(); }}>
              <Text style={[dp.hbtnTxt, { color: dc.btnTxt, fontFamily: t.font }]}>
                {viewingPrev ? 'LIVE' : 'PREV'}
              </Text>
            </TouchableOpacity>
          )}
          {isImageMode && (
            <TouchableOpacity hitSlop={6}
              style={[dp.hbtn, { borderColor: dc.btnBdr }]}
              onPress={(e: any) => { e?.stopPropagation(); onSave?.(); }}>
              <Text style={[dp.hbtnTxt, { color: dc.btnActT, fontFamily: t.font }]}>SAVE</Text>
            </TouchableOpacity>
          )}
          {isImageMode && !!imageInfo && (
            <Text style={[dp.status, { color: dc.status, fontFamily: t.font }]} numberOfLines={1}>
              {imageInfo}
            </Text>
          )}

          {/* Minimise / restore (skin _minB: − / □) */}
          <TouchableOpacity
            hitSlop={8}
            style={[dp.hbtn, { borderColor: dc.btnBdr }]}
            onPress={(e: any) => { e?.stopPropagation(); setMinimised((p: boolean) => !p); }}
          >
            <Text style={[dp.hbtnTxt, { color: dc.btnTxt, fontFamily: t.font }]}>
              {minimised ? '□' : '−'}
            </Text>
          </TouchableOpacity>
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
            <AircraftPanel aircraft={aircraft!} />
          </View>
        )}
        {!minimised && !isImageMode && !isSpotsMode && !isAircraftMode && !isDabMode && (
          <ScrollView
            ref={outputRef}
            style={dp.body}
            contentContainerStyle={dp.bodyContent}
            showsVerticalScrollIndicator
          >
            <Text style={[dp.output, { color: dc.output, fontFamily: t.font }]} selectable>
              {decoderText}
            </Text>
          </ScrollView>
        )}

        {/* DAB service list (§5.2) — logos resolve cleanly because DAB programme names are
            EXACT ensemble strings (no RDS guessing like FM). Tap a row to switch service. */}
        {!minimised && isDabMode && (
          <ScrollView style={dp.body} showsVerticalScrollIndicator>
            {dabProgrammes.map((p) => {
              const active = p.id === activeDabId;
              return (
                <TouchableOpacity key={p.id} style={[dp.dabRow, { borderBottomColor: dc.hdrBdr }]}
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
            style={dp.body}
            data={visibleSpots}
            keyExtractor={(s: SpotRow, i: number) => `${s.time}-${s.call}-${s.freqHz}-${i}`}
            renderItem={renderSpot}
            initialNumToRender={12}
            maxToRenderPerBatch={12}
            windowSize={5}
            removeClippedSubviews
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
  spotRow: {
    flexDirection: 'row', alignItems: 'center', gap: 6,
    paddingHorizontal: 10, paddingVertical: 4,
    borderBottomWidth: StyleSheet.hairlineWidth,
    borderBottomColor: 'rgba(255,160,0,0.08)',
  },
  spotEmpty:   { padding: 12, textAlign: 'center' },
  // 6-column layout (Time·Band·Mode·SNR·Call·Country) — fixed widths sized
  // for the SE's ~340pt panel; call + country flex the remainder
  spotCell:    { fontSize: 10, letterSpacing: 0.4, color: 'rgba(255,160,0,0.60)' },
  spotTime:    { width: 38 },
  spotBand:    { width: 36 },
  spotMode:    { width: 38 },
  spotSnr:     { width: 28, textAlign: 'right' },
  spotCall:    { flex: 1.2, fontSize: 11, marginLeft: 6 },
  spotCountry: { flex: 0.9, textAlign: 'right' },
  spotDist:    { width: 52, textAlign: 'right', marginLeft: 4 },
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
  output: {
    fontSize: 12, letterSpacing: 0.8, lineHeight: 20,
    color: C.outputCl, fontFamily: FONT,
    textShadowColor: 'rgba(255,220,100,0.35)',
    textShadowOffset: { width: 0, height: 0 },
    textShadowRadius: 4,
  },
});
