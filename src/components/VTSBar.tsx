/**
 * VTSBar — accessibility-skin popup notification bar (lsv-a11y-notif parity).
 * Pops up above the controls pill for 8s when:
 *   - the tuned frequency lands on / near a bookmark (station name + offset)
 *   - a band-plan boundary is crossed (band info, ham conditions colouring)
 * The skin's "TAP ◄ ► TO JUMP" tuning-guide hint is intentionally NOT ported
 * (it was erratic on the popup bar).
 * Overflowing text slides across once, like the skin's a11y-scrolling.
 */

import React, { useEffect, useMemo, useRef, useState } from 'react';
import { Animated, AppState, Easing, Image, ScrollView, StyleSheet, Text, View } from 'react-native';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import { useFaceplate, useFaceplateOnTrial } from '../contexts/FaceplateContext';
import AnnunciatorLegend from './AnnunciatorLegend';
import RdsMark from './RdsMark';
import DabMark from './DabMark';
import SectionIcon from './SectionIcon';
import { GhostGrid } from './VfdParts';
import { rgba, FONT_HYPER, FONT_DOTO, FONT_SEG14 } from '../constants/faceplate';
import {
  cellWindow, cellWindowLeft, flagToIso, segGhost, steppedOffset, toSegCells, toSegRun, toUpperDisplay,
  vfdStripText, VFD_PAUSE_MS, VFD_STEP_MS,
} from '../constants/displayText';
import { vtsIdText, vtsJoin, vtsLineSegments, vtsStationText, type VtsIdLabel } from '../services/vtsLine';

/* ★ The RDS mark is the vector mark (RdsMark, §7.1) in the strip's own colour. It replaced the fixed
 *  black-on-white `assets/rds-logo.png`, which a neon or VFD strip cannot carry. Never "ADVANCED". */

// Bookmark-source marks (uniform with the RDS logo): the node icon for a server
// bookmark, an "EiBi" text mark for the on-device EiBi schedule, and a phone
// glyph for the user's own (local) bookmarks.
// ★★ The node icon on EVERY backend (B12, Stuart: "replace the server bookmarks icon for all servers
//    with the node icon"). Backend logos fell back to UberSDR's on a VibeServer (the native pump
//    reports serverType 'local'), and a bookmark's source is "this receiver", not a brand. It is the
//    same glyph as the Servers chip.

export interface VtsNotifData {
  key:        number;   // bump to re-trigger even with identical text
  name:       string;
  /** ★ LIVE station line (2026-10-01): the message (RDS RadioText / DAB DLS) and the identity (RDS PI /
   *  DAB SId, hex), kept APART from `name` so the bar composes "PI: C363 / Name: RadioText" itself —
   *  vtsLine.ts, the same file the web client uses — and a VFD can fold each part for its glass. */
  rt?:        string;
  id?:        string;
  idLabel?:   VtsIdLabel;
  secondary?: string;   // overlap band names (band notifs only)
  offset?:    string;   // "-1.2kHz" distance to the station
  tuneDir?:   'left' | 'right';  // which way to tune to reach it
  kind:       'station-on' | 'station-off' | 'band' | 'notice';
  /** ★ How long to hold it, ms. Omitted = NOTIF_MS. An EXPLANATION is not a band announcement:
   *  it is several sentences the reader has to get through, and 8 s is not enough to read one
   *  and watch it scroll. Per-notif rather than per-kind so the caller that knows how much it
   *  wrote decides. */
  ms?:        number;
  color?:     string;   // band-condition override for the primary text
  hold?:      boolean;  // stay up (no auto-dismiss) — digital-voice caller display
  badge?:     string;   // live-data tag (e.g. 'RDS', 'DMR') — shown before the name
  source?:    'eibi' | 'server' | 'user';  // bookmark origin → source icon
  flag?:      string;   // transmitter-country flag (EiBi bookmarks / RDS)
  logoUrl?:   string;   // resolved WFM RDS station logo (radio-browser favicon)
  /** ★★ CAR-STEREO ANNUNCIATORS (Stuart, 2026-10-02: "go full car stereo and show TP/TA/AF") — drawn at
   *  the strip's right end, ALWAYS, each LIT when true and dark (a ghost, like a VFD's unlit segments)
   *  when false: TP = the station carries traffic programmes, TA = a traffic announcement is on NOW,
   *  AF = it broadcasts an alternative-frequency list.
   *  ★ Absent = no cluster at all. Only a source that genuinely has all three may pass it — an
   *    annunciator that can never light reads as a broken feature (AGENTS.md). Today: FM-DX. A
   *    VibeServer's plain RDS line carries none of the three (they ride the Advanced RDS stream). */
  annunciators?: { tp: boolean; ta: boolean; af: boolean };
  /** ★★ A DAB SERVICE (2026-10-02): the DabMark takes the RDS mark's slot (no RDS mark, no green "DAB"
   *  pill). `plus` = the service is DAB+ (AAC) — the "+]" group is drawn; false (MP2 / not yet known)
   *  keeps its room but draws MAIN only. */
  dab?: { plus: boolean };
}

const NOTIF_MS = 8000;
/** ★ The longest a VFD pass may stretch a timed notif (see onVfdPass). */
const VFD_PASS_CAP_MS = 30000;

/* ★ The strip's colours and font are the faceplate's TEXT role (constants/faceplate.ts `vts`): on the
 *  default deck they are today's on-tune green / off-tune amber / band yellow, and under the Nixie
 *  display every glyph drawn in Nixie One is neon (§2) — including a notice that carries its own
 *  colour, which is then ignored. */

export default function VTSBar({ notif, bottom, serverType, onHeight, freqLabel = '', onHoldExtended, onTimedEnd }:
    { notif: VtsNotifData | null; bottom: number; serverType?: string; onHeight?: (h: number) => void;
      /** ★ A timed notif was held longer than asked (a VFD's one full pass, onVfdPass): its key and the
       *  new total, so the screen's own deadline — which defers station names and starts the next
       *  queued notice — moves with it instead of cutting the pass short. */
      onHoldExtended?: (key: number, totalMs: number) => void;
      /** ★★ A timed notif has had its time and gone. The screen must CLEAR it (if it is still the one
       *  it holds), or the next mount of this bar — controls hidden and shown, Advanced RDS closed, a
       *  rotation, a Mac window resized — is handed the same old notif and plays it all over again:
       *  "Direct Sample Off · Gain restored" came back on the emulator a minute after it had gone. */
      onTimedEnd?: (key: number) => void;
      /** The tuned frequency as text ("7310 kHz") — what a VFD shows when a name folds to nothing
       *  it can draw (§7: Cyrillic, CJK… until native transliteration lands). */
      freqLabel?: string }) {
  useFaceplateOnTrial();   // ★★★ the VFD strip is faceplate too (constants/faceplate.ts CRASH SAFETY)
  const fp = useFaceplate();
  const COL = fp.vts;
  const [shown, setShown] = useState<VtsNotifData | null>(null);
  const shownRef = useRef<VtsNotifData | null>(null);
  const fade    = useRef(new Animated.Value(0)).current;
  const insets  = useSafeAreaInsets();
  const side    = Math.max(14, insets.left, insets.right);
  const slide   = useRef(new Animated.Value(0)).current;
  const hideRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const [areaW, setAreaW] = useState(0);
  const [textW, setTextW] = useState(0);
  /** The two side blocks' natural widths — each reserves the wider, so the text window is centred. */
  const [leftW, setLeftW] = useState(0);
  const [rightW, setRightW] = useState(0);
  const sideW = Math.max(leftW, rightW);

  // Report height 0 the moment we're hidden, so anything stacked above us (the decoder box)
  // drops back down instead of floating over the gap where the VTS used to be.
  useEffect(() => { if (!shown) onHeight?.(0); }, [shown, onHeight]);

  /** True from the moment the bar starts to appear until it starts to leave. */
  const visibleRef = useRef(false);

  /* ★★ `finished` — a fade-out that a NEW notif interrupts (it resets the opacity and fades in) is
   *  stopped, and its completion must not then null the bar under the newcomer. */
  const fadeOut = () => {
    visibleRef.current = false;
    Animated.timing(fade, { toValue: 0, duration: 300, useNativeDriver: true })
      .start(({ finished }) => { if (finished) { setShown(null); shownRef.current = null; } });
  };
  const dismiss = () => {
    if (hideRef.current) { clearTimeout(hideRef.current); hideRef.current = null; }
    fadeOut();
  };
  // ★ Read through a ref: the hide timer is armed once per notif and must call the CURRENT callback.
  const onTimedEndRef = useRef(onTimedEnd);
  onTimedEndRef.current = onTimedEnd;
  /** When the showing TIMED notif appeared, and how long it was asked to stay (for the VFD pass). */
  const shownAtRef = useRef(0);
  const holdMsRef = useRef(0);
  /* ★★ A VFD NOTICE STAYS FOR ONE WHOLE PASS. The strip steps one cell every 300 ms after a 1.5 s
   *  pause, so on a narrow glass "Direct Sample Active · Gain not available" needs longer than the 7 s
   *  the notice asked for, and the bar used to leave mid-word. The strip reports what one pass takes
   *  (VfdStrip onPassMs) and the hide timer is pushed out to it — never shortened, and capped, so a
   *  very long line on a very narrow glass still goes. The pixel slide needs none of this: its
   *  duration is fitted to the notif's time already. */
  const onVfdPass = (passMs: number) => {
    const cur = shownRef.current;
    if (!cur || cur.hold || !visibleRef.current || !hideRef.current) return;
    const want = Math.min(passMs, VFD_PASS_CAP_MS);
    if (want <= holdMsRef.current) return;
    holdMsRef.current = want;
    onHoldExtended?.(cur.key, want);
    clearTimeout(hideRef.current);
    const k = cur.key;
    hideRef.current = setTimeout(() => { hideRef.current = null; fadeOut(); onTimedEndRef.current?.(k); },
                                 Math.max(0, shownAtRef.current + want - Date.now()));
  };

  useEffect(() => {
    if (!notif) {
      /* ★★★ AN EXPLICIT CLEAR TAKES DOWN WHATEVER IS SHOWING — TIMED OR HELD (B10, 2026-10-01).
       *  This used to fade only a HELD notif and leave a timed one to "self-dismiss" — but the
       *  effect's own cleanup had ALREADY cancelled that timer when `notif` went to null (the deps
       *  change), so a timed notice that was cleared stayed on screen FOR EVER: the VFD strip
       *  parked on its last cells ("…MPLING MODE FOR HF – THE TUNER IS BYPASSED…") long after.
       *  Reproduced on the emulator with the gain-at-minimum notice: its 30 s withdrawal
       *  (setVtsNotif → null, by key) lands just before the bar's own 30 s timer, and the strip
       *  then sat frozen on "…R IS DEA…" for minutes. The other way in is the RDS-cleared path —
       *  FM → 7.092 MHz: the direct-sampling notice arrives, then the station clears to null.
       *  ★ A clear is a decision the screen made, so it is obeyed now; a notice the screen wants
       *    to KEEP is protected there (SDRScreen does not clear a showing notice for RDS). */
      if (shownRef.current && visibleRef.current) dismiss();
      return;
    }
    /* ★★★ LIVE DATA UPDATES IN PLACE — IT DOES NOT RE-ENTER (2026-09-29). Every held RDS update
     *  (a new RadioText, a logo landing, a PS change) used to snap the bar to transparent and fade
     *  it back in, and reset the scroll — so on a Brazilian station whose PS rotates every second
     *  or two the bar blinked continuously: "flickers like a broken element". A held notif
     *  replacing a held notif that is ON SCREEN is the same bar with new words: swap the content,
     *  leave the opacity alone. The scroll restarts only if the TEXT changed (the slide effect is
     *  keyed on it). Timed notifs (bookmark / band / notice) still make their entrance. */
    const inPlace = !!notif.hold && !!shownRef.current?.hold && visibleRef.current;
    const textChanged = !shownRef.current || lineKey(shownRef.current) !== lineKey(notif);
    setShown(notif);
    shownRef.current = notif;
    if (!inPlace) {
      setTextW(0);
      slide.setValue(0);
      fade.setValue(0);
      Animated.timing(fade, { toValue: 1, duration: 180, useNativeDriver: true }).start();
    } else if (textChanged) {
      slide.setValue(0);
    }
    visibleRef.current = true;
    if (hideRef.current) { clearTimeout(hideRef.current); hideRef.current = null; }
    // Live data (RDS/DMR/DAB) holds on screen; static (bookmark/band) times out.
    if (!notif.hold) {
      shownAtRef.current = Date.now();
      holdMsRef.current = notif.ms ?? NOTIF_MS;
      const k = notif.key;
      hideRef.current = setTimeout(() => { hideRef.current = null; fadeOut(); onTimedEndRef.current?.(k); },
                                   holdMsRef.current);
    }
    return () => { if (hideRef.current) clearTimeout(hideRef.current); };
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [notif?.key, notif === null]);

  // Slide for overflowing text. Held (live) notifs loop continuously so the long
  // RDS radiotext keeps marqueeing; timed notifs do the skin's one-shot a11y slide.
  useEffect(() => {
    // ★ A VFD strip never slides by pixels — VfdStrip steps whole cells (§7).
    if (COL.style === 'dot' || COL.style === 'seg') return;
    if (!shown || !areaW || !textW || textW <= areaW) return;
    const dist = textW - areaW;
    slide.setValue(0);
    if (shown.hold) {
      const dur = Math.max(2200, dist * 16);   // scroll speed scales with length
      const anim = Animated.loop(Animated.sequence([
        Animated.delay(1200),
        Animated.timing(slide, { toValue: -dist, duration: dur, easing: Easing.linear, useNativeDriver: true }),
        Animated.delay(1500),
        Animated.timing(slide, { toValue: 0, duration: 0, useNativeDriver: true }),
      ]));
      anim.start();
      return () => anim.stop();
    }
    Animated.timing(slide, {
      toValue: -dist,
      duration: Math.max(1500, (shown.ms ?? NOTIF_MS) - 2500),
      delay: 900,
      easing: Easing.linear,
      useNativeDriver: true,
    }).start();
  // ★ A held (live) notif restarts its scroll only when its TEXT changes — a new key carrying the
  //   same words (a logo arriving, a flag) must not throw the reader back to the start.
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [shown?.hold ? lineKey(shown) : shown?.key, areaW, textW]);

  if (!shown) return null;

  const onTune  = shown.kind === 'station-on';
  // ★ A notice reads as a band announcement — same amber, same static treatment. It is our own
  //   words either way, and giving it a fourth colour would imply a distinction that is not there.
  const isBand  = shown.kind === 'band' || shown.kind === 'notice';
  const nameCol = (COL.allowOverride ? shown.color : undefined) ?? (isBand ? COL.band : onTune ? COL.onTune : COL.offTune);
  // Arrows: green pair when on tune; otherwise the side you need to tune
  // toward lights amber, the other dims (skin vts-arrow-active/dim)
  const leftCol  = onTune ? COL.onTune : shown.tuneDir === 'left' ? COL.offTune : COL.dim;
  const rightCol = onTune ? COL.onTune : shown.tuneDir === 'right' ? COL.offTune : COL.dim;
  const tuneLeft = shown.tuneDir === 'left';
  const overflow = textW > areaW && areaW > 0;
  const vfd = COL.style === 'dot' || COL.style === 'seg';
  // ★ The offset carries a UNIT ("-1.2kHz"): never through the 14-segment (it has no lower case) —
  //   the sans on seg; Doto keeps the unit's case on dot.
  const offsetFont = COL.style === 'seg' ? FONT_HYPER : COL.font;
  const offsetText = COL.style === 'dot' ? toUpperDisplay(shown.offset ?? '') : shown.offset;
  // ★ The scrolling line as tagged runs — the identity drawn in the sub colour, the rest as the name.
  const runs = vtsLineSegments({ id: shown.id, idLabel: shown.idLabel, name: shown.name, text: shown.rt });

  return (
    // ★★ TWO VIEWS, NOT ONE, PURELY SO THE BAR CAN BE CAPPED AND CENTRED. The outer one does the
    // positioning (absolute, inset 14, pinned to `bottom`); the inner one is the bar itself and
    // takes the same max-width cap as the control island, so on a Mac window or an iPad on an
    // external display the two line up instead of the station strip running the full width of the
    // glass under a centred island (Stuart, 2026-08-02, with a screenshot of exactly that).
    // ★ It cannot be done on a single view: this is `position: absolute` with BOTH left and right
    // set, and in that case Yoga resolves the position from `left` — a maxWidth alone would just
    // shrink the bar towards the left-hand edge rather than centring it.
    // ★ Clear the Dynamic Island / notch in landscape (B8, 17 Pro Max: the bar's left end ran under the
    //   island). ★★ SYMMETRICAL (Stuart): whatever is cut on the island's side is cut on the other side
    //   too, so the bar stays centred over the deck. The side insets are 0 in portrait: portrait keeps 14.
    <Animated.View style={[styles.wrap, { bottom, opacity: fade, left: side, right: side }]}
                   pointerEvents="none">
    <View style={[styles.bar, { backgroundColor: fp.chassis.vtsBg, borderColor: fp.chassis.vtsBorder }]}
      onLayout={(e: { nativeEvent: { layout: { height: number } } }) => onHeight?.(e.nativeEvent.layout.height)}>
      {/* ★ The SAME glyph as the right arrow, mirrored: Apple draws ◄ (U+25C4) and ► (U+25BA) from
          different fallback fonts, so the left one came out visibly smaller (B8, Mac + iPhone). */}
      <Text style={[styles.arrow, styles.arrowLeft, { color: leftCol }]}>►</Text>
      {/* ★★ THE LEFT BADGE BLOCK — the RDS mark, logo, flag or source. When the strip carries the
          TP · TA · AF cluster, this block and that one reserve the SAME width (the wider of the two), so the
          scrolling text sits centred on the strip whether or not anything is lit (Stuart, 2026-10-02). */}
      <View style={[styles.sideBlock, shown.annunciators && { minWidth: sideW }]}>
      <View style={styles.sideInner} onLayout={(e: { nativeEvent: { layout: { width: number } } }) => setLeftW(Math.ceil(e.nativeEvent.layout.width))}>
        {/* Source mark: live-data badge (RDS mark / text) wins; otherwise the
            bookmark-origin icon — backend logo, EiBi mark, or phone glyph.
            ★ On a VFD (dot / seg) the RDS annunciator is part of the GLASS: always there, lit only
              on RDS; a colour station logo or a flag emoji cannot exist there (§7.1). */}
        {/* ★★ IN DAB THE DAB MARK TAKES THE RDS MARK'S SLOT — in every style. There is no RDS on a DAB
            service, so a dark RDS annunciator beside it said nothing, and the green "DAB" pill that stood
            in for it belonged to no display style (Stuart, 2026-10-02). */}
        {vfd && (
          <View style={styles.vfdMarks}>
            {shown.dab
              ? <DabMark kind="picto" height={15} color={COL.core} glow={COL.glow} ghost={rgba(COL.rgb, 0.10)} plus={shown.dab.plus} />
              : <RdsMark kind="picto" height={13} color={COL.core} glow={COL.glow} ghost={rgba(COL.rgb, 0.10)}
                  lit={shown.badge === 'RDS'} />}
            <VfdIso style={COL.style as 'dot' | 'seg'} code={flagToIso(shown.flag)} rgb={COL.rgb} core={COL.core} glow={COL.glow} />
          </View>
        )}
        {/* ★★ The RDS mark STAYS when the station's logo lands (B9, Stuart: it vanished as the logo rendered
            in, though the bar has room): mark first, then the logo — the web bar's order. */}
        {!vfd && shown.badge === 'RDS' && !shown.dab && (
          <View style={styles.rdsMark}><RdsMark kind="plain" height={13} color={COL.mark} glow={COL.markGlow} /></View>
        )}
        {!vfd && !!shown.dab && (
          <View style={styles.rdsMark}><DabMark kind="plain" height={15} color={COL.mark} glow={COL.markGlow} ghost={rgba(COL.rgb, 0.16)} plus={shown.dab.plus} /></View>
        )}
        {!vfd && shown.logoUrl
          ? <Image source={{ uri: shown.logoUrl }} style={styles.staLogo} resizeMode="contain" />
          : !vfd && shown.badge === 'RDS'
          ? null
          : !!shown.badge && shown.badge !== 'RDS' && !shown.dab
            ? <Text style={styles.badge}>{shown.badge}</Text>
            : vfd && shown.badge === 'RDS'
              ? null
            : shown.source === 'server'
              ? <View style={styles.srcLogo}><SectionIcon name="instance" size={16} color={COL.mark} /></View>
              : shown.source === 'eibi'
                ? <Text style={styles.eibiMark}>EiBi</Text>
                : shown.source === 'user'
                  ? <Text style={styles.phoneMark}>📱</Text>
                  : null}
        {!vfd && !!shown.flag && <Text style={styles.flag}>{shown.flag}</Text>}
      </View>
      </View>
      {!!shown.offset && tuneLeft && <Text style={[styles.offset, { color: COL.offset, fontFamily: offsetFont }]}>{offsetText}</Text>}
      {vfd ? (
        <VfdStrip style={COL.style as 'dot' | 'seg'} rgb={COL.rgb} core={COL.core} glow={COL.glow}
          text={vfdLineText(shown, COL.style as 'dot' | 'seg', freqLabel)}
          loop={!!shown.hold}
          restartKey={shown.hold ? lineKey(shown) : String(shown.key)}
          onPassMs={shown.hold ? undefined : onVfdPass} />
      ) : (<>
      {/* Horizontal ScrollView = unconstrained content width, so the text
          measures at its TRUE size (a plain View clamps Text to the parent
          width and the overflow slide never triggers). scrollEnabled off —
          the slide is driven by the Animated translateX. */}
      <ScrollView
        horizontal
        scrollEnabled={false}
        showsHorizontalScrollIndicator={false}
        style={styles.nameArea}
        contentContainerStyle={!overflow ? styles.nameCentre : undefined}
        onLayout={(e: { nativeEvent: { layout: { width: number } } }) => setAreaW(e.nativeEvent.layout.width)}
        onContentSizeChange={(w: number) => setTextW(w)}
      >
        <Animated.View style={overflow ? { transform: [{ translateX: slide }] } : undefined}>
          <Text style={[styles.name, { color: nameCol, fontFamily: COL.font }]} numberOfLines={1}>
            {runs.map((r, i) => (
              <Text key={i} style={r.kind === 'id' ? [styles.idRun, { color: COL.sub }] : r.kind === 'sep' ? { color: COL.sub } : undefined}>
                {r.s}
              </Text>
            ))}
            {shown.secondary ? <Text style={[styles.secondary, { color: COL.sub }]}>{'  │  ' + shown.secondary}</Text> : null}
          </Text>
        </Animated.View>
      </ScrollView>
      </>)}
      {!!shown.offset && shown.tuneDir === 'right' && <Text style={[styles.offset, { color: COL.offset, fontFamily: offsetFont }]}>{offsetText}</Text>}
      {/* ★★ TP · TA · AF — fixed legends in the glass, drawn like the RDS mark (AnnunciatorLegend), on the
          RIGHT inside the ▶ so they balance the badge block on the left; lit when true, ghosted when not. */}
      {!!shown.annunciators && (
        <View style={[styles.sideBlock, styles.sideRight, { minWidth: sideW }]}>
          <View style={[styles.sideInner, styles.annun]}
                onLayout={(e: { nativeEvent: { layout: { width: number } } }) => setRightW(Math.ceil(e.nativeEvent.layout.width))}>
            {(['TP', 'TA', 'AF'] as const).map(nm => (
              <AnnunciatorLegend key={nm} name={nm} height={9} kind={vfd ? 'picto' : 'plain'}
                color={COL.core} glow={COL.glow} ghost={rgba(COL.rgb, vfd ? 0.10 : 0.16)}
                lit={shown.annunciators![nm.toLowerCase() as 'tp' | 'ta' | 'af']} />
            ))}
          </View>
        </View>
      )}
      <Text style={[styles.arrow, { color: rightCol }]}>►</Text>
    </View>
    </Animated.View>
  );
}

/** What a held bar's scroll restarts on: the WORDS (identity, name, message, sub-line) — never a
 *  logo or flag landing under the same words. */
function lineKey(n: VtsNotifData): string {
  return `${n.id ?? ''}|${n.name}|${n.rt ?? ''}|${n.secondary ?? ''}`;
}

/**
 * ★★ The station line for a VFD. The identity is folded APART from the station: a Cyrillic or CJK
 * name falls back to the frequency (vfdStripText), and folding "PI C363" in with it would let seven
 * Latin characters tip a mostly-unprintable name over foldIsUsable's threshold — the glass would
 * then show "PI C363 / !!!!!" instead of the frequency. On 14-segment the label has no colon (DSEG
 * has none: ':' becomes '-'), so it reads "PI C363", as a segment radio prints it.
 */
function vfdLineText(n: VtsNotifData, display: 'dot' | 'seg', freqLabel: string): string {
  const seg = display === 'seg';
  const idText = vtsIdText(n.id, n.idLabel ?? 'PI', seg);
  const station = vtsStationText(n.name, n.rt, seg);
  if (!idText) return vfdStripText(station, n.secondary, display, freqLabel);
  const body = station || n.secondary ? vfdStripText(station, n.secondary, display, freqLabel) : '';
  return vtsJoin(display === 'dot' ? toUpperDisplay(idText) : idText, body);
}

// ── The VFD strip (dot / seg) ────────────────────────────────────────────────

/** Cell widths from the fonts' own metrics: DSEG14 is 816/1000 em, Doto 600/1000 em (monospaced),
 *  plus the 1 pt letter-spacing both are drawn with (Deck.mockup). */
const SEG_PX = 15, DOT_PX = 19, CELL_LS = 1;
/** ★ Spare cells of width the window's Text is laid out with, so the font's fractional excess never ellipsizes the last cell. */
const TEXT_SLACK = 2;
const SEG_CELL = SEG_PX * 0.816 + CELL_LS;
const DOT_CELL = DOT_PX * 0.6 + CELL_LS;

/**
 * ★★ STEPPED, NEVER SMOOTH (§7, ref vfd-scroll.gif): a fixed window of whole cells; a long run waits
 * ~1.5 s, then moves ONE WHOLE CELL every ~300 ms — no easing, no pixel offsets. The ghost layer is
 * the window's own cells, always there. 14-segment: every character is one DSEG cell (toSegCells),
 * units included, in capitals — MHZ reads like the rest of the text (toSegRun, 2026-10-02). Dot: Doto, upper
 * case with the units' case kept, over the ghost-dot grid (also stepped per whole cell — the brief
 * allows per-column, and one rule for both reads as one machine).
 */
function VfdStrip({ style, rgb, core, glow, text, loop, restartKey, onPassMs }: {
  style: 'dot' | 'seg'; rgb: string; core: string; glow: string; text: string; loop: boolean; restartKey: string;
  /** A one-shot (timed) line: told how long one full pass takes here, pauses included. */
  onPassMs?: (ms: number) => void;
}) {
  const [w, setW] = useState(0);
  const seg = style === 'seg';
  const cellW = seg ? SEG_CELL : DOT_CELL;
  const n = Math.max(0, Math.floor(w / cellW));
  const run = useMemo(() => (seg ? toSegRun(text) : { cells: Array.from(text), units: [] }), [seg, text]);
  const count = run.cells.length;
  const [offset, setOffset] = useState(0);
  useEffect(() => {
    setOffset(0);
    if (count <= n || n <= 0) return;
    if (!loop) onPassMs?.(VFD_PAUSE_MS + (count - n) * VFD_STEP_MS + VFD_PAUSE_MS);
    const t0 = Date.now();
    // Ticks faster than a step so each step lands on time; the offset only CHANGES by a whole
    // cell, and setting an unchanged number does not re-render.
    // ★ NOT WHILE BACKGROUNDED / LOCKED (power audit 2026-10-01): background audio keeps the JS thread
    //   alive, and a held RDS radiotext marquees FOREVER — 10 ticks and ~3 re-renders a second behind
    //   a screen nobody can see, on the thread the audio path shares. The step is a function of the
    //   clock (Date.now() − t0), so on return it is simply where it would have been. (useClock's rule.)
    const id = setInterval(() => {
      if (AppState.currentState === 'active') setOffset(steppedOffset(Date.now() - t0, count, n, loop));
    }, VFD_STEP_MS / 3);
    return () => clearInterval(id);
  }, [restartKey, count, n, loop]);
  const win = cellWindow(run.cells, n, offset, seg ? '!' : ' ');
  const left = cellWindowLeft(count, n);
  const shift = count <= n ? -left : Math.max(0, Math.min(offset, count - n));
  const px = seg ? SEG_PX : DOT_PX;
  const common = { fontFamily: seg ? FONT_SEG14 : FONT_DOTO, fontSize: px, letterSpacing: CELL_LS,
                   lineHeight: Math.round(px * 1.25), includeFontPadding: false } as const;
  const lit = { color: core, textShadowColor: glow, textShadowRadius: 4, textShadowOffset: { width: 0, height: 0 } };
  return (
    <View style={styles.nameArea} onLayout={(e: any) => setW(e.nativeEvent.layout.width)}>
      {n > 0 && (
        /* ★★ THE LAST CELL WAS NEVER SHOWN (B10). n cells come out a hair wider than n × cellW on the
         *  real font, so a Text held to the window's width ran out of room by a fraction and the
         *  default single-line "tail" swapped the LAST cell for "…" — on every line: Stuart's Mac read
         *  "…GAIN CONTROL IS NOT AVAIL…", the emulator "RECT SAM…" in a 9-cell window, and a notice
         *  never showed its final letter. ellipsizeMode="clip" is NOT the fix — on Android a clipped
         *  single line breaks at a WORD and drops the rest ("RECT" alone; tried, B10). So the Text is
         *  given room to spare (TEXT_SLACK cells) and the window itself clips the spill. */
        <View style={{ width: n * cellW, alignSelf: 'center', justifyContent: 'center', overflow: 'hidden' }}>
          {seg
            ? <Text style={[common, { color: rgba(rgb, 0.10), width: (n + TEXT_SLACK) * cellW }]} numberOfLines={1}>{segGhost(n)}</Text>
            : <GhostGrid rgb={rgb} pitch={3} dot={0.7} />}
          <Text style={[common, lit, seg ? styles.overlay : null, { width: (n + TEXT_SLACK) * cellW }]} numberOfLines={1}>{win.join('')}</Text>
        </View>
      )}
    </View>
  );
}

/** The transmitter country as its ISO code in the display's own characters (§7.1): Doto 13 pt over
 *  its own ghost grid, or two DSEG14 cells at 11 pt over a `~~` ghost. No flag: the cells stay ghosted. */
function VfdIso({ style, code, rgb, core, glow }: { style: 'dot' | 'seg'; code: string; rgb: string; core: string; glow: string }) {
  const lit = { color: core, textShadowColor: glow, textShadowRadius: 3, textShadowOffset: { width: 0, height: 0 } };
  if (style === 'seg') {
    return (
      <View>
        <Text style={[styles.isoSeg, { color: rgba(rgb, 0.10) }]}>{segGhost(2)}</Text>
        <Text style={[styles.isoSeg, lit, styles.overlay]}>{code ? toSegCells(code) : '!!'}</Text>
      </View>
    );
  }
  return (
    <View style={{ paddingHorizontal: 2, paddingVertical: 1 }}>
      <GhostGrid rgb={rgb} pitch={2.6} dot={0.6} />
      <Text style={[styles.isoDot, lit]}>{code || '\u00a0\u00a0'}</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  // Positioning only — see the render for why the bar is a separate child.
  wrap: {
    position: 'absolute',
    left: 14,
    right: 14,
    alignItems: 'center',
  },
  bar: {
    // ★ THE SAME CAP AS ControlsBar, and it must STAY the same: these two sit one above the other
    //   and a mismatch is more obvious than either being wrong on its own.
    maxWidth: 1400,
    width: '100%',
    alignSelf: 'center',
    flexDirection: 'row',
    alignItems: 'center',
    paddingVertical: 11,
    paddingHorizontal: 12,
    borderRadius: 12,
    // ★ Background and border colours come from the faceplate's chassis (vtsBg / vtsBorder).
    borderWidth: 1,
    zIndex: 60,
  },
  arrowLeft: { transform: [{ scaleX: -1 }] },
  // ★ The side blocks: content-sized, but each may be told to reserve the other's width (sideW).
  sideBlock: { flexDirection: 'row', alignItems: 'center', justifyContent: 'flex-start', flexShrink: 0 },
  sideRight: { justifyContent: 'flex-end' },
  sideInner: { flexDirection: 'row', alignItems: 'center' },
  // ★ The TP · TA · AF cluster.
  annun:     { gap: 5, marginLeft: 6, marginRight: 2 },
  arrow: {
    fontFamily: 'Atkinson Hyperlegible',
    fontSize: 15,
    paddingHorizontal: 4,
  },
  badge: {
    color: '#0a0a0a',
    backgroundColor: '#52dc64',   // live-data accent (matches on-tune green)
    fontFamily: 'Atkinson Hyperlegible',
    fontSize: 11,
    fontWeight: '700',
    letterSpacing: 0.5,
    overflow: 'hidden',
    borderRadius: 4,
    paddingHorizontal: 5,
    paddingVertical: 2,
    marginRight: 4,
  },
  rdsMark: {
    marginRight: 5,
  },
  vfdMarks: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 5,
    marginLeft: 4,
    marginRight: 4,
  },
  staLogo: {
    width: 20,
    height: 20,
    backgroundColor: '#ffffff',
    borderRadius: 4,
    marginRight: 5,
  },
  srcLogo: {
    width: 16,
    height: 16,
    marginRight: 5,
  },
  eibiMark: {
    color: '#0a0a0a',
    backgroundColor: '#7fb3ff',   // distinct from the green live badge
    fontFamily: 'Atkinson Hyperlegible',
    fontSize: 10.5,
    fontWeight: '700',
    letterSpacing: 0.3,
    overflow: 'hidden',
    borderRadius: 4,
    paddingHorizontal: 5,
    paddingVertical: 2,
    marginRight: 5,
  },
  phoneMark: {
    fontSize: 14,
    marginRight: 4,
  },
  flag: {
    fontSize: 15,
    marginRight: 3,
  },
  offset: {
    fontSize: 13,
    paddingHorizontal: 2,
  },
  nameArea: {
    flex: 1,
    marginHorizontal: 6,
  },
  nameCentre: {
    flexGrow: 1,
    justifyContent: 'center',
  },
  overlay: {
    position: 'absolute',
    left: 0,
    top: 0,
  },
  isoSeg: {
    fontFamily: 'DSEG14 Classic',
    fontSize: 11,
    lineHeight: 13,
    letterSpacing: 1,
    includeFontPadding: false,
  },
  isoDot: {
    fontFamily: 'Doto',
    fontSize: 13,
    lineHeight: 14,
    includeFontPadding: false,
  },
  name: {
    fontSize: 16,
    letterSpacing: 0.5,
  },
  secondary: {
    fontSize: 14,
  },
  // The PI / SId: a touch smaller, so the station's NAME is still what the eye lands on.
  idRun: {
    fontSize: 13,
    letterSpacing: 0.3,
  },
});
