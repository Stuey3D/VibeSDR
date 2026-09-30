/**
 * DecoderShell — the ONE frame every box over the waterfall renders inside (brief §10.1).
 *
 * ★★ WHY IT EXISTS: each panel carried its own copy of the frame, and the copies drifted.
 *   DecoderPanel's title was 10 pt at .65 while DAB's and RDS's were 11 pt at .86; its muted text
 *   was .38 where theirs was .60; DAB padded its header 10/7 where the others used 12/8; and
 *   DecoderPanel took `theme.font`, which would have leaked Nixie One into RTTY text. The majority
 *   values won (title 11 pt / .86, muted .60, padding 12 / 8) and live here, once.
 *
 * What it owns: the floating wrap, the frame (border, radius, shadow, tint, blur), the header row,
 * the title, the header keys, and the colour TOKENS the bodies use for labels and values.
 * What it does NOT own: the bodies. A DAB station list and a spots table share nothing but the
 * frame and the palette, and forcing them into one layout would be the drift in reverse.
 *
 * ★★★ FACEPLATES PLUG IN HERE, NOT IN THE PANELS. `useDecoderTokens()` is the only place a panel
 *   gets a colour from. Row 2 of the faceplates build resolves it from the faceplate context; row 8
 *   adds Transparent / Solid and the silver/black looks (dome header keys, recessed data window)
 *   by changing `decoderTokensFor()` and the frame below — no panel should need touching then.
 *   The default chassis values are the mockup's `isDef` branch (Decoder.mockup.dc.html), which is
 *   today's gold chrome.
 *
 * ★ MEANING COLOURS ARE NOT TOKENS OF THE LOOK. good / warn / bad (and MER pink, SNR green) never
 *   follow a colour setting: a red-controls user must still see good from bad (§10.2 TRAP).
 */

import React from 'react';
import {
  Animated, Platform, StyleSheet, Text, TouchableOpacity, View,
  type StyleProp, type TextStyle, type TouchableOpacityProps, type ViewStyle,
} from 'react-native';
import { BlurView } from 'expo-blur';
import { Fonts } from '../constants/theme';

/** ★ Pinned Atkinson Hyperlegible — see Fonts.decoder. Never the display style. */
export const DECODER_FONT = Fonts.decoder;

export interface DecoderTokens {
  /** Frame border and the hairline under the header. */
  border:        string;
  hdrBdr:        string;
  /** The box's own tint, as an rgb triplet — the ALPHA is the panel's (see DecoderShell `tint`). */
  tintRgb:       string;
  /** Title (11 pt), and the title while minimised. */
  title:         string;
  titleMin:      string;
  /** Status text, field labels' dim cousin, plot captions. */
  muted:         string;
  /** Field labels and section heads. */
  label:         string;
  /** Values. */
  value:         string;
  /** Emphasis in the header (DAB's block name) and a selected row's text. */
  accent:        string;
  rowActive:     string;
  /** Header keys. */
  keyBorder:     string;
  keyBorderAct:  string;
  keyBgAct:      string;
  keyText:       string;
  keyTextAct:    string;
  close:         string;
  /** Status dot. */
  dotIdle:       string;
  /** Charts: constellation / impulse-response backing, axes, points, bars. */
  plot:          string;
  axis:          string;
  bar:           string;
  // ── Meaning colours: never recoloured (§10.2) ──
  good:          string;
  warn:          string;
  bad:           string;
  dotOn:         string;
}

const GOLD = (a: number) => `rgba(255,160,0,${a})`;

/** The default chassis — today's gold chrome, the mockup's `isDef` branch. */
const DEFAULT_TOKENS: DecoderTokens = {
  border:       GOLD(0.28),
  hdrBdr:       GOLD(0.12),
  tintRgb:      '10,8,4',
  title:        GOLD(0.86),
  titleMin:     GOLD(0.40),
  muted:        GOLD(0.60),
  label:        GOLD(0.86),
  value:        '#ffe566',
  accent:       '#ffb833',
  rowActive:    GOLD(0.14),
  keyBorder:    GOLD(0.28),
  keyBorderAct: GOLD(0.55),
  keyBgAct:     GOLD(0.12),
  keyText:      GOLD(0.60),
  keyTextAct:   '#ffb833',
  close:        'rgba(255,100,100,0.70)',
  dotIdle:      GOLD(0.35),
  plot:         GOLD(0.05),
  axis:         GOLD(0.25),
  bar:          'rgba(255,184,51,0.75)',
  good:         '#7dff9a',
  warn:         '#ffd479',
  bad:          '#ff8a7d',
  dotOn:        '#55d98d',
};

/** ★ The resolver. One chassis today; row 8 adds silver/black and the Solid background. */
export function decoderTokensFor(_chassis: string = 'default'): DecoderTokens {
  return DEFAULT_TOKENS;
}

/** The tokens every decoder body reads. Components never hold their own palette. */
export function useDecoderTokens(): DecoderTokens {
  return decoderTokensFor('default');
}

// ── Frame ─────────────────────────────────────────────────────────────────────

export interface DecoderShellProps {
  /** Distance from the bottom of the screen (the deck's top + a gap). */
  bottom:      number;
  /** ★ The content's width, not a guess — each panel states why its number is what it is. */
  maxWidth:    number;
  maxHeight?:  number;
  /** Alpha of the box's tint over the waterfall. ★ Still per panel: DAB's is near-opaque for a
   *  measured reason and RDS's is glass for another (see their notes). Row 8's Transparent / Solid
   *  setting is what finally unifies it — until then the shell carries each panel's own. */
  tint:        number;
  /** iOS backdrop blur intensity, 0 for none. ★ Never over the spectrum (the expensive case). */
  blur?:       number;
  /** Replaces the border colour — the keyboard-focus ring. */
  borderColor?: string;
  /** Extra style on the outer wrap — DecoderPanel animates its opacity and slide here. */
  wrapStyle?:  any;
  onTouchStart?: () => void;
  children:    React.ReactNode;
}

export function DecoderShell({ bottom, maxWidth, maxHeight, tint, blur = 0, borderColor,
  wrapStyle, onTouchStart, children }: DecoderShellProps) {
  const tk = useDecoderTokens();
  return (
    /* ★ box-none: the wrap spans the screen's width so it can centre the box; without this its
       empty sides ate taps meant for the waterfall on anything wider than the box. */
    <Animated.View style={[sh.wrap, { bottom }, wrapStyle]} pointerEvents="box-none">
      <View style={[sh.inner, { maxWidth, borderColor: borderColor ?? tk.border },
                    maxHeight != null && { maxHeight }]}
            onTouchStart={onTouchStart}>
        {Platform.OS === 'ios' && blur > 0 && (
          <BlurView intensity={blur} tint="dark" style={StyleSheet.absoluteFill} />
        )}
        <View style={[StyleSheet.absoluteFill, { backgroundColor: `rgba(${tk.tintRgb},${tint})` }]}
              pointerEvents="none" />
        {children}
      </View>
    </Animated.View>
  );
}

// ── Header ────────────────────────────────────────────────────────────────────

/** The header row. Pressable when `onPress` is given (DecoderPanel's tap-to-minimise). */
export function DecoderHeader({ onPress, style, children }: {
  onPress?: () => void; style?: StyleProp<ViewStyle>; children: React.ReactNode;
}) {
  const tk = useDecoderTokens();
  const st = [sh.header, { borderBottomColor: tk.hdrBdr }, style];
  if (onPress) {
    return <TouchableOpacity style={st} onPress={onPress} activeOpacity={0.85}>{children}</TouchableOpacity>;
  }
  return <View style={st}>{children}</View>;
}

export function DecoderTitle({ minimised = false, style, children }: {
  minimised?: boolean; style?: StyleProp<TextStyle>; children: React.ReactNode;
}) {
  const tk = useDecoderTokens();
  return (
    <Text style={[sh.title, { color: minimised ? tk.titleMin : tk.title }, style]} numberOfLines={1}>
      {children}
    </Text>
  );
}

export type DecoderKeyTone = 'normal' | 'accent' | 'close';

/** Style of a header key's box. `active` = a latched state (BIG, a filter set, the pane shown). */
export function decoderKeyStyle(tk: DecoderTokens, active = false): ViewStyle {
  return active
    ? { ...sh.key, borderColor: tk.keyBorderAct, backgroundColor: tk.keyBgAct }
    : { ...sh.key, borderColor: tk.keyBorder };
}

/** Style of a header key's legend. `accent` lights the legend without latching the key (SAVE). */
export function decoderKeyTextStyle(tk: DecoderTokens, active = false,
                                    tone: DecoderKeyTone = 'normal'): TextStyle {
  const color = tone === 'close' ? tk.close
              : (active || tone === 'accent') ? tk.keyTextAct : tk.keyText;
  return { ...sh.keyTxt, color };
}

/**
 * A header key. ★ Row 8 turns this into a dome key (§5, `useDomeKey()`) on every chassis, which is
 * why every header key in every panel must come through here rather than a local TouchableOpacity.
 */
export const DecoderKey = React.forwardRef<any, TouchableOpacityProps & {
  label?: string; active?: boolean; tone?: DecoderKeyTone; textStyle?: StyleProp<TextStyle>;
}>(function DecoderKey({ label, active = false, tone = 'normal', style, textStyle, children, ...rest }, ref) {
  const tk = useDecoderTokens();
  return (
    <TouchableOpacity ref={ref} style={[decoderKeyStyle(tk, active), style]} {...rest}>
      {label != null
        ? <Text style={[decoderKeyTextStyle(tk, active, tone), textStyle]}>{label}</Text>
        : children}
    </TouchableOpacity>
  );
});

const sh = StyleSheet.create({
  wrap:  { position: 'absolute', left: 8, right: 8, zIndex: 200, alignItems: 'center' },
  inner: {
    width: '100%',
    borderWidth: 1, borderRadius: 14,
    shadowColor: '#000', shadowOffset: { width: 0, height: 4 },
    shadowOpacity: 0.80, shadowRadius: 14, elevation: 16,
    overflow: 'hidden',
  },
  header: {
    flexDirection: 'row', alignItems: 'center', gap: 6,
    paddingHorizontal: 12, paddingVertical: 8,
    borderBottomWidth: StyleSheet.hairlineWidth,
  },
  title:  { fontSize: 11, letterSpacing: 2, fontFamily: DECODER_FONT, flexShrink: 0 },
  key:    { borderWidth: 1, borderRadius: 4, paddingHorizontal: 8, paddingVertical: 3 },
  keyTxt: { fontFamily: DECODER_FONT, fontSize: 11 },
});
