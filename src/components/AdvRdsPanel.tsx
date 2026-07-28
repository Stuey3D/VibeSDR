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

import React, { useMemo, useRef } from 'react';
import { ScrollView, StyleSheet, Text, TouchableOpacity, useWindowDimensions, View } from 'react-native';
import { Canvas, Circle, Path, Rect, Skia, Text as SkText, matchFont } from '@shopify/react-native-skia';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import type { RdsExt } from '../services/UberSDRClient';
import StationLogo from './StationLogo';

const C = {
  bg:      'rgba(10,8,4,0.95)',
  border:  'rgba(255,160,0,0.28)',
  gold:    '#ffb833',
  goldDim: 'rgba(255,160,0,0.70)',
  muted:   'rgba(255,160,0,0.38)',
  hdrBdr:  'rgba(255,160,0,0.12)',
  btnBdr:  'rgba(255,160,0,0.28)',
  btnAct:  'rgba(255,160,0,0.12)',
  good:    '#7dff9a',
  warn:    '#ffd479',
  bad:     '#ff8a7d',
  value:   '#ffe566',
  closeCl: 'rgba(255,100,100,0.70)',
};
const FONT = 'Atkinson Hyperlegible';
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
  x: RdsExt | null;
  /** Basic RDS, which arrives on its own message and is shown by the VTS bar too. */
  ps?: string; rt?: string; pi?: string; ber?: number; countryIso?: string;
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
function Row({ label, value, colour, conf, raw, reserve }: {
  label: string; value: string; colour?: string; conf?: boolean; raw: boolean;
  /** ★ The LONGEST string this row can ever show. Rendered invisibly underneath to
   *  reserve the height, so the row cannot change size when the value does. */
  reserve?: string;
}) {
  const lblCol = raw && conf !== undefined ? (conf ? C.good : C.bad) : C.muted;
  return (
    <View style={s.row}>
      <Text style={[s.lbl, { color: lblCol }]} numberOfLines={1}>{label}</Text>
      {reserve ? (
        // ★★ RDS↔PILOT ALTERNATES BETWEEN A SHORT AND A VERY LONG VALUE — "27° ·
        // nominal · 98% steady" versus "rotating 2°/s — encoder not locked to pilot"
        // — and it updates several times a second, so the panel jolted every time it
        // flipped (Stuart). Truncating was the wrong fix: that message IS the
        // diagnosis the field exists to deliver. So reserve the worst case and lay
        // the live value over it — correct at any width, unlike a fixed height.
        <View style={{ flex: 1 }}>
          <Text style={[s.val, { opacity: 0 }]}>{reserve}</Text>
          <Text style={[s.val, colour ? { color: colour } : null,
                        { position: 'absolute', left: 0, right: 0, top: 0 }]}>{value}</Text>
        </View>
      ) : (
        <Text style={[s.val, colour ? { color: colour } : null]}>{value}</Text>
      )}
    </View>
  );
}

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
  if (n < 8) return { text: 'no lock', colour: C.bad };
  const meanAbsX = sumAbsX / n;
  if (meanAbsX < 1) return { text: 'no lock', colour: C.bad };
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
function Constellation({ xy, size }: { xy: number[]; size: number }) {
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
      <Rect x={0} y={0} width={size} height={size} color="rgba(255,160,0,0.05)" />
      <Rect x={size / 2 - 0.5} y={0} width={1} height={size} color="rgba(255,160,0,0.18)" />
      <Rect x={0} y={size / 2 - 0.5} width={size} height={1} color="rgba(255,160,0,0.18)" />
      {pts.map((p, i) => (
        <Circle key={i} cx={p.x} cy={p.y} r={1.2} color="rgba(125,255,154,0.75)" />
      ))}
    </Canvas>
  );
}

/** ★★ THE SYMBOL TRACE — the "two lines" read, and the one most people find easier than the
 *  constellation. Symbol value against time: two clean bands with a clear gap means every bit
 *  is being decided with margin; a filled gap means bits are landing near the threshold, and
 *  the block errors follow. Same de-rotation and scale as the constellation — |x| after
 *  de-rotation is the wanted component. */
function SymbolTrace({ xy, width, height }: { xy: number[]; width: number; height: number }) {
  const pts = useMemo(() => {
    const out: { x: number; y: number }[] = [];
    if (xy.length < 4) return out;
    const rot = constellationAngle(xy);
    const cr = Math.cos(rot), sr = Math.sin(rot);
    const k = constellationScale(xy, height) * 0.9;   // same scale, a touch of headroom
    const mid = height / 2;
    const n = xy.length / 2;
    for (let i = 0; i < n; i++) {
      const x = xy[i * 2] * cr - xy[i * 2 + 1] * sr;
      out.push({ x: (i / (n - 1)) * (width - 2) + 1, y: mid - x * k });
    }
    return out;
  }, [xy, width, height]);
  return (
    <Canvas style={{ width, height }}>
      <Rect x={0} y={0} width={width} height={height} color="rgba(255,160,0,0.05)" />
      {/* The decision threshold — the line a symbol must not stray across. */}
      <Rect x={0} y={height / 2 - 0.5} width={width} height={1} color="rgba(255,160,60,0.35)" />
      {pts.map((p, i) => (
        <Circle key={i} cx={p.x} cy={p.y} r={0.9} color="rgba(120,255,140,0.85)" />
      ))}
    </Canvas>
  );
}

/** ★ THE MPX SPECTRUM — everything the FM demodulator produces, DC to 100 kHz.
 *  ★★ LABELLED AT THE LANDMARKS, because a spectrum of a signal most listeners have never seen
 *  plotted is otherwise just a wiggly line: L+R audio at the bottom, the 19 kHz PILOT, the L−R
 *  stereo sidebands at 38 kHz, and RDS at 57 kHz. Without them the plot cannot be read at all,
 *  which is how it shipped on the phone first time round. */
const MPX_SPAN = 100000;
const MPX_MARKS: [number, string][] = [[19000, 'PILOT'], [38000, 'L−R'], [57000, 'RDS']];
const mpxFont = matchFont({ fontFamily: 'monospace', fontSize: 8 });

function Mpx({ mpx, width, height }: { mpx: number[]; width: number; height: number }) {
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
      <Rect x={0} y={0} width={width} height={height} color="rgba(255,160,0,0.05)" />
      {/* L+R occupies DC..15 kHz — a band rather than a line, so shade it. */}
      <Rect x={0} y={10} width={(15000 / MPX_SPAN) * width} height={height - 10}
            color="rgba(255,170,60,0.07)" />
      {mpxFont && <SkText x={2} y={8} text="L+R" font={mpxFont} color="rgba(255,190,110,0.85)" />}
      {MPX_MARKS.map(([hz, label]) => {
        const x = (hz / MPX_SPAN) * width;
        return (
          <React.Fragment key={label}>
            <Rect x={x - 0.5} y={10} width={1} height={height - 10} color="rgba(255,170,60,0.30)" />
            {mpxFont && <SkText x={Math.min(width - 26, x + 2)} y={8} text={label}
                                font={mpxFont} color="rgba(255,190,110,0.85)" />}
          </React.Fragment>
        );
      })}
      <Path path={path} color={C.good} style="stroke" strokeWidth={1.2} />
    </Canvas>
  );
}

export default function AdvRdsPanel(p: AdvRdsPanelProps) {
  const { x, raw } = p;
  const piNum = p.pi ? parseInt(p.pi, 16) : 0;

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
  let pilotTxt = DASH, pilotCol: string | undefined;
  if (pdev > 0.2) {
    const ok = pdev >= 6.0 && pdev <= 7.5;
    pilotTxt = `${pdev.toFixed(1)} kHz · ${ok ? 'nominal' : pdev < 6 ? 'low' : 'high'}`;
    pilotCol = ok ? C.good : C.warn;
  }
  let rdsDevTxt = DASH, rdsDevCol: string | undefined;
  if (rdev > 0.2) {
    // ★★ THE SCALE HAS A CEILING, SO THE LABELS MUST TOO. 7.5% of 75 kHz = 5.6 kHz is the
    // spec maximum; a reading past it is evidence of a MEASUREMENT problem, never of a
    // strong subcarrier, and must not be dressed up as good news.
    const impossible = rdev > 5.8, strong = rdev >= 4.0, low = rdev < 1.5;
    rdsDevTxt = `${rdev.toFixed(1)} kHz · ${
      impossible ? 'over spec — suspect' : low ? 'weak' : strong ? 'generous' : 'typical'}`;
    rdsDevCol = impossible ? C.bad : low ? C.warn : C.good;
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
      phaseTxt = `${ph.toFixed(0)}° · ${verdict} · ${(coh * 100).toFixed(0)}% steady`;
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
  const countryTxt = p.countryIso
    ? `${p.countryIso.toUpperCase()} · from PI`
    : (x?.gtot ?? 0) > 0 ? 'waiting for ECC (1A)' : DASH;

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
  const nowPlaying = [x?.rtpArtist, x?.rtpTitle].filter(Boolean).join(' — ');

  // ★★ A PERCENTAGE HEIGHT NEEDS A PARENT WITH A HEIGHT. This was maxHeight:'46%' on a child
  // of an absolutely-positioned wrap that sets only left/right/bottom — so the percentage had
  // nothing to resolve against and the panel came out small and floating mid-screen
  // (Stuart, 2026-07-27). ★ The `as any` needed to force that string past the type checker was
  // the tell; RN's own types say maxHeight here should be a number.
  // Measure the window and work in pixels, minus the space the panel is anchored above.
  const { height: winH } = useWindowDimensions();
  // ★★ LEAVE THE STATUS BAR ALONE. In BIG mode the panel is anchored at the bottom and grew
  // straight up past the notch, covering the clock and battery (Stuart's screenshot,
  // 2026-07-27). The available height has to stop at the safe area, not at the window edge.
  const insets = useSafeAreaInsets();
  const avail = Math.max(180, winH - p.bottomOffset - insets.top - 16);
  // ★ Standard is generous on purpose: the plots are the point of this panel, and at 46% they
  // sat below the fold with nothing on screen to suggest scrolling.
  const maxH = Math.min(avail, p.tall ? winH * 0.82 : winH * 0.58);

  return (
    <View style={[s.wrap, { bottom: p.bottomOffset }]}>
      <View style={[s.inner, { maxHeight: maxH }]}>
        <View style={s.header}>
          <Text style={s.title}>ADV RDS</Text>
          <View style={{ flex: 1 }} />
          <TouchableOpacity onPress={() => p.onRaw(!raw)}
            style={[s.hbtn, raw && s.hbtnActive]}>
            <Text style={[s.hbtnTxt, raw && s.hbtnTxtActive]}>RAW</Text>
          </TouchableOpacity>
          {/* ★ Say what it DOES. A bare caret read as decoration, and while the panel height
              was broken it also appeared to do nothing at all. */}
          <TouchableOpacity onPress={() => p.onTall(!p.tall)}
                            style={[s.hbtn, p.tall && s.hbtnActive]} hitSlop={6}>
            <Text style={[s.hbtnTxt, p.tall && s.hbtnTxtActive]}>{p.tall ? 'SMALL' : 'BIG'}</Text>
          </TouchableOpacity>
          <TouchableOpacity onPress={p.onClose} style={s.hbtn} hitSlop={8}>
            <Text style={[s.hbtnTxt, { color: C.closeCl }]}>✕</Text>
          </TouchableOpacity>
        </View>

        <ScrollView contentContainerStyle={s.body}>
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
          <Row raw={raw} label="PI"          value={p.pi ?? DASH} />
          <Row raw={raw} label="Station"     value={p.ps || DASH} />
          <Row raw={raw} label="Type"        value={ptyTxt}
               conf={(x?.pty ?? -1) >= 0} />
          <Row raw={raw} label="Flags"       value={flags.length ? flags.join(' · ') : DASH}
               conf={(x?.tp ?? -1) >= 0 || (x?.ms ?? -1) >= 0} />
          {/* Block error rate, before correction, last 12 groups. */}
          <Row raw={raw} label="Errors"      value={ber >= 0 ? `${ber}%` : DASH} />
          <Row raw={raw} label="Pilot dev"   value={pilotTxt} colour={pilotCol} />
          <Row raw={raw} label="RDS dev"     value={rdsDevTxt} colour={rdsDevCol} />
          <Row raw={raw} label="RDS↔pilot"   value={phaseTxt} colour={phaseCol}
               reserve="rotating 00°/s — encoder not locked to pilot" />
          <Row raw={raw} label="RadioText"   value={p.rt || DASH} />
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
          <Row raw={raw} label="Country"     value={countryTxt} />
          <Row raw={raw} label="PI detail"   value={piNum > 0
            ? `${COV[(piNum >> 8) & 0xF]} · ref ${piNum & 0xFF} · cc ${(piNum >> 12) & 0xF}`
            : DASH} />
          <Row raw={raw} label="Rate"        value={rateTxt} />
          {/* ★ AF score and AF MHz are DEAD FIELDS in the web client — the markup is there but
              nothing ever fills them, so they show a permanent dash. The data is already on
              the wire (af[] and afseen), so they are populated properly here. */}
          <Row raw={raw} label="AF score"    value={afScoreTxt} />
          <Row raw={raw} label="AF MHz"      value={afs.length
            ? afs.map(a => (a / 1000).toFixed(1)).join('  ') : DASH} />
          <Row raw={raw} label="Group share" value={groupShareTxt} />

          <Text style={s.section}>PLOTS</Text>
          <View style={s.plots}>
            <View>
              <Text style={s.plotLbl}>CONSTELLATION</Text>
              <Constellation xy={x?.xy ?? []} size={120} />
              <Text style={[s.verdict, { color: verdict.colour }]}>{verdict.text}</Text>
            </View>
            <View style={{ flex: 1 }}>
              <Text style={s.plotLbl}>MPX</Text>
              <Mpx mpx={x?.mpx ?? []} width={180} height={120} />
            </View>
          </View>
          {/* ★ THE SYMBOL TRACE, full width — the "two lines" read, and the one most people
              find easier than the constellation. It was missing entirely (Stuart, 2026-07-27),
              which mattered because it is the plot that actually explains the error rate:
              two clean bands = every bit decided with margin, a filled gap = bits landing on
              the threshold and the block errors that follow. */}
          <Text style={s.plotLbl}>SYMBOL TRACE</Text>
          <SymbolTrace xy={x?.xy ?? []} width={310} height={80} />
          <Text style={s.plotNote}>
            Two clear bands = every bit decided with margin. A filled gap means symbols are
            landing on the decision line, and the errors follow.
          </Text>

          {!!p.ps && (
            <View style={s.logoWrap}>
              <StationLogo name={p.ps} itu={p.countryIso} size={72} />
            </View>
          )}
        </ScrollView>
      </View>
    </View>
  );
}

const s = StyleSheet.create({
  wrap:  { position: 'absolute', left: 8, right: 8, zIndex: 200 },
  inner: {
    backgroundColor: C.bg,
    borderWidth: 1, borderColor: C.border, borderRadius: 14,
    shadowColor: '#000', shadowOffset: { width: 0, height: 4 },
    shadowOpacity: 0.80, shadowRadius: 14, elevation: 16,
    overflow: 'hidden',
  },
  header: {
    flexDirection: 'row', alignItems: 'center', gap: 6,
    paddingHorizontal: 12, paddingVertical: 8,
    borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: C.hdrBdr,
  },
  title: { fontSize: 10, letterSpacing: 2, color: C.goldDim, fontFamily: FONT },
  hbtn:  { borderWidth: 1, borderColor: C.btnBdr, borderRadius: 4,
           paddingHorizontal: 8, paddingVertical: 3 },
  hbtnActive:    { backgroundColor: C.btnAct, borderColor: 'rgba(255,160,0,0.55)' },
  hbtnTxt:       { fontFamily: FONT, fontSize: 11, color: 'rgba(255,160,0,0.60)' },
  hbtnTxtActive: { color: C.gold },
  body:  { paddingHorizontal: 12, paddingVertical: 8, gap: 3 },
  rawNote: { fontFamily: FONT, fontSize: 10, color: C.warn, marginBottom: 6, lineHeight: 14 },
  row:   { flexDirection: 'row', alignItems: 'flex-start', gap: 8 },
  // ★ The label column is FIXED and the value WRAPS. Letting the label wrap instead was what
  // pushed text off the panel in the browser and made the whole thing scroll sideways.
  lbl:   { fontFamily: FONT, fontSize: 9, letterSpacing: 1, color: C.muted, width: 96 },
  val:   { fontFamily: FONT, fontSize: 11, color: C.value, flex: 1 },
  section: { fontFamily: FONT, fontSize: 9, letterSpacing: 2, color: C.goldDim,
             marginTop: 10, marginBottom: 2 },
  plots:   { flexDirection: 'row', gap: 10, marginTop: 4 },
  plotLbl: { fontFamily: FONT, fontSize: 8, letterSpacing: 1, color: C.muted, marginBottom: 2, marginTop: 8 },
  verdict: { fontFamily: FONT, fontSize: 10, marginTop: 3 },
  plotNote: { fontFamily: FONT, fontSize: 10, color: C.muted, marginTop: 4, lineHeight: 14 },
  logoWrap: { alignItems: 'center', marginTop: 10 },
});
