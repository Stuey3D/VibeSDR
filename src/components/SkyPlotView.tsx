/**
 * SkyPlotView — ★★ V12 2026-11-24 THE SKY PLOT in the app (docs/v12/SKY-PLOT.md): the dome src/utils/skyPlot.ts lays out,
 * drawn with react-native-svg — the SATELLITE box's ORBCOMM panel and the gallery's SKY PLOT view both use it, and the web
 * page draws the same scene as an SVG string. The dome and its lines are in the box's own ink (every faceplate,
 * transparency on / off); the dots in the SNR ramp's colours (the server's PNG's), grey underneath.
 */
import React, { useMemo, useState } from 'react';
import { Pressable, StyleSheet, Text, View } from 'react-native';
import Svg, { Circle, Path, Text as SvgText } from 'react-native-svg';
import { SKY_LEGEND, SKY_MODES, skySceneFor, type SkyCells, type SkyDot, type SkyMode } from '../utils/skyPlot';

/** The drawing's own units (viewBox); on screen it is the width it is given, capped at maxW. */
const SIZE = 300;

function SkyPlotViewInner({ dots, ink, muted, font, maxW = 320, label = 'Sky plot', maxDraw = 3000, cells = null, mode = 'auto', onMode, nowSec = 0, norad = 0 }: {
  dots: SkyDot[]; ink: string; muted: string; font?: string; maxW?: number; label?: string;
  /** the newest this many dots are drawn (each is a native view: the live box keeps it lower — 1 GB phones) */
  maxDraw?: number;
  /** ★★★ beta94 the sky by position (skyplot.json "cells"; null = an older server: tracks only, no toggle) */
  cells?: SkyCells | null;
  /** AUTO / TRACKS / HEATMAP — the parent holds it (its summary line follows it too) */
  mode?: SkyMode; onMode?: (m: SkyMode) => void;
  /** the clock the live pass is judged by (a second's tick in the live box; 0 = nothing is live) and the picked bird */
  nowSec?: number; norad?: number;
}) {
  const scene = useMemo(() => skySceneFor(mode, cells, dots, nowSec, SIZE, maxDraw, norad), [dots, maxDraw, cells, mode, nowSec, norad]);
  const [w, setW] = useState(maxW);
  const fs = 11 * SIZE / Math.max(80, w);   // ★ labels sized on SCREEN, so they read at any width
  return (
    <View style={{ width: '100%', maxWidth: maxW, alignSelf: 'center' }}>
      <View style={{ width: '100%', aspectRatio: 1 }} onLayout={(e) => { const x = e.nativeEvent.layout.width; if (x > 0 && Math.abs(x - w) > 1) setW(x); }}>
        <Svg width="100%" height="100%" viewBox={`0 0 ${SIZE} ${SIZE}`} accessibilityLabel={label}>
          <Circle cx={scene.cx} cy={scene.cy} r={scene.R} fill={ink} fillOpacity={0.07} />
          {scene.cells.map((c) => <Path key={c.fill} d={c.d} fill={c.fill} stroke={c.fill} strokeWidth={0.6} strokeLinejoin="round" />)}
          <Path d={scene.cross} stroke={ink} strokeOpacity={0.18} strokeWidth={1} fill="none" />
          {scene.rings.map((r) => (
            <Circle key={r.el} cx={scene.cx} cy={scene.cy} r={r.r} fill="none" stroke={ink} strokeOpacity={r.el === 0 ? 0.6 : 0.28} strokeWidth={1} />
          ))}
          {scene.dots.map((d, i) => <Circle key={i} cx={d.x} cy={d.y} r={d.r} fill={d.fill} />)}
          {scene.labels.map((l) => {
            const small = /°$/.test(l.text);
            return (
              <SvgText key={l.text} x={l.x} y={l.y + (small ? 0 : fs * 0.35)} fontSize={small ? fs * 0.8 : fs} textAnchor={l.anchor}
                       fill={ink} fillOpacity={small ? 0.55 : 0.85} fontFamily={font}>{l.text}</SvgText>
            );
          })}
        </Svg>
      </View>
      {!!cells && !!onMode && (
        <View style={st.modes} accessibilityRole="radiogroup" accessibilityLabel="Sky plot view">
          {SKY_MODES.map((m) => (
            <Pressable key={m.id} onPress={() => onMode(m.id)} accessibilityRole="radio" accessibilityState={{ selected: mode === m.id }}
                       style={[st.mode, { borderColor: mode === m.id ? ink : muted, opacity: mode === m.id ? 1 : 0.7 }]}>
              <Text style={[st.kt, { color: mode === m.id ? ink : muted, fontFamily: font }]}>{m.label}</Text>
            </Pressable>
          ))}
        </View>
      )}
      <View style={st.legend} accessibilityLabel="Colour key: SNR from 0 dB, blue, to 18 dB or more, yellow; grey means tracked but nothing decoded">
        {SKY_LEGEND.map((k) => (
          <View key={k.label} style={st.key}>
            <View style={[st.sw, { backgroundColor: k.css }]} />
            <Text style={[st.kt, { color: muted, fontFamily: font }]}>{k.label}</Text>
          </View>
        ))}
      </View>
    </View>
  );
}
/** Redrawn only when the dots (a new array), the size or the colours change — never on the box's once-a-second tick. */
const SkyPlotView = React.memo(SkyPlotViewInner);
export default SkyPlotView;

const st = StyleSheet.create({
  legend: { flexDirection: 'row', flexWrap: 'wrap', justifyContent: 'center', columnGap: 10, rowGap: 2, marginTop: 4 },
  key:    { flexDirection: 'row', alignItems: 'center', gap: 4 },
  sw:     { width: 9, height: 9, borderRadius: 5 },
  kt:     { fontSize: 10, lineHeight: 14 },
  modes:  { flexDirection: 'row', justifyContent: 'center', gap: 6, marginTop: 4 },
  mode:   { borderWidth: 1, borderRadius: 10, paddingHorizontal: 8, paddingVertical: 2 },
});
