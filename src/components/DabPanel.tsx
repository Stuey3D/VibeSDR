/**
 * DabPanel — the DAB decoder window on the phone, and a deliberate MIRROR of the web client's.
 *
 * ★★★ THE BRIEF WAS "A SIMPLE MIRROR OF THE WEBCLIENT" (Stuart, 2026-09-08): zoom locked out, the
 *     VFO moves the BLOCK, and the decoder window shows what the browser's shows. So the rows
 *     below are in the browser's order, with the browser's wording and the browser's thresholds —
 *     web/client/src/main.ts dabRender(). Two clients that disagree about whether a multiplex is
 *     healthy is worse than one client that cannot show it at all.
 *
 * ★★★ AND IT CONTAINS NO DSP, exactly like AdvRdsPanel. Every number here is MEASURED beside the
 *     decoder on the server and arrives in one `dab` message about once a second. If you find
 *     yourself deriving a value in this file, it belongs in vibe_dab_service.h instead — and if it
 *     cannot be measured, the row says so rather than showing a plausible number. That is the
 *     standard the Advanced RDS panel set and this panel keeps.
 *
 * ★★ THE STRINGS ARRIVE PRE-GATED. parseDabMessage() has already stripped control characters and
 *    lone surrogates and bounded every label — see dabTypes.ts for why that is not paranoia but
 *    the RDS fault, pre-empted. Nothing in here re-checks them, and nothing in here should build a
 *    string by concatenating one into JSON.
 */
import React, { useMemo } from 'react';
import { Animated, Easing, Image, Platform, ScrollView, StyleSheet, Text, TouchableOpacity, View, useWindowDimensions } from 'react-native';
import { BlurView } from 'expo-blur';
import { Canvas, Points, Rect } from '@shopify/react-native-skia';
import type { DabState } from '../services/dabTypes';
import { DAB_BLOCKS, DAB_PTY, dabBlockAt } from '../services/dabBlocks';

const C = {
  /* ★★★ NEARLY OPAQUE, AND DELIBERATELY NOT GLASS LIKE THE RDS PANEL. That panel is see-through
   *  for one reason: "so that the user could still see a little spectrum underneath the window so
   *  that they can see to tune". In DAB there is nothing to tune behind it — the block is the
   *  tuning and the dial is locked out — so the transparency buys nothing and costs the thing this
   *  window is entirely made of: two dozen lines of small text. Measured on the Xcover over 11A,
   *  2026-09-08: at 0.72 the service list was unreadable wherever the waterfall ran hot. */
  bg:      'rgba(10,8,4,0.94)',
  border:  'rgba(255,160,0,0.28)',
  gold:    '#ffb833',
  goldDim: 'rgba(255,160,0,0.86)',
  muted:   'rgba(255,160,0,0.60)',
  hdrBdr:  'rgba(255,160,0,0.12)',
  btnBdr:  'rgba(255,160,0,0.28)',
  btnAct:  'rgba(255,160,0,0.12)',
  good:    '#7dff9a',
  warn:    '#ffd479',
  bad:     '#ff8a7d',
  value:   '#ffe566',
  rowAct:  'rgba(255,160,0,0.14)',
};
const FONT = 'Atkinson Hyperlegible';
const DASH = '—';

type Tone = 'ok' | 'warn' | 'bad' | undefined;
const toneColour = (t: Tone) => t === 'ok' ? C.good : t === 'warn' ? C.warn : t === 'bad' ? C.bad : C.value;

/** One label/value row. Memoised for the same reason AdvRdsPanel's is: this panel is ~40 rows and
 *  the message arrives about once a second, but the spectrum trace is tweened on the JS thread and
 *  a burst of reconciliation there shows up as a stuttering waterfall, not as a slow panel. */
const Row = React.memo(function Row({ label, value, tone }: {
  label: string; value: string; tone?: Tone;
}) {
  return (
    <View style={s.row}>
      <Text style={s.lbl} numberOfLines={2}>{label}</Text>
      <Marquee text={value} style={[s.val, { color: toneColour(tone) }]} />
    </View>
  );
});

const Section = ({ t }: { t: string }) => <Text style={s.section}>{t}</Text>;

/** ★ The constellation, drawn as ONE Skia primitive rather than a node per point — the lesson
 *  AdvRdsPanel paid for: a React element per sample, several hundred of them, reconciled on every
 *  message, is the cost that starves the trace. Points arrive as 192 int8 pairs, ideal radius 60. */
const Constellation = React.memo(function Constellation({ iq, size }: { iq: number[]; size: number }) {
  const pts = useMemo(() => {
    const out: { x: number; y: number }[] = [];
    const half = size / 2;
    const k = half / 90;                      // ideal radius 60 of a ~90 full scale
    for (let i = 0; i + 1 < iq.length; i += 2) {
      out.push({ x: half + iq[i] * k, y: half - iq[i + 1] * k });
    }
    return out;
  }, [iq, size]);
  return (
    <Canvas style={{ width: size, height: size }}>
      <Rect x={0} y={0} width={size} height={size} color="rgba(255,160,0,0.05)" />
      <Rect x={size / 2 - 0.5} y={0} width={1} height={size} color="rgba(255,160,0,0.18)" />
      <Rect x={0} y={size / 2 - 0.5} width={size} height={1} color="rgba(255,160,0,0.18)" />
      <Points points={pts} mode="points" style="stroke" strokeWidth={2.2}
              strokeCap="round" color="rgba(125,255,154,0.75)" />
    </Canvas>
  );
});

/** ★★ THE IMPULSE RESPONSE IS THE ONE PLOT THAT EXPLAINS A DAB FAILURE. 128 bins, 4 samples each
 *  (32 µs per division), 255 = peak. One spike = one path; a second spike beyond the guard
 *  interval is the reflection that no amount of gain will fix. Drawn as bars, like the browser. */
const ImpulseResponse = React.memo(function ImpulseResponse({ ir, width, height }: {
  ir: number[]; width: number; height: number;
}) {
  const bars = useMemo(() => {
    const out: { x: number; y: number }[] = [];
    const n = Math.max(1, ir.length);
    const w = width / n;
    for (let i = 0; i < n; i++) {
      const h = Math.max(0, Math.min(1, ir[i] / 255)) * (height - 2);
      // Two points per bar (base and top) drawn in 'lines' mode = one primitive for the plot.
      out.push({ x: i * w + w / 2, y: height }, { x: i * w + w / 2, y: height - h });
    }
    return out;
  }, [ir, width, height]);
  return (
    <Canvas style={{ width, height }}>
      <Rect x={0} y={0} width={width} height={height} color="rgba(255,160,0,0.05)" />
      <Points points={bars} mode="lines" style="stroke" strokeWidth={Math.max(1, width / 128 - 0.4)}
              color="rgba(255,190,90,0.85)" />
    </Canvas>
  );
});

/**
 * ★★★ THE STATION'S OWN ARTWORK, BY THE SAME CHAIN THE WEB CLIENT USES — and it is three sources,
 * not one. Stuart, 2026-09-08: "no station logos at all… the DAB experience is not on par with the
 * Web Client."
 *
 *   1. OFF THE AIR FIRST. `logoAir` means the multiplex's own SPI carousel carried this service's
 *      logo, or RadioDNS resolved one and the SERVER kept the file. Either way it is the
 *      broadcaster's file, served from the receiver we are already talking to.
 *   2. RADIODNS BY IDENTITY, asked once per service: /vibeserver/dablogo?ecc&eid&sid&scids answers
 *      with a URL (and keeps the bytes server-side, so the next client gets case 1 immediately).
 *   3. THE SLIDESHOW, last. Programme artwork changes through the day, so it must never displace
 *      the broadcaster's own logo — but for a small station that publishes neither it is the only
 *      picture of that station in existence.
 *
 * ★★ ASKED ONCE PER SERVICE, EVER. The `dab` message arrives about once a second and this list is
 *    re-rendered with it; a lookup per render would be a request per second per service. The cache
 *    is claimed BEFORE the fetch (the same rule primeStationLogo follows in the web client) or the
 *    first second fires one lookup per frame.
 * ★ A miss is remembered as null, so a station with no artwork is not asked about for ever.
 */
const logoCache = new Map<string, string | null>();

function useServiceLogo(base: string, d: DabState | null,
                        sv: DabState['services'][number] | undefined): string | null {
  const ecc = sv?.ecc ?? d?.ecc ?? -1;
  const key = `${ecc}|${d?.eid ?? 0}|${sv?.sid ?? 0}`;
  const [, bump] = React.useState(0);
  if (!sv || !d) return null;
  if (sv.logoAir) return `${base}/vibeserver/dablogoair?sid=${sv.sid}`;
  const known = logoCache.get(key);
  if (known === undefined) {
    logoCache.set(key, null);                       // claim it BEFORE the fetch — see above
    if (ecc >= 0 && d.eid) {
      const hex = (n: number, w: number) => n.toString(16).toUpperCase().padStart(w, '0');
      const q = `ecc=${hex(ecc, 2)}&eid=${hex(d.eid, 4)}&sid=${hex(sv.sid, 4)}&scids=${Math.max(0, sv.scids ?? 0)}`;
      fetch(`${base}/vibeserver/dablogo?${q}`)
        .then(r => (r.ok ? r.json() : null))
        .then((j: { logo?: string } | null) => {
          const u = j && typeof j.logo === 'string' && j.logo ? j.logo : '';
          if (!u) return;
          logoCache.set(key, u.startsWith('http') ? u : `${base}${u}`);
          bump(x => x + 1);                          // it arrived after the render that asked
        })
        .catch(() => {});
    }
  }
  if (known) return known;
  return sv.logoSlide ? `${base}/vibeserver/dabslide?sid=${sv.sid}` : null;
}

/**
 * ★★★ TEXT THAT IS TOO LONG SCROLLS, AS IT DOES IN THE WEB CLIENT. Stuart, 2026-09-08: "the
 * decoder window in the app needs to mirror the dab window in the web client so anything that
 * needs to scroll will scroll". A Dynamic Label is up to 128 characters and a row is ~30 wide, so
 * truncating it threw away most of the one thing DAB shows that FM cannot.
 *
 * ★ PING-PONG, NOT A CIRCULAR TICKER: the browser's ticker needs a duplicated copy of the text and
 *   a measured gap; on the native driver a two-way sweep with a pause at each end reads the same
 *   and costs one transform. It only animates when the text is actually wider than its box — a
 *   label that fits sits still, exactly like the browser's `dls` span.
 * ★ Restarts only when its own TEXT changes (the key), not on every `dab` message: the list is
 *   re-rendered every second, and a marquee that restarted with it "scrolls for a second then
 *   resets" — the browser's own fault, 2026-09-07.
 */
const Marquee = React.memo(function Marquee({ text, style }: { text: string; style: object }) {
  const [boxW, setBoxW] = React.useState(0);
  const [textW, setTextW] = React.useState(0);
  const x = React.useRef(new Animated.Value(0)).current;
  React.useEffect(() => {
    x.setValue(0);
    const over = textW - boxW;
    if (!(over > 4) || !boxW) return;
    const ms = Math.max(1500, over * 28);           // ~36 px/s — the browser's reading pace
    const loop = Animated.loop(Animated.sequence([
      Animated.delay(1200),
      Animated.timing(x, { toValue: -over, duration: ms, easing: Easing.linear, useNativeDriver: true }),
      Animated.delay(1200),
      Animated.timing(x, { toValue: 0, duration: ms, easing: Easing.linear, useNativeDriver: true }),
    ]));
    loop.start();
    return () => loop.stop();
  }, [textW, boxW, x, text]);
  return (
    <View style={{ flex: 1, overflow: 'hidden' }} onLayout={e => setBoxW(e.nativeEvent.layout.width)}>
      {/* ★★★ THE TEXT MUST NOT SHRINK. Inside a bounded row RN wraps a Text to the box rather than
          letting it overflow, so textW never exceeded boxW, nothing ever scrolled, and a long
          transmitter line showed as a clipped second line. flexShrink 0 + one line = the text keeps
          its natural width and the box hides the rest, which is what the measurement assumes. */}
      <Animated.View style={{ flexDirection: 'row', alignSelf: 'flex-start', transform: [{ translateX: x }] }}>
        <Text style={[style, { flexShrink: 0 }]} numberOfLines={1}
              onLayout={e => setTextW(e.nativeEvent.layout.width)}>{text}</Text>
      </Animated.View>
    </View>
  );
});

/** One service row's picture, or the space where it would be — a list whose rows change width as
 *  logos land is worse than one with none, so the box is always there. */
const SvcLogo = React.memo(function SvcLogo({ uri }: { uri: string | null }) {
  const [dead, setDead] = React.useState(false);
  React.useEffect(() => { setDead(false); }, [uri]);
  if (!uri || dead) return <View style={s.logoBox} />;
  return (
    <Image source={{ uri }} style={s.logoBox} resizeMode="contain"
           /* ★ A URL that resolves is not a picture that loads — the RDS panel learned this first.
            *  A logo that fails is dropped rather than left as a broken tile. */
           onError={() => setDead(true)} />
  );
});

/**
 * ★★★ EVERY SERVICE'S OWN LIVE TEXT, ON EVERY ROW — WHICH IS THE FEATURE, not a detail of it.
 * Stuart, 2026-09-08: "our full ensemble radio text is a huge boost none of the others do which is
 * cool", and, when this panel shipped without it: "the simultaneous radio text from all stations on
 * an ensemble is missing too". A DAB receiver decodes ONE service; we decode the whole multiplex's
 * X-PAD, so this list shows what twenty stations are playing AT ONCE. Drawing it only for the
 * tuned service threw away the one thing no other receiver can do.
 * ★ `sv.dls` is per service and already gated (dabTypes.parseDabMessage); `d.dls` is the tuned
 *   one's and is the fallback for the row that is playing, since the server sends it there.
 */
const ServiceRow = React.memo(function ServiceRow({ sv, d, base, onPress }: {
  sv: DabState['services'][number]; d: DabState; base: string; onPress: () => void;
}) {
  const active = sv.sid === d.sid;
  const logo = useServiceLogo(base, d, sv);
  const text = sv.dls || (active ? d.dls : '') || '';
  /* ★ THE ANNOUNCEMENT LAMP — a car's TA indicator with TA SWITCHING OFF. Stuart: "dont auto tune
   *  but if we can show the signal being recieved that would be good. Like in a car with TA off."
   *  We never retune to it: on a shared VFO one listener's traffic flash drags everybody else off
   *  the station they chose. */
  const ann = (d.announce ?? []).filter(a => a.subChId === sv.subch && a.types.length);
  const alarm = ann.some(a => a.alarm);
  return (
    <TouchableOpacity onPress={onPress} style={[s.svc, active && s.svcActive]} activeOpacity={0.7}>
      <SvcLogo uri={logo} />
      <View style={{ flex: 1 }}>
        {/* ★ GREEN for the station that is playing — the browser's `.dabSvc.on` — because gold on
            gold did not stand out (Stuart: "the highlighted station being green to stand out"). */}
        <Text style={[s.svcName, active && { color: C.good }]} numberOfLines={1}>
          {sv.label || sv.sid.toString(16).toUpperCase()}
          {ann.length ? (alarm ? '  ⚠ ALARM' : '  ● ANN') : ''}
        </Text>
        {!!text && <Marquee text={text} style={[s.svcDls, active && { color: 'rgba(125,255,154,0.80)' }]} />}
      </View>
      <Text style={s.svcCodec}>{sv.codec}{sv.kbps ? ` ${sv.kbps}k` : ''}</Text>
    </TouchableOpacity>
  );
});

/** ★ The transmitter memory — one per multiplex, cleared on a block change. Mirrors dabTxRemember
 *  in the web client, and for the same reason: a line that appears and disappears with the SNR
 *  makes everything under it jump. */
type DabTx = NonNullable<DabState['tii']>[number];
const txSeen = new Map<string, { t: DabTx; order: number }>();
let txChannel = '';
let txOrder = 0;
function rememberTransmitters(d: DabState): Array<{ t: DabTx; lost: boolean }> {
  if (d.channel !== txChannel) { txSeen.clear(); txChannel = d.channel; }
  for (const t of d.tii ?? []) {
    const k = `${t.main}/${t.sub}`;
    const e = txSeen.get(k);
    if (e) e.t = t; else txSeen.set(k, { t, order: txOrder++ });
  }
  const live = new Set((d.tii ?? []).map(t => `${t.main}/${t.sub}`));
  return [...txSeen.values()].sort((a, b) => a.order - b.order)
    .map(e => ({ t: e.t, lost: !live.has(`${e.t.main}/${e.t.sub}`) }));
}

export interface DabPanelProps {
  /** The last `dab` message, or null while the server has not sent one (starting, or off). */
  d: DabState | null;
  /** The server's refusal, when it could not start DAB at all. An explanation, not an error. */
  error?: string;
  /** Index into DAB_BLOCKS of the block being decoded or asked for. */
  blockIndex: number;
  /* ★ No onBlock any more: the block is stepped by the MAIN tuning arrows, the drum and the
   *  lock-screen skip, all in SDRScreen. A prop nothing reads is the "written and never read"
   *  trap this repo keeps finding — so it is gone rather than left for later. */
  onService: (sid: number) => void;
  /** Close the WINDOW and leave DAB running — the X. See onExit for the other one. */
  onClose: () => void;
  /** ★★★ TWO DOORS, AND THE DIFFERENCE MATTERS. Stuart, 2026-09-08: "the X button on the decoder
   *  box closes the decoder but leaves DAB active but when you press DAB again from the
   *  demodulator menu it deactivates DAB fully rather than restore the box." Closing a window and
   *  leaving a mode are different intentions and now have different controls. */
  onExit: () => void;
  bottomOffset: number;
  /** BIG/SMALL — the same control the browser's window and the RDS analyser have, and equally
   *  just a height. Stuart: "the big/small button is missing". */
  tall: boolean;
  onTall: (v: boolean) => void;
  /** The receiver's own base URL — logos are fetched FROM the server we are listening to, which is
   *  the only thing that holds this multiplex's carousel. */
  base: string;
}

export default function DabPanel(p: DabPanelProps) {
  const [pane, setPane] = React.useState<'stations' | 'signal'>('stations');
  const { height: winH } = useWindowDimensions();
  /* ★★★ THE PANEL MUST NOT GROW UP INTO THE TOP CHIPS, AND THE REASON IS TOUCH, NOT LOOKS.
   *  A full multiplex is ~20 services, which made the panel tall enough to reach the "Servers"
   *  chip, the session timer and the listener count — and those are drawn ABOVE it and swallow
   *  every touch that lands on them. The header ended up underneath: SIGNAL and EXIT DAB rendered
   *  perfectly and did nothing at all (measured on the Xcover on 11A, 2026-09-08, where the taps
   *  fell through to the list instead).
   *  ★ So the BODY is capped to what is left after the chips, and the list scrolls inside it. */
  const maxBody = p.tall ? Math.max(140, winH - p.bottomOffset - 190) : 230;
  const d = p.d;
  const cur = d ? d.services.find(x => x.sid === d.sid) : undefined;
  const txLines = useMemo(() => (d ? rememberTransmitters(d) : []), [d]);
  const noDecoder = !!d && (d.sfTried ?? 0) > 0 && d.aacServerSide === false;

  /* ★ The pane resets to STATIONS when the ENSEMBLE changes, as the browser's does: a new
   *  multiplex means a new list, and leaving the reader on a signal pane full of the last one's
   *  numbers is how a stale reading gets believed. */
  const eid = d?.eid ?? -1;
  React.useEffect(() => { setPane('stations'); }, [eid]);

  const block = p.blockIndex >= 0 ? DAB_BLOCKS[p.blockIndex] : undefined;
  const muxTitle = d?.label || (d && !d.locked ? 'searching…' : block ? block.name : DASH);

  const body = (
    <ScrollView style={{ maxHeight: maxBody }} contentContainerStyle={s.body}
                showsVerticalScrollIndicator={false}>
      {!!p.error && <Text style={s.notice}>{p.error}</Text>}
      {/* ★★★ SAY WHY IT IS SILENT, IN BOTH PANES. DAB+ is decoded ON THE SERVER — Stuart,
          2026-09-08: "dab+ audio should always be handled on the server the client shouldnt do
          anything other then play the Opus audio" — so a server without an AAC decoder gives a
          locked multiplex, a full station list, a moving DLS and no sound whatever. There is
          nothing the app can or should do about it, which is exactly why it has to SAY so: a
          silent radio with a perfect display reads as a broken app.
          ★ MEASURED, not inferred: `aacServerSide` is the server's own report of whether its
            platform decoder opened, and `sfTried` says DAB+ super frames are actually arriving. */}
      {noDecoder && (
        <Text style={[s.notice, { color: C.bad }]}>
          No sound: this server has no DAB+ decoder. MP2 services still play.
        </Text>
      )}
      {!d && !p.error && <Text style={s.notice}>Tuning the multiplex…</Text>}

      {!!d && pane === 'stations' && (
        <>
          {d.services.length === 0 && (
            <Text style={s.notice}>
              {d.locked ? 'Locked — reading the service list…' : 'Searching for the multiplex…'}
            </Text>
          )}
          {d.services.map(sv => (
            <ServiceRow key={sv.sid} sv={sv} d={d} base={p.base}
                        onPress={() => p.onService(sv.sid)} />
          ))}
        </>
      )}

      {!!d && pane === 'signal' && (
        <>
          <Section t="SERVICE" />
          <Row label="Codec" value={d.codecDetail ?? cur?.codec ?? DASH} />
          <Row label="Bit rate" value={d.bitrate ? `${d.bitrate} kbit/s` : DASH} />
          <Row label="Protection" value={cur?.prot ?? (d.protection || DASH)} />
          {!!cur && cur.cuSize !== undefined && (
            <Row label="Capacity units"
                 value={`${cur.cuStart}–${(cur.cuStart ?? 0) + (cur.cuSize ?? 0) - 1} (${cur.cuSize} CU, sub-channel ${cur.subch})`} />
          )}
          {/* ★ FIG 0/17's S/D flag (EN 300 401 8.1.5): "dynamic" follows the ITEMS within a
              programme, so it is live; static is the programme's overall genre. Saying which is
              the difference between a live readout and a label. */}
          {!!cur && cur.pty !== undefined && cur.pty >= 0 && (
            <Row label="Programme type"
                 value={`${DAB_PTY[cur.pty] ?? String(cur.pty)}${cur.ptyDyn ? ' · dynamic' : ' · static genre'}`} />
          )}
          {!!cur && cur.ecc !== undefined && cur.ecc >= 0 && (
            <Row label="Service id"
                 value={`${cur.ecc.toString(16).toUpperCase()}:${cur.sid.toString(16).toUpperCase().padStart(4, '0')}`} />
          )}
          {/* ★ The RDS side, from the ensemble's own signalling (FIG 0/6 links, FIG 0/21
              frequencies): which FM station this programme is, where it is on FM, and which other
              DAB services carry it. Stuart, 2026-09-07: "the real full RDS info from DAB". */}
          <Row label="RDS PI" value={cur?.pi?.length
            ? cur.pi.map(x => x.toString(16).toUpperCase().padStart(4, '0')).join(', ')
              + (cur.piImplicit ? ' (SId, implicit)' : cur.linkActive === false ? ' (linked, inactive)' : ' (linked)')
            : DASH} />
          <Row label="On FM" value={cur?.fm?.length
            ? cur.fm.map(hz => (hz / 1e6).toFixed(1)).join(', ') + ' MHz' : DASH} />
          <Row label="Also carried by" value={cur?.linkSids?.length
            ? cur.linkSids.map(sid => d.services.find(x => x.sid === sid)?.label
                                     ?? sid.toString(16).toUpperCase().padStart(4, '0')).join(' · ')
              + (cur.linkHard === false ? ' (soft link)' : '')
              + (cur.linkActive === false ? ' (link inactive)' : '')
            : DASH} />

          <Section t="ERROR RATE" />
          {/* ★ Errors, not passes: "it's easier to read 5 % errors rather than 95 % pass". */}
          <Row label="FIB errors this frame"
               value={`${Math.max(0, d.fibTotal - d.fibOk)} of ${d.fibTotal}`}
               tone={d.fibOk >= d.fibTotal ? 'ok' : d.fibOk >= d.fibTotal - 2 ? 'warn' : 'bad'} />
          <Row label="FIB error rate" value={`${((1 - d.fibRate) * 100).toFixed(1)} %`}
               tone={d.fibRate >= 0.99 ? 'ok' : d.fibRate >= 0.9 ? 'warn' : 'bad'} />

          <Section t="MULTIPLEX" />
          <Row label="Ensemble" value={d.label || DASH} />
          <Row label="EId" value={(d.ecc !== undefined && d.ecc >= 0 ? d.ecc.toString(16).toUpperCase() + ':' : '')
                                  + d.eid.toString(16).toUpperCase().padStart(4, '0')} />
          <Row label="Services" value={String(d.services.length)
            + (d.nsvc !== undefined && d.nsvc >= 0 ? ` of ${d.nsvc}` : '')
            + (d.mci ? '' : ' (reading…)')} />
          {d.cif !== undefined && d.cif >= 0 && <Row label="CIF count" value={String(d.cif)} />}
          <Row label="Ensemble clock" value={d.utc ? `${d.utc} UTC` : DASH} />
          <Row label="Also on" value={d.altHz?.length
            ? d.altHz.map(hz => { const i = dabBlockAt(hz); return i >= 0 ? DAB_BLOCKS[i].name : (hz / 1e6).toFixed(3); }).join(', ')
            : DASH} />
          {/* ★ TII — the transmitters behind the ensemble, with how far each stands above the
              null's noise. And when there are none, SAY WHY: a null symbol with no TII energy at
              all is a multiplex that does not transmit identification (Digital One on 11D,
              measured 2026-09-07), not a receiver that has failed to find it. */}
          {/* ★★★ TRANSMITTER LINES THAT DO NOT BOUNCE — the same memory the web client keeps. A site
              is remembered for the life of the multiplex; while unheard its line stays, dimmed and
              marked, rather than vanishing and letting the Ofcom record take its place (Stuart:
              "the whole box grows and shrinks significantly with the licenced transmitters row"). */}
          {txLines.length
            ? txLines.map((e, i) => (
                <Row key={`${e.t.main}.${e.t.sub}`} label={i === 0 ? 'Transmitters' : ''}
                     value={`${e.t.main.toString(16).toUpperCase()}/${e.t.sub.toString(16).toUpperCase().padStart(2, '0')}`
                          + (e.t.site ? ` · ${e.t.site}` : '')
                          + (e.t.area && e.t.area !== e.t.site ? ` (${e.t.area})` : '')
                          + (e.t.km !== undefined && e.t.km >= 0 ? ` · ${(e.t.km * 0.621371).toFixed(e.t.km < 16 ? 1 : 0)} mi` : '')
                          + (e.lost ? ' · not received' : ` · ${e.t.db.toFixed(1).padStart(4, '\u2007')} dB`)
                          + (e.t.ambiguous ? ' · ambiguous' : '')}
                     tone={e.lost ? 'warn' : undefined} />
              ))
            : <Row label="Transmitters" value={!d.locked ? DASH
                : (d.tiiDiag && d.tiiDiag.frames > 0 && d.tiiDiag.f4s < 2
                    ? 'none transmitted — no TII in the null symbol' : 'none identified yet')} />}
          {/* ★ Ofcom's licence record, ONLY when the air gives nothing to go on. With a real site
              identified it is clutter (Stuart: "that just adds too much in a small space"). */}
          {!txLines.some(e => !!e.t.site) && d.licensed?.map((l, i) => (
            <Row key={l.code + i} label={i === 0 ? 'Licensed sites' : ''}
                 value={`${l.site}${l.area && l.area !== l.site ? ` (${l.area})` : ''}`
                      + (l.km >= 0 ? ` · ${(l.km * 0.621371).toFixed(l.km < 16 ? 1 : 0)} mi` : '')
                      + ` · ${l.code} · Ofcom record`} />
          ))}
          {/* ★ Always present, whatever it says: a row that comes and goes with its value rebuilds
              the pane and makes everything below it jump. */}
          <Row label="Now playing" value={d.dls || DASH} />
          {/* ★★★ AN ANNOUNCEMENT IS RUNNING (FIG 0/19), drawn right under "Now playing" because
              that is what it is about — the label above may describe a programme that has been
              interrupted. ALARM leads whatever order the multiplex sent them in. */}
          {(d.announce ?? []).slice().sort((a, b) => Number(b.alarm) - Number(a.alarm)).map((a, i) => (
            <Row key={`ann${i}`} label={a.alarm ? 'ALARM' : 'Announcement'}
                 value={`${a.types.join(', ')}${a.on ? ` on ${a.on}` : ''}`}
                 tone={a.alarm ? 'bad' : 'warn'} />
          ))}
          {!!d.announceSupport?.length && <Row label="Can carry" value={d.announceSupport.join(', ')} />}
          {/* ★★★ THE PROGRAMME GUIDE OFF THE AIR (TS 102 371), and the row is drawn EVEN WHEN
              EMPTY. A feature that renders nothing is indistinguishable from a decoder that
              failed, and this one renders nothing for every UK listener — measured 2026-09-08.
              Same posture as the TII row on 11D. */}
          {d.epgNow || d.epgNext ? (
            <>
              {!!d.epgNow && <Row label="On now" value={`${d.epgNow.at} · ${d.epgNow.name}`
                + (d.epgNow.mins ? ` · ${d.epgNow.mins} min` : '')
                + (d.epgNow.desc ? ` — ${d.epgNow.desc}` : '')} />}
              {!!d.epgNext && <Row label="On next" value={`${d.epgNext.at} · ${d.epgNext.name}`
                + (d.epgNext.mins ? ` · ${d.epgNext.mins} min` : '')} />}
            </>
          ) : (d.spi ? <Row label="Guide" value="no programme information transmitted" tone="warn" /> : null)}
          {/* ★ The categorised slideshow (TS 101 499 5.3.5). An Alert is an emergency warning and
              is not buried — but it does not seize the screen either, the same reasoning as the
              announcement lamp on a shared VFO. */}
          {!!d.slideAlert && <Row label="Slideshow" value="emergency warning signalled by the broadcaster" tone="bad" />}
          {!!d.slideCats?.length && (
            <Row label="Gallery" value={d.slideCats.map(c => `${c.title} (${c.slides.length})`).join(' · ')} />
          )}
          {/* ★★★ DL PLUS — what the station says the label MEANS. Fixed order, not transmission
              order, or the rows reshuffle themselves mid-track. */}
          {!!d.dlp && DLP_ORDER.filter(k => d.dlp![k]).map(k => (
            <Row key={k} label={k[0].toUpperCase() + k.slice(1)} value={d.dlp![k]} />
          ))}
          {d.dlpRunning === false && (
            <Row label="Item" value="ended — the label is not the current item" tone="warn" />
          )}

          <Section t="PHYSICAL LAYER" />
          <Row label="Lock" value={d.locked ? 'locked' : 'searching'} tone={d.locked ? 'ok' : 'bad'} />
          {/* ★ Requested and actual side by side — the pair that separates "cannot decode" from
              "pointed at the wrong frequency" (2026-09-04, four deploys to learn it). */}
          <Row label="Radio centre" value={d.rfCentreHz
            ? `${(d.rfCentreHz / 1e6).toFixed(4)} MHz`
              + (Math.abs(d.rfCentreHz - d.centreHz) > 500 ? ` (asked ${(d.centreHz / 1e6).toFixed(4)})` : '')
            : DASH} />
          <Row label="Capture rate" value={d.rfRateHz ? `${(d.rfRateHz / 1e6).toFixed(3)} MS/s` : DASH} />
          <Row label="Null depth" value={`${d.nullDepthDb.toFixed(1)} dB`}
               tone={d.nullDepthDb >= 14 ? 'ok' : d.nullDepthDb >= 8 ? 'warn' : 'bad'} />
          <Row label="Frequency offset" value={`${d.offsetHz.toFixed(0)} Hz (${d.offsetPpm.toFixed(2)} ppm)`}
               tone={Math.abs(d.offsetPpm) < 1 ? 'ok' : Math.abs(d.offsetPpm) < 3 ? 'warn' : 'bad'} />
          <Row label="Carrier shift" value={String(d.carrierShift)} />
          <Row label="Phase reference" value={d.prs.toFixed(3)
            + (d.prsRatio !== undefined ? ` (${d.prsRatio.toFixed(2)} of ref)` : '')} />
          <Row label="MER" value={d.mer ? `${d.mer.toFixed(1)} dB` : DASH}
               tone={d.mer ? (d.mer >= 16 ? 'ok' : d.mer >= 10 ? 'warn' : 'bad') : undefined} />
          <Row label="MSC bit errors" value={d.mscBer !== undefined && d.sid
            ? `${(d.mscBer * 100).toFixed(2)} % before Viterbi` : DASH}
               tone={d.mscBer === undefined || !d.sid ? undefined
                     : d.mscBer < 0.005 ? 'ok' : d.mscBer < 0.03 ? 'warn' : 'bad'} />

          {!!(d.iq?.length || d.ir?.length) && (
            <View style={s.plots}>
              {!!d.iq?.length && (
                <View>
                  <Text style={s.plotLbl}>CONSTELLATION</Text>
                  <Constellation iq={d.iq} size={110} />
                </View>
              )}
              {!!d.ir?.length && (
                <View style={{ flex: 1 }}>
                  <Text style={s.plotLbl}>IMPULSE RESPONSE · 32 µs per division</Text>
                  <ImpulseResponse ir={d.ir} width={190} height={72} />
                </View>
              )}
            </View>
          )}

          <Row label="Frames seen" value={String(d.frames)} />
          <Row label="Frames erased" value={String(d.erased ?? 0)}
               tone={!(d.erased ?? 0) || (d.erased ?? 0) / Math.max(1, d.frames) < 0.02 ? 'ok'
                     : (d.erased ?? 0) / Math.max(1, d.frames) < 0.1 ? 'warn' : 'bad'} />
          <Row label="Re-acquisitions" value={String(d.reacquires ?? 0)}
               tone={!(d.reacquires ?? 0) ? 'ok' : (d.reacquires ?? 0) < 3 ? 'warn' : 'bad'} />
          <Row label="IQ dropped" value={String(d.dropped ?? 0)} tone={!(d.dropped ?? 0) ? 'ok' : 'warn'} />

          <Section t="AUDIO CHANNEL" />
          {!!d.mp2In && (
            <Row label="Layer II frames"
                 value={`${d.mp2In} in, ${d.mp2Bad ?? 0} bad, ${d.mp2Concealed ?? 0} concealed`}
                 tone={(d.mp2Bad ?? 0) / d.mp2In < 0.01 ? 'ok' : (d.mp2Bad ?? 0) / d.mp2In < 0.1 ? 'warn' : 'bad'} />
          )}
          {!!d.sfTried && (
            <>
              <Row label="DAB+ super frames" value={`${d.sfOk ?? 0} of ${d.sfTried}`}
                   tone={(d.sfOk ?? 0) / d.sfTried > 0.98 ? 'ok' : (d.sfOk ?? 0) / d.sfTried > 0.9 ? 'warn' : 'bad'} />
              <Row label="Reed-Solomon" value={`${d.rsFixed ?? 0} fixed, ${d.rsLost ?? 0} lost`}
                   tone={!(d.rsLost ?? 0) ? 'ok' : (d.rsLost ?? 0) / Math.max(1, d.sfTried) < 0.05 ? 'warn' : 'bad'} />
            </>
          )}
          {!!d.aacRateHz && (
            <Row label="AAC" value={`${d.aacRateHz} Hz, ${d.aacCh} ch`
              + (d.aacServerSide ? ', decoded on the server' : '')} />
          )}
          {/* ★ The same fact as the notice at the top, in the place a reader diagnosing silence
              will look for it — beside the super-frame and Reed-Solomon counters that prove the
              DECODE is fine and the problem is downstream of it. */}
          {noDecoder && (
            <Row label="DAB+ decoder" value="not available on this server — DAB+ services are silent"
                 tone="bad" />
          )}
          {!!d.spi?.sid && (
            <Row label="Service information"
                 value={`${d.spi.logoSvcs} services with logos · ${d.spi.complete} of ${d.spi.named} files`
                      + ` · ${d.spi.groups} groups, ${d.spi.crcFail} bad, ${d.spi.lost} lost`}
                 tone={!d.spi.crcFail && !d.spi.lost ? 'ok'
                       : (d.spi.crcFail + d.spi.lost) < Math.max(1, d.spi.groups) / 10 ? 'warn' : 'bad'} />
          )}
          {d.motGroups !== undefined && (
            <Row label="Slideshow"
                 value={`${d.motObjects ?? 0} images, ${d.motGroups} groups, ${d.motCrcFail ?? 0} bad`
                      + (d.slide?.name ? ` · ${d.slide.name}` : '')}
                 tone={!(d.motCrcFail ?? 0) ? 'ok' : (d.motCrcFail ?? 0) < d.motGroups / 10 ? 'warn' : 'bad'} />
          )}
          {d.dlsCrcOk !== undefined && (
            <Row label="DLS groups" value={`${d.dlsCrcOk} ok, ${d.dlsCrcFail ?? 0} bad`}
                 tone={!(d.dlsCrcFail ?? 0) ? 'ok' : (d.dlsCrcFail ?? 0) < d.dlsCrcOk / 10 ? 'warn' : 'bad'} />
          )}
          {/* ★★★ THE SERVER SAYS WHEN IT HAD TO TRUNCATE, AND WE SHOW IT. A 512-byte stats buffer
              silently cutting this JSON short is what "DAB never locks" turned out to be
              (2026-09-05, APK 431). A number that prints oddly short is the message being cut —
              so the message now says so out loud rather than looking like a dead decoder. */}
          {!!d.truncated && (
            <Row label="Report truncated" value={`server needed ${d.truncated} bytes`} tone="bad" />
          )}
        </>
      )}
    </ScrollView>
  );

  return (
    <View style={[s.wrap, { bottom: p.bottomOffset }]} pointerEvents="box-none">
      <View style={s.inner}>
        {Platform.OS === 'ios' && (
          <BlurView intensity={24} tint="dark" style={StyleSheet.absoluteFill} />
        )}
        <View style={[StyleSheet.absoluteFill, { backgroundColor: C.bg }]} />

        <View style={s.header}>
          <Text style={s.title}>DAB</Text>
          {/* ★★★ A READOUT, NOT A CONTROL. This was a pair of chevrons either side of the block —
              and they were tiny, in a panel header, duplicating a control the app already has in
              the right place. Stuart: "2 extremely tiny buttons in the decoder header are there
              instead which we moved away from in the client."
              ★★ THE BLOCK IS THE TUNING, so the MAIN tuning arrows step it (see onVfoStep), and so
                 do the drum and the lock-screen skip. One meaning, in the controls the hand is
                 already on. AGENTS.md: when a control moves, the copy that says where it is moves
                 with it — which is why nothing here says "tap these". */}
          <Text style={s.blockTxt}>
            {block ? `${block.name}` : DASH}
            <Text style={s.blockHz}>{block ? `  ${(block.hz / 1e6).toFixed(3)}` : ''}</Text>
          </Text>
          <Text style={s.mux} numberOfLines={1}>{muxTitle}</Text>
          <View style={{ flex: 1 }} />
          <TouchableOpacity onPress={() => setPane(pane === 'stations' ? 'signal' : 'stations')}
                            style={[s.hbtn, s.hbtnActive]}>
            {/* ★ The label names where the button GOES, and the state is readable without pressing
                it — the browser's rule for this same control. */}
            <Text style={[s.hbtnTxt, s.hbtnTxtActive]}>{pane === 'stations' ? 'SIGNAL' : 'STATIONS'}</Text>
          </TouchableOpacity>
          <TouchableOpacity onPress={() => p.onTall(!p.tall)} style={[s.hbtn, p.tall && s.hbtnActive]}>
            <Text style={[s.hbtnTxt, p.tall && s.hbtnTxtActive]}>{p.tall ? 'SMALL' : 'BIG'}</Text>
          </TouchableOpacity>
          <TouchableOpacity onPress={p.onExit} style={s.hbtn}>
            <Text style={s.hbtnTxt}>EXIT DAB</Text>
          </TouchableOpacity>
          <TouchableOpacity onPress={p.onClose} style={s.hbtn}>
            <Text style={[s.hbtnTxt, { color: 'rgba(255,100,100,0.70)' }]}>✕</Text>
          </TouchableOpacity>
        </View>

        {body}
      </View>
    </View>
  );
}

/** TS 102 980's content types, in the order a reader wants them rather than the order they were
 *  transmitted in — see the note at the DL Plus rows. */
const DLP_ORDER = ['artist', 'title', 'album', 'track', 'composer', 'band', 'presenter',
  'programme', 'genre', 'station', 'slogan', 'comment', 'homepage', 'phone', 'email', 'sms',
  'news', 'sport', 'weather', 'traffic', 'alarm', 'advertisement', 'country'];

const s = StyleSheet.create({
  wrap:  { position: 'absolute', left: 8, right: 8, zIndex: 200, alignItems: 'center' },
  inner: {
    width: '100%', maxWidth: 560,
    borderWidth: 1, borderColor: C.border, borderRadius: 14,
    shadowColor: '#000', shadowOffset: { width: 0, height: 4 },
    shadowOpacity: 0.80, shadowRadius: 14, elevation: 16,
    overflow: 'hidden',
  },
  header: {
    flexDirection: 'row', alignItems: 'center', gap: 6,
    paddingHorizontal: 10, paddingVertical: 8,
    borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: C.hdrBdr,
  },
  title:    { fontSize: 11, letterSpacing: 2, color: C.goldDim, fontFamily: FONT },
  blockTxt: { fontFamily: FONT, fontSize: 13, color: C.gold },
  blockHz:  { fontFamily: FONT, fontSize: 10, color: C.muted },
  mux:      { fontFamily: FONT, fontSize: 12, color: C.value, maxWidth: 150 },
  hbtn:     { borderWidth: 1, borderColor: C.btnBdr, borderRadius: 4,
              paddingHorizontal: 7, paddingVertical: 3 },
  hbtnActive:    { backgroundColor: C.btnAct, borderColor: 'rgba(255,160,0,0.55)' },
  hbtnTxt:       { fontFamily: FONT, fontSize: 11, color: 'rgba(255,160,0,0.60)' },
  hbtnTxtActive: { color: C.gold },
  body:   { paddingHorizontal: 12, paddingVertical: 8, gap: 3 },
  notice: { fontFamily: FONT, fontSize: 12, color: C.warn, paddingVertical: 6 },
  row:    { flexDirection: 'row', alignItems: 'flex-start', gap: 8 },
  lbl:    { fontFamily: FONT, fontSize: 12, letterSpacing: 1, color: C.muted, width: 124 },
  val:    { fontFamily: FONT, fontSize: 14, color: C.value },
  section:{ fontFamily: FONT, fontSize: 10, letterSpacing: 2, color: C.goldDim,
            marginTop: 10, marginBottom: 2 },
  svc:      { flexDirection: 'row', alignItems: 'center', gap: 8, paddingVertical: 6,
              paddingHorizontal: 6, borderRadius: 6 },
  svcActive:{ backgroundColor: C.rowAct },
  /* ★ A FIXED BOX WHETHER OR NOT THERE IS A PICTURE. Logos arrive one at a time over a second or
   *  two; if the box appeared with them, every name in the list would shuffle sideways as they
   *  landed. 34 px — up from the browser's 24: on a phone the picture is the fastest way to find a
   *  station (Stuart, 2026-09-09: "logos larger, font"). */
  logoBox:  { width: 34, height: 34, borderRadius: 4 },
  /* ★ Up from 14/11/10: on the Mac the list was tiny (Stuart). The browser's row is 15px. */
  svcName:  { fontFamily: FONT, fontSize: 17, color: C.value },
  svcDls:   { fontFamily: FONT, fontSize: 14, color: C.muted, marginTop: 2 },
  svcCodec: { fontFamily: FONT, fontSize: 11, color: C.muted },
  plots:    { flexDirection: 'row', gap: 10, marginTop: 8, alignItems: 'flex-end' },
  plotLbl:  { fontFamily: FONT, fontSize: 9, letterSpacing: 1, color: C.muted, marginBottom: 2 },
});
