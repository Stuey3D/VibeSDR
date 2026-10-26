/**
 * ISM on a VibeServer (V12, docs/v12/ISM-DECODER.md) — the box over the lower waterfall, the web client's IsmBox.
 *
 *   header: EXIT ISM · ◀ band ▶ · BIG/SMALL
 *   the status: "433.92 MHz ISM · 6 devices · 41 bursts (3 unidentified) · gain 33.8 dB auto · capture 100 %"
 *   chips:  ALL WEATHER TPMS SENSOR DOORBELL ENERGY KEY FOB · KM/H|MPH · PSI|KPA|BAR
 *   list:   newest first (the server's order), "now / 12 s / 3 min" last heard, the readable line in the listener's units
 *
 * ★★ THE WATERFALL STAYS LIVE above it — the bursts on it are half of what the mode is for (Stuart). So this is a box, not
 *    a full screen like ADS-B's map.
 * ★★ THE SERVER OWNS THE BAND (the shared-dial contract): ◀ ▶ send a step, the next report says where the radio is; they
 *    grey at the ends (the server clamps, so a press there would be a dead send). The deck's own tune keys / drum / pad
 *    step bands too (SDRScreen ismStep).
 * ★★★ PRIVACY IS THE SERVER'S: a TPMS or a key fob arrives as "TPMS #2" / "Key fob #3" — no ID, no code.
 * ★ NO ✕, for the DAB box's reason: closing the window would leave the receiver in a mode with nothing on screen to control
 *   it. EXIT ISM leaves the mode and the box together. After a refusal (nothing to leave) the key reads CLOSE.
 * ★ The decoder font throughout (DecoderShell) — the display font is for the main controls only. Pure logic: services/ism.ts.
 */
import React, { useEffect, useRef, useState } from 'react';
import { ScrollView, StyleSheet, Text, TouchableOpacity, View, useWindowDimensions } from 'react-native';
import AsyncStorage from '@react-native-async-storage/async-storage';
import { DecoderShell, DecoderHeader, DecoderTitle, DecoderKey, DecoderBody, DECODER_FONT, decoderBodyInset,
         useDecoderTokens, useDecoderStyles, engraveStyle, type DecoderTokens } from './DecoderShell';
import { scrollLane } from '../constants/popupTokens';
import { captureRegion, capturedRegion, useKeyboardMode, useListNav, useNavFocus } from './PanelNav';
import { screenNavRef } from './ScreenNav';
import { ISM_KINDS, ismAgo, ismBandText, ismEmptyText, ismLine, ismRows, ismStatus, nextPressure, parseIsmPrefs,
         ismNavKeys, type IsmNavKey, type IsmReport, type IsmUnits } from '../services/ism';

const ISM_MAX_W = 760;
const PREF_KEY = 'lsv_ism_prefs';

export interface IsmPanelProps {
  report: IsmReport | null;
  /** The server's refusal (`ism_error`), shown in the body. */
  error?: string;
  /** True while the receiver is in ISM; false after a refusal (the exit key then only closes). */
  on: boolean;
  onExit: () => void;
  /** ±1 band — the box's arrows. */
  onStep: (d: number) => void;
  canDown: boolean;
  canUp: boolean;
  bottomOffset: number;
  tall: boolean;
  onTall: (v: boolean) => void;
  topLimit?: number;
  topSafe?: number;
}

export default function IsmPanel(p: IsmPanelProps) {
  const s = useDecoderStyles(makeStyles);
  const tk = useDecoderTokens();
  const { height: winH } = useWindowDimensions();
  const [filter, setFilter] = useState('all');
  const [units, setUnits] = useState<IsmUnits>({ mph: false, pressure: 'psi' });
  useEffect(() => {
    AsyncStorage.getItem(PREF_KEY).then((v) => { const pr = parseIsmPrefs(v); setFilter(pr.filter); setUnits(pr.units); }).catch(() => {});
  }, []);
  const save = (f: string, u: IsmUnits) => {
    AsyncStorage.setItem(PREF_KEY, JSON.stringify({ filter: f, mph: u.mph, pressure: u.pressure })).catch(() => {});
  };
  const pickFilter = (f: string) => { setFilter(f); save(f, units); };
  const pickUnits = (u: IsmUnits) => { setUnits(u); save(filter, u); };

  /* ★ The DAB box's sizing (AdsbPanel): SMALL 230 or the room down from topLimit; BIG up to the status row, offered only
   *  where it gains 40 pt. The header here is two rows (keys, then the status), and the chips a third. */
  const HEADER_H = 46 + 22 + 40;
  const smallBody = p.topLimit != null
    ? Math.max(90, winH - p.bottomOffset - p.topLimit - HEADER_H - decoderBodyInset(tk)) : 200;
  const bigBody = p.topSafe != null
    ? Math.max(120, winH - p.bottomOffset - p.topSafe - HEADER_H - decoderBodyInset(tk))
    : Math.max(120, winH - p.bottomOffset - 300);
  const bigUseful = bigBody >= smallBody + 40;
  const tall = p.tall && bigUseful;
  const bodyH = Math.round(tall ? bigBody : smallBody);

  const r = p.report;
  const rows = ismRows(r, filter);
  const band = p.error ? 'not available' : r ? ismBandText(r) : 'finding the band…';

  /* ── BY PAD AND KEYBOARD (the DAB box's rule): the box TAKES the keys once on arrival when they are in use (the band is
   *  the tuning, so the box IS the interface); the arrows walk ismNavKeys, Enter presses, Back hands the keys back to the
   *  screen — whose highlight can come back to it ('ismBox'), Enter taking them again. A touch hands them back too. The
   *  screen's tune keys still step bands from anywhere. ── */
  const kbOn = useKeyboardMode();
  const ring = useNavFocus().ring;
  const [grab, setGrab] = useState(false);
  const autoGrabbed = useRef(false);
  useEffect(() => { if (grab) captureRegion('ism'); else if (capturedRegion() === 'ism') captureRegion(null); }, [grab]);
  useEffect(() => () => { if (capturedRegion() === 'ism') captureRegion(null); }, []);
  useEffect(() => {
    if (!kbOn) { setGrab(false); return; }
    if (autoGrabbed.current) return;
    autoGrabbed.current = true;
    setGrab(true);
  }, [kbOn]);
  const keys = ismNavKeys({ on: p.on, big: bigUseful, chips: !p.error, rows: p.error || !r ? 0 : rows.length });
  const nav = useRef({ keys, filter, units }); nav.current = { keys, filter, units };
  const listRef = useRef<ScrollView | null>(null);
  const rowY = useRef<number[]>([]);
  const press = (k: IsmNavKey) => {
    const { units: u } = nav.current;
    if (k === 'down') { if (p.canDown) p.onStep(-1); }
    else if (k === 'up') { if (p.canUp) p.onStep(1); }
    else if (k === 'big') p.onTall(!tall);
    else if (k === 'exit') { setGrab(false); p.onExit(); }
    else if (k === 'u:speed') pickUnits({ ...u, mph: !u.mph });
    else if (k === 'u:pressure') pickUnits({ ...u, pressure: nextPressure(u.pressure) });
    else if (k.startsWith('k:')) pickFilter(k.slice(2));
  };
  const chipsRef = useRef<ScrollView | null>(null);
  const chipX = useRef<Record<string, number>>({});
  const focus = useListNav(
    grab, keys.length,
    (i) => { const k = nav.current.keys[i]; if (k) press(k.key); },
    (i) => {
      const k = nav.current.keys[i];
      if (k && (k.key.startsWith('k:') || k.key.startsWith('u:'))) {
        const x = chipX.current[k.key];   // ★ the chips scroll sideways on a phone — bring the highlighted one into view
        if (x != null) chipsRef.current?.scrollTo({ x: Math.max(0, x - 40), animated: false });
        return;
      }
      if (!k || !k.key.startsWith('r:')) return;
      const y = rowY.current[Number(k.key.slice(2))];
      if (y != null) listRef.current?.scrollTo({ y: Math.max(0, y - 40), animated: false });
    },
    undefined,
    () => { setGrab(false); return true; },
    { rowOf: (i) => nav.current.keys[i]?.row ?? 0, startAt: Math.max(0, keys.findIndex((k) => k.key === 'exit')) },
  );
  const hl = (k: IsmNavKey) => (grab && keys[focus]?.key === k ? { borderColor: ring, borderWidth: 2 } : null);

  return (
    <DecoderShell bottom={p.bottomOffset} maxWidth={ISM_MAX_W} tall={tall} borderColor={grab ? ring : undefined}>
      <View pointerEvents="none" style={StyleSheet.absoluteFill} ref={screenNavRef('ismBox', () => setGrab(true))} />
      <DecoderHeader>
        <View style={[s.dot, p.on && !!r?.devices.length && s.dotOn]} />
        <DecoderTitle>ISM</DecoderTitle>
        {/* ★ The band name gives way, never the keys: EXIT must always be reachable (the DAB box's SE lesson). */}
        <View style={s.bandRow}>
          {p.on && <DecoderKey onPress={() => p.onStep(-1)} disabled={!p.canDown} label="◀"
                               accessibilityLabel="Previous band" style={[!p.canDown && s.off, hl('down')]} />}
          <Text style={s.band} numberOfLines={1}>{band}</Text>
          {p.on && <DecoderKey onPress={() => p.onStep(1)} disabled={!p.canUp} label="▶"
                               accessibilityLabel="Next band" style={[!p.canUp && s.off, hl('up')]} />}
        </View>
        {bigUseful && <DecoderKey active={tall} onPress={() => p.onTall(!tall)} label={tall ? 'SMALL' : 'BIG'} style={hl('big')} />}
        <DecoderKey onPress={p.onExit} label={p.on ? 'EXIT ISM' : 'CLOSE'} style={hl('exit')}
                    accessibilityLabel={p.on ? 'Leave ISM — the receiver goes back to where it was' : 'Close'} />
      </DecoderHeader>
      {!!r && !p.error && <Text style={s.status} numberOfLines={1}>{ismStatus(r)}</Text>}
      {!p.error && (
        <ScrollView ref={chipsRef} horizontal showsHorizontalScrollIndicator={false} style={s.chips} contentContainerStyle={s.chipsIn}
                    keyboardShouldPersistTaps="handled">
          {ISM_KINDS.map((k) => (
            <Chip key={k.id} s={s} label={k.label} on={filter === k.id} onPress={() => pickFilter(k.id)} hl={hl(`k:${k.id}`)}
                  onX={(x) => { chipX.current[`k:${k.id}`] = x; }} />
          ))}
          <View style={s.gap} />
          <Chip s={s} label={units.mph ? 'MPH' : 'KM/H'} on={false} a11y="Wind speed units"
                onPress={() => pickUnits({ ...units, mph: !units.mph })} hl={hl('u:speed')} onX={(x) => { chipX.current['u:speed'] = x; }} />
          <Chip s={s} label={units.pressure.toUpperCase()} on={false} a11y="Tyre pressure units"
                onPress={() => pickUnits({ ...units, pressure: nextPressure(units.pressure) })} hl={hl('u:pressure')} onX={(x) => { chipX.current['u:pressure'] = x; }} />
        </ScrollView>
      )}
      <DecoderBody>
        <ScrollView ref={listRef} style={{ maxHeight: bodyH, ...(tall ? { height: bodyH } : null) }} contentContainerStyle={scrollLane}
                    showsVerticalScrollIndicator>
          {p.error ? (
            <Text style={s.note}>{p.error}</Text>
          ) : !r ? (
            <Text style={s.note}>Taking the receiver to the ISM band…</Text>
          ) : rows.length ? rows.map((d, i) => (
            <View key={d.key || String(i)} style={[s.row, i % 2 === 0 && s.rowAlt, hl(`r:${i}`)]}
                  onLayout={(e) => { rowY.current[i] = e.nativeEvent.layout.y; }}
                  accessibilityLabel={`${ismLine(d, units)}, heard ${ismAgo(d.seen)}${d.seen >= 2 ? ' ago' : ''}, `
                    + `${(d.freq / 1e6).toFixed(3)} megahertz, ${Math.round(d.snr)} dB over the noise, ${d.count} ${d.count === 1 ? 'time' : 'times'}`}>
              <Text style={s.ago} numberOfLines={1}>{ismAgo(d.seen)}</Text>
              <Text style={s.txt}>{ismLine(d, units)}</Text>
            </View>
          )) : (
            <Text style={s.note}>{ismEmptyText(filter)}</Text>
          )}
        </ScrollView>
      </DecoderBody>
    </DecoderShell>
  );
}

type S = ReturnType<typeof makeStyles>;
function Chip({ s, label, on, onPress, a11y, hl, onX }: { s: S; label: string; on: boolean; onPress: () => void; a11y?: string;
                                                    hl?: object | null; onX?: (x: number) => void }) {
  return (
    <TouchableOpacity style={[s.chip, on && s.chipOn, hl]} onLayout={onX ? (e) => onX(e.nativeEvent.layout.x) : undefined} onPress={onPress} activeOpacity={0.7} accessibilityRole="button"
                      accessibilityLabel={a11y ?? label} accessibilityState={{ selected: on }}>
      <Text style={[s.chipTxt, on && s.chipTxtOn]}>{label}</Text>
    </TouchableOpacity>
  );
}

const F = DECODER_FONT;
const makeStyles = (T: DecoderTokens) => StyleSheet.create({
  dot:     { width: 8, height: 8, borderRadius: 4, backgroundColor: T.muted, opacity: 0.6 },
  dotOn:   { backgroundColor: T.good, opacity: 1 },
  bandRow: { flex: 1, flexShrink: 1, minWidth: 0, flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: 4 },
  band:    { flexShrink: 1, minWidth: 0, fontFamily: F, fontSize: 13, letterSpacing: 1, color: T.hdrValue, textAlign: 'center',
             ...engraveStyle(T) },
  off:     { opacity: 0.35 },
  status:  { fontFamily: F, fontSize: 11, color: T.muted, paddingHorizontal: 12, paddingBottom: 4 },
  chips:   { flexGrow: 0, maxHeight: 40 },
  chipsIn: { alignItems: 'center', gap: 6, paddingHorizontal: 10, paddingVertical: 4 },
  // ★ 32 pt chips (the web's 30 px), inside the box on the glass or the dark window: outline chips (decoderTokens chip*)
  chip:    { minHeight: 32, justifyContent: 'center', paddingHorizontal: 10, borderRadius: 6, borderWidth: 1, borderColor: T.chipBorder },
  chipOn:  { borderColor: T.chipBorderAct, backgroundColor: T.chipBgAct },
  chipTxt: { fontFamily: F, fontSize: 11, letterSpacing: 1, color: T.keyText },
  chipTxtOn: { color: T.keyTextAct },
  gap:     { width: 8 },
  row:     { flexDirection: 'row', gap: 10, paddingHorizontal: 10, paddingVertical: 4 },
  rowAlt:  { backgroundColor: T.rowActive },
  ago:     { width: 54, textAlign: 'right', fontFamily: F, fontSize: 12, color: T.muted, fontVariant: ['tabular-nums'] },
  txt:     { flex: 1, minWidth: 0, fontFamily: F, fontSize: 12, lineHeight: 17, color: T.value },
  note:    { fontFamily: F, fontSize: 13, color: T.muted, padding: 12, lineHeight: 18 },
});
