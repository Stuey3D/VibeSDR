/**
 * ModeIcon — the small category glyph before a decoder / whole-radio mode's label (aircraft for ADS-B, a ship for
 * AIS…). The drawings live in utils/modeIcons.ts, shared with the web client; this only paints them with
 * react-native-svg in the label's own colour (SectionIcon's way).
 * ★ ModeLabel puts it before the text for the default skin's TouchableOpacity keys; silver / black pass
 *   `icon={c => <ModeIcon … color={c} />}` to PopupKey, which draws it in the legend colour.
 */
import React from 'react';
import { Text, View, type StyleProp, type TextStyle } from 'react-native';
import Svg, { Path, Circle, Rect } from 'react-native-svg';
import { MODE_ICONS, modeIconFor, type ModeIconName } from '../utils/modeIcons';

export default function ModeIcon({ name, size = 13, color }: { name: ModeIconName; size?: number; color: string }) {
  return (
    <Svg width={size} height={size} viewBox="0 0 24 24">
      {MODE_ICONS[name].map((p, i) => {
        const paint = p.fill
          ? { fill: color }
          : { fill: 'none', stroke: color, strokeWidth: p.w ?? 1.8, strokeLinecap: 'round' as const, strokeLinejoin: 'round' as const };
        if ('d' in p) return <Path key={i} d={p.d} {...paint} />;
        if ('c' in p) return <Circle key={i} cx={p.c[0]} cy={p.c[1]} r={p.c[2]} {...paint} />;
        return <Rect key={i} x={p.r[0]} y={p.r[1]} width={p.r[2]} height={p.r[3]} rx={p.r[4]} {...paint} />;
      })}
    </Svg>
  );
}

/** For PopupKey's `icon` prop: the id's glyph in the legend colour, or undefined when the id has none. */
export function modeKeyIcon(id: string, size = 13): ((c: string) => React.ReactNode) | undefined {
  const n = modeIconFor(id);
  return n ? (c: string) => <ModeIcon name={n} size={size} color={c} /> : undefined;
}

/**
 * A key's label with its glyph before it (the default skin's keys) — or ABOVE it (`stack`: the narrowest phones, where a
 * glyph beside NAVTEX would truncate it; the caller trims the key's vertical padding so the key keeps its height).
 * Falls back to the bare Text for an id with no glyph, so a caller can wrap every key alike. `color` must be the label's
 * own colour (lit or not).
 */
export function ModeLabel({ id, color, style, children, iconSize = 13, numberOfLines, stack = false }: {
  id: string; color: string; style: StyleProp<TextStyle>; children: React.ReactNode; iconSize?: number;
  numberOfLines?: number; stack?: boolean;
}) {
  const n = modeIconFor(id);
  if (!n) return <Text numberOfLines={numberOfLines} style={style}>{children}</Text>;
  return (
    <View style={{ flexDirection: stack ? 'column' : 'row', alignItems: 'center', justifyContent: 'center',
                   gap: stack ? 1 : 4, maxWidth: '100%' }}>
      <ModeIcon name={n} size={stack ? iconSize - 1 : iconSize} color={color} />
      <Text numberOfLines={numberOfLines ?? 1} style={[style, !stack && { flexShrink: 1 }]}>{children}</Text>
    </View>
  );
}

/** Icons go ABOVE the label below this window width — four keys a row no longer fit glyph + NAVTEX side by side. */
export const MODE_ICON_STACK_BELOW = 360;
