import React, { useEffect, useMemo, useRef, useState } from 'react';
import {
  Modal, Pressable, ScrollView, StyleSheet, Text, TouchableOpacity, View,
} from 'react-native';
import Slider from '@react-native-community/slider';
import { Mode, MODES } from '../services/sdrTypes';
import { useTheme } from '../contexts/ThemeContext';
import GainSlider from './GainSlider';
import { RTTY_PRESETS, type RttySettings } from '../services/DecoderClient';

type DecId = 'rtty' | 'navtex' | 'wefax' | 'sstv' | 'morse' | 'whisper';

const BW_GOLD  = '#ffe566';
const BW_MUTED = 'rgba(255,255,255,0.92)';
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
  return <View style={dst.optRow}>{children}</View>;
}
function SegBtn({ label, active, onPress }: { label: string; active: boolean; onPress: () => void }) {
  return (
    <TouchableOpacity style={[dst.seg, active && dst.segActive]} onPress={onPress} activeOpacity={0.7}>
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
  /** V4 local hardware: quick-access RTL-SDR gain control shown above the modes. */
  gainControl?: {
    gains: number[]; gainTenthDb: number; auto: boolean;
    onAuto: (auto: boolean) => void; onGain: (tenthDb: number) => void;
  };
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
    rttySettings?: RttySettings; onRttySettings?: (s: RttySettings) => void;
    wefaxLpm?: number; onWefaxLpm?: (v: number) => void;
  } | null;
  // SERVER EXTENSIONS / DECODED SPOTS (relocated from MenuSheet §4.3). null = don't show.
  spotsControls?: {
    label: string; spotsKind: string | null;
    onSpotsToggle: (k: 'digi' | 'cw') => void; onSpotsMap?: () => void;
    showCwStt: boolean; showMap: boolean;
    sttActive: boolean; sttSelected: boolean; onSttToggle: () => void;
  } | null;
}

export default function ModeSelector({ visible, current, modes, activeDecoder, onSelect, onClose, gainControl,
  filterLow = 0, filterHigh = 0, bwEdgeMax = 6000, onFilterBoth,
  showServerMaps = false, onServerMap,
  decoderControls, spotsControls }: ModeSelectorProps) {
  const { theme: t } = useTheme();
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

  return (
    <Modal visible={visible} transparent animationType="slide" onRequestClose={onClose}
           supportedOrientations={['portrait', 'landscape', 'landscape-left', 'landscape-right']}>
      <Pressable style={st.backdrop} onPress={onClose} />
      <View style={[st.sheet, { borderTopColor: t.barBorder }]}>
        <Text style={[st.sheetLabel, { color: t.sectionColor, fontFamily: t.font }]}>
          DEMODULATOR
        </Text>
        {gainControl ? (
          <View style={st.gainWrap}>
            <GainSlider
              gains={gainControl.gains}
              gainTenthDb={gainControl.gainTenthDb}
              auto={gainControl.auto}
              onAuto={gainControl.onAuto}
              onGain={gainControl.onGain}
            />
          </View>
        ) : null}
        <View style={st.grid}>
          {common.map(m => (
            <TouchableOpacity
              key={m.id}
              style={[
                st.btn,
                { borderColor: isWhite ? 'rgba(255,255,255,0.20)' : 'rgba(80,50,0,0.40)',
                  paddingVertical: isWhite ? 12 : 10 },
                m.id === current && { backgroundColor: t.btnActiveBg, borderColor: t.btnActiveBdr },
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
          ))}
        </View>

        {/* Bandwidth — mirrored sliders around the carrier: slide the LEFT one
            LEFT to widen the lower sideband, the RIGHT one RIGHT to widen the
            upper. SYNC mirrors both edges (AM/FM symmetric). */}
        {showBw && (
          <View style={st.bwMirrorRow}>
            <Text style={st.bwEdgeVal}>{filterLow >= 0 ? '+' : '−'}{fmtHz(Math.abs(filterLow))}</Text>
            <Slider style={st.bwHalfSlider}
              minimumValue={-bwEdgeMax} maximumValue={0} step={bwStep}
              value={Math.max(-bwEdgeMax, Math.min(0, filterLow))}
              onValueChange={(v: number) => {
                if (bwSync) onFilterBoth?.(v, -v);
                else        onFilterBoth?.(v, filterHigh);
              }}
              minimumTrackTintColor={BW_MUTED} maximumTrackTintColor={BW_GOLD}
              thumbTintColor={BW_GOLD} />
            <TouchableOpacity hitSlop={6}
              style={[st.bwSyncBtn, bwSync && { borderColor: BW_GOLD, backgroundColor: 'rgba(255,200,0,0.12)' }]}
              onPress={() => setBwSync(p => !p)} activeOpacity={0.7}>
              <Text style={[st.bwSyncTxt, bwSync && { color: BW_GOLD }]}>SYNC</Text>
            </TouchableOpacity>
            <Slider style={st.bwHalfSlider}
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
            <TouchableOpacity
              style={[st.moreHead, { borderColor: t.btnBorder },
                      currentInOthers && { borderColor: t.btnActiveBdr, backgroundColor: t.btnActiveBg },
                      activeDecInOthers && { borderColor: DEC_COL, backgroundColor: 'rgba(80,220,100,0.14)' }]}
              onPress={() => setMoreOpen(o => !o)}
              activeOpacity={0.8}>
              <Text style={[st.moreHeadText, { fontFamily: t.font },
                            { color: activeDecInOthers ? DEC_COL : currentInOthers ? t.btnActiveText : t.btnText }]} numberOfLines={1}>
                {activeDecInOthers ? activeDecInOthers.label.toUpperCase()
                  : currentInOthers ? currentInOthers.label.toUpperCase()
                  : `DIGITAL / DECODERS (${others.length})`}
              </Text>
              <Text style={[st.moreChevron, { color: t.btnText }]}>{moreOpen ? '▴' : '▾'}</Text>
            </TouchableOpacity>
            {moreOpen && (
              <ScrollView ref={moreScroll} style={[st.moreList, { borderColor: t.btnBorder }]} keyboardShouldPersistTaps="handled">
                {others.map(m => (
                  <TouchableOpacity
                    key={m.id}
                    style={[st.moreItem, { borderBottomColor: t.barBorder }]}
                    onPress={() => pick(m.id)}
                    onLayout={e => { itemY.current[m.id] = e.nativeEvent.layout.y; }}
                    activeOpacity={0.7}>
                    <Text style={[st.moreItemText, { fontFamily: t.font },
                                  { color: m.id === activeDecoder ? DEC_COL : m.id === current ? t.btnActiveText : t.btnText }]}>
                      {m.id === activeDecoder || m.id === current ? '✓ ' : ''}{m.label.toUpperCase()}
                    </Text>
                  </TouchableOpacity>
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
            <View style={st.grid}>
              {(['rtty', 'navtex', 'wefax', 'sstv', 'morse'] as DecId[])
                .filter(k => !(decoderControls.isLocal && k === 'morse')).map(k => {
                const active = decoderControls.decMode === k && decoderControls.decOn;
                const selected = decoderControls.decMode === k && !decoderControls.decOn;
                return (
                  <TouchableOpacity key={k}
                    style={[st.btn, { borderColor: (active || selected) ? DEC_COL : t.btnBorder, paddingVertical: 10 },
                            active && { backgroundColor: 'rgba(80,220,100,0.14)' }]}
                    onPress={() => decoderControls.onDecToggle(k)} activeOpacity={0.8}>
                    <Text style={[st.btnText, { fontFamily: t.font, fontSize: 13, color: (active || selected) ? DEC_COL : t.btnText }]}>
                      {k.toUpperCase()}
                    </Text>
                  </TouchableOpacity>
                );
              })}
            </View>
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
            <View style={st.grid}>
              <TouchableOpacity style={[st.btn, { borderColor: spotsControls.spotsKind === 'digi' ? DEC_COL : t.btnBorder, paddingVertical: 10 },
                                        spotsControls.spotsKind === 'digi' && { backgroundColor: 'rgba(80,220,100,0.14)' }]}
                onPress={() => spotsControls.onSpotsToggle('digi')} activeOpacity={0.8}>
                <Text style={[st.btnText, { fontFamily: t.font, fontSize: 13, color: spotsControls.spotsKind === 'digi' ? DEC_COL : t.btnText }]}>DIGITAL SPOTS</Text>
              </TouchableOpacity>
              {spotsControls.showCwStt && (
                <TouchableOpacity style={[st.btn, { borderColor: spotsControls.spotsKind === 'cw' ? DEC_COL : t.btnBorder, paddingVertical: 10 },
                                          spotsControls.spotsKind === 'cw' && { backgroundColor: 'rgba(80,220,100,0.14)' }]}
                  onPress={() => spotsControls.onSpotsToggle('cw')} activeOpacity={0.8}>
                  <Text style={[st.btnText, { fontFamily: t.font, fontSize: 13, color: spotsControls.spotsKind === 'cw' ? DEC_COL : t.btnText }]}>CW SPOTS</Text>
                </TouchableOpacity>
              )}
              {spotsControls.showCwStt && (
                <TouchableOpacity style={[st.btn, { borderColor: (spotsControls.sttActive || spotsControls.sttSelected) ? DEC_COL : t.btnBorder, paddingVertical: 10 },
                                          spotsControls.sttActive && { backgroundColor: 'rgba(80,220,100,0.14)' }]}
                  onPress={spotsControls.onSttToggle} activeOpacity={0.8}>
                  <Text style={[st.btnText, { fontFamily: t.font, fontSize: 13, color: (spotsControls.sttActive || spotsControls.sttSelected) ? DEC_COL : t.btnText }]}>STT</Text>
                </TouchableOpacity>
              )}
              {spotsControls.showMap && (
                <TouchableOpacity style={[st.btn, { borderColor: t.btnBorder, paddingVertical: 10 }]}
                  onPress={() => spotsControls.onSpotsMap?.()} activeOpacity={0.8}>
                  <Text style={[st.btnText, { fontFamily: t.font, fontSize: 13, color: t.btnText }]}>🗺 MAP</Text>
                </TouchableOpacity>
              )}
            </View>
          </View>
        )}

        {/* SERVER MAPS — relocated from MenuSheet (§4.4). Same "what's on this signal"
            family as the decoders, so it belongs here. Each fires MapOverlay unchanged. */}
        {showServerMaps && onServerMap && (
          <View style={st.mapsWrap}>
            <Text style={[st.sheetLabel, { color: t.sectionColor, fontFamily: t.font, marginBottom: 8 }]}>
              SERVER MAPS
            </Text>
            <View style={st.grid}>
              {([['hfdl', '✈ HFDL'], ['digi', '📡 DIGITAL'], ['cw', '⊟ CW']] as const).map(([k, label]) => (
                <TouchableOpacity key={k}
                  style={[st.btn, { borderColor: t.btnBorder, paddingVertical: 10 }]}
                  onPress={() => onServerMap(k)} activeOpacity={0.8}>
                  <Text style={[st.btnText, { fontFamily: t.font, fontSize: 13, color: t.btnText }]}>{label}</Text>
                </TouchableOpacity>
              ))}
            </View>
          </View>
        )}

        <TouchableOpacity
          style={[st.closeBtn, { borderColor: t.btnBorder }]}
          onPress={onClose}
        >
          <Text style={[st.closeBtnText, { fontFamily: t.font, color: t.btnText }]}>CLOSE</Text>
        </TouchableOpacity>
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
  gainWrap:     { marginBottom: 12, paddingBottom: 10, borderBottomWidth: StyleSheet.hairlineWidth, borderBottomColor: 'rgba(255,255,255,0.15)' },
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
