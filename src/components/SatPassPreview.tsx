/* SatPassPreview.tsx — ★★★ beta97 PASS PREVIEW in the app's SATELLITE box (Stuart 11-27). The rules and words are
 * src/services/satPreview.ts (one set with the web page, web/client/src/satPreviewWeb.ts); this draws them with
 * react-native-svg.
 *
 *  The pass plays on the half-globe (the SAME scene as the live box's HalfGlobe — globeScene) and on a small polar sky
 *  plot over this station's reception history: the bird along the server's own track AOS → LOS in ~10 s, its footprint,
 *  the trail coloured by where this station usually decodes (green / amber / grey), the clock (UTC · local), elevation,
 *  azimuth, range, the Doppler at the row's downlink, AOS / MAX / LOS, the verdict. Stops at LOS (▶ replays), ❚❚ pauses,
 *  a tap on the bar scrubs, 0.5× / 1× / 2×; ✕ (or the row's ▶ again, or back) returns to the live view.
 *
 *  ★★ PERF: frames only while it plays (requestAnimationFrame, ~30 a second, stopped when paused / ended / unmounted);
 *     the parts that never move (dome, land, dotted orbit, chevrons, the sky plot's heat map and route) are memoised
 *     components that do not re-render per frame — each frame re-renders only the footprint, the bird, the trail, the
 *     sky plot's dot (a transform) and the words. */
import React, { useEffect, useMemo, useRef, useState } from 'react';
import { Pressable, StyleSheet, Text, View } from 'react-native';
import Svg, { Circle, G, Path, Text as SvgText } from 'react-native-svg';
import { DecoderKey, DECODER_FONT, useDecoderStyles, type DecoderTokens } from './DecoderShell';
import { GLOBE_ARROW_PX, GLOBE_GLYPH, GLOBE_RX_DOT, globeArrowPath, globeFrame, globeGlyph, globeLabels, globeLandInk, globeLayers, globeScene } from '../services/satellites';
import {
  passHistorySource, passMomentsText, passReadout, passSampleAt, passSkyPaths, passStripText, passTrailRuns, passVerdict, PassPlayer,
  PASS_GRADE_CSS, PASS_KEY_TEXT, PASS_PREVIEW_STOP, PASS_SPEEDS, type PassGrade, type PassPreviewRow, type PassTrack, type SkyHistory,
} from '../services/satPreview';
import { skyPlotScene, skyPlotXY } from '../utils/skyPlot';

const GW = 640, GH = 300, SKY = 150;
const f1 = (n: number) => n.toFixed(1);

export interface SatPassPreviewProps {
  row: PassPreviewRow;
  /** the pass from the server's track (undefined = still coming; null = none to play) */
  pt: PassTrack | null | undefined;
  hist: { kind: string; h: SkyHistory | null };
  rx: { lat: number; lon: number };
  rxName?: string;
  ink: string; beam: string; ground: string;
  /** Date.now() ms when the bird may start moving (after the box scrolled it into view — the beat so the start is seen) */
  startAt: number;
  onStop: () => void;
  /** the globe's width in points (the box's) */
  width?: number;
}

/** The parts of the globe that never move in a preview — memoised: not re-rendered per frame. */
const StaticGlobe = React.memo(function StaticGlobe({ pt, rx, ink, beam, ground, sc, small }: {
  pt: PassTrack; rx: { lat: number; lon: number }; ink: string; beam: string; ground: string; sc: number; small: boolean;
}) {
  const g = useMemo(() => globeScene(GW, GH, rx, null, pt.geo, pt.tcaIdx), [pt, rx.lat, rx.lon]);   // eslint-disable-line react-hooks/exhaustive-deps
  const ly = globeLayers(small);
  const dome = `M${f1(g.cx - g.r)} ${f1(g.cy)}A${f1(g.r)} ${f1(g.r)} 0 0 1 ${f1(g.cx + g.r)} ${f1(g.cy)}Z`;
  const arrowD = ly.arrows && g.arrows.length ? globeArrowPath(g.arrows, GLOBE_ARROW_PX * sc) : '';
  return (
    <G>
      <Path d={dome} fill={ink} fillOpacity={0.12} stroke={ink} strokeOpacity={0.55} strokeWidth={2} />
      {!!g.grid && <Path d={g.grid} fill="none" stroke={ink} strokeOpacity={0.12} strokeWidth={1} />}
      {!!g.coast && <Path d={g.coast} fill="none" stroke={globeLandInk(ground)} strokeOpacity={0.7} strokeWidth={1} />}
      {!!g.orbit && <Path d={g.orbit} fill="none" stroke={beam} strokeOpacity={0.85} strokeWidth={1.5 * sc} strokeLinecap="round"
                          strokeDasharray={`${f1(1.5 * sc)} ${f1(4 * sc)}`} />}
      {ly.tick && !!g.tcaTick && <Path d={g.tcaTick} fill="none" stroke={beam} strokeOpacity={0.85} strokeWidth={1.5 * sc} strokeLinecap="round" />}
      {!!arrowD && <>
        <Path d={arrowD} fill="none" stroke={ground} strokeOpacity={0.8} strokeWidth={3.6 * sc} strokeLinecap="round" strokeLinejoin="round" />
        <Path d={arrowD} fill="none" stroke={beam} strokeWidth={1.6 * sc} strokeLinecap="round" strokeLinejoin="round" />
      </>}
    </G>
  );
});

/** The sky plot's parts that never move: the history's heat map (faint), rings, the route coloured by history, AOS / MAX / LOS. */
const StaticSky = React.memo(function StaticSky({ pt, h, ink }: { pt: PassTrack; h: SkyHistory | null; ink: string }) {
  const sc = useMemo(() => skyPlotScene([], SKY, 0, h ? { cells: h.cells, overlay: [] } : null), [h]);
  const sp = useMemo(() => passSkyPaths(pt, h, sc.cx, sc.cy, sc.R), [pt, h, sc]);
  const mark = (p: [number, number], t: string, anchor: 'start' | 'middle' | 'end', dy: number) => (
    <G key={t}>
      <Circle cx={p[0]} cy={p[1]} r={2.6} fill={ink} />
      <SvgText x={p[0]} y={p[1] + dy} fontSize={9} textAnchor={anchor} fill={ink} fontFamily={DECODER_FONT}>{t}</SvgText>
    </G>
  );
  return (
    <G>
      <Circle cx={sc.cx} cy={sc.cy} r={sc.R} fill={ink} fillOpacity={0.07} />
      {sc.cells.length > 0 && <G opacity={0.38}>{sc.cells.map((c, i) => <Path key={i} d={c.d} fill={c.fill} stroke={c.fill} strokeWidth={0.6} />)}</G>}
      <Path d={sc.cross} stroke={ink} strokeOpacity={0.18} fill="none" />
      {sc.rings.map((r) => <Circle key={r.el} cx={sc.cx} cy={sc.cy} r={r.r} fill="none" stroke={ink} strokeOpacity={r.el === 0 ? 0.6 : 0.28} />)}
      {sp.runs.map((r, i) => <Path key={i} d={r.d} fill="none" strokeLinecap="round" strokeLinejoin="round" {...gradeStroke(r.grade, ink, 2.6)} />)}
      {mark(sp.aos, 'AOS', sp.aos[0] < sc.cx ? 'start' : 'end', sp.aos[1] < sc.cy ? 10 : -4)}
      {mark(sp.los, 'LOS', sp.los[0] < sc.cx ? 'start' : 'end', sp.los[1] < sc.cy ? 10 : -4)}
      {mark(sp.max, `MAX ${pt.maxEl.toFixed(0)}°`, 'middle', -5)}
      {sc.labels.filter((l) => !/°$/.test(l.text)).map((l) => (
        <SvgText key={l.text} x={l.x} y={l.y + 3} fontSize={10} textAnchor="middle" fill={ink} fillOpacity={0.8} fontFamily={DECODER_FONT}>{l.text}</SvgText>
      ))}
    </G>
  );
});

function gradeStroke(g: PassGrade, ink: string, w: number) {
  return g === 'none'
    ? { stroke: ink, strokeOpacity: 0.55, strokeWidth: w * 0.7, strokeDasharray: '2 3' }
    : { stroke: PASS_GRADE_CSS[g], strokeWidth: w };
}

export default function SatPassPreview(p: SatPassPreviewProps) {
  const s = useDecoderStyles(makeStyles);
  const pt = p.pt;
  const playerRef = useRef<PassPlayer | null>(null);
  const [, setFrame] = useState(0);
  const [speed, setSpeed] = useState<number>(1);
  const [barW, setBarW] = useState(0);
  const [gw, setGw] = useState(p.width ?? 300);
  const raf = useRef(0);
  const last = useRef(0);
  const lastPaint = useRef(0);
  const bump = () => setFrame((n) => (n + 1) % 1e9);
  // a new pass → a new player (paused until startAt)
  if (pt && (!playerRef.current || playerRef.current.aos !== pt.aos || playerRef.current.los !== pt.los)) {
    playerRef.current = new PassPlayer(pt.aos, pt.los);
    playerRef.current.playing = false;
    playerRef.current.setSpeed(speed);
  }
  const player = pt ? playerRef.current : null;

  const stopRaf = () => { if (raf.current) cancelAnimationFrame(raf.current); raf.current = 0; };
  const go = () => {
    if (raf.current || !playerRef.current) return;
    last.current = Date.now();
    const step = () => {
      raf.current = 0;
      const pl = playerRef.current;
      if (!pl) return;
      const now = Date.now();
      const more = pl.advance(now - last.current);
      last.current = now;
      // ★ ~30 renders a second: the bird moves smoothly, the bridge is not flooded
      if (!more || now - lastPaint.current >= 33) { lastPaint.current = now; bump(); }
      if (more) raf.current = requestAnimationFrame(step);
    };
    raf.current = requestAnimationFrame(step);
  };
  // start after the beat (the box has scrolled the preview into view)
  useEffect(() => {
    if (!pt) return;
    const t = setTimeout(() => { const pl = playerRef.current; if (pl && !pl.ended) { pl.playing = true; bump(); go(); } }, Math.max(0, p.startAt - Date.now()));
    return () => { clearTimeout(t); stopRaf(); };
  }, [pt, p.startAt]);   // eslint-disable-line react-hooks/exhaustive-deps
  useEffect(() => () => stopRaf(), []);

  const small = gw < 200;
  const sc = GW / Math.max(60, gw);
  const v = useMemo(() => (pt ? passVerdict(pt, p.hist.h) : null), [pt, p.hist.h]);

  if (pt === undefined || pt === null || !player) {
    return (
      <View style={s.box} testID="satPassPreview">
        <Head s={s} row={p.row} onStop={p.onStop} />
        <Text style={s.detail}>{pt === null ? 'This server sent no track for this pass (it is not above this receiver\'s horizon, or the orbit data has gone stale)' : 'Fetching this pass from the server…'}</Text>
      </View>
    );
  }
  const t = player.t;
  const smp = passSampleAt(pt, t);
  const g = globeScene(GW, GH, p.rx, { lat: smp.lat, lon: smp.lon, altKm: smp.altKm }, pt.geo, pt.tcaIdx, true);
  const ly = globeLayers(small);
  const fr = globeFrame(GW, GH, p.rx);
  const runs = passTrailRuns(pt, p.hist.h, t).map((r) => {
    let d = '', pen = false;
    for (const q of r.pts) { const v2 = fr.lifted(q.lat, q.lon, q.altKm); if (!v2.vis) { pen = false; continue; } d += `${pen ? 'L' : 'M'}${f1(v2.x)} ${f1(v2.y)}`; pen = true; }
    return { grade: r.grade, d };
  }).filter((r) => r.d);
  const birdInk = globeLandInk(p.ground);
  const glyph = ly.glyph && g.bird && g.sat ? globeGlyph(g.sat, g.bird.u, sc) : null;
  const font = 10 * sc;
  const L = ly.labels ? globeLabels(g, p.rxName, p.row.name, font) : { rx: null, sat: null };
  const skyS = skyPlotScene([], SKY, 0, null);
  const [dx, dy] = skyPlotXY(smp.az, smp.el, skyS.cx, skyS.cy, skyS.R);
  const ro = passReadout(pt, t, p.row.hz);
  const playing = player.playing;
  const toggle = () => { player.toggle(); bump(); if (player.playing) go(); else stopRaf(); };
  const seekTo = (x: number) => { if (barW <= 0) return; player.seek(x / barW); player.playing = false; stopRaf(); bump(); };

  return (
    <View style={s.box} testID="satPassPreview">
      <Head s={s} row={p.row} onStop={p.onStop} />
      <View style={s.pics}>
        <View style={s.globe} onLayout={(e) => { const w = e.nativeEvent.layout.width; if (w > 0 && Math.abs(w - gw) > 1) setGw(w); }}>
          <Svg width="100%" height="100%" viewBox={`0 0 ${GW} ${GH}`} accessibilityLabel="The pass played out on the globe: the satellite along its track, its footprint, the trail coloured by where this station usually hears">
            <StaticGlobe pt={pt} rx={p.rx} ink={p.ink} beam={p.beam} ground={p.ground} sc={sc} small={small} />
            {!!g.foot && <Path d={g.foot} fill={p.beam} fillOpacity={0.28} />}
            {ly.beam && !!g.beam && <Path d={g.beam} fill={p.beam} fillOpacity={0.35} />}
            {runs.map((r, i) => (
              <G key={i}>
                <Path d={r.d} fill="none" stroke={p.ground} strokeOpacity={0.8} strokeWidth={5 * sc} strokeLinecap="round" strokeLinejoin="round" />
                <Path d={r.d} fill="none" stroke={r.grade === 'none' ? p.ink : PASS_GRADE_CSS[r.grade]} strokeOpacity={r.grade === 'none' ? 0.55 : 1}
                      strokeWidth={2.6 * sc} strokeLinecap="round" strokeLinejoin="round" />
              </G>
            ))}
            {!!g.rx && <Circle cx={g.rx.x} cy={g.rx.y} r={GLOBE_RX_DOT.r * sc} fill={GLOBE_RX_DOT.fill} stroke={GLOBE_RX_DOT.ring} strokeWidth={GLOBE_RX_DOT.ringW * sc} />}
            {!!glyph && <>
              <Path d={glyph.panels} fill="none" stroke={birdInk} strokeWidth={GLOBE_GLYPH.line * sc} strokeLinecap="butt" />
              <Path d={glyph.body} fill={birdInk} />
            </>}
            {ly.bird && !!g.bird && <>
              <Path d={g.bird.panels} fill={birdInk} fillOpacity={0.45} stroke={birdInk} strokeWidth={1.2 * sc} strokeLinejoin="round" />
              <Path d={g.bird.cells} fill="none" stroke={birdInk} strokeOpacity={0.9} strokeWidth={0.8 * sc} />
              <Path d={g.bird.boom} fill="none" stroke={birdInk} strokeWidth={1.6 * sc} strokeLinecap="round" />
              <Path d={g.bird.body} fill={birdInk} stroke={p.ground} strokeWidth={1 * sc} strokeLinejoin="round" />
              <Path d={g.bird.dish} fill={birdInk} />
            </>}
            {[L.rx, L.sat].map((lb, i) => lb && (
              <React.Fragment key={i}>
                <SvgText x={lb.x} y={lb.y} fontSize={font} fill={p.ground} stroke={p.ground} strokeWidth={3 * sc} strokeOpacity={0.9} textAnchor={lb.anchor} fontFamily={DECODER_FONT}>{lb.text}</SvgText>
                <SvgText x={lb.x} y={lb.y} fontSize={font} fill={p.ink} textAnchor={lb.anchor} fontFamily={DECODER_FONT}>{lb.text}</SvgText>
              </React.Fragment>
            ))}
          </Svg>
        </View>
        <View style={s.sky}>
          <Svg width="100%" height="100%" viewBox={`0 0 ${SKY} ${SKY}`} accessibilityLabel="The pass on the sky: north up, east right, the rim is the horizon">
            <StaticSky pt={pt} h={p.hist.h} ink={p.ink} />
            <G transform={`translate(${f1(dx)} ${f1(dy)})`} testID="satPassSkyDot">
              <Circle r={4.6} fill={p.ground} />
              <Circle r={3.4} fill={p.ink} />
            </G>
          </Svg>
        </View>
      </View>
      <Text style={s.clock} testID="satPassClock">{ro.clock}</Text>
      <View style={s.roRow}>
        <Text style={s.txt} testID="satPassLook">{ro.look}</Text>
        <Text style={s.txt}>{ro.range}</Text>
      </View>
      {!!ro.doppler && <Text style={s.txt}>{ro.doppler}</Text>}
      <View style={s.ctl}>
        <DecoderKey label={playing ? '❚❚' : '▶'} onPress={toggle} testID="satPassPlay"
                    accessibilityLabel={playing ? 'Pause' : player.ended ? 'Play the pass again' : 'Play'} />
        <Pressable style={s.bar} onLayout={(e) => setBarW(e.nativeEvent.layout.width)} onPress={(e) => seekTo(e.nativeEvent.locationX)}
                   accessibilityRole="adjustable" accessibilityLabel="Pass time — tap to move through the pass"
                   accessibilityValue={{ min: 0, max: 100, now: Math.round(player.frac() * 100) }}
                   onAccessibilityAction={(e) => { const d = e.nativeEvent.actionName === 'increment' ? 0.1 : -0.1; player.seek(player.frac() + d); bump(); }}
                   accessibilityActions={[{ name: 'increment' }, { name: 'decrement' }]}>
          <View style={s.track} />
          <View style={[s.fill, { width: `${(player.frac() * 100).toFixed(1)}%` as `${number}%` }]} />
          <Text style={[s.maxMark, { left: `${((pt.tca - pt.aos) / Math.max(1, pt.los - pt.aos) * 100).toFixed(1)}%` as `${number}%` }]}>MAX</Text>
        </Pressable>
        {PASS_SPEEDS.map((sp) => (
          <DecoderKey key={sp} label={`${sp}×`} active={speed === sp} onPress={() => { setSpeed(sp); player.setSpeed(sp); }} accessibilityLabel={`Play at ${sp} times`} />
        ))}
      </View>
      <Text style={s.detail}>{passMomentsText(pt)}</Text>
      {!!v && (
        <View style={s.vRow}>
          <View style={[s.vDot, v.grade !== 'none' && { backgroundColor: PASS_GRADE_CSS[v.grade] }]} />
          <Text style={[s.txt, s.vTxt]} testID="satPassVerdict">{v.text}</Text>
        </View>
      )}
      {!!p.hist.h && <Text style={s.detail}>{passHistorySource(p.hist.kind, p.hist.h)}</Text>}
      {!!p.hist.h && <Text style={s.detail}>{PASS_KEY_TEXT}</Text>}
    </View>
  );
}

function Head({ s, row, onStop }: { s: ReturnType<typeof makeStyles>; row: PassPreviewRow; onStop: () => void }) {
  return (
    <View style={s.head}>
      <Text style={s.strip} numberOfLines={1}>{passStripText(row.name, row.aos)}</Text>
      <DecoderKey label="✕" onPress={onStop} accessibilityLabel={PASS_PREVIEW_STOP} testID="satPassStop" />
    </View>
  );
}

/** ★ the row's ▶ (house style: a small filled triangle in the key's own colour) */
export function PassPlayGlyph({ color }: { color: string }) {
  return <Svg width={10} height={11} viewBox="0 0 10 11"><Path d="M1.5 1 L9 5.5 L1.5 10 Z" fill={color} /></Svg>;
}

const F = DECODER_FONT;
const makeStyles = (T: DecoderTokens) => StyleSheet.create({
  box:    { margin: 8, padding: 8, borderRadius: 8, borderWidth: StyleSheet.hairlineWidth, borderColor: T.muted, gap: 4 },
  head:   { flexDirection: 'row', alignItems: 'center', gap: 8 },
  strip:  { flex: 1, minWidth: 0, fontFamily: F, fontSize: 12, letterSpacing: 0.5, color: T.accent },
  pics:   { flexDirection: 'row', alignItems: 'center', gap: 8 },
  globe:  { flex: 1, minWidth: 0, maxWidth: 420, aspectRatio: GW / GH },
  sky:    { width: 118, aspectRatio: 1 },
  clock:  { fontFamily: F, fontSize: 14, color: T.value, fontVariant: ['tabular-nums'] },
  roRow:  { flexDirection: 'row', justifyContent: 'space-between', gap: 8 },
  txt:    { fontFamily: F, fontSize: 12, color: T.value, fontVariant: ['tabular-nums'] },
  detail: { fontFamily: F, fontSize: 11, color: T.muted },
  ctl:    { flexDirection: 'row', alignItems: 'center', gap: 6, marginTop: 4 },
  bar:    { flex: 1, minWidth: 60, height: 30, justifyContent: 'center' },
  track:  { height: 4, borderRadius: 2, backgroundColor: T.muted, opacity: 0.4 },
  fill:   { position: 'absolute', left: 0, height: 4, borderRadius: 2, backgroundColor: T.accent },
  maxMark:{ position: 'absolute', top: -2, fontFamily: F, fontSize: 9, color: T.accent, transform: [{ translateX: -10 }] },
  vRow:   { flexDirection: 'row', alignItems: 'center', gap: 6 },
  vDot:   { width: 9, height: 9, borderRadius: 5, backgroundColor: T.muted },
  vTxt:   { flex: 1 },
});
