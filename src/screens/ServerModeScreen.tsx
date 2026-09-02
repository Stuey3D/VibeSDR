import React, { useCallback, useEffect, useRef, useState } from 'react';
import GainSlider from '../components/GainSlider';
import BandLimitEditor, { Band } from '../components/BandLimitEditor';
import {
  View, Text, TextInput, TouchableOpacity, ScrollView, ActivityIndicator,
  StyleSheet, Platform, PermissionsAndroid, Switch, Alert, NativeModules,
} from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';
import AsyncStorage from '@react-native-async-storage/async-storage';
import type { NativeStackScreenProps } from '@react-navigation/native-stack';
import { RootStackParamList } from '../../App';
import { themeFor } from '../constants/theme';
import { getServerName, saveServerName, PUBLIC_NAME_KEY } from '../services/rtlTcpServer';
import {
  startVibeServer, stopVibeServer, getVibeServerStatus, setVibeServerCompressAudio, getConnectedRadio,
  setVibeServerAdminSecret, setVibeServerUncompressedAudio, setVibeServerSessionLimit,
  setVibeServerBiasT,
  vibeServerSupported, randomPin, fmtRate, FPS_TIERS, fpsForTier,
  getServerLocationMode, setServerLocationMode, getManualServerLocation,
  getResolvedServerLocation,
  setManualServerLocation, resolveLocation, publishLocation,
  type FpsTier, type VibeServerInfo, type VibeServerStatus, type LocationMode,
} from '../services/vibeServer';
import { loadActiveEibi } from '../services/eibi';
import { advertiseServer, stopAdvertiseRtlTcp } from '../services/mdns';

type Props = NativeStackScreenProps<RootStackParamList, 'ServerMode'>;

// Server-mode picker + VibeServer control screen.
//   • VibeServer (default) — server-side DSP, compressed audio + waterfall,
//     ~25x lighter than raw IQ, HMAC PIN. Handled inline here.
//   • RTL-TCP — raw IQ, maximum compatibility. Delegates to RtlTcpServerScreen.
// The auto-discovery (mDNS) toggle and the LOCAL name are shared by both.

type Proto = 'vibeserver' | 'rtltcp';
type PinMode = 'random' | 'custom' | 'off';

// 0 = CLIENT-CONTROLLED: the client picks the span live (the same convention as the
// RTL-TCP server's overrideRate). Anything else PINS the rate — the client's picker
// is then hidden and told the server set it, because a rate it can't change is a
// rate it shouldn't offer.
// ★★ PER RADIO. These were a dongle's rates offered to whatever was plugged in, so an Airspy
// HF+ — which tops out near 912 kHz — was shown 2.4 MHz, 1.2 MHz and 960 kHz: every option
// impossible (Stuart, 2026-07-27). The shim would snap a pinned rate to the nearest real one,
// so the menu was lying rather than breaking, which is worse.
// ★ FIFTH time today the same shape has bitten: a list written when there was one radio.
const RATE_OPTIONS_RTL = [
  { label: 'Client-controlled', value: 0 },
  { label: 'Full · 2.4 MHz',  value: 2_400_000 },
  { label: '1.2 MHz',         value: 1_200_000 },
  { label: '960 kHz (light)', value: 960_000 },
];
// ★★★ HackRF One — EXPERIMENTAL, and the ONLY radio here whose FLOOR is above the dongle
// menu's CEILING. A HackRF cannot go below 2 MSPS, so the RTL list (2.4 / 1.2 / 960 kHz) would
// have offered it two impossible options out of three and the shim would have snapped them
// silently upward — the 2026-07-27 Airspy fault exactly, in reverse and quieter, because the
// TOP entry happens to be legal so the menu looks like it works.
// ★★ These mirror kRates[] in cpp/hackrf_source.cpp. That list is authoritative; this one only
// decides which menu to draw. 10 MS/s is offered but flagged — it is far more than the phone's
// DSP can carry, and the server-side comment says the ceiling is what the DSP can plausibly
// process, not what the radio can emit.
const RATE_OPTIONS_HRF = [
  { label: 'Client-controlled', value: 0 },
  { label: '2 MS/s (lightest)', value: 2_000_000 },
  { label: '2.4 MS/s',          value: 2_400_000 },
  { label: '4 MS/s',            value: 4_000_000 },
  { label: '5 MS/s',            value: 5_000_000 },
  { label: '8 MS/s (heavy)',    value: 8_000_000 },
];
// Airspy HF+ Discovery / Dual Port. ★ The radio's own list still wins once it is running —
// this is which menu to draw, not a claim about the hardware.
const RATE_OPTIONS_AHF = [
  { label: 'Client-controlled', value: 0 },
  { label: 'Full · 912 kHz',  value: 912_000 },
  { label: '768 kHz',         value: 768_000 },
  { label: '384 kHz (light)', value: 384_000 },
];

/** ★★ THE R820T/R828D's 29 TUNER GAINS, tenths of a dB. Fixed in the tuner, identical across
 *  every RTL dongle we support, and the reason the control is a slider: there is nothing between
 *  these values, so a typed number is a value the radio will quietly move.
 *  ★ 157 (15.7 dB) is in here and is the figure Stuart measured as the sweet spot on broadcast FM
 *    — a useful default for the AUTO toggle to fall back to rather than an invented number. */
const RTL_GAINS = [0, 9, 14, 27, 37, 77, 87, 125, 144, 157, 166, 197, 207, 229, 254,
                   280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496];

/** ★★★ THE SAME ELEVEN KEYS THE WEB CLIENT AND THE SETUP PAGE DRAW. The phone shows NAMES rather
 *  than the line art — a small glyph in a wrapped row is harder to read than a word, and the
 *  drawing is what a listener sees on the landing screen anyway.
 *  ★★★ THE KEYS ARE THE CONTRACT: they are what the config stores and what every other client
 *      looks up, so a key renamed here silently unsets an owner's choice on a radio they set up
 *      months ago. Redraw freely; rename never. See ANT_ICONS in web/client/src/main.ts. */
const ANT_ICONS = [
  { key: 'vertical',    label: 'Vertical' },
  { key: 'groundplane', label: 'Ground plane' },
  { key: 'whip',        label: 'Whip' },
  { key: 'discone',     label: 'Discone' },
  { key: 'dipole',      label: 'Dipole' },
  { key: 'longwire',    label: 'Long wire' },
  { key: 'loop',        label: 'Loop' },
  { key: 'deltaloop',   label: 'Delta loop' },
  { key: 'qfh',         label: 'QFH' },
  { key: 'yagi',        label: 'Yagi' },
  { key: 'dish',        label: 'Dish' },
];

const K = {
  proto: 'vs_proto', advertise: 'vs_advertise', pinMode: 'vs_pinmode',
  pin: 'vs_pin', rate: 'vs_rate', fps: 'vs_fps', compress: 'vs_compress',
  webServer: 'vs_webserver',
  landingMsg: 'vs_landingmsg', landingUrl: 'vs_landingurl', landingLbl: 'vs_landinglbl',
  idleKick: 'vs_idlekick', limitSoft: 'vs_limitsoft', idleSaver: 'vs_idlesaver',
  lockedCentre: 'vs_lockedcentre', zoomSpectrum: 'vs_zoomspec', spectrogram: 'vs_spectrogram',
  idleGrace: 'vs_idlegrace', antenna: 'vs_antenna', antennaIcon: 'vs_antennaicon',
  adminPw: 'vs_adminpw', uncomp: 'vs_uncompressed', limitMin: 'vs_sessionlimit',
  advanced: 'vs_advanced', maxUsers: 'vs_maxusers',
  allowRanges: 'vs_allow', blockRanges: 'vs_block',
  gainLimits: 'vs_gainlimits', restGain: 'vs_restgain', agcLock: 'vs_agclock',
  gainLocks: 'vs_gainlocks', gainSplits: 'vs_gainsplits',
  rtlAgc: 'vs_rtlagc', tunerBwAuto: 'vs_tunerbwauto', publicName: PUBLIC_NAME_KEY,
  proxies: 'vs_proxies', radioUse: 'vs_radiouse', oneRadioPerIp: 'vs_oneradioperip',
  landingHz: 'vs_landinghz', landingMode: 'vs_landingmode', biasT: 'vs_biast',
};

export default function ServerModeScreen({ navigation, route }: Props) {
  const { colors: C, font: F } = themeFor();

  const [proto, setProto]         = useState<Proto>('vibeserver');
  const [name, setName]           = useState(route.params?.name ?? 'VibeSDR');
  const [advertise, setAdvertise] = useState(true);
  const [pinMode, setPinMode]     = useState<PinMode>('random');
  const [pin, setPin]             = useState(() => randomPin(Date.now()));
  const [rate, setRate]           = useState(0);          // 0 = client-controlled
  const [fps, setFps]             = useState<FpsTier>('full');
  const [compress, setCompress]   = useState(true);
  // ★ Admin password — protects the CONTROLS (bias-T, direct sampling, calibration), not
  // access. Deliberately separate from the listening PIN: Hans's server is open to everyone
  // and still must not let a visitor put DC on the feedline.
  const [adminPw, setAdminPw]     = useState('');
  // ★★★ ADVANCED MODE. NOT "Full" — the Mac and the Pi serve SEVERAL radios behind a front door,
  // and a phone serves ONE (it cannot power three over OTG), so "Full" would promise a parity it
  // cannot deliver (Stuart, 2026-08-12). It adds no process and no second port: everything it
  // offers is applied to the radio this app is already running.
  const [advanced, setAdvanced]   = useState(false);
  const [landingMsg, setLandingMsg] = useState('');
  const [landingUrl, setLandingUrl] = useState('');
  const [landingLbl, setLandingLbl] = useState('');
  const [eibiBusy, setEibiBusy]     = useState(false);
  const [eibiMsg, setEibiMsg]       = useState('');
  /** The time limit as a GUARANTEE rather than a deadline, and the optional idle release. */
  const [limitSoft, setLimitSoft]   = useState(false);
  const [idleKick, setIdleKick]     = useState(0);
  /** Machine-wide spectrum slowdown when nobody is looking — lives with the frame rate. */
  const [idleSaver, setIdleSaver]   = useState(false);
  /** ★ Locked mode only: the captured window everyone shares, and real bins at deep zoom. */
  const [lockedCentre, setLockedCentre] = useState(0);
  // ★ ON by default, as the browser's setup page has it: a shared receiver that goes blocky the
  //   moment somebody zooms reads as a poor receiver rather than as a setting nobody switched on.
  //   The cost is a little CPU, which is the trade the label now states.
  const [zoomSpec, setZoomSpec]     = useState(true);
  const [spectrogram, setSpectrogram] = useState(false);
  /** Power down the radio when nobody is listening. ON by default; it never releases the dongle. */
  const [idleGrace, setIdleGrace]   = useState(300);
  const [antenna, setAntenna]       = useState('');
  const [antennaIcon, setAntennaIcon] = useState('');
  /**
   * ★★★ HOW WILL THIS RADIO BE USED — and it is asked FIRST in Advanced, because it decides which
   *     of the questions below even apply. A shared radio has a locked range and a listener count;
   *     a single-user one has neither and gets the allow/block lists instead. Asking it last means
   *     answering questions that the next answer makes irrelevant — the web setup page learnt this
   *     and says so in its own comment; this is the same choice, worded the same way.
   * ★★ 'single' = one user at a time, the whole radio and every control, as if it were plugged
   *    into their machine. 'locked' = you choose the window, listeners tune inside it and cannot
   *    move the radio for everybody.
   * ★ Simple mode IS 'single' without the restrictions, so this only appears under Advanced.
   */
  const [radioUse, setRadioUse]   = useState<'single' | 'locked'>('single');
  /** ★ True = refuse a second radio to one address. Default true, as the server's own default. */
  const [oneRadioPerIp, setOneRadioPerIp] = useState(true);
  // ★ Listeners sharing the radio. 1 = single occupant, which is Simple mode's behaviour.
  const [maxUsers, setMaxUsers]   = useState(1);
  /** ★ The listener box's raw text — see the note where it is drawn. */
  const [usersText, setUsersText] = useState('1');
  /**
   * ★★★ THE BANDS THE SERVER ITSELF KNOWS, not a copy. vibe_bands.h is what resolves "fm" or
   *     "airband" when a limit is applied, so a list written again in TypeScript would drift from
   *     the one being enforced — and the copy that drifts is the one that quietly stops matching.
   *  ★ Empty until it arrives, and the editors simply show no band chips until then: an owner can
   *    still type a range, which is the half that never needed the list.
   */
  // ── ADVERTISE ON VIBESDR.NET ───────────────────────────────────────────────────────────────
  /** ★ Its own field rather than reusing the LOCAL NAME: what a phone answers to on the LAN
   *  ("Living room Pi") is not necessarily what its owner wants published to the world. It is
   *  PREFILLED from the local name, so for most people it is still one thing to check. */
  const [publicName, setPublicName]   = useState('');
  const [publicOn, setPublicOn]       = useState(false);
  const [publicBusy, setPublicBusy]   = useState(false);
  const [publicAddr, setPublicAddr]   = useState('');
  const [publicErr, setPublicErr]     = useState('');
  /** ★★ arm64 only. Where the tunnel is not in the build the switch is ABSENT, never inert —
   *  AGENTS.md: a control that works in one scenario and not another should be removed. */
  const [publicSupported, setPublicSupported] = useState(false);

  /** ★★ PERMANENT UNLESS THE OWNER SAYS OTHERWISE — the safe default and the common case. A club
   *  server up for the week of a contest is the exception, and an exception should be chosen.
   *  ★★★ AND IT ONLY ENDS THE LISTING. When the offer runs out the receiver carries on exactly as
   *      an unlisted VibeServer does — it is removed from the directory, not switched off
   *      (Stuart, 2026-08-22: "it simply reverts to an unlisted but still active vibeserver"). */
  const [publicTemp, setPublicTemp]   = useState(false);
  const [publicForN, setPublicForN]   = useState('1');
  const [publicForU, setPublicForU]   = useState<'minutes'|'hours'|'days'|'weeks'|'months'>('days');

  /** ★ A month is 30 days and a week is 7 — this is an OFFER, not a calendar appointment, and
   *  nobody setting "2 months" means it to the hour. */
  const shareSeconds = () => {
    const n = Math.max(1, Math.floor(Number(publicForN) || 1));
    const mult = { minutes: 60, hours: 3600, days: 86400, weeks: 604800, months: 2592000 }[publicForU];
    return n * mult;
  };

  const [bands, setBands] = useState<Band[]>([]);
  const [allowRanges, setAllowRanges] = useState('');
  const [blockRanges, setBlockRanges] = useState('');
  const [gainLimits, setGainLimits]   = useState('');
  /** ★★ WHICH BANDS ARE FIXED at their ceiling rather than limited by it, and — on a HackRF — where
   *  between LNA and VGA a fixed band's total sits. Per band, both of them: an owner can hold FM at
   *  one figure and leave HF adjustable under a ceiling (Stuart, 2026-08-28). */
  const [gainLocks, setGainLocks]     = useState('');
  const [gainSplits, setGainSplits]   = useState('');
  const [restGain, setRestGain]       = useState(-1);
  /** ★★★ ONE SLIDER, TWO MEANINGS — see the note by the toggles. Protection is ON by default (it
   *  can only ever prevent clipping); the AGC is OFF, because it may raise the gain above what the
   *  owner set and that has to be asked for. */
  const [rtlAgc, setRtlAgc]           = useState(false);
  const [tunerBwAuto, setTunerBwAuto] = useState(false);
  /** ★★ WHERE A LISTENER LANDS. The macOS settings window calls these "Listener's starting
   *  frequency / mode"; same setting, same words, so an owner who runs both meets one idea once.
   *  ★ 0 = leave it to the server's own default rather than assert a frequency nobody chose. */
  const [landingHz, setLandingHz]     = useState(0);
  const [landingMode, setLandingMode] = useState('wfm');
  /** ★★ BIAS-T LIVES HERE, not only in the client's hardware panel. On a phone acting as the
   *  server there may be no client attached at all — and a powered loop needs its DC before the
   *  first listener arrives, not after one turns up and unlocks the hardware (Stuart, 2026-08-17:
   *  "as soon as you press go live you are in play").
   *  ★★★ SHOWN ONLY WHERE THE RADIO HAS ONE. The Airspy HF+ has no bias-T; drawing a dead switch
   *  teaches the owner the FEATURE is broken rather than absent — the rule in AGENTS.md. */
  const [biasT, setBiasT]             = useState(false);
  const [agcLock, setAgcLock]         = useState(false);
  const [proxies, setProxies]         = useState('');
  const [showAdminPw, setShowAdminPw] = useState(false);
  // 0 = off, 1 = listener's choice, 2 = compatibility only.
  const [uncomp, setUncomp]       = useState<0 | 1 | 2>(0);
  // ★ Per-listener time limit, minutes. 0 = unlimited — right for a private receiver.
  const [limitMin, setLimitMin]   = useState(0);
  // ★ Which radio is attached, so the menus match it. VID/PID only — see getConnectedRadio.
  const [radio, setRadio] = useState<{ driver: string; model: string } | null>(null);
  useEffect(() => { void getConnectedRadio().then(setRadio); }, []);
  // ★ THREE RADIOS, THREE MENUS — not a two-way choice with a default any more.
  const rateOptionsAll = radio?.driver === 'airspyhf' ? RATE_OPTIONS_AHF
                       : radio?.driver === 'hackrf'   ? RATE_OPTIONS_HRF
                       : RATE_OPTIONS_RTL;
  // ★★★ "CLIENT-CONTROLLED" CANNOT EXIST IN A LOCKED RANGE. The whole point of a pinned window is
  //     that the SERVER decides what is captured and everybody pans inside it — a listener who
  //     could re-rate the radio would move the window out from under every other listener. It was
  //     offered anyway, so a locked receiver had a control that either did nothing or broke the
  //     mode (Stuart, 2026-08-21: "the sample rate cannot be client controlled in this mode so
  //     that option needs removing").

  const rateOptions = radioUse === 'locked'
    ? rateOptionsAll.filter(o => o.value !== 0)
    : rateOptionsAll;
  // ★ And a receiver ALREADY set to client-controlled has to be moved off it when the mode
  //   changes, or the option is merely hidden while still in force — which is worse than showing
  //   it, because nothing on screen would say what the radio is actually doing.
  useEffect(() => {
    if (radioUse === 'locked' && rate === 0) {
      const first = rateOptionsAll.find(o => o.value !== 0);
      if (first) { setRate(first.value); AsyncStorage.setItem(K.rate, String(first.value)); }
    }
  }, [radioUse, rate, rateOptionsAll]);

  /**
   * ★★★ THE HF+ HAS NOTHING TO SET UP, so do not draw it a panel of dead controls. Android has no
   *     SDRplay support, which leaves two radios and a very short answer: the Airspy HF+ has no
   *     choosable sample rate (it is pinned to the one rate that tunes correctly — see
   *     nearestRate()), no bias-T, and its AGC is locked on. Everything hardware in here is
   *     therefore RTL-SDR's (Stuart, 2026-08-17: "the radio settings in simple mode is literally
   *     just RTL-SDR stuff").
   * ★★ This is the rule AGENTS.md states outright: branch on the driver and draw the right control,
   *    or remove it. A switch that does nothing teaches the owner the FEATURE is broken, not that
   *    their radio hasn't got one.
   * ★ Where a listener LANDS is not hardware and applies to both — it stays on show.
   */
  const isAhf = radio?.driver === 'airspyhf';
  const hasHwSetup = !isAhf;
  /* ★★★ THE GAIN + AGC + IF-FILTER BLOCK IS THE DONGLE'S, AND ONLY THE DONGLE'S.
   *   Every control in it is an R820T fact: a 29-step gain TABLE (RTL_GAINS, hardcoded),
   *   VibeAGC — which does not exist for any other radio — its lock, and the IF filter, whose
   *   own comment already says "RTL only: the R820T has a real IF filter and the other radios
   *   have nothing like it". None of it was gated on the driver, so an Airspy HF+ and a HackRF
   *   both got the lot: a gain table their radio does not have, and an AGC switch that writes a
   *   setting nothing will ever read.
   * ★★ THIS IS THE RULE FROM AGENTS.md, which is in the repo because of exactly this shape:
   *   "I would rather a control be removed if it only works in one scenario than keep it there
   *   dead." A switch that is drawn, accepted and inert teaches the owner the FEATURE is broken.
   * ★ `!radio` COUNTS AS RTL — with nothing plugged in the screen still has to draw something,
   *   and the dongle is the overwhelmingly common case. Same convention as LocalHardwarePanel. */
  const isRtl = !radio || radio.driver === 'rtl';
  // The rate ceiling to quote in prose, so the hint cannot drift from the list above it.
  const topRateLabel = radio?.driver === 'airspyhf' ? '912 kHz'
                     : radio?.driver === 'hackrf'   ? '8 MS/s'
                     : '2.4 MHz';

  // ★★★ THE ORDER AN OWNER SETS A LOCKED RECEIVER IN, which is not the order the page grew in.
  //     A pinned window is defined by its CENTRE and its WIDTH together — the centre says where
  //     the window is, the sample rate says how wide, and neither means anything without the
  //     other. They were pages apart: the centre was the last thing in Advanced (Stuart found it
  //     "buried underneath everything") and the width sat below the landing frequency.
  //  ★★ So in Locked Range the five settings that define the receiver come first, in the order
  //     they depend on each other: centre, sample rate, starting frequency/mode, gain, Bias-T.
  //     Unlocked keeps the old order, where the width belongs with the hardware because no window
  //     is being pinned.
  //  ★ Held as fragments and RENDERED in one place or the other — never both. Two copies of one
  //    control is the fault this screen already had with the machine form.
  const centreBlock = (<>
                {/* ★★★ THE CAPTURED WINDOW — the setting that makes shared listening possible at
                    all. Everybody gets a slice of ONE window, so the centre must not move; on this
                    phone the centre has always followed whatever the landing frequency was, which
                    is right for a single listener retuning the radio themselves and meaningless
                    the moment several people share it.
                    ★ Blank/0 = follow the listener, i.e. the old behaviour. */}
                <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>CENTRE FREQUENCY</Text>
                <TextInput
                  value={lockedCentre ? String(Math.round(lockedCentre / 1000)) : ''}
                  onChangeText={(t) => setLockedCentre(Math.round((Number(t) || 0) * 1000))}
                  placeholder="kHz — the window everyone shares"
                  placeholderTextColor={C.textDim} keyboardType="numeric"
                  style={[styles.input, { color: C.amber, fontFamily: F, borderColor: C.border }]} />
                <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 6 }]}>
                  Listeners tune freely INSIDE this window but cannot move the radio for everybody.
                  The width is the capture rate above, so the window runs half that either side.
                </Text>

  </>);
  const rateBlock = (<>
            {/* ★★ THE RADIO'S CAPTURE WIDTH, so it sits with the radio rather than with the
                server. On an HF+ there is one usable rate and the list says so; the choice here
                is really the RTL's. */}
            <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>CAPTURE WIDTH</Text>
            <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginBottom: 8 }]}>
              {rate === 0
                ? `Listeners choose their own span, up to ${topRateLabel}.`
                : 'Pinned — listeners cannot change it. Lower it to save processing on a slow phone.'}
            </Text>
            {rateOptions.map(o => (
              <OptRow key={o.value} C={C} F={F} active={rate === o.value} label={o.label} onPress={() => setRate(o.value)} />
            ))}

  </>);

  const [webServer, setWebServer] = useState(true);
  const [locMode, setLocMode]     = useState<LocationMode>('off');
  const [locCity, setLocCity]     = useState('');

  const [running, setRunning] = useState<VibeServerInfo | null>(null);
  const [status, setStatus]   = useState<VibeServerStatus | null>(null);
  /** The mDNS hostname the responder actually TOOK — "vibesdr-moto-g35", or with a "-2"
   *  suffix if another phone on the LAN already had the name. Asked for after the server
   *  is up, because the name isn't settled until the responder has probed for a clash. */
  const [mdnsHost, setMdnsHost] = useState('');
  const [starting, setStarting] = useState(false);
  const [error, setError]     = useState<string | null>(null);
  const runningRef = useRef(false);

  // Load saved preferences + name.
  useEffect(() => {
    (async () => {
      const n = await getServerName(route.params?.name ?? 'VibeSDR');
      setName(n);
      try {
        /* ★★ POSITIONAL, so a name added here must land in the SAME place as its getItem below.
         *   glk/gsp sit immediately after gl because that is where their reads were inserted —
         *   put them anywhere else and every variable after them silently takes its neighbour's
         *   value, which type-checks perfectly and is wrong at run time. */
        const [p, a, pm, sp, r, fp, cp, ws, apw, unc, lim, fm,
               mu, alw, blk, gl, glk, gsp, rg, agl, ragc, px, ru, lhz, lmd, bt] = await Promise.all([
          AsyncStorage.getItem(K.proto), AsyncStorage.getItem(K.advertise),
          AsyncStorage.getItem(K.pinMode), AsyncStorage.getItem(K.pin),
          AsyncStorage.getItem(K.rate), AsyncStorage.getItem(K.fps),
          AsyncStorage.getItem(K.compress), AsyncStorage.getItem(K.webServer),
          AsyncStorage.getItem(K.adminPw), AsyncStorage.getItem(K.uncomp),
          AsyncStorage.getItem(K.limitMin), AsyncStorage.getItem(K.advanced),
          AsyncStorage.getItem(K.maxUsers), AsyncStorage.getItem(K.allowRanges),
          AsyncStorage.getItem(K.blockRanges), AsyncStorage.getItem(K.gainLimits),
          AsyncStorage.getItem(K.gainLocks), AsyncStorage.getItem(K.gainSplits),
          AsyncStorage.getItem(K.restGain), AsyncStorage.getItem(K.agcLock),
          AsyncStorage.getItem(K.rtlAgc),
          AsyncStorage.getItem(K.proxies), AsyncStorage.getItem(K.radioUse),
          AsyncStorage.getItem(K.landingHz), AsyncStorage.getItem(K.landingMode),
          AsyncStorage.getItem(K.biasT),
        ]);
        if (p === 'rtltcp' || p === 'vibeserver') setProto(p);
        if (a != null) setAdvertise(a !== '0');
        if (ws != null) setWebServer(ws !== '0');
        if (apw != null) setAdminPw(apw);
        if (unc === '1' || unc === '2') setUncomp(unc === '1' ? 1 : 2);
        // ★ Absent = AGC OFF. An existing server must not come back with the AGC quietly enabled
        //   by an upgrade — it may raise the gain above what the owner set.
        if (ragc != null) setRtlAgc(ragc === '1');
        if (glk != null) setGainLocks(glk);
        if (gsp != null) setGainSplits(gsp);
        if (lim != null) setLimitMin(Number(lim) || 0);
        if (fm != null) setAdvanced(fm === '1');
        // ★ Loaded separately from the tuple above: adding thirteen more entries to a positional
        //   destructure of twenty-three is how the wrong value ends up in the wrong setting.
        // ★ Asked for once, from the shim that enforces them. Failure is silent and harmless —
        //   see the note by `bands`.
        void (async () => {
          try {
            const raw = await (NativeModules as any).VibeLocalSDR?.getBands?.();
            if (raw) { const b = JSON.parse(raw); if (Array.isArray(b)) setBands(b); }
          } catch { /* the band chips simply do not appear */ }
        })();
        /* ★★★ LOADED SEPARATELY, NOT ADDED TO THE POSITIONAL TUPLE ABOVE — see the note there.
         *     Inserting one more getItem() into a destructure of twenty-odd shifts EVERY value
         *     after it into the wrong setting, silently. I did exactly that while adding this and
         *     caught it only on re-reading; the comment warning against it is three lines away. */
        void (async () => {
          const v = await AsyncStorage.getItem(K.tunerBwAuto);
          if (v != null) setTunerBwAuto(v === '1');
        })();
        void (async () => {
          const g = async (k: string) => (await AsyncStorage.getItem(k)) ?? '';
          setLandingMsg(await g(K.landingMsg));
          setLandingUrl(await g(K.landingUrl));
          setLandingLbl(await g(K.landingLbl));
          setLimitSoft((await g(K.limitSoft)) === '1');
          setIdleKick(Number(await g(K.idleKick)) || 0);
          setIdleSaver((await g(K.idleSaver)) === '1');
          setLockedCentre(Number(await g(K.lockedCentre)) || 0);
          // ★ Absent means "never chosen", which must read as the DEFAULT (on) and not as off —
          //   the same trap as any stored boolean whose default is true.
          { const v = await g(K.zoomSpectrum); if (v) setZoomSpec(v === '1'); }
          // ★ Absent = refuse, the server's own default — an older install must not read as "allow".
          { const v = await g(K.oneRadioPerIp); if (v) setOneRadioPerIp(v === '1'); }
          setSpectrogram((await g(K.spectrogram)) === '1');
          const ig = await g(K.idleGrace);
          // ★ Default ON at 300 s — an absent value means "never set", not "off".
          setIdleGrace(ig === '' ? 300 : (Number(ig) || 0));
          setAntenna(await g(K.antenna));
          setAntennaIcon(await g(K.antennaIcon));
        })();
        if (ru === 'locked' || ru === 'single') setRadioUse(ru);
        if (lhz != null && Number.isFinite(Number(lhz))) setLandingHz(Number(lhz));
        if (lmd) setLandingMode(lmd);
        if (bt != null) setBiasT(bt === '1');
        if (mu != null) {
          const n = Math.max(1, Number(mu) || 1);
          // ★ The box's text follows the stored value — otherwise a saved 10 came back showing "1".
          setMaxUsers(n); setUsersText(String(n));
        }
        if (alw != null) setAllowRanges(alw);
        if (blk != null) setBlockRanges(blk);
        if (gl != null) setGainLimits(gl);
        // ★ -1 is "leave it alone" and is a REAL value, so no `|| default` here.
        if (rg != null && Number.isFinite(Number(rg))) setRestGain(Number(rg));
        if (agl != null) setAgcLock(agl === '1');
        if (px != null) setProxies(px);
        setLocMode(await getServerLocationMode());
        setLocCity((await getManualServerLocation())?.label ?? '');
        if (pm === 'random' || pm === 'custom' || pm === 'off') setPinMode(pm);
        // Restore the saved PIN for BOTH modes so re-opening the server keeps the
        // same code — it only changes when the user taps refresh (↻) or edits it.
        if (sp) setPin(sp);
        else AsyncStorage.setItem(K.pin, pin);   // first run: persist the generated default
        // NB: 0 is a REAL value here (client-controlled), so no `if (r)` / `|| default`
        // — both would silently turn "client-controlled" back into a pinned 2.4 MHz.
        if (r != null && Number.isFinite(Number(r))) setRate(Number(r));
        if (fp === 'full' || fp === 'half' || fp === 'quarter') setFps(fp);
        if (cp != null) setCompress(cp !== '0');
      } catch {}
    })();
  }, []);

  // Stop the server when leaving unless it's running (VibeServer is ad-hoc: a
  // single remote client, so we tear down on exit to free the dongle).
  useEffect(() => () => {
    if (runningRef.current) { stopAdvertiseRtlTcp(); stopVibeServer(); }
  }, []);

  /** ★★★ ADOPT A SERVER THAT IS ALREADY RUNNING. `running` was set in exactly ONE place — after
   *  the user pressed START — so the screen only ever knew about a server IT had started. The
   *  foreground service outlives the JS process: it is START_STICKY and VibeServerRestore rebuilds
   *  the radio behind it, which is the whole point. Come back to a phone that has restarted the
   *  app and the server is serving, the browser is connected, and this screen says nothing is
   *  happening (Stuart, 2026-08-19: "the server status screen on the phone itself is not showing
   *  any connection").
   *
   *  ★★★ AND ARMING AUTO-RESTORE PERMANENTLY MADE IT MORE LIKELY, not less — that change removed
   *      the one case where a dead app meant a dead server, so from now on the UI and the service
   *      disagree far more often. A recovery feature that the interface cannot see is
   *      indistinguishable from a broken server.
   *
   *  ★ Native is the authority, asked once on mount: it knows whether the radio is open, which is
   *    the only thing that settles it. `ip`/`port` come back with the status, so the screen can
   *    rebuild everything it would have had from start().
   */
  useEffect(() => {
    if (running) return;                    // we started it ourselves; nothing to adopt
    let cancelled = false;
    // ★★★ KEEP LOOKING, DO NOT ASK ONCE. A single check on mount loses every race there is: the
    //     service may still be rebuilding the radio, the USB permission may not have landed, or
    //     the screen may simply have opened first. Miss it and this effect NEVER runs again —
    //     `running` cannot change, so its own dependency cannot retrigger it — and the screen
    //     shows dashes for ever while the server serves happily and the admin page in a browser
    //     shows every detail correctly (Stuart, 2026-08-19: "0 and dashes", with a perfect admin
    //     page on the same server).
    // ★★ That asymmetry is the tell: a fault that shows in ONE of two readers of the same state
    //    is in the reading, not the state. The server was never wrong here.
    // ★ Stops the moment it adopts — setRunning re-runs this effect, which returns at the guard.
    const look = async () => {
      const s = await getVibeServerStatus();
      if (cancelled || !s?.running) return;
      setRunning({ ip: s.ip || '', port: s.port || 0, name });
      setStatus(s);
    };
    void look();
    const t = setInterval(look, 2000);
    return () => { cancelled = true; clearInterval(t); };
    // ★ `name` deliberately not a dependency: this is a one-shot adoption on mount, and re-running
    //   it every time the owner types a character in the name box would be absurd.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [running]);

  // Poll live status once serving.
  useEffect(() => {
    if (!running) return;
    const t = setInterval(async () => {
      const s = await getVibeServerStatus();
      if (s) setStatus(s);
      // ★★★ THE LISTING CAN COME BACK WITHOUT THE SCREEN ASKING IT TO. Turning the server on
      //     re-establishes a listing that was left on, and that happens on its own, seconds later,
      //     down in the service — so the switch showed ON with NO ADDRESS under it, which reads as
      //     a listing that half-worked (Stuart, 2026-08-22: "with this auto toggle it is not
      //     showing the friendly address anymore like it did when i toggled it manually").
      //  ★★ Read on the same beat as everything else rather than at one moment on mount: the
      //     tunnel takes several seconds to be given a hostname, and there is no event to wait
      //     for from here. The address simply appears when it exists.
      try {
        const st = await (NativeModules as any).VibeLocalSDR?.tunnelStatus?.();
        if (st) {
          const j = JSON.parse(st);
          setPublicOn(!!j.running);
          setPublicAddr(j.address || '');
          // ★ CLEARED as well as set — this only ever set it, so one transient failure stuck to
          //   the switch permanently. The status is the whole truth each time it is read.
          setPublicErr(j.error || '');
        }
      } catch { /* the switch simply keeps whatever it last knew */ }
    }, 1500);
    return () => clearInterval(t);
  }, [running]);

  /**
   * ★★★ THE PUBLIC NAME PERSISTS ON ITS OWN, and deliberately NOT via the big Start callback.
   *     That callback carries a twenty-entry dependency list, and this project has already paid
   *     for it twice: one stale closure wrote an empty password over a real one AND made the field
   *     come back blank, and another reverted twelve settings on every start. A list you have to
   *     remember is a list that will eventually be wrong, so a setting that can look after itself
   *     should. (Left as component state it was simply LOST on leaving the screen — 2026-08-22.)
   */
  useEffect(() => {
    let cancelled = false;
    AsyncStorage.getItem(K.publicName)
      .then((v) => { if (!cancelled && v != null) setPublicName(v); })
      .catch(() => { /* a missing name is not an error; the box just starts empty */ });
    return () => { cancelled = true; };
  }, []);

  useEffect(() => {
    // ★ Written as typed. There is no Save button on this screen and there should not be one.
    AsyncStorage.setItem(K.publicName, publicName).catch(() => {});
  }, [publicName]);

  /**
   * ★★★ CHANGING THE OFFER WHILE LISTED, rather than making somebody delist to do it.
   *
   *     These controls were locked once the switch was on, reasoning that altering an offer under
   *     people already relying on it should be deliberate. That was wrong in practice: the listing
   *     RESTORES ITSELF on start, so the lock meant the only way to set a share was to turn
   *     listing off first — which delists, clears the stored id, and loses the listing (Stuart,
   *     2026-08-22: "by default the public mode automatically enables and I have to disable it
   *     straight away to be able to flip the temporary toggle").
   *
   * ★★ ONLY ON A DELIBERATE CHANGE. The periodic refresh sends "unchanged" for the share window,
   *    so it can never extend an offer by accident — an offer that renewed itself every couple of
   *    minutes would never end, which is the one thing a temporary share must not do.
   * ★ Skips the first run: mounting is not somebody changing their mind.
   */
  const shareTouched = useRef(false);
  useEffect(() => {
    if (!shareTouched.current) { shareTouched.current = true; return; }
    if (!publicOn || !running?.port) return;
    const nm = (publicName || name || '').trim();
    if (nm.length < 2) return;
    void (async () => {
      try {
        const where = await getResolvedServerLocation();
        const cov = (allowRanges || '').split(',').map((t) => t.trim()).filter(Boolean)
                      .map((t) => bands.find((b) => b.id === t)?.label || t).join(', ');
        await (NativeModules as any).VibeLocalSDR?.tunnelRepublish?.(
          nm, where?.grid || '', running.port, radio?.model || '', radio?.driver || '',
          (antenna || '').trim(), cov, radioUse === 'locked',
          publicTemp ? shareSeconds() : 0);
      } catch { /* the listing keeps the window it had */ }
    })();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [publicTemp, publicForN, publicForU]);

  /**
   * ★★★ CORRECT A LISTING THAT RESTORED ITSELF. When the server comes back on its own the tunnel
   *     is re-established from values stored the last time the switch was flicked BY HAND — there
   *     is no UI to ask, which is exactly what makes a headless restore possible. The cost is that
   *     the entry can describe the server as it WAS: Stuart's listing still said "fm" hours after
   *     the app had learned to say "FM Broadcast Band" (2026-08-22), and a changed aerial or band
   *     limit would be equally stale.
   *  ★★ So once there IS a screen, it says the current truth once. It is the ordinary ping with
   *     fresh arguments — no tunnel restart, so nobody listening is disturbed.
   *  ★ Deliberately not on every render: it runs when the listing is up and the values it would
   *    send have actually changed.
   */
  const republished = useRef('');
  useEffect(() => {
    if (!publicOn || !running?.port) return;
    const nm = (publicName || name || '').trim();
    if (nm.length < 2) return;
    const cov = (allowRanges || '').split(',').map((t) => t.trim()).filter(Boolean)
                  .map((t) => bands.find((b) => b.id === t)?.label || t).join(', ');
    void (async () => {
      try {
        const where = await getResolvedServerLocation();
        // ★★★ THE LOCATOR IS PART OF WHAT MIGHT HAVE CHANGED. A receiver can be CARRIED — take the
        //     phone away for a week and the map should follow it, which it cannot do if the
        //     position is only read when some other setting is edited (Stuart, 2026-08-22: "if I
        //     were to take my moto and say set it up from a holiday destination would the map move
        //     with it"). Resolved first, then compared, so a move is a change like any other.
        const sig = [nm, cov, (antenna || '').trim(), radio?.model, radio?.driver,
                     radioUse === 'locked', where?.grid || ''].join('|');
        if (republished.current === sig) return;
        republished.current = sig;
        // ★★ AND THE NAME THE RECEIVER ANSWERS TO FOLLOWS THE PUBLIC ONE. publishLocation() runs
        //    at server start and on a location change, so editing the public name left the page
        //    calling itself by the old one until a restart — and the whole point of the public
        //    name is that it is what a stranger arriving from the directory was promised.
        try { await publishLocation(); } catch {}
        await (NativeModules as any).VibeLocalSDR?.tunnelRepublish?.(
          nm, where?.grid || '', running.port, radio?.model || '', radio?.driver || '',
          // ★★ -1 = leave the share window alone. It was 0, which the server reads as "make this
          //    PERMANENT" — so this correction, which exists only to refresh the aerial and the
          //    band names, would have wiped the end off a temporary share every time it ran.
          //  ★ This runs on a timer: an offer that renewed itself would never end.
          (antenna || '').trim(), cov, radioUse === 'locked', -1);
      } catch { /* the listing simply keeps what it had */ }
    })();
    // ★★ AND CHECK AGAIN ON A SLOW BEAT. Every other input here changes because somebody typed
    //    something, so a render tells us to look; a phone being carried changes NOTHING on screen.
    //    Two minutes is far below the 15-minute ping and costs one location read.
    const t = setInterval(() => { republished.current = ''; }, 120_000);
    return () => clearInterval(t);
  }, [publicOn, running, publicName, name, allowRanges, antenna, bands, radio, radioUse]);

  /**
   * Can this build tunnel at all, and are we already listed?
   *
   * ★★★ ITS OWN EFFECT, AND DELIBERATELY NOT GATED ON `running`. This first lived inside the mDNS
   *     hostname effect, which begins `if (!running) return` — correct there, because a .local
   *     name only exists once the responder is up. But a CAPABILITY question has to be answerable
   *     while the server is still being SET UP, which is exactly when somebody goes looking for
   *     the switch. Gated that way, publicSupported stayed false and the whole section rendered as
   *     NOTHING — no error, no log, just an absent control (2026-08-22).
   * ★ Which is the same shape as the missing-@ReactMethod bug: the failure of an optional call is
   *   silence, so anything that decides whether a control EXISTS must be checked on its own.
   */
  useEffect(() => {
    let cancelled = false;
    (async () => {
      const Local = (NativeModules as any).VibeLocalSDR;
      try {
        const ok = !!(await Local?.tunnelSupported?.());
        if (!cancelled) setPublicSupported(ok);
      } catch { if (!cancelled) setPublicSupported(false); }
      try {
        const st = await Local?.tunnelStatus?.();
        if (!cancelled && st) {
          const j = JSON.parse(st);
          setPublicOn(!!j.running); setPublicAddr(j.address || ''); setPublicErr(j.error || '');
        }
      } catch { /* status is a nicety — never let it break the screen */ }
    })();
    return () => { cancelled = true; };
  }, [running]);

  // The NAME the mDNS responder took. Asked for once the server is up, and RETRIED for a
  // few seconds: the responder probes the network for a clash before it commits to a
  // name (that's how a second phone ends up as "-2"), so the answer is not ready the
  // instant the server starts.
  useEffect(() => {
    if (!running) { setMdnsHost(''); return; }
    let cancelled = false;
    let tries = 0;
    const ask = async () => {
      const Local = (NativeModules as any).VibeLocalSDR;
      try {
        const h = await Local?.getMdnsHostname?.();
        if (cancelled) return;
        if (h) { setMdnsHost(String(h)); return; }
      } catch { /* not serving, or no responder on this platform */ }
      if (!cancelled && ++tries < 8) setTimeout(ask, 700);
    };
    void ask();
    return () => { cancelled = true; };
  }, [running]);



  /** ★★ FETCH THE SHORTWAVE SCHEDULE ON DEMAND. It already happens at start-up; this only makes
   *  it visible and repeatable, because a silent step is indistinguishable from a broken one —
   *  nobody could tell "no schedule" from "the phone had no signal that day".
   *  ★ Reports the COUNT, not just success: a schedule that downloads and parses to nothing is
   *    the failure worth catching, and it looks identical to working from the outside. */
  const onRefreshEibi = useCallback(async () => {
    setEibiBusy(true);
    setEibiMsg('');
    try {
      const list = await loadActiveEibi();
      const Local = (NativeModules as any).VibeLocalSDR;
      if (list.length) Local?.setStationsJson?.(JSON.stringify(list));
      setEibiMsg(list.length
        ? `${list.length} stations loaded — clients will see these names.`
        : 'Nothing came back. Check the phone has a connection and try again.');
    } catch (e: any) {
      setEibiMsg(e?.message ?? 'Could not download the schedule');
    } finally {
      setEibiBusy(false);
    }
  }, []);

  const effectivePin = pinMode === 'off' ? '' : pin;
  /** ★★★ ADVANCED MODE DOES NOT START WITHOUT AN ADMIN PASSWORD (Stuart, 2026-08-12). It exposes
   *  the admin page — banning, the connection log, per-address monitoring — and every one of those
   *  is decoration if a stranger can reach the page that operates them. Simple mode asks for
   *  nothing and only warns: a home receiver on a home network is a fair thing to run. */
  /**
   * ★★★ NO ADMIN PASSWORD, NO SERVER — IN EITHER MODE. This was `advanced && !adminPw.trim()`, so
   *     Simple mode would start wide open: anybody who could reach the page could change the
   *     radio's hardware settings, and the only thing standing between a Simple server and the
   *     internet was the owner remembering not to put it there.
   * ★★ Stuart, 2026-08-17: make the user set one either way and "simple is basically ready to
   *    serve to the web straight away". The cost is exactly one field; what it buys is a mode that
   *    is safe to expose by DEFAULT rather than safe only on a trusted LAN.
   * ★ Still typed by the owner, never generated — a password minted silently and shown to nobody
   *   is what issue #19 was, and it leaves a stranger being asked for something that does not
   *   exist anywhere they can read it.
   */
  const needsAdminPw = !adminPw.trim();

  /**
   * Warn if the OS has BACKGROUND-RESTRICTED us, before we start serving.
   *
   * SDRScreen already checks this — but only for local LISTENING, and it never mounts
   * when you go straight to "Use as server". So a fresh install on a phone that
   * restricts by default (Motorola does) starts a server the OS then quietly starves,
   * with nothing on screen to explain it. That's worse for a server than for audio:
   * nobody is watching it, so the only symptom is "my receiver stopped answering".
   *
   * Resolves true if we should go ahead and start.
   */
  const checkBackgroundAllowed = useCallback(async (): Promise<boolean> => {
    const Local = (NativeModules as any).VibeLocalSDR;
    if (!Local?.isBackgroundRestricted) return true;
    let restricted = false;
    try { restricted = await Local.isBackgroundRestricted(); } catch { return true; }
    if (!restricted) return true;

    return new Promise<boolean>(resolve => {
      Alert.alert(
        'Allow background usage',
        "This phone restricts VibeSDR when it isn't on screen, which will starve the server — clients drop out or stop connecting.\n\n" +
        "To fix it:\n" +
        "1. Tap “Open Settings”.\n" +
        "2. Open “App battery usage” (or “Battery”) and turn ON “Allow background usage” (some phones call it “Unrestricted” / “Don't optimise”).\n" +
        "3. Come back and start the server.",
        [
          { text: 'Open Settings', onPress: () => { Local?.openAppSettings?.(); resolve(false); } },
          { text: 'Start anyway', style: 'destructive', onPress: () => resolve(true) },
        ],
        { cancelable: true, onDismiss: () => resolve(false) },
      );
    });
  }, []);

  // ★★★ EVERY VALUE THIS CALLBACK PERSISTS, READ FRESH — because a dependency list is a list you
  //     must remember to update, and it will be wrong. `start()` writes what it is about to send,
  //     so anything missing from its deps was captured STALE and written back over the user's
  //     change: the setting reverted the moment the server started, which looks exactly like a
  //     save bug. Stuart, 2026-08-21: "the hard/soft limit always reverts back and is not
  //     remembered" — and TWELVE settings were in that state, not one (limitSoft, idleKick,
  //     idleSaver, lockedCentre, zoomSpec, spectrogram, idleGrace, antenna, antennaIcon and the
  //     three landing fields).
  //  ★★★ THE FILE HAD ALREADY LEARNED THIS AND FORGOTTEN IT. The note by the deps array describes
  //      the same fault with the admin password — "one stale closure, two symptoms, and an evening
  //      of looking at the server for a fault that was three lines above it". It came back because
  //      the fix was "add the name to the list", and the list grew.
  //  ★ A ref is assigned on every render, so it cannot be stale by construction and a setting added
  //    later is carried without anyone remembering anything. Same reasoning as VibeServerBoot
  //    carrying the whole config rather than a hand-maintained subset.
  const live = useRef<any>({});
  live.current = {
    limitSoft, idleKick, idleSaver, lockedCentre, zoomSpec, spectrogram, idleGrace,
    antenna, antennaIcon, landingMsg, landingUrl, landingLbl,
  };

  const start = useCallback(async () => {
    if (!(await checkBackgroundAllowed())) return;
    setError(null);
    setStarting(true);
    const n = name.trim() || 'VibeSDR';
    saveServerName(n);
    await AsyncStorage.multiSet([
      [K.proto, proto], [K.advertise, advertise ? '1' : '0'],
      [K.pinMode, pinMode], [K.pin, pin], [K.rate, String(rate)],
      [K.fps, fps], [K.compress, compress ? '1' : '0'],
      [K.webServer, webServer ? '1' : '0'],
      [K.adminPw, adminPw], [K.uncomp, String(uncomp)], [K.limitMin, String(limitMin)],
      [K.advanced, advanced ? '1' : '0'], [K.maxUsers, String(maxUsers)],
      [K.landingMsg, live.current.landingMsg], [K.landingUrl, live.current.landingUrl], [K.landingLbl, live.current.landingLbl],
      [K.limitSoft, live.current.limitSoft ? '1' : '0'], [K.idleKick, String(live.current.idleKick)],
      [K.idleSaver, live.current.idleSaver ? '1' : '0'], [K.lockedCentre, String(live.current.lockedCentre)],
      [K.zoomSpectrum, live.current.zoomSpec ? '1' : '0'], [K.spectrogram, live.current.spectrogram ? '1' : '0'],
      [K.idleGrace, String(live.current.idleGrace)], [K.antenna, live.current.antenna], [K.antennaIcon, live.current.antennaIcon],
      [K.allowRanges, allowRanges], [K.blockRanges, blockRanges],
      [K.gainLimits, gainLimits], [K.gainLocks, gainLocks], [K.gainSplits, gainSplits],
      [K.restGain, String(restGain)],
      [K.rtlAgc, rtlAgc ? '1' : '0'],
      [K.tunerBwAuto, tunerBwAuto ? '1' : '0'],
      [K.agcLock, agcLock ? '1' : '0'], [K.proxies, proxies],
      [K.oneRadioPerIp, oneRadioPerIp ? '1' : '0'],
    ]);
    if (Platform.OS === 'android' && Platform.Version >= 33) {
      try { await PermissionsAndroid.request(PermissionsAndroid.PERMISSIONS.POST_NOTIFICATIONS); } catch {}
    }

    // Resolve the location BEFORE starting, so the very first client to connect
    // already sees it. A typed city is geocoded once, here — never per client.
    if (locMode === 'manual') {
      const city = locCity.trim();
      const known = await getManualServerLocation();
      if (city && known?.label !== city) {
        const geo = await resolveLocation(city);
        if (!geo) {
          setStarting(false);
          setError(`Couldn't find "${city}". Check the spelling, or enter a Maidenhead locator (e.g. IO92nh) — that needs no internet.`);
          return;
        }
        await setManualServerLocation(geo);
      }
    }
    await setServerLocationMode(locMode);

    try {
      const info = await startVibeServer({
        name: n,
        // rate 0 = client-controlled: start at the full span and let the client
        // narrow it. Anything else both starts AND pins there.
        sampleRate: rate || 2_400_000,
        lockedRate: rate,
        // ★★ WHERE A LISTENER LANDS. 0 = say nothing and let the server keep its own default,
        //    rather than asserting a frequency the owner never chose.
        ...(landingHz > 0 ? { centerFreq: landingHz, mode: landingMode } : {}),

        pin: effectivePin,
        maxFftRate: fpsForTier(fps),
        compressAudio: compress,
        adminPassword: adminPw,
        uncompressedAudio: uncomp,
        sessionLimitMin: limitMin,
        webServer,
        advertise,
        // ★ Always. See the note where the switch used to be.
        autoRestore: true,
        advanced,
        maxUsers, allowRanges, blockRanges,
        // ★ The rest of the server's settings — the phone runs the same server, one radio at a
        //   time, so everything the desktop can set is set from here.
        sessionLimitSoft: live.current.limitSoft,
        idleKickMin: live.current.idleKick,
        forceIdleSaver: live.current.idleSaver,
        idleGraceSec: live.current.idleGrace,
        antenna: live.current.antenna, antennaIcon: live.current.antennaIcon,
        landingMessage: live.current.landingMsg, landingLinkUrl: live.current.landingUrl, landingLinkLabel: live.current.landingLbl,
        // ★★ Locked mode only. In single-user mode the centre follows the listener, which is what
        //    the phone has always done and is right for one person retuning the radio themselves.
        ...(advanced && radioUse === 'locked'
          ? { lockedCentre: live.current.lockedCentre, zoomSpectrum: live.current.zoomSpec,
              spectrogram: live.current.spectrogram }
          : {}),
        gainLimits, gainLocks, gainSplits, restGain, agcLock,
        /* ★★★ THE SCREEN ALREADY SAID "PINNED — listeners cannot change it" whenever a rate was
         *   chosen, and the server only ever treated `lockedRate` as a CEILING: anything narrower
         *   was allowed. So the words on this screen have been ahead of the behaviour. rateLock
         *   makes the claim true, and it is derived rather than a second switch — the app's model
         *   is already "0 = listener's choice, anything else = pinned". */
        rateLock: rate > 0,
        trustedProxies: proxies, oneRadioPerIp,
        rtlAgc,
        tunerBwAuto,
      });
      setRunning(info);
      runningRef.current = true;
      setStarting(false);
      // ★★★ BIAS-T THE MOMENT THE RADIO IS UP, not when a client eventually turns up and unlocks
      //     the hardware — a powered loop needs its DC before the first listener arrives, which is
      //     the whole point of setting it here (Stuart, 2026-08-17: "as soon as you press go live
      //     you are in play"). Never sent to a radio that has none; see hasHwSetup.
      if (hasHwSetup) setVibeServerBiasT(biasT);
      if (advertise) advertiseServer(n, info.port, 'vibeserver', effectivePin !== '');
    } catch (e: any) {
      setStarting(false);
      setError(e?.message ?? 'Could not start VibeServer. Is a supported SDR plugged in via USB OTG?');
    }
  // ★★★ EVERY PIECE OF STATE THIS READS MUST BE LISTED. adminPw, uncomp and limitMin were
  // missing, so `start` was frozen with their INITIAL values — '' and 0 — and no amount of
  // typing changed what it sent. The server dutifully applied an empty password and no limit,
  // and reported exactly that.
  // ★★ IT ALSO ATE THE SETTINGS. This callback persists what it is about to send, so every
  // Start wrote the STALE '' over the real password in storage — which is why the field came
  // back BLANK next time and made it look like a save bug. One stale closure, two symptoms,
  // and an evening of looking at the server for a fault that was three lines above it
  // (Stuart, 2026-07-27).
  }, [name, proto, advertise, pinMode, pin, rate, fps, compress, effectivePin,
      webServer, locMode, locCity, checkBackgroundAllowed,
      adminPw, uncomp, limitMin, advanced, maxUsers, allowRanges, blockRanges,
      gainLimits, gainLocks, gainSplits, restGain, agcLock, proxies, rtlAgc, tunerBwAuto,
      oneRadioPerIp]);

  const stopAndBack = useCallback(() => {
    stopAdvertiseRtlTcp();
    stopVibeServer();
    runningRef.current = false;
    navigation.goBack();
  }, [navigation]);

  /* ★★★ THREE WAYS OUT, BECAUSE STOPPING THE SERVER IS NOT ONE INTENTION.
   *
   *  There was only "stop & back to servers", and it is the WRONG one for the two commonest
   *  reasons to stop: you want to change a setting and start again, or you have finished serving
   *  and now want to go and listen to somebody else. Both were reachable only by stopping, landing
   *  on the picker, re-entering Server mode and scrolling the whole settings page back down to
   *  Start — which is exactly the walk it took to restart this phone after a test tonight.
   *  ★ Named for where they LAND, not for what they stop, because the stop is the same in all
   *    three and the destination is the entire difference.
   */
  const stopAndSetup = useCallback(() => {
    stopAdvertiseRtlTcp();
    stopVibeServer();
    runningRef.current = false;
    // ★ Stay on this screen: `running` is what chooses the status view over the config form, so
    //   clearing it drops straight back into the settings with everything as it was.
    setRunning(null);
  }, []);

  const stopAndDirectory = useCallback(() => {
    stopAdvertiseRtlTcp();
    stopVibeServer();
    runningRef.current = false;
    /* ★ noAutoConnect rides along: arriving at a directory is a choice in progress, and a default
     *  instance auto-connecting underneath it would take the user somewhere they did not ask to
     *  go — the same reason the watch path sets it. */
    navigation.navigate('InstancePicker', { openDir: 'vibeserver', noAutoConnect: true } as never);
  }, [navigation]);

  const toggleCompress = useCallback((on: boolean) => {
    setCompress(on);
    if (runningRef.current) setVibeServerCompressAudio(on);   // live toggle
  }, []);

  const regenPin = useCallback(() => {
    const p = randomPin(Date.now());
    setPin(p);
    AsyncStorage.setItem(K.pin, p);   // persist immediately so it survives re-open
  }, []);

  if (Platform.OS !== 'android' || !vibeServerSupported) {
    return (
      <SafeAreaView style={[styles.root, { backgroundColor: C.bg }]}>
        <ScrollView contentContainerStyle={{ padding: 18 }}>
          <Text style={[styles.h1, { color: C.amber, fontFamily: F }]}>Server mode</Text>
          <Text style={[styles.sub, { color: C.textDim, fontFamily: F }]}>
            Sharing a local USB dongle is only available on Android.
          </Text>
          <TouchableOpacity style={[styles.stopBtn, { borderColor: C.border }]} onPress={() => navigation.goBack()}>
            <Text style={{ color: C.gold, fontFamily: F, fontSize: 16 }}>‹ Back</Text>
          </TouchableOpacity>
        </ScrollView>
      </SafeAreaView>
    );
  }

  // ── Running view (live telemetry) ─────────────────────────────────────────
  if (running) {
    const spec = status?.specBytesPerSec ?? 0;
    const aud = status?.audioBytesPerSec ?? 0;
    const client = status?.client;
    const listeners = status?.listeners ?? 0;
    const maxUsers = status?.maxUsers ?? 1;
    return (
      <SafeAreaView style={[styles.root, { backgroundColor: C.bg }]}>
        <ScrollView contentContainerStyle={{ padding: 18, paddingBottom: 40 }}>
          <Text style={[styles.h1, { color: C.amber, fontFamily: F }]}>VibeServer</Text>
          <Text style={[styles.sub, { color: C.textDim, fontFamily: F }]}>
            {`Serving this phone's ${radio?.model ?? 'SDR'} with server-side DSP.`} Leaving this screen
            stops the server and frees the dongle.
          </Text>

          <View style={[styles.card, { borderColor: C.borderBright }]}>
            {/* BOTH WAYS IN. The IP is the one that always works; the NAME is the one a
                human can actually retype, and it survives the router handing this phone a
                different address tomorrow — which the IP does not. Showing only the IP
                hid the better of the two. */}
            <Row C={C} F={F} k="ADDRESS" v={`${running.ip}:${running.port}`} vc={C.amber} />
            {!!mdnsHost && (
              <Row C={C} F={F} k="NAME" v={`${mdnsHost}.local:${running.port}`} vc={C.amber} />
            )}
            <Row C={C} F={F} k="ACCESS" v={effectivePin ? `PIN ${effectivePin}` : 'Open (no PIN)'} vc={effectivePin ? C.green : C.amber} />
          {/* ─── ADVERTISE ON VIBESDR.NET ──────────────────────────────────────────────────
              ★★★ TWO FIELDS AND NOTHING ELSE (Stuart, 2026-08-22): a public name, and the switch.
                  Everything else the directory needs — the locator, the place, the radios — the
                  server already holds, so asking again would be asking twice.
              ★★ VibeServer only: RTL-TCP has no landing page, no PIN and no session limits, so it
                 has nothing to put in front of the public. */}
            {publicSupported && proto === 'vibeserver' ? (
            <View style={[styles.card, { borderColor: C.border, marginTop: 14 }]}>
              <Text style={[styles.section, { color: C.textDim, fontFamily: F, marginTop: 0 }]}>
                ADVERTISE ON VIBESDR.NET
              </Text>
              <TextInput value={publicName} onChangeText={setPublicName}
                editable={!publicOn}
                placeholder={name || 'My VibeServer'} placeholderTextColor={C.goldDim}
                style={[styles.input, { color: C.amber, borderColor: C.border, fontFamily: F }]} />
              {/* ★★★ A SHARE THAT ENDS. "Hans is lending me his laptop for two hours" and a club
                  server up for the week of a contest are one idea: an offer with a deadline, which
                  expires on its own so nobody has to remember to take it down (Stuart, 2026-08-22).
                  ★★ Permanent stays the default — an exception should have to be chosen — and the
                     controls lock once listed, because changing the offer under people who are
                     already relying on it is a different act from making one. */}
              <View style={[styles.rowBetween, { marginTop: 12 }]}>
                <Text style={[styles.value, { color: C.amber, fontFamily: F, flex: 1, paddingRight: 12 }]}>
                  Temporary — ends by itself
                </Text>
                <Switch value={publicTemp} disabled={publicBusy} onValueChange={setPublicTemp}
                  trackColor={{ false: C.border, true: C.green }} thumbColor={C.amber} />
              </View>
              {publicTemp ? (
                <View style={{ flexDirection: 'row', gap: 8, marginTop: 8, alignItems: 'center' }}>
                  <TextInput value={publicForN} onChangeText={setPublicForN}
                    editable={!publicBusy} keyboardType="number-pad"
                    placeholder="1" placeholderTextColor={C.goldDim}
                    style={[styles.input, { flex: 1, color: C.amber, borderColor: C.border, fontFamily: F }]} />
                  {/* ★ Pills, not a native picker: five choices, and a picker would be the only one
                      of its kind on this screen. */}
                  <View style={{ flex: 3, flexDirection: 'row', flexWrap: 'wrap', gap: 6 }}>
                    {(['minutes', 'hours', 'days', 'weeks', 'months'] as const).map((u) => (
                      <Pill key={u} C={C} F={F} active={publicForU === u} label={u}
                        onPress={() => { if (!publicBusy) setPublicForU(u); }} />
                    ))}
                  </View>
                </View>
              ) : null}
              <View style={[styles.rowBetween, { marginTop: 12 }]}>
                <Text style={[styles.value, { color: C.amber, fontFamily: F, flex: 1, paddingRight: 12 }]}>
                  {publicBusy ? 'Working\u2026' : 'List this server publicly'}
                </Text>
                <Switch value={publicOn} disabled={publicBusy || !running.port}
                  onValueChange={async (v) => {
                    const Local = (NativeModules as any).VibeLocalSDR;
                    setPublicBusy(true); setPublicErr('');
                    try {
                      if (v) {
                        const nm = (publicName || name || '').trim();
                        // ★★ An unnamed server must not be listed: the directory would have to fall
                        //    back to "vibeserver", and every unnamed server would collide on it.
                        if (nm.length < 2) {
                          setPublicErr('Give this server a public name first.');
                          setPublicBusy(false); return;
                        }
                        // ★★★ ASK FOR THE RESOLVED LOCATION, NEVER THE RAW BOX. This read locCity,
                        //     which is EMPTY on the device path (there is no locator box at all in
                        //     that mode) and holds a TOWN on the manual one — so listing was refused
                        //     with "a valid Maidenhead locator is required" by a server that knew
                        //     exactly where it was. getResolvedServerLocation() is the same
                        //     resolution publishLocation() has always done: device fix or city,
                        //     coarsened, then latLonToGrid(). ★ And it is why a CITY works — it
                        //     becomes coordinates here, so the directory never geocodes anything.
                        const where = await getResolvedServerLocation();
                        if (!where?.grid) {
                          // ★ Opted out of publishing a location, or no fix yet. Say which thing to
                          //   go and do — a bare refusal here would be a puzzle.
                          setPublicErr('Set this receiver\u2019s location first (Where this receiver is, above) \u2014 the directory places servers on a map.');
                          setPublicBusy(false); return;
                        }
                        // ★★★ THE REAL PORT, AND ONLY WHILE THE SERVER IS UP. This passed 0, so
                        //     cloudflared was told to reach http://127.0.0.1:0 — the tunnel came up
                        //     perfectly, the listing appeared on the map, and every visitor got a
                        //     502 from a receiver that was never on the other end (2026-08-22).
                        //  ★★ A tunnel to a stopped server can ONLY 502, so the switch requires a
                        //     running one rather than publishing a dead address.
                        if (!running.port) {
                          setPublicErr('Start the server first — the listing points at a receiver that has to be running.');
                          setPublicBusy(false); return;
                        }
                        // ★★ WHAT THE LISTING ACTUALLY SAYS ABOUT THIS RECEIVER. The antenna and the
                        //    tuning range are the two things a listener judges a server on before
                        //    clicking, and both are already configured here — the directory should
                        //    not be the one place they are missing. The machine it runs on is added
                        //    natively, where Build.MODEL and the SoC live.
                        const st = await Local?.tunnelStart?.(nm, where.grid, running.port, '',
                                                            radio?.model || '', radio?.driver || '',
                                                            (antenna || '').trim(),
                                                            // ★★★ THE BANDS IN WORDS, FROM THE
                                                            //   SERVER'S OWN PLAN. allowRanges is
                                                            //   the owner's shorthand — "fm" — and
                                                            //   publishing that made the directory
                                                            //   read "fm" where the receiver's own
                                                            //   landing page says "FM broadcast"
                                                            //   (Stuart, 2026-08-22). `bands` comes
                                                            //   from vibe_bands.h via getBands, so
                                                            //   this is the SAME vocabulary, not a
                                                            //   second copy that can drift.
                                                            // ★ An id with no match is passed
                                                            //   through: an owner's arbitrary range
                                                            //   is real and unnameable, and its
                                                            //   figures beat a vague label.
                                                            (allowRanges || '').split(',')
                                                              .map((t) => t.trim()).filter(Boolean)
                                                              .map((t) => bands.find((b) => b.id === t)?.label || t)
                                                              .join(', '),
                                                            // ★ 'locked' = each listener gets their
                                                            //   own VFO in a fixed window; anything
                                                            //   else with room for several is ONE
                                                            //   shared dial.
                                                            radioUse === 'locked',
                                                            // ★★★ HOW LONG THE SHARE IS OFFERED
                                                            //   FOR, in seconds — 0 = permanent.
                                                            //   Converted here where the words
                                                            //   are, so the wire carries one
                                                            //   number and no vocabulary to keep
                                                            //   in step at both ends.
                                                            publicTemp ? shareSeconds() : 0);
                        const j = st ? JSON.parse(st) : {};
                        setPublicOn(!!j.address); setPublicAddr(j.address || ''); setPublicErr(j.error || '');
                      } else {
                        const st = await Local?.tunnelStop?.('');
                        const j = st ? JSON.parse(st) : {};
                        setPublicOn(false); setPublicAddr(''); setPublicErr(j.error || '');
                      }
                    } catch (e: any) {
                      setPublicOn(false);
                      setPublicErr(String(e?.message || e));
                    } finally { setPublicBusy(false); }
                  }}
                  trackColor={{ false: C.border, true: C.green }} thumbColor={C.amber} />
              </View>
              {publicOn && publicAddr ? (
                <Text style={[styles.hint, { color: C.green, fontFamily: F, marginTop: 8 }]}>
                  Anyone can listen at {publicAddr} — share that address, it stays the same.
                </Text>
              ) : (
                <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 8 }]}>
                  Publishes this receiver on vibesdr.net so anyone can find it and listen. Your home
                  address is never published — listeners reach it through a Cloudflare tunnel.
                </Text>
              )}
              {publicErr ? (
                <Text style={[styles.hint, { color: '#ff5c5c', fontFamily: F, marginTop: 8 }]}>
                  {publicErr}
                </Text>
              ) : null}
            </View>
          ) : null}

            {/* ★★ SAY HOW MANY, not just yes/no — a shared receiver has a number, and this row is
                   the host's whole view of who is on. `listeners` and `client` come from the same
                   count in the shim (specListenerCount), so they cannot disagree with the admin
                   page the way the old spectrum-socket test did. */}
            <Row C={C} F={F} k="STATUS"
              v={client
                   ? (listeners > 1
                        ? `${listeners} listening${maxUsers > 1 ? ` of ${maxUsers}` : ''}`
                        : (status?.clientAddr ? `Connected: ${status.clientAddr}` : 'Client connected'))
                   : 'Waiting for a client…'}
              vc={client ? C.green : C.goldDim} />
            <Row C={C} F={F} k="WATERFALL" v={`${fmtRate(spec)}`} vc={client ? C.amber : C.goldDim} />
            <Row C={C} F={F} k="AUDIO" v={`${fmtRate(aud)}${status?.compressed ? '' : ' (raw)'}`} vc={client ? C.amber : C.goldDim} />
            {/* The client drives the capture rate (and the frame rate) live. Showing
                them here is how the HOST sees the server answering the client —
                otherwise a remote change is invisible from this end. */}
            <Row C={C} F={F} k="SAMPLE RATE"
              v={status?.sampleRate ? `${(status.sampleRate / 1e6).toFixed(3).replace(/0+$/, '').replace(/\.$/, '')} MS/s` : '—'}
              vc={client ? C.amber : C.goldDim} />
            <Row C={C} F={F} k="FRAME RATE"
              v={status?.fftRate ? `${Math.round(status.fftRate)} fps` : '—'}
              vc={client ? C.amber : C.goldDim} />
            {/* CPU — a phone serving on a shelf for hours invites "what is this costing me",
                and the honest answer beats guessing from how warm it feels. Percent of ONE
                core, so it stays comparable with the DSP benchmark figures; the core count is
                shown alongside so 120% reads as "1.2 of 8" rather than as an error. */}
            <Row C={C} F={F} k="CPU"
              v={status?.cpu ? `${status.cpu.toFixed(0)}% of 1 core${status?.cores ? ` (of ${status.cores})` : ''}` : '—'}
              vc={client ? C.amber : C.goldDim} />
          </View>

          <Text style={[styles.hint, { color: C.textDim, fontFamily: F }]}>
            The PIN protects tuning control — it is not audio encryption. Use a VPN
            for privacy on untrusted networks.
          </Text>

          {/* ★ The live compressed-audio toggle went with the one in the settings form — see the
              note there. Uncompressed audio is the setting that decides this now. */}

          <TouchableOpacity style={[styles.stopBtn, { borderColor: C.red }]} onPress={stopAndBack}>
            <Text style={{ color: C.red, fontFamily: F, fontSize: 16 }}>■ Stop server & back to servers</Text>
          </TouchableOpacity>

          {/* ★ The two destinations the single button could not reach — see stopAndSetup. Drawn in
              amber rather than red: they stop the server too, but the RED one is the "I have
              finished" button and these are "I am going somewhere else next". */}
          <TouchableOpacity style={[styles.stopBtn, { borderColor: C.amber, marginTop: 10 }]}
                            onPress={stopAndSetup}>
            <Text style={{ color: C.amber, fontFamily: F, fontSize: 16 }}>■ Stop server & back to setup</Text>
          </TouchableOpacity>

          <TouchableOpacity style={[styles.stopBtn, { borderColor: C.amber, marginTop: 10 }]}
                            onPress={stopAndDirectory}>
            <Text style={{ color: C.amber, fontFamily: F, fontSize: 16 }}>■ Stop server & browse the directory</Text>
          </TouchableOpacity>
        </ScrollView>
      </SafeAreaView>
    );
  }

  // ── Config view ───────────────────────────────────────────────────────────
  return (
    <SafeAreaView style={[styles.root, { backgroundColor: C.bg }]}>
      <ScrollView contentContainerStyle={{ padding: 18, paddingBottom: 40 }}>
        <Text style={[styles.h1, { color: C.amber, fontFamily: F }]}>Server mode</Text>
        <Text style={[styles.sub, { color: C.textDim, fontFamily: F }]}>
          {`Share this phone's ${radio?.model ?? 'SDR'} over your network.`}
        </Text>

        {/* Protocol picker */}
        <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>PROTOCOL</Text>
        {/* ★★★ PERSISTED THE MOMENT IT IS CHOSEN, not only when the server starts.
             Every other setting on this screen is written by start()'s multiSet, which means a
             choice you make and do not start is simply lost — and the hint two hundred lines
             below says "Settings are saved as you change them", which was not true of any of
             them. It matters most for the PROTOCOL because that is the one the owner rarely
             revisits: a public receiver that comes back on the wrong protocol is off the air in
             a way nothing on the phone reports (Stuart, 2026-08-27: "it should remember it
             always did").
             ★★ It also removes a dependence on surviving the start handoff. start() writes and
                then hands over to the foreground service; a write that has resolved but not yet
                been committed by AsyncStorage's SQLite when the process churns is lost, which
                fits the one start tonight that did not stick while the next one did. Writing on
                selection means the value is already on disk long before any of that. */}
        <ProtoCard C={C} F={F} active={proto === 'vibeserver'}
          onPress={() => { setProto('vibeserver'); void AsyncStorage.setItem(K.proto, 'vibeserver'); }}
          title="VibeServer" tag="Recommended"
          desc="More secure, less data. Server-side DSP, compressed audio + waterfall, PIN protected." />
        <ProtoCard C={C} F={F} active={proto === 'rtltcp'}
          onPress={() => { setProto('rtltcp'); void AsyncStorage.setItem(K.proto, 'rtltcp'); }}
          title="RTL-TCP" tag="Compatible"
          desc="Raw IQ, maximum compatibility. Needs a fast, stable network. No PIN." />

        {/* ★★ "LOCAL NAME", not "advertised name" — it names this server ON THIS NETWORK, and
            the .local address is derived from it. It was renamed when public listing arrived
            (Stuart, 2026-08-22): with a PUBLIC name a few rows below, "advertised" no longer said
            WHICH audience, and two name boxes that both claim to be the advertised one is exactly
            the ambiguity that makes somebody type their callsign into the wrong one. */}
        <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>LOCAL NAME</Text>
        <TextInput value={name} onChangeText={setName}
          placeholder="VibeSDR" placeholderTextColor={C.goldDim}
          style={[styles.input, { color: C.amber, borderColor: C.border, fontFamily: F }]} />

        {/* Shared: auto-discovery */}
        <View style={[styles.card, { borderColor: C.border, marginTop: 14 }]}>
          <View style={styles.rowBetween}>
            <Text style={[styles.value, { color: C.amber, fontFamily: F, flex: 1, paddingRight: 12 }]}>
              Server auto-discovery
            </Text>
            <Switch value={advertise} onValueChange={setAdvertise}
              trackColor={{ false: C.border, true: C.green }} thumbColor={C.amber} />
          </View>
          <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 8 }]}>
            {advertise
              ? 'This server appears automatically on other VibeSDR devices on the network.'
              : "Hidden — clients must enter this phone's address by hand. Good on a public hotspot" +
                (proto === 'rtltcp' ? ' (RTL-TCP has no PIN).' : '.')}
          </Text>
        </View>

        {proto === 'vibeserver' ? (
          <>
            {/* ─── SERVER SETTINGS ───────────────────────────────────────────────────────────
                ★★ Everything from here to RADIO SETTINGS belongs to the SERVER — who may reach it,
                   what it calls itself, what it costs the uplink. The radio in front of it is a
                   separate group below, because they are separate ideas and eighteen sections in
                   one undifferentiated scroll made them look like one. */}
            <Text style={[styles.section, { color: C.gold, fontFamily: F, marginTop: 22 }]}>
              SERVER SETTINGS
            </Text>

            {/* ★★ SECURITY — the PIN and the admin password TOGETHER, because they are two
                secrets with two different jobs and having them in separate parts of the screen
                invited the reading that one replaces the other (Stuart, 2026-07-27).
                The distinction in one line each: the PIN is the DOOR, the password is the
                CONTROLS INSIDE. */}
            <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>SECURITY</Text>
            <View style={[styles.card, { borderColor: C.border }]}>
              <Text style={[styles.value, { color: C.amber, fontFamily: F, fontSize: 14 }]}>
                Two separate protections
              </Text>
              {/* ★★ SHORT ON PURPOSE. This ran to a dozen lines and pushed everything else off the
                  screen; the club example carries the whole distinction in one sentence and is
                  the thing people remember (Stuart, 2026-08-17: the explanations "are super long
                  winded and makes the GUI long to scroll"). */}
              <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 8 }]}>
                <Text style={{ color: C.gold }}>Password — the SETTINGS.</Text> Bias-T, direct
                sampling, calibration, the admin page.{'\n\n'}
                <Text style={{ color: C.gold }}>PIN — the CONNECTION.</Text> Who may listen at all.
                {'\n\n'}
                A radio club keeps the password and hands out the PIN. A public receiver has a
                password and no PIN.
              </Text>
            </View>

            {/* ★★ SECURITY, IN ONE PLACE AND IN THE RIGHT ORDER: the password that protects the
                SETTINGS first, then the PIN that gates ACCESS. They answer two different
                questions and were a screen apart, which is how an owner ends up setting one
                believing it did the other's job. Stuart's example is the one that makes it
                land: a radio club keeps the admin password and hands the PIN to members. */}
            <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>PASSWORD — WHO MAY CHANGE THINGS</Text>
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
              <TextInput value={adminPw}
                onChangeText={(v) => { setAdminPw(v); AsyncStorage.setItem(K.adminPw, v);
                                       if (runningRef.current) setVibeServerAdminSecret(v); }}
                placeholder="none — controls are open" placeholderTextColor={C.goldDim}
                autoCapitalize="none" autoCorrect={false}
                // ★ Dots by default: this gets typed at a rally or a club night with people
                // stood behind you (Stuart, 2026-07-27).
                secureTextEntry={!showAdminPw}
                style={[styles.input, { color: C.amber, borderColor: C.border, fontFamily: F, flex: 1 }]} />
              <TouchableOpacity onPress={() => setShowAdminPw(v => !v)}
                style={[styles.card, { borderColor: C.border, paddingVertical: 10, paddingHorizontal: 12 }]}>
                <Text style={{ color: C.textDim, fontFamily: F, fontSize: 12 }}>
                  {showAdminPw ? 'HIDE' : 'SHOW'}
                </Text>
              </TouchableOpacity>
            </View>
            <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 6 }]}>
              Required. Protects the SETTINGS — bias-T, direct sampling, calibration, the admin
              page. A club owner keeps this; members get the PIN below.
            </Text>

            {/* PIN */}
            <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>PIN — WHO MAY CONNECT</Text>
            <View style={styles.pillRow}>
              {(['random', 'custom', 'off'] as PinMode[]).map(m => (
                <Pill key={m} C={C} F={F} active={pinMode === m}
                  label={m === 'random' ? 'Random' : m === 'custom' ? 'Custom' : 'No PIN'}
                  onPress={() => setPinMode(m)} />
              ))}
            </View>
            {pinMode !== 'off' && (
              <View style={styles.rowBetween}>
                <TextInput value={pin}
                  onChangeText={t => { setPin(t.replace(/[^0-9]/g, '').slice(0, 12)); if (pinMode === 'random') setPinMode('custom'); }}
                  editable={pinMode === 'custom'}
                  keyboardType="number-pad"
                  style={[styles.input, { flex: 1, color: C.amber, borderColor: C.border, fontFamily: F }]} />
                {pinMode === 'random' && (
                  <TouchableOpacity onPress={regenPin} style={[styles.regen, { borderColor: C.border }]}>
                    <Text style={{ color: C.gold, fontFamily: F, fontSize: 18 }}>↻</Text>
                  </TouchableOpacity>
                )}
              </View>
            )}
            <Text style={[styles.hint, { color: C.textDim, fontFamily: F }]}>
              {pinMode === 'off'
                ? 'Anyone on the network can connect and tune. Use only on a trusted LAN.'
                : 'Clients enter this PIN once. It authenticates control without ever crossing the wire (HMAC challenge-response).'}
            </Text>

            {/* Receiver location. A SEPARATE consent from the app's own location
                permission: granting location to sort the instance list by distance is
                not consent to BROADCAST that position to every client. So this is
                opt-in, and 'off' is the default. */}
            <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>RECEIVER LOCATION</Text>
            <View style={styles.pillRow}>
              {(['off', 'device', 'manual'] as LocationMode[]).map(m => (
                <Pill key={m} C={C} F={F} active={locMode === m}
                  label={m === 'off' ? 'Not set' : m === 'device' ? 'Use device' : 'Enter city'}
                  onPress={() => setLocMode(m)} />
              ))}
            </View>
            {locMode === 'manual' && (
              <TextInput value={locCity} onChangeText={setLocCity}
                placeholder="Town, or grid locator (Northampton / IO92nh)" placeholderTextColor={C.textDim}
                style={[styles.input, { color: C.amber, borderColor: C.border, fontFamily: F }]} />
            )}
            <Text style={[styles.hint, { color: C.textDim, fontFamily: F }]}>
              {locMode === 'off'
                ? 'No location is published. Clients show "receiver location not set" and go without spot distances, map centring and the regional band plan.'
                : locMode === 'device'
                ? "This phone's coarse position (~1 km) is published to every client that connects."
                : 'A town or city needs an internet connection when you press Start (looked up once, then stored). A Maidenhead locator works OFFLINE — use it if this server has no internet. Published to every client; set it if the receiver lives somewhere other than where you are.'}
            </Text>
            <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 6 }]}>
              Distances and band edges are properties of the ANTENNA, not the listener —
              80m is 3.5–3.8 MHz in Region 1 but 3.5–4.0 in Region 2.
            </Text>

            {/* ★★★ NOT AN ADVANCED SETTING. This lived under Advanced, but a SIMPLE server put on
                the internet through a tunnel needs it just as much — and without it every visitor
                arrives as 127.0.0.1, which counts as the owner, so the time limit switches itself
                OFF and the ban list cannot tell two people apart. We hit exactly that on the demo
                (2026-08-11). It belongs wherever the server is reachable from outside. */}
            <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>BEHIND A REVERSE PROXY</Text>
            <TextInput value={proxies}
              onChangeText={(v) => { setProxies(v); AsyncStorage.setItem(K.proxies, v); }}
              placeholder="e.g. 127.0.0.1, 10.0.0.0/8  (empty = direct)" placeholderTextColor={C.goldDim}
              autoCapitalize="none" autoCorrect={false}
              style={[styles.input, { color: C.amber, borderColor: C.border, fontFamily: F }]} />
            <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 6 }]}>
              Only if you reach this phone through a tunnel (Cloudflare, nginx, Tailscale). List the
              addresses you trust. Empty otherwise — without it everyone behind the tunnel looks
              like you, so the time limit and the ban list stop working.
            </Text>
            {/* ★★★ SAY THAT THE LISTING ALREADY DID THIS, because otherwise the box reads EMPTY on a
                server that is publicly listed — which looks exactly like the misconfiguration this
                setting exists to prevent (Stuart, 2026-08-22: "I thought when public listing was
                enabled it automatically filled out the reverse proxy box").
                ★★ It is NOT written INTO the box on purpose. This field is what the OWNER trusts;
                   the loopback entry is what the tunnel needs, and it lasts exactly as long as the
                   tunnel does. Merging them would mean guessing, when listing is switched off,
                   whether 127.0.0.1 was ours to remove or something they typed themselves — and
                   guessing wrong either strips a real setting or silently leaves loopback trusted
                   on a directly reachable server.
                ★ So the effective list is owner + loopback while listed, and this line is the only
                  honest way to show a value that is real but not stored here. */}
            {publicOn ? (
              <Text style={[styles.hint, { color: C.green, fontFamily: F, marginTop: 6 }]}>
                127.0.0.1 is trusted automatically while this server is listed on VibeSDR.net — the
                tunnel reaches it from loopback. You do not need to add it, and it goes again when
                you turn the listing off.
              </Text>
            ) : null}

            {/* ★★★ SEVERAL RADIOS FROM ONE ADDRESS. Alongside the proxy setting because they are
                the same subject — what the server takes an ADDRESS to mean — and an owner thinking
                about one is thinking about the other.
                ★★ Default REFUSE, which is what a public receiver needs: a single visitor once
                   took BOTH radios of one by opening a tab on each. But it is also what makes a
                   phone and its watch look like one greedy visitor, since they leave by the same
                   address (GitHub #21), and on a SHARED receiver it is the difference between a
                   household being one listener and being three — which is why the switch exists
                   rather than the rule simply being hard-wired.
                ★ Wired through VibeServerBoot like every other server setting, so it survives a
                  restart and a crash-restore by construction. */}
            <View style={[styles.rowBetween, { marginTop: 14 }]}>
              <Text style={[styles.value, { color: C.amber, fontFamily: F, flex: 1, paddingRight: 12 }]}>
                Allow several connections from one address
              </Text>
              <Switch value={!oneRadioPerIp}
                onValueChange={(v) => { setOneRadioPerIp(!v);
                                        AsyncStorage.setItem(K.oneRadioPerIp, !v ? '1' : '0'); }}
                trackColor={{ false: C.border, true: C.green }} thumbColor={C.amber} />
            </View>
            <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 6 }]}>
              {oneRadioPerIp
                ? 'Off — a second connection from an address that is already listening is refused, '
                  + 'and told so. Right for a public receiver, where it stops one visitor holding '
                  + 'more than their share.'
                : 'On — one address may hold several connections at once. What a household needs: '
                  + 'a phone and its watch, or two people on one broadband line, leave by the same '
                  + 'address and would otherwise count as one greedy visitor. Unwise on a public '
                  + 'receiver, where one person could occupy every slot you have.'}
            </Text>

            {/* Bookmarks. The server LEARNS stations from RDS as clients tune, so the
                list grows on its own — which means it also needs a way to be emptied,
                and a way to be seeded from a list you already have. */}
            {/* ★★ THE BOOKMARK IMPORT LIVED HERE AND HAS GONE. The server still learns stations
                from RDS as clients tune, still shares them with every client, and still accepts
                an imported list — through the web client or the app, signed in as admin, which
                is where somebody actually has a file to hand (Stuart, 2026-08-19). A second
                importer on a phone screen was a control for a job nobody does there. */}

            {/* ★★★ THE "RESTART IF IT CRASHES" SWITCH HAS GONE, THE BEHAVIOUR HAS NOT. It is
                always armed now: the foreground service is START_STICKY, so Android brings the
                SERVICE back on its own, and VibeServerRestore.arm() is what rebuilds the RADIO
                behind it — without which the notification claims a server that is not there.
                ★★ Nobody would rationally choose "do not come back after a crash", so the switch
                   was a decision that could only be got wrong (Stuart, 2026-08-19). The old
                   argument for it was a shim crashing REPEATEDLY and crash-looping; that is a bug
                   to fix, not a setting to offer.
                ★ The long-term warning it carried was real and moved to LEAVING IT RUNNING below
                  — a reboot needs the dongle replugged by hand, and Samsung ships scheduled
                  restarts ON, which kills an unattended receiver every few days. */}

            {/* Web server. Turning this OFF means a browser gets nothing — only the
                VibeSDR app can connect. It's the blunt lock for a server you don't
                want a stranger stumbling into via a URL. */}
            <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>UNCOMPRESSED AUDIO</Text>
            {([
              [0, 'Off', 'Never send raw audio. Listeners who cannot decode Opus are turned away with a reason.'],
              [1, "Listener's choice", 'An UNCOMPRESSED button appears in each listener’s audio menu. Defaults to Opus.'],
              [2, 'Compatibility only', 'Raw only as a fallback for a client that cannot decode Opus — no user-facing switch.'],
            ] as const).map(([v, label, hint]) => (
              <OptRow key={v} C={C} F={F} active={uncomp === v} label={label} hint={hint}
                onPress={() => { setUncomp(v as 0 | 1 | 2); AsyncStorage.setItem(K.uncomp, String(v));
                                 if (runningRef.current) setVibeServerUncompressedAudio(v as 0 | 1 | 2); }} />
            ))}
            <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 6 }]}>
              This phone always gets uncompressed audio from its own server — the setting rations
              your UPLINK, and listening on the same device never touches it.
            </Text>

            {/* ★★★ THE MODE SWITCH SITS WHERE IT CHANGES SOMETHING — immediately above the radio
                controls, because that is the only half of this screen it affects. It used to lead
                the whole page, which read as though it governed everything below it; the SERVER
                settings are identical in both modes, and putting a mode question above them
                implied a choice that was not there (Stuart, 2026-08-17: "the simple and advanced
                toggle needs to be above the radio controls since the server controls are identical
                in both modes").
                It sat at the BOTTOM, under the last setting, where it read as one more option
                rather than the choice the page is organised around (Stuart, 2026-08-12: "the
                simple and full button is at the bottom and doesnt change the GUI"). */}
            {/* Waterfall frame rate */}
            <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>WATERFALL RATE</Text>
            {FPS_TIERS.map(t => (
              <OptRow key={t.key} C={C} F={F} active={fps === t.key} label={t.label} onPress={() => setFps(t.key)} />
            ))}

            {/* ★★★ NO "COMPRESSED AUDIO" SWITCH — UNCOMPRESSED AUDIO above supersedes it. That
                three-way setting already says what this server sends: Off (Opus only), Listener's
                choice, or Compatibility. A separate switch for the same decision meant two
                controls that could contradict each other — "compressed off" plus "uncompressed
                off" is a receiver that sends nothing anyone asked for — and the browser's setup
                page has only ever had the three-way (Stuart, 2026-08-21: "the compressed audio
                switch has been superceded ... which assumes Compressed audio is the default").
                ★ Opus stays ON at the server, always: it is the default the three-way is written
                  around, and raw is reached through that setting rather than by turning Opus off.
                ★ The stored key and the wire field are untouched, so an existing config that
                  carries `compress:false` still starts — it simply no longer has a control that
                  can set it. */}

            {/* ★★ UNCOMPRESSED AUDIO — the owner's UPLINK policy, three-way.
                Raw is 48 kHz stereo int16: ~187 KB/s per listener, which is why this is not
                a plain switch. Hans asked for it after hearing Opus on a good system. */}
            <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>WEB CLIENT</Text>
            <View style={[styles.card, { borderColor: C.border }]}>
              <View style={styles.rowBetween}>
                <Text style={[styles.value, { color: C.amber, fontFamily: F, flex: 1, paddingRight: 12 }]}>
                  Serve the web client
                </Text>
                <Switch value={webServer} onValueChange={(v) => {
                  setWebServer(v);
                  // ★★★ NO WEB CLIENT, NO ADVANCED MODE. Everything Advanced adds beyond sharing —
                  //     the connection log, per-address monitoring, banning, the country map — is
                  //     ON THE ADMIN PAGE, and the admin page is the web client. Leaving Advanced
                  //     selected with the web client off would offer a management surface with no
                  //     way to reach it (Stuart, 2026-08-19).
                  // ★ Switched back rather than left dangling: a mode that silently does nothing
                  //   is worse than one that visibly turns itself off.
                  if (!v && advanced) { setAdvanced(false); AsyncStorage.setItem(K.advanced, '0'); }
                }}
                  trackColor={{ false: C.border, true: C.green }} thumbColor={C.amber} />
              </View>
              <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 8 }]}>
                {webServer
                  ? 'Anyone on the network can open this server in a browser (the PIN still applies).'
                  : 'Browsers get nothing — only the VibeSDR app can connect.'}
              </Text>
            </View>


            {/* ★★★ THE OWNER'S STANDING MESSAGE — the same field the Pi and the Mac carry, so a
                phone server can say the same things: house rules, a donation link, or an
                explanation of behaviour that looks wrong and is not.
                ★★ NOT the temporary maintenance notice, which expires and can be dismissed. This
                   one stays up and sits on the landing screen before anybody connects. */}
            <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>LANDING SCREEN MESSAGE</Text>
            <View style={[styles.card, { borderColor: C.border }]}>
              <TextInput value={landingMsg} onChangeText={setLandingMsg}
                placeholder="e.g. Shared receiver — please retune before you leave" multiline
                placeholderTextColor={C.textDim} maxLength={500}
                style={[styles.input, { color: C.amber, fontFamily: F, borderColor: C.border,
                                        minHeight: 74, textAlignVertical: 'top' }]} />
              <View style={[styles.rowBetween, { marginTop: 8, gap: 8 }]}>
                <TextInput value={landingUrl} onChangeText={setLandingUrl}
                  placeholder="https://… (optional link)" placeholderTextColor={C.textDim}
                  autoCapitalize="none" keyboardType="url"
                  style={[styles.input, { flex: 1, color: C.amber, fontFamily: F, borderColor: C.border }]} />
              </View>
              <TextInput value={landingLbl} onChangeText={setLandingLbl}
                placeholder="Link text — e.g. Support this server" placeholderTextColor={C.textDim}
                maxLength={60}
                style={[styles.input, { marginTop: 8, color: C.amber, fontFamily: F, borderColor: C.border }]} />
              <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 8 }]}>
                Shown to everybody who opens this server, so nothing private here.
                {'\n'}Only http:// and https:// links are kept — the server drops anything else.
              </Text>
            </View>

            {/* ★★ EiBi: the shortwave schedule, fetched from eibispace.de and handed to the shim so
                every client sees station names. It has always happened silently at start-up, which
                meant nobody could tell "no schedule" from "the phone had no signal that day". */}
            <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>SHORTWAVE SCHEDULE</Text>
            <View style={[styles.card, { borderColor: C.border }]}>
              <Text style={[styles.hint, { color: C.textDim, fontFamily: F }]}>
                {eibiMsg || 'The EiBi schedule names shortwave stations for every client. Downloaded '
                          + 'automatically when the server starts; refresh it here if you want the '
                          + 'newest season now.'}
              </Text>
              <TouchableOpacity onPress={onRefreshEibi} disabled={eibiBusy}
                style={[styles.regen, { borderColor: C.border, marginTop: 10, alignItems: 'center',
                                        opacity: eibiBusy ? 0.5 : 1 }]}>
                <Text style={{ color: C.gold, fontFamily: F, fontSize: 13 }}>
                  {eibiBusy ? 'DOWNLOADING…' : 'DOWNLOAD NOW'}
                </Text>
              </TouchableOpacity>
            </View>

            <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>MODE</Text>
            <View style={{ flexDirection: 'row', gap: 8 }}>
              {([false, true] as const).map(v => (
                <TouchableOpacity key={String(v)} disabled={v && !webServer}
                  onPress={() => { setAdvanced(v);
                                   AsyncStorage.setItem(K.advanced, v ? '1' : '0'); }}
                  style={[styles.card, { flex: 1, borderColor: advanced === v ? C.green : C.border,
                                         backgroundColor: advanced === v ? C.green + '18' : 'transparent',
                                         // ★ Greyed, not hidden: an option that vanishes leaves the
                                         //   owner wondering whether it ever existed.
                                         opacity: (v && !webServer) ? 0.4 : 1 }]}>
                  <Text style={{ color: advanced === v ? C.green : C.gold, fontFamily: F, fontSize: 14 }}>
                    {v ? 'Advanced' : 'Simple'}
                  </Text>
                  <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 4 }]}>
                    {v ? 'Shared, managed, public' : 'Plug in and share'}
                  </Text>
                </TouchableOpacity>
              ))}
            </View>
            <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 6 }]}>
              {!webServer
                ? 'Advanced needs the web client: the admin page — the connection log, '
                  + 'per-address monitoring and banning — is served to a browser, so with the '
                  + 'web client off there is no way to reach any of it. Turn it back on to choose '
                  + 'Advanced.'
                : advanced
                ? 'Adds shared listening, tuning and gain limits, and the admin page — '
                  + 'per-address monitoring, banning and a connection log, reachable from a browser '
                  + 'wherever you are. An admin password is REQUIRED: that page can ban people and '
                  + 'change your radio.'
                : 'One listener at a time with the whole radio, and nothing to decide beyond the '
                  + 'settings above. Choose Advanced to share it between several people, or to '
                  + 'limit where they may tune.'}
            </Text>


            {/* ─── RADIO SETTINGS ────────────────────────────────────────────────────────────
                ★★ The group heading matters as much as the contents: this screen was eighteen
                   sections in one scroll with nothing to say which belonged to the SERVER and
                   which to the RADIO in front of it. */}
            <Text style={[styles.section, { color: C.gold, fontFamily: F, marginTop: 22 }]}>
              RADIO SETTINGS
            </Text>

            {/* ★★★ IN ADVANCED, THIS QUESTION COMES FIRST BECAUSE IT DECIDES THE REST. A shared
                radio has a locked range and a listener count; a single-user one has neither and
                gets the allow/block lists instead. Asked last, as the web page once did, the
                owner answers questions the next answer makes irrelevant. */}
            {advanced && (<>
              <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>HOW WILL IT BE USED?</Text>
              <View style={{ flexDirection: 'row', gap: 8 }}>
                {/* ★★★ NAMED FOR WHAT ACTUALLY DIFFERS: whether the centre is pinned, and so
                    whether listeners share one dial or each get their own VFO. The old names
                    described a USER COUNT, which is the separate question below — and that made
                    the multi-listener UNLOCKED receiver (the FM-DX one) look like a third mode it
                    never was (Stuart, 2026-08-20). Same words as the browser's setup page. */}
                {([['single', 'Unlocked radio — one dial', 'It really retunes; 1 or many listeners'],
                   ['locked', 'Locked centre — own VFOs', 'You set the window, they tune inside it']] as const)
                  .map(([v, title, sub]) => (
                  <TouchableOpacity key={v}
                    onPress={() => { setRadioUse(v); AsyncStorage.setItem(K.radioUse, v); }}
                    style={[styles.card, { flex: 1, borderColor: radioUse === v ? C.green : C.border,
                                           backgroundColor: radioUse === v ? C.green + '18' : 'transparent' }]}>
                    <Text style={{ color: radioUse === v ? C.green : C.gold, fontFamily: F, fontSize: 13 }}>
                      {title}
                    </Text>
                    <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 4 }]}>{sub}</Text>
                  </TouchableOpacity>
                ))}
              </View>
            </>)}

            {/* ★★ WHERE A LISTENER LANDS — not hardware, so it applies to either radio. Same name
                as the macOS settings window uses, so an owner who runs both meets one idea once. */}
            {radioUse === 'locked' && centreBlock}
            {radioUse === 'locked' && rateBlock}
            <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>WHERE LISTENERS START</Text>
            <View style={{ flexDirection: 'row', gap: 8, alignItems: 'center' }}>
              <TextInput value={landingHz ? String(Math.round(landingHz / 1000)) : ''}
                onChangeText={(v) => { const hz = Math.round((Number(v.replace(/[^0-9.]/g, '')) || 0) * 1000);
                                       setLandingHz(hz); AsyncStorage.setItem(K.landingHz, String(hz)); }}
                placeholder="kHz — blank leaves it to the server" placeholderTextColor={C.goldDim}
                keyboardType="numeric"
                style={[styles.input, { color: C.amber, borderColor: C.border, fontFamily: F, flex: 1 }]} />
            </View>
            <View style={{ flexDirection: 'row', gap: 8, marginTop: 8, flexWrap: 'wrap' }}>
              {(['wfm', 'nfm', 'am', 'usb', 'lsb'] as const).map(m => (
                <TouchableOpacity key={m}
                  onPress={() => { setLandingMode(m); AsyncStorage.setItem(K.landingMode, m); }}
                  style={[styles.card, { borderColor: landingMode === m ? C.green : C.border,
                                         backgroundColor: landingMode === m ? C.green + '18' : 'transparent',
                                         paddingVertical: 8, paddingHorizontal: 12 }]}>
                  <Text style={{ color: landingMode === m ? C.green : C.textDim, fontFamily: F, fontSize: 12 }}>
                    {m.toUpperCase()}
                  </Text>
                </TouchableOpacity>
              ))}
            </View>
            <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 6 }]}>
              Where a listener is tuned when they first open the page. Their browser remembers where
              they were after that, so this mostly affects first-time visitors.
            </Text>

            {isRtl ? (<>
            {/* ★★★ THE GAIN THE RADIO STARTS AT, and returns to when the last listener leaves.
                It was buried in Advanced beside the gain LIMITS, which is a different idea —
                limits are what a listener may not exceed, this is where the radio sits by default.
                Simple mode needs it more than Advanced does: set it here and the receiver is right
                the moment it goes live, with no client attached to fix it afterwards.
                ★ One control, one value — it was not duplicated into Advanced, because two fields
                  writing one setting is how they end up disagreeing. */}
            {/* ★★★ A SLIDER, NOT A TYPING BOX. Tuner gain is not a continuous number — an R820T
                offers 29 fixed steps and nothing between them — so a text field invites a value
                the radio cannot take and then silently snaps it somewhere else. The same mistake
                was made on the Pi's setup page and fixed there for the same reason (Stuart,
                2026-08-17: "we had this issue before on the Pi").
                ★★ GainSlider is the control the hardware panel already uses: it snaps to the
                   supported list, shows the value, and owns the AUTO position — so the server
                   screen and the listener's panel behave identically rather than being two
                   different ideas of the same setting. */}
            <GainSlider
              gains={RTL_GAINS}
              gainTenthDb={restGain < 0 ? 157 : restGain}
              auto={restGain < 0}
              onAuto={(a) => { const v = a ? -1 : 157;
                               setRestGain(v); AsyncStorage.setItem(K.restGain, String(v)); }}
              onGain={(t) => { setRestGain(t); AsyncStorage.setItem(K.restGain, String(t)); }}
              modeButtons={false}
              label="STARTING GAIN" />
            <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 6 }]}>
              {rtlAgc
                ? 'Where the AGC starts. It moves up and down from here as the band demands.'
                : 'Where the radio sits before anyone connects, and where it returns when they leave — '
                  + 'so somebody who winds it up does not leave it up for the next person.'}
            </Text>

            {/* ★★★ ONE SLIDER, TWO MEANINGS — Stuart, 2026-08-21: "the current return to/starting
                gain slider applies in manual mode and with overload protection on it raises and
                lowers to the gain a listener sets at the time, in AGC mode that same slider just
                becomes the starting gain and the AGC will either increase or decrease as it sees
                fit." So the slider above never moves; only what it MEANS changes, and the hint
                changes with it.
                ★★ The dongle is the only radio that needs any of this: the Airspy HF+ and the
                   SDRplay have their own AGC and their own lock. */}

            <View style={[styles.rowBetween, { marginTop: 12 }]}>
              <Text style={[styles.value, { color: C.amber, fontFamily: F, flex: 1, paddingRight: 12 }]}>
                VibeAGC
              </Text>
              <Switch value={rtlAgc} onValueChange={setRtlAgc}
                trackColor={{ false: C.border, true: C.green }} thumbColor={C.amber} />
            </View>
            <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 6 }]}>
              VibeAGC — VibeSDR's own AGC for RTL-SDR. Lets the radio use its whole gain range, starting
              from the figure above: it measures how close the signal is to overloading the
              converter and moves the tuner a step at a time.{'\n\n'}
              This is NOT the dongle's built-in AGC — that one is unreliable and known broken on
              the RTL-SDR Blog v4, and VibeServer never uses it. Anything you have read about RTL
              automatic gain being no good is about the hardware one, not this.
            </Text>

            {/* ★★★ THE LOCK BELONGS UNDER THE THING IT LOCKS. It lived in GAIN LIMITS, in Advanced,
                which is a different subject entirely — an owner who had just switched the AGC on
                had no reason to look there, and the two settings only make sense read together.
                Stuart, 2026-08-21: "it should be on with the ability to lock it on underneath it."
                ★★ It only means anything while the AGC is ON, so it is disabled and dimmed when it
                   is off rather than offered as a switch that governs nothing — the same rule as
                   everywhere else here. */}
            <View style={[styles.rowBetween, { marginTop: 12, opacity: rtlAgc ? 1 : 0.4 }]}>
              <Text style={[styles.value, { color: C.amber, fontFamily: F, flex: 1, paddingRight: 12 }]}>
                Lock VibeAGC on
              </Text>
              <Switch value={agcLock && rtlAgc} disabled={!rtlAgc}
                onValueChange={(v) => { setAgcLock(v); AsyncStorage.setItem(K.agcLock, v ? '1' : '0'); }}
                trackColor={{ false: C.border, true: C.green }} thumbColor={C.amber} />
            </View>
            {/* ★★ THE OTHER SETTING AN OWNER SETS ONCE AND EXPECTS TO STAY SET, and the phone's
                   half of the web setup page's "IF filter follows the zoom" — the two GUIs are
                   meant to offer the same things (see memory/android_server_gui_parity). RTL only:
                   the R820T has a real IF filter and the other radios have nothing like it. */}
            <View style={[styles.rowBetween, { marginTop: 12 }]}>
              <Text style={[styles.value, { color: C.amber, fontFamily: F, flex: 1, paddingRight: 12 }]}>
                IF filter follows the zoom
              </Text>
              <Switch value={tunerBwAuto}
                onValueChange={(v) => { setTunerBwAuto(v); AsyncStorage.setItem(K.tunerBwAuto, v ? '1' : '0'); }}
                trackColor={{ false: C.border, true: C.green }} thumbColor={C.amber} />
            </View>
            <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 6 }]}>
              The tuner has a real IF filter, and it is the only selectivity ahead of the mixer.
              Narrowing it as somebody zooms into a station keeps strong neighbours out of the
              front end, where cross-modulation is made — it widens again automatically when they
              zoom out. Only for a free-tuning receiver: on a locked-frequency one you choose
              selectivity with the sample rate instead, once, at setup.
            </Text>
            <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 6 }]}>
              {rtlAgc
                ? 'Listeners get the AGC already on and can leave it on, but cannot switch it off '
                  + '— the same promise the SDRplay and Airspy locks have always kept. Without '
                  + 'this, any listener can turn it off and set their own gain, which on a shared '
                  + 'receiver changes it for everybody.'
                : 'Turn the AGC on above to lock it.'}
            </Text>
            </>) : (
              <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 12 }]}>
                {isAhf
                  ? 'The Airspy HF+ sets its own gain and its AGC stays on, so there is no starting '
                    + 'gain to choose and nothing to lock. It has no tuner IF filter either — '
                    + 'selectivity comes from the sample rate.'
                  : 'The HackRF has no AGC at all — its gain is two manual stages (LNA and VGA) that '
                    + 'a listener sets from the hardware panel, so there is no starting gain, no '
                    + 'AGC to lock, and no tuner IF filter to follow the zoom.'}
              </Text>
            )}

            {radioUse !== 'locked' && rateBlock}

            {/* ★★★ HARDWARE, AND ONLY WHERE THERE IS ANY. See isAhf: the HF+ has no rate to choose,
                no bias-T and a locked AGC, so it gets a sentence instead of a panel of dead
                switches. */}
            {hasHwSetup ? (<>
              <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>BIAS-T</Text>
              <TouchableOpacity onPress={() => { const v = !biasT; setBiasT(v);
                                                 AsyncStorage.setItem(K.biasT, v ? '1' : '0'); }}
                style={[styles.card, { borderColor: biasT ? C.green : C.border,
                                       backgroundColor: biasT ? C.green + '18' : 'transparent' }]}>
                <Text style={{ color: biasT ? C.green : C.gold, fontFamily: F, fontSize: 14 }}>
                  {biasT ? 'ON — DC on the feedline' : 'OFF'}
                </Text>
              </TouchableOpacity>
              <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 6 }]}>
                Powers an antenna amplifier or an active loop. Leave it off for anything else — DC
                into the wrong antenna can damage it.
              </Text>
            </>) : (
              <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 6 }]}>
                The Airspy HF+ has nothing to set up here — one fixed sample rate, no bias-T, and
                its AGC stays on.
              </Text>
            )}

            {/* ★★ TIME LIMIT — only earns its place on a PUBLIC receiver. */}
            <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>TIME LIMIT PER LISTENER</Text>
            {([[0, 'Unlimited'], [15, '15 minutes'], [30, '30 minutes'],
               [60, '1 hour'], [120, '2 hours']] as const).map(([v, label]) => (
              <OptRow key={v} C={C} F={F} active={limitMin === v} label={label}
                onPress={() => { setLimitMin(v); AsyncStorage.setItem(K.limitMin, String(v));
                                 if (runningRef.current) setVibeServerSessionLimit(v); }} />
            ))}
            <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 6 }]}>
              For a receiver you have put on the internet. This server serves ONE listener at a
              time, so without a limit the first person to connect can hold it all evening and
              everyone else just sees IN USE.{'\n\n'}
              A listener is warned at two minutes and again at thirty seconds, then disconnected
              with an explanation. Their address is then held off for two minutes — otherwise
              their client would simply reconnect and carry on, and the limit would achieve
              nothing.{'\n\n'}
              You are not affected: listening on this phone is exempt, and so is any session
              unlocked with the admin password. Leave it Unlimited for a private receiver.
            </Text>

            {/* ★★ WHAT THE LIMIT MEANS. Offered in BOTH modes, because a private receiver with a
                limit wants the same choice: soft turns the number into a guarantee rather than a
                deadline — you keep the radio past it until somebody else wants it, then get a few
                seconds' notice. Hard is what a limit has always meant here and stays the default. */}
            {limitMin > 0 && (
              <View style={{ flexDirection: 'row', gap: 8, marginTop: 10 }}>
                {([false, true] as const).map(v => (
                  <TouchableOpacity key={String(v)} onPress={() => setLimitSoft(v)}
                    style={[styles.card, { flex: 1, borderColor: limitSoft === v ? C.green : C.border,
                                           backgroundColor: limitSoft === v ? C.green + '18' : 'transparent' }]}>
                    <Text style={{ color: limitSoft === v ? C.green : C.gold, fontFamily: F, fontSize: 13 }}>
                      {v ? 'Soft limit' : 'Hard limit'}
                    </Text>
                    <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 4 }]}>
                      {v ? 'Kept until somebody else wants it' : 'Disconnected at the limit'}
                    </Text>
                  </TouchableOpacity>
                ))}
              </View>
            )}

            {/* ★★★ WHAT IS BOLTED TO THIS RADIO — ADVANCED ONLY. It exists to set a stranger's
                expectations on a landing page they are choosing from, and a Simple server is one
                person sharing a receiver with people who already know what is on the end of it
                (Stuart, 2026-08-19: "remove the antenna details as its not really needed in simple
                mode"). Same reasoning as everything else Advanced adds: it is about being read by
                somebody who has never seen your receiver. */}
            {advanced && (<>
            <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>ANTENNA</Text>
            <TextInput value={antenna} onChangeText={setAntenna}
              placeholder="e.g. Discone in the loft, good to 300 MHz"
              placeholderTextColor={C.textDim} maxLength={120}
              style={[styles.input, { color: C.amber, fontFamily: F, borderColor: C.border }]} />
            <View style={{ flexDirection: 'row', gap: 8, flexWrap: 'wrap', marginTop: 8 }}>
              {ANT_ICONS.map(a => (
                <TouchableOpacity key={a.key} onPress={() => setAntennaIcon(antennaIcon === a.key ? '' : a.key)}
                  style={[styles.card, { borderColor: antennaIcon === a.key ? C.green : C.border,
                                         backgroundColor: antennaIcon === a.key ? C.green + '18' : 'transparent',
                                         paddingHorizontal: 12, paddingVertical: 8 }]}>
                  <Text style={{ color: antennaIcon === a.key ? C.green : C.gold, fontFamily: F, fontSize: 12 }}>
                    {a.label}
                  </Text>
                </TouchableOpacity>
              ))}
            </View>
            <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 6 }]}>
              Shown to everybody who visits, so keep it about the aerial — not about where you
              live. Tap a picture again to choose none; a radio without one simply shows the
              description.
            </Text>
            </>)}

            {/* ★★★ POWER DOWN WHEN NOBODY IS LISTENING — ON by default, and it never lets the
                dongle go. The capture parks so the phone stops burning power on a radio nobody is
                hearing; the device stays CLAIMED, so it starts again instantly.
                ★★ NOT the Linux "release when idle", which hands the dongle to another program:
                   Android's permission model means nothing else can pick it up anyway, so
                   releasing would cost the restart and buy nothing (Stuart, 2026-08-19). */}
            <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>WHEN NOBODY IS LISTENING</Text>
            <View style={[styles.card, { borderColor: C.border }]}>
              <View style={styles.rowBetween}>
                <Text style={[styles.value, { color: C.amber, fontFamily: F, flex: 1, paddingRight: 12 }]}>
                  Power down the radio
                </Text>
                <Switch value={idleGrace > 0}
                  onValueChange={(v) => { setIdleGrace(v ? 300 : 0); if (v) setSpectrogram(false); }}
                  trackColor={{ false: C.border, true: C.green }} thumbColor={C.amber} />
              </View>
              <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 8 }]}>
                {idleGrace > 0
                  ? 'After five minutes with nobody connected the radio stops capturing and draws '
                    + 'much less power. It is never unplugged or handed away, so the next listener '
                    + 'starts it again immediately.'
                  : 'The radio keeps capturing whether or not anybody is listening — warmer phone, '
                    + 'flatter battery, and the only way to draw the 24-hour spectrogram.'}
              </Text>
            </View>

            {/* ★★ THE SPECTROGRAM AND THE POWER SAVER CANNOT BOTH BE ON, and the reason is
                physical rather than a rule: a radio that stops capturing cannot picture a band it
                is not listening to. Same on Linux. */}
            {idleGrace === 0 && advanced && radioUse === 'locked' && (
              <View style={[styles.card, { borderColor: C.border, marginTop: 10 }]}>
                <View style={styles.rowBetween}>
                  <Text style={[styles.value, { color: C.amber, fontFamily: F, flex: 1, paddingRight: 12 }]}>
                    Draw the 24-hour spectrogram
                  </Text>
                  <Switch value={spectrogram} onValueChange={setSpectrogram}
                    trackColor={{ false: C.border, true: C.green }} thumbColor={C.amber} />
                </View>
                <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 8 }]}>
                  The landing page shows what this receiver has actually been hearing all day —
                  which says more about whether it is worth connecting to than any description.
                  Only possible while the radio keeps capturing.
                </Text>
              </View>
            )}

            {/* ★★★ THE ADVANCED SECTIONS. Everything from here down is applied to the radio this
                app is already running — there is no second process and no second port. */}
            {advanced && (
              <>
                {/* ★★★ THE LISTENER WHO WENT AWAY. Shared radios only — a one-at-a-time receiver
                    has nobody to reclaim the slot for, which is why this sits inside `advanced`
                    AND behind the listener count.
                    ★★ It ASKS before it acts: "no interaction" is not "not listening", and
                       somebody sitting on one frequency for an hour is the best listener there
                       is. Watching a decoder counts as using the radio, so a weather-fax image is
                       never interrupted. */}
                {radioUse === 'locked' && (<>
                <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>IDLE LISTENERS</Text>
                <View style={[styles.card, { borderColor: C.border }]}>
                  <View style={{ flexDirection: 'row', gap: 8, flexWrap: 'wrap' }}>
                    {[0, 15, 30, 60].map(n => (
                      <TouchableOpacity key={n} onPress={() => setIdleKick(n)}
                        style={[styles.card, { borderColor: idleKick === n ? C.green : C.border,
                                               backgroundColor: idleKick === n ? C.green + '18' : 'transparent',
                                               paddingHorizontal: 14 }]}>
                        <Text style={{ color: idleKick === n ? C.green : C.gold, fontFamily: F, fontSize: 14 }}>
                          {n === 0 ? 'Off' : `${n} min`}
                        </Text>
                      </TouchableOpacity>
                    ))}
                  </View>
                  <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 8 }]}>
                    Release a listener who has stopped using the radio, so somebody else can have
                    it. They are ASKED first — a "still listening?" prompt with a countdown, and
                    anything they do answers it.{'\n\n'}
                    Somebody watching a decoder is never interrupted: a weather-fax image takes ten
                    minutes to draw and RTTY runs for hours.{'\n\n'}
                    Fifteen minutes is the shortest offered — long enough to hear a block of music
                    before the ad break. Off by default.
                  </Text>
                </View>
                </>)}
                {/* ★★★ HOW MANY — ON EITHER KIND OF RECEIVER, and on an unlocked one this is the
                    control that turns it into an FM-DX receiver. It used to live inside the
                    locked-range block, so an unlocked radio was stuck at one listener and the
                    shared dial had no switch at all (Stuart, 2026-08-20: "you forgot the most
                    important control"). The browser's setup page had the identical fault. */}
                <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>HOW MANY LISTENERS</Text>
                {/* ★★★ A NUMBER THE OWNER TYPES, not six the app happens to offer. The chips were
                    1/2/3/4/6/8, so a receiver that could serve ten could not be TOLD ten — and the
                    figure is a judgement about somebody's UPLINK, which no fixed list can guess.
                    The browser's setup page has always had a box here; this is the same control
                    (Stuart, 2026-08-21: "User Limit needs to be an actual type box not an
                    arbitury 2/4/6/8 selection").
                    ★ Held as TEXT while editing so the field can be empty mid-type — parsing on
                      every keystroke would turn a cleared box into "1" under the user's fingers. */}
                <TextInput value={usersText}
                  onChangeText={(v) => {
                    const digits = v.replace(/[^0-9]/g, '').slice(0, 3);
                    setUsersText(digits);
                    const n = Number(digits);
                    if (n >= 1) setMaxUsers(Math.min(64, n));
                  }}
                  onBlur={() => {
                    // ★ Empty or 0 is not a receiver anybody can use — settle on 1 when the field
                    //   is left, rather than refusing the keystroke while they are still typing.
                    const n = Math.max(1, Math.min(64, Number(usersText) || 1));
                    setMaxUsers(n); setUsersText(String(n));
                  }}
                  keyboardType="number-pad" placeholder="1" placeholderTextColor={C.goldDim}
                  style={[styles.input, { color: C.amber, borderColor: C.border, fontFamily: F }]} />
                {/* ★★ WHAT IT COSTS, in the units that actually run out. The DSP is nearly free —
                    listeners share one FFT — and it is the UPLINK that decides how many a phone
                    can really serve. Same arithmetic as the browser's bwNote(). */}
                <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 6 }]}>
                  About {(maxUsers * (uncomp !== 0 ? 2.0 : 0.2)).toFixed(1)} Mb/s of your upload with{' '}
                  {maxUsers} listener{maxUsers === 1 ? '' : 's'} connected
                  {uncomp !== 0
                    ? ', because uncompressed audio is switched on — roughly ten times the '
                      + 'compressed cost.'
                    : '.'}
                </Text>
                {/* ★★★ WHAT THE NUMBER MEANS DEPENDS ON THE MODE ABOVE, and the difference is the
                    whole feature: pinned centre = everybody tunes independently; unpinned = one
                    dial everybody hears, which is how FM-DX receivers work. Said here because
                    this is where the owner is standing when it matters. */}
                <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 6 }]}>
                  {maxUsers === 1
                    ? 'One listener at a time, each with the full settings surface — the second '
                      + 'person sees IN USE and waits. Right for a receiver you mostly use yourself.'
                    : radioUse === 'locked'
                    ? `Up to ${maxUsers} listeners share the radio, each with their own tuning inside `
                      + 'what the radio is receiving.\n\nThe extra DSP is close to nothing — they '
                      + 'share one FFT. What actually runs out is UPLINK, so on a phone this is a '
                      + 'question about your connection, not about the handset.'
                    : `ONE DIAL, SHARED. Up to ${maxUsers} listeners hear the same frequency and `
                      + 'anybody may move it — the way FM-DX receivers work. They get a small set '
                      + 'of fixed messages ("Can I tune?", "Please hold — chasing DX") to agree who '
                      + 'goes next; there is no free text and nobody has a name, so there is nothing '
                      + 'to moderate.\n\nOne VFO means the DSP cost barely moves with the number of '
                      + 'listeners. What runs out is UPLINK.'}
                </Text>

                {/* ★★ LOCKED-RANGE ONLY, and it is the setting that DEFINES that mode: pinning the
                    centre is what gives every listener their own VFO inside one captured window.
                    An unlocked radio has no window to pin — it retunes — which is why the listener
                    count above now sits outside this block and this does not. */}
                {radioUse === 'locked' && (<>
                {/* ★★★ REAL BINS AT DEEP ZOOM. Without it a shared receiver interpolates, and the
                    waterfall turns to blocks the moment anybody zooms in — which a listener reads
                    as a poor receiver rather than a setting. */}
                <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>ZOOM DETAIL</Text>
                <View style={[styles.card, { borderColor: C.border }]}>
                  <View style={styles.rowBetween}>
                    <Text style={[styles.value, { color: C.amber, fontFamily: F, flex: 1, paddingRight: 12 }]}>
                      {/* ★★★ NOT "KA9Q" — that is OUR internal name for the technique, and it
                          means nothing to an owner reading this screen. It named the implementation
                          rather than the effect, which is the one thing a label must not do
                          (Stuart, 2026-08-21). Worded as the browser's setup page words it. */}
                      Keep the spectrum sharp when zoomed in
                    </Text>
                    <Switch value={zoomSpec} onValueChange={setZoomSpec}
                      trackColor={{ false: C.border, true: C.green }} thumbColor={C.amber} />
                  </View>
                  <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 8 }]}>
                    {zoomSpec
                      ? 'Recomputes real detail as listeners zoom, instead of magnifying what is '
                        + 'already on screen. Uses slightly more CPU, and is what makes a shared '
                        + 'receiver worth zooming into.'
                      : 'Without it a close-in view goes blocky — the wide picture is magnified '
                        + 'rather than recomputed.'}
                  </Text>
                </View>

                </>)}


                {/* ★★★ NOT IN LOCKED-RANGE MODE. The window IS the limit there, so an allow/block
                    list is a second answer to a question already answered — and two ways to say
                    the same thing is how they end up disagreeing. The web setup page gates it the
                    same way, for the same reason. */}
                {radioUse === 'single' && (<>
                <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>WHERE LISTENERS MAY TUNE</Text>
                {/* ★★ PICK A BAND OR TYPE A RANGE — the browser's shape, and the same string on
                    the wire. The free-text box asked the owner to remember a syntax and told them
                    nothing until the server refused it. */}
                <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 4 }]}>
                  Allowed — only these, if you list any
                </Text>
                <BandLimitEditor C={C} F={F} bands={bands} kind="range"
                  value={allowRanges}
                  onChange={(v) => { setAllowRanges(v); AsyncStorage.setItem(K.allowRanges, v); }}
                  placeholder="or 87.5M-108M"
                  emptyText="Empty — this radio can tune anywhere it hears." />
                <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 12 }]}>
                  Blocked — never these
                </Text>
                <BandLimitEditor C={C} F={F} bands={bands} kind="range"
                  value={blockRanges}
                  onChange={(v) => { setBlockRanges(v); AsyncStorage.setItem(K.blockRanges, v); }}
                  placeholder="or 108M-137M"
                  emptyText="Empty — nothing is blocked." />
                <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 6 }]}>
                  Band names or frequency pairs, comma separated — &quot;fm&quot;, &quot;airband&quot;,
                  &quot;88M-108M&quot;. Blocked always wins over allowed.{'\n\n'}
                  You are exempt: listening on this phone, and any session unlocked with the admin
                  password, tune anywhere the radio can hear.
                </Text>
                </>)}

                {/* ★★★ NOT IN LOCKED-RANGE MODE. A pinned window is ONE band, so a per-band
                    ceiling has no second band to differ from — the whole receiver takes the one
                    gain. The web page hides it in this mode for the same reason (Stuart,
                    2026-08-21: "per band gain isnt needed in this mode as it is a locked range").*/}
                {radioUse === 'single' && (<>
                <Text style={[styles.section, { color: C.textDim, fontFamily: F }]}>PER-BAND CEILINGS</Text>
                <BandLimitEditor C={C} F={F} bands={bands} kind="gain" allowAll
                  gainSteps={RTL_GAINS}
                  value={gainLimits}
                  onChange={(v) => { setGainLimits(v); AsyncStorage.setItem(K.gainLimits, v); }}
                  /* ★★ THE LOCK, PER BAND — the phone's half of the setup page's "Lock this band".
                       Pick "All bands" and lock that and the whole radio is fixed, which is how a
                       radio-wide lock is spelled: "all bands" was always just another band. */
                  lockable
                  locks={gainLocks}
                  onLocksChange={(v) => { setGainLocks(v); AsyncStorage.setItem(K.gainLocks, v); }}
                  /* ★★★ THE HACKRF NEEDS A SECOND NUMBER TO BE SET WITH. LNA + VGA = 30 dB describes
                       a family of radios, not one — so a FIXED HackRF band carries where between the
                       two stages that total sits. Only on a HackRF, and only when the band is being
                       locked: as a limiter the listener still chooses their own split, which is
                       today's behaviour and stays. The SDRplay's IF ceiling has no equivalent here
                       on purpose — there is no SDRplay on Android, and a control with no radio to
                       act on is one to leave out rather than ship inert (AGENTS.md). */
                  splittable={radio?.driver === 'hackrf'}
                  splits={gainSplits}
                  onSplitsChange={(v) => { setGainSplits(v); AsyncStorage.setItem(K.gainSplits, v); }}
                  placeholder="max, e.g. 25 dB"
                  emptyText="No ceilings — listeners have the full range." />
                <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 6 }]}>
                  Cap the bands that overload and leave the rest open — a strong local FM
                  transmitter is the usual reason, while HF wants everything the radio has.
                  &quot;all&quot; caps everywhere; a tighter per-band ceiling still wins.{'\n\n'}
                  Lock a band and its ceiling becomes the SETTING: the gain sits exactly there and
                  listeners get no gain controls at all on it. Leave it unlocked and they keep the
                  controls and simply cannot go past it — so FM can be held at one figure while HF
                  stays adjustable underneath a ceiling.{'\n\n'}
                  The return gain is applied when the LAST listener leaves, so somebody who turns it
                  up does not leave it up for the next person. Tuning into a capped band brings the
                  gain down automatically.
                </Text>
                </>)}

              </>
            )}

            {/* ★★★ WARN WHILE SERVING, AND ONLY THEN. The phone is a SIMPLE-mode server: plug in
                and share, which is the behaviour people like, so a password is NOT forced here.
                But an owner who never opened this screen met the consequence as a stranger's
                demand for a password they had never set (issue #19, on the Mac, where one was
                being generated silently). Nothing is generated anywhere now — so the honest thing
                is to say plainly, at the moment it is true, that this receiver is unprotected.
                ★ Advice, not an error: it is a perfectly reasonable way to run a radio on your own
                  network, and the colour says "read this", not "you did something wrong". */}
            {running && !adminPw.trim() && (
              <View style={[styles.card, { borderColor: C.goldDim, marginTop: 14 }]}>
                <Text style={{ color: C.amber, fontFamily: F, fontSize: 13 }}>
                  No admin password set
                </Text>
                <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 6 }]}>
                  Recommended if you are on a PUBLIC NETWORK, or intend to port forward this and
                  serve it to the internet — anyone who can reach the server can otherwise change
                  the gain, switch on the bias-T or alter the calibration. On your own home network
                  that is usually nobody but you, and no password is needed.{'\n\n'}
                  A listening PIN above is the other half: the PIN decides who may LISTEN, the
                  password decides who may CHANGE the radio.{'\n\n'}
                  Serving to the internet? Switch to ADVANCED at the top. It is built for it — an
                  admin password is required there, and it adds the controls that go with being
                  public: per-address monitoring and banning, tuning and gain limits, and a
                  connection log you can read from a browser wherever you are.
                </Text>
              </View>
            )}

            {error && (
              <View style={[styles.card, { borderColor: C.red, marginTop: 14 }]}>
                <Text style={{ color: C.red, fontFamily: F, fontSize: 14 }}>{error}</Text>
              </View>
            )}

            {/* ★ SAY THERE IS NO SAVE BUTTON. Every setting here persists the moment it is
                changed and START applies them — but a settings screen with no Save invites the
                reasonable worry that nothing was kept (Stuart: "there is no save button but I
                assumed pressing start server was the save button", 2026-07-27). He was right,
                and having to assume is the problem. */}
            <Text style={[styles.hint, { color: C.textDim, fontFamily: F, marginTop: 16, textAlign: 'center' }]}>
              Settings are saved as you change them. Starting the server applies them.
            </Text>
            {/* ★★★ FULL MODE WILL NOT START WITHOUT AN ADMIN PASSWORD (Stuart, 2026-08-12). Not a
                generated one — issue #19 was a stranger being asked for a password that had been
                minted silently and shown to nobody. The owner types it or Full mode does not run.
                ★★ The button says WHY it is unavailable. A greyed-out control with no reason reads
                   as a broken app, and the reason is two lines above where nobody rereads. */}
            {/* ★★★ IT REFUSES WITH A REASON, rather than greying out. A disabled button explains
                nothing: the owner cannot tell "not allowed yet" from "this app is broken", and the
                remedy is a section away. Pressing it says what to do (Stuart, 2026-08-12: "it will
                give an error stating to setup an admin password first"). */}
            {/* ★★ THE BUTTON IS THE INSTRUCTION. Red and saying what is missing, rather than
                   green-and-refusing or greyed-out-and-silent: a disabled control cannot be told
                   apart from a broken app, and an error raised on tap puts the reason at the top
                   of the screen while the remedy is at the bottom. Stuart, 2026-08-17: "keep the
                   green serve now button red with the text set an admin password to serve".
                ★ Still pressable, and pressing it says where to go — the password field is far
                  enough down that "set one" is not obviously actionable on a phone. */}
            <TouchableOpacity
              style={[styles.startBtn, needsAdminPw
                ? { borderColor: C.red,   backgroundColor: C.red   + '18' }
                : { borderColor: C.green, backgroundColor: C.green + '18' }]}
              onPress={() => {
                if (needsAdminPw) {
                  setError('Set an admin password below, then Start. It is what stops a listener '
                         + 'reaching the admin page and changing your radio — and it is what '
                         + 'makes this server safe to put on the internet.');
                  return;
                }
                void start();
              }} disabled={starting}>
              {starting
                ? <ActivityIndicator color={C.green} />
                : <Text style={{ color: needsAdminPw ? C.red : C.green, fontFamily: F, fontSize: 16 }}>
                    {needsAdminPw ? 'Set an admin password to serve' : '▶ Start VibeServer'}
                  </Text>}
            </TouchableOpacity>
          </>
        ) : (
          <TouchableOpacity style={[styles.startBtn, { borderColor: C.amber, backgroundColor: C.amber + '18' }]}
            onPress={() => navigation.replace('RtlTcpServer', { name: name.trim() || 'VibeSDR RTL-SDR', advertise })}>
            <Text style={{ color: C.amber, fontFamily: F, fontSize: 16 }}>▶ Start RTL-TCP server</Text>
          </TouchableOpacity>
        )}

        <TouchableOpacity style={[styles.stopBtn, { borderColor: C.border }]} onPress={() => navigation.goBack()}>
          <Text style={{ color: C.gold, fontFamily: F, fontSize: 15 }}>‹ Cancel</Text>
        </TouchableOpacity>
      </ScrollView>
    </SafeAreaView>
  );
}

function Row({ C, F, k, v, vc }: any) {
  return (
    <View style={styles.rowBetween}>
      <Text style={[styles.label, { color: C.textDim, fontFamily: F }]}>{k}</Text>
      <Text style={[styles.value, { color: vc, fontFamily: F }]}>{v}</Text>
    </View>
  );
}

function ProtoCard({ C, F, active, onPress, title, tag, desc }: any) {
  return (
    <TouchableOpacity onPress={onPress}
      style={[styles.protoCard, { borderColor: active ? C.amber : C.border, backgroundColor: active ? C.amber + '14' : 'transparent' }]}>
      <View style={styles.rowBetween}>
        <Text style={{ color: active ? C.amber : C.gold, fontFamily: F, fontSize: 17 }}>{title}</Text>
        <Text style={{ color: active ? C.amber : C.goldDim, fontFamily: F, fontSize: 11 }}>{tag}</Text>
      </View>
      <Text style={{ color: C.textDim, fontFamily: F, fontSize: 12, lineHeight: 16, marginTop: 6 }}>{desc}</Text>
    </TouchableOpacity>
  );
}

function Pill({ C, F, active, label, onPress }: any) {
  return (
    <TouchableOpacity onPress={onPress}
      style={[styles.pill, { borderColor: active ? C.amber : C.border, backgroundColor: active ? C.amber + '22' : 'transparent' }]}>
      <Text style={{ color: active ? C.amber : C.gold, fontFamily: F, fontSize: 14 }}>{label}</Text>
    </TouchableOpacity>
  );
}

function OptRow({ C, F, active, label, hint, onPress }: any) {
  return (
    <TouchableOpacity style={[styles.optRow, { borderColor: active ? C.amber : C.border }]} onPress={onPress}>
      {/* ★ The label column must FLEX and the tick stay fixed — a three-way policy choice
          needs a sentence to be choosable at all, and without the flex a long hint pushes
          the tick off the row. */}
      <View style={{ flex: 1, paddingRight: 10 }}>
        <Text style={{ color: active ? C.amber : C.gold, fontFamily: F, fontSize: 15 }}>{label}</Text>
        {!!hint && (
          <Text style={{ color: C.textDim, fontFamily: F, fontSize: 11, marginTop: 3, lineHeight: 15 }}>
            {hint}
          </Text>
        )}
      </View>
      {active && <Text style={{ color: C.amber, fontFamily: F }}>✓</Text>}
    </TouchableOpacity>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1 },
  h1: { fontSize: 24, marginBottom: 6 },
  sub: { fontSize: 13, lineHeight: 18, marginBottom: 16 },
  card: { borderWidth: 1, borderRadius: 10, padding: 14, marginBottom: 8, gap: 10 },
  protoCard: { borderWidth: 1, borderRadius: 10, padding: 14, marginBottom: 10 },
  rowBetween: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center' },
  label: { fontSize: 11, letterSpacing: 1 },
  value: { fontSize: 15, flexShrink: 1, textAlign: 'right', marginLeft: 8 },
  section: { fontSize: 11, letterSpacing: 1, marginTop: 16, marginBottom: 6 },
  input: { borderWidth: 1, borderRadius: 8, paddingHorizontal: 12, paddingVertical: 10, fontSize: 16 },
  regen: { borderWidth: 1, borderRadius: 8, paddingHorizontal: 14, paddingVertical: 8, marginLeft: 8 },
  hint: { fontSize: 11.5, lineHeight: 16, marginTop: 6 },
  pillRow: { flexDirection: 'row', gap: 8, marginBottom: 8 },
  pill: { borderWidth: 1, borderRadius: 8, paddingHorizontal: 16, paddingVertical: 9 },
  optRow: {
    borderWidth: 1, borderRadius: 8, paddingHorizontal: 14, paddingVertical: 12,
    marginBottom: 8, flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center',
  },
  startBtn: { borderWidth: 1, borderRadius: 10, paddingVertical: 15, alignItems: 'center', marginTop: 20 },
  stopBtn: { borderWidth: 1, borderRadius: 10, paddingVertical: 14, alignItems: 'center', marginTop: 12 },
});
