/**
 * sessionHoldNotice — "VibeSDR paused": the note iOS gets to show when it suspends the app (V12, 2026-10-27,
 * docs/v12/BACKGROUND-DECODING.md, "The held session").
 *
 * ★★★ STUART: "when the OS suspends the app we send a little notification to advise the user that due to lack of
 *     background audio playing the OS has suspended the app for powersaving, your session on the server is held for
 *     3 minutes all decoders are still running for this period and no data is missed."
 *     The server half is the HELD SESSION (local_sdr_shim.cpp "THE HELD SESSION"): a listener whose sockets have all
 *     gone keeps everything for holdS (180 s unless the owner's idle grace is shorter), and a hard limit ends it sooner.
 *
 * ★★★ ONLY WHEN iOS WILL ACTUALLY SUSPEND US (Stuart: "if audio playing the app will happily stay all day in the
 *     background so nobody ever sees that notification if audio is in use"). iOS keeps an app with a live audio session
 *     running for as long as it likes; it suspends one that has nothing playing within seconds. So the note is for the
 *     QUIET modes only — ADS-B, NDB, ISM, AIS, which release the audio session (setQuiet) — and the native side checks
 *     once more as it posts: an engine running means no note.
 * ★★★ AND A CLOSED SQUELCH IS NOT QUIET (Stuart: "if a user has a signal in squelch mode waiting to hear an incoming we
 *     must not allow the OS to kill the app in the background, that is the one time where silence is justified"). The
 *     server sends a squelched listener SILENCE PACKETS in lockstep with the stream (the shim's silencePending_), the
 *     engine plays them, and the session stays live — so squelch, manual or AUTO, VFO A or B, is deliberately NOT an
 *     input to audioQuietFor below: whatever it is, it can never release the session.
 */

export interface QuietModes { adsb: boolean; ism: boolean; ndb: boolean; ais: boolean }

/** ★★ The ONE rule for "nothing to hear — let the audio session go" (SDRScreen's setNativeQuiet). Only the whole-radio
 *  modes the server sends no audio for. A squelch, a mute of one VFO, a decoder, a waterfall-only view are not here,
 *  and must never be: each of those still has a live stream (gated silence) the listener may be waiting on. */
export function audioQuietFor(m: QuietModes): boolean {
  return m.adsb || m.ism || m.ndb || m.ais;
}

/** What the server said (hwinfo): seconds it holds us, and Date.now() at which a hard limit ends that sooner (0 = none). */
export interface HoldTerms { holdS: number; hardAt: number }

export interface NoticeInput {
  platform: string;              // Platform.OS
  isVibeServer: boolean;
  terms: HoldTerms | null;       // null = the server never said (older server): no hold, no promise
  quiet: boolean;                // audioQuietFor — the audio session is released
  now: number;                   // Date.now() as the app goes to the background
  serverName: string;
  /** HH:MM for a moment — injectable for tests (the device's own clock format in the app). */
  clock: (ms: number) => string;
}

export type NoticePlan =
  | { post: false; why: string }
  | { post: true; title: string; body: string; deadline: number; byLimit: boolean };

/** How soon after backgrounding the note is posted — long enough that a glance at another app and straight back does
 *  not leave a note behind (it is cancelled on return), short enough to arrive before the listener has moved on. */
export const NOTICE_DELAY_S = 4;
/** A deadline nearer than this is not worth announcing — it would expire before it was read. */
const MIN_USEFUL_MS = 20_000;

/** "3 minutes", "1 minute", "45 seconds". */
export function holdWords(s: number): string {
  if (s >= 60 && s % 60 === 0) { const m = s / 60; return `${m} minute${m === 1 ? '' : 's'}`; }
  if (s >= 90) return `${Math.round(s / 60)} minutes`;
  return `${Math.round(s)} seconds`;
}

/** ★★ Post the note, and with what words? Pure — SDRScreen asks it as the app goes to the background. */
export function noticePlan(i: NoticeInput): NoticePlan {
  if (i.platform !== 'ios') return { post: false, why: 'not iOS — Android keeps the app running (its media service)' };
  if (!i.isVibeServer) return { post: false, why: 'not a VibeServer — no server holds the session' };
  if (!i.quiet) return { post: false, why: 'audio is live — iOS keeps the app running' };
  if (!i.terms || !(i.terms.holdS > 0)) return { post: false, why: 'this server does not hold a session' };
  const holdEnd = i.now + i.terms.holdS * 1000;
  const byLimit = i.terms.hardAt > 0 && i.terms.hardAt < holdEnd;
  const deadline = byLimit ? i.terms.hardAt : holdEnd;
  if (deadline - i.now < MIN_USEFUL_MS) return { post: false, why: 'the session ends too soon to be worth a note' };
  const name = (i.serverName || 'the server').trim();
  const at = i.clock(deadline);
  const title = 'VibeSDR paused';
  const body = byLimit
    ? `iOS pauses apps that aren't playing sound. Your session on ${name} reaches its time limit at ${at}, so it is held `
      + `until then, with its decoders still running. Come back before ${at} to carry on.`
    : `iOS pauses apps that aren't playing sound. Your place on ${name} is held for ${holdWords(i.terms.holdS)} and its `
      + `decoders keep running, so nothing is missed. Come back by ${at} to carry on.`;
  return { post: true, title, body, deadline, byLimit };
}

/** ★ Should we ask for notification permission now? The first time the note would be needed (entering a quiet mode on
 *  a VibeServer that holds sessions) — never at launch, never twice. */
export function shouldAskPermission(o: { platform: string; isVibeServer: boolean; quiet: boolean; terms: HoldTerms | null;
                                         status: 'granted' | 'denied' | 'undetermined' | 'unknown'; askedBefore: boolean }): boolean {
  return o.platform === 'ios' && o.isVibeServer && o.quiet && !!o.terms && o.terms.holdS > 0
    && o.status === 'undetermined' && !o.askedBefore;
}

/** The one line said in the app before iOS's own permission prompt. */
export const PERMISSION_TITLE = 'Know when your place is held';
export const PERMISSION_BODY = 'When you switch away from a map, iOS pauses VibeSDR because nothing is playing. '
  + 'Allow notifications and VibeSDR will tell you how long the server is holding your place.';

/** The words when the listener comes back after the hold has gone and the mode with it. */
export function endedWhy(label: string): string {
  return `You were away longer than the server holds a session, so it ended and the receiver left ${label}.`;
}
