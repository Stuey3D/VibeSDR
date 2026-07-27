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

import React, { useMemo } from 'react';
import { ScrollView, StyleSheet, Text, TouchableOpacity, View } from 'react-native';
import { Canvas, Circle, Path, Rect, Skia } from '@shopify/react-native-skia';
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
function Row({ label, value, colour, conf, raw }: {
  label: string; value: string; colour?: string; conf?: boolean; raw: boolean;
}) {
  const lblCol = raw && conf !== undefined ? (conf ? C.good : C.bad) : C.muted;
  return (
    <View style={s.row}>
      <Text style={[s.lbl, { color: lblCol }]} numberOfLines={1}>{label}</Text>
      <Text style={[s.val, colour ? { color: colour } : null]}>{value}</Text>
    </View>
  );
}

/** ★★ The constellation. Two tight lobes = a clean BPSK subcarrier; a RING means the encoder
 *  is sweeping against the pilot, which is a diagnosis and not a fault of ours. Points arrive
 *  pre-scaled x100 and clipped to +/-127 by the server. */
function Constellation({ xy, size }: { xy: number[]; size: number }) {
  const pts = useMemo(() => {
    const out: { x: number; y: number }[] = [];
    const half = size / 2;
    for (let i = 0; i + 1 < xy.length; i += 2) {
      out.push({ x: half + (xy[i] / 127) * half * 0.92,
                 y: half - (xy[i + 1] / 127) * half * 0.92 });
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

/** The MPX spectrum strip — dB integers in [-128, 0], one per bin. */
function Mpx({ mpx, width, height }: { mpx: number[]; width: number; height: number }) {
  const path = useMemo(() => {
    const p = Skia.Path.Make();
    if (!mpx.length) return p;
    // Floor at -90 dB rather than -128: the bottom 38 dB is all noise and spending
    // a third of the strip's height on it flattens everything worth looking at.
    const FLOOR = -90;
    for (let i = 0; i < mpx.length; i++) {
      const x = (i / (mpx.length - 1)) * width;
      const norm = Math.max(0, Math.min(1, (mpx[i] - FLOOR) / (0 - FLOOR)));
      const y = height - norm * height;
      if (i === 0) p.moveTo(x, y); else p.lineTo(x, y);
    }
    return p;
  }, [mpx, width, height]);
  return (
    <Canvas style={{ width, height }}>
      <Rect x={0} y={0} width={width} height={height} color="rgba(255,160,0,0.05)" />
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

  // ── Clock. The offset is in HALF-hours, which is why India works. ───────────
  let ctTxt = DASH;
  if (x && x.ct >= 0) {
    const local = x.ct + (x.ctoff ?? 0) * 30;
    const wrapped = ((local % 1440) + 1440) % 1440;
    const hh = String(Math.floor(wrapped / 60)).padStart(2, '0');
    const mm = String(wrapped % 60).padStart(2, '0');
    const off = (x.ctoff ?? 0) / 2;
    ctTxt = `${hh}:${mm} · UTC${off >= 0 ? '+' : ''}${off}`;
  }

  const odas = x?.oda ?? [];
  const eons = x?.eon ?? [];
  const afs  = x?.af ?? [];
  const nowPlaying = [x?.rtpArtist, x?.rtpTitle].filter(Boolean).join(' — ');

  const height = p.tall ? '78%' : '46%';

  return (
    <View style={[s.wrap, { bottom: p.bottomOffset }]}>
      <View style={[s.inner, { maxHeight: height as any }]}>
        <View style={s.header}>
          <Text style={s.title}>ADV RDS</Text>
          <View style={{ flex: 1 }} />
          <TouchableOpacity onPress={() => p.onRaw(!raw)}
            style={[s.hbtn, raw && s.hbtnActive]}>
            <Text style={[s.hbtnTxt, raw && s.hbtnTxtActive]}>RAW</Text>
          </TouchableOpacity>
          <TouchableOpacity onPress={() => p.onTall(!p.tall)} style={s.hbtn}>
            <Text style={s.hbtnTxt}>{p.tall ? '▾' : '▴'}</Text>
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

          <Row raw={raw} label="PI"      value={p.pi ?? DASH} />
          <Row raw={raw} label="PI DETAIL" value={piNum > 0
            ? `${COV[(piNum >> 8) & 0xF]} · ref ${piNum & 0xFF} · cc ${(piNum >> 12) & 0xF}`
            : DASH} />
          <Row raw={raw} label="PS"      value={p.ps || DASH} />
          <Row raw={raw} label="LONG PS" value={x?.longPs || DASH} />
          <Row raw={raw} label="PTY"     value={ptyTxt}
               conf={(x?.pty ?? -1) >= 0 || ptyV < 0 ? (x?.pty ?? -1) >= 0 : false} />
          <Row raw={raw} label="PTYN"    value={x?.ptyn || DASH} />
          <Row raw={raw} label="FLAGS"   value={flags.length ? flags.join(' · ') : DASH}
               conf={(x?.tp ?? -1) >= 0 || (x?.ms ?? -1) >= 0} />
          <Row raw={raw} label="RADIOTEXT" value={p.rt || DASH} />
          <Row raw={raw} label="NOW PLAYING" value={nowPlaying || DASH} />
          <Row raw={raw} label="LANGUAGE" value={x?.lang ? (LANGS[x.lang] ?? `code ${x.lang}`) : DASH} />
          <Row raw={raw} label="CLOCK"   value={ctTxt} />
          <Row raw={raw} label="BER"     value={ber >= 0 ? `${ber}%` : DASH} />
          <Row raw={raw} label="GROUPS"  value={x && x.gtot > 0 ? String(x.gtot) : DASH} />

          <Text style={s.section}>SIGNAL</Text>
          {/* ★ Say what the level is relative to: on its own a bare number invites the
              reading that the signal is weak, when it is a normal injection ratio. */}
          <Row raw={raw} label="PILOT DEV" value={pilotTxt} colour={pilotCol} />
          <Row raw={raw} label="RDS DEV"   value={rdsDevTxt} colour={rdsDevCol} />
          <Row raw={raw} label="RDS PHASE" value={phaseTxt} colour={phaseCol} />

          <Text style={s.section}>NETWORK</Text>
          <Row raw={raw} label="ODA" value={odas.length
            ? odas.map(o => `${ODA_NAMES[o.aid] ?? o.aid} in ${o.grp >> 1}${(o.grp & 1) ? 'B' : 'A'}`).join(', ')
            : DASH} />
          {/* ★ EON — the sister stations. TA on one of them is why a car radio switches over. */}
          <Row raw={raw} label="EON" value={eons.length
            ? eons.map(e => {
                const ps = e.ps.trim();
                const f = e.af ? ` ${(e.af / 1000).toFixed(1)}` : '';
                return `${ps || e.pi}${f}${e.ta === 1 ? ' [TA]' : ''}`;
              }).join('  ')
            : DASH} />
          <Row raw={raw} label="AF" value={afs.length
            ? afs.map(a => (a / 1000).toFixed(1)).join(' ') : DASH} />

          <Text style={s.section}>PLOTS</Text>
          <View style={s.plots}>
            <View>
              <Text style={s.plotLbl}>CONSTELLATION</Text>
              <Constellation xy={x?.xy ?? []} size={120} />
            </View>
            <View style={{ flex: 1 }}>
              <Text style={s.plotLbl}>MPX</Text>
              <Mpx mpx={x?.mpx ?? []} width={180} height={120} />
            </View>
          </View>

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
  plotLbl: { fontFamily: FONT, fontSize: 8, letterSpacing: 1, color: C.muted, marginBottom: 2 },
  logoWrap: { alignItems: 'center', marginTop: 10 },
});
