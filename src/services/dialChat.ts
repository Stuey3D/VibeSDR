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

/** ★ `group`/`key`: phrases drawn as ONE row — a label and a short key each ("This sounds" Awesome · Weird …).
 *  ★ An ANSWER (group 'answer') also says which band it belongs to, its Signal Identification Wiki page and — when this
 *    app has one — the decoder that reads it. See ANSWERS below. */
export type Phrase = { id: string; text: string; group?: string; key?: string;
                       band?: 'hf' | 'vhf'; wiki?: string; decoder?: 'rtty' | 'navtex' | 'wefax' | 'sstv' | 'time' };
/** The label a phrase group's row starts with. */
export const PHRASE_GROUP_LABEL: Record<string, string> = { sounds: 'This sounds', answer: "It's" };

/** ★★ THE SIGNAL IDENTIFICATION WIKI (Stuart, 2026-10-09: "Anyone know what this is with no way of someone telling
 *  them"). Linked, never copied or framed: opened in the system's in-app browser (SFSafariViewController / Chrome
 *  Custom Tabs) — the real site, its address bar, its own attribution, exactly as published. Every page below was
 *  checked to exist (HTTP 200) on 2026-10-09; one without a page (time signals) simply has no link. */
export const SIGID_HOME = 'https://www.sigidwiki.com/wiki/Signal_Identification_Guide';
const SIGID_PAGE = 'https://www.sigidwiki.com/wiki/';

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
  /* ★★★ THE ANSWERS TO "Anyone know what this is?" (Stuart, 2026-10-09). Still ids, never typed — free text would need
   *  the stores' user-generated-content machinery (filtering, reporting, a moderator) and lose the watches, the TV
   *  remotes and translation. Drawn as one "It's …" row ONLY while somebody else's question is open (answerRowOpen),
   *  and only the half for the band the dial is on, so the everyday pad does not grow. A message reads in full and
   *  carries a WIKI key (and DECODE, where this receiver runs that decoder). */
  { id: 'not_sure', text: 'Not sure, sorry', group: 'answer', key: 'Not sure' },
  { id: 'is_wefax',  text: "It's WEFAX (weather fax)", group: 'answer', key: 'WEFAX', band: 'hf', wiki: 'WEFAX', decoder: 'wefax' },
  { id: 'is_rtty',   text: "It's RTTY", group: 'answer', key: 'RTTY', band: 'hf', wiki: 'RTTY', decoder: 'rtty' },
  { id: 'is_navtex', text: "It's NAVTEX", group: 'answer', key: 'NAVTEX', band: 'hf', wiki: 'NAVTEX', decoder: 'navtex' },
  { id: 'is_sstv',   text: "It's SSTV (slow-scan TV)", group: 'answer', key: 'SSTV', band: 'hf', wiki: 'SSTV', decoder: 'sstv' },
  { id: 'is_ft8',    text: "It's FT8", group: 'answer', key: 'FT8', band: 'hf', wiki: 'FT8' },
  { id: 'is_cw',     text: "It's Morse (CW)", group: 'answer', key: 'Morse', band: 'hf', wiki: 'Morse_Code_(CW)' },
  { id: 'is_ssb',    text: "It's SSB voice", group: 'answer', key: 'SSB voice', band: 'hf', wiki: 'Single_Sideband_Voice' },
  { id: 'is_drm',    text: "It's DRM (digital radio)", group: 'answer', key: 'DRM', band: 'hf', wiki: 'DRM' },
  { id: 'is_stanag', text: "It's STANAG (military data)", group: 'answer', key: 'STANAG', band: 'hf', wiki: 'STANAG_4285' },
  { id: 'is_ale',    text: "It's ALE", group: 'answer', key: 'ALE', band: 'hf', wiki: 'ALE' },
  { id: 'is_hfdl',   text: "It's HFDL (aircraft data)", group: 'answer', key: 'HFDL', band: 'hf', wiki: 'HFDL' },
  { id: 'is_codar',  text: "It's CODAR (ocean radar)", group: 'answer', key: 'CODAR', band: 'hf', wiki: 'CODAR' },
  { id: 'is_oth',    text: "It's over-the-horizon radar", group: 'answer', key: 'OTH radar', band: 'hf', wiki: 'Over_the_Horizon_Radar' },
  { id: 'is_time',   text: "It's a time signal", group: 'answer', key: 'Time signal', band: 'hf', decoder: 'time' },
  { id: 'is_dmr',    text: "It's DMR", group: 'answer', key: 'DMR', band: 'vhf', wiki: 'DMR' },
  { id: 'is_dstar',  text: "It's D-STAR", group: 'answer', key: 'D-STAR', band: 'vhf', wiki: 'D-STAR' },
  { id: 'is_p25',    text: "It's P25", group: 'answer', key: 'P25', band: 'vhf', wiki: 'P25' },
  { id: 'is_nxdn',   text: "It's NXDN", group: 'answer', key: 'NXDN', band: 'vhf', wiki: 'NXDN' },
  { id: 'is_dpmr',   text: "It's dPMR", group: 'answer', key: 'dPMR', band: 'vhf', wiki: 'DPMR' },
  { id: 'is_pocsag', text: "It's POCSAG (pager)", group: 'answer', key: 'POCSAG', band: 'vhf', wiki: 'POCSAG' },
  { id: 'is_aprs',   text: "It's APRS", group: 'answer', key: 'APRS', band: 'vhf', wiki: 'APRS' },
  { id: 'is_adsb',   text: "It's ADS-B (aircraft)", group: 'answer', key: 'ADS-B', band: 'vhf', wiki: 'ADS-B' },
  { id: 'is_acars',  text: "It's ACARS (aircraft data)", group: 'answer', key: 'ACARS', band: 'vhf', wiki: 'ACARS' },
  { id: 'is_ais',    text: "It's AIS (ships)", group: 'answer', key: 'AIS', band: 'vhf', wiki: 'AIS' },
  { id: 'is_apt',    text: "It's a weather satellite (APT)", group: 'answer', key: 'Weather sat', band: 'vhf', wiki: 'Automatic_Picture_Transmission_(APT)' },
  { id: 'is_fm_bc',  text: "It's an FM broadcast station", group: 'answer', key: 'FM broadcast', band: 'vhf', wiki: 'FM_Broadcast_Radio' },
];

const TEXT: Record<string, string> = Object.fromEntries(DIAL_PHRASES.map(p => [p.id, p.text]));
const BY_ID: Record<string, Phrase> = Object.fromEntries(DIAL_PHRASES.map(p => [p.id, p]));

/** How long "Anyone know what this is?" keeps the answer row open, unless somebody answers first. */
export const ANSWER_WINDOW_MS = 5 * 60_000;
/** HF below 30 MHz, VHF/UHF above — the half of the answers that can be on the dial. */
export function answerBand(hz: number): 'hf' | 'vhf' { return hz > 0 && hz < 30_000_000 ? 'hf' : 'vhf'; }

/** The pad to draw: everything but the answers, plus — while a question is open — the answers for this band. */
export function padPhrases(questionOpen: boolean, hz: number): Phrase[] {
  const band = answerBand(hz);
  return DIAL_PHRASES.filter(p => p.group !== 'answer' || (questionOpen && (!p.band || p.band === band)));
}
/** True for the question that opens the answer row, and for any answer (which closes it). */
export const isQuestion = (id: string) => id === 'what_is_this';
export const isAnswer = (id: string) => BY_ID[id]?.group === 'answer';
/** The answer's Signal Identification Wiki page, or null. */
export function phraseWiki(id: string): string | null { const w = BY_ID[id]?.wiki; return w ? SIGID_PAGE + w : null; }
/** The decoder this app runs for the answer, or null. */
export function phraseDecoder(id: string): Phrase['decoder'] | null { return BY_ID[id]?.decoder ?? null; }

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
