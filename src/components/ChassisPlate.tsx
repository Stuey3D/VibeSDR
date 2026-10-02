/**
 * ChassisPlate — the silver / black deck plate, the black gloss display panel and the recessed
 * windows (faceplates brief §3.2–§3.4). Numbers are Deck.mockup's `T.silver` / `T.black`.
 *
 * ★★ OPAQUE. The default deck is glass (BlurView + tint) and blurs the waterfall behind it — on iOS
 *   the expensive case. A metal plate hides what is behind it, so on silver and black there is NO
 *   BlurView at all (§3.4); ControlsBar draws this instead.
 * ★★ DRAWN ONCE. Texture, lighting, lips, screws and border are one static Skia canvas that redraws
 *   only when its SIZE changes (a rotation). Nothing that moves — meter, digits, key presses — is in
 *   it: those are separate views above it, so a meter update never touches the plate.
 * ★ §3.4 TRAP: the lighting is its own layer, never baked into the texture, so it stays right in
 *   landscape and on tablets; and the grain is sampled linear + mipmapped, or it moirés when the
 *   1200 px image is drawn at 620 pt.
 */

import React, { useCallback, useState } from 'react';
import { StyleSheet, View, type LayoutChangeEvent, type StyleProp, type ViewStyle } from 'react-native';
import {
  Canvas, Circle, Group, ImageShader, Line, LinearGradient, RadialGradient, Rect, RoundedRect,
  Skia, vec,
} from '@shopify/react-native-skia';
import { useTexture, TEXTURE_SAMPLING } from './DomeKey';
import type { PlateTokens } from '../constants/faceplate';

/** CSS `linear-gradient(<deg>, …)` → Skia start/end points over a w × h box. */
export function cssAngle(deg: number, w: number, h: number) {
  const a = (deg * Math.PI) / 180;
  const dx = Math.sin(a), dy = -Math.cos(a);
  const len = Math.abs(w * dx) + Math.abs(h * dy);
  const cx = w / 2, cy = h / 2;
  return { start: vec(cx - (dx * len) / 2, cy - (dy * len) / 2), end: vec(cx + (dx * len) / 2, cy + (dy * len) / 2) };
}

function useSize() {
  const [sz, setSz] = useState({ w: 0, h: 0 });
  const onLayout = useCallback((e: LayoutChangeEvent) => {
    const w = Math.round(e.nativeEvent.layout.width), h = Math.round(e.nativeEvent.layout.height);
    setSz(p => (p.w === w && p.h === h ? p : { w, h }));
  }, []);
  return [sz, onLayout] as const;
}

/** One corner screw (§3.2): 9 pt, radial #fff → #a7abb0 60% → #6d7176, its slot at its own angle. */
function Screw({ x, y, angle }: { x: number; y: number; angle: number }) {
  const r = 4.5, cx = x + r, cy = y + r;
  const a = (angle * Math.PI) / 180, l = r - 1;
  return (
    <Group>
      <Circle cx={cx} cy={cy + 1} r={r} color="rgba(255,255,255,0.8)" />
      <Circle cx={cx} cy={cy} r={r}>
        <RadialGradient c={vec(x + 9 * 0.35, y + 9 * 0.30)} r={9 * 0.8}
          colors={['#ffffff', '#a7abb0', '#6d7176']} positions={[0, 0.6, 1]} />
      </Circle>
      <Circle cx={cx} cy={cy} r={r - 0.25} color="rgba(0,0,0,0.6)" style="stroke" strokeWidth={0.5} />
      <Line p1={vec(cx - Math.cos(a) * l, cy - Math.sin(a) * l)} p2={vec(cx + Math.cos(a) * l, cy + Math.sin(a) * l)}
        color="rgba(0,0,0,0.55)" strokeWidth={1} />
    </Group>
  );
}

const PlateCanvas = React.memo(function PlateCanvas({ w, h, r, plate }: {
  w: number; h: number; r: number; plate: PlateTokens;
}) {
  const img = useTexture(plate.texture);
  const clip = Skia.RRectXY(Skia.XYWHRect(0, 0, w, h), r, r);
  const light = cssAngle(104, w, h);
  // `.tex-*`: the grain shown at 620 pt wide, centred. ★ Mirrored rather than repeated past its
  // edges (a landscape deck is wider than 620 pt), so no seam line crosses the plate.
  const k = 620 / 1200;
  // `radial-gradient(140% 70% at 28% -10%)`: an ellipse, drawn as a circle squashed vertically.
  const rx = 1.4 * w, ry = 0.7 * h, ecx = 0.28 * w, ecy = -0.10 * h;
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
        {/* ── The lighting layer (separate from the grain, §3.4) ── */}
        <Rect x={0} y={0} width={w} height={h}>
          <LinearGradient start={light.start} end={light.end} colors={plate.lightColors} positions={plate.lightPos} />
        </Rect>
        <Group transform={[{ translateX: ecx }, { translateY: ecy }, { scaleY: ry / rx }, { translateX: -ecx }, { translateY: -ecy }]}>
          <Circle cx={ecx} cy={ecy} r={rx}>
            <RadialGradient c={vec(ecx, ecy)} r={rx} colors={[plate.radialColor, 'rgba(255,255,255,0)']} positions={[0, 0.6]} />
          </Circle>
        </Group>
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
        {plate.screws && (<>
          <Screw x={8} y={8} angle={35} />
          <Screw x={w - 17} y={8} angle={-20} />
          <Screw x={8} y={h - 17} angle={80} />
          <Screw x={w - 17} y={h - 17} angle={10} />
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
  return (
    <View style={StyleSheet.absoluteFill} onLayout={onLayout} pointerEvents="none">
      {/* ★★★ KEYED BY SIZE: a resize builds a FRESH canvas. On a Mac a sheet laid out narrow and then grew,
          and the canvas kept drawing into its first surface — the brushed plate covered the left half of
          the tuning-step sheet until it was closed and reopened, sometimes several times (Stuart, B16).
          A size change is rare (open, rotate, window resize), so a remount costs nothing that matters. */}
      {w > 0 && h > 0 && <PlateCanvas key={`${w}x${h}`} w={w} h={h} r={radius} plate={plate} />}
    </View>
  );
}

const GlossCanvas = React.memo(function GlossCanvas({ w, h, r, trim, squareBottom }: {
  w: number; h: number; r: number; trim: boolean; squareBottom: boolean;
}) {
  const reflect = cssAngle(112, w, h);
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
        {/* The hard diagonal reflection, 38–56 %, α .10 → .035. */}
        <Rect x={0} y={0} width={w} height={h}>
          <LinearGradient start={reflect.start} end={reflect.end}
            colors={['rgba(255,255,255,0)', 'rgba(255,255,255,0)', 'rgba(255,255,255,0.10)', 'rgba(255,255,255,0.035)', 'rgba(255,255,255,0)']}
            positions={[0, 0.38, 0.385, 0.56, 0.565]} />
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
  return (
    <View style={[StyleSheet.absoluteFill, style]} onLayout={onLayout} pointerEvents="none">
      {/* ★ Keyed by size for the same reason as ChassisPlate's canvas. */}
      {w > 0 && h > 0 && <GlossCanvas key={`${w}x${h}`} w={w} h={h} r={radius} trim={trim} squareBottom={squareBottom} />}
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
