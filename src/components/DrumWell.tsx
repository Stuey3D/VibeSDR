/**
 * DrumWell — the parts of a drum well that DrumWheel and TunerKeys share (faceplates §6.1, §6.2):
 * the face, the drum's edge (its dark cut — no ring since 2026-10-01) and the LED pool. Skia components, drawn inside the
 * caller's <Canvas>. The rules and numbers are constants/drumWell.ts; the colours are the chassis
 * tokens and the controls colour.
 *
 * ★★ The well's face is ONE implementation so the keys well is the drum well with the drum swapped
 *   out (Stuart, 2026-09-30: the recess is what he likes) — two copies would drift apart the first
 *   time either was adjusted. Its RING went on 2026-10-01 (constants/drumWell.ts).
 * ★ Everything here is STATIC: it redraws only when the size, chassis or colour changes. The
 *   callers put it in canvases the drum's rolling notches never touch.
 */

import React from 'react';
import {
  Circle, Group, ImageShader, Line, LinearGradient, RadialGradient,
  Rect, RoundedRect, Skia, vec,
} from '@shopify/react-native-skia';
import { useTexture, TEXTURE_SAMPLING } from './DomeKey';
import { ledA, type ChassisTokens, type LedColour } from '../constants/faceplate';
import { POOL, WELL_R, poolEllipse } from '../constants/drumWell';

/**
 * The panel face. Default drum well: TODAY'S machined gradient, drawn exactly as DrumWheel always
 * drew it. Metal: the brushed grain over its base colour (Deck.mockup `.tex-silver` / `.tex-black`,
 * shown at 620 pt wide and centred on the well). `keys`: the tuner-keys face (`tk.bg`).
 */
export function WellFace({ W, H, ct, keys = false }: {
  W: number; H: number; ct: ChassisTokens; keys?: boolean;
}) {
  const img = useTexture(ct.wellTexture);
  if (!ct.wellTexture && !keys) {
    return (
      <RoundedRect x={0} y={0} width={W} height={H} r={WELL_R}>
        <LinearGradient start={vec(0, 0)} end={vec(0, H)} colors={ct.wellFace} positions={[0, 0.4, 1]} />
      </RoundedRect>
    );
  }
  const k = 620 / 1200;
  return (
    <Group clip={Skia.RRectXY(Skia.XYWHRect(0, 0, W, H), WELL_R, WELL_R)}>
      <Rect x={0} y={0} width={W} height={H} color={keys ? ct.keysFace : ct.wellFace[0]} />
      {img && ct.wellTexture && (
        <Rect x={0} y={0} width={W} height={H}>
          <ImageShader image={img} tx="mirror" ty="mirror" fit="none" sampling={TEXTURE_SAMPLING}
            transform={[{ translateX: W / 2 - 600 * k }, { translateY: H / 2 - 450 * k }, { scale: k }]} />
        </Rect>
      )}
      {/* The plate's veil over the grain, so a well's face stays the plate's shade (PlateTokens.textureDim). */}
      {img && ct.wellTexture && (ct.plate?.textureDim ?? 0) > 0 && (
        <Rect x={0} y={0} width={W} height={H} color={`rgba(0,0,0,${ct.plate!.textureDim})`} />
      )}
    </Group>
  );
}

/**
 * The DRUM well's edge: on metal, the dark 0.9 pt gap — the panel's cut-out the drum sits in — and the
 * face's top lip; on the default chassis, nothing. Drawn inside the well's own box.
 * ★★★ NO RING, NO GLOW ROUND THE WELL (Stuart, 2026-10-01) — see constants/drumWell.ts. The drum needs no
 *   panel-gap light of its own: DrumWheel already lights its seams from behind ("backlight seeping through
 *   the panel/wheel gaps"), and a light round the whole well box is the box he asked to be rid of
 *   (headless render 2026-10-01). The tuner-keys well draws no edge at all; its keys are lit one by one.
 */
export function WellEdge({ W, H, ct }: { W: number; H: number; ct: ChassisTokens }) {
  return (
    <Group>
      {ct.wellTopLip && (
        <Line p1={vec(WELL_R * 0.7, 1.4)} p2={vec(W - WELL_R * 0.7, 1.4)} color={ct.wellTopLip} strokeWidth={1} />
      )}
      {ct.wellBorder && (
        <RoundedRect x={0.5} y={0.5} width={W - 1} height={H - 1} r={WELL_R}
                     color={ct.wellBorder} strokeWidth={0.9} style="stroke" />
      )}
    </Group>
  );
}

/**
 * ★★ §6.1: THE LED GLOWING THROUGH FROM BEHIND THE DRUM — what replaces the red index needle on
 * every chassis. A radial pool in the controls colour over the drum body (x, y, w, h), clipped to
 * it. CSS's elliptical radial is a circle squashed about its centre.
 */
export function DrumPool({ x, y, w, h, led }: {
  x: number; y: number; w: number; h: number; led: LedColour;
}) {
  const e = poolEllipse(x, y, w, h);
  if (!(e.rx > 0) || !(e.ry > 0)) return null;
  return (
    <Group clip={Skia.XYWHRect(x, y, w, h)}>
      <Group origin={vec(e.cx, e.cy)} transform={[{ scaleY: e.ry / e.rx }]}>
        <Circle cx={e.cx} cy={e.cy} r={e.rx}>
          <RadialGradient c={vec(e.cx, e.cy)} r={e.rx}
            colors={[ledA(led, POOL.alphas[0]), ledA(led, POOL.alphas[1]), 'rgba(0,0,0,0)']}
            positions={POOL.positions} />
        </Circle>
      </Group>
    </Group>
  );
}
