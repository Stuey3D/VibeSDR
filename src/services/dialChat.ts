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

/** ★ `group`/`key`: phrases drawn as ONE row — a label and a short key each ("This sounds" Awesome · Weird …). */
export type Phrase = { id: string; text: string; group?: string; key?: string };
/** The label a phrase group's row starts with. */
export const PHRASE_GROUP_LABEL: Record<string, string> = { sounds: 'This sounds' };

/** In the order a conversation actually runs: ask, act, answer, thank. */
export const DIAL_PHRASES: Phrase[] = [
  { id: 'ask_tune',       text: 'Can I tune?' },
  { id: 'anyone_using',   text: 'Anyone using this?' },
  { id: 'tuning_now',     text: 'Tuning now' },
  { id: 'go_ahead',       text: 'Go ahead, tune' },
  { id: 'please_hold',    text: 'Please hold — chasing DX' },
  { id: 'yes_go_ahead',   text: "Yes, I'm on it — but go ahead and tune" },
  { id: 'yes_hold',       text: "Yes, I'm on it — please hold on" },
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
  // ★ The other side of "sorry" (Stuart, 2026-10-05): somebody moved the shared dial off what you were hearing.
  { id: 'tune_back',      text: 'Tuning back — I was listening to that' },
  // ★★ THE SOCIAL ONES (Stuart, 2026-10-08): "make it a little more social without opening it up to full text chat". Still ids only — a fixed, friendly vocabulary nobody can be abused with; "Not my kind of music" is the diplomatic way to move the dial on.
  { id: 'hello', text: 'Hello everyone!' },
  { id: 'just_scanning', text: "Just scanning to see what's about" },
  { id: 'what_is_this', text: 'Anyone know what this is?' },
  // ★ "THIS SOUNDS …" IS ONE ROW (Stuart, 2026-10-08: "This sounds awesome could be a selection") — each choice its own id, drawn as a 'This sounds' label and a key per word (group/key below); a message reads in full.
  { id: 'sounds_awesome', text: 'This sounds awesome!', group: 'sounds', key: 'Awesome' },
  { id: 'sounds_great', text: 'This sounds great', group: 'sounds', key: 'Great' },
  { id: 'sounds_interesting', text: 'This sounds interesting', group: 'sounds', key: 'Interesting' },
  { id: 'sounds_weird', text: 'This sounds weird', group: 'sounds', key: 'Weird' },
  { id: 'sounds_distorted', text: 'This sounds distorted', group: 'sounds', key: 'Distorted' },
  { id: 'sounds_bad', text: 'This sounds bad', group: 'sounds', key: 'Bad' },
  { id: 'nice_catch', text: 'Nice catch!' },
  { id: 'not_my_music', text: 'Not my kind of music' },
  { id: 'good_conditions', text: 'Conditions are great today' },
  { id: 'poor_conditions', text: 'Conditions are poor today' },
  { id: 'off_73', text: 'Off now — 73!' },
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
