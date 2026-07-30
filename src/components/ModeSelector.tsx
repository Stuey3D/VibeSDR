import React, { useEffect, useMemo, useRef, useState } from 'react';
import {
  Modal, Pressable, ScrollView, StyleSheet, Text, TouchableOpacity, useWindowDimensions, View,
} from 'react-native';
import Slider from '@react-native-community/slider';
import { Mode, MODES } from '../services/sdrTypes';
import { useTheme } from '../contexts/ThemeContext';
import { NavCtx, NavRow, usePanelNav, useNavButton, useNavRange, NAV_FOCUS, noteTouchInteraction, useKeyboardMode } from './PanelNav';
import { NativeEventEmitter, NativeModules } from 'react-native';
import { RTTY_PRESETS, type RttySettings } from '../services/DecoderClient';

type DecId = 'rtty' | 'navtex' | 'wefax' | 'sstv' | 'morse' | 'whisper';

const BW_GOLD  = '#ffe566';
const BW_MUTED = 'rgba(255,255,255,0.92)';
// ── Keyboard / D-pad navigation (shared machinery — PanelNav) ────────────────
// NavItem is a render prop rather than a wrapper component because every button in
// this panel has bespoke inline styling; this way focus is threaded in without any
// of that being rewritten.
/**
 * A row inside the digital/decoders dropdown.
 *
 * ★ The generic NavItem reveals against the SHEET's ScrollView, but these rows live in their
 * own nested list — so focusing one scrolled the sheet while the list it was in stayed put,
 * and the caret walked off the bottom of the dropdown. This scrolls the list it is actually
 * in, using the y each row already records on layout.
 */
/**
 * A bandwidth slider that takes part in the focus order.
 *
 * ★ Each takes its OWN row rather than sharing one with SYNC between them: a focused range
 * consumes left/right to change its value, so if all three shared a row you could never move
 * between them. Up and down step slider → SYNC → slider, which still reads left to right.
 */
function NavSlider(props: React.ComponentProps<typeof Slider>) {
  const { minimumValue = 0, maximumValue = 1, step, value = 0, onValueChange } = props;
  const nudge = step && step > 0 ? step : (maximumValue - minimumValue) / 20;
  const { focused, viewRef } = useNavRange((dir) => {
    const next = Math.max(minimumValue, Math.min(maximumValue, value + dir * nudge));
    if (next !== value) onValueChange?.(next);
  });
  return (
    <Slider ref={viewRef as any} {...props}
      minimumTrackTintColor={focused ? NAV_FOCUS : props.minimumTrackTintColor}
      thumbTintColor={focused ? NAV_FOCUS : props.thumbTintColor} />
  );
}

function MoreItem({ onPress, onReveal, children }: {
  onPress?: () => void;
  onReveal: () => void;
  children: (focused: boolean, ref: React.MutableRefObject<View | null>) => React.ReactNode;
}) {
  const { focused, viewRef } = useNavButton(onPress);
  const rev = useRef(onReveal); rev.current = onReveal;
  useEffect(() => { if (focused) rev.current(); }, [focused]);
  return <>{children(focused, viewRef)}</>;
}

function NavItem({ onPress, children }: {
  onPress?: () => void;
  children: (focused: boolean, ref: React.MutableRefObject<View | null>) => React.ReactNode;
}) {
  const { focused, viewRef } = useNavButton(onPress);
  return <>{children(focused, viewRef)}</>;
}

function fmtHz(hz: number) {
  return hz >= 1000 ? (hz / 1000).toFixed(1) + ' kHz' : hz + ' Hz';
}

// Common analog/voice demodulators shown as buttons; everything else the server
// offers (digital, decoders, sondes…) goes into the in-popup dropdown.
const COMMON_IDS = ['nfm', 'fm', 'wfm', 'am', 'sam', 'lsb', 'usb', 'cw', 'cwu', 'cwl', 'data'];
const DEC_COL = '#52dc64';   // active-decoder accent (matches VTS live-data green)

// ── Decoder-settings helpers (moved with the decoders from MenuSheet §4.3) ──────
function SubLabel({ label, small }: { label: string; small?: boolean }) {
  return <Text style={[dst.subLabel, small && { fontSize: 9, opacity: 0.7 }]}>{label}</Text>;
}
function OptRow({ children }: { children: React.ReactNode }) {
  return <NavRow><View style={dst.optRow}>{children}</View></NavRow>;
}
function SegBtn({ label, active, onPress }: { label: string; active: boolean; onPress: () => void }) {
  const { focused, viewRef } = useNavButton(onPress);
  return (
    <TouchableOpacity ref={viewRef as any}
      style={[dst.seg, active && dst.segActive, focused && { borderColor: NAV_FOCUS, borderWidth: 2 }]}
      onPress={onPress} activeOpacity={0.7}>
      <Text style={[dst.segText, active && dst.segTextActive]}>{label}</Text>
    </TouchableOpacity>
  );
}
function RttySettingsRows({ s, onChange }: { s: RttySettings; onChange: (s: RttySettings) => void }) {
  const presetKey = Object.entries(RTTY_PRESETS).find(([, p]) =>
    p.shift === s.shift && p.baud === s.baud && p.encoding === s.encoding && p.inverted === s.inverted)?.[0] ?? '';
  return (
    <>
      <SubLabel label="Preset" />
      <OptRow>{([['ham', 'HAM'], ['weather', 'WX'], ['sitor-b', 'SITOR-B']] as const).map(([k, l]) => (
        <SegBtn key={k} label={l} active={presetKey === k} onPress={() => onChange({ ...RTTY_PRESETS[k] })} />
      ))}</OptRow>
      <SubLabel label="Shift (Hz)" />
      <OptRow>{[170, 200, 425, 450, 850].map(v => (
        <SegBtn key={v} label={String(v)} active={s.shift === v} onPress={() => onChange({ ...s, shift: v })} />
      ))}</OptRow>
      <SubLabel label="Baud" />
      <OptRow>{[45.45, 50, 75, 100].map(v => (
        <SegBtn key={v} label={String(v)} active={s.baud === v} onPress={() => onChange({ ...s, baud: v })} />
      ))}</OptRow>
      <SubLabel label="Encoding" />
      <OptRow>{(['ITA2', 'ASCII', 'CCIR476'] as const).map(v => (
        <SegBtn key={v} label={v} active={s.encoding === v} onPress={() => onChange({ ...s, encoding: v })} />
      ))}</OptRow>
      <OptRow>
        <SegBtn label={s.inverted ? 'INVERT: ON' : 'INVERT: OFF'} active={s.inverted}
                onPress={() => onChange({ ...s, inverted: !s.inverted })} />
      </OptRow>
    </>
  );
}

interface ModeSelectorProps {
  visible:  boolean;
  current:  Mode;
  /** Gated demodulator list (OWRX reports its own, incl. WFM/digital). When
   *  absent, the default UberSDR MODES are shown. */
  modes?:   { id: string; label: string }[];
  /** OWRX secondary decoder running on top of the carrier (e.g. 'sstv'/'fax').
   *  Highlighted separately so the carrier demod (`current`) stays lit too. */
  activeDecoder?: string;
  onSelect: (mode: Mode) => void;
  onClose:  () => void;
  /** ★★★ THE QUICK-ACCESS GAIN SLIDER WAS REMOVED (2026-07-30). It only ever spoke ONE radio's
   *  gain model — the RTL's list of discrete tuner gains — and three are supported: the Airspy HF+
   *  has no variable gain at all (fixed front end, +6 dB preamp, 0-48 dB attenuator in 6 dB steps)
   *  and the SDRplay RSP uses IF gain reduction with an LNA state table. So on two of the three it
   *  was a live-looking control that did nothing, or worse, described the hardware wrongly.
   *  ★★ LocalHardwarePanel already branches on radio.driver and draws the RIGHT control for each —
   *  that is where gain belongs. Stuart: "I would rather a control be removed if it only works in
   *  one scenario than keep it there dead."
   *  If it comes back, it must be per-radio like the panel, not a second copy of the RTL slider. */
  /** Bandwidth (passband) — a demodulator control, so it lives here under the
   *  mode grid. Mirrored sliders around the carrier; SYNC mirrors both edges. */
  filterLow?:    number;
  filterHigh?:   number;
  bwEdgeMax?:    number;   // per-edge half-width cap (Hz) for the active mode
  onFilterBoth?: (low: number, high: number) => void;
  // SERVER MAPS (relocated from MenuSheet §4.4). SDRScreen computes the guard
  // (UberSDR feeds only — hidden for OWRX/Kiwi/local); we just render the buttons.
  showServerMaps?: boolean;
  onServerMap?:    (kind: 'hfdl' | 'digi' | 'cw') => void;

  // CLIENT DECODERS (relocated from MenuSheet §4.3). null = don't show (OWRX, or
  // landscape on a non-tablet where the settings callout has no room). SDRScreen bundles
  // the state so the interface stays small.
  decoderControls?: {
    decMode: string | null; decOn: boolean; isLocal: boolean;
    onDecToggle: (m: DecId) => void;
    /** ★ ADVANCED RDS is NOT a DecoderClient decoder — it is a server-side analyser switched
     *  by one control message, so it deliberately does not join the DecId union. It sits in
     *  this row because that is where a user looks for "show me more about this signal",
     *  and it appears only on a VibeServer in WFM, where it is the only place it means
     *  anything. Offering a dead button elsewhere reads as broken support, not as
     *  "unavailable here". */
    advRdsAvail?: boolean; advRdsOn?: boolean; onAdvRds?: () => void;
    rttySettings?: RttySettings; onRttySettings?: (s: RttySettings) => void;
    wefaxLpm?: number; onWefaxLpm?: (v: number) => void;
  } | null;
  // OWRX MAP + FILES pages (relocated from MenuSheet OPENWEBRX section) — same
  // "what's on this signal" family. null = not OWRX.
  owrxPages?: { onMap: () => void; onFiles: () => void } | null;
  // SERVER EXTENSIONS / DECODED SPOTS (relocated from MenuSheet §4.3). null = don't show.
  spotsControls?: {
    label: string; spotsKind: string | null;
    onSpotsToggle: (k: 'digi' | 'cw') => void; onSpotsMap?: () => void;
    showCwStt: boolean; showMap: boolean;
    sttActive: boolean; sttSelected: boolean; onSttToggle: () => void;
  } | null;
}

export default function ModeSelector({ visible, current, modes, activeDecoder, onSelect, onClose,
  filterLow = 0, filterHigh = 0, bwEdgeMax = 6000, onFilterBoth,
  showServerMaps = false, onServerMap, owrxPages,
  decoderControls, spotsControls }: ModeSelectorProps) {
  const { theme: t } = useTheme();
  const { height: winH, width: winW } = useWindowDimensions();
  // ★ "ADV RDS" was an abbreviation forced by nothing — the button spans the whole row and has
  //   room to spare. Only the very narrowest phones need the short form (Stuart, 2026-07-28).
  const advRdsLabel = winW >= 340 ? 'ADVANCED RDS' : 'ADV RDS';
  const isWhite = t.name === 'white';
  const [moreOpen, setMoreOpen] = useState(false);
  const [bwSync, setBwSync] = useState(false);
  const bwStep = bwEdgeMax > 20000 ? 1000 : 50;
  const showBw = onFilterBoth != null;
  // Collapse the decoder dropdown when the sheet closes, so reopening it lands on
  // the current decoder (the scroll-to-active effect only fires on open) instead
  // of staying open scrolled to the top.
  useEffect(() => { if (!visible) setMoreOpen(false); }, [visible]);

  const list = modes && modes.length ? modes : MODES.map(m => ({ id: m, label: m.toUpperCase() }));
  const common = list.filter(m => COMMON_IDS.includes(m.id.toLowerCase()));
  // Decoder/digital list — OWRX reports these in server-add order (no order at
  // all), so sort alphabetically by label to make the long list scannable.
  const others = useMemo(
    () => list.filter(m => !COMMON_IDS.includes(m.id.toLowerCase()))
              .sort((a, b) => a.label.localeCompare(b.label, undefined, { sensitivity: 'base' })),
    [list],
  );
  const currentInOthers = others.find(m => m.id === current);
  const activeDecInOthers = activeDecoder ? others.find(m => m.id === activeDecoder) : undefined;
  // The carrier label for the "Decoding X over Y" caption (e.g. USB).
  const carrierLabel = (common.find(m => m.id === current) ?? list.find(m => m.id === current))?.label.toUpperCase() ?? String(current).toUpperCase();

  // Remember the spot: when the dropdown opens, jump to the active decoder so
  // the user lands where they were instead of scrolling a long list.
  const moreScroll = useRef<ScrollView | null>(null);
  const itemY = useRef<Record<string, number>>({});
  useEffect(() => {
    if (!moreOpen) return;
    const target = activeDecInOthers ?? currentInOthers;   // active decoder, else current mode
    if (!target) return;
    // Read the captured y INSIDE the delay so onLayout has populated it first.
    const id = setTimeout(() => {
      const y = itemY.current[target.id];
      if (y != null) moreScroll.current?.scrollTo({ y: Math.max(0, y - 8), animated: false });
    }, 60);
    return () => clearTimeout(id);
  }, [moreOpen, currentInOthers, activeDecInOthers]);

  const pick = (id: string) => { onSelect(id as Mode); onClose(); };

  // Keyboard / D-pad navigation — shared machinery (PanelNav). Each grid is a NavRow,
  // so up/down moves between grids and left/right within one.
  // ★ [D] AGAIN OPENS THE DECODERS LIST. D opens this card from the main screen, so pressing
  // it again to reach the long digital/decoder list is the obvious follow-through — and it
  // saves arrowing past the whole mode grid to get there. Backspace leaves it without
  // choosing, like every other dropdown.
  //
  // No double-fire on the way in: SDRScreen ignores letters once a panel is open, and this
  // listener only exists while the card is visible.
  const kbSeen = useKeyboardMode();
  useEffect(() => {
    if (!visible) return;
    const emitter = new NativeEventEmitter(NativeModules.VibePowerModule);
    const sub = emitter.addListener('VibeKeyDown', (e: { key: string }) => {
      if (e?.key === 'D' && others.length > 0) setMoreOpen(o => !o);
    });
    return () => sub.remove();
  }, [visible, others.length]);

  const { navCtx, scrollProps } = usePanelNav(visible, {
    onTimeout: onClose,
    // Backspace closes the decoder list first; only then does it mean anything else.
    onBack: () => { if (moreOpen) setMoreOpen(false); },
  });

  return (
    <Modal visible={visible} transparent animationType="slide" onRequestClose={onClose}
           supportedOrientations={['portrait', 'landscape', 'landscape-left', 'landscape-right']}>
      <Pressable style={st.backdrop} onPress={onClose} onTouchStart={noteTouchInteraction} />
      <View style={[st.sheet, { borderTopColor: t.barBorder }]} onTouchStart={noteTouchInteraction}>
        {/* Scrolls when the content (decoders + callout + extensions + maps) overflows on a
            small screen (§7). Capped so big screens render static as before. */}
        <ScrollView {...scrollProps} style={{ maxHeight: winH * 0.82 }} showsVerticalScrollIndicator={false}
                    keyboardShouldPersistTaps="handled">
        <NavCtx.Provider value={navCtx}>
        <Text style={[st.sheetLabel, { color: t.sectionColor, fontFamily: t.font }]}>
          DEMODULATOR
        </Text>
        <NavRow><View style={st.grid}>
          {common.map(m => (
            <NavItem key={m.id} onPress={() => pick(m.id)}>{(navFocused, navRef) => (
            <TouchableOpacity
              ref={navRef as any}
              style={[
                st.btn,
                { borderColor: isWhite ? 'rgba(255,255,255,0.20)' : 'rgba(80,50,0,0.40)',
                  paddingVertical: isWhite ? 12 : 10 },
                m.id === current && { backgroundColor: t.btnActiveBg, borderColor: t.btnActiveBdr },
                navFocused && { borderColor: NAV_FOCUS, borderWidth: 2 },
              ]}
              onPress={() => pick(m.id)}
            >
              <Text style={[
                st.btnText,
                { fontFamily: t.font, fontSize: isWhite ? 15 : 14,
                  color: isWhite ? 'rgba(255,255,255,0.55)' : 'rgba(150,100,30,0.70)' },
                m.id === current && { color: t.btnActiveText },
              ]}>
                {m.label.toUpperCase()}
              </Text>
            </TouchableOpacity>
            )}</NavItem>
          ))}
        </View></NavRow>

        {/* Bandwidth — mirrored sliders around the carrier: slide the LEFT one
            LEFT to widen the lower sideband, the RIGHT one RIGHT to widen the
            upper. SYNC mirrors both edges (AM/FM symmetric). */}
        {showBw && (
          <View style={st.bwMirrorRow}>
            <Text style={st.bwEdgeVal}>{filterLow >= 0 ? '+' : '−'}{fmtHz(Math.abs(filterLow))}</Text>
            <NavSlider style={st.bwHalfSlider}
              minimumValue={-bwEdgeMax} maximumValue={0} step={bwStep}
              value={Math.max(-bwEdgeMax, Math.min(0, filterLow))}
              onValueChange={(v: number) => {
                if (bwSync) onFilterBoth?.(v, -v);
                else        onFilterBoth?.(v, filterHigh);
              }}
              minimumTrackTintColor={BW_MUTED} maximumTrackTintColor={BW_GOLD}
              thumbTintColor={BW_GOLD} />
            <NavRow><NavItem onPress={() => setBwSync(p => !p)}>{(navFocused, navRef) => (
            <TouchableOpacity hitSlop={6} ref={navRef as any}
              style={[st.bwSyncBtn, bwSync && { borderColor: BW_GOLD, backgroundColor: 'rgba(255,200,0,0.12)' },
                      navFocused && { borderColor: NAV_FOCUS, borderWidth: 2 }]}
              onPress={() => setBwSync(p => !p)} activeOpacity={0.7}>
              <Text style={[st.bwSyncTxt, bwSync && { color: BW_GOLD }]}>SYNC</Text>
            </TouchableOpacity>
            )}</NavItem></NavRow>
            <NavSlider style={st.bwHalfSlider}
              minimumValue={0} maximumValue={bwEdgeMax} step={bwStep}
              value={Math.min(bwEdgeMax, Math.max(0, filterHigh))}
              onValueChange={(v: number) => {
                if (bwSync) onFilterBoth?.(-v, v);
                else        onFilterBoth?.(filterLow, v);
              }}
              minimumTrackTintColor={BW_GOLD} maximumTrackTintColor={BW_MUTED}
              thumbTintColor={BW_GOLD} />
            <Text style={st.bwEdgeVal}>{filterHigh < 0 ? '−' : '+'}{fmtHz(Math.abs(filterHigh))}</Text>
          </View>
        )}

        {/* Combo dropdown: all the digital / decoder modes the server offers */}
        {others.length > 0 && (
          <View style={st.moreWrap}>
            <NavRow><NavItem onPress={() => setMoreOpen(o => !o)}>{(navFocused, navRef) => (
            <TouchableOpacity
              ref={navRef as any}
              style={[st.moreHead, { borderColor: t.btnBorder },
                      currentInOthers && { borderColor: t.btnActiveBdr, backgroundColor: t.btnActiveBg },
                      activeDecInOthers && { borderColor: DEC_COL, backgroundColor: 'rgba(80,220,100,0.14)' },
                      navFocused && { borderColor: NAV_FOCUS, borderWidth: 2 }]}
              onPress={() => setMoreOpen(o => !o)}
              activeOpacity={0.8}>
              {kbSeen && (
                <View style={st.keyCap}><Text style={st.keyCapText}>D</Text></View>
              )}
              <Text style={[st.moreHeadText, { fontFamily: t.font },
                            { color: activeDecInOthers ? DEC_COL : currentInOthers ? t.btnActiveText : t.btnText }]} numberOfLines={1}>
                {activeDecInOthers ? activeDecInOthers.label.toUpperCase()
                  : currentInOthers ? currentInOthers.label.toUpperCase()
                  : `DIGITAL / DECODERS (${others.length})`}
              </Text>
              <Text style={[st.moreChevron, { color: t.btnText }]}>{moreOpen ? '▴' : '▾'}</Text>
            </TouchableOpacity>
            )}</NavItem></NavRow>
            {moreOpen && (
              <ScrollView ref={moreScroll} style={[st.moreList, { borderColor: t.btnBorder }]} keyboardShouldPersistTaps="handled">
                {others.map(m => (
                  <NavRow key={m.id}><MoreItem onPress={() => pick(m.id)}
                    onReveal={() => {
                      const y = itemY.current[m.id];
                      if (y != null) moreScroll.current?.scrollTo({ y: Math.max(0, y - 60), animated: true });
                    }}>{(navFocused, navRef) => (
                  <TouchableOpacity
                    ref={navRef as any}
                    // ★ Focus is a background TINT here, not a border. moreItem has only
                    // a hairline BOTTOM border, so adding a 2px border on all sides would
                    // shift every row in the list as focus moved down it.
                    style={[st.moreItem, { borderBottomColor: t.barBorder },
                            navFocused && { backgroundColor: 'rgba(124,255,155,0.16)' }]}
                    onPress={() => pick(m.id)}
                    onLayout={e => { itemY.current[m.id] = e.nativeEvent.layout.y; }}
                    activeOpacity={0.7}>
                    <Text style={[st.moreItemText, { fontFamily: t.font },
                                  { color: m.id === activeDecoder ? DEC_COL : m.id === current ? t.btnActiveText : t.btnText }]}>
                      {m.id === activeDecoder || m.id === current ? '✓ ' : ''}{m.label.toUpperCase()}
                    </Text>
                  </TouchableOpacity>
                  )}</MoreItem></NavRow>
                ))}
              </ScrollView>
            )}
            {/* Advisory only for decoders that ride on a carrier (RTTY/WEFAX/SSTV
                etc., where the decoder id differs from the current demod). OWRX
                decodes whatever sideband you're on — we don't force it. Standalone
                decoders (ADSB/POCSAG, where current === the decoder) need nothing. */}
            {!!activeDecInOthers && activeDecoder !== current && (
              <Text style={[st.decCaption, { fontFamily: t.font }]}>
                ⚠ {activeDecInOthers.label.toUpperCase()} decodes your demodulator's audio — set the correct sideband (USB/LSB) above before using it.
              </Text>
            )}
          </View>
        )}

        {/* CLIENT DECODERS — relocated from MenuSheet (§4.3). Below the mode grid + passband:
            a decoder rides on the demod you set above. Selecting one starts DecoderClient;
            its settings drop into a callout beneath the row; tapping again tears it down. */}
        {decoderControls && (
          <View style={dst.decWrap}>
            <Text style={[st.sheetLabel, { color: t.sectionColor, fontFamily: t.font, marginBottom: 8 }]}>
              CLIENT DECODERS
            </Text>
            <NavRow><View style={st.grid}>
              {/* No MORSE — the decoder was dropped (too heavy), so it's not offered. */}
              {(['rtty', 'navtex', 'wefax', 'sstv'] as DecId[]).map(k => {
                const active = decoderControls.decMode === k && decoderControls.decOn;
                const selected = decoderControls.decMode === k && !decoderControls.decOn;
                return (
                  <NavItem key={k} onPress={() => decoderControls.onDecToggle(k)}>{(navFocused, navRef) => (
                  <TouchableOpacity ref={navRef as any}
                    style={[st.btn, { borderColor: (active || selected) ? DEC_COL : t.btnBorder, paddingVertical: 10 },
                            active && { backgroundColor: 'rgba(80,220,100,0.14)' },
                            navFocused && { borderColor: NAV_FOCUS, borderWidth: 2 }]}
                    onPress={() => decoderControls.onDecToggle(k)} activeOpacity={0.8}>
                    <Text style={[st.btnText, { fontFamily: t.font, fontSize: 13, color: (active || selected) ? DEC_COL : t.btnText }]}>
                      {k.toUpperCase()}
                    </Text>
                  </TouchableOpacity>
                  )}</NavItem>
                );
              })}
              {decoderControls.advRdsAvail && (
                <NavItem key="advrds" onPress={() => decoderControls.onAdvRds?.()}>{(navFocused, navRef) => (
                <TouchableOpacity ref={navRef as any}
                  style={[st.btn, { borderColor: decoderControls.advRdsOn ? DEC_COL : t.btnBorder, paddingVertical: 10 },
                          decoderControls.advRdsOn && { backgroundColor: 'rgba(80,220,100,0.14)' },
                          navFocused && { borderColor: NAV_FOCUS, borderWidth: 2 }]}
                  onPress={() => decoderControls.onAdvRds?.()} activeOpacity={0.8}>
                  <Text numberOfLines={1}
                        style={[st.btnText, { fontFamily: t.font, fontSize: 13, color: decoderControls.advRdsOn ? DEC_COL : t.btnText }]}>
                    {advRdsLabel}
                  </Text>
                </TouchableOpacity>
                )}</NavItem>
              )}
            </View></NavRow>
            {decoderControls.decMode === 'rtty' && decoderControls.rttySettings && decoderControls.onRttySettings && (
              <View style={dst.callout}>
                <RttySettingsRows s={decoderControls.rttySettings} onChange={decoderControls.onRttySettings} />
              </View>
            )}
            {decoderControls.decMode === 'wefax' && (
              <View style={dst.callout}>
                <SubLabel label="LPM" />
                <OptRow>{[60, 120, 240].map(v => (
                  <SegBtn key={v} label={String(v)} active={decoderControls.wefaxLpm === v}
                          onPress={() => decoderControls.onWefaxLpm?.(v)} />
                ))}</OptRow>
              </View>
            )}
          </View>
        )}

        {/* SERVER EXTENSIONS / DECODED SPOTS — relocated from MenuSheet (§4.3). */}
        {spotsControls && (
          <View style={dst.decWrap}>
            <Text style={[st.sheetLabel, { color: t.sectionColor, fontFamily: t.font, marginBottom: 8 }]}>
              {spotsControls.label}
            </Text>
            <NavRow><View style={st.grid}>
              <NavItem onPress={() => spotsControls.onSpotsToggle('digi')}>{(nf, nr) => (
              <TouchableOpacity ref={nr as any} style={[st.btn, { borderColor: spotsControls.spotsKind === 'digi' ? DEC_COL : t.btnBorder, paddingVertical: 10 },
                                        spotsControls.spotsKind === 'digi' && { backgroundColor: 'rgba(80,220,100,0.14)' },
                                        nf && { borderColor: NAV_FOCUS, borderWidth: 2 }]}
                onPress={() => spotsControls.onSpotsToggle('digi')} activeOpacity={0.8}>
                <Text style={[st.btnText, { fontFamily: t.font, fontSize: 13, color: spotsControls.spotsKind === 'digi' ? DEC_COL : t.btnText }]}>DIGITAL SPOTS</Text>
              </TouchableOpacity>)}</NavItem>
              {spotsControls.showCwStt && (
                <NavItem onPress={() => spotsControls.onSpotsToggle('cw')}>{(nf, nr) => (
                <TouchableOpacity ref={nr as any} style={[st.btn, { borderColor: spotsControls.spotsKind === 'cw' ? DEC_COL : t.btnBorder, paddingVertical: 10 },
                                          spotsControls.spotsKind === 'cw' && { backgroundColor: 'rgba(80,220,100,0.14)' },
                                          nf && { borderColor: NAV_FOCUS, borderWidth: 2 }]}
                  onPress={() => spotsControls.onSpotsToggle('cw')} activeOpacity={0.8}>
                  <Text style={[st.btnText, { fontFamily: t.font, fontSize: 13, color: spotsControls.spotsKind === 'cw' ? DEC_COL : t.btnText }]}>CW SPOTS</Text>
                </TouchableOpacity>)}</NavItem>
              )}
              {spotsControls.showCwStt && (
                <NavItem onPress={spotsControls.onSttToggle}>{(nf, nr) => (
                <TouchableOpacity ref={nr as any} style={[st.btn, { borderColor: (spotsControls.sttActive || spotsControls.sttSelected) ? DEC_COL : t.btnBorder, paddingVertical: 10 },
                                          spotsControls.sttActive && { backgroundColor: 'rgba(80,220,100,0.14)' },
                                          nf && { borderColor: NAV_FOCUS, borderWidth: 2 }]}
                  onPress={spotsControls.onSttToggle} activeOpacity={0.8}>
                  <Text style={[st.btnText, { fontFamily: t.font, fontSize: 13, color: (spotsControls.sttActive || spotsControls.sttSelected) ? DEC_COL : t.btnText }]}>STT</Text>
                </TouchableOpacity>)}</NavItem>
              )}
              {spotsControls.showMap && (
                <NavItem onPress={() => spotsControls.onSpotsMap?.()}>{(nf, nr) => (
                <TouchableOpacity ref={nr as any} style={[st.btn, { borderColor: t.btnBorder, paddingVertical: 10 },
                                  nf && { borderColor: NAV_FOCUS, borderWidth: 2 }]}
                  onPress={() => spotsControls.onSpotsMap?.()} activeOpacity={0.8}>
                  <Text style={[st.btnText, { fontFamily: t.font, fontSize: 13, color: t.btnText }]}>🗺 MAP</Text>
                </TouchableOpacity>)}</NavItem>
              )}
            </View></NavRow>
          </View>
        )}

        {/* OWRX MAP + FILES — relocated from the MenuSheet OPENWEBRX section. The OWRX
            equivalent of the server maps (its combined map + the decoded-image gallery). */}
        {owrxPages && (
          <View style={dst.decWrap}>
            <Text style={[st.sheetLabel, { color: t.sectionColor, fontFamily: t.font, marginBottom: 8 }]}>
              OPENWEBRX
            </Text>
            <NavRow><View style={st.grid}>
              <NavItem onPress={owrxPages.onMap}>{(nf, nr) => (
              <TouchableOpacity ref={nr as any} style={[st.btn, { borderColor: t.btnBorder, paddingVertical: 10 },
                                nf && { borderColor: NAV_FOCUS, borderWidth: 2 }]}
                onPress={owrxPages.onMap} activeOpacity={0.8}>
                <Text style={[st.btnText, { fontFamily: t.font, fontSize: 13, color: t.btnText }]}>🗺 MAP</Text>
              </TouchableOpacity>)}</NavItem>
              <NavItem onPress={owrxPages.onFiles}>{(nf, nr) => (
              <TouchableOpacity ref={nr as any} style={[st.btn, { borderColor: t.btnBorder, paddingVertical: 10 },
                                nf && { borderColor: NAV_FOCUS, borderWidth: 2 }]}
                onPress={owrxPages.onFiles} activeOpacity={0.8}>
                <Text style={[st.btnText, { fontFamily: t.font, fontSize: 13, color: t.btnText }]}>🖼 FILES</Text>
              </TouchableOpacity>)}</NavItem>
            </View></NavRow>
          </View>
        )}

        {/* SERVER MAPS — relocated from MenuSheet (§4.4). Same "what's on this signal"
            family as the decoders, so it belongs here. Each fires MapOverlay unchanged. */}
        {showServerMaps && onServerMap && (
          <View style={st.mapsWrap}>
            <Text style={[st.sheetLabel, { color: t.sectionColor, fontFamily: t.font, marginBottom: 8 }]}>
              SERVER MAPS
            </Text>
            <NavRow><View style={st.grid}>
              {([['hfdl', '✈ HFDL'], ['digi', '📡 DIGITAL'], ['cw', '⊟ CW']] as const).map(([k, label]) => (
                <NavItem key={k} onPress={() => onServerMap(k)}>{(nf, nr) => (
                <TouchableOpacity ref={nr as any}
                  style={[st.btn, { borderColor: t.btnBorder, paddingVertical: 10 },
                          nf && { borderColor: NAV_FOCUS, borderWidth: 2 }]}
                  onPress={() => onServerMap(k)} activeOpacity={0.8}>
                  <Text style={[st.btnText, { fontFamily: t.font, fontSize: 13, color: t.btnText }]}>{label}</Text>
                </TouchableOpacity>)}</NavItem>
              ))}
            </View></NavRow>
          </View>
        )}

        <NavRow><NavItem onPress={onClose}>{(nf, nr) => (
        <TouchableOpacity ref={nr as any}
          style={[st.closeBtn, { borderColor: t.btnBorder },
                  nf && { borderColor: NAV_FOCUS, borderWidth: 2 }]}
          onPress={onClose}
        >
          <Text style={[st.closeBtnText, { fontFamily: t.font, color: t.btnText }]}>CLOSE</Text>
        </TouchableOpacity>)}</NavItem></NavRow>
                </NavCtx.Provider>
        </ScrollView>
      </View>
    </Modal>
  );
}

const st = StyleSheet.create({
  backdrop:     { flex: 1, backgroundColor: 'rgba(0,0,0,0.50)' },
  sheet: {
    backgroundColor: 'rgba(8,6,1,0.97)',
    borderTopWidth: 1, borderRadius: 14,
    padding: 16, paddingBottom: 40,
  },
  sheetLabel:   { textAlign: 'center', fontSize: 10, letterSpacing: 3, marginBottom: 14 },
  grid:         { flexDirection: 'row', flexWrap: 'wrap', gap: 7 },
  bwMirrorRow:  { flexDirection: 'row', alignItems: 'center', gap: 4, marginTop: 12 },
  bwHalfSlider: { flex: 1, height: 32 },
  bwEdgeVal:    { color: BW_GOLD, fontFamily: 'Atkinson Hyperlegible', fontSize: 10, minWidth: 44, textAlign: 'center' },
  bwSyncBtn:    { borderWidth: 1, borderColor: 'rgba(255,255,255,0.30)', borderRadius: 5, paddingHorizontal: 12, paddingVertical: 8, alignItems: 'center', justifyContent: 'center' },
  bwSyncTxt:    { color: BW_MUTED, fontFamily: 'Atkinson Hyperlegible', fontSize: 12, fontWeight: 'bold', letterSpacing: 0.5 },
  btn: {
    flex: 1, minWidth: '22%', backgroundColor: 'transparent',
    borderWidth: 1, borderRadius: 3, paddingHorizontal: 4, alignItems: 'center',
  },
  btnText:      { textAlign: 'center' },
  mapsWrap:     { marginTop: 14, paddingTop: 12, borderTopWidth: StyleSheet.hairlineWidth, borderTopColor: 'rgba(255,255,255,0.12)' },
  moreWrap:     { marginTop: 12 },
  // A real view, not styled text: iOS drops borders on nested Text runs.
  keyCap:       { borderWidth: 1, borderColor: NAV_FOCUS, borderRadius: 3, paddingHorizontal: 3, marginRight: 6 },
  keyCapText:   { color: NAV_FOCUS, fontSize: 11, fontWeight: '700' as const },
  moreHead: {
    flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between',
    borderWidth: 1, borderRadius: 3, paddingVertical: 10, paddingHorizontal: 12,
  },
  moreHeadText: { fontSize: 13, flex: 1 },
  moreChevron:  { fontSize: 13, marginLeft: 8 },
  moreList:     { marginTop: 4, maxHeight: 260, borderWidth: 1, borderRadius: 3 },
  moreItem:     { paddingVertical: 11, paddingHorizontal: 12, borderBottomWidth: StyleSheet.hairlineWidth },
  moreItemText: { fontSize: 14 },
  decCaption:   { color: DEC_COL, fontSize: 11, marginTop: 7, opacity: 0.85, lineHeight: 15 },
  closeBtn: {
    marginTop: 14, alignSelf: 'center', borderWidth: 1,
    borderRadius: 3, paddingVertical: 7, paddingHorizontal: 24,
  },
  closeBtnText: { fontSize: 11 },
});

// Decoder / spots section styles (moved with the decoders from MenuSheet §4.3).
const dst = StyleSheet.create({
  decWrap:      { marginTop: 14, paddingTop: 12, borderTopWidth: StyleSheet.hairlineWidth, borderTopColor: 'rgba(255,255,255,0.12)' },
  callout:      { marginTop: 8, padding: 10, borderWidth: 1, borderColor: DEC_COL, borderRadius: 6, backgroundColor: 'rgba(80,220,100,0.06)' },
  subLabel:     { color: 'rgba(255,255,255,0.55)', fontSize: 10, letterSpacing: 1, marginTop: 6, marginBottom: 3 },
  optRow:       { flexDirection: 'row', flexWrap: 'wrap', gap: 5 },
  seg:          { flexGrow: 1, minWidth: '18%', borderWidth: 1, borderColor: 'rgba(255,255,255,0.25)', borderRadius: 3, paddingVertical: 7, alignItems: 'center' },
  segActive:    { borderColor: DEC_COL, backgroundColor: 'rgba(80,220,100,0.14)' },
  segText:      { color: 'rgba(255,255,255,0.6)', fontSize: 11, fontWeight: '600' },
  segTextActive:{ color: DEC_COL },
});
