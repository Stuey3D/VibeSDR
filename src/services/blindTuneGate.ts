/**
 * blindTuneGate — may a tune that arrives WITHOUT THE DECK IN FRONT OF YOU move this receiver?
 *
 * ★★★ "BLIND" = the lock screen / Control Centre / headphone / car ⏮⏭, an Android Auto or CarPlay
 *     list pick, and Siri. None of them shows the SHARED TUNER banner, so none of them can ask. On
 *     one shared tuner a blind skip retunes EVERY listener, with nobody having seen who was there.
 *
 * ★★ FM-DX has always had this: the native audio engine switches ⏮⏭ OFF for an FM-DX session
 *    (VibePowerModule `fmdxAudio`, VibeStreamService `fmdxAudio`), greyed on the lock screen and
 *    refused if a stale control fires anyway. A VibeServer SHARED DIAL is the same one-tuner
 *    arrangement and got none of it — every media control stayed live (Stuart, 2026-09-30: "We
 *    don't give our shared VFO servers the same treatment as FM-DX — all the media controls are
 *    currently unlocked").
 *
 * ★ THE DECK IS NOT GATED HERE, deliberately. On a shared dial the server enforces nothing and
 *   anybody may tune (dialChat's header) — with the banner in view, which is the whole difference.
 *   Hardware-keyboard keys are the deck under a finger, so they follow the deck, not this.
 *
 * ★ The rule follows the banner's own words, so the lock screen and the deck never disagree:
 *     exclusive receiver (or no dial yet)   → allowed — the dial is yours
 *     shared, FREE TO TUNE (alone)          → allowed — nobody to disturb
 *     shared, ASK TO TUNE (others listening)→ DISABLED — you cannot ask from a lock screen
 *     spectator ("the owner tunes")         → DISABLED, unless you are the unlocked admin (then as open)
 *     listen-only (SpyServer, no control)   → DISABLED — every use would be a no-op
 *     FM-DX                                 → DISABLED — its own rule, kept exactly as it was
 *   A tune STEP choice by voice is not a tune (it is this client's setting) and is never gated.
 *
 * Pure: no React Native, so scripts/test_blindTuneGate.ts runs it directly under node.
 */
import type { DialState } from './dialChat';

/** Where the blind tune came from. `next`/`prev` = the media ⏮⏭ (either skip setting). */
export type BlindTuneAction = 'next' | 'prev' | 'carPick' | 'voiceTune' | 'voiceMode' | 'voiceStep';

export type BlindTuneInput = {
  /** Route server type ('fmdx', 'ubersdr', 'owrx', 'kiwi', 'vibeserver', …). */
  serverType: string;
  /** The server's dial message, or null when none has arrived (an ordinary receiver never sends one). */
  dial: Pick<DialState, 'mode' | 'listeners'> | null;
  /** The receiver will not take a tune from us at all (SpyServer canControl=0). */
  readOnly?: boolean;
  /** Unlocked admin of this receiver — on a spectator dial the owner is the one who tunes. */
  admin?: boolean;
};

export type BlindTuneReason =
  | 'ok' | 'not-a-tune' | 'fmdx-shared-tuner' | 'listen-only' | 'spectator' | 'others-listening';

/** FREE TO TUNE — the banner's own test (ControlsBar: `alone: listeners <= 1`). ONE rule, two readers. */
export function dialAlone(d: Pick<DialState, 'listeners'>): boolean {
  return d.listeners <= 1;
}

/** Why a blind tune may or may not go ahead. */
export function blindTuneReason(inp: BlindTuneInput, action: BlindTuneAction): BlindTuneReason {
  if (action === 'voiceStep') return 'not-a-tune';
  if (inp.serverType === 'fmdx') return 'fmdx-shared-tuner';
  if (inp.readOnly) return 'listen-only';
  const d = inp.dial;
  if (!d || d.mode === 'exclusive') return 'ok';
  if (d.mode === 'spectator' && !inp.admin) return 'spectator';
  return dialAlone(d) ? 'ok' : 'others-listening';
}

export function blindTuneAllowed(inp: BlindTuneInput, action: BlindTuneAction): boolean {
  const r = blindTuneReason(inp, action);
  return r === 'ok' || r === 'not-a-tune';
}

/** The ⏮⏭ pair as one switch — what the native lock-screen / notification controls are told. */
export function mediaSkipEnabled(inp: BlindTuneInput): boolean {
  return blindTuneAllowed(inp, 'next');
}

/** One line for the screen when a blind tune was refused (a voice or car request that did nothing
 *  must say why, or it reads as the feature being broken). */
export function blindTuneRefusal(r: BlindTuneReason): string {
  switch (r) {
    case 'others-listening': return 'Shared tuner — others are listening, so tune from the app and ask first.';
    case 'spectator':        return 'This receiver is set to listen only — the owner tunes it.';
    case 'listen-only':      return 'Listen-only receiver — tuning is not available.';
    case 'fmdx-shared-tuner':return 'Shared tuner — tune from the app.';
    default:                 return '';
  }
}
