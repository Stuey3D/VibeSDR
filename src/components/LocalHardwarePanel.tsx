import React from 'react';
import {
  Modal, View, Text, TouchableOpacity, TouchableWithoutFeedback, ScrollView,
  Switch, StyleSheet,
} from 'react-native';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import GainSlider from './GainSlider';

// VibeSDR V4 — RTL-SDR hardware controls submenu (Android, local hardware only).
// Gain (also mirrored in the demodulators popup), PPM, sample rate, bias-T,
// RTL2832 digital AGC, and direct sampling. Direct sampling is not needed on the
// Blog V4 (it covers HF directly) — kept for V3/other dongles.

const C = {
  bg:     'rgba(6,4,2,0.99)',
  border: 'rgba(255,255,255,0.30)',
  gold:   '#ffe566',
  muted:  'rgba(255,255,255,0.92)',
  dim:    'rgba(200,210,225,0.90)',
  sectionC: 'rgba(180,190,210,0.80)',
  btnBg:  'rgba(20,18,14,0.85)',
  active: 'rgba(255,200,0,0.16)',
  abtn:   'rgba(255,229,102,0.55)',
};

// 3.2 MSPS is gone: the RTL2832U accepts the rate but cannot sustain it — above
// ~2.56 MSPS the USB transfers fall behind, so it drops samples and runs hot doing
// it. Offering it only invited people to pick the biggest number and then blame the
// receiver for the gaps. 2.56 is the real ceiling.
const SAMPLE_RATES = [250000, 1024000, 1536000, 1800000, 2048000, 2400000, 2560000];
const DS_MODES: { label: string; value: number }[] = [
  { label: 'Off', value: 0 }, { label: 'I', value: 1 }, { label: 'Q', value: 2 },
];

export interface LocalHardwarePanelProps {
  visible: boolean;
  onClose: () => void;
  gains: number[];
  gainTenthDb: number;
  autoGain: boolean;
  onAuto: (auto: boolean) => void;
  onGain: (tenthDb: number) => void;
  ppm: number;
  onPpm: (ppm: number) => void;
  sampleRate: number;
  onSampleRate: (rate: number) => void;
  isTcp?: boolean;           // RTL-TCP allows low rates (UberSDR sends ~192k); USB doesn't
  /** VibeServer: the exact sample rates this server offers (sent over the wire),
   *  so the picker aligns with the server rather than a generic RTL-TCP/USB list. */
  serverRates?: number[] | null;
  /** >0 = the server PINNED the capture rate; the picker is replaced by a note. */
  lockedRate?: number | null;
  /** SpyServer: the server owns the radio, so most RTL-specific controls do not
   *  apply. Gain does (it is in the protocol); sample rate, PPM, bias-T, digital
   *  AGC and direct sampling do not — some have no wire representation at all,
   *  and the rest belong to whoever runs the server. De-emphasis and stereo are
   *  ours (they act on our own demodulator), so they stay. */
  isSpy?: boolean;
  biasTee: boolean;
  onBiasTee: (on: boolean) => void;
  agc: boolean;
  onAgc: (on: boolean) => void;
  directSampling: number;
  onDirectSampling: (mode: number) => void;
  deemph: number;            // FM de-emphasis tau (0=off, 50e-6, 75e-6)
  onDeemph: (tau: number) => void;
  stereo: boolean;           // WFM stereo on (true) vs forced mono
  onStereo: (on: boolean) => void;
}

const DEEMPH_OPTS: { label: string; value: number }[] = [
  { label: 'Off', value: 0 }, { label: '50µs', value: 50e-6 }, { label: '75µs', value: 75e-6 },
];

function Seg<T>({ options, value, onChange, fmt }: {
  options: T[]; value: T; onChange: (v: T) => void; fmt: (v: T) => string;
}) {
  return (
    <View style={styles.segRow}>
      {options.map((o, i) => {
        const active = o === value;
        return (
          <TouchableOpacity key={i} style={[styles.seg, active && styles.segActive]} onPress={() => onChange(o)}>
            <Text style={[styles.segTxt, active && styles.segTxtActive]}>{fmt(o)}</Text>
          </TouchableOpacity>
        );
      })}
    </View>
  );
}

export default function LocalHardwarePanel(p: LocalHardwarePanelProps) {
  const insets = useSafeAreaInsets();
  return (
    <Modal visible={p.visible} transparent animationType="slide" onRequestClose={p.onClose}
           supportedOrientations={['portrait', 'landscape', 'landscape-left', 'landscape-right']}>
      <TouchableWithoutFeedback onPress={p.onClose}>
        <View style={styles.backdrop} />
      </TouchableWithoutFeedback>
      <View style={[styles.sheet, {
        paddingBottom: insets.bottom + 12,
        paddingLeft: 16 + insets.left, paddingRight: 16 + insets.right,  // clear the notch in landscape
      }]}>
        <View style={styles.handleBar}>
          <Text style={styles.title}>{p.isSpy ? 'SpyServer Controls' : 'RTL-SDR Controls'}</Text>
          <TouchableOpacity onPress={p.onClose} hitSlop={10}><Text style={styles.close}>✕</Text></TouchableOpacity>
        </View>
        <ScrollView contentContainerStyle={{ paddingBottom: 16 }}>
          <Text style={styles.section}>GAIN</Text>
          <GainSlider gains={p.gains} gainTenthDb={p.gainTenthDb} auto={p.autoGain}
                      onAuto={p.onAuto} onGain={p.onGain} />
          {p.isSpy && <Text style={styles.note}>
            The SpyServer protocol sends a gain step, not a dB value — the labels are
            this receiver's nearest published gains. There is no auto-gain over the wire.
          </Text>}

          {!p.isSpy && <>
          <Text style={styles.section}>SAMPLE RATE</Text>
          {p.lockedRate && p.lockedRate > 0 ? (
            // The SERVER pinned the rate — it IGNORES a sampleRate message outright.
            // Showing a picker whose every use is silently dropped is worse than
            // showing none, so say who set it instead.
            <Text style={styles.note}>
              {`${(p.lockedRate / 1e6).toFixed(p.lockedRate % 1e6 === 0 ? 1 : 3)
                   .replace(/0+$/, '').replace(/\.$/, '.0')}M — set by the server.`}
            </Text>
          ) : <>
          {/* A real USB dongle runs sluggish/underfiltered below ~1 MHz, so only
              offer >=1 MHz for local hardware; RTL-TCP keeps the low rates (a
              networked rtl_tcp source like UberSDR only sends ~192 kHz). */}
          {/* VibeServer sends its own supported rates → use them verbatim; else
              RTL-TCP keeps the low rates and local USB filters to >=1 MHz. */}
          <Seg options={p.serverRates && p.serverRates.length
                          ? [...p.serverRates].sort((a, b) => a - b)
                          : p.isTcp ? SAMPLE_RATES : SAMPLE_RATES.filter(r => r >= 1_000_000)}
               value={p.sampleRate} onChange={p.onSampleRate}
               fmt={(r) => `${(r / 1e6).toFixed(r % 1e6 === 0 ? 1 : 3).replace(/0+$/, '').replace(/\.$/, '.0')}M`} />
          </>}
          </>}
          {p.isSpy && <Text style={styles.note}>
            Sample rate is chosen automatically from the mode: the server decimates
            before sending, which is what keeps a SpyServer usable over a hotspot or
            mobile data.
          </Text>}

          <Text style={styles.section}>FM DE-EMPHASIS</Text>
          <Seg options={DEEMPH_OPTS.map(d => d.value)} value={p.deemph} onChange={p.onDeemph}
               fmt={(v) => DEEMPH_OPTS.find(d => d.value === v)?.label ?? String(v)} />
          <Text style={styles.note}>50µs Europe/UK, 75µs Americas/Korea.</Text>

          <View style={styles.toggleRow}>
            <Text style={styles.toggleLabel}>FM Stereo</Text>
            <Switch value={p.stereo} onValueChange={p.onStereo} trackColor={{ true: C.abtn, false: '#444' }} thumbColor={p.stereo ? C.gold : '#ccc'} />
          </View>
          <Text style={styles.note}>Off forces mono — cleaner on weak/noisy signals.</Text>

          {!p.isSpy && <>
          <Text style={styles.section}>FREQUENCY CORRECTION (PPM)</Text>
          <View style={styles.stepperRow}>
            <TouchableOpacity style={styles.stepBtn} onPress={() => p.onPpm(p.ppm - 1)}><Text style={styles.stepBtnTxt}>−</Text></TouchableOpacity>
            <Text style={styles.stepVal}>{p.ppm > 0 ? `+${p.ppm}` : p.ppm} ppm</Text>
            <TouchableOpacity style={styles.stepBtn} onPress={() => p.onPpm(p.ppm + 1)}><Text style={styles.stepBtnTxt}>+</Text></TouchableOpacity>
          </View>

          <View style={styles.toggleRow}>
            <Text style={styles.toggleLabel}>Bias-T (5V antenna power)</Text>
            <Switch value={p.biasTee} onValueChange={p.onBiasTee} trackColor={{ true: C.abtn, false: '#444' }} thumbColor={p.biasTee ? C.gold : '#ccc'} />
          </View>
          <View style={styles.toggleRow}>
            <Text style={styles.toggleLabel}>RTL2832 digital AGC</Text>
            <Switch value={p.agc} onValueChange={p.onAgc} trackColor={{ true: C.abtn, false: '#444' }} thumbColor={p.agc ? C.gold : '#ccc'} />
          </View>

          <Text style={styles.section}>DIRECT SAMPLING</Text>
          <Seg options={DS_MODES.map(d => d.value)} value={p.directSampling} onChange={p.onDirectSampling}
               fmt={(v) => DS_MODES.find(d => d.value === v)?.label ?? String(v)} />
          <Text style={styles.note}>Not needed on RTL-SDR Blog V4 (HF is covered directly).</Text>
          </>}
          {p.isSpy && <Text style={[styles.note, { marginTop: 16 }]}>
            Frequency correction, bias-T, digital AGC and direct sampling are not part
            of the SpyServer protocol — they belong to whoever runs this receiver.
          </Text>}
        </ScrollView>
      </View>
    </Modal>
  );
}

const styles = StyleSheet.create({
  backdrop: { position: 'absolute', top: 0, left: 0, right: 0, bottom: 0, backgroundColor: 'rgba(0,0,0,0.5)' },
  sheet: { position: 'absolute', left: 0, right: 0, bottom: 0, maxHeight: '85%',
           backgroundColor: C.bg, borderTopLeftRadius: 16, borderTopRightRadius: 16,
           borderWidth: 1, borderColor: C.border, paddingHorizontal: 16, paddingTop: 10 },
  handleBar: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', marginBottom: 8 },
  title: { fontSize: 16, color: C.gold, fontWeight: '700' },
  close: { fontSize: 18, color: C.muted },
  section: { fontSize: 10, letterSpacing: 2, color: C.sectionC, marginTop: 16, marginBottom: 4 },
  segRow: { flexDirection: 'row', flexWrap: 'wrap', gap: 6 },
  seg: { paddingHorizontal: 12, paddingVertical: 6, borderRadius: 6, borderWidth: 1, borderColor: 'rgba(255,255,255,0.25)', backgroundColor: C.btnBg },
  segActive: { borderColor: C.abtn, backgroundColor: C.active },
  segTxt: { fontSize: 12, color: C.muted },
  segTxtActive: { color: C.gold },
  stepperRow: { flexDirection: 'row', alignItems: 'center', gap: 16 },
  stepBtn: { width: 44, height: 36, borderRadius: 6, borderWidth: 1, borderColor: 'rgba(255,255,255,0.25)', backgroundColor: C.btnBg, alignItems: 'center', justifyContent: 'center' },
  stepBtnTxt: { fontSize: 20, color: C.gold },
  stepVal: { fontSize: 15, color: C.muted, minWidth: 80, textAlign: 'center' },
  toggleRow: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', marginTop: 16 },
  toggleLabel: { fontSize: 14, color: C.muted },
  note: { fontSize: 11, color: C.dim, marginTop: 6, fontStyle: 'italic' },
});
