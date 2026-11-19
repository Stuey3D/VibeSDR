/**
 * The SATELLITES box on a VibeServer (V12, docs/v12/SATELLITE-ENGINE.md) — the picker (where each bird is, its next
 * pass, its downlinks), the live readout of a Doppler lock, a sky track, and the DOPPLER key. The decoder boxes' place
 * and look (DecoderShell), the words of src/services/satellites.ts — the web's satellites.ts says the same.
 *
 *   header: ● SATELLITES · DOPPLER · CLOSE
 *   readout (while locked): ISS · ▼ 3.21 kHz · TRACK · FM repeater · AZ/EL · LOS 6:12 — and the pass's sky track
 *   list:   up now first, then by AOS: name · UP EL 34° / AOS 21:34 (in 30:59) · max 56° — a key per downlink this
 *           radio can hear (TRACK: tune there, then lock)
 *
 * ★★ THE SERVER OWNS THE LOCK (the shared-dial contract): the keys send a request; the dial it snaps to comes back in the
 *    config, the readout as {"type":"sat"}. ★ No dead controls: the box exists only where the server sends `doppler`;
 *    greyed (the reason shown) when not ready; a downlink the radio cannot hear is not listed. ★ No flashing: COAST dims.
 */
import React, { useEffect, useState } from 'react';
import { ScrollView, StyleSheet, Text, View, useWindowDimensions } from 'react-native';
import Svg, { Circle, Path, Text as SvgText } from 'react-native-svg';
import { DecoderShell, DecoderHeader, DecoderTitle, DecoderKey, DecoderBody, DECODER_FONT, decoderBodyInset,
         useDecoderTokens, useDecoderStyles, engraveStyle, type DecoderTokens } from './DecoderShell';
import { scrollLane } from '../constants/popupTokens';
import { ageText, clockIn, pickerRows, satChanText, satStateWord, satStatusLine, skyPath, skyXY, txText,
         type SatList, type SatReadout, type SatTrack, type SatTx } from '../services/satellites';

const SAT_MAX_W = 760;

export interface SatellitePanelProps {
  list: SatList | null;
  readout: SatReadout | null;
  track: SatTrack | null;
  /** hwinfo's doppler / dopplerWhy: ok false = greyed, `why` shown. */
  cap: { ok: boolean; why: string };
  onDoppler: (on: boolean) => void;
  /** TRACK on a downlink: tune there, then lock that bird. */
  onTrack: (norad: number, tx: SatTx) => void;
  onClose: () => void;
  bottomOffset: number;
  topLimit?: number;
}

function SkyPlot({ track, now, color, dim }: { track: SatTrack | null; now?: { az?: number; el?: number }; color: string; dim: string }) {
  const ring = (el: number) => <Circle key={el} cx={0} cy={0} r={(90 - el) / 90} fill="none" stroke={dim} strokeWidth={0.012} />;
  const dot = now && now.az != null && now.el != null && now.el >= 0 ? skyXY(now.az, now.el) : null;
  return (
    <Svg width={104} height={104} viewBox="-1.15 -1.15 2.3 2.3" accessibilityLabel="Sky track">
      {ring(0)}{ring(30)}{ring(60)}
      <SvgText x={0} y={-1.04} fontSize={0.13} fill={dim} textAnchor="middle">N</SvgText>
      <SvgText x={1.08} y={0.04} fontSize={0.13} fill={dim} textAnchor="middle">E</SvgText>
      <SvgText x={0} y={1.12} fontSize={0.13} fill={dim} textAnchor="middle">S</SvgText>
      <SvgText x={-1.08} y={0.04} fontSize={0.13} fill={dim} textAnchor="middle">W</SvgText>
      {!!track && track.pts.length > 1 && <Path d={skyPath(track.pts)} fill="none" stroke={color} strokeWidth={0.03} strokeLinecap="round" />}
      {!!dot && <Circle cx={dot[0]} cy={dot[1]} r={0.06} fill={color} />}
    </Svg>
  );
}

export default function SatellitePanel(p: SatellitePanelProps) {
  const s = useDecoderStyles(makeStyles);
  const tk = useDecoderTokens();
  const { height: winH } = useWindowDimensions();
  const HEADER_H = 46 + 22;
  const bodyH = Math.round(p.topLimit != null
    ? Math.max(120, winH - p.bottomOffset - p.topLimit - HEADER_H - decoderBodyInset(tk)) : 260);
  // ★ the countdowns tick on between lists (the server is asked again every 30 s by the screen)
  const [, setTick] = useState(0);
  useEffect(() => { const t = setInterval(() => setTick((n) => n + 1), 1000); return () => clearInterval(t); }, []);
  const now = Date.now() / 1000;
  const r = p.readout && p.readout.state !== 'OFF' ? p.readout : null;
  const rows = pickerRows(p.list, now);
  const greyed = !p.cap.ok;
  const sel = r ? p.list?.sats.find((x) => x.norad === r.norad) : undefined;

  return (
    <DecoderShell bottom={p.bottomOffset} maxWidth={SAT_MAX_W}>
      <DecoderHeader>
        <View style={[s.dot, !!r && r.state === 'TRACK' && s.dotOn]} />
        <DecoderTitle>SATELLITES</DecoderTitle>
        <View style={{ flex: 1 }} />
        <DecoderKey label="DOPPLER" active={!!r} disabled={greyed} onPress={() => p.onDoppler(!r)}
                    accessibilityLabel={greyed ? `Doppler correction unavailable: ${p.cap.why}` : r ? 'Doppler correction off' : 'Doppler correction: keep the satellite centred for the whole pass'} />
        <DecoderKey label="CLOSE" onPress={p.onClose} accessibilityLabel="Close the satellites box (a lock carries on)" />
      </DecoderHeader>
      {greyed && <Text style={s.note}>{p.cap.why}</Text>}
      {!!p.readout && p.readout.state === 'OFF' && !!p.readout.why && <Text style={s.note}>{p.readout.why}</Text>}
      <DecoderBody>
        <ScrollView style={{ maxHeight: bodyH }} contentContainerStyle={scrollLane} showsVerticalScrollIndicator>
          {!!r && (
            <View style={[s.ro, r.state === 'COAST' && s.coast]} accessibilityLabel={`${satChanText(r)}, ${satStateWord(r)}`}>
              <View style={s.txtCol}>
                <Text style={s.big}>{satChanText(r)}</Text>
                <Text style={s.txt}>{satStateWord(r)}{r.tx ? ` · ${r.tx}` : ''}</Text>
                <Text style={s.detail}>{satStatusLine(r, now)}</Text>
                {r.az != null && r.el != null && r.state !== 'ARMED' &&
                  <Text style={s.detail}>{`AZ ${Math.round(r.az)}° · EL ${Math.round(r.el)}° · ${(r.rate ?? 0).toFixed(0)} Hz/s`}</Text>}
                {!!sel && !!ageText(sel) && <Text style={s.detail}>{ageText(sel)}</Text>}
              </View>
              <SkyPlot track={p.track && p.track.norad === r.norad ? p.track : null} now={r} color={tk.value} dim={tk.muted} />
            </View>
          )}
          {!p.list ? <Text style={s.note}>Asking the server…</Text>
            : !rows.length ? <Text style={s.note}>{p.list.why || 'No listed satellite can be heard on this receiver'}</Text>
            : rows.slice(0, 20).map((row, i) => {
                const x = row.sat;
                const when = row.up ? `UP · EL ${Math.round(x.el ?? 0)}°`
                  : row.next ? `AOS ${new Date(row.next.aos * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })} (in ${clockIn(row.next.aos, now)}) · max ${Math.round(row.next.maxEl)}°`
                  : 'no pass in the next day';
                const dis = greyed || x.ageState === 'expired' || x.ageState === 'none';
                return (
                  <View key={x.norad} style={[s.row, i % 2 === 0 && s.rowAlt]}>
                    <View style={s.nameRow}>
                      <Text style={s.txt} numberOfLines={1}>{x.name}</Text>
                      <Text style={s.detail} numberOfLines={1}>{when}</Text>
                    </View>
                    {!!x.note && <Text style={s.detail}>{x.note}</Text>}
                    {!!ageText(x) && <Text style={s.detail}>{ageText(x)}</Text>}
                    <View style={s.keys}>
                      {x.tx.map((t, k) => !t.reach ? null : (
                        <DecoderKey key={k} label={`${t.label} · ${txText(t)}`} disabled={dis} onPress={() => p.onTrack(x.norad, t)}
                                    accessibilityLabel={`Track ${x.name} ${t.label}, ${txText(t)}`} />
                      ))}
                    </View>
                  </View>
                );
              })}
          {p.list?.time === 'system' && <Text style={s.detail}>TIME: SYSTEM — this server's clock has not been checked against a time server</Text>}
        </ScrollView>
      </DecoderBody>
    </DecoderShell>
  );
}

const F = DECODER_FONT;
const makeStyles = (T: DecoderTokens) => StyleSheet.create({
  dot:     { width: 8, height: 8, borderRadius: 4, backgroundColor: T.muted, opacity: 0.6 },
  dotOn:   { backgroundColor: T.good, opacity: 1 },
  ro:      { flexDirection: 'row', gap: 10, padding: 10, alignItems: 'center' },
  coast:   { opacity: 0.6 },
  big:     { fontFamily: F, fontSize: 16, color: T.value, fontVariant: ['tabular-nums'], ...engraveStyle(T) },
  row:     { paddingHorizontal: 10, paddingVertical: 6, gap: 3 },
  rowAlt:  { backgroundColor: T.rowActive },
  nameRow: { flexDirection: 'row', justifyContent: 'space-between', gap: 8 },
  keys:    { flexDirection: 'row', flexWrap: 'wrap', gap: 6, marginTop: 2 },
  txt:     { fontFamily: F, fontSize: 12, lineHeight: 17, color: T.value, flexShrink: 1 },
  txtCol:  { flex: 1, minWidth: 0 },
  detail:  { fontFamily: F, fontSize: 11, lineHeight: 15, color: T.muted, flexShrink: 1 },
  note:    { fontFamily: F, fontSize: 13, color: T.muted, padding: 12, lineHeight: 18 },
});
