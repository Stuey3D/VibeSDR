/**
 * ★★★ THE SATELLITE MODE's box on a VibeServer (V12, docs/v12/SATELLITE-MODE.md, Stuart 2026-11-20) — ONE whole-radio mode
 * for every satellite; it replaced the SAT picker and the METEOR LRPT decoder key. The decoder boxes' place and look
 * (DecoderShell — every faceplate, transparency on/off), the words and the half-globe's geometry of src/services/satellites.ts
 * (the web's satellites.ts says and draws the same).
 *
 *   header:  ● SATELLITE · AUTO DOPPLER (default ON) · EXIT
 *   the pick (once the server says "picked"):
 *     voice / packet → COMPACT: name · downlink, the corrected frequency, "In range for 9:55" (or the next pass), the
 *                      decoder's line, the APRS lines (packet), and a half-globe (react-native-svg — no tiles, no data):
 *                      the bird above its sub-point, its beam cone to the ground, the receiver on the dome
 *     image          → BIG: the picture as it arrives (DecoderImageCanvas on the screen's decoderImageRef — the decoder box
 *                      stays shut while SATELLITE owns the decoder), the finished picture at pass end
 *     ISS 145.800 (sstv) opens compact and grows when an SSTV picture starts (the canvas is mounted, hidden, from the pick)
 *   "Orbits synced 16:08 · 2 h ago" / "Fetching orbit data…" / "No orbit data — why"
 *   the timetable: name · frequency · "UP · 6:12 left" / "AOS 16:42 (in 0:38)" — up now SHORTEST remaining first, then the
 *     next passes; what this radio cannot hear GREYED with why (the list is a timetable, so greying is the rule here)
 *
 * ★★ THE SERVER DOES THE WORK (THE CONTRACT): a row asks {"op":"pick"}; the server tunes, locks and attaches the decoder.
 *    ★ No flashing: COAST dims the readout.
 */
import React, { useEffect, useMemo, useRef, useState } from 'react';
import { Pressable, ScrollView, StyleSheet, Text, View, useWindowDimensions } from 'react-native';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import Svg, { Circle, Path, Text as SvgText } from 'react-native-svg';
import { DecoderShell, DecoderHeader, DecoderTitle, DecoderKey, DecoderBody, DECODER_FONT, decoderBodyInset,
         useDecoderTokens, useDecoderStyles, engraveStyle, type DecoderTokens } from './DecoderShell';
import DecoderImageCanvas, { type DecoderImageHandle } from './DecoderImageCanvas';
import TlmCharts from './TlmCharts';
import { tlmLivePass, tlmLoadPasses, type TlmPass } from '../utils/tlmCharts';
import { scrollLane } from '../constants/popupTokens';
import { GLOBE_ARROW_PX, GLOBE_KEY_TEXT, GLOBE_RX_DOT, globeLandInk, globeArrowPath, globeLabels, satRemindEnds, satRemindKeyText, satRemindOffered, satRemindText, satTelemetryHead,
         satTelemetryLine, satTelemetryHex, satTxDetail, satTxModeLabel, SAT_REMIND_LEADS, type GeoPt, type SatReminder, type SatTelemetry,
         globeScene, satAltKm, satBoxSize, satPassQuality, satPassText, satCorrectedHz, satCountdownText, satCreditText, satUplinkText, satHzText, satModeRows, satStateWord,
         satSyncedText, satTxCreditText, satWhenText, txText, txUse,
         satCwTail, satDecView, satDecWord, satViewShown, satViewSwitch, satVfoBLine, satVfoBOffers, SAT_VFOB_KEY,
         satBeaconText, SAT_BEACON_EMPTY, type SatBeaconList,
         type SatInfo, type SatList, type SatPicked, type SatReadout, type SatVfoB } from '../services/satellites';

const SAT_MAX_W = 760;
/** ★ The half-globe's drawing size (viewBox); on screen it is the box's width, capped at GLOBE_MAX_W (Stuart 11-20). */
const GLOBE_W = 640, GLOBE_H = 300, GLOBE_MAX_W = 420;
/** ★ SMALL: the globe beside the words (Stuart 11-21: the box took too much room) */
const GLOBE_SMALL_W = 120;

export interface SatellitePanelProps {
  list: SatList | null;
  readout: SatReadout | null;
  /** hwinfo's doppler / dopplerWhy: ok false = greyed, `why` shown. */
  cap: { ok: boolean; why: string };
  autoDoppler: boolean;
  picked: SatPicked | null;
  pending: { norad: number; tx: number } | null;
  /** a refused pick's reason (the server's OFF + why), or a greyed row's why after a press */
  why: string;
  /** a picture has started on the pick's image decoder (the ISS SSTV box grows) / finished (pass end) */
  picture: boolean;
  pictureDone: boolean;
  /** the decoder's own line (LRPT's carrier / lines, SSTV's mode, APRS listening) */
  decLine: string;
  /** APRS lines heard on a packet downlink, newest first */
  packets: string[];
  /** the receiver (server) position — null = no globe */
  rxPos: { lat: number; lon: number } | null;
  imageRef: React.MutableRefObject<DecoderImageHandle | null>;
  onPick: (norad: number, tx: number) => void;
  onGreyed: (why: string) => void;
  onDoppler: () => void;
  onExit: () => void;
  bottomOffset: number;
  topLimit?: number;
  /** BIG's ceiling: the status row (DecoderPanel's topSafe) */
  topSafe?: number;
  /** ★ 1.7 the pick's pass path (the track reply's geo) — the dotted orbit; null = none (an older server) */
  orbit?: GeoPt[] | null;
  /** the orbit's highest point (the server's tca) as a geo index */
  tcaIdx?: number;
  /** ★ 1.8 the receiver dot's label: the server's name ('' = RECEIVER) */
  rxName?: string;
  /** ★ 1.3 the pick's telemetry (fctlm) */
  telemetry?: SatTelemetry | null;
  /** ★★ beta91 TELEMETRY CHARTS: the server's gallery, read (a path → its text, null = not there) — the LAST PASSES
   *  charts; absent = no gallery to read (the CHARTS key still draws this pass) */
  galleryGet?: ((path: string) => Promise<string | null>) | null;
  /** ★★ 1.6 REMIND — held in this app session only; `held` = the reminder on that downlink, if any */
  remindHeld?: (norad: number, tx: number) => boolean;
  remindLead?: number;
  onRemind?: (norad: number, tx: number) => void;
  onRemindLead?: (min: number) => void;
  /** ★ 1.9 the MAP key (a packet downlink, once a station with a position is heard) → the DIGI MODES map's APRS layer */
  mapOffered?: boolean;
  onMap?: () => void;
  /** ★ beta92 a CW beacon picked on A: its decoded characters (the pick's own CW decoder) */
  cwText?: string;
  /** ★★ beta92 a SKIMMED CW beacon ('cwskim'): the skimmer's list on A and on B — shown nearest the prediction first, each
   *  with its offset from it (satBeaconText) */
  beacon?: SatBeaconList | null;
  bBeacon?: SatBeaconList | null;
  /** ★★★ beta92 OPEN IN VFO B (docs/v12/SAT-VFOB.md): the server's word on B (on), what B's decoder has said (its text,
   *  packet lines, own status, a picture started), the canvas B's picture draws on, and the key's press. */
  vfob?: SatVfoB | null;
  bText?: string;
  bPackets?: string[];
  bStatus?: string;
  bPicture?: boolean;
  bImageRef?: React.MutableRefObject<DecoderImageHandle | null>;
  onVfoB?: (norad: number, tx: number) => void;
}

/** ★ The half-globe (spec item 4): the scene's paths in react-native-svg — the dotted orbit (1.7), the drawn bird, the green
 *  receiver dot and the labels (1.8); sizes on SCREEN (scene units per point from the laid-out width), so they read at
 *  SMALL and BIG alike. */
function HalfGlobeView({ rx, sat, ink, beam, ground, small = false, orbit, tcaIdx, rxName }: {
  rx: { lat: number; lon: number }; sat: SatInfo | undefined; ink: string; beam: string; ground: string; small?: boolean;
  orbit?: GeoPt[] | null; tcaIdx?: number; rxName?: string;
}) {
  const g = useMemo(() => globeScene(GLOBE_W, GLOBE_H, rx,
    sat && sat.lat != null && sat.lon != null ? { lat: sat.lat, lon: sat.lon, altKm: satAltKm(sat.rangeKm, sat.el) } : null, orbit ?? null, tcaIdx),
  [rx.lat, rx.lon, sat?.lat, sat?.lon, sat?.rangeKm, sat?.el, orbit, tcaIdx]);   // eslint-disable-line react-hooks/exhaustive-deps
  const [wPx, setWPx] = useState(small ? GLOBE_SMALL_W : GLOBE_MAX_W);
  const sc = GLOBE_W / Math.max(60, wPx);
  const font = 10 * sc;
  const L = globeLabels(g, rxName, sat?.name, font);
  const arrowD = g.arrows.length ? globeArrowPath(g.arrows, GLOBE_ARROW_PX * sc) : '';
  const birdInk = globeLandInk(ground);
  const dome = `M${(g.cx - g.r).toFixed(1)} ${g.cy.toFixed(1)}A${g.r.toFixed(1)} ${g.r.toFixed(1)} 0 0 1 ${(g.cx + g.r).toFixed(1)} ${g.cy.toFixed(1)}Z`;
  return (
    <View style={small ? { width: GLOBE_SMALL_W, aspectRatio: GLOBE_W / GLOBE_H } : { width: '100%', maxWidth: GLOBE_MAX_W, aspectRatio: GLOBE_W / GLOBE_H, alignSelf: 'center' }}
          onLayout={(e) => { const w = e.nativeEvent.layout.width; if (w > 0 && Math.abs(w - wPx) > 1) setWPx(w); }}>
    <Svg width="100%" height="100%" viewBox={`0 0 ${GLOBE_W} ${GLOBE_H}`}
         accessibilityLabel="Where the satellite is: above the globe, its beam to the ground, the pass's orbit dotted with arrows for its direction">
      <Path d={dome} fill={ink} fillOpacity={0.12} stroke={ink} strokeOpacity={0.55} strokeWidth={2} />
      {!!g.grid && <Path d={g.grid} fill="none" stroke={ink} strokeOpacity={0.12} strokeWidth={1} />}
      {!!g.coast && <Path d={g.coast} fill="none" stroke={globeLandInk(ground)} strokeOpacity={0.7} strokeWidth={1} />}
      {!!g.foot && <Path d={g.foot} fill={beam} fillOpacity={0.28} />}
      {!!g.beam && <Path d={g.beam} fill={beam} fillOpacity={0.35} />}
      {!!g.orbit && <Path d={g.orbit} fill="none" stroke={beam} strokeOpacity={0.85} strokeWidth={1.5 * sc} strokeLinecap="round"
                          strokeDasharray={`${(1.5 * sc).toFixed(1)} ${(4 * sc).toFixed(1)}`} />}
      {!!g.tcaTick && <Path d={g.tcaTick} fill="none" stroke={beam} strokeOpacity={0.85} strokeWidth={1.5 * sc} strokeLinecap="round" />}
      {/* ★ beta91 which way it travels: open chevrons along increasing time, haloed in the ground so they read over the land */}
      {g.arrows.length > 0 && <>
        <Path d={arrowD} fill="none" stroke={ground} strokeOpacity={0.8} strokeWidth={3.6 * sc} strokeLinecap="round" strokeLinejoin="round" />
        <Path d={arrowD} fill="none" stroke={beam} strokeWidth={1.6 * sc} strokeLinecap="round" strokeLinejoin="round" />
      </>}
      {!!g.rx && <Circle cx={g.rx.x} cy={g.rx.y} r={GLOBE_RX_DOT.r * sc} fill={GLOBE_RX_DOT.fill} stroke={GLOBE_RX_DOT.ring} strokeWidth={GLOBE_RX_DOT.ringW * sc} />}
      {/* ★ beta91 the bird is neutral like the land, so it stands off the beam (Stuart 11-22) */}
      {!!g.bird && <>
        <Path d={g.bird.panels} fill={birdInk} fillOpacity={0.45} stroke={birdInk} strokeWidth={1.2 * sc} strokeLinejoin="round" />
        <Path d={g.bird.cells} fill="none" stroke={birdInk} strokeOpacity={0.9} strokeWidth={0.8 * sc} />
        <Path d={g.bird.boom} fill="none" stroke={birdInk} strokeWidth={1.6 * sc} strokeLinecap="round" />
        <Path d={g.bird.body} fill={birdInk} stroke={ground} strokeWidth={1 * sc} strokeLinejoin="round" />
        <Path d={g.bird.dish} fill={birdInk} />
      </>}
      {[L.rx, L.sat].map((lb, i) => lb && (
        <React.Fragment key={i}>
          {/* a halo in the window's ground, then the word — legible over the coastlines and the beam */}
          <SvgText x={lb.x} y={lb.y} fontSize={font} fill={ground} stroke={ground} strokeWidth={3 * sc} strokeOpacity={0.9}
                   textAnchor={lb.anchor} fontFamily={DECODER_FONT}>{lb.text}</SvgText>
          <SvgText x={lb.x} y={lb.y} fontSize={font} fill={ink} textAnchor={lb.anchor} fontFamily={DECODER_FONT}>{lb.text}</SvgText>
        </React.Fragment>
      ))}
    </Svg>
    </View>
  );
}
/** ★★ beta91 (Stuart 11-22: stutters with the satellite box open) the panel re-renders every second for its countdowns;
 *  the half-globe — a long coastline path handed to the native SVG each time — is redrawn only when it would look
 *  different: the bird moved ~0.1° (every second or two in a pass), a new orbit, a new size or colour. */
const r1 = (v: number | undefined) => (v == null ? '' : v.toFixed(1));
const HalfGlobe = React.memo(HalfGlobeView, (a, b) =>
  a.rx.lat === b.rx.lat && a.rx.lon === b.rx.lon && a.ink === b.ink && a.beam === b.beam && a.ground === b.ground
  && a.small === b.small && a.orbit === b.orbit && a.tcaIdx === b.tcaIdx && a.rxName === b.rxName
  && a.sat?.norad === b.sat?.norad && a.sat?.name === b.sat?.name && r1(a.sat?.lat) === r1(b.sat?.lat) && r1(a.sat?.lon) === r1(b.sat?.lon)
  && Math.round((a.sat?.rangeKm ?? 0) / 20) === Math.round((b.sat?.rangeKm ?? 0) / 20) && r1(a.sat?.el) === r1(b.sat?.el));


/* ★★★ SATELLITE-BEST 1.6 — THE REMIND BANNER (Stuart 11-21: in-app banner only, no OS notification module). Each fired
 *  reminder its own banner, newest on top, three at most ("+2 more"); LISTEN enters SATELLITE and picks through the
 *  ordinary pick (ask-before-tuning); a pass that can no longer be picked says why and offers no LISTEN. */
export interface SatRemindToast { r: SatReminder; why: string }
export function SatRemindBanner({ toasts, nowSec, picked, pickedName, top, onListen, onDismiss }: {
  toasts: SatRemindToast[]; nowSec: number; picked: { norad: number; tx: number } | null; pickedName?: string; top: number;
  onListen: (key: string) => void; onDismiss: (key: string) => void;
}) {
  const s = useDecoderStyles(makeStyles);
  if (!toasts.length) return null;
  return (
    <View pointerEvents="box-none" style={[s.remWrap, { top }]}>
      {toasts.slice(0, 3).map((t) => {
        const ends = t.why ? '' : satRemindEnds(picked, pickedName, t.r);
        const line = t.why ? satRemindText(t.r, nowSec).replace(/ — LISTEN\?$/, '') : satRemindText(t.r, nowSec);
        return (
          <View key={t.r.key} style={s.remToast} accessibilityRole="alert" accessibilityLiveRegion="polite">
            <Text style={s.txt}>{line}</Text>
            {!!(t.why || ends) && <Text style={s.remWhy}>{t.why || ends}</Text>}
            <View style={s.remKeys}>
              {!t.why && <DecoderKey label="LISTEN" active onPress={() => onListen(t.r.key)} accessibilityLabel={`Listen: ${line}`} />}
              <DecoderKey label="DISMISS" onPress={() => onDismiss(t.r.key)} accessibilityLabel="Dismiss this reminder" />
            </View>
          </View>
        );
      })}
      {toasts.length > 3 && <Text style={[s.remWhy, s.remMore]}>{`+${toasts.length - 3} more`}</Text>}
    </View>
  );
}

export default function SatellitePanel(p: SatellitePanelProps) {
  const s = useDecoderStyles(makeStyles);
  const tk = useDecoderTokens();
  const { height: winH } = useWindowDimensions();
  const insets = useSafeAreaInsets();
  /* ★★★ THE DECODER BOX's SIZES (Stuart 2026-11-21: "the satellite decoder box needs to be the same as a standard decoder
   *  box with the minimise and small/big buttons as right now on the phone you cant see the waterfall behind") —
   *  DecoderPanel's arithmetic: SMALL up to the server-name row (topLimit), BIG up to the status row; BIG offered only where
   *  it is bigger; − folds it to the header. A picture grows it once; a size chosen by hand carries over to the next pick. */
  const HEADER_H = 46;
  const availH = Math.max(160, winH - p.bottomOffset - (p.topSafe ?? insets.top + 16) - HEADER_H - decoderBodyInset(tk));
  const smallFit = p.topLimit != null ? Math.max(100, winH - p.bottomOffset - p.topLimit - HEADER_H - decoderBodyInset(tk)) : 200;
  const bigH = winH > 950 ? Math.min(availH, winH * 0.62) : availH;
  const bigUseful = bigH >= smallFit + 40;
  const [bigPick, setBigPick] = useState(false);
  const [minimised, setMinimised] = useState(false);
  const handBig = useRef(false);
  const sized = useRef(false);
  const big = bigPick && bigUseful;
  const bodyH = Math.round(big ? bigH : Math.min(availH, smallFit));
  const setBigByHand = (on: boolean) => { handBig.current = on; sized.current = true; setBigPick(on); if (on) setMinimised(false); };
  // a new pick opens at the size last chosen by hand
  useEffect(() => { sized.current = false; setBigPick(handBig.current); }, [p.picked?.norad, p.picked?.tx]);   // eslint-disable-line react-hooks/exhaustive-deps
  // a picture starts: grow once, unless SMALL was chosen for this pick
  useEffect(() => { if (p.picture && !sized.current) { setBigPick(true); setMinimised(false); } }, [p.picture]);
  // ★ the countdowns tick on between lists
  const [, setTick] = useState(0);
  useEffect(() => { const t = setInterval(() => setTick((n) => n + 1), 1000); return () => clearInterval(t); }, []);
  const [listOpen, setListOpen] = useState(true);
  const [tlmRaw, setTlmRaw] = useState(false);
  // ★★ beta91 TELEMETRY CHARTS — this pass from the live frames, the last passes from the gallery (read when CHARTS opens)
  const [tlmCharts, setTlmCharts] = useState(false);
  const [tlmAcross, setTlmAcross] = useState<TlmPass[] | null>(null);
  const [tlmAcrossNote, setTlmAcrossNote] = useState('');
  useEffect(() => { if (p.picked) setListOpen(false); }, [p.picked?.norad, p.picked?.tx]);   // eslint-disable-line react-hooks/exhaustive-deps
  const now = Date.now() / 1000;
  const greyed = !p.cap.ok;
  const rows = satModeRows(p.list, now);
  const sync = satSyncedText(p.list, now);
  const credit = satCreditText(p.list, now), txc = satTxCreditText(p.list);

  const sel = p.picked ?? p.pending;
  const sat = sel ? p.list?.sats.find((x) => x.norad === sel.norad) : undefined;
  const tx = sel ? sat?.tx[sel.tx] : undefined;
  const confirmed = !!p.picked && !!tx;
  const mine = p.readout && p.readout.state !== 'OFF' && p.readout.norad === sat?.norad ? p.readout : null;
  // ★ the picture area only once a picture has started (or finished), and only BIG — never an empty black frame
  const showPic = big && confirmed && satBoxSize(tx, true) === 'big' && (p.picture || p.pictureDone);
  const use = tx ? txUse(tx) : 'voice';
  const imageDec = p.picked?.decoder === 'lrpt' || p.picked?.decoder === 'sstv';
  const lockWord = !p.autoDoppler ? 'DOPPLER OFF' : mine ? satStateWord(mine) : '';
  const tlmDec = p.picked?.decoder === 'fctlm' || !!p.telemetry?.values.length;
  const tlmHex = satTelemetryHex(p.telemetry ?? null);
  const tlmSat = p.telemetry?.sat || '';
  const tlmLive = useMemo(() => tlmLivePass(p.telemetry?.hist, tlmSat), [p.telemetry?.hist, tlmSat]);
  const galleryGet = p.galleryGet;
  useEffect(() => {
    if (!tlmCharts) return;
    if (!galleryGet) { setTlmAcross(null); setTlmAcrossNote('LAST PASSES · this server has no gallery to read'); return; }
    let live = true;
    setTlmAcross(null); setTlmAcrossNote('');
    tlmLoadPasses(galleryGet, tlmSat).then((x) => { if (live) setTlmAcross(x); })
      .catch(() => { if (live) setTlmAcrossNote('LAST PASSES · the gallery could not be read'); });
    return () => { live = false; };
  }, [tlmCharts, galleryGet, tlmSat, p.picked?.norad]);   // eslint-disable-line react-hooks/exhaustive-deps
  const ground = tk.window?.bg ?? tk.solidBg ?? '#070605';
  // ★★★ beta92 OPEN IN VFO B — B's line and the line switch; with no B this is exactly the one-downlink box
  const vb = p.vfob && p.vfob.on && confirmed && p.vfob.norad === p.picked?.norad ? p.vfob : null;
  const [view, setView] = useState<'A' | 'B'>('A');
  // ★ the line just opened is the one shown (a tap on A's line goes back); B gone = A
  useEffect(() => { setView(vb && satDecView(vb.decoder) ? 'B' : 'A'); }, [vb?.norad, vb?.tx]);   // eslint-disable-line react-hooks/exhaustive-deps
  const sw = satViewSwitch(p.picked?.decoder, vb);
  const shown = satViewShown(p.picked?.decoder, vb, view);
  const showA = shown === 'A';
  const bView = vb && !showA ? satDecView(vb.decoder) : '';
  const offers = p.onVfoB && confirmed ? satVfoBOffers(p.picked, sat, vb) : [];
  const txB = vb ? sat?.tx[vb.tx] : undefined;
  const bLine = vb ? [satVfoBLine(vb, txB?.label, mine), p.bStatus || (vb.decoderWhy ? `decoder: ${vb.decoderWhy}` : satDecWord(vb.decoder, vb.use ?? (txB ? txUse(txB) : 'voice'), txB?.sstv, big))].filter(Boolean).join(' · ') : '';
  const showBPic = bView === 'image' && big && !!p.bPicture;
  // B's picture grows the box once, as A's does (unless SMALL was chosen for this pick)
  useEffect(() => { if (p.bPicture && !showA && !sized.current) { setBigPick(true); setMinimised(false); } }, [p.bPicture]);   // eslint-disable-line react-hooks/exhaustive-deps
  const decWord = !confirmed ? 'Tuning…'
    : tlmDec && !p.decLine ? 'listening for telemetry'
    // ★★ beta91: LRPT's own line is the live state (it says a filed pass itself) — never a stale "picture complete"
    : p.pictureDone && imageDec && !(p.picked?.decoder === 'lrpt' && p.decLine) ? 'picture complete — saved to the gallery'
    // ★ beta92: a CW beacon says its CW decoder is listening for the beacon (it said "listening for packets") — satDecWord
    : p.decLine || satDecWord(p.picked?.decoder, use, tx?.sstv, big);
  // the folded header still says the pick: "ISS · UP · 6:12 left"
  const selRow = sat ? rows.find((r) => r.sat.norad === sat.norad) : undefined;
  const headLine = minimised && sat ? (selRow ? `${sat.name} · ${satWhenText(selRow, now)}` : sat.name) : '';
  const stateLine0 = [confirmed ? lockWord : '', decWord].filter(Boolean).join(' · ');
  const stateLine = sw ? `VFO A · ${stateLine0 || tx?.label || ''}` : stateLine0;
  // ★ beta91 "In range for 4:12 · heading south" — from the pick's track (time order), only where certain
  const countdown = satCountdownText(sat, now, confirmed ? p.orbit : null);

  return (
    <DecoderShell bottom={p.bottomOffset} maxWidth={SAT_MAX_W}>
      <DecoderHeader onPress={() => setMinimised((v) => !v)}>
        <View style={[s.dot, !!mine && mine.state === 'TRACK' && s.dotOn]} />
        <DecoderTitle minimised={minimised}>SATELLITE</DecoderTitle>
        {minimised ? <Text style={s.headLine} numberOfLines={1}>{headLine}</Text> : <View style={{ flex: 1 }} />}
        {/* ★★ 1.9 MAP — a packet downlink's stations on the DIGI MODES map (APRS layer), once one with a position is heard */}
        {!minimised && !!p.mapOffered && !!p.onMap && (
          <DecoderKey label="MAP" onPress={p.onMap} accessibilityLabel="The stations heard through this satellite, on the DIGI MODES map" />
        )}
        <DecoderKey label="AUTO DOPPLER" active={p.autoDoppler} disabled={greyed} onPress={p.onDoppler}
                    accessibilityLabel={greyed ? `Auto Doppler unavailable: ${p.cap.why}` : `Auto Doppler ${p.autoDoppler ? 'on' : 'off'}: keep the satellite centred for the whole pass`} />
        {bigUseful && !minimised && (
          <DecoderKey label={big ? 'SMALL' : 'BIG'} active={big} onPress={() => setBigByHand(!big)}
                      accessibilityLabel={big ? 'Smaller box' : 'Bigger box'} />
        )}
        <DecoderKey label="EXIT" onPress={p.onExit} accessibilityLabel="Leave SATELLITE (the Doppler lock and the decoder stop)" />
        <DecoderKey label={minimised ? '□' : '−'} onPress={() => setMinimised((v) => !v)}
                    accessibilityLabel={minimised ? 'Restore' : 'Minimise'} />
      </DecoderHeader>
      {!minimised && (greyed || !!p.why) && <Text style={s.note}>{greyed ? p.cap.why : p.why}</Text>}
      {!minimised && <DecoderBody>
        {/* ★ beta91 (Stuart 11-22, a screenshot: BIG with the list folded and nothing decoded yet, half the box empty) the box
             holds its full BIG height only while a picture is on show; otherwise it fits what is in it, up to that height */}
        <ScrollView style={big && showPic ? { height: bodyH } : { maxHeight: bodyH }} contentContainerStyle={scrollLane} showsVerticalScrollIndicator>
          {!!sel && !!sat && !!tx && (
            <View style={s.box}>
              <View style={[big ? s.ro : s.roSmall, mine?.state === 'COAST' && s.coast]}
                    accessibilityLabel={`${sat.name} ${tx.label}, ${satHzText(satCorrectedHz(tx, mine))}, ${countdown}`}>
                <View style={s.txtCol}>
                  <Text style={s.txt} numberOfLines={1}>{`${sat.name} · ${tx.label}${satTxModeLabel(tx) ? ' · ' + satTxModeLabel(tx) : ''}`}</Text>
                  <Text style={s.big}>{satHzText(satCorrectedHz(tx, mine))}</Text>
                  {/* ★ beta91 which frequency is which: the big one is the downlink; the uplink (where to call) under it */}
                  {!!satUplinkText(tx) && <Text style={s.detail}>{`${satUplinkText(tx)} · the frequency above is the downlink`}</Text>}
                  <Text style={s.txt}>{countdown}</Text>
                  {!!stateLine && (sw
                    ? <Pressable onPress={() => setView('A')} accessibilityRole="button" accessibilityState={{ selected: shown === 'A' }}
                                 accessibilityLabel={`${stateLine}. Show VFO A's decoder in the box`} style={[s.ln, shown === 'A' && s.lnOn]}>
                        <Text style={[s.detail, s.lnTxt, shown === 'A' && s.lnTxtOn]}>{stateLine}</Text></Pressable>
                    : <Text style={s.detail}>{stateLine}</Text>)}
                  {/* ★★★ beta92 B's line (tappable only while the switch exists) and the OPEN IN VFO B key(s) */}
                  {!!vb && (sw
                    ? <Pressable onPress={() => setView('B')} accessibilityRole="button" accessibilityState={{ selected: shown === 'B' }}
                                 accessibilityLabel={`${bLine}. Show VFO B's decoder in the box`} style={[s.ln, shown === 'B' && s.lnOn]}>
                        <Text style={[s.detail, s.lnTxt, shown === 'B' && s.lnTxtOn]}>{bLine}</Text></Pressable>
                    : <Text style={s.detail}>{bLine}</Text>)}
                  {offers.map((o) => (
                    <View key={o.i} style={s.vbKey}>
                      <DecoderKey label={`${SAT_VFOB_KEY} · ${o.tx.label}`} onPress={() => p.onVfoB!(sat!.norad, o.i)}
                                  accessibilityLabel={`Open ${o.tx.label} in VFO B, with its decoder and the same Doppler correction — close it with VFO A/B on the frequency card`} />
                    </View>
                  ))}
                </View>
                {!!p.rxPos && (
                  <View style={!big ? undefined : { alignSelf: 'stretch' }}>
                    <HalfGlobe rx={p.rxPos} sat={sat} ink={tk.value} beam={tk.accent} ground={ground} small={!big}
                               orbit={confirmed ? p.orbit : null} tcaIdx={p.tcaIdx} rxName={p.rxName} />
                    {/* ★ BIG: the one-line key (the dotted half only when there is an orbit to see) */}
                    {big && <Text style={[s.detail, s.key]}>{confirmed && p.orbit ? GLOBE_KEY_TEXT : GLOBE_KEY_TEXT.replace(/^.*· /, '')}</Text>}
                  </View>
                )}
              </View>
              {/* ★ the picture: mounted from the pick while the decoder draws one (so an ISS SSTV frame's first lines are
                   not lost before the box grows), shown only BIG */}
              {confirmed && imageDec && (
                <View style={showPic && showA ? s.img : s.hidden}>
                  <DecoderImageCanvas ref={p.imageRef} maxHeight={Math.max(160, Math.min(440, bodyH - 150))} fullWidth
                    decoderName={p.picked!.decoder === 'lrpt' ? 'meteor' : 'sstv'}
                    onInfo={() => {}} onStatus={() => {}} onPrevState={() => {}} />
                </View>
              )}
              {/* ★★ 1.3 the telemetry, as values with units (a channel with no published calibration says "raw") */}
              {/* ★ beta92 a CW beacon on A: its decoded text, as CW shows it elsewhere */}
              {confirmed && showA && p.picked?.decoder === 'cw' && (
                <View style={s.pk}><Text style={s.mono} selectable>{satCwTail(p.cwText ?? '') || 'no characters yet'}</Text></View>
              )}
              {/* ★★ beta92 a skimmed beacon: the signals in its 24 kHz, nearest the prediction first, each with its offset */}
              {confirmed && showA && p.picked?.decoder === 'cwskim' && (
                <View style={s.pk}><Text style={s.mono} selectable>{satBeaconText(p.beacon) || SAT_BEACON_EMPTY}</Text></View>
              )}
              {confirmed && showA && tlmDec && (
                <View style={s.pk}>
                  <View style={s.nameRow}>
                    <Text style={[s.mono, { flexShrink: 1 }]}>{satTelemetryHead(p.telemetry ?? null)}</Text>
                    <View style={s.keys}>
                      {/* ★★ beta91 CHARTS: this pass and the last passes, a small chart a channel (TlmCharts) */}
                      <DecoderKey label="CHARTS" active={tlmCharts} onPress={() => { setTlmCharts((v) => !v); setTlmRaw(false); }}
                                  accessibilityLabel="Telemetry charts: this pass, and the last passes saved in the gallery" />
                      {!!tlmHex && <DecoderKey label="RAW" active={tlmRaw} onPress={() => { setTlmRaw((v) => !v); setTlmCharts(false); }} accessibilityLabel="The frame's bytes as received" />}
                    </View>
                  </View>
                  {tlmCharts ? <TlmCharts pass={tlmLive} live across={tlmAcross} acrossNote={tlmAcrossNote}
                                          ink={{ text: tk.value, muted: tk.muted, line: tk.accent, grid: tk.border, font: F }} />
                  : tlmRaw && !!tlmHex ? <Text style={s.mono} selectable>{tlmHex}</Text>
                  : !!p.telemetry?.values.length && <View style={s.tlmGrid}>
                    {p.telemetry.values.map((v, i) => <Text key={i} style={[s.mono, s.tlmCell]} numberOfLines={1}>{satTelemetryLine(v)}</Text>)}
                  </View>}
                </View>
              )}
              {confirmed && showA && use === 'packet' && !tlmDec && satDecView(p.picked?.decoder) !== 'text' && (
                <View style={s.pk}>
                  {p.packets.length ? p.packets.slice(0, 12).map((l, i) => <Text key={i} style={s.mono} numberOfLines={2}>{l}</Text>)
                    : <Text style={s.detail}>no packets yet</Text>}
                </View>
              )}
              {/* ★★★ beta92 B's decoder area — only while B's line is the one shown. B's picture canvas is mounted from the
                   open (so a picture's first lines are not lost), shown only BIG. */}
              {!!vb && satDecView(vb.decoder) === 'image' && !!p.bImageRef && (
                <View style={showBPic ? s.img : s.hidden}>
                  <DecoderImageCanvas ref={p.bImageRef} maxHeight={Math.max(160, Math.min(440, bodyH - 150))} fullWidth
                    decoderName={vb.decoder === 'lrpt' ? 'meteor' : 'sstv'}
                    onInfo={() => {}} onStatus={() => {}} onPrevState={() => {}} />
                </View>
              )}
              {(bView === 'text' || bView === 'telemetry') && (
                <View style={s.pk}><Text style={s.mono} selectable>{vb?.decoder === 'cwskim' ? satBeaconText(p.bBeacon) || SAT_BEACON_EMPTY
                  : satCwTail(p.bText ?? '', bView === 'telemetry' ? 6 : 4) || (bView === 'text' ? 'no characters yet' : 'no frames yet')}</Text></View>
              )}
              {bView === 'packet' && (
                <View style={s.pk}>
                  {p.bPackets?.length ? p.bPackets.slice(0, 12).map((l, i) => <Text key={i} style={s.mono} numberOfLines={2}>{l}</Text>)
                    : <Text style={s.detail}>no packets yet</Text>}
                </View>
              )}
            </View>
          )}
          {!!sync && <Text style={s.detail}>{sync}</Text>}
          <Pressable onPress={() => setListOpen((v) => !v)} accessibilityRole="button" accessibilityState={{ expanded: listOpen }}
                     style={s.sumRow}>
            <Text style={s.sum}>{`${listOpen ? '▾' : '▸'} ${p.picked ? 'ALL SATELLITES' : 'SATELLITES'}`}</Text>
          </Pressable>
          {listOpen && (!p.list ? <Text style={s.note}>Asking the server…</Text>
            : !rows.length ? <Text style={s.note}>{p.list.why || (p.list.source?.fetching ? 'The satellites appear once the orbit data has arrived' : 'No satellites are listed on this server')}</Text>
            : rows.slice(0, 40).map((r) => {
                const on = !!sel && sel.norad === r.sat.norad && sel.tx === r.i;
                const name = r.sat.tx.length > 1 ? `${r.sat.name} · ${r.tx.label}` : r.sat.name;
                // ★★ 1.6 REMIND — only where a pick disturbs nobody (pickPrivate), hearable, a pass to come; hidden otherwise
                const offer = !!p.onRemind && !greyed && satRemindOffered(p.list, r);
                const held = offer && !!p.remindHeld?.(r.sat.norad, r.i);
                return (
                  <View key={`${r.sat.norad}.${r.i}`} style={s.rw}>
                  <Pressable
                    onPress={() => (r.grey || greyed ? p.onGreyed(r.grey ? r.why : p.cap.why) : p.onPick(r.sat.norad, r.i))}
                    accessibilityRole="button" accessibilityState={{ disabled: r.grey || greyed, selected: on }}
                    accessibilityLabel={`${name}, ${txText(r.tx)}, ${satWhenText(r, now)}${r.grey ? `, unavailable: ${r.why}` : r.pass ? `, pass ${satPassText(r.pass)}` : ''}`}
                    style={[s.row, on && s.rowOn, (r.grey || greyed) && s.grey]}>
                    <View style={s.nameRow}>
                      <Text style={s.txt} numberOfLines={1}>{name}</Text>
                      <Text style={[s.txt, r.up && s.up]} numberOfLines={1}>{satWhenText(r, now)}</Text>
                    </View>
                    <Text style={s.detail}>{`${satTxDetail(r.tx)}${r.tx.status === 'inactive' ? ' · INACTIVE' : ''}${r.grey ? ` — ${r.why}` : ''}`}
                      {/* ★★ the pass's rating (satPassQuality — its highest elevation), shaded like n2yo's legend (Stuart 11-21) */}
                      {!r.grey && !!r.pass && <Text>{' · '}<Text style={[s.q, s[`q_${satPassQuality(r.pass.maxEl)}`]]}>{` ${satPassText(r.pass)} `}</Text></Text>}
                    </Text>
                  </Pressable>
                  {offer && (
                    <View style={s.remKey}>
                      <DecoderKey label={satRemindKeyText(held, p.remindLead ?? 2)} active={held} onPress={() => p.onRemind!(r.sat.norad, r.i)}
                                  accessibilityLabel={held ? `Cancel the reminder for ${name}` : `Remind me shortly before ${name} rises — while still connected to this server`} />
                    </View>
                  )}
                  </View>
                );
              }))}
          {listOpen && !!p.onRemind && !greyed && rows.some((r) => satRemindOffered(p.list, r)) && (
            <View style={s.leadRow}>
              <Text style={s.detailNp}>REMIND</Text>
              {SAT_REMIND_LEADS.map((v) => (
                <DecoderKey key={v} label={String(v)} active={(p.remindLead ?? 2) === v} onPress={() => p.onRemindLead?.(v)}
                            accessibilityLabel={`Remind ${v} minute${v === 1 ? '' : 's'} before a pass rises`} />
              ))}
              <Text style={s.detailNp}>min before it rises · this app session only</Text>
            </View>
          )}
          {p.list?.time === 'system' && <Text style={s.detail}>TIME: SYSTEM — this server's clock has not been checked against a time server</Text>}
          {!!credit && <Text style={s.detail}>{credit}{txc ? ` · ${txc}` : ''}</Text>}
        </ScrollView>
      </DecoderBody>}
    </DecoderShell>
  );
}

const F = DECODER_FONT;
const makeStyles = (T: DecoderTokens) => StyleSheet.create({
  dot:     { width: 8, height: 8, borderRadius: 4, backgroundColor: T.muted, opacity: 0.6 },
  dotOn:   { backgroundColor: T.good, opacity: 1 },
  box:     { margin: 8, padding: 8, borderRadius: 8, borderWidth: StyleSheet.hairlineWidth, borderColor: T.muted, gap: 6 },
  ro:      { flexDirection: 'column', gap: 6, alignItems: 'stretch' },   // ★ BIG: the globe on its own row, full width
  roSmall: { flexDirection: 'row', gap: 8, alignItems: 'center' },      // ★ SMALL: the globe beside the words
  headLine:{ flex: 1, minWidth: 0, fontFamily: F, fontSize: 11, color: T.muted },
  q:       { color: T.value, borderRadius: 3, overflow: 'hidden' },
  q_EXCELLENT: { backgroundColor: 'rgba(255,176,0,0.42)' },
  q_GOOD:      { backgroundColor: 'rgba(255,176,0,0.24)' },
  q_MARGINAL:  { backgroundColor: 'rgba(255,176,0,0.10)' },
  q_POOR:      { color: T.muted },
  coast:   { opacity: 0.6 },
  big:     { fontFamily: F, fontSize: 18, color: T.value, fontVariant: ['tabular-nums'], ...engraveStyle(T) },
  img:     { marginTop: 4 },
  hidden:  { display: 'none' },
  pk:      { gap: 2, marginTop: 2 },
  // ★★★ beta92 the line switch: a line is a key only while both have a decoder view; the one shown is lit
  ln:      { borderRadius: 6, borderWidth: StyleSheet.hairlineWidth, borderColor: 'transparent', paddingVertical: 3, minHeight: 30, justifyContent: 'center' },
  lnOn:    { borderColor: T.accent, backgroundColor: T.rowActive },
  lnTxt:   { paddingHorizontal: 8 },
  lnTxtOn: { color: T.value },
  vbKey:   { alignSelf: 'flex-start', paddingLeft: 8, paddingTop: 2 },
  mono:    { fontFamily: F, fontSize: 11, lineHeight: 15, color: T.value },
  sumRow:  { paddingHorizontal: 10, paddingVertical: 8, minHeight: 36, justifyContent: 'center' },
  sum:     { fontFamily: F, fontSize: 12, letterSpacing: 1, color: T.value },
  rw:      { flexDirection: 'row', alignItems: 'center' },
  remKey:  { paddingRight: 6 },
  leadRow: { flexDirection: 'row', flexWrap: 'wrap', alignItems: 'center', gap: 6, paddingHorizontal: 10, paddingVertical: 6 },
  detailNp:{ fontFamily: F, fontSize: 11, lineHeight: 15, color: T.muted },
  key:     { textAlign: 'center', marginTop: 2 },
  tlmGrid: { flexDirection: 'row', flexWrap: 'wrap', columnGap: 12 },
  keys:    { flexDirection: 'row', gap: 6 },
  tlmCell: { minWidth: 140 },
  remWrap: { position: 'absolute', left: 0, right: 0, alignItems: 'center', gap: 6, zIndex: 60 },
  remToast:{ width: '94%', maxWidth: 460, padding: 10, borderRadius: 10, borderWidth: StyleSheet.hairlineWidth, borderColor: T.border,
             backgroundColor: T.solidBg ?? T.window?.bg ?? 'rgba(8,6,2,0.96)', gap: 4,
             shadowColor: '#000', shadowOpacity: 0.5, shadowRadius: 16, shadowOffset: { width: 0, height: 8 }, elevation: 12 },
  remWhy:  { fontFamily: F, fontSize: 11, lineHeight: 15, color: T.muted },
  remKeys: { flexDirection: 'row', justifyContent: 'flex-end', gap: 8, marginTop: 2 },
  remMore: { textAlign: 'center' },
  row:     { flex: 1, paddingHorizontal: 10, paddingVertical: 7, gap: 2, minHeight: 44, justifyContent: 'center' },
  rowOn:   { backgroundColor: T.rowActive },
  grey:    { opacity: 0.45 },
  up:      { color: T.accent },
  nameRow: { flexDirection: 'row', justifyContent: 'space-between', gap: 8 },
  txt:     { fontFamily: F, fontSize: 12, lineHeight: 17, color: T.value, flexShrink: 1 },
  txtCol:  { flex: 1, minWidth: 0, gap: 1 },
  detail:  { fontFamily: F, fontSize: 11, lineHeight: 15, color: T.muted, flexShrink: 1, paddingHorizontal: 10 },
  note:    { fontFamily: F, fontSize: 13, color: T.muted, padding: 12, lineHeight: 18 },
});
