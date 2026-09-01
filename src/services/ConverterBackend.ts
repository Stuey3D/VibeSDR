/**
 * ConverterBackend.ts — the converter transform, applied at the ONE place the app meets the radio.
 *
 * ★★★ THE OFFSET IS NOT A DISPLAY SKIN, AND THAT IS THE WHOLE DESIGN DECISION.
 *
 *  The obvious reading of "an up-converter shifts the frequency" is to subtract the LO when
 *  DRAWING the readout. It gives the right number on screen and it is wrong everywhere else,
 *  because a great deal of this app reasons about frequency rather than merely printing it. With
 *  a display skin, while the converter is engaged: a bookmark saved at "3 MHz" stores whichever
 *  of the two numbers its call site happened to hold; the shared dial sends the other listener a
 *  dial reading 125 MHz from where you are; EIBI, the band plan, station lookup, RadioDNS, the
 *  VTS bar, Buddy and CarPlay each have to be taught the transform separately, and the one that
 *  is missed is a silent wrong answer. Toggling the converter off then silently reinterprets
 *  every bookmark saved while it was on.
 *
 *  So it is inverted: THE WHOLE APP SPEAKS TRUE RF, and the LO is applied only where the app
 *  talks to the tuner. Bookmarks store 3 MHz, EIBI matches 3 MHz, the watch is sent 3 MHz, and
 *  128 MHz exists nowhere above this file. Identical on screen; nothing else has to know a
 *  converter exists, and the engage toggle is safe to flip at any moment.
 *
 * ★★★ ONE PLACE IN EACH DIRECTION. Every frequency going DOWN to the radio passes through
 *  toHardware() here; every frequency coming UP passes through toDisplay() here. This is not
 *  tidiness — it is what keeps display Hz off the native bridge, which is uint32 throughout
 *  (vibe_localsdr_jni.cpp, RtlTcpServer::start, all of spyserver_protocol.h). Hardware
 *  frequencies are always under 2 GHz so nothing needs widening, but a display frequency that
 *  ever crossed it would WRAP silently and tune to garbage. See the note in converter.ts on why
 *  the LNB presets are not shipped yet.
 *
 * ★★ A DECORATOR, NOT A HUNDRED EDITS. Wrapping SDRBackend means SDRScreen (8,800 lines) and
 *  WaterfallView are untouched, and a call site added later is transformed by construction
 *  rather than by whoever adds it remembering.
 *
 * ★★★ BUT IT IS NOT THE ONLY DOOR TO THE RADIO, AND SAYING SO HERE ONCE WAS NOT ENOUGH. This
 *  file claimed a frequency could not reach the radio without passing through it. That was
 *  false: WatchSpectrumForwarder (iOS, native) opens its OWN socket to the server and sends its
 *  own `zoom` while the phone is locked and the watch is showing the waterfall. Handed display
 *  Hz it asked the radio for 3 MHz when the tuner needed 128, and its bin crop subtracted a
 *  HARDWARE centre (from the server's SPEC header) from a DISPLAY VFO and picked bins 125 MHz
 *  away. It now takes hardware Hz and the offset separately — the "one rule, two readers" fault
 *  this comment was congratulating itself on avoiding.
 * ★ SO: ANYTHING THAT TALKS TO THE RADIO WITHOUT GOING THROUGH SDRBackend MUST BE GIVEN THE
 *   OFFSET EXPLICITLY. Today that is the watch forwarder alone. Check for others before assuming
 *   a new native path is covered.
 *
 * ★★ THE PROFILE IS READ LIVE, never captured. The engage toggle has to take effect on a running
 *  session without rebuilding the backend — and when it flips, the TUNER HAS NOT MOVED, only the
 *  interpretation of it has, because the user unplugged something. So the hardware frequency is
 *  held and the display jumps (3 MHz → 128 MHz on bypass), which is provably always in range;
 *  holding the DISPLAY frequency and retuning the hardware would be wrong on the physics and can
 *  land outside caps.freqRange.
 *
 * ★ Bins are deliberately NOT transformed. They are derived from centerHz by their consumers, so
 *   transforming the centre transforms them; doing both would give a waterfall whose scale is
 *   right in the middle and progressively wrong towards the edges.
 */
import type { BackendCapabilities, SDRBackend } from './SDRBackend';
import type { SDRMode, SDRStatus } from './UberSDRClient';
import {
  type ConverterProfile, active, isIdentity, toDisplay, toHardware, displayRange,
} from './converter';

/* ★★★ AN OPTIONAL METHOD MUST STAY ABSENT WHEN THE INNER BACKEND HAS NONE.
 *   Half of SDRBackend is optional and the callers TEST FOR IT — `caps.profiles` gates the
 *   profile pill, `watchSpectrumUrl` decides whether the native locked-pocket forwarder can be
 *   used at all, `forceResubscribe` is the starvation watchdog's only lever. A class declaring
 *   them declares them PRESENT: every `if (c.watchSpectrumUrl)` would then pass on an adapter
 *   that has none, and the call inside would throw. So the wrapper is stripped of every optional
 *   its inner does not actually implement — the presence test keeps meaning what it meant. */
const OPTIONAL_MEMBERS = [
  'disconnectSocket', 'watchSpectrumUrl', 'forceResubscribe',
  'getProfiles', 'selectProfile', 'getSecondaryDecoder', 'sendChat',
] as const;

export function wrapWithConverter(
  inner: SDRBackend,
  getProfile: () => ConverterProfile,
): SDRBackend {
  const w = new ConverterBackend(inner, getProfile);
  const bag = w as unknown as Record<string, unknown>;
  const src = inner as unknown as Record<string, unknown>;
  for (const m of OPTIONAL_MEMBERS) {
    /* ★ SHADOWED WITH undefined, not deleted. These are PROTOTYPE methods, and `delete
     *   instance.method` removes an OWN property — of which there are none — so it silently does
     *   nothing and every optional stays present. An own `undefined` shadows the prototype and
     *   makes the `if (c.watchSpectrumUrl)` tests falsy, which is what they are for. */
    if (typeof src[m] !== 'function') bag[m] = undefined;
  }
  return w;
}

class ConverterBackend implements SDRBackend {
  constructor(
    private readonly inner: SDRBackend,
    private readonly getProfile: () => ConverterProfile,
  ) {}

  /** The transform in force RIGHT NOW — `active()` has already resolved a bypassed converter to
   *  the identity, so nothing below ever tests `engaged`. */
  private get c(): ConverterProfile { return active(this.getProfile()); }

  private up   = (hwHz: number): number => toDisplay(hwHz, this.c);
  private down = (dispHz: number): number => Math.round(toHardware(dispHz, this.c));

  /** Frequencies inside a status blob, on the way UP. `trueCenterHz` is the one the watch crop
   *  and the bin indexing read, so missing it would draw the signal offset from the VFO. */
  private upStatus = (s: SDRStatus): SDRStatus => {
    if (isIdentity(this.c)) return s;
    return {
      ...s,
      frequency: this.up(s.frequency),
      centerHz:  this.up(s.centerHz),
      ...(s.trueCenterHz !== undefined ? { trueCenterHz: this.up(s.trueCenterHz) } : {}),
    };
  };

  get kind() { return this.inner.kind; }
  get uuid() { return this.inner.uuid; }

  /** ★★ THE TUNABLE RANGE IS DERIVED, WHICH IS WHY THE USER NEVER ENTERS ONE. The converter's
   *  display range is just the dongle's own range under the same transform, so band edges, VFO
   *  clamping, drum limits and the pan walls all follow for free. */
  get caps(): BackendCapabilities {
    const inner = this.inner.caps;
    if (isIdentity(this.c)) return inner;
    return { ...inner, freqRange: displayRange(inner.freqRange, this.c) };
  }

  connect(frequency?: number, mode?: SDRMode, opts?: { allowServerDefault?: boolean }) {
    return this.inner.connect(frequency === undefined ? undefined : this.down(frequency), mode, opts);
  }
  destroy() { this.inner.destroy(); }
  disconnectSocket?(): void { this.inner.disconnectSocket?.(); }

  tune(frequency: number, mode?: SDRMode, opts?: { recenter?: boolean }) {
    this.inner.tune(this.down(frequency), mode, opts);
  }
  /** ★ Takes DISPLAY Hz like tune(), so the two halves of "the frequency changed" cannot disagree.
   *  The native audio echo that feeds this arrives in HARDWARE Hz and is converted at that
   *  boundary — see the VibeTuned listener — rather than being special-cased here. */
  syncFrequency(frequency: number, mode?: SDRMode) {
    this.inner.syncFrequency(this.down(frequency), mode);
  }

  setFollowMode(follow: boolean) { this.inner.setFollowMode(follow); }

  panSpan() {
    const s = this.inner.panSpan();
    if (isIdentity(this.c)) return s;
    const [loHz, hiHz] = displayRange([s.loHz, s.hiHz], this.c);
    return { loHz, hiHz, movable: s.movable };
  }

  setMode(mode: SDRMode) { this.inner.setMode(mode); }
  setBandwidth(low: number, high: number) { this.inner.setBandwidth(low, high); }

  /** ★ binBandwidth is a WIDTH, not a position — an offset must never touch it, and with a
   *  non-inverting converter a width is unchanged anyway. Only the anchor moves. */
  zoom(frequency: number, binBandwidth: number) { this.inner.zoom(this.down(frequency), binBandwidth); }
  pan(frequency: number) { this.inner.pan(this.down(frequency)); }
  resetView() { this.inner.resetView(); }

  setRate(divisor: number) { this.inner.setRate(divisor); }
  pauseSpectrum() { this.inner.pauseSpectrum(); }
  resumeSpectrum() { this.inner.resumeSpectrum(); }
  watchSpectrumUrl?(): string { return this.inner.watchSpectrumUrl!(); }
  forceResubscribe?(reason: string): void { this.inner.forceResubscribe?.(reason); }

  getStatus(): SDRStatus { return this.upStatus(this.inner.getStatus()); }
  getView():   SDRStatus { return this.upStatus(this.inner.getView()); }

  // ── Everything below carries no frequency: passed straight through. ──
  getProfiles?() { return this.inner.getProfiles!(); }
  selectProfile?(id: string) { this.inner.selectProfile?.(id); }
  getSecondaryDecoder?() { return this.inner.getSecondaryDecoder!(); }
  sendChat?(text: string, name: string) { this.inner.sendChat?.(text, name); }
}
