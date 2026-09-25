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
  { id: 'yes_go_ahead',   text: "Yes, I'm on it — but go ahead and tune" },
  { id: 'yes_hold',       text: "Yes, I'm on it — please hold on" },
  { id: 'mid_decode',     text: "I'm running a decoder — can you wait please?" },
  { id: 'decoding_10min', text: 'Decoding — about 10 minutes' },
  { id: 'decode_done',    text: 'Decode finished — all yours' },
  { id: 'wont_tune',      text: "OK, I won't tune yet" },
  { id: 'all_yours',      text: 'Done — all yours' },
  { id: 'thanks',         text: 'Thanks!' },
  { id: 'sorry',          text: "Sorry, didn't realise!" },
];

const TEXT: Record<string, string> = Object.fromEntries(PHRASES.map(p => [p.id, p.text]));

/* ★★★ THE ONE PHRASE THAT CARRIES FACTS (Stuart, 2026-09-20): "Hey, check out 96.1 MHz Advanced RDS". A shared
 *  receiver is a room of people finding things, and the canned vocabulary let them agree who tunes but never
 *  say WHAT they found — the one thing worth saying on a radio.
 *  ★★ Still not free text: a NUMBER and a mode from this receiver's own list, both validated by the server. */
const MODE_LABEL: Record<string, string> = {
  wfm: 'WFM', nfm: 'NFM', am: 'AM', usb: 'USB', lsb: 'LSB', cwu: 'CW-U', cwl: 'CW-L',
  dab: 'DAB', rds: 'Advanced RDS', rtty: 'RTTY', navtex: 'NAVTEX', wefax: 'WEFAX',
  sstv: 'SSTV', ft8: 'FT8 / FT4', time: 'Time signal',
};
function checkOutText(hz: number, mode?: string): string {
  const mhz = hz >= 1e6 ? `${(hz / 1e6).toFixed(3).replace(/0+$/, '').replace(/\.$/, '')} MHz`
            : `${Math.round(hz / 1e3)} kHz`;
  const m = mode ? MODE_LABEL[mode] || mode.toUpperCase() : '';
  return `Hey, check out ${mhz}${m ? ' ' + m : ''}`;
}

export type DialState = {
  mode: string; tuner: number; mine: boolean; you: number;
  listeners: number; decoding?: boolean;
};

type Deps = {
  /** Send a phrase id to the server. `extra` carries the check-out payload — see checkOutText. */
  say: (id: string, extra?: { hz: number; mode?: string }) => void;
  /** The modes and decoders this receiver actually offers, so the picker cannot suggest a dead one. */
  modes?: () => string[];
  /** Where the dial is now — the frequency box starts there, because "check out" usually means "here". */
  freqHz?: () => number;
  /** Tune there, for when somebody taps what another listener found. */
  tuneTo?: (hz: number, mode?: string) => void;
  /** Raise the unread count on whatever button opens this. */
  onUnread: (n: number) => void;
};

let deps: Deps | null = null;
let dial: DialState | null = null;
let unread = 0;

/* ★★★ THE TAB TITLE, so a chat reaches somebody who is not looking at the page. The unread badge on
 *   the chat button is useless in a BACKGROUND TAB — and a background tab is exactly where a
 *   listener sits while somebody else is trying to ask them for the dial. Stuart: "if someone chats
 *   whilst the tab is in the background a (Chat) should appear in the tab bar."
 * ★★ Captured ONCE at load rather than assumed: the title is "VibeSDR" today, but reading it back
 *   means a rename never leaves this file stamping a stale name over it. And restoring means
 *   restoring THAT, not writing a constant.
 * ★ Driven off `unread`, so it clears exactly when the chat is read — chatOpened(true) zeroes the
 *   count, which is the same moment the badge clears. One source of truth for "seen it". */
const BASE_TITLE = typeof document !== 'undefined' ? document.title : 'VibeSDR';
function syncTitle() {
  if (typeof document === 'undefined') return;
  document.title = unread > 0 ? `${BASE_TITLE}: (Chat)` : BASE_TITLE;
}
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
    /* ★★ THE COMPOSER, at the end of the canned buttons: a frequency box, its unit, and the modes this
     *  receiver offers. Everything a listener can say here is still chosen from a list or typed as a number —
     *  no sentence can get through. Defaults to where the dial is now, which is what "check out" usually
     *  means, so the common case is two taps. */
    const wrap = document.createElement('div');
    wrap.className = 'chatCheckOut';
    const freq = document.createElement('input');
    freq.type = 'text'; freq.inputMode = 'decimal'; freq.placeholder = 'frequency';
    freq.className = 'chatFreq';
    const unit = document.createElement('select');
    for (const u of ['MHz', 'kHz', 'Hz']) { const o = document.createElement('option'); o.value = u; o.textContent = u; unit.appendChild(o); }
    const mode = document.createElement('select');
    const none = document.createElement('option'); none.value = ''; none.textContent = '(mode)'; mode.appendChild(none);
    for (const m of (deps?.modes?.() ?? Object.keys(MODE_LABEL))) {
      const o = document.createElement('option'); o.value = m; o.textContent = MODE_LABEL[m] || m.toUpperCase(); mode.appendChild(o);
    }
    const send = document.createElement('button');
    send.className = 'btn'; send.textContent = 'Hey, check out…';
    const fill = () => {
      const hz = deps?.freqHz?.() ?? 0;
      if (hz > 0 && !freq.value) { unit.value = 'MHz'; freq.value = (hz / 1e6).toFixed(3).replace(/0+$/, '').replace(/\.$/, ''); }
    };
    freq.onfocus = fill;
    send.onclick = () => {
      fill();
      const n = parseFloat(freq.value.replace(',', '.'));
      if (!Number.isFinite(n) || n <= 0) { freq.focus(); return; }
      const hz = Math.round(n * (unit.value === 'MHz' ? 1e6 : unit.value === 'kHz' ? 1e3 : 1));
      deps?.say('check_out', { hz, mode: mode.value || undefined });
      send.disabled = true;
      setTimeout(() => { send.disabled = false; }, 3000);
    };
    wrap.append(freq, unit, mode, send);
    list.appendChild(wrap);
  }
}

/** Called when the panel opens or closes, so the unread count can be cleared and stop counting. */
export function chatOpened(open: boolean) {
  isOpen = open;
  if (open) { unread = 0; deps?.onUnread(0); syncTitle(); }
}

/** A line arrived. */
export function onSaid(from: number, id: string, admin = false, extra?: { hz?: number; mode?: string }) {
  /* ★ "check out" writes its own sentence from the payload; every other phrase is a fixed string. A check-out
   *  with no usable frequency is dropped rather than drawn as a bare "Hey, check out" — see the server, which
   *  refuses to send one. */
  const text = id === 'check_out'
    ? (extra && Number(extra.hz) > 0 ? checkOutText(Number(extra.hz), extra.mode) : '')
    : TEXT[id];
  if (!text) return;                       // an id this build cannot draw — see the header note
  const log = $('chatLog');
  if (log) {
    const row = document.createElement('div');
    row.className = 'chatLine';
    const who = document.createElement('span');
    who.className = 'chatWho';
    // ★ "You" rather than your own number: everybody else sees an ordinal, and you know which is
    //   yours, but reading your own words back as a stranger's is oddly cold.
    /* ★★★ THE HANDLE STAYS AND "(admin)" IS ADDED TO IT — tgcfabian's suggestion, Stuart's shape:
     *   "User 3 (admin)", not "Admin". On a club receiver with several operators, replacing the
     *   number would make two admins indistinguishable, and following who said what is the whole
     *   point of having handles. ★ It also survives the lock changing hands: the server records
     *   what the sender WAS when the line was said, so history does not rewrite itself.
     * ★ Marked on your OWN lines too. "You (admin)" is worth knowing — it is the difference
     *   between a request and an instruction, and you may not remember you are still signed in. */
    const base = (dial && from === dial.you) ? 'You' : `User ${from}`;
    who.textContent = admin ? `${base} (admin)` : base;
    if (admin) who.classList.add('chatAdmin');
    const what = document.createElement('span');
    what.textContent = text;               // textContent, never innerHTML — see the header note
    /* ★★ A frequency somebody found is worth a tap. On a shared dial this moves the room, so it asks the same
     *  way any other tune does — through the host's own tuneTo, which obeys the etiquette and the limits. */
    if (id === 'check_out' && extra && Number(extra.hz) > 0 && deps?.tuneTo) {
      what.classList.add('chatGoTo');
      what.title = 'Tune this receiver there';
      what.onclick = () => deps?.tuneTo?.(Number(extra.hz), extra.mode);
    }
    row.append(who, what);
    log.appendChild(row);
    while (log.children.length > 40) log.removeChild(log.firstChild!);
    log.scrollTop = log.scrollHeight;
  }
  if (!isOpen && !(dial && from === dial.you)) { unread++; deps?.onUnread(unread); syncTitle(); }
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

/** ★★★ THE ROOM'S COUNT COMES FROM ONE PLACE (Stuart, 2026-09-20). This strip only ever redrew when a DIAL
 *  message arrived, and a listener LEAVING does not produce one — so after the second browser closed it still
 *  read "2 listening · User 0 is decoding" while the tuner banner had already gone back to "free to tune". Two
 *  readers of one fact, disagreeing on screen, which is the shape this project keeps paying for.
 *  ★ Alone in the room, nobody else can be tuning or decoding: those are cleared rather than left to rot. */
export function onListenerCount(n: number) {
  if (!dial) return;
  dial = { ...dial, listeners: n, ...(n <= 1 ? { tuner: 0, decoding: false } : {}) };
  onDial(dial);
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
