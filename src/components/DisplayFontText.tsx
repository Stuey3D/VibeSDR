/**
 * DisplayFontText — a line of a BIG on-screen element in the main display's font (★ 2026-10-06).
 *
 * Stuart, RC18 on Nixie: "Whatever font the main display is set to is for all the big on screen elements should
 * follow, only decoder boxes etc need to have the hyperlegible as those have super small text." The notice pills over
 * the controls (SDRScreen's idle-terms notice and rotate hints) draw their text through this, so they read
 * `fp.screen` (faceplate.ts ScreenText — the one source the status row, the SHARED TUNER banners, the mode box, the
 * DAB meter and AntennaBandNotice read too):
 *   • the font — Atkinson / Nixie One / Doto / DSEG14, the text made drawable by screenString();
 *   • its size × sizeK (DSEG14's cell is the whole em), and no bold on a one-weight face;
 *   • the element's own colour on Hyperlegible; the display's colour and glow on a lit display (Nixie neon, a
 *     one-colour VFD) — unless `keepColor`, for a meaning colour (a warning) no faceplate may replace.
 * ★ Subscribes to the faceplate itself, so the screen around it (SDRScreen) does not have to.
 */
import React from 'react';
import { StyleSheet, type TextProps } from 'react-native';
import { useFaceplate } from '../contexts/FaceplateContext';
import { screenInk, screenOneWeight } from '../constants/faceplate';
import { screenString } from '../constants/displayText';
import SegLowerDText from './SegLowerDText';

export default function DisplayFontText({ style, children, keepColor = false, wrap = false, ...rest }:
    Omit<TextProps, 'children'> & { children: string; keepColor?: boolean;
      /** A sentence that may wrap (a segment string gets break points after its blank cells). */
      wrap?: boolean }) {
  const sc = useFaceplate().screen;
  const flat = StyleSheet.flatten(style) ?? {};
  const own = typeof flat.color === 'string' ? flat.color : '#ffffff';
  const size = typeof flat.fontSize === 'number' ? flat.fontSize : 12;
  const lit = !sc.allowOverride && !keepColor;
  // ★ SegLowerDText (2026-10-06): on VCR a dB unit's d is the lower-case d (J + the centre bar); otherwise a plain Text.
  return (
    <SegLowerDText {...rest} seg={sc.style === 'seg'} cells={screenString(sc.style, children, { wrap })}
      accessibilityLabel={rest.accessibilityLabel ?? children} style={[flat, {
      fontFamily: sc.font,
      // ★ The cells keep §8.2's 10 pt floor (dot matrix and segments fall apart below it).
      fontSize: sc.face ? Math.max(10, size * sc.sizeK) : size,
      color: keepColor ? own : screenInk(sc, own),
      ...(lit ? { textShadowColor: sc.glow, textShadowRadius: 4, textShadowOffset: { width: 0, height: 0 } } : null),
    }, screenOneWeight(sc)]} />
  );
}
