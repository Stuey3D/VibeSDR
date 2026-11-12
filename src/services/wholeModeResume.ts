/**
 * wholeModeResume — coming back to a whole-radio mode (ADS-B, NDB, ISM, AIS) after the app was away.
 *
 * ★★★ THE BUG (Stuart, 2026-10-27, iPhone, V12 498/499): "a couple of times I was in the map view for NDB or ADS-B and
 *     when I moved to another app leaving VibeSDR running in the background when I switched back the map dropped back
 *     to a waterfall view on the previous frequency, this may have been poor cellular connection though."
 *     Two faults, one after the other, and either alone shows exactly that:
 *       1. THE SERVER GAVE THE RADIO BACK. Those modes send no audio, so the app lets go of the audio session (quiet) and
 *          iOS suspends it seconds after it goes to the background — the spectrum socket the app closes itself, the
 *          audio socket goes with the suspension (at once on a poor cellular link). Five seconds with nobody connected
 *          and the server left the mode, put the dial back where the mode found it, and told nobody (nobody was here).
 *          The server now waits as long as its idle park would (≤ 60 s) — local_sdr_shim.cpp modeLeaveGraceS.
 *       2. THE APP JUDGED ITS OWN SILENCE AS THE SERVER'S. The "lapsed hold" watchdog let the mode go after ten seconds
 *          without a report — and while backgrounded the app had CLOSED the socket the reports come on. So the moment it
 *          came back the map was dropped, before the reopened socket could say anything. Silence is now measured only
 *          while a spectrum socket is open (holdLapsed below, VibeServerWsClient.spectrumOpenSince).
 *
 * ★★★ AND WHEN THE SERVER REALLY HAS LEFT THE MODE, the app ASKS for it back (`mode_resume`) instead of quietly dropping
 *     the map — a restoration of this listener's own last action, which the SERVER grants only if nobody else is on the
 *     radio and nobody has tuned it since (modeResume in the shim). The shared-dial contract holds: the client never
 *     re-enters by itself (recovery is not a user action), it asks a question the server answers with the whole room in
 *     view. A refusal, or no answer (an older server ignores the type), lets the mode go and SAYS WHY — never silently.
 */

export type WholeKind = 'adsb' | 'ndb' | 'ism' | 'ais';

/** The name a listener reads. */
export const WHOLE_LABEL: Record<WholeKind, string> = { adsb: 'ADS-B', ndb: 'NDB', ism: 'ISM', ais: 'AIS' };

/** ★ Ten seconds without a report on a socket that HAS delivered one — the old lapsed-hold rule, unchanged. */
export const WHOLE_STEADY_SILENCE_MS = 10_000;
/** ★ How long a freshly opened socket may go without its first report. The server sends one a second (ADS-B, NDB, AIS)
 *  and sends a joiner one at once; ISM's full list may be five seconds apart on a busy band, hence its longer wait. */
export function firstReportMs(kind: WholeKind): number { return kind === 'ism' ? 7_000 : 4_000; }
/** ★ How long the server has to answer `mode_resume` — or, having said yes, to send the first report (an entry can
 *  change the rate, which takes a moment on a slow radio). */
export const RESUME_ANSWER_MS = 8_000;

export interface LapseInput {
  kind: WholeKind;
  now: number;
  /** Date.now() of the last report of this mode (0 = none on this client). */
  lastReportAt: number;
  /** VibeServerWsClient.spectrumOpenSince(): when the OPEN spectrum socket opened, 0 = none open (paused, reconnecting). */
  openSince: number;
}

/** ★★ Has the server stopped reporting a mode we believe the radio is in? Never while no socket is open — that silence
 *  is ours. From the socket's opening when it has not reported yet; from the last report when it has. */
export function holdLapsed(i: LapseInput): boolean {
  if (!(i.openSince > 0)) return false;
  if (i.lastReportAt >= i.openSince) return i.now - i.lastReportAt > WHOLE_STEADY_SILENCE_MS;
  return i.now - i.openSince > firstReportMs(i.kind);
}

export type ResumeStep =
  | { do: 'none' }
  | { do: 'ask'; kind: WholeKind }
  | { do: 'giveUp'; kind: WholeKind; why: string };

const NONE: ResumeStep = { do: 'none' };

/** ★ The words when the server gave no reason (an older server that does not know `mode_resume`, or no answer). */
export function defaultGoneWhy(kind: WholeKind): string {
  return `The receiver left ${WHOLE_LABEL[kind]} while you were away.`;
}

/**
 * ★★ ONE ASK PER LAPSE. `step` is called every couple of seconds with the mode the screen believes it is in (null =
 * none) and whether its hold has lapsed; `answered` with the server's `mode_resume`; `sendFailed` when the ask could
 * not go (no socket, or the dial is locked to somebody else).
 */
export class WholeModeResume {
  private asked: { kind: WholeKind; at: number; socket: number } | null = null;
  /** ★ Given up on this lapse — never asked again until reports return or the mode changes (one ask per lapse). */
  private gaveUp: WholeKind | null = null;

  /** `socket` identifies the spectrum socket the ask would go on (its spectrumOpenSince): an ask made on a socket that
   *  has since been replaced (a half-open one the watchdog closed after a phone woke) is asked again on the new one,
   *  rather than given up on for want of an answer that could never come. */
  step(kind: WholeKind | null, lapsed: boolean, now: number, socket = 0): ResumeStep {
    // ★ Reports flowing (or no mode at all): any ask is answered by them.
    if (!kind || !lapsed) { this.asked = null; this.gaveUp = null; return NONE; }
    if (this.gaveUp === kind) return NONE;
    if (!this.asked || this.asked.kind !== kind || this.asked.socket !== socket) {
      this.asked = { kind, at: now, socket };
      return { do: 'ask', kind };
    }
    if (now - this.asked.at > RESUME_ANSWER_MS) { this.asked = null; this.gaveUp = kind; return { do: 'giveUp', kind, why: defaultGoneWhy(kind) }; }
    return NONE;
  }

  /** The server's answer. Yes: the reports follow — wait for them (afresh, the entry may take a moment). No: let it go.
   *  ★★ V12 2026-11-12: `believed` = the mode the screen shows now. A refusal we did not ask for is the server's own
   *     answer at our ARRIVAL (the shim's arrivalResume: we came back past the hold and it could not put us back) — for
   *     the mode we still show, it is the reason the mode has gone, and it is said. */
  answered(kind: WholeKind, ok: boolean, why: string | undefined, now: number, believed: WholeKind | null = null): ResumeStep {
    if (!this.asked || this.asked.kind !== kind) {
      if (!ok && believed === kind && this.gaveUp !== kind) {
        this.asked = null; this.gaveUp = kind;
        return { do: 'giveUp', kind, why: why || defaultGoneWhy(kind) };
      }
      return NONE;   // ★ not ours (a stale answer after the mode changed)
    }
    if (ok) { this.asked = { ...this.asked, at: now }; return NONE; }
    this.asked = null; this.gaveUp = kind;
    return { do: 'giveUp', kind, why: why || defaultGoneWhy(kind) };
  }

  /** The ask could not be sent. */
  sendFailed(kind: WholeKind, why?: string): ResumeStep {
    this.asked = null; this.gaveUp = kind;
    return { do: 'giveUp', kind, why: why || defaultGoneWhy(kind) };
  }

  /** The listener chose something else (EXIT, another mode): nothing is pending. */
  reset() { this.asked = null; this.gaveUp = null; }

  get pending(): WholeKind | null { return this.asked?.kind ?? null; }
}

/** ★ How long after OUR OWN entry of a mode a report of the mode we came from is still believed to be in flight. */
export const WHOLE_SWITCH_MS = 5_000;
const KINDS: WholeKind[] = ['adsb', 'ndb', 'ism', 'ais'];

/**
 * ★★★ ONE WHOLE-RADIO MODE AT A TIME — A REPORT OF ONE MEANS THE OTHERS HAVE ENDED (V12, 2026-11-12).
 * Stuart, iPhone, the Pi 500's V4L (idle default ADS-B): back from the background past the hold, the app was handed ADS-B's
 * reports but nothing ever said ISM had gone (the server left ISM while nobody was here, telling nobody), so it kept the
 * ISM screen — and ISM's frequency box — over a radio at 1090 MHz: "I stay in ISM mode but … it tunes to 1089.590MHz".
 * The server runs ONE whole-radio mode (each entry leaves the others through their own doors), so `kind` reporting is
 * the truth about all four: the others are over.
 * ★ Not for a mode WE entered in the last WHOLE_SWITCH_MS — the mode we came from may still have a report on the wire.
 * @returns the modes to let go, without telling the server (it has left them).
 */
export function othersEnded(kind: WholeKind, held: Record<WholeKind, boolean>, askedAt: Record<WholeKind, number>,
                            now: number): WholeKind[] {
  return KINDS.filter((k) => k !== kind && held[k] && !(now - (askedAt[k] ?? 0) < WHOLE_SWITCH_MS));
}
