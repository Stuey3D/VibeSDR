/**
 * AdvRdsPanel — the Advanced RDS analyser, on the phone.
 *
 * ★★★ THIS COMPONENT CONTAINS NO DSP, AND MUST NOT GROW ANY. Every number, the constellation
 * and the MPX curve are computed by VibeServer beside the decoder, where the baseband is, and
 * arrive as one `rdsx` frame at ~5 Hz (see UberSDRClient.RdsExt). The browser client draws the
 * same frame. That is the whole reason the phone can show a broadcast-analyser panel at all —
 * and it means a decoder fix reaches every client at once. If you find yourself deriving a
 * value here, it belongs in rds.cpp instead.
 *
 * ★★ THE THRESHOLDS AND WORDINGS BELOW ARE MIRRORED FROM web/client/src/main.ts renderRds().
 * They are not arbitrary: each was set against HansVanEijsden's Pira analyser on real Dutch
 * stations (see the rds_pira_calibration note), and several are deliberately WIDE because a
 * verdict that flips on one degree of drift makes a steady measurement look unstable. Keep the
 * two in step — if you change a boundary here, change it there, or two of our own clients will
 * disagree about whether a transmitter is faulty.
 *
 * ★ Opening this panel is the switch: it calls setAdvRds(true), which is what makes the server
 * spend the extra CPU and bytes. Closing it must turn that back off.
 */

import { receiverIso } from '../services/rdsCountry';
import React, { useMemo, useRef } from 'react';
import { useBusValue, type ValueBus } from '../services/valueBus';
import { ScrollView, StyleSheet, Text, TouchableOpacity, useWindowDimensions, View } from 'react-native';
import type { LayoutChangeEvent } from 'react-native';
import { DecoderShell, DecoderHeader, DecoderTitle, DecoderKey, DecoderBody, DECODER_FONT,
         useDecoderStyles, type DecoderTokens } from './DecoderShell';
import { DECODER_MEANING } from '../constants/decoderTokens';
import { AlphaType, Canvas, ColorType, Image as SkiaImage, Path, Points, Rect, Skia,
         Text as SkText, matchFont } from '@shopify/react-native-skia';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import type { RdsExt } from '../services/UberSDRClient';
import StationLogo from './StationLogo';
import { mpxPowerParts } from '../services/mpxPower';
import { scrollLane } from '../constants/popupTokens';

/* ★★ THE PALETTE IS THE SHELL'S (DecoderShell, brief §10.1), and it is LIVE: every component reads
 *  `useDecoderStyles(makeStyles)`, so a chassis / colour / Decoder-background change reaches the
 *  text and the plots, not just the frame. Module-level code (the verdict functions, which pick a
 *  MEANING colour) reads the fixed meaning colours below — they never follow a setting (§10.2).
 *
 * ★★★ THE TINT WAS THIS PANEL'S, AND IS NOW EVERY PANEL'S TRANSPARENCY-ON GLASS (§10.2): 0.72 in SMALL over
 *  an iOS blur, 0.62 in BIG with none. Its history, which is why those are the numbers:
 *  ★ 0.95 was an opaque slab that blanked the waterfall behind it on BOTH platforms. Softened so
 *    the spectrum reads through; on iOS a BlurView sits underneath to keep the text legible
 *    against it, which is what the control island has always done.
 *  ★★ BIG IS MORE TRANSPARENT THAN SMALL, and only now can be. It carries no blur, so its tint is
 *    the ONLY thing between the reader and the spectrum. Stuart, once 49 fixed the stutter: "we
 *    could now make it slightly more transparent to allow for the signals to be seen behind it
 *    slightly better." Free: alpha blending does not resample what is underneath.
 *  ★★★ BIG IS ANDROID'S LOOK, DELIBERATELY — the same tint, no blur. THE POINT OF SEEING THROUGH IT
 *    IS TUNING — Stuart: "so that the user could still see a little spectrum underneath the window
 *    so that they can see to tune". An earlier pass raised BIG to 0.93 to protect legibility once
 *    the blur was gone, which traded away the one thing the transparency existed for. That trade
 *    is now the user's: Transparency OFF (opaque at 1.0 / metal) for reading, ON for tuning.
 *  ★★ LABELS LIFTED FROM 0.38 (2026-08-02: "with the waterfall behind they are a little hard to
 *    see"), and to .72 by the §10.2 contrast sweep — still clearly SUBORDINATE to the full-strength
 *    values, which is the hierarchy the dimming exists for. */
const C = DECODER_MEANING;
const FONT = DECODER_FONT;
const DASH = '—';

const PTY_EU = [
  'None', 'News', 'Current Affairs', 'Information', 'Sport', 'Education', 'Drama',
  'Culture', 'Science', 'Varied', 'Pop Music', 'Rock Music', 'Easy Listening',
  'Light Classical', 'Serious Classical', 'Other Music', 'Weather', 'Finance',
  "Children's", 'Social Affairs', 'Religion', 'Phone In', 'Travel', 'Leisure',
  'Jazz Music', 'Country Music', 'National Music', 'Oldies Music', 'Folk Music',
  'Documentary', 'Alarm Test', 'Alarm',
];

const LANGS: Record<number, string> = {
  1:'Albanian',2:'Breton',3:'Catalan',4:'Croatian',5:'Welsh',6:'Czech',7:'Danish',
  8:'German',9:'English',10:'Spanish',11:'Esperanto',12:'Estonian',13:'Basque',
  14:'Faroese',15:'French',16:'Frisian',17:'Irish',18:'Gaelic',19:'Galician',
  20:'Icelandic',21:'Italian',22:'Lappish',23:'Latin',24:'Latvian',25:'Luxembourgish',
  26:'Lithuanian',27:'Hungarian',28:'Maltese',29:'Dutch',30:'Norwegian',31:'Occitan',
  32:'Polish',33:'Portuguese',34:'Romanian',35:'Romansh',36:'Serbian',37:'Slovak',
  38:'Slovene',39:'Finnish',40:'Swedish',41:'Turkish',42:'Flemish',43:'Walloon',
};

/** ★ Seeing "RT+ in 12A" proves only that the ODA ANNOUNCEMENT decoded — not that the tags
 *  were then used. The Now-playing row is the evidence for the second half. */
const ODA_NAMES: Record<string, string> = {
  '4BD7': 'RT+', '6552': 'eRT', 'CD46': 'TMC', 'CD47': 'TMC', '0093': 'DAB x-ref',
  '4BD8': 'RT+ (group B)', 'C563': 'ID Logic', '6365': 'RDS2 station logo',
};

const COV = ['Local', 'International', 'National', 'Supra-regional',
             'Regional 1', 'Regional 2', 'Regional 3', 'Regional 4',
             'Regional 5', 'Regional 6', 'Regional 7', 'Regional 8',
             'Regional 9', 'Regional 10', 'Regional 11', 'Regional 12'];

export interface AdvRdsPanelProps {
  /** The analyser frame. Either directly, or — preferred — over a bus, so the five-a-second
   *  arrival re-renders this panel and not the whole screen (see services/valueBus.ts). */
  x?: RdsExt | null;
  bus?: ValueBus<RdsExt | null>;
  /** Basic RDS, which arrives on its own message and is shown by the VTS bar too. */
  ps?: string; rt?: string; pi?: string; ber?: number; countryIso?: string;
  /** ★ US call letters the server derived from the PI (a US receiver only) — shown as their own row. */
  call?: string;
  /** ★ The Extended Country Code, so the COUNTRY row can say how it knows. Without it the row
   *  hardcoded "· from PI", which is a claim about PROVENANCE and was false whenever the ECC had
   *  actually arrived — the web client has shown "GB · ECC E1" all along. ONE RULE, TWO READERS. */
  ecc?: number;
  /** ★★ Whether weak-signal processing is ON. The MPX S/N row's second clause describes what the
   *  receiver is DOING, so with WSP switched off "clean · no treatment" is not a measurement, it
   *  is the panel mistaking a disabled feature for a clean signal. */
  wsp?: boolean;
  /** ★★★ THE LOGO THE REST OF THE APP IS ALREADY SHOWING, resolved once by SDRScreen with the PI
   *  and the tuned FREQUENCY — which is what RadioDNS needs and what this panel does not have.
   *  Resolving again from the NAME alone gave a different, weaker answer: the identity path could
   *  never run here, so the panel fell back to a name search for a station whose own broadcaster
   *  publishes the artwork. One lookup, one cache entry, one answer. */
  logoUri?: string | null;
  /** ★ RAW is PER USER, PER SESSION. It changes only what this viewer is shown — the server
   *  always sends both, so it cannot affect anyone else on the same receiver. */
  raw: boolean;
  onRaw: (v: boolean) => void;
  /** Taller panel. Same control as the browser's, and equally just a height. */
  tall: boolean;
  onTall: (v: boolean) => void;
  bottomOffset: number;
  onClose: () => void;
}

/** One label/value row. `conf` drives the RAW-mode confirmation colouring: in RAW a label is
 *  red until the field has earned its confirmation, so the panel visibly resolves. */
/** ★★★ MEMOISED, AND IT IS NOT A MICRO-OPTIMISATION — IT IS THE FRAME RATE.
 *  The spectrum trace is tweened by a `setInterval` in WaterfallView (startSpecTween), which runs
 *  on the JS THREAD. Its comment says "UI-thread — no React render", which means it does not
 *  trigger a React render; the callback itself is JS-thread work. So the trace stays smooth only
 *  while the JS thread is free every tick.
 *  ★★ In BIG this panel is ~25 of these rows plus three Skia canvases, and WITHOUT memo every one
 *  of them re-renders on every rdsx — six times a second — even though most of these values change
 *  once a minute or never (PI, Station, Country, PI detail, Language, ODA). That is the burst of
 *  JS work that starves the tween, and it is why the spectrum jerks with a big panel open.
 *  ★ THE TELL THAT IT IS THE JS THREAD AND NOT COMPOSITING: it happens with the MAIN MENU open too,
 *  which does not cover the spectrum at all (Stuart, 2026-08-02) — and removing the panel's blur
 *  changed nothing. Overlap is not the variable; the size of the React tree being re-rendered is.
 *  ★ Props are all primitives, so the default shallow compare is exactly right here. */
/** ★★★ ONE SHARED EMPTY ARRAY. `xy={x?.xy ?? []}` built a FRESH literal on every render, so the
 *  three plot canvases below failed their React.memo compare EVEN WITH NO DATA AT ALL — a new
 *  reference every time is a changed prop. On a station with no constellation to draw, three Skia
 *  canvases were re-rendering six times a second to draw nothing. */
const NO_POINTS: number[] = [];

/** ★★★ THE PLOTS DO NOT NEED THE FULL rdsx RATE, AND PAYING IT IS WHAT COSTS.
 *  ★★★ The note by Row explains why this panel starves the JS thread: ~25 rows plus three Skia
 *      canvases re-rendering on every rdsx. The ROWS were fixed with React.memo — their props are
 *      primitives, so the shallow compare works. The CANVASES take ARRAYS, and a fresh array
 *      arrives with every message, so memo can never help them: all three redraw six times a
 *      second, each plotting hundreds of points.
 *  ★★ AND IT SHOWS UP AS SOMETHING ELSE ENTIRELY. Stuart hit it twice without either looking like
 *     a rendering cost: the SNR box needing several presses "only when the advanced RDS box is on
 *     screen", and audio stutter with the link meter dropping to yellow and red — which is the JS
 *     thread stalling, since that meter times frame arrivals AS OBSERVED BY JS.
 *  ★ 3 Hz is indistinguishable by eye on a scatter plot and halves the work. The TEXT rows keep
 *    the full rate: they are cheap (memoised primitives) and a laggy readout is a different
 *    annoyance. */
function useThrottledPoints(src: number[] | undefined, ms = 320): number[] {
  const [held, setHeld] = React.useState<number[]>(NO_POINTS);
  const lastRef = React.useRef(0);
  const pending = React.useRef<number[] | null>(null);
  pending.current = src && src.length ? src : null;
  React.useEffect(() => {
    const now = Date.now();
    const wait = Math.max(0, ms - (now - lastRef.current));
    const t = setTimeout(() => {
      lastRef.current = Date.now();
      setHeld(pending.current ?? NO_POINTS);
    }, wait);
    return () => clearTimeout(t);
  });
  return held;
}

const Row = React.memo(function Row({ label, value, colour, conf, raw, reserve }: {
  label: string; value: string; colour?: string; conf?: boolean; raw: boolean;
  /** ★ The LONGEST string this row can ever show. Rendered invisibly underneath to
   *  reserve the height, so the row cannot change size when the value does. */
  reserve?: string;
}) {
  const { s, C } = useDecoderStyles(makeStyles);
  const lblCol = raw && conf !== undefined ? (conf ? C.good : C.bad) : C.rowLabel;
  /* ★★★ AND EVERY ROW REMEMBERS THE TALLEST IT HAS EVER BEEN. `reserve` above is the right idea
   *     but it has to be told the longest string by hand, and most of these values are ASSEMBLED
   *     from optional clauses — "3.6% · slight · held · IMS standing by · multipath not measurable
   *     at this S/N" is four independent decisions — so a hand-written worst case would be wrong
   *     now and would rot the next time a message changes. Stuart: "lock the fields in the advanced
   *     RDS box to the maximum text size they will show as there is a lot of jitter when the info
   *     changes".
   *  ★★ GROW ONLY, NEVER SHRINK, so a value that gets shorter cannot pull the panel up under the
   *     reader's eye. It converges: minHeight is only ever set to a height the row actually had,
   *     so it can never demand more space than its own content once needed.
   *  ★ NOT truncation. The web panel's CSS records why in capitals — an ellipsis once ate exactly
   *    the half of "5.5 kHz · nominal" that was worth reading. Reserve space; never remove text. */
  const [minH, setMinH] = React.useState(0);
  /* ★★ THE OVERLAID VALUE MUST ALSO BE ABLE TO GROW THE ROW. With `reserve`, the live value is laid
   *  ABSOLUTELY over an invisible worst case — so when a value turned out LONGER than the reserve
   *  ("16.7% · moderate · IMS standing by · NR already blending further"), it spilled out of the row
   *  and printed over the next one (Stuart, 2026-09-28, Multipath over CEQ), and onRowLayout never
   *  saw it: an absolute child adds no height. It reports its own height here; grow-only, as below. */
  const [valH, setValH] = React.useState(0);
  const onValLayout = React.useCallback((e: LayoutChangeEvent) => {
    const h = e.nativeEvent.layout.height;
    setValH(prev => (h > prev + 0.5 ? h : prev));
  }, []);
  const onRowLayout = React.useCallback((e: LayoutChangeEvent) => {
    const h = e.nativeEvent.layout.height;
    // ★ Half a pixel of slack: layout returns fractional heights, and firing setState on every
    //   sub-pixel difference would re-render this row several times a second for no visible gain.
    setMinH(prev => (h > prev + 0.5 ? h : prev));
  }, []);
  return (
    <View style={[s.row, minH ? { minHeight: minH } : null]} onLayout={onRowLayout}>
      <Text style={[s.lbl, { color: lblCol }]} numberOfLines={1}>{label}</Text>
      {reserve ? (
        // ★★ RDS↔PILOT ALTERNATES BETWEEN A SHORT AND A VERY LONG VALUE — "27° ·
        // nominal · 98% steady" versus "rotating 2°/s — encoder not locked to pilot"
        // — and it updates several times a second, so the panel jolted every time it
        // flipped (Stuart). Truncating was the wrong fix: that message IS the
        // diagnosis the field exists to deliver. So reserve the worst case and lay
        // the live value over it — correct at any width, unlike a fixed height.
        <View style={[{ flex: 1 }, valH ? { minHeight: valH } : null]}>
          <Text style={[s.val, { opacity: 0 }]}>{reserve}</Text>
          <Text onLayout={onValLayout}
                style={[s.val, colour ? { color: colour } : null,
                        { position: 'absolute', left: 0, right: 0, top: 0 }]}>{value}</Text>
        </View>
      ) : (
        <Text style={[s.val, colour ? { color: colour } : null]}>{value}</Text>
      )}
    </View>
  );
});

/** ★ Scale that fits the MEAN LOBE DISTANCE to a fixed fraction of the box.
 *  ★★ NEVER SCALE TO A CONSTANT. A constellation's meaning is its SHAPE — how tight the lobes
 *  are and how far from centre — so absolute magnitude is not information. Pinning the scale
 *  made a strong station's points fly out of the box and a weak one's huddle at the origin. */
function constellationScale(xy: number[], box: number): number {
  let n = 0, sum = 0;
  for (let i = 0; i + 1 < xy.length; i += 2) {
    const r = Math.hypot(xy[i], xy[i + 1]);
    if (r < 1) continue;
    n++; sum += r;
  }
  if (!n) return (box / 2) / 110;
  return (box * 0.30) / Math.max(1, sum / n);
}

/** ★★ Rotation that lays the two BPSK lobes on the horizontal. Our detector is DIFFERENTIAL —
 *  it cancels carrier phase in the arithmetic rather than physically de-rotating — so the
 *  constellation arrives tilted by however far our pilot-derived 57 kHz reference sits from the
 *  station's subcarrier. That tilt is real information, but it makes the plot incomparable with
 *  SDR++ or a hardware receiver, where a Costas loop has already flattened it.
 *  BPSK's 180-degree ambiguity is handled by DOUBLING each angle (folding both lobes onto one),
 *  magnitude-weighting so the strong symbols dominate, averaging, then halving. */
function constellationAngle(xy: number[]): number {
  let sx = 0, sy = 0;
  for (let i = 0; i + 1 < xy.length; i += 2) {
    const x = xy[i], y = xy[i + 1];
    const r2 = x * x + y * y;
    if (r2 < 1) continue;
    const a2 = 2 * Math.atan2(y, x);
    sx += r2 * Math.cos(a2);
    sy += r2 * Math.sin(a2);
  }
  return (sx || sy) ? -0.5 * Math.atan2(sy, sx) : 0;
}

/** ★ A plain-English verdict, because the plot assumes you can already read it.
 *  ★★ DE-ROTATE FIRST — computing this on the raw points while only the DRAWING was de-rotated
 *  counted the whole carrier phase offset as error, and a visibly clean constellation reported
 *  "299% EVM". Two consumers of one transform is exactly where that bug lives: share it. */
function constellationVerdict(xy: number[], phaseCoh: number, ber: number):
    { text: string; colour: string } {
  const rot = constellationAngle(xy);
  const cr = Math.cos(rot), sr = Math.sin(rot);
  const rx: number[] = [], ry: number[] = [];
  let n = 0, sumAbsX = 0, sumY2 = 0, sumXErr2 = 0;
  for (let i = 0; i + 1 < xy.length; i += 2) {
    const r2 = xy[i] * xy[i] + xy[i + 1] * xy[i + 1];
    if (r2 < 1) continue;
    const x = xy[i] * cr - xy[i + 1] * sr;
    const y = xy[i] * sr + xy[i + 1] * cr;
    rx.push(x); ry.push(y);
    n++; sumAbsX += Math.abs(x); sumY2 += y * y;
  }
  if (n < 8) return { text: 'RDS no lock', colour: C.bad };
  const meanAbsX = sumAbsX / n;
  if (meanAbsX < 1) return { text: 'RDS no lock', colour: C.bad };
  for (let i = 0; i < rx.length; i++) { const dx = Math.abs(rx[i]) - meanAbsX; sumXErr2 += dx * dx; }
  const evm = (Math.sqrt((sumY2 + sumXErr2) / n) / meanAbsX) * 100;
  // ★ EVM assumes two lobes. A ROTATING constellation defeats that — the points are ordered,
  // not scattered — so it reports a huge figure for a signal decoding flawlessly.
  if (phaseCoh < 0.35 && ber >= 0 && ber < 20)
    return { text: 'rotating — unlocked encoder', colour: C.warn };
  // ★ "SCATTER", not "EVM": the correct term means nothing to someone new to this, and the
  // whole panel is written to explain itself rather than assume.
  if (evm < 45) return { text: `clean · ${evm.toFixed(0)}% scatter`,  colour: C.good };
  if (evm < 80) return { text: `usable · ${evm.toFixed(0)}% scatter`, colour: C.warn };
  return { text: `noisy · ${evm.toFixed(0)}% scatter`, colour: C.bad };
}

/** ★★ The constellation. Two tight lobes = a clean BPSK subcarrier; a RING means the encoder
 *  is sweeping against the pilot, which is a diagnosis and not a fault of ours. Points arrive
 *  pre-scaled x100 and clipped to +/-127 by the server. */
const Constellation = React.memo(function Constellation({ xy, size }: { xy: number[]; size: number }) {
  const { C } = useDecoderStyles(makeStyles);
  const pts = useMemo(() => {
    const out: { x: number; y: number }[] = [];
    const half = size / 2;
    const rot = constellationAngle(xy);
    const cr = Math.cos(rot), sr = Math.sin(rot);
    const k = constellationScale(xy, size);
    for (let i = 0; i + 1 < xy.length; i += 2) {
      const x = xy[i] * cr - xy[i + 1] * sr;
      const y = xy[i] * sr + xy[i + 1] * cr;
      out.push({ x: half + x * k, y: half - y * k });
    }
    return out;
  }, [xy, size]);
  return (
    <Canvas style={{ width: size, height: size }}>
      <Rect x={0} y={0} width={size} height={size} color={C.plot} />
      <Rect x={size / 2 - 0.5} y={0} width={1} height={size} color={C.axis} />
      <Rect x={0} y={size / 2 - 0.5} width={size} height={1} color={C.axis} />
      {/* ★★★ ONE NODE FOR THE WHOLE SCATTER, NOT ONE PER POINT. This was `pts.map(... <Circle/>)`
          — a React ELEMENT for every point, several hundred of them, created, reconciled and
          turned into Skia scene-graph nodes SIX TIMES A SECOND, across two plots.
          ★★★ AND THAT IS THE WHOLE DIFFERENCE FROM THE BROWSER, which draws the identical picture
          with a few hundred imperative arc() calls into a canvas and allocates nothing. Stuart:
          "the browser doesnt stutter with advanced RDS open, or nowhere near as much as the app
          did", and Safari sits at 6% CPU with the full panel running. The cost was never
          JavaScript — the web client IS JavaScript. It was declarative-per-point drawing.
          ★ `Points` takes the array and draws it in one primitive; round caps make each a dot, so
          it is the same picture. */}
      <Points points={pts} mode="points" style="stroke" strokeWidth={2.4}
              strokeCap="round" color={C.dot} />
    </Canvas>
  );
});

/** ★ THE EYE — symbol value against time, the web's `drawEye`: two clean bands with a clear gap
 *  between them means every bit is being read; a smear across the middle is where errors come
 *  from. The same points as the constellation, rotated onto the wanted axis, drawn left to right.
 *  One Points node, as the constellation — never an element per symbol. */
const Eye = React.memo(function Eye({ xy, width, height }: { xy: number[]; width: number; height: number }) {
  const { C } = useDecoderStyles(makeStyles);
  const pts = useMemo(() => {
    const out: { x: number; y: number }[] = [];
    const n = xy.length / 2;
    if (n < 2) return out;
    const rot = constellationAngle(xy);
    const cr = Math.cos(rot), sr = Math.sin(rot);
    const k = constellationScale(xy, height) * 0.9;
    const mid = height / 2;
    for (let i = 0; i < n; i++) {
      const x = xy[i * 2] * cr - xy[i * 2 + 1] * sr;
      out.push({ x: (i / (n - 1)) * (width - 2) + 1, y: mid - x * k });
    }
    return out;
  }, [xy, width, height]);
  return (
    <Canvas style={{ width, height }}>
      <Rect x={0} y={0} width={width} height={height} color={C.plot} />
      <Rect x={0} y={height / 2 - 0.5} width={width} height={1} color={C.axisStrong} />
      <Points points={pts} mode="points" style="stroke" strokeWidth={1.8}
              strokeCap="round" color={C.trace} />
    </Canvas>
  );
});

/** ★ THE MPX SPECTRUM — everything the FM demodulator produces, DC to 100 kHz.
 *  ★★ LABELLED AT THE LANDMARKS, because a spectrum of a signal most listeners have never seen
 *  plotted is otherwise just a wiggly line: L+R audio at the bottom, the 19 kHz PILOT, the L−R
 *  stereo sidebands at 38 kHz, and RDS at 57 kHz. Without them the plot cannot be read at all,
 *  which is how it shipped on the phone first time round. */
const MPX_SPAN = 100000;
const MPX_MARKS: [number, string][] = [[19000, 'PILOT'], [38000, 'L−R'], [57000, 'RDS']];
const mpxFont = matchFont({ fontFamily: 'monospace', fontSize: 8 });

const Mpx = React.memo(function Mpx({ mpx, width, height }: { mpx: number[]; width: number; height: number }) {
  const { C } = useDecoderStyles(makeStyles);
  const path = useMemo(() => {
    const p = Skia.Path.Make();
    if (!mpx.length) return p;
    // ★ AUTO-RANGE on what is present, as the web client does: injection levels vary, and a
    // fixed floor either clips a loud station or flattens a quiet one into noise.
    let lo = 999, hi = -999;
    for (const v of mpx) { if (v < lo) lo = v; if (v > hi) hi = v; }
    if (hi - lo < 12) hi = lo + 12;
    for (let i = 0; i < mpx.length; i++) {
      const x = (i / (mpx.length - 1)) * width;
      const y = height - ((mpx[i] - lo) / (hi - lo)) * (height - 12);
      if (i === 0) p.moveTo(x, y); else p.lineTo(x, y);
    }
    return p;
  }, [mpx, width, height]);
  return (
    <Canvas style={{ width, height }}>
      <Rect x={0} y={0} width={width} height={height} color={C.plot} />
      {/* L+R occupies DC..15 kHz — a band rather than a line, so shade it. */}
      <Rect x={0} y={10} width={(15000 / MPX_SPAN) * width} height={height - 10}
            color={C.axisFaint} />
      {mpxFont && <SkText x={2} y={8} text="L+R" font={mpxFont} color={C.chartText} />}
      {MPX_MARKS.map(([hz, label]) => {
        const x = (hz / MPX_SPAN) * width;
        return (
          <React.Fragment key={label}>
            <Rect x={x - 0.5} y={10} width={1} height={height - 10} color={C.mark} />
            {mpxFont && <SkText x={Math.min(width - 26, x + 2)} y={8} text={label}
                                font={mpxFont} color={C.chartText} />}
          </React.Fragment>
        );
      })}
      <Path path={path} color={C.mpxLine} style="stroke" strokeWidth={1.2} />
    </Canvas>
  );
});

/** The eye grid's alphabet — MUST match kEyeAlphabet in local_sdr_shim.cpp and EYE_ALPHABET in
 *  the web client. An offset encoding was tried first and put a '"' and a '\\' inside a JSON
 *  string, which killed the whole rdsx message (2026-09-13). */
const EYE_ALPHABET = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-';

/** ★★ THE COMPOSITE EYE — the MPX in TIME, triggered on the pilot PLL's own recovered phase, so
 *  the sweeps cannot jitter the way a scope's do. Two pilot cycles wide, high-passed above the
 *  audio server-side (L+R is not pilot-coherent and smears into a band), so the 19 kHz pilot,
 *  the 38 kHz L−R sidebands and 57 kHz RDS braid across it. Shows composite PEAK behaviour —
 *  overmodulation flattens the tops — which neither the spectrum nor the constellation reveals.
 *
 *  ★★★ ONE IMAGE, NOT 2048 RECTS. The grid is an intensity histogram accumulated on the server,
 *      and drawing a Rect per cell is exactly the mistake the Constellation note below warns
 *      about. Raw pixels into an SkImage is one draw call regardless of the grid size.
 */
/** ★★ Decode the run-length form — '.' then one character giving 1..64 zeros, anything else a
 *  literal cell. See the encoder in local_sdr_shim.cpp; it is what lets the grid be 96x48 per
 *  component without the wire cost tripling. */
function decodeEye(src: string, want: number): Uint8Array | null {
  if (!src) return null;
  const out = new Uint8Array(want);
  let o = 0;
  for (let i = 0; i < src.length && o < want; i++) {
    const c = src[i];
    if (c === '.') {
      const n = EYE_ALPHABET.indexOf(src[++i]) + 1;
      if (n <= 0) return null;
      o += n;
    } else {
      const v = EYE_ALPHABET.indexOf(c);
      if (v < 0) return null;
      out[o++] = v;
    }
  }
  return o >= want ? out : null;
}

/** The three component colours — cool end on purpose: this panel already speaks green/amber/red
 *  for QUALITY and its chrome is amber, so a warm trace would read as a verdict. */
const EYE_COLOURS: Array<[number, number, number]> = [
  [ 80, 230, 255],   // pilot  — cyan
  [255,  90, 210],   // stereo — magenta
  [150, 165, 255],   // RDS    — blue-lavender: violet at 170,110 vanished into the magenta (Stuart, 2026-09-14)
];

/** ★★★ SCOPE VIEW — three boxes, one per component, each band drawn ALONE from its own grid in
 *  the colour of the panel's verdict for it (the web's drawMpxEye, ported 2026-09-14 after the
 *  app was found still on the overlaid composite: "wrong scope in the app"). Nothing is
 *  composited, so nothing can hide anything; the server already scales each band to its own
 *  peak, so the client only paints. One SkImage per box, never a rect per cell. */
const ScopeBox = React.memo(function ScopeBox(
  { grid, ew, eh, width, height, colour }:
  { grid: string; ew: number; eh: number; width: number; height: number; colour: [number, number, number] }) {
  const { C } = useDecoderStyles(makeStyles);
  const img = useMemo(() => {
    if (ew <= 0 || eh <= 0) return null;
    const n = ew * eh;
    const cells = decodeEye(grid, n);
    if (!cells) return null;
    const px = new Uint8Array(n * 4);
    for (let i = 0; i < n; i++) {
      const v = cells[i];
      if (v <= 0) continue;
      const a = 0.10 + 0.90 * Math.pow(v / 63, 0.55);
      const o = i * 4;
      px[o] = colour[0]; px[o + 1] = colour[1]; px[o + 2] = colour[2];
      px[o + 3] = Math.min(255, Math.round(255 * a));
    }
    return Skia.Image.MakeImage(
      { width: ew, height: eh, colorType: ColorType.RGBA_8888, alphaType: AlphaType.Unpremul },
      Skia.Data.fromBytes(px), ew * 4);
  }, [grid, ew, eh, colour]);
  return (
    <Canvas style={{ width, height }}>
      <Rect x={0} y={0} width={width} height={height} color={C.plot} />
      {img && <SkiaImage image={img} x={0} y={0} width={width} height={height} fit="fill" />}
      <Rect x={0} y={height / 2 - 0.5} width={width} height={1} color={C.mark} />
      <Rect x={width / 2 - 0.5} y={0} width={1} height={height} color={C.markFaint} />
    </Canvas>
  );
});

export default function AdvRdsPanel(p: AdvRdsPanelProps) {
  const { s, C } = useDecoderStyles(makeStyles);
  const busX = useBusValue(p.bus);
  const x = p.bus ? (busX ?? null) : (p.x ?? null);
  const { raw } = p;
  // ★ The three Skia plots, at 3 Hz rather than the full rdsx rate — see useThrottledPoints.
  const plotXy  = useThrottledPoints(x?.xy);
  const plotMpx = useThrottledPoints(x?.mpx);
  /** ★ SAY WHAT FULL SCALE IS. The eye autoscales — a quiet passage genuinely shrinks the
   *  composite — so without this it would look identical at every level. */
  /* ★ Colour is the VERDICT, not the identity — the same thresholds and colours as the readouts
   *  and the web (main.ts drawMpxEye): pilot green in 6.0–7.5 kHz and locked; RDS green 1.5–5.8,
   *  amber weak, red absent/over spec; stereo by MPX S/N (mono is legitimate). */
  const scopeBands = useMemo(() => {
    const GOOD: [number, number, number] = [125, 255, 154], WARN: [number, number, number] = [255, 212, 121], BAD: [number, number, number] = [255, 138, 125];
    const pdev = x?.pilotDev ?? 0, rdev = x?.rdsDev ?? 0, snr = x?.mpxSnr ?? 0;
    const pilotCol  = (x && !x.pilotLock) ? WARN : (pdev >= 6.0 && pdev <= 7.5) ? GOOD : WARN;
    const rdsCol    = (rdev <= 0.2 || rdev > 5.8) ? BAD : rdev < 1.5 ? WARN : GOOD;
    const stereoCol = snr >= 28 ? GOOD : snr >= 10 ? WARN : BAD;
    const amp = x?.eyeAmp ?? [0, 0, 0];
    return [
      { name: 'PILOT',  g: x?.eyeP ?? '', colour: pilotCol,  khz: pdev },
      { name: 'STEREO', g: x?.eyeS ?? '', colour: stereoCol, khz: amp[1] ?? 0 },
      { name: 'RDS',    g: x?.eyeR ?? '', colour: rdsCol,    khz: rdev },
    ];
  }, [x?.eyeP, x?.eyeS, x?.eyeR, x?.pilotDev, x?.rdsDev, x?.rdsDevPeak, x?.mpxSnr, x?.pilotLock, x?.eyeAmp]);
  /* ★★ THE SAME PLAIN-ENGLISH READING THE WEB CLIENT GIVES. A bare scale says how far the axis
   *  goes and nothing about whether that is good, which is the only question anyone has.
   *  ★ THREE PILOT STATES, and the middle one is the point of the plot: a pilot that is PRESENT
   *  but repeatedly almost locking shows as a trace that forms and collapses, which no single
   *  number reports. And the level wording is suppressed entirely at low S/N, because the
   *  autoscale then tracks NOISE and would call mush "very strong". */
  /* ★★★ DECLARED BEFORE THE EYE VERDICT THAT READS IT. It sat below, next to the deviation
   *  readout it belongs to, and the eye's useMemo above referenced it — a `const` in the
   *  temporal dead zone, so the first frame with a LOCKED pilot and a positive S/N threw
   *  "Cannot access 'devGate' before initialization" and the whole SDR screen unmounted back
   *  to the directory (Stuart, 2026-09-14: "opening the advanced RDS box in the app crashes
   *  back to the directory screen"). Every other branch was fine, which is why it survived a
   *  typecheck: TypeScript does not flag use-before-declare inside a closure. */
  const devGate = useRef(false);
  const eyeVerdict = useMemo(() => {
    const d = x?.eyeDev ?? 0;
    const snr = x?.mpxSnr ?? 0;
    const locked = !!x?.pilotLock;
    const pilotSeen = (x?.pilotDev ?? 0) > 0.5;
    if (!locked && pilotSeen) return { t: 'pilot present but not locking — forms and collapses', c: C.warn };
    if (!locked)              return { t: 'no pilot — untriggered, so this is noise not a trace', c: C.bad };
    // ★ Same latched gate as the meter below, so the two readouts in one box cannot contradict
    //   each other on a station sitting near the threshold.
    if (snr > 0 && !devGate.current) return { t: 'buried in noise — level not measurable', c: C.bad };
    let what: string;
    if (d < 3)       what = 'little above the audio — mono or blended';
    else if (d < 10) what = 'pilot dominates · little stereo';
    else if (d < 25) what = 'pilot, stereo and RDS';
    else if (d < 50) what = 'strong stereo';
    else             what = 'very strong · near full deviation';
    const noisy = (snr > 0 && snr < 28) ? ' · noisy' : '';
    return { t: d > 0.1 ? `${what}${noisy}` : '—', c: (snr > 0 && snr < 28) ? C.warn : C.muted };
  }, [x?.eyeDev, x?.mpxSnr, x?.pilotLock, x?.pilotDev]);

  /* ★★ TOTAL PEAK DEVIATION against the 75 kHz limit — the headline broadcast figure, and the
   *  one the eye draws as a flattened top while nothing reports it. Averaged on the panel's
   *  clock; the number NEVER disappears, it dims and says so, because a row that vanishes reads
   *  as a broken feature (Stuart, 2026-09-13). */
  /* ★★★ HYSTERESIS, SAME AS THE WEB METER — 10 dB in, 8 dB out. One threshold on a wandering
   *  measurement is a flap: a station sitting on the line makes the label blink between trusted
   *  and unreliable. This codebase has built that fault three times already (CEQ across one
   *  multipath threshold, the RF AGC window narrower than one LNA step, and this readout), so
   *  the two figures deliberately cannot meet.
   *  ★ A ref rather than state: it must not trigger a re-render, only colour the next one. */
  const mpxDevInfo = useMemo(() => {
    const md = x?.mpxDev ?? 0;
    const hd = x?.mpxHold ?? 0;
    const av = x?.mpxAvg ?? 0;
    const snr = x?.mpxSnr ?? 0;
    if (snr >= 10) devGate.current = true;
    else if (snr > 0 && snr < 8) devGate.current = false;
    /* ★ Live if EITHER figure is up: the hold outlives the peak by 6 s, so gating on one alone
     *  would blank a reading that is still legitimately on screen. */
    const live = md > 0.1 || hd > 0.1;
    const ok = live && devGate.current;
    /* ★ The cells keep their shape with no signal too — a readout must not disappear, and it
     *  must not change SIZE either or the panel jumps as a station fades in. */
    if (!live) return { avg: '—', peak: '—', verdict: 'no signal', c: C.muted,
                        pct: 0, avgPct: 0, hold: 0 };
    /* ★★★ THE DIGITS READ THE 6 s HOLD, THE BAR READS THE FAST PEAK. Since 5.6.50 the server
     *  publishes three statistics of one measurement (see RdsExt.mpxDev): the true peak moves
     *  every frame, and Stuart, 2026-09-25: "if it is bouncing up and down like a yoyo then the
     *  number looks like a stopwatch, how do you read that?" So the number quoted — and the
     *  verdict judged — is the hold, because what a modulation monitor is asked is "did it go
     *  over", not "where is it this instant"; quoting the steady average beside it is what makes
     *  judging on the peak safe. Movement stays visible in the bar fill, where it is information.
     *  ★ Before this, mpxDev was a 1.5 s AVERAGE mislabelled as peak, so a 75 kHz peak on
     *  jazz/classical/speech read about 38 (Onfliner, 2026-09-25, [[BRIEF-deviation-meter]]). */
    const pk = hd;
    /* ★ SAY WHAT WAS REMOVED, as the web does: the server takes the guard-band noise out in
     *  quadrature and reports it; NEGATIVE means a neighbour sat in the guard band and nothing
     *  was removed (Stuart, 2026-09-14: "this one doesn't say if it is subtracting any noise"). */
    const nz = x?.mpxNoise ?? 0;
    const nzTxt = nz >= 1 ? ` · ${nz.toFixed(0)} kHz noise removed`
                : nz <= -1 ? ' · neighbour in the guard band, noise not removed' : '';
    /* ★ OMITTED, NOT ZEROED, when the server is older than the three-figure rdsx: an "avg 0 kHz"
     *  would read as a measurement of nothing. Against an old server this line is byte-identical
     *  to what it printed before, bar the "peak" label. */
    /* ★★★ THREE CELLS, NOT ONE SENTENCE. As a single line the readout re-wrapped every time a
     *  digit changed width and "the whole block shifts" (Stuart, 2026-09-25) — two numbers that
     *  move independently, several times a second, cannot share a line of running text. So the
     *  values are returned separately and drawn in a fixed grid; nothing moves but the digits. */
    const avTxt = av >= 0.5 ? `${av.toFixed(0)} kHz` : '—';
    /* ★★★ A VERDICT MAY NOT RESOLVE FINER THAN THE MEASUREMENT, and the hedge goes ABOVE the
     *  limit. Stuart's RTL-vs-RSP A/B (2026-09-26) put the same transmitter, same instant, at
     *  peak 75 on one radio and 80 on the other: the numbers agreed to 7 %, so a hard step at 75
     *  makes two radios disagree in WORDS about a signal they agree about in figures.
     *  ★★ So 75–82 hedges — at 78 the reading could be a compliant 75 on another radio. Below 75
     *     there is nothing to hedge: ±75 kHz IS the spec and reaching it is correct behaviour, so
     *     the panel says "nominal" and means it. ✗ The web client had this band at 72–82, which
     *     straddled the limit and swallowed "nominal" entirely — every healthy station sits just
     *     under 75 by design (Stuart: "at around 74-75KHz which is the spec it says close to the
     *     limit"). Fixed in both readers together; they must keep saying the same thing.
     *  ★ OVERMODULATED stays above 82, where both radios agreed (92/94 on 96.1). */
    const verdict = ok ? (pk > 82 ? 'OVERMODULATED'
                        : pk > 75 ? 'close to the limit' : 'nominal')
                       : 'low S/N, unreliable';
    const c = !ok ? C.muted : pk > 82 ? C.bad : pk > 75 ? C.warn : C.good;
    return { avg: avTxt, peak: `${pk.toFixed(0)} kHz`,
             verdict: (verdict + nzTxt).replace(/^ · /, ''),
             c, pct: Math.max(0, Math.min(100, md)),
             avgPct: Math.max(0, Math.min(100, av)),
             hold: Math.max(0, Math.min(100, hd)) };
  }, [x?.mpxDev, x?.mpxAvg, x?.mpxHold, x?.mpxSnr, x?.mpxNoise]);
  /** MPX power (BS.412) as drawn — see the row and services/mpxPower.ts ("settling 44 s" until the minute is in). */
  const mpxPowTxt = useMemo(() => {
    return mpxPowerParts(x?.mpxPow ?? 0, x?.mpxPowS ?? 0);   // ★ shared with the web panel
  }, [x?.mpxPow, x?.mpxPowS]);
  const piNum = p.pi ? parseInt(p.pi, 16) : 0;
  /** Last real RDS deviation reading, so a momentary dropout does not blank the row. */
  const rdsHold = useRef<{ txt: string; col: string; at: number } | null>(null);
  /** Last real multipath string, shown dimmed as "… · held" when the S/N dips out. */
  const mpHeld = useRef<string | null>(null);

  // ── PTY ─────────────────────────────────────────────────────────────────────
  const ptyV = (raw ? x?.ptyRaw : x?.pty) ?? -1;
  const ptyTxt = ptyV >= 0 ? `${PTY_EU[ptyV] ?? '?'} (${ptyV})` : DASH;

  // ── TP / TA / MS. All three ride block B, so they confirm together. ──────────
  const tpV = (raw ? x?.tpRaw : x?.tp) ?? -1;
  const taV = (raw ? x?.taRaw : x?.ta) ?? -1;
  const msV = (raw ? x?.msRaw : x?.ms) ?? -1;
  const flags: string[] = [];
  if (tpV === 1) flags.push('TP');
  if (taV === 1) flags.push('TA');
  if (msV === 1) flags.push('Music'); else if (msV === 0) flags.push('Speech');

  // ── Deviations, each said against its own spec band so the number explains itself ──
  const pdev = x?.pilotDev ?? 0;
  const rdev = x?.rdsDev ?? 0;
  // ── The weak-signal readings (VibeServer 3.1) ─────────────────────────────────────────────
  // ★★ Each says what it MEANS, not just a number. "22 dB" invites "is that good?"; "22 dB · blend
  //    8.4k · cut 11.2k" answers the question the listener actually has, which is why the station
  //    sounds the way it does.
  const snrOk = x?.snrOk !== false;
  const mpxSnr = x?.mpxSnr ?? 0;
  let snrTxt = DASH, snrCol: string | undefined;
  if (!snrOk) {
    // ★★★ NO PILOT, NO FIGURE. Both this and multipath are ratios against the 19 kHz pilot, and
    //     with the pilot collapsed the quotients explode — 70 dB beside 580% multipath, measured on
    //     air. A confident number about an unmeasurable signal is worse than a blank.
    snrTxt = 'no pilot to measure';
  } else if (mpxSnr > 0.5 && p.wsp === false) {
    /* ★★★ A SWITCHED-OFF PROCESSOR IS NOT A CLEAN SIGNAL. The second clause describes what the
     *  receiver is DOING about the noise, and with weak-signal processing off it is doing nothing
     *  BY INSTRUCTION — which reads identically to "nothing was needed" unless we say so. The
     *  browser has printed "bypassed" here all along and the app printed the flattering version,
     *  so the same server produced two different stories about the same signal.
     *  ★ No colour: neither good news nor bad, just a control the listener set. */
    snrTxt = `${mpxSnr.toFixed(0)} dB · bypassed`;
  } else if (mpxSnr > 0.5) {
    const lmr = x?.hiCutLmr ?? 15000, aud = x?.hiCutAud ?? 15000;
    const acting = lmr < 14000 || aud < 14000;
    snrTxt = `${mpxSnr.toFixed(0)} dB · ` + (!acting ? 'clean · no treatment'
      : aud < 14000 ? `blend ${(lmr / 1000).toFixed(1)}k · cut ${(aud / 1000).toFixed(1)}k`
                    : `blend ${(lmr / 1000).toFixed(1)}k`);
    // ★ Amber is not a warning — it means the receiver is working for its living, which on a
    //   difficult signal is the good outcome.
    snrCol = acting ? C.warn : C.good;
  }
  const mp = x?.multipath ?? 0;
  let mpTxt = DASH, mpCol: string | undefined;
  if (!snrOk) mpTxt = 'no pilot to measure';
  else if (x?.multipathOk) {
    const pct = mp * 100;
    // ★ "None detected" is a RESULT. Printing a dash for zero multipath on a signal we CAN measure
    //   says "no data", which is the opposite meaning.
    const label = pct < 0.5 ? 'none detected' : mp < 0.03 ? 'clean'
                : mp < 0.10 ? 'slight' : mp < 0.20 ? 'moderate' : 'severe';
    mpTxt = pct < 0.5 ? label : `${pct.toFixed(1)}% · ${label}`;
    mpCol = mp < 0.03 ? C.good : mp < 0.10 ? undefined : mp < 0.20 ? C.warn : C.bad;
    mpHeld.current = mpTxt;
  } else if (x && mpHeld.current) {
    /* ★★ HOLD THE LAST REAL READING RATHER THAN ANNOUNCE DEFEAT. `multipathOk` drops out on a
     *  fade for a frame or two, and "too noisy to judge" flashing over a perfectly good 4.6%
     *  reading is a worse answer than the reading itself. Dimmed and labelled "held" so it is
     *  never mistaken for live — same as the browser. */
    mpTxt = `${mpHeld.current} · held`;
    mpCol = C.muted;
  } else if (x) mpTxt = 'too noisy to judge';
  /* ★★★ AND SAY WHAT THE SUPPRESSOR IS DOING ABOUT IT. The row measured the damage and never
   *  mentioned the cure, so a listener watching IMS pull a station out of a reflection saw only
   *  the damage figure and concluded nothing was happening.
   *  ★ Appended AFTER the whole chain, deliberately — it applies to the held and the live cases
   *    alike, and the browser's own comment warns against burying it inside one arm. */
  if (x && snrOk) {
    const ib = x.imsBlend ?? 0, iw = x.imsWhy ?? 1;
    mpTxt += ib > 0 ? ` · IMS blending L−R to ${(ib / 1000).toFixed(1)}k`
      : iw === 2 ? ' · IMS standing by · CEQ has it'
      : iw === 3 ? ' · IMS standing by · nothing to suppress'
      : iw === 4 ? ' · IMS standing by · NR already blending further'
      : iw === 5 ? ' · multipath not measurable at this S/N'
      : '';
  }
  // ★ CEQ is shown as BEFORE → AFTER: "engaged" says nothing about whether it helped, and a blind
  //   equaliser quietly making things worse is the failure mode that matters.
  let ceqTxt = DASH;
  // ★★ GREEN MEANS IT IS DOING SOMETHING FOR YOU, and it is the same rule as the browser's — the
  //    two clients read the same fields and must not draw different conclusions from them. Green
  //    on "engaged" alone would reward the equaliser for running rather than for helping.
  let ceqCol: string | undefined;
  if (x?.ceqOn) {
    const before = mp * 100, after = (x?.ceqAfter ?? 0) * 100;
    ceqCol = (before > 0.5 && after < before * 0.8) ? C.good : undefined;
    ceqTxt = `${before.toFixed(1)}% → ${after.toFixed(1)}%`
           + (before > 0.5 && after < before * 0.8 ? '' : ' · little change');
  } else if (x) {
    const why = x?.ceqWhy ?? 3;
    ceqTxt = why === 1 ? 'off' : why === 2 ? 'signal too weak to equalise'
                                           : 'standing by · nothing to correct';
  }
  const nbPct = (x?.nbRate ?? 0) * 100;
  const nbTxt = !x ? DASH
    : nbPct < 0.005 ? 'nothing to blank'
    : `${nbPct.toFixed(nbPct < 1 ? 2 : 1)}% blanked`;
  // ★ The IF figure is "the OTHER option minus this one", so it MUST be read against the current
  //   state — the sign flips the moment the filter engages, and a bare number is genuinely
  //   ambiguous. Printed as a sentence.
  const ifG = x?.ifGain ?? 0, ifC = x?.ifCand ?? 0, ifBw = x?.ifBw ?? 0;
  let ifTxt = DASH;
  // ★★★ GREEN WHILE NARROWED, because that is the one state the reading cannot make obvious on its
  //     own: the shadow always evaluates THE OTHER OPTION, so once IMS engages the number turns
  //     NEGATIVE ("wide would cost 3.9 dB") and an unhighlighted negative reads like bad news when
  //     it means the opposite. The browser has always coloured it; the app did not, so the same
  //     server produced two different-looking answers (Stuart, 2026-08-15).
  let ifCol: string | undefined;
  /* ★★★ REPORT THE FILTER THAT IS ACTUALLY APPLIED, EVEN WITH NO SHADOW TO COMPARE IT TO. The row
   *  was gated ENTIRELY on `ifC > 0` — the shadow measurement — so with IMS off, or before the
   *  shadow settles, a genuinely narrowed IF showed a DASH. A dash reads as "no filtering", which
   *  is the opposite of the truth, and it is the auto-bandwidth case that hits ordinary users.
   *  ★★★ AND THE ADJECTIVE MUST COME FROM THE WIDTH. "narrow" was hardcoded, so a 196k IF — wider
   *  than the 150k a broadcast signal needs — was announced as "196k narrow". The panel was
   *  contradicting its own number. <=160k is narrow, >=220k is wide, and in between gets NO
   *  adjective rather than a made-up one.
   *  ★ The comparison clause is appended only when the shadow exists, because that is the only
   *    thing that ever needed the shadow. */
  if (x && ifBw > 0) {
    ifCol = C.good;
    const w = Math.round(ifBw / 1000);
    const adj = ifBw <= 160000 ? ' narrow' : ifBw >= 220000 ? ' wide' : '';
    ifTxt = `${w}k${adj}`;
    if (ifC > 0 && mpxSnr > 0.5) {
      ifTxt += ' · wide would ' +
        (ifG > 1.5 ? `gain ${ifG.toFixed(1)} dB` : `cost ${Math.abs(ifG).toFixed(1)} dB`);
    }
  } else if (x && ifC > 0 && mpxSnr > 0.5) {
    ifCol = ifG > 1.5 ? C.good : undefined;
    ifTxt = `wide · ${Math.round(ifC / 1000)}k ` +
      (ifG > 1.5 ? 'would help' : ifG < -1.5 ? 'would cost' : 'no real gain');
  }

  let pilotTxt = DASH, pilotCol: string | undefined;
  if (pdev > 0.2) {
    const ok = pdev >= 6.0 && pdev <= 7.5;
    /* ★★ AND WHETHER THE PLL IS LOCKED. Deviation alone does not say it: 5.2 kHz reads "low"
     *    against the 6.0-7.5 spec while the loop is locked and stereo is playing. The panel's
     *    only other "lock" is the RDS constellation's, so one word covered two independent
     *    failures until now — it misled this panel's own author on air (2026-08-25). */
    const plk = x?.pilotLock;
    const lockTxt = plk === undefined ? '' : plk ? ' · locked' : ' · NOT locked';
    pilotTxt = `${pdev.toFixed(1)} kHz · ${ok ? 'nominal' : pdev < 6 ? 'low' : 'high'}${lockTxt}`;
    pilotCol = plk === false ? C.warn : ok ? C.good : C.warn;
  }
  // ★★ NEVER FALL BACK TO A DASH HERE. The measurement drops below the 0.2 kHz floor for a frame
  // or two on a marginal signal, so the row flipped value→dash→value several times a second:
  // "no point showing a dash with a figure quickly snapping in then going again" (Stuart,
  // 2026-07-28). Hold the last real reading briefly, then say zero honestly — a steady "0.0 kHz"
  // is information, a strobing dash is not.
  let rdsDevTxt = '0.0 kHz · none', rdsDevCol: string | undefined = C.muted;
  /* ★★★ REFUSE THE FIGURE BELOW THE GATE — the row was answering a question it could not hear.
   *  `devGate` is the same latched MPX S/N hysteresis the deviation METER already uses (in at
   *  >=10 dB, out below 8), and it was computed right here and consulted only for the meter's
   *  verdict. So on a 5 dB signal this row still printed something like
   *  "avg 3.3 · peak 11.3 · raw 4.7 kHz · nominal" — a peak nearly 2x the spec maximum, dressed
   *  up with the word "nominal", from noise in the RDS band.
   *  ★★ Kiko's Brazilian set is the live evidence: WHATS 67 at 35 % block errors and 15 dB MPX
   *     S/N, mento at 18 % — the two least trustworthy readings of the seven, with nothing on
   *     screen saying so (2026-09-27).
   *  ★ The 4 s hold below is for a momentary DROPOUT and must not paper over this: a signal that
   *    is simply too weak is a standing condition, not a blink, so the refusal comes first. */
  /* ✗✗✗ AND BLOCK SYNC IS NOT THE SAME AS USABLE BLOCKS. Gated on S/N alone, a station at 100 %
   *     BLOCK ERRORS printed "avg 7.2 · peak 13.6 · raw 10.2 kHz" — measured from pure noise in the
   *     ±2.4 kHz band — while every neighbouring field said "not measurable" (Stuart, 2026-09-27,
   *     107.400). The frame before showed a dash only because errors read "—" (no sync at all); at
   *     100 % you HAVE sync and zero good blocks, and the row measured the hiss.
   *  ★ 50 % is not a tuned constant — it is the point past which most blocks are wrong. The
   *    "over spec — suspect" ceiling still stands behind this as the last line of defence. */
  /* ★ Read from the live extras here rather than the `ber` further down, which is declared for
   *  the flags row 60 lines later — same number, but this gate runs first. */
  const berNow = (x && x.ber >= 0) ? x.ber : (p.ber ?? -1);
  const rdsBerBad = berNow >= 50;
  const devGateOpen = (devGate.current || mpxSnr <= 0) && !rdsBerBad;
  if (rdev > 0.2 && !devGateOpen) {
    rdsDevTxt = rdsBerBad ? 'not measurable at this error rate' : 'not measurable at this S/N';
    rdsDevCol = C.warn;
    // ✗ Deliberately NOT written to rdsHold: holding a refusal would keep it on screen for 4 s
    //   after the signal recovered, and the hold exists to smooth the opposite case.
  } else if (rdev > 0.2) {
    // ★★ THE SCALE HAS A CEILING, SO THE LABELS MUST TOO. 7.5% of 75 kHz = 5.6 kHz is the
    // spec maximum; a reading past it is evidence of a MEASUREMENT problem, never of a
    // strong subcarrier, and must not be dressed up as good news.
    /* ★★★ JUDGE ON THE MEASURED PEAK WHEN THERE IS ONE. `rdev` scales a mean envelope by a
     *  crest factor. ✓ FIXED 2026-09-26 (40857b93): the deficit was TWO things, a 1.205 chain
     *  loss and a crest factor of 1.659 rather than 1.520, and both are now applied in rds.cpp.
     *  ★ Historical note, because the numbers below are read against it: BEFORE that fix this
     *  read ~16 % low — the "~1.3 dB low against a Pira" the engine already knew about and
     *  wrongly blamed on a signal-path loss. A verdict drawn on a systematically low figure
     *  mislabels stations at the boundaries, which is why it mattered.
     *  ✗ Any reading captured on a build older than 5.6.64 needs ×1.205 before comparing.
     *  ★★ BOTH ARE SHOWN and the averaged one is NOT changed — it is the figure validated against
     *  Hans's analyser (Stuart, 2026-09-26: "we must however also preserve our PIRA tested
     *  numbers"). Peak first, average beside it, as the deviation meter reads. */
    /* ★★★ VERDICT ON THE PIRA-TESTED FIGURE, peak shown beside it. The peak's percentile is a
     *  free parameter and currently over-corrects by ~23 % against what Hans's table implies, so
     *  it is information, not authority, until a known MPX input settles the statistic. Nothing
     *  validated against his analyser changes. */
    const rpk = x?.rdsDevPeak ?? 0;
    /* ★★ "TYPICAL" READ AS A SECOND AVERAGE. Stuart, 2026-09-26: "difference between average and
     *  typical? I read both of those as an average." Beside a figure labelled `avg` it does —
     *  and it was already the odd one out: the PILOT row above says "nominal" and the MPX
     *  deviation row says "nominal", so RDS alone spoke a different dialect for the same idea.
     *  ★ Both numbers are labelled now, in the same words and the same order the deviation row
     *    uses, so nothing has to be inferred from position. */
    const pkTxt = rpk > 0.2 ? ` · peak ${rpk.toFixed(1)}` : '';
    /* ★★★ THE UNCORRECTED FIGURE, AND THIS PANEL IS THE ONE THAT MATTERS FOR IT. `rdsDevRaw` is
     *  `avg` with the guard-band noise subtraction skipped — identical maths otherwise, identical
     *  smoother — so the pair said whether the ~16 % deficit against MpxTool lived in the
     *  subtraction or in the crest factor. ✓ It answered: the crest factor, plus a chain loss.
     *  ★ Kept, because it is the control arm that makes any FUTURE deviation claim checkable.
     *  ★★★ AND IT NEARLY SHIPPED TO THE ONE PERSON WHO CAN ANSWER THAT WITHOUT THIS LINE. The web
     *     client got `raw` and this panel did not, because they are two renderers of one rule —
     *     the ONE RULE, TWO READERS shape, again. Onfliner runs the APP with a local dongle and NO
     *     server (Stuart, 2026-09-26: "he is using android directly no server"), so the web
     *     client's copy is invisible to him and build 494 would have been useless for the
     *     measurement it was built for. ✗ Whoever changes one of these rows changes BOTH.
     *  ★ Drawn only where the subtraction is actually doing something (>2 %): with the guard idle
     *    the server sends the same number twice and "avg 2.1 · raw 2.1" is noise in the row. */
    const rrawV = x?.rdsDevRaw ?? 0;
    const rawTxt = rrawV > 0.2 && Math.abs(rrawV - rdev) > rdev * 0.02
      ? ` · raw ${rrawV.toFixed(1)}` : '';
    const impossible = rdev > 5.8, strong = rdev >= 4.0, low = rdev < 1.5;
    rdsDevTxt = `avg ${rdev.toFixed(1)}${pkTxt}${rawTxt} kHz · ${
      impossible ? 'over spec — suspect' : low ? 'weak' : strong ? 'generous' : 'nominal'}`;
    rdsDevCol = impossible ? C.bad : low ? C.warn : C.good;
    rdsHold.current = { txt: rdsDevTxt, col: rdsDevCol, at: Date.now() };
  } else if (rdsHold.current && Date.now() - rdsHold.current.at < 4000) {
    // Within the hold window — keep showing what we last actually measured.
    rdsDevTxt = rdsHold.current.txt;
    rdsDevCol = rdsHold.current.col;
  }

  // ── ★★★ RDS-to-pilot phase — the field this panel exists for ────────────────
  // Correct is near 0 or near 90 (quadrature); the middle is a transmitter fault. Reading it
  // takes equipment most people do not have, which is the entire point of showing it.
  const ph = x?.phase ?? -1;
  const coh = x?.phaseCoh ?? 0;
  // ★ Prefer the frame's own BER: it was measured alongside the phase in the same window, so
  // the two agree. The prop is the basic-RDS one, kept as a fallback for an older server.
  const ber = (x && x.ber >= 0) ? x.ber : (p.ber ?? -1);
  let phaseTxt = DASH, phaseCol: string | undefined;
  if (ph < 0 || coh < 0.35) {
    // ★★ A ROTATING CONSTELLATION IS A DIAGNOSIS, NOT A FAILURE. Low coherence with a LOW
    // block error rate means the symbols decode perfectly while the phase sweeps — the
    // encoder is not locked to the pilot. It draws as a clean ring; calling that "noisy"
    // is exactly wrong.
    const rotating = ph >= 0 && coh < 0.35 && ber >= 0 && ber < 20;
    phaseTxt = ph < 0 ? DASH
             : rotating ? 'rotating — encoder not locked to pilot'
                        : 'unstable — not measurable';
    phaseCol = rotating ? C.warn : C.muted;
  } else {
    const d0 = Math.min(ph, 180 - ph);
    const d90 = Math.abs(ph - 90);
    const near = Math.min(d0, d90);
    const drift = x?.phaseDrift ?? 0;
    // ★★ SLOW ROTATION LOOKS PERFECTLY STEADY — coherence only collapses when the phase turns
    // FAST. A station slightly off its pilot keeps coherence high while the angle walks the
    // whole range. The drift RATE is the honest test, and needs no coherence at all: our
    // 57 kHz reference is the station's own pilot tripled, so a locked encoder sits still
    // however weak the signal.
    if (drift >= 2) {
      phaseTxt = `rotating ${drift.toFixed(0)}°/s — encoder not locked to pilot`;
      phaseCol = C.warn;
    } else {
      // ★ FAULT is reserved for genuinely far out AND a solid estimate: asserting that a
      // broadcaster's transmitter is defective deserves the higher bar.
      const verdict = near <= 12 ? (d0 <= d90 ? 'in phase' : 'quadrature')
                    : near <= 40 ? 'off nominal'
                    : coh > 0.7  ? 'FAULT'
                    : 'off nominal';
      // ★ Signed when the server sends it (2026-10-10): + = RDS leads 3 × pilot. The verdict stays on the distance.
      const sg = x?.phaseSigned;
      const shown = sg !== undefined ? `${Math.round(sg) > 0 ? '+' : Math.round(sg) < 0 ? '\u2212' : ''}${Math.abs(Math.round(sg))}°` : `${ph.toFixed(0)}°`;
      phaseTxt = `${shown} · ${verdict} · ${(coh * 100).toFixed(0)}% steady`;
      phaseCol = near <= 12 ? C.good : near <= 40 ? C.warn : C.bad;
    }
  }

  // ── DI — decoder identification, four bits across four groups ───────────────
  const diV = (raw ? x?.diRaw : x?.di) ?? -1;
  let diTxt = DASH;
  if (diV >= 0) {
    const d: string[] = [diV & 1 ? 'Stereo' : 'Mono'];
    if (diV & 2) d.push('Artificial head');
    if (diV & 4) d.push('Compressed');
    if (diV & 8) d.push('Dynamic PTY');
    diTxt = d.join(' · ');
  }

  // ── Clock (4A). ★ Shown as TRANSMITTED with its offset stated, not converted to local —
  // the offset identifies the network's timezone and is information in its own right.
  // ★★ CT arrives ONCE A MINUTE against ~11 groups a second — about one group in 660 — and
  // needs both blocks C and D intact with no repetition to fall back on. So a dash means
  // "not caught yet" far more often than "not transmitted"; saying "waiting" stops the user
  // concluding the station does not send it.
  const ctV = x?.ct ?? -1;
  const g4a = x?.grp?.[8] ?? 0;          // group 4A = index 4*2+0
  let ctTxt = DASH;
  if (ctV < 0) {
    if ((x?.gtot ?? 0) > 0) ctTxt = g4a > 0 ? 'seen, damaged' : 'waiting… (1/min)';
  } else {
    const hh = String(Math.floor(ctV / 60)).padStart(2, '0');
    const mm = String(ctV % 60).padStart(2, '0');
    const off = x!.ctoff;
    ctTxt = `${hh}:${mm} ${off === 0 ? 'UTC' : `UTC${off > 0 ? '+' : '−'}${Math.abs(off) / 2}`}`;
  }

  // ── PIN — the scheduled start of the current programme (1A) ─────────────────
  const pinTxt = (x && x.pinDay > 0)
    ? `day ${x.pinDay} ${String(x.pinHour).padStart(2, '0')}:${String(x.pinMin).padStart(2, '0')}`
    : DASH;

  // ── Country. ★ Say WHY it is blank: the flag logic refuses to guess, so "waiting" is the
  // honest reading rather than a bare dash that looks like a failure.
  /* ★★★ SAY HOW WE KNOW. "from PI" is a claim about PROVENANCE — the country was GUESSED from the
   *  PI code's country nibble — and it was hardcoded, so the row went on saying "guessed" long
   *  after group 1A had delivered the Extended Country Code that settles it properly. The ECC is
   *  what distinguishes the countries that share a PI nibble, so the difference is not cosmetic.
   *  ★ Four states, the same four the browser draws: confirmed by ECC, guessed from PI, an ECC we
   *    hold but cannot match to a country, and nothing yet. */
  const eccV = p.ecc ?? 0;
  const countryTxt = p.countryIso
    ? `${p.countryIso.toUpperCase()} · ${eccV > 0
        ? `ECC ${eccV.toString(16).toUpperCase()}` : 'from PI'}`
    : eccV > 0 ? `ECC ${eccV.toString(16).toUpperCase()} · unmatched`
    // ★ Still gated on groups actually RECEIVED — "waiting for ECC" before a single group has
    //   arrived would blame a missing 1A for what is really a missing signal.
    : (x?.gtot ?? 0) > 0 ? 'waiting for ECC (1A)' : DASH;
  /* ★★ A COUNTRY THAT IS NOT THIS RECEIVER'S IS SAID SO — never "corrected" (Stuart, 2026-10-08, a Ukrainian server
   *  whose local stations send MOROCCO's ECC E2 / PI 1xxx). The flag shows what is transmitted; on a DX catch the far
   *  country is the truth, so it is not guessed away. One neutral note, true either way. */
  const rxIso = receiverIso();
  const countryNote = p.countryIso && rxIso && p.countryIso.toUpperCase() !== rxIso.toUpperCase()
    ? `not this receiver's country (${rxIso.toUpperCase()}) — a long-distance catch, or a station sending the wrong code`
    : '';

  // ── Group share + rate ──────────────────────────────────────────────────────
  const grp = x?.grp ?? [];
  const tot = x?.gtot ?? 0;
  let groupShareTxt = DASH;
  if (tot > 0) {
    const parts: { n: string; pc: number }[] = [];
    for (let i = 0; i < grp.length; i++) {
      if (!grp[i]) continue;
      parts.push({ n: `${i >> 1}${(i & 1) ? 'B' : 'A'}`, pc: Math.round((grp[i] / tot) * 100) });
    }
    parts.sort((a, b) => b.pc - a.pc);
    // ★ SAY WHAT THE PERCENTAGES ARE OF. There are two percentage figures on this panel —
    // this one and the block ERROR RATE — and a bare "0A 40%" gives no clue which it is.
    if (parts.length) groupShareTxt = `of ${tot} groups: ${parts.map(q => `${q.n} ${q.pc}%`).join('  ')}`;
  }

  // ★★ RATE FROM SUCCESSIVE DELTAS, never total-over-elapsed. `gtot` accumulates from when the
  // DECODER started, but the panel opens later — dividing one by the other reported 113/s
  // against a theoretical maximum of 11.4. Two clocks with different origins is not a rate.
  const rateRef = useRef({ tot: 0, at: 0, rate: 0 });
  {
    const now = Date.now();
    const r = rateRef.current;
    if (tot > r.tot && r.at > 0) {
      const dt = (now - r.at) / 1000;
      // ★ A FULL SECOND MINIMUM. At ~5 frames a second, a 0.2s window turns ordinary arrival
      // jitter into rate spikes — it read "11.9/s of 11.4", i.e. faster than the protocol
      // permits, which makes the whole figure look invented. Measure over a longer window.
      if (dt >= 1.0) {
        const inst = (tot - r.tot) / dt;
        r.rate = r.rate > 0 ? r.rate * 0.7 + inst * 0.3 : inst;
        r.tot = tot; r.at = now;
      }
    } else if (tot !== r.tot) { r.tot = tot; r.at = now; }
  }
  const rateTxt = tot > 0
    ? (rateRef.current.rate > 0
        ? `${rateRef.current.rate.toFixed(1)}/s of 11.4 · ${tot} total`
        : `${tot} total`)
    : DASH;

  // ── AF score: confirmed against glimpsed. Below 100% means entries arrive damaged.
  const afSeen = x?.afseen ?? 0;
  const afScoreTxt = afSeen > 0
    ? `${(x?.af.length ?? 0)}/${afSeen} · ${Math.round(((x?.af.length ?? 0) / afSeen) * 100)}%`
    : DASH;

  const verdict = constellationVerdict(x?.xy ?? [], coh, ber);
  const odas = x?.oda ?? [];
  const eons = x?.eon ?? [];
  const afs  = x?.af ?? [];
  // ★★★ THE LIST WITH A TICK EACH, as the browser draws it. The app showed only the CONFIRMED
  //     frequencies as bare numbers, so the one genuinely useful distinction was invisible: on a
  //     noisy station the unconfirmed entries are usually PHANTOMS manufactured by block errors,
  //     and a list that hides which is which invites someone to go hunting for a transmitter that
  //     was never announced. `afAll` has carried the flag since this morning; only the browser
  //     read it (Stuart, 2026-08-15: "the AF doesn't have the ticks like the webclient does").
  // ★★ De-duplicated with confirmation STICKY — the same AF is re-announced constantly and a
  //    later damaged copy must not un-confirm one already believed. Exactly the browser's rule.
  // ★ Falls back to the plain confirmed list when afAll is absent, which is what an older server
  //   sends.
  const afTxt = (() => {
    const all = x?.afAll ?? [];
    if (all.length) {
      const seen = new Map<number, boolean>();
      for (const [khz, ok] of all) seen.set(khz, !!seen.get(khz) || !!ok);
      return [...seen.entries()].sort((a, b) => a[0] - b[0])
        .map(([khz, ok]) => `${(khz / 1000).toFixed(1)}${ok ? ' \u2713' : ''}`).join('  ');
    }
    if (!afs.length) return DASH;
    return [...new Set(afs)].sort((a, b) => a - b)
      .map((khz) => (khz / 1000).toFixed(1)).join('  ');
  })();
  const nowPlaying = [x?.rtpArtist, x?.rtpTitle].filter(Boolean).join(' — ');

  // ★★ A PERCENTAGE HEIGHT NEEDS A PARENT WITH A HEIGHT. This was maxHeight:'46%' on a child
  // of an absolutely-positioned wrap that sets only left/right/bottom — so the percentage had
  // nothing to resolve against and the panel came out small and floating mid-screen
  // (Stuart, 2026-07-27). ★ The `as any` needed to force that string past the type checker was
  // the tell; RN's own types say maxHeight here should be a number.
  // Measure the window and work in pixels, minus the space the panel is anchored above.
  const { width: winW, height: winH } = useWindowDimensions();
  /** ★★ TWO COLUMNS WHEN THERE IS ROOM FOR TWO — a REFLOW, keyed on the width available, never on
   *  the device. Rotate an iPad and it reflows; a phone and a portrait iPad simply never qualify,
   *  so they keep the stacked layout they already had. Same rule the web client's decoder panels
   *  use, and the reason BIG on a Mac was a ~300 pt column of text beside a metre of empty panel
   *  (Stuart, 2026-08-02: "put the graphs on the side like it does on the web client so no
   *  scrolling needed… and then when pushed to portrait the graphs move underneath like they are
   *  on the phone, a proper reflow").
   *  ★ 820 = the fields column (~340) plus the plots (~330) plus padding and the gap. Below that
   *  they would be squeezed rather than placed, which is worse than stacking. */
  const wideCols = p.tall && winW >= 820;
  // ★★ LEAVE THE STATUS BAR ALONE. In BIG mode the panel is anchored at the bottom and grew
  // straight up past the notch, covering the clock and battery (Stuart's screenshot,
  // 2026-07-27). The available height has to stop at the safe area, not at the window edge.
  const insets = useSafeAreaInsets();
  const avail = Math.max(180, winH - p.bottomOffset - insets.top - 16);
  // ★★ SMALL IS A SUMMARY, NOT A SHORTER SCROLL. Both modes used to render every field and the
  // plots, differing only in viewport height — so SMALL still buried the waterfall and BIG
  // "didn't make much difference, just a couple of lines" (Stuart, 2026-07-28). SMALL now shows
  // the seven fields you glance at while tuning and nothing else; BIG is the full instrument.
  const maxH = Math.min(avail, p.tall ? winH * 0.82 : winH * 0.34);

  return (
    <DecoderShell bottom={p.bottomOffset} maxHeight={maxH} maxWidth={wideCols ? 800 : 600}
                  tall={p.tall}>
      {/* ★ The cap is the CONTENT's width, not a guess: the fields column is 340 and the plots
          column ~330, plus the 18 gap and 24 of padding — so ~760 in two columns, and ~560
          stacked where only the fields and the 310-wide symbol trace have to fit. */}
        {/* ★★ THE PANEL HAD NO BLUR AT ALL — a 95%-opaque slab that blanked the waterfall behind
            it. The control island next to it has used BlurView since it was built, which is
            exactly why the two looked like different apps on iOS (Stuart, 2026-07-28). Blur
            plus a light tint: the spectrum reads through it, the text still reads on top. */}
        {/* ★★★ NO BLUR IN BIG — A BACKDROP BLUR OVER THE SPECTRUM IS THE EXPENSIVE CASE, AND IT
            IS THE SPECTRUM SPECIFICALLY, NOT THE AREA. Stuart pinned it exactly: "it's only when
            it covers the spectrum part; in small mode when it's over the waterfall only, no
            issues" (2026-08-02). The two are drawn by completely different means and the blur
            costs accordingly:
              • the WATERFALL is a Shader/ImageShader — a GPU texture blit, nearly free to resample;
              • the SPECTRUM is a Skia vector Path REBUILT EVERY FRAME (buildSpecPathRef →
                Skia.Path.Make) and filled.
            A backdrop blur forces whatever sits beneath it into an offscreen buffer to sample. Over
            a texture that is cheap; over a live per-frame vector path it is not, and at 20 fps it
            stutters — on an M4, which is the tell that this is a compositing cost and not raw GPU
            power. Stuart: "I'd like to hope that the glass effect of the box isn't causing a GPU
            issue on an M4 MacBook."
            ★★ AND IT IS WHY SLOWING THE RDS PAYLOAD DID NOT FIX THIS. The blur redraws because the
            content BENEATH it moved, not because the analyser updated — so the analyser's rate was
            never the lever. That rate was genuinely wrong and worth fixing on its own; it was not
            this bug, and I said it was.
            ★ BIG is exactly the mode that reaches up over the spectrum; SMALL sits over the
            waterfall alone and has never stuttered. So SMALL keeps its blur — the original
            complaint that produced it was a 0.95 slab blanking the waterfall behind that strip —
            and BIG, where you are reading an instrument rather than watching through it, takes a
            near-solid tint instead. Platform.OS is 'ios' on a Mac, which is how the Mac got here. */}
        {/* ★ Blur and tint are DecoderShell's now, from TRANSPARENCY EFFECTS: with it ON, 35 in
            SMALL and 0 in BIG, iOS only — never on silver / black, and never with it OFF. */}
        <DecoderHeader>
          {/* ★ Matches the button that opens it — an abbreviation in one place and the full
              name in the other reads as two different features. */}
          <DecoderTitle>ADVANCED RDS</DecoderTitle>
          <View style={{ flex: 1 }} />
          {/* ★ RAW REMOVED 2026-07-28. It showed the UNCONFIRMED value for five block-B
              fields (PTY/TP/TA/MS/DI) and coloured those labels by confirmation state —
              but in the field the raw and confirmed values were always identical, so the
              data never changed and only three labels ever went green (Stuart). A
              control that appears to do nothing is worse than no control.
              ★ The server still sends both sets, so this is a UI removal: reinstating it
              means making it cover more than five fields, not re-adding a button. */}
          {/* ★ Say what it DOES. A bare caret read as decoration, and while the panel height
              was broken it also appeared to do nothing at all. */}
          <DecoderKey active={p.tall} onPress={() => p.onTall(!p.tall)} hitSlop={6}
                      label={p.tall ? 'SMALL' : 'BIG'} />
          <DecoderKey tone="close" onPress={p.onClose} hitSlop={8} label="✕" />
        </DecoderHeader>

        <DecoderBody>
        <ScrollView contentContainerStyle={[s.body, scrollLane, wideCols && s.bodyWide]}>
          <View style={wideCols ? s.colFields : undefined}>
          {/* ★ RAW mode needs saying, not just showing — a panel full of red labels with no
              explanation reads as breakage rather than as "arriving but not yet trusted". */}
          {raw && (
            <Text style={s.rawNote}>
              RAW — unconfirmed values, live. Red labels have not yet been confirmed by
              repetition; treat them with a pinch of salt.
            </Text>
          )}

          {/* ★★ THE LABELS AND THE VALUES MATCH THE WEB CLIENT EXACTLY, field for field and in
              its order. Two clients describing the same decoder differently is worse than one
              of them being sparse: a DXer comparing a phone against a laptop on the same
              station cannot tell a real difference from a naming difference. */}
          {/* ★ Hex AND decimal, as the browser has always shown it: the databases and
              lists DXers actually use are split between the two notations, so printing one
              forces a conversion by hand at the very moment someone is logging a catch. */}
          <Row raw={raw} label="PI"
               value={p.pi ? `${p.pi} · ${piNum}` : DASH} />
          <Row raw={raw} label="Station"     value={p.ps || DASH} />
          <Row raw={raw} label="Type"        value={ptyTxt}
               conf={(x?.pty ?? -1) >= 0} />
          <Row raw={raw} label="Flags"       value={flags.length ? flags.join(' · ') : DASH}
               conf={(x?.tp ?? -1) >= 0 || (x?.ms ?? -1) >= 0} />
          {/* Block error rate, before correction, last 12 groups. */}
          <Row raw={raw} label="Errors"      value={ber >= 0 ? `${ber}%` : DASH} />
          {/* ★ The two deviations stay in SMALL: they are the readings you watch while tuning,
              not reference material (Stuart, 2026-07-28). */}
          <Row raw={raw} label="Pilot dev"   value={pilotTxt} colour={pilotCol} />
          <Row raw={raw} label="RDS dev"     value={rdsDevTxt} colour={rdsDevCol} />
          {/* ★★ THE WEAK-SIGNAL READINGS, beside the deviations because they are the same KIND of
              thing: numbers a listener judges a marginal signal by while judging it by ear. MPX S/N
              stays in SMALL with the deviations — it is the one you watch while tuning — and the
              rest are reference material, so they sit in the tall panel below. */}
          <Row raw={raw} label="MPX S/N"     value={snrTxt} colour={snrCol}
               reserve="22 dB · blend 12.6k · cut 11.2k" />
          <Row raw={raw} label="RadioText"   value={p.rt || DASH} />
          <Row raw={raw} label="Rate"        value={rateTxt} />

          {/* ── Everything below is BIG only. ───────────────────────────────────────── */}
          {p.tall && <>
          {/* ★ U+21D4 ⇔, NOT U+2194 ↔. The latter has EMOJI presentation by default and iOS
              rendered it as a blue tile mid-label (Stuart's screenshot, 2026-07-28). U+21D4
              has no emoji form, so it stays a glyph on every platform without a variation
              selector to get lost in a copy-paste. */}
          <Row raw={raw} label="RDS⇔pilot"   value={phaseTxt} colour={phaseCol}
               reserve="rotating 000°/s — encoder not locked to pilot" />
          {/* ★ Multipath is a REFLECTION, not weakness — a strong station can show it, and unlike
              noise it is not cured by narrowing. Worth knowing which fault you are hearing. */}
          {/* ★★ THE RESERVE IS THE TRUE WORST CASE, not a guess (Stuart, 2026-09-28: fixed heights stop the
              box pulsing, "but it needs to have enough room to display its longest message"). This was
              "too noisy to judge" — a third of the real longest, so the value overprinted CEQ. Longest
              base ("NN.N% · moderate · held", padded to 100.0%) + longest IMS clause. If a clause is
              added to mpTxt, add it here too. */}
          <Row raw={raw} label="Multipath"   value={mpTxt} colour={mpCol}
               reserve="100.0% · moderate · held · IMS standing by · NR already blending further" />
          <Row raw={raw} label="CEQ"         value={ceqTxt} colour={ceqCol}
               reserve="standing by · nothing to correct" />
          <Row raw={raw} label="Blanker"     value={nbTxt} reserve="nothing to blank" />
          <Row raw={raw} label="IF narrow"   value={ifTxt} colour={ifCol}
               reserve="999k narrow · wide would cost 99.9 dB" />
          <Row raw={raw} label="Now playing" value={nowPlaying || DASH} />
          <Row raw={raw} label="Long PS"     value={x?.longPs || DASH} />
          <Row raw={raw} label="PTYN"        value={x?.ptyn || DASH} />
          <Row raw={raw} label="Language"    value={x?.lang ? (LANGS[x.lang] ?? `code ${x.lang}`) : DASH} />
          <Row raw={raw} label="PIN"         value={pinTxt} />
          <Row raw={raw} label="ODA"         value={odas.length
            ? odas.map(o => `${ODA_NAMES[o.aid] ?? o.aid} in ${o.grp >> 1}${(o.grp & 1) ? 'B' : 'A'}`).join(', ')
            : DASH} />
          {/* EON — the sister stations. TA on one of them is why a car radio switches over. */}
          <Row raw={raw} label="Other networks" value={eons.length
            ? eons.map(e => {
                const ps = e.ps.trim();
                const f = e.af ? ` ${(e.af / 1000).toFixed(1)}` : '';
                return `${ps || e.pi}${f}${e.ta === 1 ? ' [TA]' : ''}`;
              }).join('  ')
            : DASH} />
          {/* DI — four single bits spread across four groups, so one bad group could once set
              a flag for the whole session. In RAW you can watch them flicker: a genuine flag
              sits steady across hundreds of groups, corruption does not. */}
          <Row raw={raw} label="DI"          value={diTxt} conf={(x?.di ?? -1) >= 0} />
          <Row raw={raw} label="Clock"       value={ctTxt} />
          <Row raw={raw} label="Country"     value={countryNote ? `${countryTxt} · ${countryNote}` : countryTxt} />
          {!!p.call && <Row raw={raw} label="Call" value={`${p.call} · from the PI (US)`} />}
          <Row raw={raw} label="PI detail"   value={piNum > 0
            ? `${COV[(piNum >> 8) & 0xF]} · ref ${piNum & 0xFF} · cc ${(piNum >> 12) & 0xF}`
            : DASH} />
          {/* ★ Rate is shown in SMALL too — it moved up with the other essentials. */}
          {/* ★ AF score and AF MHz are DEAD FIELDS in the web client — the markup is there but
              nothing ever fills them, so they show a permanent dash. The data is already on
              the wire (af[] and afseen), so they are populated properly here. */}
          <Row raw={raw} label="AF score"    value={afScoreTxt} />
          <Row raw={raw} label="AF MHz"      value={afTxt} />
          <Row raw={raw} label="Group share" value={groupShareTxt} />

          </>}
          </View>

          {/* ★ The plots are their own column when wide, and simply the next thing down the page
              when not. Splitting them out of the BIG-only fragment above is what makes both
              layouts expressible without duplicating the field list. */}
          {p.tall && (
          <View style={wideCols ? s.colPlots : undefined}>
          <Text style={s.section}>PLOTS</Text>
          <View style={s.plots}>
            <View>
              <Text style={s.plotLbl}>CONSTELLATION</Text>
              <Constellation xy={plotXy} size={120} />
              <Text style={[s.verdict, { color: verdict.colour }]}>{verdict.text}</Text>
            </View>
            <View style={{ flex: 1 }}>
              <Text style={s.plotLbl}>MPX</Text>
              <Mpx mpx={plotMpx} width={180} height={72} />
              <Text style={[s.plotLbl, { marginTop: 4 }]}>SCOPE VIEW · PILOT · STEREO · RDS</Text>
              {scopeBands.map((b) => (
                <View key={b.name} style={{ marginBottom: 3 }}>
                  <ScopeBox grid={b.g} ew={x?.eyeW ?? 0} eh={x?.eyeH ?? 0} width={180} height={40} colour={b.colour} />
                  <Text style={[s.scopeLbl, { color: `rgb(${b.colour[0]},${b.colour[1]},${b.colour[2]})` }]}>
                    {b.name}{b.khz > 0.05 ? `  ${b.khz.toFixed(1)} kHz` : ''}
                  </Text>
                </View>
              ))}
              <Text style={[s.verdict, { color: eyeVerdict.c, minHeight: 30 }]}>
                {eyeVerdict.t}
              </Text>
              {/* ★★★ WHAT EACH PART OF THIS BAR ACTUALLY IS — the old note here claimed the FILL
                  was the peak hold, which it never was, and a stale design note describing a meter
                  we did not have is exactly why the under-read survived three releases
                  ([[BRIEF-deviation-meter]], 2026-09-25):
                    • the FILL  = mpxDev, the fast true peak (instant attack, 0.9 s decay);
                    • the TICK  = mpxHold, the 6 s excursion memory — also what the digits quote;
                    • the LINE at 75 % = the limit.
                  0-100 kHz, so 1 kHz = 1 %. Drawn dim when the reading is not trusted. */}
              <View style={{ width: 180, height: 8, marginTop: 4, borderRadius: 2,
                             backgroundColor: C.devBarBg,
                             borderWidth: 1, borderColor: C.devBarBorder,
                             overflow: 'hidden' }}>
                <View style={{ position: 'absolute', left: 0, top: 0, bottom: 0,
                               width: `${mpxDevInfo.pct}%`, backgroundColor: mpxDevInfo.c }} />
                {/* ★ The AVERAGE mark — quieter than the hold tick, because the hold is the thing
                    you are meant to notice and this is context. Matches the web client's bar. */}
                {mpxDevInfo.avgPct > 0.5 && (
                  <View style={{ position: 'absolute', top: 0, bottom: 0, width: 2,
                                 left: `${mpxDevInfo.avgPct}%`, backgroundColor: 'rgba(255,255,255,0.45)' }} />
                )}
                <View style={{ position: 'absolute', top: 0, bottom: 0, width: 2,
                               left: `${mpxDevInfo.hold}%`, backgroundColor: '#fff' }} />
                <View style={{ position: 'absolute', top: 0, bottom: 0, width: 1,
                               left: '75%', backgroundColor: 'rgba(255,255,255,0.55)' }} />
              </View>
              {/* ★★ FIXED COLUMNS: a right-aligned key and a value column wide enough for the
                  longest reading, with tabular figures so a 1 is not narrower than a 0. The
                  verdict keeps its own line so a long one wraps under the numbers instead of
                  pushing them sideways. */}
              <View style={s.devGrid}>
                <Text style={[s.devKey, { color: mpxDevInfo.c }]}>Deviation   Average:</Text>
                <Text style={[s.devVal, { color: mpxDevInfo.c }]}>{mpxDevInfo.avg}</Text>
              </View>
              <View style={s.devGrid}>
                <Text style={[s.devKey, { color: mpxDevInfo.c }]}>Peak:</Text>
                <Text style={[s.devVal, { color: mpxDevInfo.c }]}>{mpxDevInfo.peak}</Text>
              </View>
              <Text style={[s.verdict, s.devVerdict, { color: mpxDevInfo.c, minHeight: 15 }]}>
                {mpxDevInfo.verdict}
              </Text>
              {/* ★★ MPX POWER (ITU-R BS.412) — the 60 s mean power of the whole multiplex against a
                  ±19 kHz sine, the figure MPXtool shows as "Power" (2026-10-02). Neutral colour: above
                  0 dB is over the BS.412 limit, a fact about the STATION, never a receiver fault. The
                  seconds show until the minute is full. Same words as the web panel. */}
              <View style={s.devGrid}>
                <Text style={[s.devKey, { color: C.rowLabel }]}>MPX power:</Text>
                <Text style={[s.devVal, s.powVal, { color: C.rowLabel }]}>{mpxPowTxt.value}</Text>
              </View>
              {/* ★ The countdown to a full 60 s mean, on its own line — services/mpxPower.ts. */}
              {!!mpxPowTxt.settling && (
                <Text style={[s.verdict, s.devVerdict, { color: C.rowLabel, opacity: 0.75, marginTop: 1 }]}>
                  {mpxPowTxt.settling}
                </Text>
              )}
            </View>
          </View>
          {/* ★ ONE SYMBOL PLOT, full width — the web's EYE. The app carried this AND a second
              "SYMBOL TRACE" of the same data (Stuart, 2026-09-14: "there is 2 symbol trace
              lines"); the wide one stays. Two clean bands = every bit decided with margin. */}
          {/* ★ Its name is SYMBOL TRACE — what the website and the About note call it. It became "SYMBOL EYE"
              when the duplicate went (above), and Stuart could not remember the original (2026-09-19). */}
          <Text style={s.plotLbl}>SYMBOL TRACE</Text>
          <Eye xy={plotXy} width={310} height={70} />
          <Text style={s.plotNote}>
            Two clear bands = every bit decided with margin. A filled gap means symbols are
            landing on the decision line, and the errors follow.
          </Text>

          {!!p.ps && (
            <View style={s.logoWrap}>
              <StationLogo name={p.ps} itu={p.countryIso} size={72} uri={p.logoUri} />
            </View>
          )}
          </View>
          )}
        </ScrollView>
        </DecoderBody>
    </DecoderShell>
  );
}

type Palette = ReturnType<typeof palette>;
/** The panel's colours. ★ On the default chassis the plots keep their exact literals (§13.1: today's
 *  look); on silver / black they take the controls colour (§10.2 "charts take the controls colour").
 *  The eye's per-component colours (ScopeBox `colour`) are VERDICTS and stay as they are. */
const palette = (T: DecoderTokens) => {
  const def = T.chassis === 'default';
  return {
    ...DECODER_MEANING,
    goldDim:  T.label,
    muted:    T.muted,
    rowLabel: T.rowLabel,
    value:    T.value,
    plot:     T.plot,
    axis:     T.axis,
    axisStrong: T.axisStrong,
    axisFaint:  T.axisFaint,
    chartText:  T.chartText,
    mark:       def ? 'rgba(255,170,60,0.30)' : T.axisStrong,
    markFaint:  def ? 'rgba(255,160,60,0.18)' : T.axis,
    dot:      T.dot,
    trace:    T.trace,
    mpxLine:  def ? DECODER_MEANING.good : T.trace,
    note:     def ? 'rgba(255,190,90,0.80)' : T.label,
    devBarBg:     def ? 'rgba(255,160,0,0.10)' : T.rowActive,
    devBarBorder: def ? 'rgba(255,160,0,0.25)' : T.axis,
  };
};

/** ★ Built once per setting (useDecoderStyles), never per render. */
const makeStyles = (T: DecoderTokens) => { const C: Palette = palette(T); const s = StyleSheet.create({
  // ★★ CENTRED, AND CAPPED BY ITS CONTENT — the panel used to stretch to whatever was available,
  //   so on a Mac it was a column of text against a metre of empty box (Stuart: "needs to be
  //   centre justified so the box doesn't end up huge like this"). Same rule as the control
  //   island: content decides the width, the window only decides whether it fits.
  /* ★ Wrap, frame, header, title and header keys are DecoderShell's. */
  body:  { paddingHorizontal: 12, paddingVertical: 8, gap: 3 },
  // Wide: fields on the left at a FIXED width, plots beside them taking the rest.
  // ★★ EXPLICIT WIDTH, NOT flexBasis + flexShrink. The first attempt used
  // `flexBasis: 340, flexShrink: 1` for the fields and `flexGrow: 1` for the plots, and in build
  // 51 the fields column collapsed to ~130 pt — every value wrapped onto three lines — while both
  // columns still left dead space to the right, so neither the basis nor the grow resolved as
  // intended. A definite width cannot be shrunk out from under the text, and `flex: 1` on the
  // other column has one obvious meaning: take what is left.
  // ★ Grew 340 → 372 with the type. A column sized for the OLD font would have wrapped the long
  //   values ("Other networks", the AF list) onto extra lines, which costs more height than the
  //   larger text gains in legibility — and the label column went 96 → 108 for the same reason,
  //   so "Other networks" still fits on one line.
  bodyWide:  { flexDirection: 'row', alignItems: 'flex-start', gap: 18 },
  colFields: { width: 372, gap: 3 },
  colPlots:  { flex: 1, minWidth: 320 },
  rawNote: { fontFamily: FONT, fontSize: 11, color: C.warn, marginBottom: 6, lineHeight: 15 },
  row:   { flexDirection: 'row', alignItems: 'flex-start', gap: 8 },
  // ★ The label column is FIXED and the value WRAPS. Letting the label wrap instead was what
  // pushed text off the panel in the browser and made the whole thing scroll sideways.
  lbl:   { fontFamily: FONT, fontSize: 11, letterSpacing: 1, color: C.rowLabel, width: 112 },
  val:   { fontFamily: FONT, fontSize: 13, color: C.value, flex: 1, fontVariant: ['tabular-nums'] },
  section: { fontFamily: FONT, fontSize: 10, letterSpacing: 2, color: C.goldDim,
             marginTop: 10, marginBottom: 2 },
  plots:   { flexDirection: 'row', gap: 10, marginTop: 4 },
  plotLbl: { fontFamily: FONT, fontSize: 10, letterSpacing: 1, color: C.muted, marginBottom: 2, marginTop: 8 },
  scopeLbl: { fontFamily: FONT, fontSize: 10, letterSpacing: 1, marginTop: 1 },
  verdict: { fontFamily: FONT, fontSize: 12, marginTop: 3 },
  /* ★ One row of the deviation readout: key right-aligned in a fixed column, value left-aligned
     in another, so neither moves when the other changes width. */
  devGrid: { flexDirection: 'row' as const, alignItems: 'baseline' as const, marginTop: 2 },
  /* ★ The verdict is CENTRED across the figures' block (key 132 + gap 6 + value 62), as the web panel's
   *  #rdsMpxVerdict is — left-aligned it sat at the panel edge, out of line with every figure around it
   *  (Stuart, 2026-10-02: "it's the nominal above it that isn't"). A long verdict wraps inside the block. */
  devVerdict: { width: 200, textAlign: 'center' as const },
  devKey:  { fontFamily: FONT, fontSize: 12, width: 132, textAlign: 'right' as const, opacity: 0.8 },
  devVal:  { fontFamily: FONT, fontSize: 12, width: 62, textAlign: 'right' as const,
             marginLeft: 6, fontVariant: ['tabular-nums'] as const },
  // ★ "+6.1 dB (24 s)" is wider than a kHz figure; it grows rightwards so the numbers above stay put.
  powVal:  { width: undefined, minWidth: 62, textAlign: 'left' as const },
  // ★ BRIGHTER THAN THE LABELS, deliberately. This is the sentence that TEACHES the plot — "two
  //   clear bands = every bit decided with margin" — so it is prose to be read, not a caption to
  //   be glanced at, and it is the longest run of small text sitting over a live waterfall.
  //   Stuart asked for the titles and then "including the text about the constellation etc".
  plotNote: { fontFamily: FONT, fontSize: 12, color: C.note, marginTop: 4, lineHeight: 16 },
  logoWrap: { alignItems: 'center', marginTop: 10 },
}); return { s, C }; };
