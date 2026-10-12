import React from 'react';
import { View, Text, TouchableOpacity, StyleSheet } from 'react-native';
import Slider from '@react-native-community/slider';
import type { PopupTokens } from '../constants/popupTokens';

// VibeSDR V4 — RTL-SDR tuner gain control. Used in both the RTL-SDR controls
// submenu and (a second copy) the demodulators popup for quick access. The
// `gains` list is the device's supported tuner gains in tenths of a dB.

/* ★★★ THE COLOURS COME IN AS A PROP (Stuart, 2026-10-13: "fix it" — the gain slider still drew its own gold in the
 *   main app). Main app: track / fill / thumb and the VibeAGC / MANUAL keys in the CONTROLS colour, the label, value and
 *   note in the TEXT colour (`gainSliderInk(tokens)`). There are no meaning colours here — AUTO vs MANUAL is a key's
 *   state, not a status.
 * ★★★ LITE-SAFE: VibeServer Lite bundles this (via ServerModeScreen) on RN 0.73 with no Reanimated / Skia, so this file
 *   imports NOTHING from PopupShell / FaceplateContext — only a TYPE from popupTokens. With no `ink` it draws EXACTLY
 *   the old gold (GAIN_SLIDER_GOLD), which is what Lite shows; scripts/test_popup.ts holds both to it. */
export interface GainSliderInk {
  /** Fill and thumb (manual). */   fill: string;   thumb: string;
  /** A latched key's legend, border and fill. */   selText: string; selBorder: string; selBg: string;
  /** An unlatched key's legend, border and fill. */ key: string; keyBorder: string; keyBg: string;
  /** The unfilled track. */         track: string;
  /** Label, note, and the parked track / thumb while VibeAGC drives. */ dim: string;
  /** The value read-out. */         value: string;
}

/** Today's gold — the literals this slider always drew. Lite, and any caller passing no `ink`, gets exactly this. */
export const GAIN_SLIDER_GOLD: GainSliderInk = {
  fill: '#ffe566', thumb: '#ffe566',
  selText: '#ffe566', selBorder: 'rgba(255,229,102,0.55)', selBg: 'rgba(255,200,0,0.16)',
  key: 'rgba(255,255,255,0.92)', keyBorder: 'rgba(255,255,255,0.25)', keyBg: 'rgba(20,18,14,0.85)',
  track: 'rgba(255,255,255,0.25)', dim: 'rgba(200,210,225,0.90)', value: 'rgba(255,255,255,0.92)',
};

/** The user's colours as the slider's ink — controls colour for the keys and slider, text colour for the words. */
const inkCache = new WeakMap<PopupTokens, GainSliderInk>();
/** ★ Memoised per token object (tokens are memoised per setting), so a re-render reuses the same ink and sheet. */
export function gainSliderInk(pt: PopupTokens): GainSliderInk {
  let ink = inkCache.get(pt);
  if (!ink) { ink = buildInk(pt); inkCache.set(pt, ink); }
  return ink;
}
function buildInk(pt: PopupTokens): GainSliderInk {
  return {
    fill: pt.gold.fill, thumb: pt.gold.thumb,
    selText: pt.gold.sel, selBorder: pt.gold.selBorder, selBg: pt.ink.keyBgAct,
    key: pt.ink.key, keyBorder: pt.ink.keyBorder, keyBg: 'rgba(20,18,14,0.85)',
    track: pt.ink.keyBorder, dim: pt.ink.textDim, value: pt.ink.text,
  };
}

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
  /** The colours (see GAIN_SLIDER_GOLD / gainSliderInk). Omitted = today's gold, as Lite needs. */
  ink?: GainSliderInk;
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
                                    vibeAgc = true, modeButtons = true, ink = GAIN_SLIDER_GOLD }: GainSliderProps) {
  const styles = gainStyles(ink);
  const C = ink;
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
          minimumTrackTintColor={auto && modeButtons ? C.dim : C.fill}
          maximumTrackTintColor={C.track}
          thumbTintColor={auto && modeButtons ? C.dim : C.thumb}
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

/** One sheet per ink object (callers pass a memoised token-derived object, or the shared default). */
const styleCache = new WeakMap<GainSliderInk, ReturnType<typeof makeStyles>>();
function gainStyles(ink: GainSliderInk) {
  let st = styleCache.get(ink);
  if (!st) { st = makeStyles(ink); styleCache.set(ink, st); }
  return st;
}
const makeStyles = (C: GainSliderInk) => StyleSheet.create({
  note: { color: C.dim, fontSize: 11, lineHeight: 15, marginTop: 6 },
  wrap: { paddingVertical: 6 },
  headerRow: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between' },
  label: { fontSize: 12, letterSpacing: 1.5, color: C.dim, fontWeight: '600' },
  btnRow: { flexDirection: 'row', gap: 6 },
  btn: { paddingHorizontal: 12, paddingVertical: 4, borderRadius: 6, borderWidth: 1, borderColor: C.keyBorder, backgroundColor: C.keyBg },
  btnActive: { borderColor: C.selBorder, backgroundColor: C.selBg },
  btnTxt: { fontSize: 11, color: C.key },
  btnTxtActive: { color: C.selText },
  sliderRow: { flexDirection: 'row', alignItems: 'center', gap: 8, marginTop: 2 },
  val: { width: 64, textAlign: 'right', fontSize: 13, color: C.value },
});
