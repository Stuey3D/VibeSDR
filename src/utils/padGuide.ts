/**
 * ★★★ THE CONTROLLER GUIDE — the drawings and the words, as DATA (Stuart approved the prototype 2026-10-19: "controller
 * guides look fine"; memory: gamepad_mapping). ONE copy for the web page (web/client/src/padGuide.ts draws it as SVG
 * markup) and the app (src/components/PadGuide.tsx draws it with react-native-svg), so the two can never describe the
 * pad differently. Tested by scripts/test_pad_guide.ts.
 *
 *  • Two layouts — TWO STICKS and BUTTONS — the two schemes gamepad.ts / padActions.ts implement.
 *  • Only the CONTROLLER is a drawing (it scales with the window); every word is page text beside it, numbered to a
 *    marker on the drawing, so it stays readable at any size. The leader lines are worked out from where things ARE on
 *    screen, by each client, every time the window changes — never fixed points (leaderPath).
 *  • Colours are ROLES, not values: each client maps them to its own theme (green = an action, amber = BACK, ink =
 *    SELECT / START).
 * ★ SELECT opens and closes the guide (and the menu has a "Controller guide" entry); BACK closes it; the D-pad left /
 *   right moves between the two tabs; it opens on the layout in use and follows a HOLD START switch while open.
 * ★★ WHEN A CONTROL MOVES, FIX THIS COPY (AGENTS.md). These words say where every pad control is — change the scheme in
 *    gamepad.ts / padActions.ts and this file in the same commit.
 * Pure: no imports, so Node runs the test straight from this file.
 */

export type PadLayout = 'sticks' | 'buttons';
export const PAD_LAYOUTS: PadLayout[] = ['sticks', 'buttons'];

/** What a colour MEANS in the drawing; each client maps these to its theme. */
export type ArtRole = 'body' | 'edge' | 'key' | 'keyEdge' | 'well' | 'action' | 'actionBg' | 'none';

export type ArtShape =
  | { k: 'rect'; x: number; y: number; w: number; h: number; rx: number; fill: ArtRole; stroke?: ArtRole; sw?: number;
      /** Rotation in degrees about (cx, cy). */ rot?: [number, number, number] }
  | { k: 'circle'; cx: number; cy: number; r: number; fill: ArtRole; stroke?: ArtRole; sw?: number }
  | { k: 'path'; d: string; fill: ArtRole; stroke?: ArtRole; sw?: number };

/** A label's colour: an action (green), BACK (amber), or SELECT / START (ink). */
export type ItemKind = 'action' | 'back' | 'meta';

export interface GuideItem {
  side: 'left' | 'right';
  /** Where the marker sits on the drawing, in viewBox units. */
  at: [number, number];
  t: string;
  s?: string;
  kind: ItemKind;
}

export interface Guide {
  tab: string;
  title: string;
  sub: string;
  foot: string;
  /** The drawing's viewBox: x, y, width, height. */
  view: [number, number, number, number];
  art: ArtShape[];
  items: GuideItem[];
  /** ★ In the CONTROLLER KEYBOARD (app only — the web page has no on-screen keyboard yet, so its guide skips these):
   *  what each control does while typing. Decided by Stuart 2026-10-20 (docs/v12/BRIEF-controller-keyboard.md). */
  keyboard: string[];
}

// ── The parts ────────────────────────────────────────────────────────────────

const dpad = (x: number, y: number): ArtShape[] => [
  { k: 'rect', x: x - 15, y: y - 46, w: 30, h: 92, rx: 5, fill: 'key', stroke: 'keyEdge', sw: 2 },
  { k: 'rect', x: x - 46, y: y - 15, w: 92, h: 30, rx: 5, fill: 'key', stroke: 'keyEdge', sw: 2 },
  { k: 'rect', x: x - 14, y: y - 14, w: 28, h: 28, rx: 0, fill: 'key' },
];
const face = (x: number, y: number, r = 23): ArtShape => ({ k: 'circle', cx: x, cy: y, r, fill: 'key', stroke: 'keyEdge', sw: 2 });
const pill = (x: number, y: number, rot = 0): ArtShape =>
  ({ k: 'rect', x: x - 21, y: y - 8, w: 42, h: 16, rx: 8, fill: 'key', stroke: 'keyEdge', sw: 2, rot: rot ? [rot, x, y] : undefined });

const f1 = (v: number) => v.toFixed(1);

/** An arrowhead at (x, y) pointing along angle `a` (radians) — the prototype's SVG marker, drawn as a shape so
 *  react-native-svg (whose markers are unreliable) draws it the same. 4.5 × the 5-unit stroke, its centre on the end. */
function arrowHead(x: number, y: number, a: number): ArtShape {
  const pts: [number, number][] = [[-11.25, -11.25], [11.25, 0], [-11.25, 11.25]];
  const c = Math.cos(a), s = Math.sin(a);
  const p = pts.map(([u, v]) => `${f1(x + u * c - v * s)},${f1(y + u * s + v * c)}`);
  return { k: 'path', d: `M${p[0]} L${p[1]} L${p[2]} Z`, fill: 'action' };
}

/** The round arrow: a dial turned either way (−160° round to 130°, arrowheads at both ends). */
function turn(cx: number, cy: number, r: number): ArtShape[] {
  const a0 = -160 * Math.PI / 180, a1 = 130 * Math.PI / 180;
  const x0 = cx + r * Math.cos(a0), y0 = cy + r * Math.sin(a0), x1 = cx + r * Math.cos(a1), y1 = cy + r * Math.sin(a1);
  return [
    { k: 'path', d: `M${f1(x0)},${f1(y0)} A${r},${r} 0 1 1 ${f1(x1)},${f1(y1)}`, fill: 'none', stroke: 'action', sw: 5 },
    // Clockwise on screen is a growing angle; the path's direction there is (−sin, cos) — the start's head points back.
    arrowHead(x1, y1, a1 + Math.PI / 2),
    arrowHead(x0, y0, a0 - Math.PI / 2),
  ];
}

const stick = (x: number, y: number): ArtShape[] => [
  { k: 'circle', cx: x, cy: y, r: 44, fill: 'well', stroke: 'keyEdge', sw: 2 },
  { k: 'circle', cx: x, cy: y, r: 31, fill: 'key', stroke: 'keyEdge', sw: 2 },
  { k: 'circle', cx: x, cy: y, r: 20, fill: 'none', stroke: 'edge', sw: 2 },
  ...turn(x, y, 64),
];

// ── The two guides ───────────────────────────────────────────────────────────

export const PAD_GUIDES: Record<PadLayout, Guide> = {
  sticks: {
    tab: 'TWO STICKS',
    title: 'TWO ANALOGUE STICKS', sub: 'PlayStation · Xbox · and other twin-stick pads',
    foot: 'Turn a stick round like a dial — a quarter turn is a few steps. Hold START to switch to the buttons layout.',
    view: [310, 240, 780, 480],
    art: [
      { k: 'path', d: 'M480 300 C560 272 840 272 920 300 C1000 322 1040 382 1060 470 C1090 600 1060 690 990 700 C940 705 900 660 870 612 C840 572 560 572 530 612 C500 660 460 705 410 700 C340 690 310 600 340 470 C360 382 400 322 480 300 Z', fill: 'body', stroke: 'edge', sw: 3 },
      { k: 'rect', x: 470, y: 262, w: 110, h: 24, rx: 12, fill: 'key', stroke: 'edge', sw: 2 },
      { k: 'rect', x: 820, y: 262, w: 110, h: 24, rx: 12, fill: 'key', stroke: 'edge', sw: 2 },
      ...dpad(495, 405), face(905, 355), face(950, 402), face(905, 449), face(860, 402),
      pill(650, 385), pill(750, 385), ...stick(600, 525), ...stick(800, 525),
    ],
    items: [
      { side: 'left',  at: [495, 405], t: 'D-PAD', s: 'move the highlight', kind: 'action' },
      { side: 'left',  at: [650, 385], t: 'SELECT', s: 'this guide', kind: 'meta' },
      { side: 'left',  at: [600, 525], t: 'LEFT STICK — TURN IT', s: 'clockwise tunes up · anticlockwise down', kind: 'action' },
      { side: 'right', at: [750, 385], t: 'HOLD START (1 s)', s: 'switch layout', kind: 'meta' },
      { side: 'right', at: [950, 402], t: 'RIGHT BUTTON — BACK', s: 'close · step out', kind: 'back' },
      { side: 'right', at: [905, 449], t: 'BOTTOM BUTTON — ENTER', s: 'press · select', kind: 'action' },
      { side: 'right', at: [800, 525], t: 'RIGHT STICK — TURN IT', s: 'clockwise zooms in · anticlockwise out', kind: 'action' },
    ],
    keyboard: [
      'TYPING (enter on a text box opens the on-screen keyboard):',
      'D-pad moves between keys · bottom button presses one · right button steps back a layer, then closes',
      'Left stick turned moves the cursor · left button ⌫ · top button space',
      'L shift · R symbols (on the number pad, L / R switch kHz and MHz) · tap START to finish',
      'The keyboard has ↵ and Back keys of its own · touch the screen to use the phone\'s keyboard instead',
    ],
  },
  buttons: {
    tab: 'BUTTONS',
    title: 'BUTTONS BY POSITION', sub: 'Super Nintendo style · handhelds · pads without sticks',
    foot: 'The four face buttons go by where they are, whatever is printed on them. Hold START to switch to the two-stick layout.',
    view: [360, 290, 680, 340],
    art: [
      { k: 'path', d: 'M520 330 L880 330 A140 140 0 0 1 880 610 L520 610 A140 140 0 0 1 520 330 Z', fill: 'body', stroke: 'edge', sw: 3 },
      { k: 'path', d: 'M430 352 Q470 318 540 314 L560 314 L560 326 L520 330 Q470 336 440 362 Z', fill: 'key', stroke: 'keyEdge', sw: 2 },
      { k: 'path', d: 'M970 352 Q930 318 860 314 L840 314 L840 326 L880 330 Q930 336 960 362 Z', fill: 'key', stroke: 'keyEdge', sw: 2 },
      ...dpad(520, 470), face(880, 418, 25), face(932, 470, 25), face(880, 522, 25), face(828, 470, 25),
      pill(662, 505, -25), pill(742, 505, -25),
    ],
    items: [
      { side: 'left',  at: [480, 330], t: 'L — BACK', s: 'close · step out', kind: 'back' },
      { side: 'left',  at: [520, 470], t: 'D-PAD', s: 'move the highlight', kind: 'action' },
      { side: 'left',  at: [662, 505], t: 'SELECT', s: 'this guide', kind: 'meta' },
      { side: 'left',  at: [742, 505], t: 'HOLD START (1 s)', s: 'switch layout', kind: 'meta' },
      { side: 'right', at: [920, 330], t: 'R — ENTER', s: 'press · select', kind: 'action' },
      { side: 'right', at: [880, 418], t: 'TOP — ZOOM IN', kind: 'action' },
      { side: 'right', at: [932, 470], t: 'RIGHT — TUNE UP', kind: 'action' },
      { side: 'right', at: [880, 522], t: 'BOTTOM — ZOOM OUT', kind: 'action' },
      { side: 'right', at: [828, 470], t: 'LEFT — TUNE DOWN', kind: 'action' },
    ],
    keyboard: [
      'TYPING (enter on a text box opens the on-screen keyboard):',
      'D-pad moves between keys · R presses one · L steps back a layer, then closes',
      'Left / right face buttons move the cursor · ⌫, space, ⇧ and #+= are keys on the keyboard',
      'Press ↵ (or tap START) to finish, Back to leave · touch the screen to use the phone\'s keyboard instead',
    ],
  },
};

// ── Layout rules both clients follow ─────────────────────────────────────────

/** ★ THE LAYOUT FOLLOWS THE WINDOW: three columns (labels | pad | labels, with leader lines) when there is room; the
 *  pad then the numbered list in two columns, no lines, when there is not; one column on a phone. Width in px / pt of
 *  the guide's own box (the prototype's 900 / 520 breakpoints). */
export function guideColumns(width: number): 3 | 2 | 1 {
  return width > 900 ? 3 : width > 520 ? 2 : 1;
}

/** A leader line: from the marker (x0, y0) diagonally to just short of the label's number, then level into it — the
 *  prototype's drawLines. (x1, y1) is the point beside the number, on the pad's side of it. */
export function leaderPath(x0: number, y0: number, x1: number, y1: number, side: 'left' | 'right'): string {
  const xm = side === 'left' ? Math.min(x1 + 24, x0) : Math.max(x1 - 24, x0);
  return `M${f1(x0)},${f1(y0)} L${f1(xm)},${f1(y1)} L${f1(x1)},${f1(y1)} `;
}

/** Where a viewBox point lands in a box of w × h drawn with preserveAspectRatio "xMidYMid meet". */
export function viewToBox(view: [number, number, number, number], w: number, h: number, x: number, y: number): [number, number] {
  const [vx, vy, vw, vh] = view;
  const sc = Math.min(w / vw, h / vh);
  return [(w - vw * sc) / 2 + (x - vx) * sc, (h - vh * sc) / 2 + (y - vy) * sc];
}

/** The D-pad left / right while the guide is open: to the other tab, stopping at the ends (there are two). */
export function guideTabStep(cur: PadLayout, dir: -1 | 1): PadLayout {
  const i = Math.max(0, Math.min(PAD_LAYOUTS.length - 1, PAD_LAYOUTS.indexOf(cur) + dir));
  return PAD_LAYOUTS[i];
}
