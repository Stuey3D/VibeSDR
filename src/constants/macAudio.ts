/**
 * macAudio.ts (constants) — the pure rules behind the Mac-only VOLUME fader and MUTE key.
 *
 * ★★★ ON A MAC THE iPAD APP HAS NO VOLUME OF ITS OWN. macOS has no per-app volume and the system
 *     volume is every app's at once, so the AUDIO popup grows a VOLUME fader and a MUTE key — on a
 *     Mac ONLY. Stuart: "a volume control separate from the system volume could end up looking like
 *     a broken app if forgotten about" — on an iPhone or iPad the system volume is right there under
 *     the user's thumb, and a second, forgotten one turned down is exactly a silent "broken" app.
 *
 * ★ No React Native here, so scripts/test_mac_audio.ts can run it under plain node. The store, the
 *   persistence and the native call are src/services/macAudio.ts.
 */

export interface MacAudioState {
  /** The fader's POSITION, 0..1 — what is drawn and saved. The gain it gives is macOutputGain(). */
  volume: number;
  /** MUTE key. Gain 0 with nothing torn down, so unmute is instant. */
  muted: boolean;
}

/** Today's behaviour: full volume, not muted — a Mac that never touches the fader hears no change. */
export const MAC_AUDIO_DEFAULT: MacAudioState = { volume: 1, muted: false };

/** Per DEVICE (AsyncStorage, never synced): one Mac's speakers say nothing about another's. */
export const MAC_AUDIO_STORAGE_KEY = 'vibe.macAudio.v1';

/** Where unmuting lands when the fader had been pulled all the way down — an unmute that is still
 *  silent would read as the MUTE key being broken. */
export const MAC_UNMUTE_FLOOR = 0.5;

/**
 * Show the VOLUME fader and MUTE key? Only on iOS, and only when the native deviceClass() says the
 * process is the iPad app on a Mac (ProcessInfo.isiOSAppOnMac, or Catalyst). Android, web, tvOS,
 * an iPhone, an iPad — never. An old binary without the getter reads as "not a Mac" (hidden).
 */
export function showMacAudio(platformOS: string, isMac: boolean): boolean {
  return platformOS === 'ios' && isMac === true;
}

export function clampVolume(v: unknown): number {
  const n = typeof v === 'number' && Number.isFinite(v) ? v : MAC_AUDIO_DEFAULT.volume;
  return Math.max(0, Math.min(1, n));
}

/** What AsyncStorage hands back, checked. Anything malformed falls back field by field. */
export function parseMacAudio(raw: unknown): MacAudioState {
  let o: unknown = raw;
  if (typeof raw === 'string') { try { o = JSON.parse(raw); } catch { o = null; } }
  const r = (o && typeof o === 'object' ? o : {}) as Record<string, unknown>;
  return {
    volume: 'volume' in r ? clampVolume(r.volume) : MAC_AUDIO_DEFAULT.volume,
    muted: r.muted === true,
  };
}

/**
 * The gain native applies (the engine's main mixer, 0..1). ★ Unity anywhere but a Mac, whatever
 * is stored — native forces the same, so a stray value can never quieten an iPhone.
 * ★ A CUBIC TAPER: loudness is logarithmic, and a linear gain puts the whole audible range in the
 *   top quarter of the fader. x³ is the usual cheap fit (half way ≈ -18 dB, a tenth ≈ -60 dB).
 */
export function macOutputGain(s: MacAudioState, onMac: boolean): number {
  if (!onMac) return 1;
  if (s.muted) return 0;
  const v = clampVolume(s.volume);
  return v * v * v;
}

/** The speaker key's legend is the speaker-with-an-X whenever the Mac is making no sound because of
 *  US — muted, or the fader at zero (a fader left at the bottom is a mute you have forgotten). */
export function macSilenced(s: MacAudioState, onMac: boolean): boolean {
  return onMac && (s.muted || clampVolume(s.volume) <= 0);
}

/** MUTE key. Unmuting a fader left at zero brings it up to MAC_UNMUTE_FLOOR so the press is heard. */
export function toggleMacMute(s: MacAudioState): MacAudioState {
  if (s.muted || s.volume <= 0) {
    return { volume: s.volume > 0 ? s.volume : MAC_UNMUTE_FLOOR, muted: false };
  }
  return { ...s, muted: true };
}

/** The fader moved. Moving it UNMUTES, as the system volume does — you reached for the volume
 *  because you want to hear something. */
export function setMacVolume(_s: MacAudioState, v: number): MacAudioState {
  return { volume: clampVolume(v), muted: false };
}

/** The fader's readout. */
export function macVolumeLabel(s: MacAudioState): string {
  return s.muted ? 'MUTED' : `${Math.round(clampVolume(s.volume) * 100)}%`;
}
