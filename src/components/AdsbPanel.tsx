/**
 * ADS-B on a VibeServer (V12, docs/v12/ADSB.md) — the decoder box, NOW FOR APPLE TV ONLY.
 *
 * ★★★ EVERYWHERE ELSE ADS-B'S SCREEN IS THE FULL-SCREEN MAP (MapOverlay 'adsb' + AdsbLiveList; Stuart, 2026-10-24:
 *     "when ADS-B is selected the waterfall drops and the full-screen map is on"), which superseded this box and its
 *     MAP key. tvOS has no WebView, so the map cannot exist there and this box stays the whole interface — without
 *     the MAP key, which would open nothing. The notes below are from when it was the interface everywhere.
 *
 * ★★★ BUILT LIKE THE DAB BOX, BECAUSE IT IS THE SAME KIND OF THING. Choosing ADS-B takes the whole
 *     radio to 1090 MHz and the box IS the interface: there is nothing to tune, so the list of what
 *     is being heard is the screen (Stuart, 2026-10-23: "like DAB … opens the decoder box").
 *     The body is the existing AircraftPanel — the same table OWRX's ADS-B fills — and the header
 *     carried MAP (the first spec: the map button IN THE BOX HEADER — superseded, see above) and EXIT ADS-B.
 * ★★ NO ✕, for the DAB box's reason (2026-09-20): closing the window would leave the receiver in a
 *    mode with nothing on screen to control it. EXIT ADS-B leaves the mode and the window together;
 *    SMALL is there for anyone who wants the waterfall back. After a REFUSAL (nothing to leave) the
 *    same key reads CLOSE and only puts the box away.
 * ★ The decoder font throughout (DecoderShell) — the display font is for the main controls only.
 */
import React from 'react';
import { StyleSheet, Text, View, useWindowDimensions } from 'react-native';
import { DecoderShell, DecoderHeader, DecoderTitle, DecoderKey, DecoderBody, DECODER_FONT, decoderBodyInset,
         useDecoderTokens, useDecoderStyles, engraveStyle, type DecoderTokens } from './DecoderShell';
import AircraftPanel from './AircraftPanel';
import type { Aircraft } from '../services/SDRBackend';
import { adsbStatusText, type AdsbReport } from '../services/adsbTypes';

/** ★ The table is four narrow columns; wider than this is padding (DecoderPanel's PANEL_MAX_W reasoning). */
const ADSB_MAX_W = 560;

export interface AdsbPanelProps {
  /** The last report, or null while the server has not sent one (starting). */
  report: AdsbReport | null;
  /** The same report in the app's shape (adsbToAircraft) — what the table and the watch read. */
  aircraft: Aircraft[];
  /** The server's refusal (`adsb_error`) — an explanation, shown in the body, not a red card. */
  error?: string;
  /** True while the receiver is in ADS-B; false after a refusal (the exit key then only closes). */
  on: boolean;
  onExit: () => void;
  bottomOffset: number;
  tall: boolean;
  onTall: (v: boolean) => void;
  /** ★ SDRScreen's boxTopLimit / boxTopSafe — the same room rules as every other box. */
  topLimit?: number;
  topSafe?: number;
}

export default function AdsbPanel(p: AdsbPanelProps) {
  const s = useDecoderStyles(makeStyles);
  const dtk = useDecoderTokens();
  const { height: winH } = useWindowDimensions();
  /* ★★ THE DAB BOX'S SIZING, unchanged (DabPanel): SMALL a fixed 230 or the room down from topLimit; BIG up to the
   *  status row; BIG offered only where it gains at least 40 pt — on an SE in Display Zoom it would not. */
  const HEADER_H = 46;
  const smallBody = p.topLimit != null
    ? Math.max(100, winH - p.bottomOffset - p.topLimit - HEADER_H - decoderBodyInset(dtk)) : 230;
  const bigBody = p.topSafe != null
    ? Math.max(140, winH - p.bottomOffset - p.topSafe - HEADER_H - decoderBodyInset(dtk))
    : Math.max(140, winH - p.bottomOffset - 280);
  const bigUseful = bigBody >= smallBody + 40;
  const tall = p.tall && bigUseful;
  const bodyH = Math.round(tall ? bigBody : smallBody);

  const status = p.error ? 'not available' : adsbStatusText(p.report);

  return (
    <DecoderShell bottom={p.bottomOffset} maxWidth={ADSB_MAX_W} tall={tall}>
      <DecoderHeader>
        <View style={[s.dot, p.on && !!p.report?.aircraft.length && s.dotOn]} />
        <DecoderTitle>ADS-B</DecoderTitle>
        {/* ★ The text gives way, never the keys (the DAB box's SE lesson): EXIT must always be reachable. */}
        <Text style={[s.status, { flex: 1, flexShrink: 1, minWidth: 0 }]} numberOfLines={1}>{status}</Text>
        {bigUseful && <DecoderKey active={tall} onPress={() => p.onTall(!tall)} label={tall ? 'SMALL' : 'BIG'} />}
        <DecoderKey onPress={p.onExit} label={p.on ? 'EXIT ADS-B' : 'CLOSE'} />
      </DecoderHeader>
      <DecoderBody>
        {/* ★ HEIGHT, not maxHeight — AircraftPanel is flex-based and collapses to nothing without one
            (the note in DecoderPanel where the OWRX table is drawn). */}
        <View style={{ height: bodyH }}>
          {p.error ? (
            <Text style={s.note}>{p.error}</Text>
          ) : p.aircraft.length ? (
            <AircraftPanel aircraft={p.aircraft} />
          ) : (
            <Text style={s.note}>
              {p.report ? 'Listening on 1090 MHz — no aircraft heard yet.' : 'Taking the receiver to 1090 MHz…'}
            </Text>
          )}
        </View>
      </DecoderBody>
    </DecoderShell>
  );
}

const F = DECODER_FONT;
const makeStyles = (T: DecoderTokens) => StyleSheet.create({
  dot:    { width: 8, height: 8, borderRadius: 4, backgroundColor: T.muted, opacity: 0.6 },
  dotOn:  { backgroundColor: T.good, opacity: 1 },
  status: { fontFamily: F, fontSize: 12, color: T.hdrValue, ...engraveStyle(T) },
  note:   { fontFamily: F, fontSize: 13, color: T.muted, padding: 12, lineHeight: 18 },
});
