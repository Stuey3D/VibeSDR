/**
 * recordingLabel.ts — WHAT A RECORDING IS CALLED: its frequency and its mode word, for the file name.
 * ONE rule for the web page (web/client/src/main.ts recordingName), the app (SDRScreen toggleRecording → native
 * startRecording) and, in C++, the R36S handheld (handheld-ui sec_audio.h stopRecording).
 *
 * ★★ DAB IS NAMED BY ITS SERVICE (Stuart, 2026-11-08: a DAB recording on the handheld was named "222064.000 kHz WFM",
 *    "that DAB recording name fix needs to go everywhere the web client and the app"). On DAB the dial is not a station:
 *    the file says "DAB <service> <channel>" at the multiplex's centre frequency — "DAB talkSPORT 11D", 222.064 MHz.
 * ★ The service name comes off the air, so it is cleaned for a file name: letters, digits, space, + . - only (never _: the app's list splits names on it), at most
 *   40 characters (FAT, Android's MediaStore and iOS all take that).
 * PURE (type-only imports), so scripts/test_recording_label.ts runs it under plain node.
 */
import type { DabState } from './dabTypes';

export interface RecordingLabel { hz: number; mode: string }

/** A string off the air, made safe in a file name. */
export function fileSafe(s: string, max = 40): string {
  return String(s ?? '').replace(/[^A-Za-z0-9 +.-]+/g, ' ').replace(/\s+/g, ' ').trim().slice(0, max).trim();
}

/** The frequency and mode word a recording made now is filed under. `dab`: the DAB state while DAB plays, else null. */
export function recordingLabel(hz: number, mode: string, dab: Pick<DabState, 'channel' | 'centreHz' | 'services' | 'sid'> | null | undefined): RecordingLabel {
  if (dab && dab.channel) {
    const svc = Array.isArray(dab.services) ? dab.services.find((x) => x && x.sid === dab.sid) : undefined;
    const name = fileSafe(svc?.label ?? '');
    return { hz: dab.centreHz > 0 ? dab.centreHz : hz, mode: ['DAB', name, fileSafe(dab.channel, 8)].filter(Boolean).join(' ') };
  }
  return { hz, mode: fileSafe(String(mode || '').toUpperCase(), 12) };
}
