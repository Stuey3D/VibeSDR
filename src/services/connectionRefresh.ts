/**
 * CONNECTION REFRESH — the app's equivalent of a browser page refresh for the receiver in front of
 * you (Servers menu → Connection Refresh, 2026-10-01).
 *
 * ★★★ WHY IT EXISTS. A session is several connections that fail independently: the spectrum socket
 *     (JS), an audio socket that may be NATIVE (UberSDR's Opus engine, or the VibeServer / dongle
 *     pump) or JS-owned (OWRX / Kiwi push PCM from their adapters), and a decoder / chat socket. Any
 *     one of them can be left dead while the others look healthy — the DAB fault that prompted this
 *     was a waterfall and a DAB tuning screen that came back after an interruption over a native
 *     audio socket that never did, and "leave the server and come back" was the only cure. That
 *     remount is what this does without the trip through the server list.
 *
 * ★★★ IT IS THE ORDINARY RECONNECT, NOT A NEW ONE. The screen already has exactly one way to rebuild
 *     a session — `fullReconnect` → connEpoch — and every rule about reconnecting lives on that path:
 *     the stable session id (the server re-affirms our slot, so a guaranteed-time session keeps its
 *     clock instead of meeting a stranger), the shared-dial "adopt, never assert" restore, the
 *     occupancy probe, the hwinfo the new socket is sent. A refresh that grew its own copy of those
 *     would be the "one rule, two readers" fault this project keeps paying for. So it adds only what
 *     that path was MISSING: the audio it claimed to restart and did not (see `audio` below).
 *
 * ★ Pure: no React, no native — the screen turns the plan into key bumps. Tested by
 *   scripts/test_connectionRefresh.ts.
 */

/** Who owns the AUDIO socket for this session, and therefore what a refresh must restart. */
export type RefreshAudio =
  /** UberSDR's native Opus engine (AudioPlayer). Restarted only AFTER the new POST /connection has
   *  registered the session — an audio socket for an unregistered id is dropped on sight (issue #20),
   *  and a refresh is exactly when the server may have reaped the old registration. */
  | 'native-after-register'
  /** The native /ws/audio pump (LocalAudioPlayer) — a VibeServer, or this phone's own dongle via
   *  the loopback shim. Restarted with the connection: it needs no preflight. */
  | 'local-pump'
  /** OWRX / Kiwi / Web-888: the adapter opens its own audio, so a new adapter IS new audio. */
  | 'adapter'
  /** Nothing on this screen plays audio for it (an unknown type). */
  | 'none';

export interface RefreshPlan {
  /** The spectrum / control socket and the server state it brings (config, hwinfo, dial). */
  spectrum: boolean;
  audio: RefreshAudio;
  /** The decoder / spots / chat socket — re-opened only if something on it is in use. */
  decoders: boolean;
}

/**
 * What a reconnect of this session rebuilds. Mirrors the mount gates in SDRScreen exactly: the
 * native AudioPlayer is mounted only for a NON-local 'ubersdr' type, LocalAudioPlayer only for a
 * local session (VibeServer or the phone's own dongle), and OWRX / Kiwi play from their adapters.
 */
export function connectionRefreshPlan(serverType: string | undefined, isLocal: boolean): RefreshPlan {
  const t = serverType ?? 'ubersdr';
  let audio: RefreshAudio = 'none';
  if (isLocal) audio = 'local-pump';
  else if (t === 'ubersdr') audio = 'native-after-register';
  else if (t === 'owrx' || t === 'kiwi' || t === 'web888') audio = 'adapter';
  return { spectrum: true, audio, decoders: true };
}

export interface RefreshAvailability {
  serverType?: string;
  /** The receiver is shown in the owner's own web page (compatibility mode) — no session of ours. */
  compatOnly?: boolean;
  /** Still choosing a radio behind a front door, or resolving it — nothing to refresh yet. */
  noSessionYet?: boolean;
  /** A refusal / eviction / session-ended card owns the screen. */
  refused?: boolean;
  /** A KiwiSDR turned us away (its own card, with its own Try again). */
  kiwiRefused?: boolean;
}

/**
 * ★★★ IS THE ROW THERE AT ALL? AGENTS.md: a control whose every use is a no-op must not be shown.
 *  ★★ A REFUSAL IS NOT A CONNECTION FAULT. The connect path deliberately refuses to reopen after a
 *     terminal refusal — every retry disturbs whoever now holds the radio — so a refresh there would
 *     do nothing, and the card already offers the choices that mean something (TRY AGAIN, TAKE OVER,
 *     back). Same for Kiwi's refusal card.
 *  ★ FM-DX never reaches this menu (it has its own tuner screen), but answers false here too: its
 *    audio and text sockets reconnect themselves and this screen has no way to rebuild them.
 */
export function canRefreshConnection(a: RefreshAvailability): boolean {
  if (a.compatOnly || a.noSessionYet || a.refused || a.kiwiRefused) return false;
  if ((a.serverType ?? 'ubersdr') === 'fmdx') return false;
  return true;
}

/**
 * ★★ THE NATIVE OPUS ENGINE RESTARTS ON THE NEW REGISTRATION — unless a takeover is in flight.
 *  With an admin credential held, `onConnect` already restarts the engine (it must: on a busy radio
 *  the audio socket is refused until the spectrum socket has evicted the occupant), and a second
 *  restart a few hundred ms earlier would only be refused and then cost a gap.
 */
export function restartAudioOnRegister(pending: boolean, adminHeld: boolean): boolean {
  return pending && !adminHeld;
}
