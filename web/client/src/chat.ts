/**
 * ★★★ THE CANNED CHAT, AND THE OCCUPANCY STRIP THAT GOES WITH IT.
 *
 * On a shared-dial (FM-DX style) receiver anybody may tune, and the server enforces nothing —
 * Stuart, 2026-08-20: *"the dial must be like FM-DX where anybody can tune it, otherwise I would
 * need to be on the server 24/7 to allow access to it."* So this is not decoration beside the
 * mechanism; it IS the mechanism. Two strangers sort out the dial between themselves, and the
 * owner is asleep.
 *
 * ★★★ NOTHING HERE IS TYPED. The vocabulary is fixed, ids travel on the wire, and the text below
 *     is this client's rendering of them. That single decision removes the moderation burden, the
 *     abuse vector, the translation problem and the XSS surface in one go — and it is what makes
 *     the feature possible for a one-person operator at all (Stuart: "that way we dont have to
 *     build a moderation system in").
 *
 * ★★ PEOPLE ARE ORDINALS. "User 3" needs no identity, no account and no name box, and the room is
 *    never handed a stranger's address or country. The server assigns the numbers; we only draw
 *    them.
 *
 * ★ AN UNKNOWN ID IS DROPPED. A newer server may know a phrase this build does not, and showing
 *   `decode_done` raw would be worse than showing nothing.
 */

/** The vocabulary, in the order a conversation actually runs: ask, act, answer, thank.
 *  ★★ TAKEN FROM JR, NOT INVENTED HERE — `Canned.fmdx` in Chat.swift, plus the long-decode lines.
 *     One vocabulary across the watch, the phone and the browser, so a conversation reads the same
 *     wherever it is held. Change a wording freely; change or remove an ID and the two ends stop
 *     understanding each other. */
export const PHRASES: Array<{ id: string; text: string }> = [
  { id: 'ask_tune',       text: 'Can I tune?' },
  { id: 'anyone_using',   text: 'Anyone using this?' },
  { id: 'tuning_now',     text: 'Tuning now' },
  { id: 'go_ahead',       text: 'Go ahead, tune' },
  { id: 'please_hold',    text: 'Please hold — chasing DX' },
  { id: 'mid_decode',     text: "I'm running a decoder — can you wait please?" },
  { id: 'decoding_10min', text: 'Decoding — about 10 minutes' },
  { id: 'decode_done',    text: 'Decode finished — all yours' },
  { id: 'wont_tune',      text: "OK, I won't tune yet" },
  { id: 'all_yours',      text: 'Done — all yours' },
  { id: 'thanks',         text: 'Thanks!' },
  { id: 'sorry',          text: "Sorry, didn't realise!" },
];

const TEXT: Record<string, string> = Object.fromEntries(PHRASES.map(p => [p.id, p.text]));

export type DialState = {
  mode: string; tuner: number; mine: boolean; you: number;
  listeners: number; decoding?: boolean;
};

type Deps = {
  /** Send a phrase id to the server. */
  say: (id: string) => void;
  /** Raise the unread count on whatever button opens this. */
  onUnread: (n: number) => void;
};

let deps: Deps | null = null;
let dial: DialState | null = null;
let unread = 0;
/** ★ Own the "is it open" question here rather than reading a class off the DOM: the panel is
 *  shared machinery (see the panel helpers in main.ts) and this file must not care how it opens. */
let isOpen = false;

const $ = (id: string) => document.getElementById(id);

export function initChat(d: Deps) {
  deps = d;
  const list = $('chatPhrases');
  if (list) {
    list.innerHTML = '';
    for (const p of PHRASES) {
      const b = document.createElement('button');
      b.className = 'btn';
      b.textContent = p.text;
      b.onclick = () => {
        deps?.say(p.id);
        // ★ NO LOCAL ECHO. The server is what everybody else sees, so waiting for it to come back
        //   is the only way this client's transcript matches theirs — and if flood control drops
        //   the phrase, nothing should be shown that other people never received.
        b.disabled = true;
        setTimeout(() => { b.disabled = false; }, 3000);   // mirrors the server's own 3s gap
      };
      list.appendChild(b);
    }
  }
}

/** Called when the panel opens or closes, so the unread count can be cleared and stop counting. */
export function chatOpened(open: boolean) {
  isOpen = open;
  if (open) { unread = 0; deps?.onUnread(0); }
}

/** A line arrived. */
export function onSaid(from: number, id: string) {
  const text = TEXT[id];
  if (!text) return;                       // an id this build cannot draw — see the header note
  const log = $('chatLog');
  if (log) {
    const row = document.createElement('div');
    row.className = 'chatLine';
    const who = document.createElement('span');
    who.className = 'chatWho';
    // ★ "You" rather than your own number: everybody else sees an ordinal, and you know which is
    //   yours, but reading your own words back as a stranger's is oddly cold.
    who.textContent = (dial && from === dial.you) ? 'You' : `User ${from}`;
    const what = document.createElement('span');
    what.textContent = text;               // textContent, never innerHTML — see the header note
    row.append(who, what);
    log.appendChild(row);
    while (log.children.length > 40) log.removeChild(log.firstChild!);
    log.scrollTop = log.scrollHeight;
  }
  if (!isOpen && !(dial && from === dial.you)) { unread++; deps?.onUnread(unread); }
}

/** The occupancy strip: who is here, who moved the dial, and why it is not moving. */
export function onDial(d: DialState) {
  dial = d;
  const strip = $('dialStrip');
  if (!strip) return;
  if (d.mode === 'exclusive') { strip.hidden = true; return; }
  strip.hidden = false;
  const bits: string[] = [];
  bits.push(`${d.listeners} listening`);
  // ★★ WHO MOVED IT LAST, not who owns it — nobody owns it. Said plainly, because a frequency
  //    that changes under you with no explanation reads as the receiver glitching, and that is
  //    the one thing a shared dial must never look like.
  if (d.mode === 'spectator') bits.push('the owner tunes this receiver');
  else if (d.decoding)        bits.push(d.mine ? 'you are decoding' : `User ${d.tuner} is decoding`);
  // ★ "You are tuning", not "you have the dial": nobody HOLDS it. Saying otherwise would promise
  //   an exclusivity the server does not enforce and Stuart deliberately did not want.
  else if (d.tuner && d.mine) bits.push('you tuned last');
  else if (d.tuner)           bits.push(`User ${d.tuner} is tuning`);
  else                        bits.push('nobody is tuning — go ahead');
  strip.textContent = bits.join(' · ');
  strip.classList.toggle('busy', !!d.decoding);
}

/** Spectator mode said no. ★ One line, then it fades: it is an explanation, not an error. */
export function onDialRefused() {
  const strip = $('dialStrip');
  if (!strip) return;
  strip.hidden = false;
  strip.textContent = 'This receiver is set to listen only — the owner tunes it.';
  strip.classList.add('busy');
}

/** Is this receiver running a shared dial at all? Used to decide whether the chat button and the
 *  strip belong on screen — on an ordinary receiver neither does. */
export function chatAvailable(): boolean {
  return !!dial && dial.mode !== 'exclusive';
}
