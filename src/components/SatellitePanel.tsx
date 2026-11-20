/**
 * ★★★ THE SATELLITE MODE's box on a VibeServer (V12, docs/v12/SATELLITE-MODE.md, Stuart 2026-11-20) — ONE whole-radio mode
 * for every satellite; it replaced the SAT picker and the METEOR LRPT decoder key. The decoder boxes' place and look
 * (DecoderShell — every faceplate, transparency on/off), the words and the half-globe's geometry of src/services/satellites.ts
 * (the web's satellites.ts says and draws the same).
 *
 *   header:  ● SATELLITE · AUTO DOPPLER (default ON) · EXIT
 *   the pick (once the server says "picked"):
 *     voice / packet → COMPACT: name · downlink, the corrected frequency, "In range for 9:55" (or the next pass), the
 *                      decoder's line, the APRS lines (packet), and a half-globe (react-native-svg — no tiles, no data):
 *                      the bird above its sub-point, its beam cone to the ground, the receiver on the dome
 *     image          → BIG: the picture as it arrives (DecoderImageCanvas on the screen's decoderImageRef — the decoder box
 *                      stays shut while SATELLITE owns the decoder), the finished picture at pass end
 *     ISS 145.800 (sstv) opens compact and grows when an SSTV picture starts (the canvas is mounted, hidden, from the pick)
 *   "Orbits synced 16:08 · 2 h ago" / "Fetching orbit data…" / "No orbit data — why"
 *   the timetable: name · frequency · "UP · 6:12 left" / "AOS 16:42 (in 0:38)" — up now SHORTEST remaining first, then the
 *     next passes; what this radio cannot hear GREYED with why (the list is a timetable, so greying is the rule here)
 *
 * ★★ THE SERVER DOES THE WORK (THE CONTRACT): a row asks {"op":"pick"}; the server tunes, locks and attaches the decoder.
 *    ★ No flashing: COAST dims the readout.
 */
import React, { useEffect, useMemo, useState } from 'react';
import { Pressable, ScrollView, StyleSheet, Text, View, useWindowDimensions } from 'react-native';
import Svg, { Circle, Path, Rect } from 'react-native-svg';
import { DecoderShell, DecoderHeader, DecoderTitle, DecoderKey, DecoderBody, DECODER_FONT, decoderBodyInset,
         useDecoderTokens, useDecoderStyles, engraveStyle, type DecoderTokens } from './DecoderShell';
import DecoderImageCanvas, { type DecoderImageHandle } from './DecoderImageCanvas';
import { scrollLane } from '../constants/popupTokens';
import { globeScene, satAltKm, satBoxSize, satCorrectedHz, satCountdownText, satCreditText, satHzText, satModeRows, satStateWord,
         satSyncedText, satTxCreditText, satWhenText, txText, txUse,
         type SatInfo, type SatList, type SatPicked, type SatReadout } from '../services/satellites';

const SAT_MAX_W = 760;
/** ★ The half-globe's drawing size (viewBox); on screen it is the box's width, capped at GLOBE_MAX_W (Stuart 11-20). */
const GLOBE_W = 640, GLOBE_H = 300, GLOBE_MAX_W = 420;

export interface SatellitePanelProps {
  list: SatList | null;
  readout: SatReadout | null;
  /** hwinfo's doppler / dopplerWhy: ok false = greyed, `why` shown. */
  cap: { ok: boolean; why: string };
  autoDoppler: boolean;
  picked: SatPicked | null;
  pending: { norad: number; tx: number } | null;
  /** a refused pick's reason (the server's OFF + why), or a greyed row's why after a press */
  why: string;
  /** a picture has started on the pick's image decoder (the ISS SSTV box grows) / finished (pass end) */
  picture: boolean;
  pictureDone: boolean;
  /** the decoder's own line (LRPT's carrier / lines, SSTV's mode, APRS listening) */
  decLine: string;
  /** APRS lines heard on a packet downlink, newest first */
  packets: string[];
  /** the receiver (server) position — null = no globe */
  rxPos: { lat: number; lon: number } | null;
  imageRef: React.MutableRefObject<DecoderImageHandle | null>;
  onPick: (norad: number, tx: number) => void;
  onGreyed: (why: string) => void;
  onDoppler: () => void;
  onExit: () => void;
  bottomOffset: number;
  topLimit?: number;
}

/** ★ The half-globe (spec item 4): the scene's paths in react-native-svg. */
function HalfGlobe({ rx, sat, ink, beam }: { rx: { lat: number; lon: number }; sat: SatInfo | undefined; ink: string; beam: string }) {
  const g = useMemo(() => globeScene(GLOBE_W, GLOBE_H, rx,
    sat && sat.lat != null && sat.lon != null ? { lat: sat.lat, lon: sat.lon, altKm: satAltKm(sat.rangeKm, sat.el) } : null),
  [rx.lat, rx.lon, sat?.lat, sat?.lon, sat?.rangeKm, sat?.el]);   // eslint-disable-line react-hooks/exhaustive-deps
  const dome = `M${(g.cx - g.r).toFixed(1)} ${g.cy.toFixed(1)}A${g.r.toFixed(1)} ${g.r.toFixed(1)} 0 0 1 ${(g.cx + g.r).toFixed(1)} ${g.cy.toFixed(1)}Z`;
  return (
    <View style={{ width: '100%', maxWidth: GLOBE_MAX_W, aspectRatio: GLOBE_W / GLOBE_H, alignSelf: 'center' }}>
    <Svg width="100%" height="100%" viewBox={`0 0 ${GLOBE_W} ${GLOBE_H}`}
         accessibilityLabel="Where the satellite is: above the globe, its beam to the ground">
      <Path d={dome} fill={ink} fillOpacity={0.12} stroke={ink} strokeOpacity={0.55} strokeWidth={2} />
      {!!g.grid && <Path d={g.grid} fill="none" stroke={ink} strokeOpacity={0.22} strokeWidth={1} />}
      {!!g.coast && <Path d={g.coast} fill="none" stroke={ink} strokeOpacity={0.55} strokeWidth={1} />}
      {!!g.foot && <Path d={g.foot} fill={beam} fillOpacity={0.28} />}
      {!!g.beam && <Path d={g.beam} fill={beam} fillOpacity={0.35} />}
      {!!g.rx && <Circle cx={g.rx.x} cy={g.rx.y} r={4} fill={ink} />}
      {!!g.sat && <>
        <Rect x={g.sat.x - 4} y={g.sat.y - 4} width={8} height={8} fill={beam} />
        <Path d={`M${g.sat.x - 14} ${g.sat.y}H${g.sat.x - 6}M${g.sat.x + 6} ${g.sat.y}H${g.sat.x + 14}`} stroke={beam} strokeWidth={2} />
        <Rect x={g.sat.x - 20} y={g.sat.y - 4} width={6} height={8} fill={beam} />
        <Rect x={g.sat.x + 14} y={g.sat.y - 4} width={6} height={8} fill={beam} />
      </>}
    </Svg>
    </View>
  );
}

export default function SatellitePanel(p: SatellitePanelProps) {
  const s = useDecoderStyles(makeStyles);
  const tk = useDecoderTokens();
  const { height: winH } = useWindowDimensions();
  const HEADER_H = 46 + 22;
  const bodyH = Math.round(p.topLimit != null
    ? Math.max(140, winH - p.bottomOffset - p.topLimit - HEADER_H - decoderBodyInset(tk)) : 320);
  // ★ the countdowns tick on between lists
  const [, setTick] = useState(0);
  useEffect(() => { const t = setInterval(() => setTick((n) => n + 1), 1000); return () => clearInterval(t); }, []);
  const [listOpen, setListOpen] = useState(true);
  useEffect(() => { if (p.picked) setListOpen(false); }, [p.picked?.norad, p.picked?.tx]);   // eslint-disable-line react-hooks/exhaustive-deps
  const now = Date.now() / 1000;
  const greyed = !p.cap.ok;
  const rows = satModeRows(p.list, now);
  const sync = satSyncedText(p.list, now);
  const credit = satCreditText(p.list, now), txc = satTxCreditText(p.list);

  const sel = p.picked ?? p.pending;
  const sat = sel ? p.list?.sats.find((x) => x.norad === sel.norad) : undefined;
  const tx = sel ? sat?.tx[sel.tx] : undefined;
  const confirmed = !!p.picked && !!tx;
  const mine = p.readout && p.readout.state !== 'OFF' && p.readout.norad === sat?.norad ? p.readout : null;
  const big = confirmed && satBoxSize(tx, p.picture) === 'big';
  const use = tx ? txUse(tx) : 'voice';
  const imageDec = p.picked?.decoder === 'lrpt' || p.picked?.decoder === 'sstv';
  const lockWord = !p.autoDoppler ? 'DOPPLER OFF' : mine ? satStateWord(mine) : '';
  const decWord = !confirmed ? 'Tuning…'
    : big && p.pictureDone ? 'picture complete — saved to the gallery'
    : p.decLine || (use === 'image' ? 'waiting for the picture' : use === 'packet' ? 'listening for packets' : tx?.sstv ? 'listening — an SSTV picture opens here' : '');
  const stateLine = [confirmed ? lockWord : '', decWord].filter(Boolean).join(' · ');

  return (
    <DecoderShell bottom={p.bottomOffset} maxWidth={SAT_MAX_W}>
      <DecoderHeader>
        <View style={[s.dot, !!mine && mine.state === 'TRACK' && s.dotOn]} />
        <DecoderTitle>SATELLITE</DecoderTitle>
        <View style={{ flex: 1 }} />
        <DecoderKey label="AUTO DOPPLER" active={p.autoDoppler} disabled={greyed} onPress={p.onDoppler}
                    accessibilityLabel={greyed ? `Auto Doppler unavailable: ${p.cap.why}` : `Auto Doppler ${p.autoDoppler ? 'on' : 'off'}: keep the satellite centred for the whole pass`} />
        <DecoderKey label="EXIT" onPress={p.onExit} accessibilityLabel="Leave SATELLITE (the Doppler lock and the decoder stop)" />
      </DecoderHeader>
      {(greyed || !!p.why) && <Text style={s.note}>{greyed ? p.cap.why : p.why}</Text>}
      <DecoderBody>
        <ScrollView style={{ maxHeight: bodyH }} contentContainerStyle={scrollLane} showsVerticalScrollIndicator>
          {!!sel && !!sat && !!tx && (
            <View style={s.box}>
              <View style={[s.ro, mine?.state === 'COAST' && s.coast]}
                    accessibilityLabel={`${sat.name} ${tx.label}, ${satHzText(satCorrectedHz(tx, mine))}, ${satCountdownText(sat, now)}`}>
                <View style={s.txtCol}>
                  <Text style={s.txt} numberOfLines={1}>{`${sat.name} · ${tx.label}`}</Text>
                  <Text style={s.big}>{satHzText(satCorrectedHz(tx, mine))}</Text>
                  <Text style={s.txt}>{satCountdownText(sat, now)}</Text>
                  {!!stateLine && <Text style={s.detail}>{stateLine}</Text>}
                </View>
                {!!p.rxPos && <HalfGlobe rx={p.rxPos} sat={sat} ink={tk.value} beam={tk.accent} />}
              </View>
              {/* ★ the picture: mounted from the pick while the decoder draws one (so an ISS SSTV frame's first lines are
                   not lost before the box grows), shown only BIG */}
              {confirmed && imageDec && (
                <View style={big ? s.img : s.hidden}>
                  <DecoderImageCanvas ref={p.imageRef} maxHeight={Math.max(160, Math.min(440, bodyH - 150))}
                    decoderName={p.picked!.decoder === 'lrpt' ? 'meteor' : 'sstv'}
                    onInfo={() => {}} onStatus={() => {}} onPrevState={() => {}} />
                </View>
              )}
              {confirmed && use === 'packet' && (
                <View style={s.pk}>
                  {p.packets.length ? p.packets.slice(0, 12).map((l, i) => <Text key={i} style={s.mono} numberOfLines={2}>{l}</Text>)
                    : <Text style={s.detail}>no packets yet</Text>}
                </View>
              )}
            </View>
          )}
          {!!sync && <Text style={s.detail}>{sync}</Text>}
          <Pressable onPress={() => setListOpen((v) => !v)} accessibilityRole="button" accessibilityState={{ expanded: listOpen }}
                     style={s.sumRow}>
            <Text style={s.sum}>{`${listOpen ? '▾' : '▸'} ${p.picked ? 'ALL SATELLITES' : 'SATELLITES'}`}</Text>
          </Pressable>
          {listOpen && (!p.list ? <Text style={s.note}>Asking the server…</Text>
            : !rows.length ? <Text style={s.note}>{p.list.why || (p.list.source?.fetching ? 'The satellites appear once the orbit data has arrived' : 'No satellites are listed on this server')}</Text>
            : rows.slice(0, 40).map((r) => {
                const on = !!sel && sel.norad === r.sat.norad && sel.tx === r.i;
                const name = r.sat.tx.length > 1 ? `${r.sat.name} · ${r.tx.label}` : r.sat.name;
                return (
                  <Pressable key={`${r.sat.norad}.${r.i}`}
                    onPress={() => (r.grey || greyed ? p.onGreyed(r.grey ? r.why : p.cap.why) : p.onPick(r.sat.norad, r.i))}
                    accessibilityRole="button" accessibilityState={{ disabled: r.grey || greyed, selected: on }}
                    accessibilityLabel={`${name}, ${txText(r.tx)}, ${satWhenText(r, now)}${r.grey ? `, unavailable: ${r.why}` : ''}`}
                    style={[s.row, on && s.rowOn, (r.grey || greyed) && s.grey]}>
                    <View style={s.nameRow}>
                      <Text style={s.txt} numberOfLines={1}>{name}</Text>
                      <Text style={[s.txt, r.up && s.up]} numberOfLines={1}>{satWhenText(r, now)}</Text>
                    </View>
                    <Text style={s.detail}>{`${txText(r.tx)}${r.tx.status === 'inactive' ? ' · INACTIVE' : ''}${r.grey ? ` — ${r.why}` : ''}`}</Text>
                  </Pressable>
                );
              }))}
          {p.list?.time === 'system' && <Text style={s.detail}>TIME: SYSTEM — this server's clock has not been checked against a time server</Text>}
          {!!credit && <Text style={s.detail}>{credit}{txc ? ` · ${txc}` : ''}</Text>}
        </ScrollView>
      </DecoderBody>
    </DecoderShell>
  );
}

const F = DECODER_FONT;
const makeStyles = (T: DecoderTokens) => StyleSheet.create({
  dot:     { width: 8, height: 8, borderRadius: 4, backgroundColor: T.muted, opacity: 0.6 },
  dotOn:   { backgroundColor: T.good, opacity: 1 },
  box:     { margin: 8, padding: 8, borderRadius: 8, borderWidth: StyleSheet.hairlineWidth, borderColor: T.muted, gap: 6 },
  ro:      { flexDirection: 'column', gap: 6, alignItems: 'stretch' },   // ★ the globe on its own row, full width
  coast:   { opacity: 0.6 },
  big:     { fontFamily: F, fontSize: 18, color: T.value, fontVariant: ['tabular-nums'], ...engraveStyle(T) },
  img:     { marginTop: 4 },
  hidden:  { display: 'none' },
  pk:      { gap: 2, marginTop: 2 },
  mono:    { fontFamily: F, fontSize: 11, lineHeight: 15, color: T.value },
  sumRow:  { paddingHorizontal: 10, paddingVertical: 8, minHeight: 36, justifyContent: 'center' },
  sum:     { fontFamily: F, fontSize: 12, letterSpacing: 1, color: T.value },
  row:     { paddingHorizontal: 10, paddingVertical: 7, gap: 2, minHeight: 44, justifyContent: 'center' },
  rowOn:   { backgroundColor: T.rowActive },
  grey:    { opacity: 0.45 },
  up:      { color: T.accent },
  nameRow: { flexDirection: 'row', justifyContent: 'space-between', gap: 8 },
  txt:     { fontFamily: F, fontSize: 12, lineHeight: 17, color: T.value, flexShrink: 1 },
  txtCol:  { flex: 1, minWidth: 0, gap: 1 },
  detail:  { fontFamily: F, fontSize: 11, lineHeight: 15, color: T.muted, flexShrink: 1, paddingHorizontal: 10 },
  note:    { fontFamily: F, fontSize: 13, color: T.muted, padding: 12, lineHeight: 18 },
});
