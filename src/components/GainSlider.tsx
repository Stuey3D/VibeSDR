import React from 'react';
import { View, Text, TouchableOpacity, StyleSheet } from 'react-native';
import Slider from '@react-native-community/slider';

// VibeSDR V4 — RTL-SDR tuner gain control. Used in both the RTL-SDR controls
// submenu and (a second copy) the demodulators popup for quick access. The
// `gains` list is the device's supported tuner gains in tenths of a dB.

const C = {
  gold:   '#ffe566',
  muted:  'rgba(255,255,255,0.92)',
  dim:    'rgba(200,210,225,0.90)',
  btnBg:  'rgba(20,18,14,0.85)',
  active: 'rgba(255,200,0,0.16)',
  border: 'rgba(255,229,102,0.55)',
};

export interface GainSliderProps {
  gains: number[];        // supported tuner gains, tenths of dB, ascending
  gainTenthDb: number;    // current manual gain (tenths of dB)
  auto: boolean;
  onAuto: (auto: boolean) => void;
  onGain: (tenthDb: number) => void;
  label?: string;
  /**
   * ★★★ WHOSE AGC THE AUTO BUTTON ENGAGES, which is not a detail — it decides whether the button
   *     should exist at all.
   *  ★★ On local hardware and on a VibeServer, "auto" is **VibeSDR's own AGC**: the shim watches
   *     the ADC and steps the tuner itself, and it deliberately never touches
   *     rtlsdr_set_tuner_gain_mode(dev, 0). It is named on screen because of what people have
   *     read elsewhere — the dongle's built-in AGC is unreliable and is KNOWN BROKEN on the v4, so
   *     a bare "AUTO" invites a user to dismiss a control that actually works (Stuart, 2026-08-21).
   *  ★★★ Over RTL-TCP there is no such thing. The protocol's only automatic mode is command 0x03,
   *      which turns on the dongle's own broken loop, so the button is NOT DRAWN there rather than
   *      offered and quietly harmful — the same rule as everywhere else in this app: a control that
   *      works on one path and misleads on another should not be there.
   */
  vibeAgc?: boolean;
  /**
   * ★★★ FALSE WHERE SOMETHING ELSE ALREADY OWNS THE AGC. On the server screen this slider sets the
   *     STARTING gain and the VibeAGC switch decides whether the loop runs — so an AGC/MANUAL pair
   *     above it was a second switch for the same thing, and pressing it DISABLED the slider,
   *     which is the one control that screen needs (Stuart, 2026-08-21: "we have 2 options for AGC
   *     ... if you used the agc button above the slider it locked the slider out").
   *  ★ Still true in the CLIENT, where this pair is the only way to engage the AGC.
   */
  modeButtons?: boolean;
}

function nearestIndex(gains: number[], tenthDb: number): number {
  let best = 0, bestD = Infinity;
  for (let i = 0; i < gains.length; i++) {
    const d = Math.abs(gains[i] - tenthDb);
    if (d < bestD) { bestD = d; best = i; }
  }
  return best;
}

export default function GainSlider({ gains, gainTenthDb, auto, onAuto, onGain, label = 'RF GAIN',
                                    vibeAgc = true, modeButtons = true }: GainSliderProps) {
  const haveGains = gains.length > 0;
  const idx = haveGains ? nearestIndex(gains, gainTenthDb) : 0;

  return (
    <View style={styles.wrap}>
      <View style={styles.headerRow}>
        <Text style={styles.label}>{label}</Text>
        {vibeAgc && modeButtons ? (
          <View style={styles.btnRow}>
            <TouchableOpacity
              style={[styles.btn, auto && styles.btnActive]}
              onPress={() => onAuto(true)}
            >
              {/* ★ "AGC", not "AUTO": it is a gain CONTROL LOOP, and the note below says whose. */}
              <Text style={[styles.btnTxt, auto && styles.btnTxtActive]}>VibeAGC</Text>
            </TouchableOpacity>
            <TouchableOpacity
              style={[styles.btn, !auto && styles.btnActive]}
              onPress={() => onAuto(false)}
            >
              <Text style={[styles.btnTxt, !auto && styles.btnTxtActive]}>MANUAL</Text>
            </TouchableOpacity>
          </View>
        ) : null}
      </View>
      <View style={styles.sliderRow}>
        <Slider
          style={{ flex: 1 }}
          minimumValue={0}
          maximumValue={haveGains ? gains.length - 1 : 1}
          step={1}
          value={idx}
          disabled={(auto && modeButtons) || !haveGains}
          onValueChange={(v: number) => { if (haveGains) onGain(gains[Math.round(v)]); }}
          minimumTrackTintColor={auto && modeButtons ? C.dim : C.gold}
          maximumTrackTintColor="rgba(255,255,255,0.25)"
          thumbTintColor={auto && modeButtons ? C.dim : C.gold}
        />
        <Text style={styles.val}>
          {auto && modeButtons ? 'VibeAGC' : haveGains ? `${(gains[idx] / 10).toFixed(1)} dB` : '—'}
        </Text>
      </View>
      {/* ★★ SAY WHOSE AGC IT IS. Everything written about RTL-SDR AGC online is about the dongle's
             own loop, which is unreliable and broken on the v4 — so an unnamed "auto" reads as that
             one, and a listener skips a control that works. */}
      {vibeAgc && modeButtons ? (
        <Text style={styles.note}>
          {auto
            ? "VibeAGC \u2014 VibeSDR's own AGC for RTL-SDR. It watches the ADC for overload and steps "
              + 'the tuner gain to suit. Switch to Manual to set the gain yourself.'
            : "Manual gain \u2014 nothing moves it but you. VibeAGC is VibeSDR's own loop for "
              + "RTL-SDR, not the dongle's built-in one, which is never used."}
        </Text>
      ) : !modeButtons ? null : (
        <Text style={styles.note}>
          Manual gain only over RTL-TCP: the protocol's automatic mode is the dongle's own AGC,
          which is unreliable and is known broken on the v4. VibeAGC needs a VibeServer.
        </Text>
      )}
    </View>
  );
}

const styles = StyleSheet.create({
  note: { color: C.dim, fontSize: 11, lineHeight: 15, marginTop: 6 },
  wrap: { paddingVertical: 6 },
  headerRow: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between' },
  label: { fontSize: 12, letterSpacing: 1.5, color: C.dim, fontWeight: '600' },
  btnRow: { flexDirection: 'row', gap: 6 },
  btn: { paddingHorizontal: 12, paddingVertical: 4, borderRadius: 6, borderWidth: 1, borderColor: 'rgba(255,255,255,0.25)', backgroundColor: C.btnBg },
  btnActive: { borderColor: C.border, backgroundColor: C.active },
  btnTxt: { fontSize: 11, color: C.muted },
  btnTxtActive: { color: C.gold },
  sliderRow: { flexDirection: 'row', alignItems: 'center', gap: 8, marginTop: 2 },
  val: { width: 64, textAlign: 'right', fontSize: 13, color: C.muted },
});
