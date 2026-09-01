import React, { useEffect, useRef, useState } from 'react';
import {
  Modal, View, Text, TextInput, TouchableOpacity, TouchableWithoutFeedback, ScrollView,
  Switch, StyleSheet,
} from 'react-native';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import { useRepeatingKeys, NAV_REPEAT_KEYS, NAV_FOCUS, useKeyboardMode } from './PanelNav';
import GainSlider from './GainSlider';
import Slider from '@react-native-community/slider';
import type { RadioCaps } from '../services/UberSDRClient';
import {
  CONVERTER_PRESETS, type ConverterProfile, HF_CEILING_HZ, V4_WARNING,
  fromPreset, isIdentity, presetById,
} from '../services/converter';

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
/** ★ The tuner IF widths the picker offers. -1 = AUTO (follows the zoom), 0 = wide open.
 *  ★★ The same list the web client offers, minus the ones that were no-ops there: below about
 *     350 kHz the R820T stops honouring the request, so offering 300/250/200 was three choices
 *     that all did the same thing. */
const TUNER_BWS = [-1, 0, 1_500_000, 1_000_000, 700_000, 500_000, 350_000];

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
   *  and the rest belong to whoever runs the server.
   *  ★ De-emphasis and stereo used to be the exception here — ours, not the radio's, so they
   *  stayed on a SpyServer. They now live in the AUDIO sheet, which is where being "ours"
   *  always pointed. */
  isSpy?: boolean;
  /** ★★★ THE CONVERTER IN FRONT OF THIS RADIO — absent (and the section undrawn) wherever it
   *  cannot apply. The PARENT decides that: local USB, rtl_tcp and SpyServer get one; a
   *  VibeServer does not, because its owner sets a converter on the radio's own setup page and
   *  the server then publishes true RF to every client. See converter.ts. */
  converter?:   ConverterProfile;
  onConverter?: (c: ConverterProfile) => void;
  biasTee: boolean;
  onBiasTee: (on: boolean) => void;
  agc: boolean;
  onAgc: (on: boolean) => void;
  directSampling: number;
  onDirectSampling: (mode: number) => void;
  /** ★★ WHAT THE CONNECTED RADIO ACTUALLY IS, from the server. Null = unknown, in which case
   *  we fall back to the dongle layout, which is what every VibeServer was before there were
   *  other radios. ★ Never INFER the driver from what else is present: an Airspy HF+ was drawn
   *  as a dongle for exactly that reason, and the controls it does have were nowhere. */
  radio?: RadioCaps | null;
  /** ★★ ADMIN LOCK. The server ENFORCES this already — bias-T, PPM, direct sampling and
   *  calibration are all refused without it. What was missing was any sign of it here: the
   *  controls drew as normal and the user found out only when one silently did nothing
   *  (Stuart, 2026-07-27). A protection nobody can see is not obviously protecting anything. */
  /** ★ The owner has FORCED the AGC on — a listener may not set a gain at all. From hwinfo. */
  agcLocked?: boolean;
  /** ★★★ The owner has FIXED the gain on this band: the ceiling IS the setting, and the server
   *  refuses every gain message from anybody. Unlike agcLocked this applies to EVERY driver —
   *  including the HackRF, where there is no AGC to reason about and the lock still closes both
   *  stages. Per band, so it comes and goes as the listener tunes. */
  gainLocked?: boolean;
  /** ★★ THE TUNER'S IF FILTER — the only selectivity ahead of the mixer, and the app never had a
   *  picker for it while the web client always has. `hasTunerBw` is false on a server that never
   *  mentioned it, and then nothing is drawn: an absent field is not "wide open", it is a receiver
   *  we have not been told about. -1 = auto (follows the zoom), 0 = wide, else Hz. */
  hasTunerBw?: boolean;
  tunerBw?: number;
  tunerBwAuto?: boolean;
  onTunerBw?: (hz: number) => void;
  adminSet?: boolean;
  adminOk?: boolean;
  /** Password entered by the user. Resolving it to a nonce+HMAC is the screen's job. */
  onAdminUnlock?: (password: string) => void;
  /** Set when the server refused something, so the panel can say why rather than sit there. */
  adminRefused?: boolean;
  /** Airspy HF+ live state + setters (only used when radio.driver === 'airspyhf'). */
  ahfAgc?: boolean;      onAhfAgc?: (on: boolean) => void;
  ahfAgcHigh?: boolean;  onAhfAgcThreshold?: (high: boolean) => void;
  ahfAtt?: number;       onAhfAtt?: (steps: number) => void;
  ahfLna?: boolean;      onAhfLna?: (on: boolean) => void;
  /** ★★ SDRplay RSP live state + setters (only when radio.driver === 'sdrplay'). Mirrors the web
   *  client's rspCtls, which is the reference implementation — the two should look the same, and
   *  an RSP arriving on the phone with a DONGLE's single gain slider is the "else means dongle"
   *  trap in its user-visible form: the panel said "RSP1B Controls" and then offered RF GAIN.
   *  ★ The RSP's gain is TWO things — an LNA STATE (RF, where overload happens) and an IF gain
   *  REDUCTION — and conflating them into one slider is what made a dongle-shaped control useless
   *  here. On an RSP it is the front end that overloads, and RF overload is what destroys RDS. */
  rspSysGain?: number;   // API's own total system gain, dB (0 = unknown)
  rspOverload?: boolean; // the radio's OWN ADC-clipping event, not inferred from the spectrum
  rspSettling?: boolean; // just after a gain change: the reading is not meaningful yet
  rspLna?: number;       onRspLna?: (state: number) => void;
  rspIfGr?: number;      onRspIfGr?: (db: number) => void;
  rspIfAgc?: boolean;    onRspIfAgc?: (on: boolean) => void;
  rspAgcSet?: number;    onRspAgcSet?: (dbfs: number) => void;
  rspRfNotch?: boolean;  onRspRfNotch?: (on: boolean) => void;
  rspDabNotch?: boolean; onRspDabNotch?: (on: boolean) => void;
  rspBiasT?: boolean;    onRspBiasT?: (on: boolean) => void;
  /** ★★★ HackRF One live state + setters (only when radio.driver === 'hackrf'). EXPERIMENTAL —
   *  nobody on the VibeSDR side owns one, and the panel says so on screen for the same reason the
   *  web client's does: a user meeting a control that has never been tested deserves to be told.
   *  ★★★ THREE STAGES AND NO AGC. libhackrf exposes no automatic mode, so there is deliberately
   *  no auto switch here — an AGC toggle that did nothing is the fault AGENTS.md names outright,
   *  and it is why this radio cannot reuse the dongle's GainSlider (which offers exactly that).
   *  ★★★ amp and bias-T are OWNER-ONLY. The server refuses them too (adminGate), so `locked`
   *  here makes the lock visible rather than being the only thing standing in the way. */
  /** ★★★ THE OWNER'S GAIN CEILING for the frequency in use, TENTHS of a dB, -1 = none.
   *  ★★★ THE SERVER ENFORCES IT ANYWAY — see the hackrf_control and gain handlers, which clamp
   *  whatever arrives, so a hand-rolled client cannot get past it. This is here so the PANEL does
   *  not lie: without it a listener drags to maximum, the readout says 40 dB and the radio is at
   *  20. Stuart: "cant have a client saying they are max gain when the server is limiting them
   *  which I have seen before." Same contract as lockedRate and agcLocked.
   *  ★ It is frequency-dependent, so it can change on a RETUNE — it is live state, not a
   *  property of the radio, which is why it is not in RadioCaps. */
  gainCapTenthDb?: number;
  /** ★★ The LEAST IF gain reduction the owner allows on this band, in dB — the same units as the
   *  slider below, so there is no conversion to get backwards (Saber, 2026-08-28: it should read
   *  as gain reduction "as per the control in the client"). -1 = none. Clamped rather than refused
   *  server-side, so without this the slider visibly springs back and reads as broken. */
  ifGrFloorDb?: number;
  hrfAmp?: boolean;      onHrfAmp?: (on: boolean) => void;
  hrfLna?: number;       onHrfLna?: (db: number) => void;
  hrfVga?: number;       onHrfVga?: (db: number) => void;
  hrfBiasT?: boolean;    onHrfBiasT?: (on: boolean) => void;
}

function Seg<T>({ options, value, onChange, fmt, slot }: {
  options: T[]; value: T; onChange: (v: T) => void; fmt: (v: T) => string;
  /** Claims a place in the panel's focus order — see the note in LocalHardwarePanel. */
  slot?: (run: () => void) => boolean;
}) {
  // ★★★ THE FOCUS RING IS NOT A SELECTION. It was drawn whenever a slot held keyboard focus —
  //   including when nobody had touched a keyboard — so opening this panel put a green ring on the
  //   FIRST control (AGC threshold "Low") while the actually-selected option was highlighted too.
  //   Two green things, and it reads as both being on. (Stuart, 2026-07-30, on the Mac.)
  //   The app already tracks this: PanelNav.useKeyboardMode() is true only after a real key, and
  //   SDRScreen calls noteTouchInteraction() on every touch to end it. This panel simply never
  //   asked. Gate the ring on it and the ambiguity disappears without changing the focus order.
  const kb = useKeyboardMode();
  return (
    <View style={styles.segRow}>
      {options.map((o, i) => {
        const active = o === value;
        const on = slot?.(() => onChange(o));
        return (
          <TouchableOpacity key={i}
            style={[styles.seg, active && styles.segActive,
                    on && kb && { borderColor: NAV_FOCUS, borderWidth: 2 }]}
            onPress={() => onChange(o)}>
            <Text style={[styles.segTxt, active && styles.segTxtActive]}>{fmt(o)}</Text>
          </TouchableOpacity>
        );
      })}
    </View>
  );
}

export default function LocalHardwarePanel(p: LocalHardwarePanelProps) {
  const insets = useSafeAreaInsets();
  // ★ Same rule as Seg's ring: only show keyboard focus when a keyboard is actually driving.
  const kbNav = useKeyboardMode();
  const [adminPw, setAdminPw] = useState('');
  // ★ Decide from what the RADIO SAID, never from what else happens to be set.
  const isAhf = p.radio?.driver === 'airspyhf';
  const isRsp = p.radio?.driver === 'sdrplay';
  const isHrf = p.radio?.driver === 'hackrf';
  /* ★★★ THE CEILING, IN WHOLE dB, or -1 for none. The wire carries tenths (the dongle's spelling,
   *   which every radio's limit reuses); the HackRF's stages are whole dB, so it is floored —
   *   NEVER rounded, because rounding up would offer a decibel the server then refuses. */
  const capT = p.gainCapTenthDb ?? -1;
  /* ★ The owner's figure IS this slider's floor — both are a reduction in dB. Held inside what the
   *  radio reports it can do, so a stale or silly figure cannot push the control off its own scale. */
  const ifGrFloor = (p.ifGrFloorDb ?? -1) >= 0
    ? Math.max(p.radio?.ifGrMin ?? 20, Math.min(p.radio?.ifGrMax ?? 59, p.ifGrFloorDb ?? 20))
    : (p.radio?.ifGrMin ?? 20);
  const capDb = capT >= 0 ? Math.floor(capT / 10) : -1;
  /* ★★★ THE CAP IS ON THE TOTAL, SO THE TWO STAGES SHARE ONE BUDGET — each one's ceiling is the
   *   cap MINUS what the other is using. Both sit AFTER the mixer (the only pre-mixer stage is the
   *   amp, which is owner-only), so LNA + VGA together is what drives the 8-bit converter;
   *   capping them separately would permit twice what the owner wrote. Mirrors applyHrfGainCap()
   *   in the web client and the clamp in the server's hackrf_control handler — all three must
   *   agree or the panel misdescribes the radio. */
  const hrfLnaNow = p.hrfLna ?? 0, hrfVgaNow = p.hrfVga ?? 0;
  const hrfLnaMax = capDb < 0 ? 40 : Math.min(40, Math.max(0, capDb - hrfVgaNow));
  const hrfVgaMax = capDb < 0 ? 62 : Math.min(62, Math.max(0, capDb - hrfLnaNow));
  /* ★★★ AND THE DONGLE'S SLIDER TOO — this was wrong before the HackRF existed. The gain LIST
   *   came straight from the radio with the owner's ceiling never applied, so the slider ran to
   *   the hardware's maximum while the server clamped every value above the limit. Drop the
   *   entries the server would refuse rather than let the readout claim them. ★ Keep at least
   *   one, so a very low cap leaves a working control rather than an empty slider. */
  const cappedGains = capT >= 0 && p.gains.length
    ? (p.gains.filter((g) => g <= capT).length ? p.gains.filter((g) => g <= capT)
                                               : [p.gains[0]])
    : p.gains;
  // ★ Locked = the server has a password and this session has not cleared it.
  const locked = !!p.adminSet && !p.adminOk;
  /* ★★★ WHAT THIS LISTENER CAN ACTUALLY CHANGE — and the panel now shows ONLY that.
   *  ★★★ IT USED TO SHOW EVERYTHING AND MEAN NOTHING. Bias-T, PPM, the digital AGC and direct
   *      sampling were drawn for every listener on a protected receiver, so the switches moved,
   *      the panel showed bias-T ON, and the server ignored every one of them. Stuart: "a user
   *      could turn on the Bias-T in their app although it is not going to the server but it
   *      still shows as being on ... they say locked out by admin but still look like they do
   *      stuff." A control that reports a state the radio is not in is worse than a missing one:
   *      the user believes it, and on bias-T they may believe they are powering an aerial.
   *  ★★ THIS IS THE HOUSE RULE, not a new idea — AGENTS.md: never offer a control whose every use
   *     is a no-op, and the same reasoning already hides the gain slider while the AGC owns it
   *     ("a disabled slider still reads as an offer"). Hidden, not greyed. */
  const canProtected = !locked;                      // bias-T, PPM, digital AGC, direct sampling
  /* ★★★ "AGC LOCKED" CANNOT LOCK AN AGC THAT DOES NOT EXIST. agcLocked is a flat server-wide flag
   *   with no notion of driver, and this gate wraps the WHOLE gain branch — so on a HackRF it hid
   *   all three manual stages and printed "the gain is managed by this receiver: its owner has
   *   fixed the automatic gain control on" to the owner of a radio that has no automatic gain of
   *   any kind. Every word of that sentence is false there.
   * ★★ The web client is unaffected only by accident of structure: agcLocked touches #gainAuto and
   *   #gain, and a HackRF uses neither — it has its own hrfLna/hrfVga and no AUTO button at all.
   *   This brings the panel to the same place deliberately.
   * ★ NOT extended to the HF+ or the RSP, which DO have an AGC: the server closes their manual
   *   path when locked (see ahf_control — "a lock in name only" otherwise), so hiding is right
   *   there. ★★ AND WHEN VibeAGC FOR THE HACKRF ARRIVES, this must come back: at that point the
   *   lock means something and should close LNA and VGA too, exactly as the HF+ does. Stuart,
   *   2026-08-27: "there is no hackrf agc yet." */
  /* ★★★ AND THE GAIN LOCK CLOSES IT ON EVERY RADIO, HackRF included. The HackRF exception above is
   *   about an AGC it does not have; this is about a figure the owner has fixed, which is a claim
   *   about the RADIO and true of all three. The server refuses the message, so an offered control
   *   would spring back — the same house rule, reached from the other direction. */
  const canGain      = !p.gainLocked && (!p.agcLocked || p.radio?.driver === 'hackrf');
  const canRate      = !(p.lockedRate && p.lockedRate > 0);
  /* ★ Nothing left but the password box. Then the panel does not pretend to be a control panel:
   *  it says what it is and puts the cursor where the only useful action is. */
  const nothingForUser = !canProtected && !canGain && !canRate;
  const isRtl = !p.radio || p.radio.driver === 'rtl';

  // ★ One flat focus order over everything on the panel, claimed during render in JSX order —
  // the same pattern as the server picker, so the order is whatever is actually on screen.
  // Arrows move, Enter or Space activates, and Esc is handled by SDRScreen along with every
  // other overlay.
  //
  // ★ ANDROID ONLY, so this cannot be exercised on an iPhone: local USB hardware is an Android
  // feature. Wired for completeness rather than because it could be tested here.
  const slots = useRef<Array<() => void>>([]);
  slots.current = [];
  const [count, setCount] = useState(0);
  const [idx, setIdx] = useState(0);
  useEffect(() => { if (slots.current.length !== count) setCount(slots.current.length); });
  useEffect(() => { if (p.visible) setIdx(0); }, [p.visible]);

  const kb = useRef({ idx });
  kb.current = { idx };
  useRepeatingKeys(p.visible, (k: string) => {
    const n = slots.current.length;
    if (!n) return;
    const i = kb.current.idx;
    if (k === 'ArrowUp' || k === 'ArrowLeft')    { setIdx(Math.max(0, i - 1)); return; }
    if (k === 'ArrowDown' || k === 'ArrowRight') { setIdx(Math.min(n - 1, i + 1)); return; }
    if (k === 'Enter' || k === 'Space') slots.current[i]?.();
  }, NAV_REPEAT_KEYS);

  const slot = (run: () => void) => {
    const i = slots.current.length;
    slots.current.push(run);
    return idx === i;
  };
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
          {/* ★ NAME THE ACTUAL RADIO. This said "RTL-SDR Controls" over a panel driving an
              Airspy HF+ (Stuart, 2026-07-27) — which is not just wrong, it tells the user the
              app has misidentified their hardware. The server already reports a model string
              taken from the USB descriptor, i.e. what is written on the box. */}
          <Text style={styles.title}>
            {p.isSpy ? 'SpyServer Controls'
             : p.radio?.model ? `${p.radio.model} Controls`
             : 'Local SDR Controls'}
          </Text>
          <TouchableOpacity onPress={p.onClose} hitSlop={10}><Text style={styles.close}>✕</Text></TouchableOpacity>
        </View>
        <ScrollView contentContainerStyle={{ paddingBottom: 16 }}>
          {/* ★ The lock notice goes FIRST, before any control — it explains the whole panel,
              and finding it underneath the thing it applies to would be no use. */}
          {p.adminSet && (
            <View style={[styles.adminCard, p.adminOk && styles.adminCardOk]}>
              <Text style={styles.adminTitle}>
                {p.adminOk ? 'UNLOCKED' : 'PROTECTED BY THE OWNER'}
              </Text>
              <Text style={styles.note}>
                {p.adminOk
                  ? 'Full settings are unlocked for this session.'
                  /* ★★ SAY WHAT IS LEFT, TRUTHFULLY. The old line always promised "Gain, sample
                     rate and tuning stay open" — which is a lie on a receiver whose owner has
                     also fixed the AGC and pinned the rate, and that is exactly the receiver
                     where a listener finds nothing to do and no explanation. */
                  : nothingForUser
                    ? 'Every hardware control on this receiver is set by its owner. There is '
                      + 'nothing here for a listener to change — enter the password to take it over.'
                    : 'Bias-T, frequency correction and direct sampling are locked on this receiver.'
                      + (canGain ? (canRate ? ' Gain, sample rate and tuning stay open.'
                                            : ' Gain and tuning stay open.')
                                 : (canRate ? ' Sample rate and tuning stay open.'
                                            : ' Tuning stays open.'))}
              </Text>
              {!p.adminOk && (
                <View style={styles.adminRow}>
                  <TextInput
                    value={adminPw} onChangeText={setAdminPw}
                    placeholder="Admin password" placeholderTextColor="rgba(200,210,225,0.45)"
                    secureTextEntry autoCapitalize="none" autoCorrect={false}
                    /* ★ When the password is the ONLY thing on the panel, put the cursor in it.
                       Stuart: "it should immediately ask for the admin password". Never focus it
                       otherwise — a keyboard covering a panel somebody opened to move the gain is
                       an obstacle, not a shortcut. */
                    autoFocus={nothingForUser}
                    onSubmitEditing={() => { p.onAdminUnlock?.(adminPw); setAdminPw(''); }}
                    returnKeyType="go"
                    style={styles.adminInput} />
                  <TouchableOpacity style={styles.adminBtn}
                    onPress={() => { p.onAdminUnlock?.(adminPw); setAdminPw(''); }}>
                    <Text style={styles.adminBtnTxt}>UNLOCK</Text>
                  </TouchableOpacity>
                </View>
              )}
              {p.adminRefused && !p.adminOk && (
                <Text style={[styles.note, { color: '#ff8a7d' }]}>
                  That control is locked. Enter the owner's password to use it.
                </Text>
              )}
            </View>
          )}
          {/* ★★ GAIN IS NOT ONE CONTROL ACROSS RADIOS. A dongle has a tuner gain TABLE; an
              HF+ has an AGC, an attenuator in 6 dB steps and a preamp, and no table at all —
              so the slider was drawing an empty/meaningless scale while the controls that do
              work were missing entirely. */}
          {/* ★★ AND THE GAIN GOES WITH THEM when the owner has FORCED the AGC on. The server
                 refuses a gain from a listener then — the same "every use is a no-op" test — so
                 the slider is hidden rather than left to move and snap back. The line below says
                 why, because a missing gain control with no explanation reads as a fault. */}
          {/* ★★ AND SAY WHICH OF THE TWO IT IS. A fixed gain and a locked AGC are different facts
                 about the radio — one is a figure the owner chose, the other is a loop they trust
                 — and telling a listener "the automatic gain control is on" about a receiver whose
                 gain is simply pinned is the same class of wrong sentence as printing it on a
                 HackRF, which is the note this branch already carries. */}
          {!canGain && (
            <Text style={[styles.note, { marginTop: 12 }]}>
              {p.gainLocked
                ? 'The gain is fixed on this band: its owner has set it and it is the same for '
                  + 'everybody listening. Tune elsewhere and the controls come back if they are '
                  + 'free there.'
                : 'The gain is managed by this receiver: its owner has fixed the automatic gain '
                  + 'control on, so it is the same for everybody listening.'}
            </Text>
          )}
          {canGain && (isAhf ? (
            <>
              <Text style={styles.section}>GAIN — {p.radio?.model || 'Airspy HF+'}</Text>
              <View style={styles.toggleRow}>
                <Text style={styles.toggleLabel}>Automatic gain (AGC)</Text>
                <Switch value={!!p.ahfAgc} onValueChange={(v) => p.onAhfAgc?.(v)}
                  trackColor={{ true: C.abtn, false: '#444' }} thumbColor={p.ahfAgc ? C.gold : '#ccc'} />
              </View>
              <Text style={styles.note}>
                The radio's own AGC, and the right answer for most listening. Turn it off to set
                the attenuator by hand.
              </Text>

              {p.radio?.agcThreshold && (
                <>
                  <Text style={styles.section}>AGC THRESHOLD</Text>
                  <Seg slot={slot} options={[false, true]} value={!!p.ahfAgcHigh}
                       onChange={(v) => p.onAhfAgcThreshold?.(v)}
                       fmt={(v) => (v ? 'High' : 'Low')} />
                  <Text style={styles.note}>
                    Tells the AGC to tolerate 3 dB more signal before it steps the attenuator, so
                    High is the MORE sensitive setting — Airspy recommend it for marginal signals
                    next to very strong blockers. ★ Measured to have no audible effect above
                    60 MHz — the API accepts it and the radio ignores it there.
                  </Text>
                </>
              )}

              {/* ATTENUATOR — a SLIDER, like every other gain control in the app. It is a
                  continuous quantity you sweep while watching the noise floor, not something
                  you step to a known number, and steppers make that a chore.
                  ★ Ignored by the hardware while the AGC is on, so say so rather than let
                  someone drag a control that is being overridden. */}
              <Text style={styles.section}>ATTENUATOR</Text>
              <View style={styles.sliderRow}>
                <Text style={styles.sliderEnd}>0</Text>
                <Slider
                  style={{ flex: 1, height: 40 }}
                  minimumValue={0}
                  maximumValue={(p.radio?.attSteps ?? 9) - 1}
                  step={1}
                  value={p.ahfAtt ?? 0}
                  onValueChange={(v) => p.onAhfAtt?.(Math.round(v))}
                  minimumTrackTintColor={C.abtn}
                  maximumTrackTintColor="#444"
                  thumbTintColor={C.gold}
                />
                <Text style={styles.sliderEnd}>
                  {((p.radio?.attSteps ?? 9) - 1) * (p.radio?.attStepDb ?? 6)}
                </Text>
              </View>
              {/* ★ Never quote a dB figure the radio is not applying. While the AGC is on it owns
                  the front end and libairspyhf offers no getter to read back what it chose, so the
                  last manual value would sit here looking like it was in effect. */}
              <Text style={styles.stepVal}>
                {p.ahfAgc ? 'set by AGC' : `${(p.ahfAtt ?? 0) * (p.radio?.attStepDb ?? 6)} dB`}
              </Text>
              <Text style={styles.note}>
                {p.ahfAgc
                  ? 'The AGC is on, so the radio is setting this itself — turn AGC off to use it.'
                  : `0–${((p.radio?.attSteps ?? 9) - 1) * (p.radio?.attStepDb ?? 6)} dB in ${p.radio?.attStepDb ?? 6} dB steps. On an HF+ the useful range is attenuation, not gain.`}
              </Text>

              {/* ★★★ "+6 dB preamp" WAS THE WRONG STORY. Airspy's own documentation: the preamp
                  option "might be misleading, and should probably be renamed postamp. It has the
                  same effect on the noise figure and the linearity as a preamp, but acts on the
                  BACK-END (ADC + Digital/DSP post processing)... turn the preamp option off to get
                  an extra 12 dB of dynamic range by scrapping the quantization noise margin."
                  ★ So it is not free gain, it is a dynamic-range trade — and OFF is the better
                  default on any real antenna. Stuart measured the same thing independently: on VHF
                  the preamp HALVED the RDS constellation. Naming a control after what it does to
                  YOUR signal beats naming it after the number in the datasheet. */}
              {p.radio?.hfLna && (
                <>
                  <View style={styles.toggleRow}>
                    <Text style={styles.toggleLabel}>Preamp</Text>
                    <Switch value={!!p.ahfLna} onValueChange={(v) => p.onAhfLna?.(v)}
                      trackColor={{ true: C.abtn, false: '#444' }} thumbColor={p.ahfLna ? C.gold : '#ccc'} />
                  </View>
                  <Text style={styles.note}>
                    Airspy call this one misleading: it acts on the ADC and DSP back end, not the
                    antenna. Leaving it OFF buys about 12 dB of dynamic range. Turn it on only on a
                    quiet band chasing something weak.
                  </Text>
                </>
              )}
            </>
          ) : isRsp ? (
            <>
              {/* ★★★ AN RSP'S GAIN IS TWO CONTROLS, NOT ONE. LNA STATE sets the RF front end —
                  where overload actually happens — and IF GAIN REDUCTION sets what follows it.
                  A dongle's single gain slider cannot express that, which is why this panel
                  said "RSP1B Controls" and then offered a control the radio does not have.
                  Ported from the web client's rspCtls, which is the reference: the two clients
                  should offer the same radio the same things. */}
              <Text style={styles.section}>GAIN — {p.radio?.model || 'SDRplay RSP'}</Text>
              {/* ★★ WHAT THE RADIO IS DOING, not what the sliders were last set to. The API
                  computes total system gain itself, and under AGC the IF reduction is the AGC's
                  to move — so a slider reading would be a lie while this is the truth. */}
              <View style={styles.toggleRow}>
                <Text style={styles.toggleLabel}>System gain</Text>
                <Text style={[styles.stepVal, p.rspOverload ? { color: '#ff8a7d' } : null]}>
                  {p.rspOverload ? 'OVERLOAD'
                    : p.rspSettling ? 'settling…'
                    : (p.rspSysGain ?? 0) > 0 ? `${(p.rspSysGain ?? 0).toFixed(1)} dB` : '—'}
                </Text>
              </View>
              {/* ★ The radio raises OVERLOAD itself when its ADC clips — no inference from the
                  spectrum, unlike a dongle. It is also the thing that destroys RDS, so it is
                  worth shouting about rather than burying. */}
              <Text style={styles.note}>
                RF overload is what kills RDS on this radio. If OVERLOAD shows, drop the LNA
                state (more reduction) before touching anything else.
              </Text>

              <Text style={styles.section}>RF GAIN (LNA STATE)</Text>
              <View style={styles.stepperRow}>
                <TouchableOpacity style={styles.stepBtn}
                  onPress={() => p.onRspLna?.(Math.max(0, (p.rspLna ?? 0) - 1))}>
                  <Text style={styles.stepBtnTxt}>−</Text></TouchableOpacity>
                <Text style={styles.stepVal}>
                  {(p.rspLna ?? 0)} / {Math.max(0, (p.radio?.lnaStates ?? 10) - 1)}
                </Text>
                <TouchableOpacity style={styles.stepBtn}
                  onPress={() => p.onRspLna?.(Math.min((p.radio?.lnaStates ?? 10) - 1, (p.rspLna ?? 0) + 1))}>
                  <Text style={styles.stepBtnTxt}>+</Text></TouchableOpacity>
              </View>
              <Text style={styles.note}>0 = most RF gain. Higher states attenuate the front end.</Text>

              <View style={styles.toggleRow}>
                <Text style={styles.toggleLabel}>IF AGC</Text>
                <Switch value={p.rspIfAgc !== false} onValueChange={(v) => p.onRspIfAgc?.(v)}
                  trackColor={{ true: C.abtn, false: '#444' }} thumbColor={p.rspIfAgc !== false ? C.gold : '#ccc'} />
              </View>

              {/* ★ Hidden, not greyed, while the AGC owns it — a disabled slider still reads as
                  an offer, and the same rule removed the HF+'s rate picker. */}
              {p.rspIfAgc === false && (
                <>
                  <Text style={styles.section}>IF GAIN REDUCTION</Text>
                  <View style={styles.stepperRow}>
                    <Slider style={{ flex: 1 }}
                            minimumValue={ifGrFloor} maximumValue={p.radio?.ifGrMax ?? 59}
                            step={1} value={Math.max(ifGrFloor, p.rspIfGr ?? 40)}
                            onSlidingComplete={(v: number) => p.onRspIfGr?.(Math.round(v))}
                            minimumTrackTintColor={C.gold} maximumTrackTintColor="#555" thumbTintColor={C.gold} />
                    <Text style={styles.stepVal}>{p.rspIfGr ?? 40} dB</Text>
                  </View>
                  <Text style={styles.note}>
                    More reduction = less gain.
                    {(p.ifGrFloorDb ?? -1) >= 0
                      ? ` The owner requires at least ${ifGrFloor} dB of reduction on this band.` : ''}
                  </Text>
                </>
              )}

              {(p.radio?.rfNotch || p.radio?.dabNotch || p.radio?.biasT) && (
                <>
                  <Text style={styles.section}>FILTERS</Text>
                  {p.radio?.rfNotch && (
                    <View style={styles.toggleRow}>
                      {/* ★★ RF, not FM: the RSP's broadcast notch covers MW AND FM (the call has
                          always been setRfNotch). Labelling it FM told anyone with MW breakthrough
                          there was nothing here for them. */}
                      <Text style={styles.toggleLabel}>RF notch (MW/FM)</Text>
                      <Switch value={!!p.rspRfNotch} onValueChange={(v) => p.onRspRfNotch?.(v)}
                        trackColor={{ true: C.abtn, false: '#444' }} thumbColor={p.rspRfNotch ? C.gold : '#ccc'} />
                    </View>
                  )}
                  {p.radio?.dabNotch && (
                    <View style={styles.toggleRow}>
                      <Text style={styles.toggleLabel}>DAB notch</Text>
                      <Switch value={!!p.rspDabNotch} onValueChange={(v) => p.onRspDabNotch?.(v)}
                        trackColor={{ true: C.abtn, false: '#444' }} thumbColor={p.rspDabNotch ? C.gold : '#ccc'} />
                    </View>
                  )}
                  <Text style={styles.note}>
                    A notch is wanted ON elsewhere in the spectrum, where a strong local
                    transmitter is what overloads the front end — and OFF when that band is what
                    you came to hear.
                  </Text>
                </>
              )}
            </>
          ) : isHrf ? (
            <>
              <Text style={styles.section}>GAIN — {p.radio?.model || 'HackRF One'}</Text>
              <Text style={styles.note}>
                Experimental, and manual: this radio has three gain stages and no automatic gain
                of any kind. Bring the LNA up until signals are clear of the noise and no further
                — eight bits clip where other radios cope.
              </Text>

              {/* ★★★ RF AMP — OWNER-ONLY, AND THE HARDEST-EARNED LOCK IN THE APP. Stuart: "the
                  preamp control MUST be treated with the same caution as the Bias-T and password
                  protected so a stranger cannot enable the preamp and blow it up." It is the U14
                  MGA-81563, a real 14 dB amplifier in FRONT of everything on a radio with no AGC
                  and an unprotected front end — max safe RX is -5 dBm and static while swapping
                  an aerial can kill it. ★★ AND THE TELL IS BACKWARDS: a blown amp acts as an
                  ATTENUATOR, so switching it ON makes the band go quieter. */}
              <View style={styles.toggleRow}>
                <Text style={styles.toggleLabel}>RF amp (+14 dB){locked ? ' — owner only' : ''}</Text>
                <Switch value={!!p.hrfAmp} onValueChange={(v) => p.onHrfAmp?.(v)}
                  disabled={locked}
                  trackColor={{ true: C.abtn, false: '#444' }} thumbColor={p.hrfAmp ? C.gold : '#ccc'} />
              </View>
              <Text style={styles.note}>
                A 0 or +14 dB amplifier ahead of everything, and the part of a HackRF that most
                commonly dies. Leave it off unless you know what is on the end of the coax. If
                switching it on makes signals get QUIETER, the amp is already blown.
              </Text>

              {/* 0-40 dB in 8 dB steps — the hardware's own granularity, not a scale we invented.
                  ★ A slider, like every other gain control here, because it is swept while
                  watching the noise floor rather than stepped to a known number. */}
              <Text style={styles.section}>LNA GAIN</Text>
              <View style={styles.sliderRow}>
                <Text style={styles.sliderEnd}>0</Text>
                <Slider style={{ flex: 1, height: 40 }}
                  minimumValue={0} maximumValue={Math.max(8, hrfLnaMax)} step={8}
                  value={Math.min(hrfLnaNow, hrfLnaMax)}
                  onValueChange={(v) => p.onHrfLna?.(Math.min(hrfLnaMax, Math.round(v)))}
                  minimumTrackTintColor={C.abtn} maximumTrackTintColor="#444" thumbTintColor={C.gold} />
                <Text style={styles.sliderEnd}>{hrfLnaMax}</Text>
              </View>
              <Text style={styles.stepVal}>{Math.min(hrfLnaNow, hrfLnaMax)} dB</Text>
              <Text style={styles.note}>
                The front-end stage. Raise this BEFORE the VGA — gain taken here costs the least
                noise. Back it off if you see images or the audio breaks up on peaks.
              </Text>

              {/* 0-62 dB in 2 dB steps, baseband, after the mixer. */}
              <Text style={styles.section}>VGA GAIN</Text>
              <View style={styles.sliderRow}>
                <Text style={styles.sliderEnd}>0</Text>
                <Slider style={{ flex: 1, height: 40 }}
                  minimumValue={0} maximumValue={Math.max(2, hrfVgaMax)} step={2}
                  value={Math.min(hrfVgaNow, hrfVgaMax)}
                  onValueChange={(v) => p.onHrfVga?.(Math.min(hrfVgaMax, Math.round(v)))}
                  minimumTrackTintColor={C.abtn} maximumTrackTintColor="#444" thumbTintColor={C.gold} />
                <Text style={styles.sliderEnd}>{hrfVgaMax}</Text>
              </View>
              <Text style={styles.stepVal}>{Math.min(hrfVgaNow, hrfVgaMax)} dB</Text>
              <Text style={styles.note}>
                The baseband stage, after the mixer — it lifts noise along with signal. Use it to
                top up once the LNA is set, not as the main control.
              </Text>
              {/* ★ SAY WHY IT STOPS THERE. A slider that simply will not go further is
                  indistinguishable from a broken one, and the two stages moving each other's
                  ceilings is genuinely surprising until you know it is one shared budget. */}
              {capDb >= 0 && (
                <Text style={styles.note}>
                  This receiver's owner has limited the total gain to {capDb} dB, so the two
                  stages share one budget — raising one lowers what the other can reach. You are
                  using {Math.min(hrfLnaNow, hrfLnaMax) + Math.min(hrfVgaNow, hrfVgaMax)} dB of it.
                </Text>
              )}

              {/* ★ Bias-T, owner-only for the reason it is everywhere else: feeding voltage into
                  somebody else's feedline is not a listener's decision. */}
              <View style={styles.toggleRow}>
                <Text style={styles.toggleLabel}>Bias-T (3.3 V){locked ? ' — owner only' : ''}</Text>
                <Switch value={!!p.hrfBiasT} onValueChange={(v) => p.onHrfBiasT?.(v)}
                  disabled={locked}
                  trackColor={{ true: C.abtn, false: '#444' }} thumbColor={p.hrfBiasT ? C.gold : '#ccc'} />
              </View>
              <Text style={styles.note}>
                Puts 3.3 V at 50 mA on the antenna port to power a mast-head amplifier. Off unless
                you know what is on the end of the coax.
              </Text>
            </>
          ) : (
            <>
              <Text style={styles.section}>GAIN</Text>
              {/* ★ Over RTL-TCP "auto" is the dongle's own broken AGC — see GainSlider.vibeAgc. */}
              <GainSlider gains={cappedGains} gainTenthDb={p.gainTenthDb} auto={p.autoGain}
                          onAuto={p.onAuto} onGain={p.onGain} vibeAgc={!p.isTcp} />
            </>
          ))}
          {p.isSpy && <Text style={styles.note}>
            The SpyServer protocol sends a gain step, not a dB value — the labels are
            this receiver's nearest published gains. There is no auto-gain over the wire.
          </Text>}

          {/* ★★★ NO SAMPLE-RATE CONTROL ON AN AIRSPY HF+. Its rate is fixed when the radio opens,
              because changing it on a LIVE stream is a path no other SDR client takes — SDR++
              (mainline and Brown) grey the control out while running, gr-osmosdr sets it once at
              construction, OpenWebRX is profile-based and never changes it on the fly. Ours could,
              and it mis-tuned the receiver, put audio a full span off with an image beside it, and
              once wedged the USB endpoint hard enough that the host needed a reboot (2026-08-02).
              ★ Stuart: "we cannot have users wedge a device that shouldn't really have its sample
              rate changed on the fly." The owner still picks the rate — in the server's config,
              applied at startup, which is the stop-and-start every other client already requires.
              ★★ Hidden rather than disabled: a greyed control still reads as an offer. */}
          {!p.isSpy && p.radio?.driver !== 'airspyhf' && <>
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
          {/* ★★★ ASK THE RADIO WHAT IT SUPPORTS. The >=1 MHz filter below is an RTL RULE —
              a real RTL dongle runs sluggish and underfiltered under ~1 MHz — and it was applied to
              ALL local USB hardware. The Airspy HF+ MAXES OUT AT 912 kHz, so every option in the
              list failed that test, every choice collapsed to the same rate, and the picker looked
              broken (Stuart: "Airspy sample rate does nothing on Android").
              ★★ This is the one-radio assumption family again: a rule true of the first radio,
              applied to a radio it was never about. hwinfo has carried `rates` all along.
              ★ Priority: the server's pin > the server's list > THE RADIO'S OWN LIST > the RTL
              default. The last branch keeps the >=1 MHz filter, because there it IS an RTL. */}
          <Seg slot={slot} options={p.serverRates && p.serverRates.length
                          ? [...p.serverRates].sort((a, b) => a - b)
                          : p.radio?.rates && p.radio.rates.length
                          ? [...p.radio.rates].sort((a, b) => a - b)
                          : p.isTcp ? SAMPLE_RATES : SAMPLE_RATES.filter(r => r >= 1_000_000)}
               value={p.sampleRate} onChange={p.onSampleRate}
               fmt={(r) => `${(r / 1e6).toFixed(r % 1e6 === 0 ? 1 : 3).replace(/0+$/, '').replace(/\.$/, '.0')}M`} />
          </>}
          </>}
          {/* ★★★ THE TUNER'S IF FILTER — beside the sample rate, because they are the same
              decision twice over: setting a rate re-derives this filter in librtlsdr and the
              server re-asserts ours afterwards. The web client has had this picker for as long as
              the filter has existed; the app had NOTHING, so the one control that keeps a strong
              neighbour out of the front end was reachable only from a browser.
              ★★ AUTO IS THE ONE WORTH HAVING: it narrows as the listener zooms in and widens again
                 when they zoom out, so the selectivity follows what they are actually looking at
                 and nobody has to know what an IF filter is. It is also the only setting that
                 cannot be got wrong by leaving it alone.
              ★ Drawn only when the server has actually told us it has one — see hasTunerBw. */}
          {p.hasTunerBw && !p.isSpy && isRtl && p.onTunerBw && <>
          <Text style={styles.section}>IF FILTER</Text>
          <Seg slot={slot} options={TUNER_BWS}
               value={p.tunerBwAuto ? -1 : (p.tunerBw ?? 0)}
               onChange={(v) => p.onTunerBw?.(v)}
               fmt={(v) => v < 0 ? 'Auto' : v === 0 ? 'Wide' : `${Math.round(v / 1000)}k`} />
          <Text style={styles.note}>
            {p.tunerBwAuto
              ? 'Following the zoom — narrower as you zoom in, wide again as you zoom out.'
              : 'Auto follows the zoom, which keeps strong neighbours out of the front end '
                + 'without you having to think about it.'}
          </Text>
          </>}
          {p.isSpy && <Text style={styles.note}>
            Sample rate is chosen automatically from the mode: the server decimates
            before sending, which is what keeps a SpyServer usable over a hotspot or
            mobile data.
          </Text>}

          {/* ★★ FM DE-EMPHASIS AND FM STEREO HAVE MOVED TO THE AUDIO SHEET (the speaker button),
              2026-08-02. They were never properties of the RADIO — they act on our own
              demodulator, which is exactly why the SpyServer note above had to carve out an
              exception for them while the rest of this panel did not apply. The web client has
              always grouped them with volume, NR and the auto-notch; the app is now the same.
              ★ If you are looking for them here, that is the point: this panel is the hardware. */}

          {/* ── CONVERTER ────────────────────────────────────────────────────────────────
              ★★★ THE HARDWARE PANEL IS WHERE THIS BELONGS, because that is what it is: a box
                bolted between the aerial and the tuner, sitting with the sample rate, the IF
                filter and the frequency correction rather than with the app's own display
                preferences. It began life in the settings menu and moved here.
              ★★★ AND IT SITS OUTSIDE THE DONGLE GATE BELOW — deliberately, and this is the trap
                in moving it. PPM, bias-T, digital AGC and direct sampling are all properties of
                an RTL dongle and are hidden for anything else; a converter is a property of the
                AERIAL, so an Airspy, an RSP or a HackRF can sit behind one exactly as a dongle
                can. Dropping this inside `isRtl` would have made it vanish on three of the four
                radios we support — the "a control that only works on one radio" fault from
                AGENTS.md, reached from the other side.
              ★★ NOR IS IT GATED ON isSpy. The note below says frequency correction and bias-T
                "belong to whoever runs this receiver", and that is right for those — but a
                SpyServer carries no notion of an LO in its protocol at all, so a listener with a
                converter on their own aerial has NOWHERE ELSE to correct it. It is ours.
              ★ The VibeServer case is gated by the PARENT, which simply passes no `converter` —
                there the owner sets one on the radio's setup page and the server publishes true
                RF, so correcting again here would double it. One gate, in one place. */}
          {p.converter && p.onConverter && (() => {
            const conv = p.converter, setConv = p.onConverter;
            const loMhz = conv.offsetHz ? Math.abs(conv.offsetHz) / 1e6 : 0;
            return (<>
              <Text style={styles.section}>CONVERTER</Text>
              <Seg slot={slot}
                   options={CONVERTER_PRESETS.map(c => c.id)}
                   value={conv.id}
                   onChange={(id) => {
                     const pr = presetById(id);
                     if (!pr) return;
                     setConv(id === 'custom'
                       ? { ...conv, id: 'custom', label: 'Custom' }
                       : fromPreset(pr, conv));
                   }}
                   fmt={(id) => presetById(id)?.label ?? id} />
              <Text style={styles.note}>{presetById(conv.id)?.hint ?? 'Custom converter'}</Text>

              {conv.id === 'custom' && (<>
                {/* ★★ A POSITIVE LO AND A DIRECTION, never a signed number. It is STORED signed —
                    an up-converter is a down-converter with a negative LO, which is what makes
                    this one transform instead of two features — but the box in the user's hands
                    is labelled 125 MHz, and asking anyone to type a minus sign is a sign error
                    waiting to happen. */}
                {/* ★★ ONE nudge() FOR BOTH BUTTONS AND BOTH INPUT PATHS. Written out twice, the
                    minus button's KEYBOARD action was a copy of the plus button's and stepped the
                    wrong way — a fault invisible to touch and only reachable with a keyboard, so
                    it would have shipped. The step is in MAGNITUDE (the LO the user typed), and
                    the sign stays wherever the direction control put it. */}
                {(() => {
                  const nudge = (deltaMhz: number) => () => setConv({
                    ...conv, label: 'Custom',
                    offsetHz: Math.sign(conv.offsetHz || -1)
                              * Math.max(0, Math.abs(conv.offsetHz) + deltaMhz * 1e6),
                  });
                  const down = nudge(-1), up = nudge(1);
                  return (
                    <View style={styles.stepperRow}>
                      <TouchableOpacity
                        style={[styles.stepBtn, slot(down) && kbNav
                                && { borderColor: NAV_FOCUS, borderWidth: 2 }]}
                        onPress={down}><Text style={styles.stepBtnTxt}>−</Text></TouchableOpacity>
                      <Text style={styles.stepVal}>{loMhz} MHz</Text>
                      <TouchableOpacity
                        style={[styles.stepBtn, slot(up) && kbNav
                                && { borderColor: NAV_FOCUS, borderWidth: 2 }]}
                        onPress={up}><Text style={styles.stepBtnTxt}>+</Text></TouchableOpacity>
                    </View>
                  );
                })()}
                <Seg slot={slot}
                     options={['up', 'down']}
                     value={conv.offsetHz > 0 ? 'down' : 'up'}
                     onChange={(d) => setConv({ ...conv, label: 'Custom',
                       offsetHz: (d === 'down' ? 1 : -1) * Math.abs(conv.offsetHz),
                       // ★★ The direction also sets what the converter PASSES, because that is
                       //   the half that decides how far the dial may go. An up-converter takes
                       //   HF in and filters the rest out, so the app's HF ceiling is the honest
                       //   default; a down-converter's input is its own high band and we cannot
                       //   guess it, so it declares none and the range is merely shifted.
                       inputLoHz: 0, inputHiHz: d === 'down' ? 0 : HF_CEILING_HZ })}
                     fmt={(d) => d === 'down' ? 'Converts down' : 'Converts up'} />
                <Text style={styles.note}>{V4_WARNING}</Text>
              </>)}

              {/* ★★★ CONFIGURATION ABOVE, STATE HERE, and they are kept apart on purpose. The
                  preset is set once per radio; this is flipped whenever the lead moves — an
                  up-converter is inline, and you unplug it to get VHF back. A switch, not a
                  button, because it mirrors something physical that is either true or not. */}
              {!isIdentity(conv) && (<>
                <View style={styles.toggleRow}>
                  <Text style={styles.toggleLabel}>Converter fitted</Text>
                  <Switch value={conv.engaged}
                          onValueChange={(v) => setConv({ ...conv, engaged: v })}
                          trackColor={{ true: C.abtn, false: '#444' }}
                          thumbColor={conv.engaged ? C.gold : '#ccc'} />
                </View>
                <Text style={styles.note}>
                  {conv.engaged
                    ? `Tuning is offset by ${loMhz} MHz, and the dial is limited to what the `
                      + 'converter passes. Turn this off when you unplug it.'
                    : 'Off — the app tunes the radio directly. Turn this on when the converter '
                      + 'is plugged in.'}
                </Text>
              </>)}
            </>);
          })()}

          {/* ★ DONGLE-ONLY from here: PPM (the HF+ calibrates in parts per BILLION, and that
              control is admin-gated), bias-T, the RTL2832's digital AGC and direct sampling
              are all properties of an RTL dongle. Showing them for another radio is how the
              panel became a hybrid of two receivers. */}
          {!p.isSpy && isRtl && canProtected && <>
          <Text style={styles.section}>FREQUENCY CORRECTION (PPM)</Text>
          <View style={styles.stepperRow}>
            <TouchableOpacity style={[styles.stepBtn, slot(() => p.onPpm(p.ppm - 1)) && kbNav && { borderColor: NAV_FOCUS, borderWidth: 2 }]} onPress={() => p.onPpm(p.ppm - 1)}><Text style={styles.stepBtnTxt}>−</Text></TouchableOpacity>
            <Text style={styles.stepVal}>{p.ppm > 0 ? `+${p.ppm}` : p.ppm} ppm</Text>
            <TouchableOpacity style={[styles.stepBtn, slot(() => p.onPpm(p.ppm + 1)) && kbNav && { borderColor: NAV_FOCUS, borderWidth: 2 }]} onPress={() => p.onPpm(p.ppm + 1)}><Text style={styles.stepBtnTxt}>+</Text></TouchableOpacity>
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
          <Seg slot={slot} options={DS_MODES.map(d => d.value)} value={p.directSampling} onChange={p.onDirectSampling}
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
  adminCard: { borderWidth: 1, borderColor: 'rgba(255,140,60,0.55)', borderRadius: 8,
               padding: 12, marginBottom: 14, backgroundColor: 'rgba(255,140,60,0.08)' },
  adminCardOk: { borderColor: 'rgba(120,220,140,0.5)', backgroundColor: 'rgba(120,220,140,0.08)' },
  adminTitle: { color: C.gold, fontSize: 11, letterSpacing: 2, marginBottom: 6 },
  adminRow:  { flexDirection: 'row', alignItems: 'center', gap: 8, marginTop: 10 },
  adminInput: { flex: 1, borderWidth: 1, borderColor: C.border, borderRadius: 6,
                paddingHorizontal: 10, paddingVertical: 8, color: C.gold, fontSize: 14 },
  adminBtn:  { borderWidth: 1, borderColor: C.abtn, borderRadius: 6,
               paddingHorizontal: 14, paddingVertical: 9 },
  adminBtnTxt: { color: C.gold, fontSize: 12, letterSpacing: 1 },
  sliderRow: { flexDirection: 'row', alignItems: 'center', gap: 10 },
  sliderEnd: { color: C.dim, fontSize: 12, minWidth: 26, textAlign: 'center' },
  stepVal: { fontSize: 15, color: C.muted, minWidth: 80, textAlign: 'center' },
  toggleRow: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', marginTop: 16 },
  toggleLabel: { fontSize: 14, color: C.muted },
  note: { fontSize: 11, color: C.dim, marginTop: 6, fontStyle: 'italic' },
});
