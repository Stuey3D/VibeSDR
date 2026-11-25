/**
 * ★★ V12 beta91 TELEMETRY CHARTS (Stuart 2026-11-22) — FUNcube telemetry as small multiples: one calm chart per channel,
 * THIS PASS (the readings against time) and, under it, the LAST passes the gallery holds (a point per pass — its mean —
 * with its min–max shaded). The geometry and the words are src/utils/tlmCharts.ts (the web draws the same).
 *
 * ★ One hue (the faceplate's line colour), the text in the faceplate's text inks — a single series needs no legend; the
 *   chart's name says what it is, the reading and its range say the numbers (units on them: "7.88 V", "37.1 °C"; beta94
 *   a channel with no published conversion says "raw", and the note names it).
 * ★ The power system first, TLM_FIRST charts, then "+ N MORE CHANNELS" — no control anywhere else (the control deck
 *   stays minimal); this sits inside the SATELLITE box and the gallery's pass item only.
 */
import React, { useMemo, useState } from 'react';
import { Pressable, StyleSheet, Text, View, type LayoutChangeEvent } from 'react-native';
import Svg, { Circle, Line, Path } from 'react-native-svg';
import { tlmAbsentText, tlmAcross, tlmAcrossHead, tlmChartOrder, tlmFootNote, tlmMinSpan, tlmPassHead, tlmRangeText, tlmSpark,
         tlmValueText, TLM_FIRST, type TlmChannel, type TlmPass } from '../utils/tlmCharts';

export interface TlmChartsInk { text: string; muted: string; line: string; grid: string; font?: string }

const CH_H = 34;

function Cell({ ch, t, v, w, ink, band }: {
  ch: TlmChannel; t: number[]; v: (number | null)[]; w: number; ink: TlmChartsInk;
  band?: { lo: (number | null)[]; hi: (number | null)[] };
}) {
  const sp = useMemo(() => tlmSpark(t, v, w, CH_H, 3, band ? Infinity : undefined, band?.lo, band?.hi, tlmMinSpan(ch)), [t, v, w, band, ch]);
  const range = sp.n ? tlmRangeText(band?.lo ?? v, ch, band?.hi) : '';
  const s = styles;
  const f = ink.font ? { fontFamily: ink.font } : null;
  return (
    <View style={{ width: w }} accessible
          accessibilityLabel={`${ch.name}${ch.raw ? ', raw' : ''}: ${sp.n} ${band ? 'passes' : 'readings'}${range ? `, ${range}` : ''}, latest ${tlmValueText(sp.lastV, ch)}`}>
      <Text style={[s.name, f, { color: ink.text }]} numberOfLines={1}>{ch.name}</Text>
      <Svg width={w} height={CH_H}>
        <Line x1={0} y1={CH_H - 0.5} x2={w} y2={CH_H - 0.5} stroke={ink.grid} strokeWidth={1} />
        {!!sp.band && <Path d={sp.band} fill={ink.line} fillOpacity={0.16} stroke="none" />}
        {!!sp.line && <Path d={sp.line} stroke={ink.line} strokeWidth={1.5} fill="none" strokeLinejoin="round" strokeLinecap="round" />}
        {!!sp.last && <Circle cx={sp.last.x} cy={sp.last.y} r={2.5} fill={ink.line} />}
      </Svg>
      <View style={s.top}>
        <Text style={[s.val, f, { color: ink.text }]} numberOfLines={1}>{tlmValueText(sp.lastV, ch)}</Text>
        <Text style={[s.foot, f, { color: ink.muted }]} numberOfLines={1}>{range}</Text>
      </View>
    </View>
  );
}

export default function TlmCharts({ pass, live, across, acrossNote, ink }: {
  /** this pass (the live frames, or the gallery item's CSV); null = none yet */
  pass: TlmPass | null;
  live: boolean;
  /** the last passes, oldest first; null = not loaded (acrossNote says why) */
  across: TlmPass[] | null;
  acrossNote?: string;
  ink: TlmChartsInk;
}) {
  const [w, setW] = useState(0);
  const [all, setAll] = useState(false);
  const ref = pass ?? (across && across.length ? across[across.length - 1] : null);
  const order = useMemo(() => tlmChartOrder(ref), [ref]);
  const shown = all ? order : order.slice(0, TLM_FIRST);
  const cols = w >= 480 ? 3 : w >= 280 ? 2 : 1;
  const gap = 12;
  const cw = cols > 0 && w > 0 ? Math.floor((w - gap * (cols - 1)) / cols) : 0;
  const f = ink.font ? { fontFamily: ink.font } : null;
  const onLayout = (e: LayoutChangeEvent) => { const x = Math.round(e.nativeEvent.layout.width); if (Math.abs(x - w) > 1) setW(x); };
  const acrossData = useMemo(() => (across ?? []).length
    ? shown.map((i) => ({ ch: ref!.channels[i], a: tlmAcross(across!, ref!.channels[i].name) })) : [], [across, shown, ref]);
  return (
    <View onLayout={onLayout} style={styles.wrap}>
      <Text style={[styles.head, f, { color: ink.text }]}>{tlmPassHead(pass, live)}</Text>
      {/* ★★ beta94 a board whose data-valid flag is off: said, never charted as zeros */}
      {live && !!tlmAbsentText(pass?.absent) && <Text style={[styles.note, f, { color: ink.muted, marginTop: 0 }]}>{tlmAbsentText(pass?.absent)}</Text>}
      {cw > 0 && !!pass && pass.t.length > 0 && (
        <View style={[styles.grid, { columnGap: gap }]}>
          {shown.map((i) => <Cell key={`p${i}`} ch={pass.channels[i]} t={pass.t} v={pass.v[i]} w={cw} ink={ink} />)}
        </View>
      )}
      {order.length > TLM_FIRST && (
        <Pressable onPress={() => setAll((x) => !x)} accessibilityRole="button" accessibilityState={{ expanded: all }} style={styles.more}>
          <Text style={[styles.moreText, f, { color: ink.text }]}>{all ? '− FEWER CHANNELS' : `+ ${order.length - TLM_FIRST} MORE CHANNELS`}</Text>
        </Pressable>
      )}
      <Text style={[styles.head, f, { color: ink.text, marginTop: 8 }]}>{across == null ? (acrossNote || 'LAST PASSES · loading from the gallery…') : tlmAcrossHead(across.length, across)}</Text>
      {cw > 0 && acrossData.length > 0 && (
        <View style={[styles.grid, { columnGap: gap }]}>
          {acrossData.map(({ ch, a }, k) => (
            <Cell key={`a${k}`} ch={ch} t={a.t} v={a.mean} band={{ lo: a.min, hi: a.max }} w={cw} ink={ink} />
          ))}
        </View>
      )}
      <Text style={[styles.note, f, { color: ink.muted }]}>{tlmFootNote(ref)}</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  wrap:  { gap: 4, marginTop: 4 },
  head:  { fontSize: 11, letterSpacing: 1 },
  grid:  { flexDirection: 'row', flexWrap: 'wrap', rowGap: 8 },
  top:   { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'baseline', gap: 6, marginTop: 1 },
  name:  { fontSize: 10 },
  val:   { fontSize: 11, fontVariant: ['tabular-nums'] },
  foot:  { fontSize: 9, fontVariant: ['tabular-nums'], flexShrink: 1 },
  more:  { paddingVertical: 6, minHeight: 32, justifyContent: 'center' },
  moreText: { fontSize: 11, letterSpacing: 1 },
  note:  { fontSize: 9, marginTop: 4 },
});
