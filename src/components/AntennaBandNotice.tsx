/**
 * ★★★ "FM BAND-STOP FILTER FITTED — RECEPTION HERE IS DELIBERATELY REDUCED" (Stuart, 2026-10-06).
 *
 * The owner says on the setup page which ranges the aerial covers and which filters sit in the feed
 * (RadioConfig::antennaRanges / antennaFilters, parsed by src/utils/antennaBands.ts — the same file
 * the web client uses). This tells the listener when they tune into one, so a deliberately filtered
 * band is not mistaken for a poor receiver: "that way they don't think the user's SDR or our software
 * is shit".
 *
 * ★★ ONCE PER ENTRY, THEN QUIET. The full sentence shows when you CROSS INTO the zone, for a few
 *    seconds; then it settles to the short form ("FM band-stop filter fitted") and stays, dimmed,
 *    for as long as you are inside — so someone flicking about the FM band is told once and never
 *    nagged. Tapping the quiet form brings the sentence back. Leaving the zone clears it, so coming
 *    back is a new entry (antennaNoticeTrack).
 * ★★ NOT A MODAL, and not dismissible: it is a standing fact about where you are tuned, like the
 *    SHARED TUNER banner, and it goes away by itself when it stops being true.
 * ★ THE CONTROLS' OWN CLOTHES. Font and colour come from the faceplate the user picked — Doto in the
 *   text colour inside a metal chassis (as the status display draws, ControlsBar StatusWell), the
 *   banner font (Nixie One under the Nixie display) on the default deck — with the frequency pill's
 *   dark window behind it. Transparency OFF draws it opaque with no shadow (useSurface).
 */
import React, { useEffect, useMemo, useRef, useState } from 'react';
import { StyleSheet, Text, TouchableOpacity, type LayoutChangeEvent } from 'react-native';
import { useFaceplate, useSurface } from '../contexts/FaceplateContext';
import { FONT_DOTO, NO_DROP_SHADOW } from '../constants/faceplate';
import { useUiScale } from '../hooks/useUiScale';
import { antennaNoticeAt, antennaNoticeTrack, hasAntennaBands, type AntennaBands } from '../utils/antennaBands';

/** How long the full sentence stays before it settles to the quiet form. */
const LOUD_MS = 7000;

export default function AntennaBandNotice({ hz, bands, port, bottom, onHeight }: {
  hz: number;
  bands: AntennaBands | null;
  /** The socket in use (caps.antenna) — entries tied to another socket are ignored. */
  port?: string | null;
  bottom: number;
  /** Reports the drawn height (0 when hidden), so the decoder box can stack above it. */
  onHeight?: (h: number) => void;
}) {
  const fp = useFaceplate();
  const surf = useSurface();
  const s = useUiScale();
  const notice = useMemo(() => (hasAntennaBands(bands) ? antennaNoticeAt(hz, bands, port) : null),
                         [hz, bands, port]);
  const keyRef = useRef<string | null>(null);
  const [loud, setLoud] = useState(false);
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null);

  const shout = () => {
    setLoud(true);
    if (timer.current) clearTimeout(timer.current);
    timer.current = setTimeout(() => setLoud(false), LOUD_MS);
  };

  useEffect(() => {
    const t = antennaNoticeTrack(keyRef.current, notice);
    keyRef.current = t.key;
    if (t.entered) shout();
    else if (!notice) { setLoud(false); if (timer.current) clearTimeout(timer.current); }
  }, [notice]);
  // ★ Unmounted (controls hidden, another radio) = no height: the stack above must not keep room for it.
  useEffect(() => () => { if (timer.current) clearTimeout(timer.current); onHeight?.(0); },
            // eslint-disable-next-line react-hooks/exhaustive-deps
            []);
  useEffect(() => { if (!notice) onHeight?.(0); }, [notice, onHeight]);

  if (!notice) return null;

  const plate = fp.chassis.plate;
  // ★ Doto is a dot-matrix face with a small glyph set: the dash and the curly apostrophe are spelt
  //   in ASCII for it, as a real VFD would have to.
  const dot = !!plate;
  const raw = loud ? notice.text : notice.short;
  const text = dot ? raw.replace(/—/g, '-').replace(/’/g, "'") : raw;
  const color = dot ? fp.text.core : fp.deck.style === 'nixie' ? fp.deck.bannerFree : '#ffd479';
  const glow = dot ? fp.text.glow : fp.deck.style === 'nixie' ? fp.deck.modeGlow : 'transparent';

  return (
    <TouchableOpacity
      activeOpacity={0.85}
      onPress={shout}
      onLayout={(e: LayoutChangeEvent) => onHeight?.(Math.round(e.nativeEvent.layout.height))}
      accessibilityRole="text"
      accessibilityLabel={notice.text}
      style={[st.pill, {
        bottom,
        backgroundColor: surf.fill('rgba(20,10,0,0.92)'),
        borderColor: plate ? plate.border : fp.chassis.sharedBorder,
        opacity: loud ? 1 : 0.78,
        maxWidth: s.isLandscape ? '60%' : '92%',
        paddingHorizontal: s.r(12), paddingVertical: s.r(5),
      }, surf.dropShadow ? st.shadow : NO_DROP_SHADOW]}>
      <Text numberOfLines={loud ? 2 : 1} adjustsFontSizeToFit={!loud} minimumFontScale={0.8}
            style={{
              color, textAlign: 'center',
              fontFamily: dot ? FONT_DOTO : fp.deck.bannerFont,
              // ★ Doto's 10 pt floor (§8.2): dot matrix falls apart below it.
              fontSize: dot ? Math.max(10, s.f(12)) : Math.max(11, s.f(12.5)),
              fontWeight: dot ? 'normal' : '600', letterSpacing: dot ? 0.4 : 0.3,
              textShadowColor: glow, textShadowRadius: glow === 'transparent' ? 0 : 4,
              textShadowOffset: { width: 0, height: 0 },
            }}>
        {text}
      </Text>
    </TouchableOpacity>
  );
}

const st = StyleSheet.create({
  // ★ zIndex 240 = the idle-terms pill's: above the decoder box (200), which stacks above us anyway.
  pill: { position: 'absolute', alignSelf: 'center', zIndex: 240, borderWidth: 1, borderRadius: 8 },
  shadow: { shadowColor: '#000', shadowOpacity: 0.6, shadowRadius: 5, shadowOffset: { width: 0, height: 1 }, elevation: 4 },
});
