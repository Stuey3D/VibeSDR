/**
 * dialChat — the canned vocabulary for a shared-VFO receiver, and how to draw it.
 *
 * ★★★ ON A SHARED DIAL THE SERVER ENFORCES NOTHING. Anybody may tune, deliberately — Stuart,
 *     2026-08-20: *"the dial must be like FM-DX where anybody can tune it, otherwise I would need
 *     to be on the server 24/7 to allow access to it."* So this list is not decoration beside the
 *     mechanism; it IS the mechanism by which two strangers sort the dial out between themselves,
 *     while the owner is asleep.
 *
 * ★★★ NOTHING HERE IS TYPED BY A USER. The vocabulary is fixed, IDS travel on the wire, and the
 *     text below is this client's rendering of them. That one decision removes the moderation
 *     burden, the abuse vector, the translation problem and the injection surface together — and
 *     it is what makes the feature possible for a one-person operator at all (Stuart: *"that way
 *     we dont have to build a moderation system in"*).
 *
 * ★★ ONE VOCABULARY ACROSS FOUR SURFACES. The ids are the server's (`chatPhrases()` in
 *    local_sdr_shim.cpp) and the wording matches the browser (web/client/src/chat.ts) and the
 *    watch. Change a WORDING freely — it is per-client by design. Change or remove an ID and the
 *    ends stop understanding each other.
 *
 * ★ AN UNKNOWN ID IS DROPPED, never shown raw: a newer server may know a phrase this build does
 *   not, and `decode_done` on screen would be worse than nothing.
 */

export type Phrase = { id: string; text: string };

/** In the order a conversation actually runs: ask, act, answer, thank. */
export const DIAL_PHRASES: Phrase[] = [
  { id: 'ask_tune',       text: 'Can I tune?' },
  { id: 'anyone_using',   text: 'Anyone using this?' },
  { id: 'tuning_now',     text: 'Tuning now' },
  { id: 'go_ahead',       text: 'Go ahead, tune' },
  { id: 'please_hold',    text: 'Please hold — chasing DX' },
  { id: 'mid_decode',     text: "I'm running a decoder — can you wait please?" },
  // ★★ THE LONG ONES EARN THEIR PLACE. A WEFAX chart or an SSTV frame is ten minutes of holding
  //    still, and "please wait" with no duration is what makes people ask again ninety seconds
  //    later. Saying HOW LONG is the difference between a queue and an argument.
  { id: 'decoding_10min', text: 'Decoding — about 10 minutes' },
  { id: 'decode_done',    text: 'Decode finished — all yours' },
  { id: 'wont_tune',      text: "OK, I won't tune yet" },
  { id: 'all_yours',      text: 'Done — all yours' },
  { id: 'thanks',         text: 'Thanks!' },
  { id: 'sorry',          text: "Sorry, didn't realise!" },
];

const TEXT: Record<string, string> = Object.fromEntries(DIAL_PHRASES.map(p => [p.id, p.text]));

/** The text for a phrase id, or null if this build cannot draw it (drop it — see the header). */
export function phraseText(id: string): string | null {
  return TEXT[id] ?? null;
}

/** What the server says about the room. `mode` is 'exclusive' on an ordinary receiver. */
export type DialState = {
  mode: string; tuner: number; mine: boolean; you: number;
  listeners: number; decoding: boolean;
};

/** True when this receiver shares one dial between its listeners. */
export function isSharedDial(d: DialState | null): boolean {
  return !!d && d.mode !== 'exclusive';
}

/**
 * One line of plain English about the room, for the strip above the phrases.
 *
 * ★★ WHO MOVED IT LAST, NOT WHO OWNS IT — nobody owns it. Said plainly, because a frequency that
 *    changes under you with no explanation reads as the receiver glitching, and that is the one
 *    thing a shared dial must never look like.
 * ★ "You tuned last", not "you have the dial": nobody HOLDS it, and saying otherwise would promise
 *   an exclusivity the server does not enforce and Stuart deliberately did not want.
 */
export function dialSummary(d: DialState): string {
  const bits: string[] = [`${d.listeners} listening`];
  if (d.mode === 'spectator')      bits.push('the owner tunes this receiver');
  else if (d.decoding)             bits.push(d.mine ? 'you are decoding' : `User ${d.tuner} is decoding`);
  else if (d.tuner && d.mine)      bits.push('you tuned last');
  else if (d.tuner)                bits.push(`User ${d.tuner} is tuning`);
  else                             bits.push('nobody is tuning — go ahead');
  return bits.join(' · ');
}

/** How a speaker is named. ★ "You" rather than your own ordinal: everybody else sees a number, and
 *  you know which is yours, but reading your own words back as a stranger's is oddly cold. */
export function speakerName(from: number, you: number): string {
  return from === you ? 'You' : `User ${from}`;
}
