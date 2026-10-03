/**
 * ChassisPlate — the silver / black deck plate, the black gloss display panel and the recessed
 * windows (faceplates brief §3.2–§3.4). Numbers are Deck.mockup's `T.silver` / `T.black`.
 *
 * ★★ OPAQUE. The default deck is glass (BlurView + tint) and blurs the waterfall behind it — on iOS
 *   the expensive case. A metal plate hides what is behind it, so on silver and black there is NO
 *   BlurView at all (§3.4); ControlsBar draws this instead.
 * ★★ DRAWN ONCE. Texture, lighting, lips, screws and border are one Skia canvas that redraws only when
 *   its SIZE changes (a rotation) — or, for the light alone, when the light angle moves. Nothing that
 *   moves — meter, digits, key presses — is in it: those are separate views above it, so a meter update
 *   never touches the plate.
 * ★★★ THE LIGHT IS DERIVED, NOT A SEPARATE CANVAS (lighting brief §2, and the Mac GPU-memory audit — every
 *   <Canvas> is its own Metal layer): the sheen's points and the hot-spot are useDerivedValues of
 *   FaceplateContext's `lightSv`, so a moving angle repaints this canvas on the UI thread with NO React
 *   render; a still angle costs nothing. At 104° (LEFT) every value is today's.
 * ★ §3.4 TRAP: the lighting is its own layer, never baked into the texture, so it stays right in
 *   landscape and on tablets; and the grain is sampled linear + mipmapped, or it moirés when the
 *   1200 px image is drawn at 620 pt.
 */

import React, { useCallback, useEffect, useState } from 'react';
import { useDerivedValue, type SharedValue } from 'react-native-reanimated';
import { StyleSheet, View, type LayoutChangeEvent, type StyleProp, type ViewStyle } from 'react-native';
import {
  Canvas, Circle, Group, ImageShader, Line, LinearGradient, RadialGradient, Rect, RoundedRect,
  Skia, vec,
} from '@shopify/react-native-skia';
import { useTexture, TEXTURE_SAMPLING } from './DomeKey';
import type { PlateTokens } from '../constants/faceplate';
import { cssAnglePts, glossAngle, hotspotX, HOTSPOT_Y, screwHighlight } from '../constants/plateLight';
import { useLight } from '../contexts/FaceplateContext';

/** CSS `linear-gradient(<deg>, …)` → Skia start/end points over a w × h box. */
export function cssAngle(deg: number, w: number, h: number) {
  // ★ One formula: constants/plateLight.ts cssAnglePts (the worklet the plate's light derives from).
  const p = cssAnglePts(deg, w, h);
  return { start: vec(p.sx, p.sy), end: vec(p.ex, p.ey) };
}

/* ★★ A REMOUNT KEY THAT ONLY CHANGES ONCE A RESIZE HAS SETTLED. Keying the canvas by its live size fixed the stale
 *  surface (half a plate after a Mac window grew), but a window DRAG changes the size every frame, so the plate
 *  was torn down and rebuilt every frame — and the waterfall and spectrum paused while it happened (Stuart,
 *  2026-10-03: "resizing now slightly pauses the waterfall and spectrum"). The canvas now follows the size live
 *  without remounting, and is rebuilt ONCE, 250 ms after the size stops changing. */
function useSettledKey(w: number, h: number): string {
  const [key, setKey] = useState(`${w}x${h}`);
  useEffect(() => {
    const t = setTimeout(() => setKey(`${w}x${h}`), 250);
    return () => clearTimeout(t);
  }, [w, h]);
  return key;
}

function useSize() {
  const [sz, setSz] = useState({ w: 0, h: 0 });
  const onLayout = useCallback((e: LayoutChangeEvent) => {
    const w = Math.round(e.nativeEvent.layout.width), h = Math.round(e.nativeEvent.layout.height);
    setSz(p => (p.w === w && p.h === h ? p : { w, h }));
  }, []);
  return [sz, onLayout] as const;
}

/** One corner screw (§3.2): 9 pt, radial #fff → #a7abb0 60% → #6d7176, its slot at its own angle.
 *  ★ Its highlight sits TOWARD the light (`light` = the settled angle; 104 → today's 35 % 30 %). */
function Screw({ x, y, angle, light }: { x: number; y: number; angle: number; light: number }) {
  const r = 4.5, cx = x + r, cy = y + r;
  const a = (angle * Math.PI) / 180, l = r - 1;
  const hl = screwHighlight(light);
  return (
    <Group>
      <Circle cx={cx} cy={cy + 1} r={r} color="rgba(255,255,255,0.8)" />
      <Circle cx={cx} cy={cy} r={r}>
        <RadialGradient c={vec(x + 9 * hl.fx, y + 9 * hl.fy)} r={9 * 0.8}
          colors={['#ffffff', '#a7abb0', '#6d7176']} positions={[0, 0.6, 1]} />
      </Circle>
      <Circle cx={cx} cy={cy} r={r - 0.25} color="rgba(0,0,0,0.6)" style="stroke" strokeWidth={0.5} />
      <Line p1={vec(cx - Math.cos(a) * l, cy - Math.sin(a) * l)} p2={vec(cx + Math.cos(a) * l, cy + Math.sin(a) * l)}
        color="rgba(0,0,0,0.55)" strokeWidth={1} />
    </Group>
  );
}

/**
 * The plate: ONE canvas, painted in today's order — grain (base, texture, veil), then the LIGHT (sheen +
 * radial hot-spot), then the top (bottom shade, lips, screws, border).
 * ★★★ ONE CANVAS, NOT A LAYER PER STRATUM (Mac performance audit, 2026-10-02: 1 GB RAM, ~400 MB of it GPU in
 *   ~900 regions): every Skia <Canvas> is its own Metal layer with up to three full-size drawables, so the
 *   light stays INSIDE this canvas as a Group. Its gradient points and hot-spot are useDerivedValues of the
 *   live angle `sv`, so a moving angle still updates on the UI thread with no React render; a still angle
 *   costs nothing.
 */
const PlateCanvas = React.memo(function PlateCanvas({ w, h, r, plate, sv, light }: {
  w: number; h: number; r: number; plate: PlateTokens; sv: SharedValue<number>; light: number;
}) {
  const img = useTexture(plate.texture);
  const clip = Skia.RRectXY(Skia.XYWHRect(0, 0, w, h), r, r);
  // `.tex-*`: the grain shown at 620 pt wide, centred. ★ Mirrored rather than repeated past its
  // edges (a landscape deck is wider than 620 pt), so no seam line crosses the plate.
  const k = 620 / 1200;
  // ── The light, derived from the live angle (lighting brief §2). At 104°: cssAngle(104, w, h), 28 % −10 %. ──
  const start = useDerivedValue(() => { const p = cssAnglePts(sv.value, w, h); return { x: p.sx, y: p.sy }; }, [w, h]);
  const end   = useDerivedValue(() => { const p = cssAnglePts(sv.value, w, h); return { x: p.ex, y: p.ey }; }, [w, h]);
  // `radial-gradient(140% 70% at <x> -10%)`: an ellipse, drawn as a circle squashed vertically about its
  // centre. ★ The squash is in Y only, so the centre's x does not enter the transform — which is what lets
  // the centre MOVE without a re-render (today's translateX(ecx) … translateX(−ecx) cancelled anyway).
  const rx = 1.4 * w, ry = 0.7 * h, ecy = HOTSPOT_Y * h;
  const ecx = useDerivedValue(() => hotspotX(sv.value) * w, [w]);
  const c = useDerivedValue(() => ({ x: hotspotX(sv.value) * w, y: HOTSPOT_Y * h }), [w, h]);
  return (
    <Canvas style={{ width: w, height: h }} pointerEvents="none">
      <Group clip={clip}>
        <Rect x={0} y={0} width={w} height={h} color={plate.base} />
        {img && (
          <Rect x={0} y={0} width={w} height={h}>
            <ImageShader image={img} tx="mirror" ty="mirror" fit="none" sampling={TEXTURE_SAMPLING}
              transform={[{ translateX: w / 2 - 600 * k }, { translateY: h / 2 - 450 * k }, { scale: k }]} />
          </Rect>
        )}
        {/* The veil over the grain (black: a touch darker than the mockup — PlateTokens.textureDim). */}
        {plate.textureDim > 0 && <Rect x={0} y={0} width={w} height={h} color={`rgba(0,0,0,${plate.textureDim})`} />}
        {/* ── The lighting layer (separate from the grain, §3.4) — follows the ONE light angle ── */}
        <Rect x={0} y={0} width={w} height={h}>
          <LinearGradient start={start} end={end} colors={plate.lightColors} positions={plate.lightPos} />
        </Rect>
        <Group transform={[{ translateY: ecy }, { scaleY: ry / rx }, { translateY: -ecy }]}>
          <Circle cx={ecx} cy={ecy} r={rx}>
            <RadialGradient c={c} r={rx} colors={[plate.radialColor, 'rgba(255,255,255,0)']} positions={[0, 0.6]} />
          </Circle>
        </Group>
        {/* ★ The bottom shade is GRAVITY, not the lamp — never derived from the angle. */}
        {plate.bottomShade && (
          <Rect x={0} y={0} width={w} height={h}>
            <LinearGradient start={vec(0, 0)} end={vec(0, h)}
              colors={['rgba(0,0,0,0)', 'rgba(0,0,0,0)', plate.bottomShade]} positions={[0, 0.7, 1]} />
          </Rect>
        )}
        {/* The plate's edges: the bright top lip, the softer line under it, the bottom shadow. */}
        <Line p1={vec(0, 0.5)} p2={vec(w, 0.5)} color={plate.lipTop} strokeWidth={1} />
        <Line p1={vec(0, 2)} p2={vec(w, 2)} color={plate.lipTop2} strokeWidth={1.5} />
        <Line p1={vec(0, h - 1)} p2={vec(w, h - 1)} color={plate.lipBottom} strokeWidth={2} />
        {/* ★ Screws follow the SETTLED light (brief §5.1: small and many — the eye does not track them live). */}
        {plate.screws && (<>
          <Screw x={8} y={8} angle={35} light={light} />
          <Screw x={w - 17} y={8} angle={-20} light={light} />
          <Screw x={8} y={h - 17} angle={80} light={light} />
          <Screw x={w - 17} y={h - 17} angle={10} light={light} />
        </>)}
      </Group>
      <RoundedRect x={0.5} y={0.5} width={w - 1} height={h - 1} r={Math.max(0, r - 0.5)}
        color={plate.border} style="stroke" strokeWidth={1} />
    </Canvas>
  );
});

/** The plate, filling its parent. Mark the parent opaque; there is nothing to see through it. */
export default function ChassisPlate({ plate, radius }: { plate: PlateTokens; radius: number }) {
  const [{ w, h }, onLayout] = useSize();
  const settledKey = useSettledKey(w, h);
  const light = useLight();
  return (
    <View style={StyleSheet.absoluteFill} onLayout={onLayout} pointerEvents="none">
      {/* ★★★ KEYED BY SIZE: a resize builds a FRESH canvas. On a Mac a sheet laid out narrow and then grew,
          and the canvas kept drawing into its first surface — the brushed plate covered the left half of
          the tuning-step sheet until it was closed and reopened, sometimes several times (Stuart, B16).
          A size change is rare (open, rotate, window resize), so a remount costs nothing that matters. */}
      {w > 0 && h > 0 && <PlateCanvas key={settledKey} w={w} h={h} r={radius} plate={plate} sv={light.sv} light={light.deg} />}
    </View>
  );
}

const GlossCanvas = React.memo(function GlossCanvas({ w, h, r, trim, squareBottom, sv }: {
  w: number; h: number; r: number; trim: boolean; squareBottom: boolean; sv: SharedValue<number>;
}) {
  /* ★★ THE REFLECTION IS A FIXED DIAGONAL (today's 112°), NOT THE LIGHT ANGLE + 8°. Following the light turned it
   *  into a near-horizontal bar straight through the frequency digits under TOP, and even as a diagonal its hard
   *  bright edge was "a bit distracting" — the thing Stuart noticed most on the black skin (2026-10-03). A glossy
   *  panel's reflection is the ROOM, not the lamp; a fixed soft diagonal reads as acrylic without drawing the eye. */
  const p0 = cssAnglePts(glossAngle(104), w, h);
  const H = h + (trim ? 4 : 0);
  // Square bottom corners: round a box that runs r past the bottom, then only fill down to h.
  const shape = Skia.RRectXY(Skia.XYWHRect(0, 0, w, h + (squareBottom ? r : 0)), r, r);
  return (
    <Canvas style={{ width: w, height: H }} pointerEvents="none">
      {trim && (<>
        {/* The aluminium trim line beneath the acrylic (§3.3): 0 1 #8e9196, 0 2 #3b3d41, 0 3 3 shadow. */}
        <Rect x={0} y={h + 2} width={w} height={2} color="rgba(0,0,0,0.6)" />
        <Rect x={squareBottom ? 0 : r * 0.3} y={h + 1} width={w - (squareBottom ? 0 : r * 0.6)} height={1} color="#3b3d41" />
        <Rect x={squareBottom ? 0 : r * 0.3} y={h} width={w - (squareBottom ? 0 : r * 0.6)} height={1} color="#8e9196" />
      </>)}
      <Group clip={shape}>
        <Rect x={0} y={0} width={w} height={h}>
          <LinearGradient start={vec(0, 0)} end={vec(0, h)} colors={['#111214', '#060607', '#0b0b0d']} positions={[0, 0.55, 1]} />
        </Rect>
        {/* The diagonal reflection: soft-edged, α .045 → .018 (was a hard-edged .10 bar). */}
        <Rect x={0} y={0} width={w} height={h}>
          <LinearGradient start={vec(p0.sx, p0.sy)} end={vec(p0.ex, p0.ey)}
            colors={['rgba(255,255,255,0)', 'rgba(255,255,255,0)', 'rgba(255,255,255,0.045)', 'rgba(255,255,255,0.018)', 'rgba(255,255,255,0)']}
            positions={[0, 0.30, 0.42, 0.56, 0.70]} />
        </Rect>
        <Line p1={vec(0, 0.5)} p2={vec(w, 0.5)} color="rgba(255,255,255,0.28)" strokeWidth={1} />
      </Group>
    </Canvas>
  );
});

/**
 * Black's gloss acrylic display panel (§3.3) — a background for whatever it wraps. Portrait runs it
 * to the plate's top and side edges with square bottom corners (16 16 0 0); landscape wraps the
 * display column (10). The trim hangs below the panel's own box, so it never changes layout.
 */
export function GlossPanel({ style, radius, trim = true, squareBottom = false }: {
  style?: StyleProp<ViewStyle>; radius: number; trim?: boolean; squareBottom?: boolean;
}) {
  const [{ w, h }, onLayout] = useSize();
  const settledKey = useSettledKey(w, h);
  const { sv } = useLight();
  return (
    <View style={[StyleSheet.absoluteFill, style]} onLayout={onLayout} pointerEvents="none">
      {/* ★ Keyed by size for the same reason as ChassisPlate's canvas. */}
      {w > 0 && h > 0 && <GlossCanvas key={settledKey} w={w} h={h} r={radius} trim={trim} squareBottom={squareBottom} sv={sv} />}
    </View>
  );
}

/**
 * A recessed dark window in the plate (the status display, §8.1): #050505, an inner shadow at the
 * top, and the light lip below — light text needs a dark window on a silver plate (§10.3 TRAP, the
 * same rule the popups follow).
 * ★ This is only the window. ControlsBar's StatusWell fills it with the ghost grid and Doto in the
 *   text colour (row 4); row 9 adds the landscape drop order.
 */
export function RecessedWindow({ lip, style, children }: {
  lip: string; style?: StyleProp<ViewStyle>; children: React.ReactNode;
}) {
  return (
    <View style={[styles.window, style]}>
      <View pointerEvents="none" style={styles.windowShade} />
      <View pointerEvents="none" style={[styles.windowLip, { backgroundColor: lip }]} />
      {children}
    </View>
  );
}

const styles = StyleSheet.create({
  window:      { backgroundColor: '#050505', borderRadius: 5, paddingHorizontal: 10, paddingVertical: 4,
                 borderWidth: 1, borderColor: 'rgba(0,0,0,0.8)' },
  windowShade: { position: 'absolute', left: 0, right: 0, top: 0, height: 3, borderTopLeftRadius: 5,
                 borderTopRightRadius: 5, backgroundColor: 'rgba(0,0,0,0.6)' },
  windowLip:   { position: 'absolute', left: 4, right: 4, bottom: -2, height: 1 },
});
