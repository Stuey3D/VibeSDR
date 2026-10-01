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

import {
  shareFromBookmark, shareFromTuned, shareSummary, parseShared, sharedLineText, shareTuneStep,
  type ShareOut, type SharedStation,
} from '../../../src/services/chatShare';
import type { UserBookmark } from '../../../src/services/userBookmarks';

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
 *  ★★★ AND SINCE 2026-10-01 IT IS "SHARE A STATION": the one you are on (a DAB SERVICE when DAB is on), one of
 *     your bookmarks, or a frequency typed in the composer. Built by src/services/chatShare.ts — the same file
 *     the app uses — which never reads a bookmark's label: the room hears the name THE RECEIVER knows. */
const MODE_LABEL: Record<string, string> = {
  wfm: 'WFM', nfm: 'NFM', am: 'AM', usb: 'USB', lsb: 'LSB', cwu: 'CW-U', cwl: 'CW-L',
  dab: 'DAB', rds: 'Advanced RDS', rtty: 'RTTY', navtex: 'NAVTEX', wefax: 'WEFAX',
  sstv: 'SSTV', ft8: 'FT8 / FT4', time: 'Time signal',
};

export type DialState = {
  mode: string; tuner: number; mine: boolean; you: number;
  listeners: number; decoding?: boolean;
};

type Deps = {
  /** Send a phrase id to the server. */
  say: (id: string) => void;
  /** Send a station share — the frame chatShare built (numbers and ids, never a label). */
  share?: (out: ShareOut) => void;
  /** This listener's own bookmarks — their labels are shown in the picker HERE and never sent. */
  bookmarks?: () => UserBookmark[];
  /** The DAB service playing now, when DAB is on — what "Now playing" shares instead of the dial. */
  dabNow?: () => { channel: string; sid?: number; eid?: number; label?: string } | null;
  /** Can this receiver play DAB? A DAB bookmark is not offered, and a DAB TUNE not attempted, where not. */
  dabCapable?: () => boolean;
  /** Is this listener the unlocked admin (a spectator dial is theirs to tune)? */
  isAdmin?: () => boolean;
  /** TUNE on a shared line — the host's ordinary tune path (dabGoTo / the bookmark tune). */
  tuneShare?: (s: SharedStation) => void;
  /** The current dial, for "Now playing". */
  tuned?: () => { frequency: number; mode: string; bandwidthLow?: number; bandwidthHigh?: number } | null;
  /** The modes and decoders this receiver actually offers, so the picker cannot suggest a dead one. */
  modes?: () => string[];
  /** Where the dial is now — the frequency box starts there, because "check out" usually means "here". */
  freqHz?: () => number;
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
      /* ★★ DAB TOO (Stuart, 2026-10-01: the share "can't share DAB stations"). An empty box while DAB is on
       *  means "this" — the SERVICE playing, not the analogue frequency under it, which opens as FM hiss. A
       *  typed frequency with the DAB mode is that multiplex. */
      const dab = !freq.value ? deps?.dabNow?.() : null;
      if (dab) { sendShare(shareFromTuned({ frequency: 0, mode: 'dab' }, dab), send); return; }
      fill();
      const n = parseFloat(freq.value.replace(',', '.'));
      if (!Number.isFinite(n) || n <= 0) { freq.focus(); return; }
      const hz = Math.round(n * (unit.value === 'MHz' ? 1e6 : unit.value === 'kHz' ? 1e3 : 1));
      const out = shareFromBookmark({ frequency: hz, mode: mode.value || undefined });
      if (!out) { freq.focus(); return; }
      sendShare(out, send);
    };
    wrap.append(freq, unit, mode, send);

    /* ★★★ SHARE A STATION — the picker: what is playing now, then this listener's own bookmarks. Their labels
     *  are shown here, on their own screen; what is SENT is the row's ShareOut, which carries no label. */
    const pickBtn = document.createElement('button');
    pickBtn.className = 'btn chatShareBtn';
    pickBtn.textContent = '📻 Share a station…';
    const pick = document.createElement('div');
    pick.className = 'chatSharePick';
    pick.hidden = true;
    pickBtn.onclick = () => {
      pick.hidden = !pick.hidden;
      if (!pick.hidden) renderPicker(pick, pickBtn);
    };
    list.insertBefore(pick, list.firstChild);
    list.insertBefore(pickBtn, list.firstChild);
    list.appendChild(wrap);
  }
}

/** ★★ THE ONE SEND PATH for a share — the frame chatShare built, and nothing else. */
function sendShare(out: ShareOut | null, btn?: HTMLButtonElement) {
  if (!out) return;
  deps?.share?.(out);
  if (btn) { btn.disabled = true; setTimeout(() => { btn.disabled = false; }, 3000); }   // the server's 3 s gap
}

function renderPicker(pick: HTMLElement, pickBtn: HTMLButtonElement) {
  pick.innerHTML = '';
  const note = document.createElement('div');
  note.className = 'chatShareNote';
  note.textContent = 'Shares the frequency and mode only — the room sees the name this receiver knows.';
  pick.appendChild(note);
  const rows: Array<{ title: string; out: ShareOut }> = [];
  const dab = deps?.dabNow?.() ?? null;
  const t = deps?.tuned?.() ?? null;
  const now = t || dab ? shareFromTuned(t ?? { frequency: 0, mode: 'dab' }, dab) : null;
  if (now) rows.push({ title: dab?.label ? `Now playing — ${dab.label.trim()}` : 'Now playing', out: now });
  const canDab = deps?.dabCapable?.() ?? false;
  for (const b of deps?.bookmarks?.() ?? []) {
    const out = shareFromBookmark(b);
    if (!out || (out.kind === 'dab' && !canDab)) continue;   // ★ never offer what this receiver cannot play
    rows.push({ title: b.name, out });
  }
  if (!rows.length) {
    const none = document.createElement('div');
    none.className = 'chatShareNote';
    none.textContent = 'Nothing to share yet — tune a station or save a bookmark.';
    pick.appendChild(none);
  }
  for (const r of rows) {
    const b = document.createElement('button');
    b.className = 'btn chatShareRow';
    const a = document.createElement('span'); a.textContent = r.title;               // textContent — never innerHTML
    const d = document.createElement('span'); d.className = 'chatShareDetail'; d.textContent = shareSummary(r.out);
    b.append(a, d);
    b.onclick = () => { sendShare(r.out, pickBtn); pick.hidden = true; };
    pick.appendChild(b);
  }
}

/** Called when the panel opens or closes, so the unread count can be cleared and stop counting. */
export function chatOpened(open: boolean) {
  isOpen = open;
  if (open) { unread = 0; deps?.onUnread(0); syncTitle(); }
}

/** A line arrived. */
export function onSaid(from: number, id: string, admin = false, msg?: Record<string, unknown>) {
  /* ★★ A share is drawn from the SERVER's line (chatShare.parseShared): the station as THE RECEIVER named it.
   *  The sender's label never travelled and there is no client name field to trust. One with nothing usable is
   *  dropped rather than drawn as a bare "shared". Every other phrase is a fixed string. */
  const share = id === 'check_out' ? parseShared(msg) : null;
  const text = id === 'check_out' ? (share ? sharedLineText(share) : '') : TEXT[id];
  if (!text) return;                       // an id this build cannot draw — see the header note
  const log = $('chatLog');
  if (log) {
    const row = document.createElement('div');
    row.className = 'chatLine';
    const who = document.createElement('span');
    who.className = 'chatWho';
    // ★ "You" rather than your own number: everybody else sees an ordinal, and you know which is
    //   yours, but reading your own words back as a stranger's is oddly cold.
    /* ★★★ THE HANDLE STAYS AND "(admin)" IS ADDED TO IT — TGCFabian's suggestion, Stuart's shape:
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
    row.append(who, what);
    /* ★★ A station somebody found is worth a tap — but ARRIVING tunes nothing. TUNE is this listener's choice,
     *  through the host's ordinary tune path; on a shared dial somebody else is on, it asks first. */
    if (share && deps?.tuneShare) {
      const go = document.createElement('button');
      go.className = 'btn chatTune';
      go.textContent = 'TUNE';
      go.title = 'Tune this receiver there';
      go.onclick = () => onShareTune(share, row);
      row.append(go);
    }
    log.appendChild(row);
    while (log.children.length > 40) log.removeChild(log.firstChild!);
    log.scrollTop = log.scrollHeight;
  }
  if (!isOpen && !(dial && from === dial.you)) { unread++; deps?.onUnread(unread); syncTitle(); }
}

/** TUNE on a shared line: go, ask first, or say why not (chatShare.shareTuneStep — the same rule as the app). */
function onShareTune(s: SharedStation, row: HTMLElement) {
  const step = shareTuneStep(s, dial, { admin: deps?.isAdmin?.() ?? false, dabCapable: deps?.dabCapable?.() ?? false });
  if (step === 'refused') { onDialRefused(); return; }
  if (step === 'no-dab') {
    const strip = $('dialStrip');
    if (strip) { strip.hidden = false; strip.textContent = 'This receiver cannot play DAB.'; }
    return;
  }
  if (step === 'tune') { deps?.tuneShare?.(s); return; }
  // ★ ASK: the banner's ASK TO TUNE, offered as the canned "Can I tune?" — tuning anyway stays your call.
  if (row.nextElementSibling?.classList.contains('chatAsk')) row.nextElementSibling.remove();
  const ask = document.createElement('div');
  ask.className = 'chatAsk';
  const others = Math.max(1, (dial?.listeners ?? 2) - 1);
  const why = document.createElement('span');
  why.textContent = `${others === 1 ? 'Somebody else is' : `${others} others are`} listening — tuning moves it for everyone.`;
  const askBtn = document.createElement('button');
  askBtn.className = 'btn'; askBtn.textContent = 'Ask "Can I tune?"';
  askBtn.onclick = () => { deps?.say('ask_tune'); ask.remove(); };
  const now = document.createElement('button');
  now.className = 'btn'; now.textContent = 'Tune now';
  now.onclick = () => { ask.remove(); deps?.tuneShare?.(s); };
  const no = document.createElement('button');
  no.className = 'btn'; no.textContent = 'Cancel';
  no.onclick = () => ask.remove();
  ask.append(why, askBtn, now, no);
  row.after(ask);
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
