/**
 * statusLogos.ts — the status row's LOGOS as VCR ELECTRODES (2026-10-06), for StatusField: the server mark, the
 * connection bars and the ✕, the gain arrow, the recording dot, the lightning bolt, the admin key, the listener
 * figure, and the DSP badges' frames. Each is cut into the electrodes a real annunciator would have — separate
 * shapes, or vfdCut's breaks — and SegField ghosts, lights and meshes them with the cells (vfdMesh.ts), the RDS
 * mark's treatment. Paths only (no React): built once per shape × place × size.
 */

import { PathOp, Skia, StrokeJoin, type SkPath } from '@shopify/react-native-skia';
import { vfdCut } from './vfdMesh';
import { SEG14_PITCH } from '../constants/modeBox';
import { statusSegWidth, STATUS_LOGO_CELLS, STATUS_SEG_ADV, type StatusLogo } from '../constants/statusField';

/** An electrode: its outline (pt) and whether it is lit (SegField's SegExtra). */
export interface LogoElectrode { path: SkPath; lit: boolean }
type SegExtra = LogoElectrode;

/** Cell i's pen x (SegField segCellX). */
const segCellX = (i: number, fs: number) => i * SEG14_PITCH * fs;

// ── VCR logo electrodes (pt; the cell's pen at x0, the cell 0 … fs tall) ─────

function line(x1: number, y1: number, x2: number, y2: number, w: number): SkPath {
  const l = Skia.Path.Make();
  l.moveTo(x1, y1); l.lineTo(x2, y2);
  return l.stroke({ width: w }) ?? l;
}
function poly(pts: ReadonlyArray<readonly [number, number]>, w: number | null): SkPath {
  const p = Skia.Path.Make();
  pts.forEach(([x, y], i) => (i ? p.lineTo(x, y) : p.moveTo(x, y)));
  if (w == null) { p.close(); return p; }
  return p.stroke({ width: w, join: StrokeJoin.Miter }) ?? p;
}
function ring(cx: number, cy: number, r: number, w: number): SkPath {
  const c = Skia.Path.Make();
  c.addCircle(cx, cy, r);
  return c.stroke({ width: w }) ?? c;
}
function disc(cx: number, cy: number, r: number): SkPath {
  const c = Skia.Path.Make();
  c.addCircle(cx, cy, r);
  return c;
}
export function rect(x: number, y: number, w: number, h: number, r = 0): SkPath {
  const p = Skia.Path.Make();
  if (r > 0) p.addRRect(Skia.RRectXY(Skia.XYWHRect(x, y, w, h), r, r)); else p.addRect(Skia.XYWHRect(x, y, w, h));
  return p;
}

/** Which of a logo's electrodes (segLogo's order) are lit — apart from the shapes, which do not change with it. */
export function segLogoLit(logo: StatusLogo): boolean[] {
  switch (logo.kind) {
    case 'node': return [true, true, true, true, true, true];
    case 'bars': return [0, 1, 2].map(i => i < logo.q);
    case 'arrow': return [logo.dir != null, logo.dir === 'up', logo.dir === 'down'];
    case 'key': case 'person': return [true, true];
    default: return [true];
  }
}

/** The electrodes of one logo at cell `at` (their lit flags are segLogoLit's). A bars / arrow logo is drawn the same
 *  whatever its q / dir. */
export function segLogo(logo: StatusLogo, at: number, fs: number): SegExtra[] {
  const x0 = segCellX(at, fs);
  const u = (v: number) => v * fs;
  const cx = x0 + u(STATUS_SEG_ADV / 2);
  switch (logo.kind) {
    case 'node': {
      // SectionIcon 'instance' (24-unit box: rings r 2 at (6,7) (18,7) (12,18), 1.7 strokes), fitted to the cell
      // height and centred in its two cells; the links stop short of the rings — the breaks between electrodes.
      const k = fs / 17.5, boxW = statusSegWidth(STATUS_LOGO_CELLS.node, fs);
      const ox = x0 + (boxW - 17.7 * k) / 2 - 3.15 * k, oy = (fs - 16.7 * k) / 2 - 4.15 * k;
      const P = (x: number, y: number) => [ox + x * k, oy + y * k] as const;
      const nodes = [[6, 7], [18, 7], [12, 18]] as const;
      const out: SegExtra[] = nodes.map(([x, y]) => ({ path: ring(...P(x, y), 2 * k, 1.7 * k), lit: true }));
      const gap = 2.85 + 1.0;
      for (const [a, b] of [[0, 1], [0, 2], [1, 2]] as const) {
        const [ax, ay] = nodes[a], [bx, by] = nodes[b];
        const len = Math.hypot(bx - ax, by - ay), dx = (bx - ax) / len, dy = (by - ay) / len;
        out.push({ path: line(...P(ax + dx * gap, ay + dy * gap), ...P(bx - dx * gap, by - dy * gap), 1.7 * k), lit: true });
      }
      return out;
    }
    case 'bars':
      return [0.4, 0.7, 1.0].map((h, i) => ({ path: rect(x0 + u(0.06 + 0.27 * i), u(1 - h), u(0.18), u(h)), lit: i < logo.q }));
    case 'cross': {
      const w = u(0.13), l = x0 + u(0.14), r = x0 + u(0.68), t = u(0.18), b = u(0.82);
      const sx = Skia.Path.MakeFromOp(line(l, t, r, b, w), line(r, t, l, b, w), PathOp.Union) ?? line(l, t, r, b, w);
      return [{ path: vfdCut(sx, [[l - u(0.1), u(0.5), r + u(0.1), u(0.5), u(0.06)]]), lit: true }];
    }
    case 'arrow': {
      const w = u(0.11), dx = u(0.24);
      return [
        { path: rect(cx - u(0.055), u(0.3), u(0.11), u(0.4)), lit: logo.dir != null },
        { path: poly([[cx - dx, u(0.36)], [cx, u(0.1)], [cx + dx, u(0.36)]], w), lit: logo.dir === 'up' },
        { path: poly([[cx - dx, u(0.64)], [cx, u(0.9)], [cx + dx, u(0.64)]], w), lit: logo.dir === 'down' },
      ];
    }
    case 'rec':
      return [{ path: disc(cx, u(0.5), u(0.3)), lit: true }];
    case 'bolt': {
      const pts = ([[0.50, 0], [0.70, 0], [0.48, 0.40], [0.70, 0.40], [0.25, 1], [0.38, 0.55], [0.14, 0.55]] as const)
        .map(([x, y]) => [x0 + u(x), u(y)] as const);
      return [{ path: vfdCut(poly(pts, null), [[x0, u(0.475), x0 + u(STATUS_SEG_ADV), u(0.475), u(0.05)]]), lit: true }];
    }
    case 'key': {
      const p = ring(cx, u(0.22), u(0.15), u(0.08));
      const shaft = rect(cx - u(0.04), u(0.44), u(0.08), u(0.56));
      shaft.addPath(rect(cx + u(0.04), u(0.70), u(0.16), u(0.08)));
      shaft.addPath(rect(cx + u(0.04), u(0.86), u(0.16), u(0.08)));
      return [{ path: p, lit: true }, { path: shaft, lit: true }];
    }
    case 'person':
      return [{ path: disc(cx, u(0.22), u(0.17)), lit: true },
              { path: rect(x0 + u(0.12), u(0.48), u(0.58), u(0.52), u(0.2)), lit: true }];
  }
}

/** The DSP tag frame's room each side of the cells (pt) — inside SegField's 4 pt canvas margin. */
export const framePad = (fs: number) => Math.min(2.5, fs * 0.25);
