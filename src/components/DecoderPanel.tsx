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
  Platform,
  ScrollView,
  Share,
  StyleSheet,
  Text,
  TouchableOpacity,
  View,
  useWindowDimensions,
} from 'react-native';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import { NativeEventEmitter, NativeModules } from 'react-native';
import { NAV_FOCUS, captureRegion, useAnnounce, useKeyboardMode, noteTouchInteraction, useRepeatingKeys, NAV_REPEAT_KEYS, PANEL_IDLE_MS } from './PanelNav';
import DecoderImageCanvas, { type DecoderImageHandle } from './DecoderImageCanvas';
import AsyncStorage from '@react-native-async-storage/async-storage';
import { File, Paths } from 'expo-file-system';
import { type NavtexAssembler, type NavtexMessage, navtexTitle, navtexLostPct, navtexBody, navtexFileName,
         navtexFileText } from '../utils/navtex';
import { SLANT_STEP, parseAlign, wefaxAlignKey, wefaxPreset, type WefaxAlign } from '../utils/wefaxAlign';
import { type MorseQuality, type SpotRow, type SpotsKind } from '../services/DecoderClient';
import { abbrCountry } from '../assets/countryAbbr';
import AircraftPanel from './AircraftPanel';
import { DecoderShell, DecoderHeader, DecoderTitle, DecoderKey, DecoderKeyLabel, DecoderBody,
         DECODER_FONT, decoderBodyInset, engraveStyle, useDecoderTokens, useDecoderStyles,
         type DecoderTokens } from './DecoderShell';
import StationLogo from './StationLogo';
import { scrollLane } from '../constants/popupTokens';
import type { Aircraft } from '../services/SDRBackend';

// ── Types ──────────────────────────────────────────────────────────────────────

export type DecoderType = 'rtty' | 'navtex' | 'wefax' | 'sstv' | 'morse' | 'whisper' | 'ft8'
                        | 'time' | null;
const IMAGE_DECODERS: DecoderType[] = ['wefax', 'sstv'];

export interface DecoderPanelProps {
  /** Called with true while the box is open and not minimised — the screen hides the station strip then. */
  onShownChange?: (shown: boolean) => void;
  /** ★ On a short portrait window (SDRScreen boxTopLimit): the box fills from here down to the controls,
   *  and BIG / SMALL go. */
  topLimit?: number;
  /** ★ BIG's ceiling — just below the status row (SDRScreen boxTopSafe). */
  topSafe?: number;
  /** ★ Opens the tune box. Needed because on DAB this panel owns the keyboard outright, so
   *  Enter never reaches the main screen — see the T shortcut below. */
  onOpenFreq?: () => void;
  activeDecoder: DecoderType;
  decoderText:   string;
  /** ADS-B: the structured aircraft table. When present it REPLACES the text body —
   *  the records carry far more than the flattened "CALLSIGN 39700ft 434kt -25dB"
   *  line ever could (registry country, vertical trend, range and bearing). */
  aircraft?:     Aircraft[];
  decoderStatus: string;   // 'listening…' | 'decoding…' | custom
  decoding:      boolean;  // true = green dot
  /** The tuned (dial) frequency, Hz — WEFAX SHIFT / SLANT are remembered per frequency. */
  tunedHz?:      number;
  bottomOffset:  number;   // distance from bottom of screen (pillTop - 8)
  /** Clear the text output (skin CLR — text decoders only). */
  onClear?:      () => void;
  /** ★ NAVTEX: the stream cut into messages (utils/navtex), fed by SDRScreen as text arrives. */
  navtex?:       NavtexAssembler | null;
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
  const dp = useDecoderStyles(makeDp);
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
  // ★ Not "MSF" or "WWV" — the panel is the same control whichever station is being read, and the
  //   station itself appears in every line the decoder emits.
  time:    'TIME',
};

/* ★★ THE PALETTE IS THE SHELL'S (DecoderShell, brief §10.1), and it is LIVE — `dp` below is built
 *  from the faceplate's tokens (useDecoderStyles), not read once at load. This panel used to keep
 *  its own, with a separate white-theme copy, and it had drifted: title 10 pt at .65, muted .38,
 *  and `theme.font` — which would carry Nixie One into decoder text.
 * ★ The tint is the user's (§10.2): this box was 0.95 ("mostly text, read rather than seen
 *  through") — which is now what Transparency OFF gives (at 1.0); ON is the same glass as every box. */
const FONT = DECODER_FONT;

// ── Component ──────────────────────────────────────────────────────────────────

export default function DecoderPanel({
  activeDecoder, decoderText, aircraft, decoderStatus, decoding,
  bottomOffset, onClear, onClose, navtex,
  imageRef, onImageStatus,
  morseQuality = 'all', onMorseQuality,
  spotsKind = null, spots = [], onTuneHz,
  dabProgrammes = [], dabEnsemble = '', activeDabId, onSelectDab, dabSpeed = 1, onDabSpeed,
  onOpenFreq, onShownChange, topLimit, topSafe, tunedHz = 0,
}: DecoderPanelProps) {
  // ★★★ BIG / SMALL — the decoder box could not be made bigger, for ANY decoder.
  //
  // The image height was a hardcoded 200 pt on every device while DecoderImageCanvas scales the
  // image to the panel's WIDTH, so THE WIDER THE SCREEN THE WORSE IT GOT: a 320x256 SSTV frame
  // draws ~288 pt on a phone and ~640 pt on an iPad, both into the same 200 pt window. The device
  // with the most room to spare showed the least of the picture — and an iPad is what an App Store
  // reviewer picks up (Stuart, 2026-07-30: "SSTV may bite us if a user cannot view the whole
  // thing").
  //
  // ★★ SMALL stays 200 pt ON PURPOSE. The box is capped so the waterfall and spectrum remain
  // visible; that is a deliberate choice, not an oversight. BIG is for when the decoded content IS
  // what the user came for.
  // ★ EVERY decoder benefits, not just SSTV: more rows of RTTY/NAVTEX/Morse text, more FT8 spots,
  // more aircraft, more DAB services.
  //
  // Sizing copied from AdvRdsPanel, which solved this first — including the hazard its own comment
  // records: "LEAVE THE STATUS BAR ALONE. In BIG mode the panel is anchored at the bottom and grew
  // straight up past the notch, covering the clock and battery." Inherited, not rediscovered.
  const [tallPick, setTall] = useState(false);
  const { height: winH } = useWindowDimensions();
  const insets = useSafeAreaInsets();
  // ★★★ THE HEADER IS PART OF THE PANEL AND WAS NOT COUNTED. AdvRdsPanel applies its computed
  // maximum to the INNER CONTAINER — the whole panel — whereas this value sizes the BODY ALONE. So
  // a body allowed to fill the available height put the header ON TOP of it and the panel grew
  // straight past the notch: exactly the hazard AdvRdsPanel's own comment records ("LEAVE THE
  // STATUS BAR ALONE"), reintroduced by applying the right number to the wrong box.
  // ★ Reserved rather than measured: onLayout would settle a frame late and make BIG visibly jump.
  const HEADER_H = 46;
  // ★ OFF on silver / black puts the body in a recessed window with an 8 pt margin under it: height
  //   the reservation above never counted, so it is counted here or BIG reaches the notch again.
  const tk = useDecoderTokens();
  const dp = useDecoderStyles(makeDp);
  // BIG's room: up to the status row. SMALL on a short window: up to the server-name row (topLimit).
  const availH = Math.max(160, winH - bottomOffset - (topSafe ?? insets.top + 16) - HEADER_H - decoderBodyInset(tk));
  const smallFit = topLimit != null ? Math.max(100, winH - bottomOffset - topLimit - HEADER_H - decoderBodyInset(tk)) : 200;
  // ★★★ ONE COMPUTED VALUE DRIVES ALL THREE PLACES THE 200 USED TO LIVE (the image canvas, the
  // ADS-B box and the text ScrollView). They MUST move together or BIG works in some modes and not
  // others.
  // ★★ 0.62, not AdvRdsPanel's 0.82. That panel is a phone-shaped stack of text rows; this one has
  // to look sane in a MAC WINDOW ~1300 pt tall, where 0.82 was a decoder box swallowing the entire
  // app (Stuart, 2026-07-31: "on the Mac the SSTV box is too big"). Images additionally shrink to
  // their own natural size — see DecoderImageCanvas — so this ceiling mostly governs the text and
  // list modes, which genuinely benefit from the rows.
  /* ★★ BIG ONLY WHERE IT IS BIGGER (Stuart, 2026-10-03, the SE in Display Zoom: "remove the option for Big
   *  since big and small take the full screen") — the DAB box's rule (DabPanel bigUseful): offered only when
   *  BIG gains at least 80 pt over SMALL's 200; otherwise SMALL, and no key. */
  /* ★★ BIG IS THE ROOM, ON A PHONE — RTTY / FT8 were held short by the cards, which tuck away now (Stuart, 2026-10-03:
   *  "we can have bigger decoder boxes for things like RTTY/FT8"). The 0.62 cap stays for a big window (a Mac
   *  ~1300 pt tall, where it was "a decoder box swallowing the entire app"). */
  const bigH = winH > 950 ? Math.min(availH, winH * 0.62) : availH;
  const bigUseful = bigH >= smallFit + 40;
  const tall = tallPick && bigUseful;
  const bodyH = Math.round(tall ? bigH : Math.min(availH, smallFit));
  // ★★★ EVERY SCROLLING BODY MUST USE THIS. `dp.body` used to carry `maxHeight: 200` and FOUR
  // places relied on it — the text ScrollView, the DAB list, the spots list and (via its own prop)
  // the image canvas. Moving the number inline and updating only ONE of them left DAB and SPOTS
  // completely UNCAPPED, so an FT8 feed grew the panel straight off the top of the screen, header
  // and controls with it (Stuart, 2026-07-31). A shared constant became four call sites and three
  // were missed.
  // ★ Use `bodySize` everywhere. If a new scrolling body is added, it gets this too.
  const bodySize = tall ? { height: bodyH } : { maxHeight: bodyH };
  // ★★★ AND CAP THE WIDTH. `wrap` is left:8/right:8 — full bleed, which is correct on a phone and
  // absurd on an iPad or a Mac window, where a decoder box stretched the entire width of the screen
  // (Stuart, 2026-07-31: "it's also a bit wide on the iPad, Mac view too"). It was sized to the
  // CONTROLS, and the controls are full-width because THAT is right on a handset — a phone-shaped
  // decision inherited by every larger screen.
  // ★ 760 is about the width the content actually wants: an SSTV frame at its 2x ceiling is 640,
  // and the widest control row (RTTY shift/baud) fits comfortably. Beyond that the box is just
  // padding. Centred, so it sits under the middle of the screen rather than hugging an edge.
  const PANEL_MAX_W = 760;
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

  const renderSpot = useCallback(({ item }: { item: SpotRow }) => (
    <SpotRowView s={item} isCW={spotsKind === 'cw'} font={FONT}
                 callColor={tk.accent}
                 onTuneHz={onTuneHz} expanded={spotsExpanded} />
  ), [spotsKind, onTuneHz, spotsExpanded, tk.accent]);
  // Canvas header state — fed by DecoderImageCanvas callbacks (skin parity)
  const [imageInfo,   setImageInfo]   = useState('');
  const [hasPrev,     setHasPrev]     = useState(false);
  const [viewingPrev, setViewingPrev] = useState(false);
  const onTogglePrev = () => {
    if (viewingPrev) imageRef?.current?.showLive();
    else             imageRef?.current?.showPrev();
  };
  const onSave = () => { imageRef?.current?.save(); };

  /* ★★★ NAVTEX — A MESSAGE AT A TIME (Stuart, 2026-10-05): "a hybrid of RTTY & WEFAX … message arrives and is
   *  displayed with the option to save it … next message arrives previous message gets moved in the background and
   *  user can alternate between live receive or previous message like WEFAX." The text is RTTY's stream; the blocks
   *  (ZCZC…NNNN, damaged ones too) are found by utils/navtex, shared with the web client. LIVE is the message
   *  arriving (or the last one finished); PREV the one before it — one only, as WEFAX keeps one picture.
   *  ★ Between messages it says STANDING BY, as WEFAX does between charts. */
  const isNavtex = activeDecoder === 'navtex' && !!navtex && !isSpotsMode && !isDabMode && !isImageMode && !isAircraftMode;
  const [nvViewPrev, setNvViewPrev] = useState(false);
  const [, setNvTick] = useState(0);
  // ★ A lost NNNN ends the message after 75 s of silence (NAVTEX_END_LOST_MS) — nothing else would redraw the box
  //   then, so look every 5 s while NAVTEX is open. Nothing runs for any other decoder.
  useEffect(() => {
    if (!isNavtex) return;
    const id = setInterval(() => { if (navtex?.tick(Date.now())) setNvTick((n) => n + 1); }, 5000);
    return () => clearInterval(id);
  }, [isNavtex, navtex]);
  const nvPrevMsg = isNavtex ? navtex!.prev : null;
  const nvLiveMsg = isNavtex ? navtex!.live : null;
  const nvViewingPrev = nvViewPrev && !!nvPrevMsg;
  const nvShown: NavtexMessage | null = nvViewingPrev ? nvPrevMsg : nvLiveMsg;
  const nvReceiving = isNavtex && navtex!.receiving;
  // ★ Cleared (CLR) or reopened: back to LIVE — there is no previous any more.
  useEffect(() => { if (!decoderText) setNvViewPrev(false); }, [decoderText]);
  const onNavtexSave = async () => {
    const m = nvShown;
    if (!m) return;
    const name = navtexFileName(m);
    const body = navtexFileText(m);
    try {
      // ★ A REAL .txt FILE on iOS / Mac (Save to Files, AirDrop, Mail with an attachment) — the same reason
      //   DecoderImageCanvas.save writes a file rather than a data: URL. Android's share sheet ignores `url`, so
      //   there the message itself is shared as text (what SDRScreen's station share does for the same reason).
      if (Platform.OS === 'ios') {
        const f = new File(Paths.cache, name);
        try { f.create({ overwrite: true }); } catch {}
        f.write(body);
        await Share.share({ url: f.uri } as any, { subject: name } as any);
      } else {
        await Share.share({ title: name, message: body });
      }
    } catch {}
  };
  /* ★ Picture zoom over FIT — − / + in the header (Stuart, 2026-10-04). Back to fit when the decoder changes. */
  const IMG_ZOOMS = [1, 1.5, 2, 3, 4];
  const [imgZoomI, setImgZoomI] = useState(0);
  useEffect(() => { setImgZoomI(0); }, [activeDecoder]);
  /* ★★ WEFAX SHIFT / SLANT, per frequency (utils/wefaxAlign): the listener's own setting if saved, else the
   *  station preset (Northwood), else none. ADJ opens the strip; every change is saved at once. */
  const isWefax = activeDecoder === 'wefax';
  const alignKey = wefaxAlignKey(tunedHz);
  const [align, setAlign] = useState<WefaxAlign>(() => wefaxPreset(tunedHz));
  const [alignSaved, setAlignSaved] = useState(false);
  const [adjOpen, setAdjOpen] = useState(false);
  const [autoAl, setAutoAl] = useState<WefaxAlign | null>(null);   // ★ this chart's own (margin / border + slant)
  const autoShift = autoAl ? autoAl.shift : null;
  /* ★★ SHIFT IS PER CHART, SLANT PER STATION (Stuart, 2026-10-04, from FLDigi: "slant correction dialled in … you
   *  could move it across so the black line was at the edge, but then next decode happened and the position had
   *  shifted again and needed setting every time"). So only the slant is saved; each chart's margin is found
   *  automatically, and ALIGN (drag + 1 / 5 px keys) moves THIS chart only — cleared when the next chart's margin is found. */
  const [manualShift, setManualShift] = useState<number | null>(null);
  const onChartAlign = useCallback((a: WefaxAlign | null) => { setAutoAl(a); setManualShift(null); }, []);
  /* ★★ RAW (Stuart, 2026-10-06: "another button to remove all correction to just show the raw image as received.
   *  Reset defaults back to the auto settings we chose, and a No Correct or RAW button shows the image without any
   *  correction at all"). A toggle, PER CHART like the shift: the next chart comes in automatic. Nothing is
   *  discarded — auto-align, the manual shift and the saved slant all stay underneath and return when it goes off.
   *  Geometry only (utils/wefaxAlign drawnAlign); SAVE writes the picture shown, so RAW saves raw. */
  const [rawChart, setRawChart] = useState(false);
  const onNewChart = useCallback(() => { setRawChart(false); setManualShift(null); }, []);
  /* ★★ The slant drawn: the listener's own if saved for this frequency, else THIS chart's measured one (utils/wefaxAlign
   *  findMarginSlant — MadPsy/Stuart 2026-10-05: never tied to one radio's clock), else the station's. */
  const drawSlant = !alignSaved && autoAl ? autoAl.slant : align.slant;
  useEffect(() => {
    if (!isWefax || !tunedHz) return;
    let dead = false;
    AsyncStorage.getItem(alignKey).then((v: string | null) => {
      if (dead) return;
      let a: WefaxAlign | null = null;
      try { a = v ? parseAlign(JSON.parse(v)) : null; } catch { a = null; }
      setAlign(a ?? wefaxPreset(tunedHz)); setAlignSaved(!!a);
    }).catch(() => { if (!dead) { setAlign(wefaxPreset(tunedHz)); setAlignSaved(false); } });
    return () => { dead = true; };
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [isWefax, alignKey]);
  const changeAlign = (a: WefaxAlign | null) => {
    if (!a) { AsyncStorage.removeItem(alignKey).catch(() => {}); setAlign(wefaxPreset(tunedHz)); setAlignSaved(false); return; }
    const r = { shift: 0, slant: Math.round(a.slant * 1000) / 1000 };   // ★ only the slant is the station's
    setAlign(r); setAlignSaved(true);
    AsyncStorage.setItem(alignKey, JSON.stringify(r)).catch(() => {});
  };
  /* ★★★ ALIGN + HELD KEYS (Stuart, 2026-10-06: "those buttons are really hard to press and are finicky and cannot be
   *  held requiring multiple taps which then meant hitting the buttons next to it … on the ALIGN button have an
   *  overlay pop up over the chart <-----------> drag for rough alignment then use buttons to fine tune. Slant keeps
   *  the buttons, but they need to be made bigger and also be able to be held for larger adjustments").
   * ★★★ …AND ONLY THE KEYS IN USE (Stuart, same day, on the first cut: "the align buttons also need to be bigger too,
   *  same size as the slant ones, show only the ones in use. So when opening the adjust it shows the auto margin and
   *  auto slant figures and then you press to adjust them and then the arrows show"). ADJ opens a SUMMARY — the two
   *  figures actually applied (this chart's shift, the slant drawn), each a key, then AUTO and RAW (a mode pair). MARGIN opens the drag
   *  cover over the chart (DecoderImageCanvas alignPreview) with ◀ ▶ DONE; SLANT opens − + DONE; DONE goes back.
   *  Every arrow is the same 44 pt key and repeats while held, speeding up (useHoldRepeat). The shift is still THIS
   *  chart's only; the slant is still saved per frequency; AUTO (was RESET) returns both to automatic. */
  /* ★ The shift as the listener reads it: px, signed, the short way round on this chart's width (from "1809x…"). */
  const chartW = parseInt(imageInfo, 10) || 1809;
  const signedPx = (s: number) => { const v = Math.round((((s % chartW) + chartW + chartW / 2) % chartW) - chartW / 2);
                                    return `${v > 0 ? '+' : v < 0 ? '−' : ''}${Math.abs(v)}`; };
  const signedSlant = (k: number) => `${k > 0 ? '+' : k < 0 ? '−' : ''}${Math.abs(k).toFixed(3)}`;
  const [adjMode, setAdjMode] = useState<'summary' | 'align' | 'slant'>('summary');
  const aligning = adjMode === 'align';
  const curShift = manualShift ?? autoShift ?? 0;
  const shiftRef = useRef(curShift);
  shiftRef.current = curShift;
  const slantRef = useRef(drawSlant);
  slantRef.current = drawSlant;
  // ★ From refs, written through at once: a held key repeats faster than this box re-renders.
  const nudgeShift = (d: number) => { const s = shiftRef.current + d; shiftRef.current = s; setManualShift(s); };
  const nudgeSlant = (d: number) => {
    const k = Math.round((slantRef.current + d) * 1000) / 1000;
    slantRef.current = k; changeAlign({ shift: 0, slant: k });
  };
  const onAlignDrag = useCallback((s: number, done: boolean) => { if (done) setManualShift(s); }, []);
  const hold = useHoldRepeat();
  useEffect(() => { if (!adjOpen || !isWefax) setAdjMode('summary'); }, [adjOpen, isWefax]);
  const [minimised, setMinimised] = useState(false);
  const [dabSpeedOpen, setDabSpeedOpen] = useState(false);   // DAB speed-fix popup
  // ★★ The spots filters were CYCLERS: each tap advanced by one and you read the label to find
  // out where you had landed. BAND has ELEVEN options, so choosing 10m meant ten taps and ten
  // list redraws — and on an iPhone SE, where the run already has to scroll, that is the worst
  // possible way to pick from a list. They now open a popup and you choose. (Stuart, 2026-08-01.)
  // ★ SPEED FIX had been a popup since it was written; this makes the header consistent with the
  // one control in it that already did the right thing, rather than inventing a new pattern.
  const [sfOpen, setSfOpen] = useState<null | 'mode' | 'band' | 'age'>(null);

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
  /* ★★ FOLLOW THE NEW TEXT ONLY WHILE THE READER IS AT THE BOTTOM (B11, Stuart: "cannot scroll up to
   *  view history — as I attempt to scroll up the new content snaps my view back to the latest lines").
   *  Every new character used to scrollToEnd unconditionally. Now scrolling up stops the follow; coming
   *  back within ~24 pt of the end resumes it. CLR and opening the box start following again. */
  const followTail = useRef(true);
  const noteTail = (e: any) => {
    const n = e?.nativeEvent;
    if (!n?.contentSize || !n?.layoutMeasurement) return;
    const gap = n.contentSize.height - (n.contentOffset.y + n.layoutMeasurement.height);
    followTail.current = gap < 24;
  };
  const bodyScroll = {
    scrollEventThrottle: 32,
    onScroll: (e: any) => { bodyScrollY.current = e?.nativeEvent?.contentOffset?.y ?? 0; noteTail(e); },
    // ★ Resync from reality when the list settles or the user drags it. Without this, a target
    // that ran past the end leaves the arrows pressing against a wall — you would have to key
    // back through the overshoot before anything moved.
    onMomentumScrollEnd: (e: any) => { bodyScrollY.current = e?.nativeEvent?.contentOffset?.y ?? 0; noteTail(e); },
    onScrollEndDrag:     (e: any) => { bodyScrollY.current = e?.nativeEvent?.contentOffset?.y ?? 0; noteTail(e); },
    // ★ The finger going down is the reader's intent: stop following at once, before the next line lands.
    onScrollBeginDrag:   () => { followTail.current = false; },
  };

  // ★ One palette for every box (DecoderShell). The white-theme branch that lived here is gone:
  //   the boxes follow the faceplate now, whose default chassis is today's gold chrome
  //   (Decoder.mockup `isDef`) — the look DAB and RDS already had.
  const dc = {
    hdrBdr:  tk.hdrBdr ?? tk.divider,
    status:  tk.muted,
    btnBdr:  tk.chipBorder,
    btnBdrA: tk.chipBorderAct,
    btnAct:  tk.chipBgAct,
    btnTxt:  tk.keyText,
    btnActT: tk.keyTextAct,
    output:  tk.value,
  };


  // Appear / disappear
  const panelOn = !!activeDecoder || isSpotsMode || isDabMode;
  // ★ Tells the screen whether the box is OPEN (and not minimised) — on a short portrait window the station
  //   strip gives the box its room (SDRScreen boxHidesVts).
  useEffect(() => { onShownChange?.(panelOn && !minimised); }, [panelOn, minimised, onShownChange]);
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

  // Scroll to bottom when text grows.
  //
  // ★★★ GUARDED, AND THE TIMER IS CANCELLED ON UNMOUNT. Under the New Architecture an imperative
  // command dispatched to a view that has since DETACHED throws — and `outputRef.current?.` does
  // NOT protect against it, because the ref still holds a detached instance rather than null. A
  // throw from a timer or frame callback propagates out through JSI as a C++ exception, which
  // means it NEVER REACHES crashGuard's ErrorUtils handler and takes the whole process down with
  // a bare "abort() called" and no JS message.
  // ★★ Suspected cause of the crash Stuart hit on 2026-07-31: RTTY, tap BIG, tap CLR — clearing
  // the text re-runs this effect, and the 40 ms timer can land after the panel has gone.
  // ★ Cheap either way: cancelling the timer on unmount is correct regardless, and the try/catch
  // costs nothing on the happy path. A failed scroll is not worth a crash.
  // ★ Cleared or reopened: back to following the newest line.
  useEffect(() => { if (!decoderText || !minimised) followTail.current = true; }, [!decoderText, minimised]);
  useEffect(() => {
    // ★ NAVTEX's PREV is a finished message being READ — new text belongs to LIVE and must not move it.
    if (minimised || !followTail.current || nvViewingPrev) return;
    const t = setTimeout(() => {
      try { outputRef.current?.scrollToEnd({ animated: false }); } catch {}
    }, 40);
    return () => clearTimeout(t);
  }, [decoderText, minimised, nvViewingPrev]);

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
  // ★★ ENTER IS THE KEY WE NAME; Space still works but is no longer advertised anywhere
  // (Stuart, 2026-07-26: "now we don't have the spacebar so any references to it need
  // removing"). Documenting both meant naming a key that silently fails for anyone using Full
  // Keyboard Access, which is worse than naming one that always works. Space is kept in the
  // handlers because it costs nothing and is what a hand reaches for out of habit.
  //
  // ★★ ENTER WORKS TOO, added 2026-07-26. iOS Full Keyboard Access uses SPACE to activate the
  // focused element, so under FKA the space bar never reaches us and the box became unusable —
  // a list you can move through and cannot select from. Stuart: "space is not working in FKA
  // mode, so all those places you didn't want Enter, unfortunately it is needed."
  //
  // ★ The conflict that ruled Enter out does not actually arise HERE: the main screen stops
  // acting on keys whenever this box owns them (see region capture), so while the user is in
  // the list there is no tune box for Enter to open. Space stays the documented key because it
  // is what the on-screen hint has room for and what works in the normal case; Enter is a
  // silent second door, which is what you want for a fallback.
  //
  // ★ SPACE, NOT ENTER, was the original rule. Enter is the tune box across the whole app and
  // Stuart flagged the exception himself — a key that means something different depending on
  // where you are is the thing that has caused most of the confusion in this work. Space is
  // free, and "space activates the focused thing" is a convention rather than a rule to learn.
  const [kbZone, setKbZone] = useState<null | 'header' | 'list' | 'popup'>(null);
  const [speedIdx, setSpeedIdx] = useState(0);
  const [hdrIdx, setHdrIdx] = useState(0);
  const [listIdx, setListIdx] = useState(0);
  const hdrSlots = useRef<Array<() => void>>([]);
  hdrSlots.current = [];
  const [hdrCount, setHdrCount] = useState(0);
  useEffect(() => {
    if (hdrSlots.current.length !== hdrCount) setHdrCount(hdrSlots.current.length);
  });

  // ★★ THE HEADER RUN SCROLLS. On an iPhone SE — and worse, an SE in Display Zoom, which is the
  // narrowest layout the app ever draws — the header controls ran off the right edge. AGE was
  // half-cut and BIG, − and × were off-screen ENTIRELY: the box could not be resized, minimised
  // or CLOSED. A clipped decoration is untidy; a clipped × is a trap, and it is the reason this
  // is a blocker rather than a polish item. (Stuart, 2026-08-01, on the SE.)
  //
  // So: the variable-length run of controls scrolls horizontally, and MINIMISE and CLOSE are
  // PINNED OUTSIDE it. Whatever else happens, the two controls that get you out of the box are
  // always on screen. The chevrons only appear when there is genuinely something more to reach —
  // an arrow that cannot move is the same lie as a control that does nothing.
  const hdrScroll = useRef<ScrollView | null>(null);
  const [runW, setRunW] = useState(0);          // the visible width of the run
  const [runContentW, setRunContentW] = useState(0);
  const [runX, setRunX] = useState(0);          // current horizontal offset
  const runOverflow = runContentW > runW + 1;
  const canScrollL = runOverflow && runX > 1;
  const canScrollR = runOverflow && runX < runContentW - runW - 1;
  const nudge = (dir: 1 | -1) => {
    const step = Math.max(80, runW * 0.7);
    const to = Math.max(0, Math.min(runContentW - runW, runX + dir * step));
    hdrScroll.current?.scrollTo({ x: to, animated: true });
  };

  // Keyboard focus must drag the run with it, or the focus ring lands on a button that is
  // scrolled out of sight and the layer looks broken. This is not SE-only — a narrow Mac window
  // overflows too, and the keyboard layer is a Mac feature. Layouts are recorded only for
  // buttons INSIDE the run; the pinned ones live in another coordinate space.
  const hdrBtnX = useRef<Record<number, { x: number; width: number }>>({});
  const runIdx = useRef<Set<number>>(new Set());
  useEffect(() => {
    if (kbZone !== 'header') return;
    const l = hdrBtnX.current[hdrIdx];
    if (!l || !runIdx.current.has(hdrIdx) || runW <= 0) return;
    if (l.x < runX) hdrScroll.current?.scrollTo({ x: Math.max(0, l.x - 12), animated: true });
    else if (l.x + l.width > runX + runW)
      hdrScroll.current?.scrollTo({ x: l.x + l.width - runW + 12, animated: true });
  }, [kbZone, hdrIdx, runW, runX]);

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
        if (kbZoneRef.current === 'popup') { popCloseRef.current(); return; }
        // Where the box OWNS the arrows there is nothing to hand them back to, so these step
        // back to the list rather than leaving them tuning a locked VFO.
        if (autoOwnRef.current) { setKbZone('list'); return; }
        leave();
        return;
      }
      // The popup is a dropdown: it owns every arrow while open, Space picks, Backspace leaves.
      if (kbZoneRef.current === 'popup') {
        if (k === 'ArrowLeft' || k === 'ArrowUp') { setSpeedIdx(i => Math.max(0, i - 1)); return; }
        if (k === 'ArrowRight' || k === 'ArrowDown') { setSpeedIdx(i => Math.min(popLenRef.current - 1, i + 1)); return; }
        if (k === 'Space' || k === 'Enter') { popApplyRef.current(speedIdxRef.current); return; }
        if (k === 'Tab') { popCloseRef.current(); return; }
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
          // ★★ NOT ANIMATED, deliberately. An animated scroll emits onScroll frames while it
          // runs, and this ref is updated from those — so with key repeat each step computed
          // its next target from a MID-FLIGHT position and the list oscillated. Stepping
          // straight there removes the whole class: the repeat rate is what makes it look like
          // scrolling, and it does so more smoothly than fighting an animation.
          bodyScrollY.current = Math.max(0, bodyScrollY.current + (k === 'ArrowDown' ? 90 : -90));
          if (sv.scrollToOffset) sv.scrollToOffset({ offset: bodyScrollY.current, animated: false });
          else sv.scrollTo?.({ y: bodyScrollY.current, animated: false });
          return;
        }
        setKbZone('list');
        setListIdx(i => Math.max(0, Math.min(listLenRef.current - 1, i + (k === 'ArrowDown' ? 1 : -1))));
        return;
      }
      // ★★ T OPENS THE TUNE BOX (Stuart). Only needed where the panel owns the keyboard
      // OUTRIGHT — DAB and ADS-B — because there Enter is ours for selecting and can never
      // reach the main screen, leaving no way to change frequency at all. On the borrowed
      // decoders Enter still opens the tune box whenever the box does not hold the keyboard,
      // so they need nothing.
      if (k === 'T' && autoOwnRef.current) { onOpenFreqRef.current?.(); return; }
      if (k === 'Space' || k === 'Enter') {
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
  const onOpenFreqRef = useRef(onOpenFreq); onOpenFreqRef.current = onOpenFreq;
  const speedIdxRef = useRef(speedIdx); speedIdxRef.current = speedIdx;
  // ★ The DAB-speed-specific refs that used to live here are gone: popApplyRef/popCloseRef do the
  // same job for whichever popup is open, so there is one path to keep working rather than four.
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
    if (!dabSpeedOpen) { setKbZone(z => (z === 'popup' ? 'header' : z)); return; }
    const cur = DAB_SPEEDS.findIndex(o => Math.abs((dabSpeed ?? 1) - o.v) < 0.001);
    setSpeedIdx(cur >= 0 ? cur : 0);
    setKbZone('popup');
    announce();
  }, [dabSpeedOpen]);   // eslint-disable-line react-hooks/exhaustive-deps

  // ── The header popup ──────────────────────────────────────────────────────────
  // ★ ONE popup, described by whichever control opened it, so SPEED FIX and the three spots
  // filters share a single renderer and a single keyboard path. The alternative — four popups
  // that each grew their own arrow handling — is how the DAB one ended up reachable by mouse
  // but not by keyboard in the first place.
  // `wrap` is the one real difference: four speed presets sit happily in one row, eleven bands
  // do not.
  const popup: null | {
    title: string; wrap: boolean;
    opts: Array<{ key: string; label: string; on: boolean; apply: () => void }>;
    close: () => void;
  } =
    isDabMode && dabSpeedOpen ? {
      title: 'SPEED FIX · remembered per station', wrap: false,
      opts: DAB_SPEEDS.map(o => ({
        key: o.l, label: o.l, on: Math.abs((dabSpeed ?? 1) - o.v) < 0.001,
        apply: () => onDabSpeed?.(o.v),
      })),
      close: () => setDabSpeedOpen(false),
    }
    : sfOpen === 'mode' ? {
      title: 'MODE', wrap: true,
      opts: SF_MODES.map(m => ({ key: m, label: m, on: sfMode === m, apply: () => setSfMode(m) })),
      close: () => setSfOpen(null),
    }
    : sfOpen === 'band' ? {
      title: 'BAND', wrap: true,
      opts: SF_BANDS.map(b => ({ key: b, label: b, on: sfBand === b, apply: () => setSfBand(b) })),
      close: () => setSfOpen(null),
    }
    : sfOpen === 'age' ? {
      title: 'AGE · hide spots older than', wrap: true,
      opts: SF_AGES.map(a => ({
        key: a.label, label: a.minutes === 0 ? 'ANY' : a.label,
        on: sfAge === a.minutes, apply: () => setSfAge(a.minutes),
      })),
      close: () => setSfOpen(null),
    }
    : null;

  // Same hand-the-keys rule as SPEED FIX above, for the filters.
  useEffect(() => {
    if (!sfOpen) { setKbZone(z => (z === 'popup' ? 'header' : z)); return; }
    setSpeedIdx(Math.max(0, popup?.opts.findIndex(o => o.on) ?? 0));
    setKbZone('popup');
    announce();
  }, [sfOpen]);   // eslint-disable-line react-hooks/exhaustive-deps

  // Only one popup at a time, and none of them survive a decoder change — a BAND list left open
  // over a WEFAX image would be filtering a list that is no longer on screen.
  useEffect(() => { setSfOpen(null); setDabSpeedOpen(false); }, [activeDecoder, spotsKind, isDabMode]);

  const popCloseRef = useRef(() => {});
  popCloseRef.current = () => { popup?.close(); };
  const popLenRef = useRef(0);
  popLenRef.current = popup?.opts.length ?? 0;
  const popApplyRef = useRef((i: number) => {});
  popApplyRef.current = (i: number) => {
    const o = popup?.opts[i];
    if (!o) return;
    o.apply();
    popup?.close();
  };

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
  // ★ `run` marks a button as living INSIDE the scrolling run, so its layout is recorded in the
  // run's coordinate space and keyboard focus can scroll to it. It must be an explicit prop, not
  // a ref read during render: children render AFTER the parent's JSX is built, so a ref toggled
  // around the run would always read back false by the time these bodies actually run.
  // ★★ useCallback, so HBtn keeps its identity across the text updates that re-render this box
  //   (RTTY / FT8 arrive many times a second). A component defined inline is a NEW type every
  //   render, so React remounted every header key each time — which a dome key cannot survive:
  //   its snap and its 45 ms press click (useDomeKey) were torn down mid-press. Only the keyboard
  //   focus it draws can change what it renders.
  const HBtn = useCallback(({ onPress, style, children, run, ...rest }: any) => {
    const i = hdrSlots.current.length;
    hdrSlots.current.push(onPress ?? (() => {}));
    const on = kbZone === 'header' && hdrIdx === i;
    const mine = !!run;
    if (mine) runIdx.current.add(i); else runIdx.current.delete(i);
    return (
      <DecoderKey onPress={onPress}
        onLayout={mine ? (e: any) => {
          const { x, width } = e.nativeEvent.layout;
          hdrBtnX.current[i] = { x, width };
        } : undefined}
        style={[style, on && { borderColor: NAV_FOCUS, borderWidth: 2 }]} {...rest}>
        {children}
      </DecoderKey>
    );
  }, [kbZone, hdrIdx]);

  if (!panelOn) return null;

  const title = isDabMode
    ? 'DAB'
    : isSpotsMode
    ? (spotsKind === 'cw' ? 'CW SPOTS' : 'DIGITAL SPOTS')
    : (DECODER_LABELS[activeDecoder!] ?? String(activeDecoder).toUpperCase());

  return (
    <DecoderShell bottom={bottomOffset} maxWidth={PANEL_MAX_W} tall={tall}
                  borderColor={kbZone ? NAV_FOCUS : undefined}
                  wrapStyle={{ opacity, transform: [{ translateY: slideY }] }}
                  onTouchStart={noteTouchInteraction}>

        {/* ★ Arrival / departure flash. A border that brightens once and fades, so taking the
            keyboard and handing it back are both announced. pointerEvents none — it is a
            signal, never a target. */}
        <Animated.View pointerEvents="none"
          style={[dp.flash, { opacity: flash }]} />

        {/* Header */}
        <DecoderHeader onPress={() => setMinimised((p: boolean) => !p)}>
          {/* Status dot */}
          <View style={[dp.dot, decoding && dp.dotOn]} />

          {/* Title */}
          <DecoderTitle minimised={minimised}>{title}</DecoderTitle>

          {/* Status text — directly after title (skin layout).
              ★ DAB has no "listening…" state to report: it is not hunting for a signal,
              the multiplex either decodes or it does not. So that slot carries the
              MULTIPLEX NAME instead, matching the watch's DabView, which is the one
              thing you actually want to read there. */}
          <Text style={[dp.status, dp.statusGrow, dp.hdrStatus]}
                numberOfLines={1}>
            {kbZone
              // ★ Shown for EVERY decoder, not just DAB. On RTTY the box took the keyboard
              // and said nothing about it — the CLR button responded to space, so it worked,
              // but nothing told you it would. A box that has the keyboard should say so
              // whatever it is showing. (Stuart, 2026-07-25.)
              // ★ Always says where Tab goes NEXT, so the cycle is discoverable by using it
              // rather than by being remembered. The wording differs by zone AND by whether
              // the box owns the arrows or merely borrowed them.
              ? (kbZone === 'popup' ? 'enter to set · backspace to cancel'
                 : kbZone === 'header' ? (autoOwn ? (isDabMode ? 'enter to press · tab for stations · t to tune'
                                                               : 'enter to press · tab for the list · t to tune')
                                                  : 'enter to press · tab to leave')
                 : listLen > 0        ? (autoOwn ? 'enter to select · tab for controls · t to tune'
                                                  : 'enter to select · tab for controls')
                 : 'scroll with ↑↓ · tab for controls')
              : isDabMode ? (dabEnsemble || 'reading multiplex…')
              // ★ NAVTEX says where it is in the broadcast; a refusal or an error still speaks for itself.
              : isNavtex && !/^(error|not started)/.test(decoderStatus) ? (nvReceiving ? 'receiving' : 'standing by')
              : decoderStatus}
          </Text>

          {/* ── The scrolling control run ─────────────────────────────────────────────────
              Everything variable-length lives in here. MINIMISE and CLOSE do NOT — see the
              note by `hdrScroll`. ‹ and › appear only when there is more to reach. */}
          {canScrollL && (
            <TouchableOpacity hitSlop={8} style={dp.runArrow}
              onPress={(e: any) => { e?.stopPropagation(); nudge(-1); }}>
              <Text style={dp.runArrowTxt}>‹</Text>
            </TouchableOpacity>
          )}
          <ScrollView
            ref={hdrScroll}
            horizontal
            showsHorizontalScrollIndicator={false}
            keyboardShouldPersistTaps="always"
            style={dp.btnScroll}
            contentContainerStyle={dp.btnScrollContent}
            onLayout={(e: any) => setRunW(e.nativeEvent.layout.width)}
            onContentSizeChange={(w: number) => setRunContentW(w)}
            onScroll={(e: any) => setRunX(e.nativeEvent.contentOffset.x)}
            scrollEventThrottle={16}
          >
          {/* EXPAND LEADS, and stays outside the filter run. It changes how each row is DRAWN;
              MODE/BAND/AGE change WHICH rows are listed. Sitting it between two cyclers read as
              a fourth filter. */}
          {isSpotsMode && spotsKind === 'digi' && (
            <HBtn active={spotsExpanded} run hitSlop={6}
              onPress={(e: any) => { e?.stopPropagation(); setSpotsExpanded(v => !v); }}>
              <DecoderKeyLabel active={spotsExpanded}>
                {spotsExpanded ? 'COLLAPSE' : 'EXPAND'}
              </DecoderKeyLabel>
            </HBtn>
          )}
          {/* Spots filter cyclers (skin sf-mode / sf-band / sf-age dropdowns) */}
          {isSpotsMode && spotsKind === 'digi' && (
            <HBtn active={sfMode !== 'ALL'} run hitSlop={6}
              onPress={(e: any) => {
                e?.stopPropagation();
                setSfOpen(o => (o === 'mode' ? null : 'mode'));
              }}>
              <DecoderKeyLabel active={sfMode !== 'ALL'}>
                {sfMode === 'ALL' ? 'MODE' : sfMode}
              </DecoderKeyLabel>
            </HBtn>
          )}
          {isSpotsMode && (
            <HBtn active={sfBand !== 'ALL'} run hitSlop={6}
              onPress={(e: any) => {
                e?.stopPropagation();
                setSfOpen(o => (o === 'band' ? null : 'band'));
              }}>
              <DecoderKeyLabel active={sfBand !== 'ALL'}>
                {sfBand === 'ALL' ? 'BAND' : sfBand}
              </DecoderKeyLabel>
            </HBtn>
          )}
          {isSpotsMode && (
            <HBtn active={sfAge > 0} run hitSlop={6}
              onPress={(e: any) => {
                e?.stopPropagation();
                setSfOpen(o => (o === 'age' ? null : 'age'));
              }}>
              <DecoderKeyLabel active={sfAge > 0}>
                {SF_AGES.find(a => a.minutes === sfAge)?.label ?? 'AGE'}
              </DecoderKeyLabel>
            </HBtn>
          )}

          {/* CLR — text decoders (skin _clearB) */}
          {!isImageMode && !isSpotsMode && !isDabMode && (
            <HBtn run hitSlop={6}
              onPress={(e: any) => { e?.stopPropagation(); onClear?.(); }}>
              <DecoderKeyLabel>CLR</DecoderKeyLabel>
            </HBtn>
          )}

          {/* DAB speed correction (§4.5) — opens the SPEED FIX popup (separate preset
              buttons), a popup like the spots filters. Highlighted when not Off. */}
          {isDabMode && onDabSpeed && (
            <HBtn active={Math.abs((dabSpeed ?? 1) - 1) > 0.001} run hitSlop={6}
              onPress={(e: any) => { e?.stopPropagation(); setDabSpeedOpen(o => !o); }}>
              <DecoderKeyLabel active={Math.abs((dabSpeed ?? 1) - 1) > 0.001}>
                SPEED FIX
              </DecoderKeyLabel>
            </HBtn>
          )}

          {/* Morse quality filter (skin lsv-dec-sf-quality) */}
          {activeDecoder === 'morse' && (
            <HBtn run hitSlop={6}
              onPress={(e: any) => {
                e?.stopPropagation();
                const i = MORSE_QUALITIES.indexOf(morseQuality);
                onMorseQuality?.(MORSE_QUALITIES[(i + 1) % MORSE_QUALITIES.length]);
              }}>
              <DecoderKeyLabel tone="accent">
                {MORSE_QUALITY_LABELS[morseQuality]}
              </DecoderKeyLabel>
            </HBtn>
          )}

          {/* ★ NAVTEX PREV/LIVE + SAVE — WEFAX's pair, for a message. PREV only once there is one; SAVE only
              with a message on screen (never a dead key). */}
          {isNavtex && !!nvPrevMsg && (
            <HBtn run hitSlop={6} accessibilityLabel={nvViewingPrev ? 'Show the live message' : 'Show the previous message'}
              onPress={(e: any) => {
                e?.stopPropagation();
                const toPrev = !nvViewingPrev;
                setNvViewPrev(toPrev);
                // PREV opens at its top (it is read from the start); LIVE goes back to following the newest text.
                followTail.current = !toPrev;
                requestAnimationFrame(() => {
                  try {
                    if (toPrev) outputRef.current?.scrollTo({ y: 0, animated: false });
                    else outputRef.current?.scrollToEnd({ animated: false });
                  } catch {}
                });
              }}>
              <DecoderKeyLabel>{nvViewingPrev ? 'LIVE' : 'PREV'}</DecoderKeyLabel>
            </HBtn>
          )}
          {isNavtex && !!nvShown && (
            <HBtn run hitSlop={6} accessibilityLabel="Save this message as a text file"
              onPress={(e: any) => { e?.stopPropagation(); onNavtexSave(); }}>
              <DecoderKeyLabel tone="accent">SAVE</DecoderKeyLabel>
            </HBtn>
          )}

          {/* PREV/LIVE + SAVE — image decoders (skin _prevB/_saveB) */}
          {isImageMode && hasPrev && (
            <HBtn run hitSlop={6}
              onPress={(e: any) => { e?.stopPropagation(); onTogglePrev?.(); }}>
              <DecoderKeyLabel>
                {viewingPrev ? 'LIVE' : 'PREV'}
              </DecoderKeyLabel>
            </HBtn>
          )}
          {isImageMode && (
            <HBtn run hitSlop={6}
              onPress={(e: any) => { e?.stopPropagation(); onSave?.(); }}>
              <DecoderKeyLabel tone="accent">SAVE</DecoderKeyLabel>
            </HBtn>
          )}
          {/* ★ Zoom out / in over the fitted picture. − only once zoomed (never a dead key); + stops at 4×. */}
          {isImageMode && imgZoomI > 0 && (
            <HBtn run hitSlop={6} accessibilityLabel="Zoom out"
              onPress={(e: any) => { e?.stopPropagation(); setImgZoomI((i) => Math.max(0, i - 1)); }}>
              <DecoderKeyLabel>−</DecoderKeyLabel>
            </HBtn>
          )}
          {isImageMode && isWefax && (
            <HBtn run hitSlop={6} accessibilityLabel="Align the chart and set its slant"
              onPress={(e: any) => { e?.stopPropagation(); setAdjOpen((o) => !o); }}>
              <DecoderKeyLabel active={adjOpen}>ADJ</DecoderKeyLabel>
            </HBtn>
          )}
          {isImageMode && imgZoomI < IMG_ZOOMS.length - 1 && (
            <HBtn run hitSlop={6} accessibilityLabel="Zoom in"
              onPress={(e: any) => { e?.stopPropagation(); setImgZoomI((i) => Math.min(IMG_ZOOMS.length - 1, i + 1)); }}>
              <DecoderKeyLabel active={imgZoomI > 0}>{imgZoomI > 0 ? `+ ${IMG_ZOOMS[imgZoomI]}×` : '+'}</DecoderKeyLabel>
            </HBtn>
          )}
          {/* ★★ BIG / SMALL — offered for EVERY decoder, not just images. See the block at the top
              of this component for why the 200 pt cap was wrong on large screens. */}
          {bigUseful && (
          <HBtn active={tall} run hitSlop={6}
            onPress={(e: any) => { e?.stopPropagation(); setTall(!tall); }}>
            <DecoderKeyLabel active={tall}>
              {tall ? 'SMALL' : 'BIG'}
            </DecoderKeyLabel>
          </HBtn>
          )}
          {isImageMode && !!imageInfo && (
            <Text style={[dp.status, dp.hdrStatus]} numberOfLines={1}>
              {imageInfo}
            </Text>
          )}
          </ScrollView>
          {canScrollR && (
            <TouchableOpacity hitSlop={8} style={dp.runArrow}
              onPress={(e: any) => { e?.stopPropagation(); nudge(1); }}>
              <Text style={dp.runArrowTxt}>›</Text>
            </TouchableOpacity>
          )}

          {/* ★ PINNED — outside the run, so the way out of the box is never scrolled away. */}
          {/* Minimise / restore (skin _minB: − / □) */}
          <HBtn
            hitSlop={8}
            onPress={(e: any) => { e?.stopPropagation(); setMinimised((p: boolean) => !p); }}
          >
            <DecoderKeyLabel>
              {minimised ? '□' : '−'}
            </DecoderKeyLabel>
          </HBtn>

          {/* Close — stops the decoder and dismisses the panel (see dismissDecoderPanel).
              ★ NOT shown for DAB or ADS-B: there the whole profile IS the decoder, so closing the
              box would leave the receiver in a mode with nothing to show and no obvious way back.
              Every other decoder is something layered ON a mode you can happily return to, on
              every server type — hence no backend condition here. */}
          {!isDabMode && !isAircraftMode && (
            <HBtn
              hitSlop={8}
              onPress={(e: any) => { e?.stopPropagation(); onClose(); }}
            >
              <DecoderKeyLabel tone="close">×</DecoderKeyLabel>
            </HBtn>
          )}
        </DecoderHeader>

        {/* ★ With Transparency OFF on silver / black the body sits in the recessed dark window (§10.2); on glass
            DecoderBody is nothing at all. Not drawn when minimised — an empty window is not "hidden". */}
        {!minimised && (<DecoderBody>
        {/* Body — hidden when minimised; image canvas for WEFAX/SSTV */}
        {!minimised && isImageMode && isWefax && adjOpen && (
          /* ★ One row at a time (see 'ONLY THE KEYS IN USE'): the summary, or MARGIN's keys, or SLANT's. Every key the
             same 44 pt, 12 pt apart. ★ MARGIN has ◀ ▶ only, no 5 px pair: on the narrowest phone (SE, Display Zoom:
             ~284 pt for the row) four arrows + DONE at this size do not fit on one line, and a held 1 px key goes
             ×5 after eight repeats anyway — and the drag is the coarse move. */
          <View style={dp.adjRow}>
            {adjMode === 'align' && !viewingPrev ? (<>
              <HBtn run hitSlop={4} style={dp.arrowKey} accessibilityLabel="Move the chart left (hold to repeat)"
                {...hold((m) => nudgeShift(m))}><DecoderKeyLabel style={dp.bigKeyTxt}>◀</DecoderKeyLabel></HBtn>
              <HBtn run hitSlop={4} style={dp.arrowKey} accessibilityLabel="Move the chart right (hold to repeat)"
                {...hold((m) => nudgeShift(-m))}><DecoderKeyLabel style={dp.bigKeyTxt}>▶</DecoderKeyLabel></HBtn>
              <Text style={[dp.status, dp.adjLabel]} numberOfLines={1}>{`${signedPx(curShift)} px`}</Text>
              <HBtn run hitSlop={4} style={dp.bigKey} accessibilityLabel="Finish aligning"
                onPress={() => setAdjMode('summary')}><DecoderKeyLabel active style={dp.bigKeyTxt}>DONE</DecoderKeyLabel></HBtn>
            </>) : adjMode === 'slant' ? (<>
              <HBtn run hitSlop={4} style={dp.arrowKey} accessibilityLabel="Slant less (hold to repeat)"
                {...hold((m) => nudgeSlant(-SLANT_STEP * m))}><DecoderKeyLabel style={dp.bigKeyTxt}>−</DecoderKeyLabel></HBtn>
              <HBtn run hitSlop={4} style={dp.arrowKey} accessibilityLabel="Slant more (hold to repeat)"
                {...hold((m) => nudgeSlant(SLANT_STEP * m))}><DecoderKeyLabel style={dp.bigKeyTxt}>+</DecoderKeyLabel></HBtn>
              <Text style={[dp.status, dp.adjLabel]} numberOfLines={1}>{signedSlant(drawSlant)}</Text>
              <HBtn run hitSlop={4} style={dp.bigKey} accessibilityLabel="Finish setting the slant"
                onPress={() => setAdjMode('summary')}><DecoderKeyLabel active style={dp.bigKeyTxt}>DONE</DecoderKeyLabel></HBtn>
            </>) : (<>
              {/* ★ The figures APPLIED: this chart's shift (auto-align's, or the listener's) and the slant drawn. */}
              {!viewingPrev && (
                <HBtn run hitSlop={4} style={dp.bigKey} accessibilityLabel="Adjust the margin: drag the chart sideways"
                  onPress={() => { setRawChart(false); setAdjMode('align'); }}>
                  <DecoderKeyLabel active={!rawChart && manualShift != null} style={dp.bigKeyTxt}>
                    {rawChart ? 'MARGIN RAW' : `MARGIN ${manualShift != null ? 'MANUAL' : 'AUTO'} ${signedPx(curShift)} px`}
                  </DecoderKeyLabel>
                </HBtn>
              )}
              <HBtn run hitSlop={4} style={dp.bigKey} accessibilityLabel="Adjust the slant"
                onPress={() => { setRawChart(false); setAdjMode('slant'); }}>
                <DecoderKeyLabel active={!rawChart && alignSaved} style={dp.bigKeyTxt}>
                  {rawChart ? 'SLANT RAW' : `SLANT ${alignSaved ? 'MANUAL' : 'AUTO'} ${signedSlant(drawSlant)}`}
                </DecoderKeyLabel>
              </HBtn>
              {/* ★ AUTO and RAW are a MODE PAIR (Stuart, 2026-10-06 — AUTO replaces RESET): AUTO is lit while nothing
                  manual and no RAW is in effect, and pressing it goes back to automatic from either. */}
              <HBtn run hitSlop={4} style={dp.bigKey} accessibilityLabel="Automatic margin and slant"
                accessibilityState={{ selected: !rawChart && !alignSaved && manualShift == null }}
                onPress={() => { changeAlign(null); setManualShift(null); setRawChart(false); }}>
                <DecoderKeyLabel active={!rawChart && !alignSaved && manualShift == null} style={dp.bigKeyTxt}>AUTO</DecoderKeyLabel>
              </HBtn>
              <HBtn run hitSlop={4} style={dp.bigKey} accessibilityLabel="Show the chart exactly as received, no correction"
                accessibilityState={{ selected: rawChart }}
                onPress={() => setRawChart((r) => !r)}><DecoderKeyLabel active={rawChart} style={dp.bigKeyTxt}>RAW</DecoderKeyLabel></HBtn>
            </>)}
          </View>
        )}
        {!minimised && isImageMode && imageRef && (
          <View style={dp.bodyContent}>
            <DecoderImageCanvas
              align={isWefax ? { shift: manualShift ?? 0, slant: drawSlant } : undefined}
              autoMargin={isWefax && manualShift == null}
              onAutoAlign={onChartAlign}
              autoSlant={!alignSaved}
              raw={isWefax && rawChart}
              onNewChart={onNewChart}
              alignPreview={isWefax && adjOpen && aligning && !viewingPrev ? curShift : undefined}
              onAlignDrag={onAlignDrag}
              ref={imageRef}
              maxHeight={bodyH}
              decoderName={activeDecoder ?? 'image'}
              zoom={IMG_ZOOMS[imgZoomI]}
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
          <View style={[dp.bodyContent, { height: bodyH }]}>
            <AircraftPanel aircraft={aircraft!} scrollRef={aircraftRef} />
          </View>
        )}
        {/* ★ NAVTEX: one message at a time — its title line (station · subject · serial, and how much was lost),
            then the text. Standing by until the first one arrives. */}
        {!minimised && isNavtex && (
          <ScrollView
            ref={outputRef}
            style={[dp.body, bodySize]}
            contentContainerStyle={[dp.bodyContent, scrollLane]}
            {...bodyScroll}
            showsVerticalScrollIndicator
          >
            {nvShown ? (
              <>
                <View style={[dp.nvHead, { borderBottomColor: tk.divider }]}>
                  <Text style={[dp.nvTitle, { color: tk.accent, fontFamily: FONT }]} numberOfLines={2}>
                    {navtexTitle(nvShown)}
                  </Text>
                  <Text style={[dp.nvMeta, { color: tk.muted, fontFamily: FONT }]} numberOfLines={1}>
                    {/* ★ Where it is, under the title too — on an SE the header's status is the first thing squeezed out. */}
                    {[nvViewingPrev ? 'PREVIOUS' : !nvShown.done ? 'RECEIVING' : 'STANDING BY',
                      navtexLostPct(nvShown) != null ? `${navtexLostPct(nvShown)}% lost` : '']
                      .filter(Boolean).join(' · ')}
                  </Text>
                </View>
                <Text style={[dp.output, { color: dc.output, fontFamily: FONT }]} selectable>
                  {navtexBody(nvShown)}
                </Text>
              </>
            ) : (
              <Text style={[dp.output, dp.spotEmpty, { color: dc.status, fontFamily: FONT }]}>
                standing by — each message appears here as it arrives
              </Text>
            )}
          </ScrollView>
        )}
        {!minimised && !isNavtex && !isImageMode && !isSpotsMode && !isAircraftMode && !isDabMode && (
          <ScrollView
            ref={outputRef}
            // ★★★ minHeight AS WELL AS maxHeight. As a cap alone, BIG did NOTHING on a text
            // decoder until the output already overflowed 200 pt — so on RTTY with a few lines
            // Stuart pressed it repeatedly and nothing moved, which is indistinguishable from a
            // broken button. In BIG the box takes the room whether or not there is text to fill
            // it; in SMALL it goes back to sizing itself.
            style={[dp.body, bodySize]}
            contentContainerStyle={[dp.bodyContent, scrollLane]}
            {...bodyScroll}
            showsVerticalScrollIndicator
          >
            <Text style={[dp.output, { color: dc.output, fontFamily: FONT }]} selectable>
              {decoderText}
            </Text>
          </ScrollView>
        )}

        {/* DAB service list (§5.2) — logos resolve cleanly because DAB programme names are
            EXACT ensemble strings (no RDS guessing like FM). Tap a row to switch service. */}
        {/* The header popup — SPEED FIX (§4.5, the dablin/OWRX chipmunk misread, remembered per
            station) or one of the spots filters. Drawn as a strip under the header rather than
            as a floating overlay: the box already sits above the control bar and an overlay
            would need measuring to avoid going off the top of an SE. */}
        {!minimised && popup && (
          <View style={[dp.dabSpeedPop, { borderBottomColor: dc.hdrBdr }]}>
            <Text style={[dp.dabSpeedTitle, { color: dc.status, fontFamily: FONT }]}>{popup.title}</Text>
            <View style={popup.wrap ? dp.popWrapRow : dp.dabSpeedRow}>
              {popup.opts.map((o, oi) => (
                <TouchableOpacity key={o.key}
                  style={[popup.wrap ? dp.popChip : dp.dabSpeedBtn,
                          { borderColor: o.on ? dc.btnBdrA : dc.btnBdr }, o.on && { backgroundColor: dc.btnAct },
                          kbZone === 'popup' && speedIdx === oi && { borderColor: NAV_FOCUS, borderWidth: 2 }]}
                  onPress={() => { o.apply(); popup.close(); }} activeOpacity={0.7}>
                  <Text style={[dp.dabSpeedBtnTxt, { color: o.on ? dc.btnActT : dc.btnTxt, fontFamily: FONT }]}>
                    {o.label}
                  </Text>
                </TouchableOpacity>
              ))}
            </View>
          </View>
        )}
        {!minimised && isDabMode && (
          <ScrollView ref={dabScroll} style={[dp.body, bodySize]} showsVerticalScrollIndicator {...bodyScroll}
            /* ★ scroll lane: full-width rows, each inset 12 pt by dabRow's own padding */>
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
                  <Text style={[dp.dabName, { color: active ? dc.btnActT : dc.output, fontFamily: FONT }]}
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
            style={[dp.body, bodySize]}
            // ★ The indicator's lane: a spot row's columns run to its right edge (10 pt inset only).
            contentContainerStyle={scrollLane}
            {...bodyScroll}
            data={visibleSpots}
            // ★★ NO INDEX IN THE KEY. Spots are newest-first, so a burst PREPENDS rows and
            // every index shifts — with the index in the key, every existing row got a new
            // identity, the whole list was treated as new, and the scroll position went with
            // it. On UberSDR a burst is every band at once, which is why it looked violent.
            keyExtractor={(sp: SpotRow) => `${sp.time}-${sp.call}-${sp.freqHz}`}
            // ★ And this is the property built for exactly this: content added ABOVE must not
            // move what you are looking at. Within 40px of the top it still follows the live
            // feed, so sitting at the top behaves as before — it is only reading further down
            // that is now left alone. (Stuart's diagnosis: "all the bursts of new contacts try
            // and pull that list rapidly to the top again".)
            maintainVisibleContentPosition={{ minIndexForVisible: 0, autoscrollToTopThreshold: 40 }}
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
              <Text style={[dp.output, dp.spotEmpty, { color: dc.status, fontFamily: FONT }]}>
                waiting for spots…
              </Text>
            }
          />
        )}
        </DecoderBody>)}

    </DecoderShell>
  );
}

/**
 * ★★ A KEY THAT REPEATS WHILE HELD (Stuart, 2026-10-06: the WEFAX adjust keys "cannot be held requiring multiple
 * taps"). `hold(fn)` gives a key's press handlers: fn(1) on touch-down, then after HOLD_DELAY_MS again every
 * HOLD_EVERY_MS, and from the HOLD_FAST_AFTER-th repeat with a step of HOLD_FAST (so a long hold covers ground and
 * a tap is still the finest step). A keyboard / screen-reader press (onPress with no touch-down) steps once.
 */
const HOLD_DELAY_MS = 380, HOLD_EVERY_MS = 90, HOLD_FAST_AFTER = 8, HOLD_FAST = 5;
function useHoldRepeat() {
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const touched = useRef(false);
  const stop = useCallback(() => { if (timer.current) clearTimeout(timer.current); timer.current = null; }, []);
  useEffect(() => stop, [stop]);
  return useCallback((fn: (mult: number) => void) => ({
    onPressIn: () => {
      stop(); touched.current = true; fn(1);
      let n = 0;
      const tick = () => { n++; fn(n >= HOLD_FAST_AFTER ? HOLD_FAST : 1); timer.current = setTimeout(tick, HOLD_EVERY_MS); };
      timer.current = setTimeout(tick, HOLD_DELAY_MS);
    },
    onPressOut: stop,
    onPress: () => { if (touched.current) { touched.current = false; return; } fn(1); },
  }), [stop]);
}

// ── Styles ────────────────────────────────────────────────────────────────────

/** ★ Built once per setting (useDecoderStyles) — never per render, never at load. */
const makeDp = (T: DecoderTokens) => StyleSheet.create({
  /* ★ The WEFAX SHIFT / SLANT strip under the header (ADJ). Wraps on a narrow box rather than clipping. */
  adjRow: { flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', gap: 12, paddingHorizontal: 8, paddingVertical: 6 },
  adjLabel: { minWidth: 70, flexShrink: 1 },
  /* ★ The adjust keys (2026-10-06): a 44 pt touch target each (Apple's minimum), 12 pt apart (adjRow gap) — the
   *  header keys' size was what made "hitting the buttons next to it" so easy. Same DecoderKey, same chassis look. */
  bigKey: { minWidth: 48, minHeight: 44, paddingHorizontal: 10, alignItems: 'center', justifyContent: 'center' },
  bigKeyTxt: { fontSize: 14 },
  /* ★ An arrow key: the same 44 pt key, wider, so a thumb on ◀ cannot reach ▶. */
  arrowKey: { minWidth: 64, minHeight: 44, paddingHorizontal: 10, alignItems: 'center', justifyContent: 'center' },
  // Sits over the whole box; only ever an opacity animation, so it stays on the native driver.
  flash: {
    position: 'absolute', left: 0, right: 0, top: 0, bottom: 0,
    borderWidth: 2, borderColor: NAV_FOCUS, borderRadius: 8,
  },
  /* ★ Wrap, frame, header, title and the header keys' boxes are DecoderShell's. */
  dot:       { width: 6, height: 6, borderRadius: 3, backgroundColor: T.dotIdle, flexShrink: 0 },
  dotOn:     { backgroundColor: T.dotOn, shadowColor: '#55d98d', shadowOpacity: 0.60, shadowRadius: 4, shadowOffset: { width:0, height:0 } },
  // flexShrink with an auto basis is what makes the priority come out right: the status text is
  // flex:1 (basis 0) so it only ever takes SPARE room, while the run keeps its natural width and
  // gives ground only when there is none. On a wide panel the status fills the gap; on the SE it
  // collapses and the controls get the space, which is the correct order of importance.
  btnScroll: { flexShrink: 1, flexGrow: 0 },
  btnScrollContent: { flexDirection: 'row', gap: 5, alignItems: 'center' },
  // The chevrons are affordances, not buttons in the visual sense — no border, so they read as
  // "there is more that way" rather than as two more controls to understand.
  runArrow:    { paddingHorizontal: 2, paddingVertical: 3, flexShrink: 0 },
  // On the header, so the header's colour (engraved on metal); a chevron is not a key.
  runArrowTxt: { fontFamily: FONT, fontSize: 15, lineHeight: 17,
                 color: T.keyLook === 'outline' ? T.keyText : T.title, ...engraveStyle(T) },
  hbtnTxt:       { fontFamily: FONT, fontSize: 11, color: T.keyText },
  status:     { fontSize: 9, letterSpacing: 1, color: T.muted, flexShrink: 1, overflow: 'hidden' },
  // The status in the HEADER sits on the metal under OFF on silver / black: engraved (§10.2).
  hdrStatus:  { color: T.hdrMuted, fontFamily: FONT, ...engraveStyle(T) },
  statusGrow: { flex: 1 },
  // Spots table
  // The cells now live in the inner `spotLine`, so the row box itself must NOT be a row — a nested
  // row would shrink to its content and the right-hand cells would lose their alignment.
  spotRow: {
    paddingHorizontal: 10, paddingVertical: 4,
    borderBottomWidth: StyleSheet.hairlineWidth,
    borderBottomColor: T.divider,
  },
  // Expanded rows keep the same horizontal padding and divider; only the vertical box grows.
  spotRowTall: {
    paddingHorizontal: 10, paddingVertical: 4,
    borderBottomWidth: StyleSheet.hairlineWidth,
    borderBottomColor: T.divider,
  },
  spotLine:    { flexDirection: 'row', alignItems: 'center', gap: 6 },
  // Indented to the width of the time cell so it reads as belonging to the row above rather than
  // as a row of its own, and dimmer so a collapsed-style scan still skims past it.
  // Line 2 is prose to READ, not a column to scan, so it gets a readable size rather than a
  // decorative one. Still stepped back from line 1, but by opacity alone now.
  spotDetail:  { fontSize: 11, letterSpacing: 0.2, color: 'rgba(255,255,255,0.62)',
                 marginLeft: 44, marginTop: 3 },
  spotEmpty:   { padding: 12, textAlign: 'center' },
  /* ★ NAVTEX's title line: what the message is on the left, how it arrived on the right; wraps rather than
   *  clipping on the SE. A hairline under it, as the list rows have. */
  nvHead:      { flexDirection: 'row', flexWrap: 'wrap', alignItems: 'baseline', columnGap: 10, rowGap: 2,
                 paddingBottom: 6, marginBottom: 6, borderBottomWidth: StyleSheet.hairlineWidth },
  nvTitle:     { flexShrink: 1, fontSize: 12, fontWeight: '700', letterSpacing: 0.6 },
  nvMeta:      { marginLeft: 'auto', fontSize: 10, letterSpacing: 1 },
  // 7-column layout (Time·Call·Band·Mode·SNR·Country·Distance) — fixed widths sized
  // for the SE's ~340pt panel; call + country flex the remainder
  // ★ Data WHITE, callsign AMBER — the reverse of the original. Dim amber on black at 10pt was
  // unreadable on a 17 Pro Max even at arm's length: the colour was doing the work of a hierarchy
  // that weight and size should do. Now the data reads plainly and the amber marks the one field
  // you scan for.
  // ★ tabular-nums: time, SNR and distance are columns and must line up down the table (§10.2).
  spotCell:    { fontSize: 11, letterSpacing: 0.3, color: 'rgba(255,255,255,0.88)', fontVariant: ['tabular-nums'] },
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
  closeBtn: { color: T.close, fontSize: 16, paddingHorizontal: 2, flexShrink: 0 },
  // ★ maxHeight is applied INLINE from bodyH (see the BIG/SMALL block); this keeps the rest of
  // the style and no longer hardcodes the cap.
  body:        {},
  bodyContent: { padding: 12 },
  dabRow:      { flexDirection: 'row', alignItems: 'center', gap: 10, paddingVertical: 9, paddingHorizontal: 12, borderBottomWidth: StyleSheet.hairlineWidth },
  dabName:     { flex: 1, fontSize: 14 },
  dabSpeedPop: { paddingVertical: 8, paddingHorizontal: 12, borderBottomWidth: StyleSheet.hairlineWidth },
  dabSpeedTitle: { fontSize: 10, letterSpacing: 0.5, marginBottom: 6 },
  dabSpeedRow: { flexDirection: 'row', gap: 6 },
  dabSpeedBtn: { flex: 1, borderWidth: 1, borderRadius: 4, paddingVertical: 8, alignItems: 'center' },
  // Eleven bands will not sit in one row on an SE, so the filter popups wrap. Chips size to
  // their own text rather than sharing the width equally — '160m' and 'ALL' in equal columns
  // wastes the room the SE has least of.
  popWrapRow: { flexDirection: 'row', flexWrap: 'wrap', gap: 6 },
  popChip:    { borderWidth: 1, borderRadius: 4, paddingVertical: 7, paddingHorizontal: 12, alignItems: 'center' },
  dabSpeedBtnTxt: { fontSize: 11, fontWeight: '600' },
  output: {
    fontSize: 12, letterSpacing: 0.8, lineHeight: 20,
    color: T.value, fontFamily: FONT,
    textShadowColor: T.outputGlow,
    textShadowOffset: { width: 0, height: 0 },
    textShadowRadius: 4,
  },
});
