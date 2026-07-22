import React, { useCallback, useEffect, useState } from 'react';
import {
  Modal, Pressable, ScrollView, StyleSheet, Text, TouchableOpacity, View,
} from 'react-native';
import Slider from '@react-native-community/slider';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import { useTheme } from '../contexts/ThemeContext';
import type { DspFilterDesc, DspParamDesc } from './MenuSheet';
import SectionIcon, { type SectionIconName } from './SectionIcon';

// Local copy of the menu's accessibility palette so this sheet is self-contained
// (no shared-internals refactor of MenuSheet). Values mirror MenuSheet's `C`.
const C = {
  gold:        '#ffe566',
  goldDim:     'rgba(255,229,102,0.70)',
  muted:       'rgba(255,255,255,0.92)',
  btnBg:       'rgba(20,18,14,0.85)',
  border:      'rgba(255,255,255,0.30)',
  active:      'rgba(255,200,0,0.12)',
  divider:     'rgba(255,255,255,0.12)',
  sectionC:    'rgba(180,190,210,0.80)',
};

// ── Helpers (local copies) ────────────────────────────────────────────────────
function fmtRecTime(s: number) {
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60;
  return `${h}:${String(m).padStart(2, '0')}:${String(sec).padStart(2, '0')}`;
}
function fmtParamName(n: string) { return n.replace(/_/g, ' ').toUpperCase(); }
function dspStep(min: number, max: number) {
  const r = max - min;
  if (r <= 1)   return 0.01;
  if (r <= 10)  return 0.1;
  if (r <= 100) return 1;
  return Math.pow(10, Math.floor(Math.log10(r)) - 2);
}
function fmtDspVal(v: number, step: number) {
  return v.toFixed(step < 0.1 ? 2 : step < 1 ? 1 : 0);
}

// ── Small primitives (local copies of MenuSheet's) ───────────────────────────
function SectionLabel({ label, icon }: { label: string; icon?: SectionIconName }) {
  return (
    <View style={st.sectionBar}>
      <View style={st.sectionRow}>
        {icon && <SectionIcon name={icon} size={16} color={C.sectionC} />}
        <Text style={st.sectionLabel}>{label}</Text>
      </View>
    </View>
  );
}
function BtnRow({ children }: { children: React.ReactNode }) {
  return <View style={st.btnRow}>{children}</View>;
}
function Btn({ label, active, onPress, full, style }: {
  label: string; active?: boolean; onPress?: () => void; full?: boolean; style?: object;
}) {
  return (
    <TouchableOpacity
      style={[st.btn, active && st.btnActive, full && st.btnFull, style]}
      onPress={onPress} hitSlop={4} activeOpacity={0.7}
    >
      <Text style={[st.btnText, active && st.btnTextActive]}>{label}</Text>
    </TouchableOpacity>
  );
}
function SubLabel({ label }: { label: string }) {
  return <Text style={st.subLabel}>{label}</Text>;
}
function SegBtn({ label, active, onPress }: { label: string; active: boolean; onPress: () => void }) {
  return (
    <TouchableOpacity style={[st.btn, active && st.btnActive]} onPress={onPress} hitSlop={4} activeOpacity={0.7}>
      <Text style={[st.btnText, active && st.btnTextActive]}>{label}</Text>
    </TouchableOpacity>
  );
}

export interface AudioSheetProps {
  visible:  boolean;
  onClose:  () => void;
  /** iOS Modal onDismiss — fires after the sheet is fully gone. SDRScreen uses
   *  it to present the recording share sheet only once no RN modal is up (else
   *  the native share VC presents over this Modal and wedges touch handling). */
  onDismiss?: () => void;
  serverType?: string;         // 'ubersdr' | 'owrx' | 'kiwi'
  /** Meter display mode — squelch readouts follow it (S-units when 'smeter'), while the value SENT
   *  to the backend stays in its native unit. */
  signalMode?: 'snr' | 'smeter' | 'dbfs';
  isLocal?:  boolean;          // V4 local hardware
  /** FM-DX: only REC + Recordings apply (no client DSP / squelch / notch). */
  recordingOnly?: boolean;

  // Client-side NR/NB (UberSDR only)
  nr?:   boolean;
  onNr?: (mode: 'off' | 'nr' | 'nr2') => void;
  nb?:   boolean;
  onNb?: (on: boolean) => void;

  // Recording
  recording?:   boolean;
  onRec?:       () => void;
  recSeconds?:  number;
  onRecordings?: () => void;

  // Squelch variants (gated by backend)
  snrSquelch?:   number;  onSnrSquelch?:   (v: number) => void;
  localSquelch?: number;  onLocalSquelch?: (db: number) => void;
  localNR?:      number;  onLocalNR?:      (level: number) => void;
  kiwiSquelch?:  number;  onKiwiSquelch?:  (v: number) => void;
  fmSquelch?:    number;  onFmSquelch?:    (v: number) => void;
  isFmMode?:     boolean;

  // Auto-notch (all backends)
  notchOn?: boolean;
  onNotch?: (on: boolean) => void;

  // OWRX server-side squelch (dB) + NR (threshold dB)
  onOwrxSquelch?: (db: number) => void;
  onOwrxNr?:      (threshold: number) => void;
  owrxDspDefaults?: { squelchDb?: number; nrEnabled?: boolean; nrThreshold?: number; seq: number };

  // UberSDR server-side NR (DSP insert)
  serverDspEnabled?:  boolean;
  serverDspFilter?:   string;
  serverDspParams?:   Record<string, string>;
  dspFilters?:        DspFilterDesc[];
  dspError?:          string | null;
  onServerDsp?:       (enabled: boolean) => void;
  onServerDspFilter?: (name: string) => void;
  onServerDspParam?:  (name: string, value: string) => void;
}

export default function AudioSheet({
  visible, onClose, onDismiss, serverType = 'ubersdr', signalMode = 'smeter', isLocal = false, recordingOnly = false,
  nr = false, onNr, nb = false, onNb,
  recording = false, onRec, recSeconds = 0, onRecordings,
  snrSquelch = -999, onSnrSquelch,
  localSquelch = -100, onLocalSquelch,
  localNR = 0, onLocalNR,
  kiwiSquelch = 0, onKiwiSquelch,
  fmSquelch = -999, onFmSquelch, isFmMode = false,
  notchOn = false, onNotch,
  onOwrxSquelch, onOwrxNr, owrxDspDefaults,
  serverDspEnabled = false, serverDspFilter = '', serverDspParams = {},
  dspFilters = [], dspError = null, onServerDsp, onServerDspFilter, onServerDspParam,
}: AudioSheetProps) {
  const { theme: t } = useTheme();
  const insets = useSafeAreaInsets();
  const isOwrx = serverType === 'owrx';
  const isKiwi = serverType === 'kiwi';
  const uberDsp = !recordingOnly && !isOwrx && !isLocal && !isKiwi;

  // Squelch readout in the DISPLAYED meter unit (S-units when the meter shows S-meter), while the
  // slider's value stays in the backend's NATIVE unit for the wire. dBm/dBFS → S (S9 = −73, 6 dB/S).
  const sqlDisp = (v: number) => {
    if (signalMode === 'smeter') {
      if (v >= -73) { const o = Math.round(v + 73); return o > 0 ? `S9+${o}` : 'S9'; }
      return `S${Math.max(1, 9 - Math.ceil((-73 - v) / 6))}`;
    }
    return `${Math.round(v)}dB`;
  };

  // OWRX squelch/NR sliders — seeded from the server/profile preset (keyed on
  // seq so a profile switch re-syncs even when the new preset equals the old).
  const [owrxSql, setOwrxSql] = useState(-150);
  const [owrxNr,  setOwrxNr]  = useState(0);
  useEffect(() => {
    if (!owrxDspDefaults) return;
    if (owrxDspDefaults.squelchDb !== undefined) setOwrxSql(owrxDspDefaults.squelchDb);
    if (owrxDspDefaults.nrThreshold !== undefined) {
      setOwrxNr(owrxDspDefaults.nrEnabled ? owrxDspDefaults.nrThreshold : 0);
    }
  }, [owrxDspDefaults?.seq]);   // eslint-disable-line react-hooks/exhaustive-deps

  // NR cycle — off→nr→nr2. SERV is locked while the server DSP section is on.
  const [nrMode, setNrMode] = useState<'off' | 'nr' | 'nr2' | 'serv'>(
    serverDspEnabled ? 'serv' : nr ? 'nr' : 'off'
  );
  const cycleNr = useCallback(() => {
    if (nrMode === 'serv') return;   // locked — server DSP section controls this
    const next = nrMode === 'off' ? 'nr' : nrMode === 'nr' ? 'nr2' : 'off';
    setNrMode(next);
    onNr?.(next);
  }, [nrMode, onNr]);
  useEffect(() => {
    if (serverDspEnabled) setNrMode('serv');
    else if (nrMode === 'serv') setNrMode('off');
  }, [serverDspEnabled]);   // eslint-disable-line react-hooks/exhaustive-deps

  return (
    <Modal visible={visible} transparent animationType="slide" onRequestClose={onClose}
           onDismiss={onDismiss}
           supportedOrientations={['portrait', 'landscape', 'landscape-left', 'landscape-right']}>
      <Pressable style={st.backdrop} onPress={onClose} />
      <View style={[st.sheet, {
        borderTopColor: t.barBorder,
        // Landscape: keep clear of the Dynamic Island and don't sprawl the full
        // (very wide) width — cap it and centre it.
        paddingLeft: 16 + insets.left, paddingRight: 16 + insets.right,
        paddingBottom: 40 + insets.bottom,
        alignSelf: 'center', width: '100%', maxWidth: 640,
      }]}>
        <View style={st.titleRow}>
          <SectionIcon name="audio" size={15} color={t.sectionColor} />
          <Text style={[st.sheetLabel, { color: t.sectionColor, fontFamily: t.font, marginBottom: 0 }]}>
            AUDIO
          </Text>
        </View>

        <ScrollView style={st.scroll} keyboardShouldPersistTaps="handled">

          {/* NR / NB (UberSDR client-side DSP) + REC — REC stays for all backends */}
          <BtnRow>
            {uberDsp && (
              <Btn
                label={nrMode === 'serv' ? 'SERV' : nrMode === 'nr2' ? 'NR2' : 'NR'}
                active={nrMode !== 'off'}
                style={nrMode === 'serv' ? { borderColor: 'rgba(50,210,100,0.60)', backgroundColor: 'rgba(50,210,100,0.10)' } : undefined}
                onPress={cycleNr}
              />
            )}
            {uberDsp && <Btn label="NB" active={nb} onPress={() => onNb?.(!nb)} />}
            <Btn label="⏺ REC" active={recording} onPress={onRec} />
          </BtnRow>
          {recording && (
            <View style={st.recTimer}>
              <View style={st.recDot} />
              <Text style={st.recTime}>{fmtRecTime(recSeconds)}</Text>
            </View>
          )}
          {onRecordings && (
            <BtnRow>
              <Btn label="RECORDINGS" full onPress={onRecordings} />
            </BtnRow>
          )}

          {/* OWRX server-side squelch (dB) + NR (threshold dB). Squelch left =
              Off (open); NR left = Off, slides up for more reduction. */}
          {isOwrx && (<>
            <View style={st.bwRow}>
              <Text style={st.bwLabel}>SQUELCH</Text>
              <Slider style={st.bwSlider}
                minimumValue={-130} maximumValue={-20} step={1}
                value={owrxSql <= -130 ? -130 : owrxSql}
                onValueChange={(v: number) => { const db = v <= -130 ? -150 : v; setOwrxSql(db); onOwrxSquelch?.(db); }}
                minimumTrackTintColor={owrxSql > -130 ? C.gold : C.muted}
                maximumTrackTintColor={C.muted} thumbTintColor={C.gold} />
              <Text style={st.bwVal}>{owrxSql <= -130 ? 'Off' : sqlDisp(owrxSql)}</Text>
            </View>
            <View style={st.bwRow}>
              <Text style={st.bwLabel}>NR</Text>
              <Slider style={st.bwSlider}
                minimumValue={0} maximumValue={30} step={1}
                value={owrxNr}
                onValueChange={(v: number) => { setOwrxNr(v); onOwrxNr?.(v); }}
                minimumTrackTintColor={owrxNr > 0 ? C.gold : C.muted}
                maximumTrackTintColor={C.muted} thumbTintColor={C.gold} />
              <Text style={st.bwVal}>{owrxNr <= 0 ? 'Off' : `${owrxNr}dB`}</Text>
            </View>
          </>)}

          {/* Local SDR: power-based squelch (dBFS). */}
          {onLocalSquelch ? (
            <View style={st.bwRow}>
              <Text style={st.bwLabel}>SQUELCH</Text>
              <Slider style={st.bwSlider}
                minimumValue={-100} maximumValue={-20} step={1}
                value={localSquelch}
                onValueChange={(v: number) => onLocalSquelch?.(v <= -100 ? -100 : v)}
                minimumTrackTintColor={localSquelch > -100 ? C.gold : C.muted}
                maximumTrackTintColor={C.muted} thumbTintColor={C.gold} />
              <Text style={st.bwVal}>{localSquelch <= -100 ? 'Off' : sqlDisp(localSquelch)}</Text>
            </View>
          ) : null}

          {/* Local SDR audio noise reduction — strength slider (0=off..20). */}
          {onLocalNR && (
            <View style={st.bwRow}>
              <Text style={st.bwLabel}>NR</Text>
              <Slider style={st.bwSlider}
                minimumValue={0} maximumValue={20} step={1}
                value={localNR}
                onValueChange={(v: number) => onLocalNR?.(v)}
                minimumTrackTintColor={localNR > 0 ? C.gold : C.muted}
                maximumTrackTintColor={C.muted} thumbTintColor={C.gold} />
              <Text style={st.bwVal}>{localNR <= 0 ? 'Off' : String(localNR)}</Text>
            </View>
          )}

          {/* Automatic notch (adaptive line enhancer) — on/off, all backends. */}
          {onNotch && (
            <View style={st.bwRow}>
              <Text style={[st.bwLabel, { width: 78 }]}>AUTO NOTCH</Text>
              <View style={{ flex: 1 }} />
              <TouchableOpacity onPress={() => onNotch?.(!notchOn)} hitSlop={8}
                style={{ paddingHorizontal: 16, paddingVertical: 4, borderRadius: 6,
                         backgroundColor: notchOn ? C.gold : 'transparent',
                         borderWidth: 1, borderColor: notchOn ? C.gold : C.muted }}>
                <Text style={{ color: notchOn ? '#000' : C.muted,
                               fontFamily: 'Atkinson Hyperlegible', fontSize: 11, letterSpacing: 1 }}>
                  {notchOn ? 'ON' : 'OFF'}
                </Text>
              </TouchableOpacity>
            </View>
          )}

          {/* Kiwi squelch — client-side dBFS gate (dBm threshold, −130 = Off). */}
          {onKiwiSquelch && (
            <View style={st.bwRow}>
              <Text style={st.bwLabel}>SQUELCH</Text>
              <Slider style={st.bwSlider}
                minimumValue={-130} maximumValue={-20} step={1}
                value={kiwiSquelch <= -130 ? -130 : kiwiSquelch}
                onValueChange={(v: number) => onKiwiSquelch?.(v <= -130 ? -130 : v)}
                minimumTrackTintColor={kiwiSquelch > -130 ? C.gold : C.muted}
                maximumTrackTintColor={C.muted} thumbTintColor={C.gold} />
              <Text style={st.bwVal}>{kiwiSquelch <= -130 ? 'Off' : sqlDisp(kiwiSquelch)}</Text>
            </View>
          )}

          {/* SNR Squelch — UberSDR audio gate (0–50 dB in our meter's units). */}
          {!recordingOnly && !onLocalSquelch && !onKiwiSquelch && !isOwrx && (
            <View style={st.bwRow}>
              <Text style={st.bwLabel}>SNR SQL</Text>
              <Slider style={st.bwSlider}
                minimumValue={0} maximumValue={50} step={0.5}
                value={Math.max(0, snrSquelch === -999 ? 0 : snrSquelch)}
                onValueChange={(v: number) => onSnrSquelch?.(v <= 0.1 ? -999 : v)}
                minimumTrackTintColor={snrSquelch > 0 ? C.gold : C.muted}
                maximumTrackTintColor={C.muted} thumbTintColor={C.gold} />
              <Text style={st.bwVal}>{snrSquelch <= -999 ? 'Off' : `≥${snrSquelch.toFixed(0)}`}</Text>
            </View>
          )}

          {/* FM Squelch — only for fm/nfm. */}
          {!isOwrx && isFmMode && (
            <View style={st.bwRow}>
              <Text style={st.bwLabel}>FM SQL</Text>
              <Slider style={st.bwSlider}
                minimumValue={0} maximumValue={100} step={1}
                value={fmSquelch <= -999 ? 0 : Math.round((fmSquelch + 48) * 99 / 68 + 1)}
                onValueChange={(v: number) => {
                  const db = v === 0 ? -999 : -48 + (v - 1) * (68 / 99);
                  onFmSquelch?.(db);
                }}
                minimumTrackTintColor={fmSquelch > -999 ? C.gold : C.muted}
                maximumTrackTintColor={C.muted} thumbTintColor={C.gold} />
              <Text style={st.bwVal}>{fmSquelch <= -999 ? 'Open' : `${fmSquelch.toFixed(1)}dB`}</Text>
            </View>
          )}

          {/* ── SERVER SIDE NR (DSP insert) — only when the server advertises
                 filters (UberSDR). Type selector + per-filter params. ── */}
          {dspFilters.length > 0 && (<>
            <SectionLabel label="SERVER SIDE NR" icon="nr" />
            <BtnRow>
              <Btn
                label={serverDspEnabled ? 'DISABLE SERVER NR' : 'ENABLE SERVER NR'}
                active={serverDspEnabled}
                full
                style={serverDspEnabled ? { borderColor: 'rgba(50,210,100,0.50)', backgroundColor: 'rgba(50,210,100,0.10)' } : undefined}
                onPress={() => onServerDsp?.(!serverDspEnabled)}
              />
            </BtnRow>
            {dspError != null && <Text style={st.dspError}>{dspError}</Text>}
            {serverDspEnabled && (
              <View style={st.subPanel}>
                <SubLabel label="DSP TYPE" />
                <View style={[st.btnRow, { paddingTop: 2, paddingBottom: 0 }]}>
                  {dspFilters.map((f: DspFilterDesc) => (
                    <SegBtn key={f.name} label={f.name.toUpperCase()}
                            active={serverDspFilter === f.name}
                            onPress={() => onServerDspFilter?.(f.name)} />
                  ))}
                </View>
                {(dspFilters.find((f: DspFilterDesc) => f.name === serverDspFilter)?.params ?? [])
                  .filter((p: DspParamDesc) => p.runtime_safe !== false)
                  .map((p: DspParamDesc) => {
                    const val = serverDspParams[p.name] ?? p.default ?? '';
                    if ((p.type ?? 'float').toLowerCase() === 'bool') {
                      return (
                        <BtnRow key={p.name}>
                          <Btn label={fmtParamName(p.name)} active={val === 'true'} full
                               onPress={() => onServerDspParam?.(p.name, val === 'true' ? 'false' : 'true')} />
                        </BtnRow>
                      );
                    }
                    const min = parseFloat(p.min ?? ''), max = parseFloat(p.max ?? '');
                    if (!Number.isFinite(min) || !Number.isFinite(max) || max <= min) return null;
                    const step = dspStep(min, max);
                    const num  = Number.isFinite(parseFloat(val)) ? parseFloat(val) : min;
                    return (
                      <View key={p.name} style={st.bwRow}>
                        <Text style={st.bwLabel} numberOfLines={1}>{fmtParamName(p.name)}</Text>
                        <Slider style={st.bwSlider}
                          minimumValue={min} maximumValue={max} step={step}
                          value={Math.max(min, Math.min(max, num))}
                          onValueChange={(v: number) => onServerDspParam?.(p.name, fmtDspVal(v, step))}
                          minimumTrackTintColor={C.gold} maximumTrackTintColor={C.muted}
                          thumbTintColor={C.gold} />
                        <Text style={st.bwVal}>{fmtDspVal(num, step)}</Text>
                      </View>
                    );
                  })}
              </View>
            )}
          </>)}

        </ScrollView>

        <TouchableOpacity style={[st.closeBtn, { borderColor: t.btnBorder }]} onPress={onClose}>
          <Text style={[st.closeBtnText, { fontFamily: t.font, color: t.btnText }]}>CLOSE</Text>
        </TouchableOpacity>
      </View>
    </Modal>
  );
}

const st = StyleSheet.create({
  backdrop:   { flex: 1, backgroundColor: 'rgba(0,0,0,0.50)' },
  sheet: {
    backgroundColor: 'rgba(8,6,1,0.97)',
    borderTopWidth: 1, borderRadius: 14,
    padding: 16, paddingBottom: 40,
  },
  sheetLabel: { textAlign: 'center', fontSize: 10, letterSpacing: 3, marginBottom: 12 },
  titleRow: { flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: 7, marginBottom: 12 },
  sectionRow: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  scroll:     { maxHeight: 420 },

  sectionBar: {
    borderTopWidth: StyleSheet.hairlineWidth, borderTopColor: C.divider,
    paddingTop: 12, paddingBottom: 6, marginTop: 6,
  },
  sectionLabel: {
    color: C.sectionC, fontFamily: 'Atkinson Hyperlegible', fontSize: 12,
    fontWeight: 'bold', letterSpacing: 2,
  },

  btnRow:  { flexDirection: 'row', flexWrap: 'wrap', gap: 6, paddingVertical: 4 },
  btn: {
    backgroundColor: C.btnBg, borderWidth: 1, borderColor: C.border,
    borderRadius: 5, paddingHorizontal: 16, paddingVertical: 11,
    alignItems: 'center', justifyContent: 'center',
  },
  btnActive:     { backgroundColor: C.active, borderColor: C.goldDim },
  btnFull:       { flex: 1, alignSelf: 'stretch' },
  btnText:       { color: C.muted, fontFamily: 'Atkinson Hyperlegible', fontSize: 15, fontWeight: 'bold', letterSpacing: 0.5 },
  btnTextActive: { color: C.gold },

  bwRow:    { flexDirection: 'row', alignItems: 'center', gap: 6, paddingVertical: 2 },
  bwLabel:  { color: C.sectionC, fontFamily: 'Atkinson Hyperlegible', fontSize: 11, letterSpacing: 1, width: 32 },
  bwSlider: { flex: 1, height: 32 },
  bwVal:    { color: C.gold, fontFamily: 'Atkinson Hyperlegible', fontSize: 11, minWidth: 68, textAlign: 'right' },

  subPanel: {
    backgroundColor: 'rgba(255,255,255,0.04)', borderRadius: 6,
    borderWidth: StyleSheet.hairlineWidth, borderColor: C.divider,
    padding: 10, marginBottom: 4,
  },
  subLabel: { color: C.sectionC, fontFamily: 'Atkinson Hyperlegible', fontSize: 12, letterSpacing: 1, paddingTop: 8, paddingBottom: 3 },

  recTimer: { flexDirection: 'row', alignItems: 'center', gap: 8, paddingVertical: 4 },
  recDot:   { width: 8, height: 8, borderRadius: 4, backgroundColor: '#cc2222' },
  recTime:  { color: C.gold, fontFamily: 'Atkinson Hyperlegible', fontSize: 13 },
  dspError: { color: 'rgba(220,53,69,0.95)', fontFamily: 'Atkinson Hyperlegible', fontSize: 13, paddingBottom: 6 },

  closeBtn: {
    marginTop: 14, alignSelf: 'center', borderWidth: 1,
    borderRadius: 3, paddingVertical: 7, paddingHorizontal: 24,
  },
  closeBtnText: { fontSize: 11 },
});
