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
import RdsMark from './RdsMark';
import { GhostGrid } from './VfdParts';
import { rgba, FONT_HYPER, FONT_DOTO, FONT_SEG14 } from '../constants/faceplate';
import {
  cellWindow, cellWindowLeft, flagToIso, segGhost, steppedOffset, toSegCells, toSegRun, toUpperDisplay,
  vfdStripText, VFD_STEP_MS,
} from '../constants/displayText';

/* ★ The RDS mark is the vector mark (RdsMark, §7.1) in the strip's own colour. It replaced the fixed
 *  black-on-white `assets/rds-logo.png`, which a neon or VFD strip cannot carry. Never "ADVANCED". */

// Bookmark-source marks (uniform with the RDS logo): the backend logo for a
// server bookmark, an "EiBi" text mark for the on-device EiBi schedule, and a
// phone glyph for the user's own (local) bookmarks.
const SERVER_LOGOS: Record<string, any> = {
  ubersdr: require('../../assets/logo_ubersdr.png'),
  owrx:    require('../../assets/logo_owrx.png'),
  kiwi:    require('../../assets/logo_kiwi.png'),
  // ★ VibeServer had no logo, so our OWN server fell back to a generic radio glyph
  //   in the very list where every other backend is branded (Stuart, 2026-07-29).
  vibeserver: require('../../assets/logo_vibeserver.png'),
};

export interface VtsNotifData {
  key:        number;   // bump to re-trigger even with identical text
  name:       string;
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
}

const NOTIF_MS = 8000;

/* ★ The strip's colours and font are the faceplate's TEXT role (constants/faceplate.ts `vts`): on the
 *  default deck they are today's on-tune green / off-tune amber / band yellow, and under the Nixie
 *  display every glyph drawn in Nixie One is neon (§2) — including a notice that carries its own
 *  colour, which is then ignored. */

export default function VTSBar({ notif, bottom, serverType, onHeight, freqLabel = '' }:
    { notif: VtsNotifData | null; bottom: number; serverType?: string; onHeight?: (h: number) => void;
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

  // Report height 0 the moment we're hidden, so anything stacked above us (the decoder box)
  // drops back down instead of floating over the gap where the VTS used to be.
  useEffect(() => { if (!shown) onHeight?.(0); }, [shown, onHeight]);

  /** True from the moment the bar starts to appear until it starts to leave. */
  const visibleRef = useRef(false);

  const dismiss = () => {
    if (hideRef.current) { clearTimeout(hideRef.current); hideRef.current = null; }
    visibleRef.current = false;
    Animated.timing(fade, { toValue: 0, duration: 300, useNativeDriver: true })
      .start(() => { setShown(null); shownRef.current = null; });
  };

  useEffect(() => {
    if (!notif) {
      // Explicit clear (e.g. live data ended / mode change) — a held (live) notif
      // has no auto-dismiss timer, so fade it out here. Timed notifs self-dismiss.
      if (shownRef.current?.hold) dismiss();
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
    const textChanged = shownRef.current?.name !== notif.name
                     || shownRef.current?.secondary !== notif.secondary;
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
      hideRef.current = setTimeout(() => {
        visibleRef.current = false;
        Animated.timing(fade, { toValue: 0, duration: 300, useNativeDriver: true })
          .start(() => { setShown(null); shownRef.current = null; });
      }, notif.ms ?? NOTIF_MS);
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
  }, [shown?.hold ? `${shown.name}|${shown.secondary ?? ''}` : shown?.key, areaW, textW]);

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
      <Text style={[styles.arrow, { color: leftCol }]}>◄</Text>
      {/* Source mark: live-data badge (RDS mark / text) wins; otherwise the
          bookmark-origin icon — backend logo, EiBi mark, or phone glyph.
          ★ On a VFD (dot / seg) the RDS annunciator is part of the GLASS: always there, lit only
            on RDS; a colour station logo or a flag emoji cannot exist there (§7.1). */}
      {vfd && (
        <View style={styles.vfdMarks}>
          <RdsMark kind="picto" height={13} color={COL.core} glow={COL.glow} ghost={rgba(COL.rgb, 0.10)}
            lit={shown.badge === 'RDS'} />
          <VfdIso style={COL.style as 'dot' | 'seg'} code={flagToIso(shown.flag)} rgb={COL.rgb} core={COL.core} glow={COL.glow} />
        </View>
      )}
      {!vfd && shown.logoUrl
        ? <Image source={{ uri: shown.logoUrl }} style={styles.staLogo} resizeMode="contain" />
        : !vfd && shown.badge === 'RDS'
        ? <View style={styles.rdsMark}><RdsMark kind="plain" height={13} color={COL.mark} glow={COL.markGlow} /></View>
        : !!shown.badge && shown.badge !== 'RDS'
          ? <Text style={styles.badge}>{shown.badge}</Text>
          : vfd && shown.badge === 'RDS'
            ? null
          : shown.source === 'server'
            ? <Image source={SERVER_LOGOS[serverType ?? 'ubersdr'] ?? SERVER_LOGOS.ubersdr}
                style={[styles.srcLogo, serverType === 'owrx' && styles.srcLogoLight]} resizeMode="contain" />
            : shown.source === 'eibi'
              ? <Text style={styles.eibiMark}>EiBi</Text>
              : shown.source === 'user'
                ? <Text style={styles.phoneMark}>📱</Text>
                : null}
      {!vfd && !!shown.flag && <Text style={styles.flag}>{shown.flag}</Text>}
      {!!shown.offset && tuneLeft && <Text style={[styles.offset, { color: COL.offset, fontFamily: offsetFont }]}>{offsetText}</Text>}
      {vfd ? (
        <VfdStrip style={COL.style as 'dot' | 'seg'} rgb={COL.rgb} core={COL.core} glow={COL.glow}
          text={vfdStripText(shown.name, shown.secondary, COL.style as 'dot' | 'seg', freqLabel)}
          loop={!!shown.hold}
          restartKey={shown.hold ? `${shown.name}|${shown.secondary ?? ''}` : String(shown.key)} />
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
            {shown.name}
            {shown.secondary ? <Text style={[styles.secondary, { color: COL.sub }]}>{'  │  ' + shown.secondary}</Text> : null}
          </Text>
        </Animated.View>
      </ScrollView>
      </>)}
      {!!shown.offset && shown.tuneDir === 'right' && <Text style={[styles.offset, { color: COL.offset, fontFamily: offsetFont }]}>{offsetText}</Text>}
      <Text style={[styles.arrow, { color: rightCol }]}>►</Text>
    </View>
    </Animated.View>
  );
}

// ── The VFD strip (dot / seg) ────────────────────────────────────────────────

/** Cell widths from the fonts' own metrics: DSEG14 is 816/1000 em, Doto 600/1000 em (monospaced),
 *  plus the 1 pt letter-spacing both are drawn with (Deck.mockup). */
const SEG_PX = 15, DOT_PX = 19, CELL_LS = 1;
const SEG_CELL = SEG_PX * 0.816 + CELL_LS;
const DOT_CELL = DOT_PX * 0.6 + CELL_LS;

/**
 * ★★ STEPPED, NEVER SMOOTH (§7, ref vfd-scroll.gif): a fixed window of whole cells; a long run waits
 * ~1.5 s, then moves ONE WHOLE CELL every ~300 ms — no easing, no pixel offsets. The ghost layer is
 * the window's own cells, always there. 14-segment: every character is one DSEG cell (toSegCells);
 * units are drawn in the sans over their blank cells, never through the segments. Dot: Doto, upper
 * case with the units' case kept, over the ghost-dot grid (also stepped per whole cell — the brief
 * allows per-column, and one rule for both reads as one machine).
 */
function VfdStrip({ style, rgb, core, glow, text, loop, restartKey }: {
  style: 'dot' | 'seg'; rgb: string; core: string; glow: string; text: string; loop: boolean; restartKey: string;
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
        <View style={{ width: n * cellW, alignSelf: 'center', justifyContent: 'center' }}>
          {seg
            ? <Text style={[common, { color: rgba(rgb, 0.10) }]} numberOfLines={1}>{segGhost(n)}</Text>
            : <GhostGrid rgb={rgb} pitch={3} dot={0.7} />}
          <Text style={[common, lit, seg ? styles.overlay : null]} numberOfLines={1}>{win.join('')}</Text>
          {seg && run.units.map((u, i) => {
            const at = u.at - shift;
            if (at < 0 || at + u.len > n) return null;
            return (
              <Text key={i} style={[styles.segUnit, lit, { left: at * cellW, width: u.len * cellW }]} numberOfLines={1}>
                {u.text}
              </Text>
            );
          })}
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
    width: 26,
    height: 16,
    marginRight: 5,
  },
  // OWRX's logo is a black antenna that vanishes on the dark bar — sit it on a
  // light chip, same as the menu footer.
  srcLogoLight: {
    backgroundColor: '#ffffff',
    borderRadius: 4,
    paddingHorizontal: 3,
    width: 30,
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
  segUnit: {
    position: 'absolute',
    top: 0,
    bottom: 0,
    textAlign: 'center',
    textAlignVertical: 'center',
    fontFamily: 'Atkinson Hyperlegible',
    fontSize: 12,
    lineHeight: 19,
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
});
