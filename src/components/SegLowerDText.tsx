/**
 * SegLowerDText — a line of PLAIN DSEG14 text whose dB units keep a LOWER-CASE d (★ 2026-10-06).
 *
 * Stuart: "make sure that in the next build any dB icons on the VCR VFD display are correctly set with the lower case
 * d." DSEG14 has no lower case (its 'd' is its 'D'), so toSegCells() marks a dB unit's d as 'd' and this draws it as
 * the classic VFD d, b c d e g, out of two of DSEG14's own glyphs (displayText "THE LOWER-CASE d OF dB"):
 *   • the line itself, with each d drawn as 'J' (b c d e)            — in flow, exactly as the Text it replaces;
 *   • the SAME line again over it, every character TRANSPARENT except a '-' (the centre bar, g) where each d is.
 * The same characters in the same font and style lay out to the same pens, so the bar lands in the d's cell whatever
 * the letter-spacing, the decimal points, the wrap (screenString's break points) or adjustsFontSizeToFit does.
 * ★ The overlay is not a second line for assistive tech: it is hidden from it and from touches.
 * ★ A string with no d — every non-VCR display, and nearly every VCR line — is the plain Text, nothing added.
 * ★ The transparent runs carry no shadow: Android draws a shadow layer under transparent text (the "shadow only"
 *   trick), which would double the glow of every other cell.
 */
import React from 'react';
import { StyleSheet, Text, View, type StyleProp, type TextProps, type TextStyle, type ViewStyle } from 'react-native';
import { segBarRuns, segHasLowerD, segLitText } from '../constants/displayText';

/** A layer that draws nothing itself (the overlay's own style; its bars set their colour). */
export const SEG_CLEAR: TextStyle = { color: 'transparent', textShadowColor: 'transparent', textShadowRadius: 0 };
const CLEAR = SEG_CLEAR;

/** The overlay's content: the line, transparent, with the d's bars lit in the line's own colour and glow. */
export function SegBarSpans({ cells, style }: { cells: string; style: StyleProp<TextStyle> }) {
  const flat = StyleSheet.flatten(style) ?? {};
  const bar: TextStyle = { color: flat.color, textShadowColor: flat.textShadowColor, textShadowRadius: flat.textShadowRadius,
                           textShadowOffset: flat.textShadowOffset };
  return (
    <>
      {segBarRuns(cells).map((r, i) => (
        <Text key={i} style={r.bar ? bar : CLEAR}>{r.text}</Text>
      ))}
    </>
  );
}

export default function SegLowerDText({ seg, cells, style, wrapStyle, ...rest }: Omit<TextProps, 'children'> & {
  /** The line is a 14-segment string. ★ Only then is a lower-case 'd' the dB mark: on any other display the text is
   *  as written, and its d's are just d's. */
  seg: boolean;
  /** The text as drawn: screenString(style, …) — toSegCells() cells when `seg`. */
  cells: string;
  /** Layout for the wrapper that holds the two layers (only when there is a d to draw). */
  wrapStyle?: StyleProp<ViewStyle>;
}) {
  if (!seg || !segHasLowerD(cells)) return <Text {...rest} style={style}>{cells}</Text>;
  // ★ The overlay takes only what shapes the line — never a handler or a second onLayout.
  const { numberOfLines, adjustsFontSizeToFit, minimumFontScale, ellipsizeMode, allowFontScaling, maxFontSizeMultiplier } = rest;
  const shape = { numberOfLines, adjustsFontSizeToFit, minimumFontScale, ellipsizeMode, allowFontScaling, maxFontSizeMultiplier };
  return (
    <View style={wrapStyle}>
      <Text {...rest} style={style}>{segLitText(cells)}</Text>
      <Text {...shape} style={[style, CLEAR, StyleSheet.absoluteFill]} accessible={false}
            importantForAccessibility="no-hide-descendants" accessibilityElementsHidden pointerEvents="none">
        <SegBarSpans cells={cells} style={style} />
      </Text>
    </View>
  );
}
