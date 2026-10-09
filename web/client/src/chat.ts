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
  shareSummary, parseShared, sharedLineText, shareTuneStep, shareFromManual, manualFieldFrom,
  MANUAL_SHARE_MODES, SHARE_MODE_LABEL, type ShareOut, type SharedStation,
} from '../../../src/services/chatShare';
import { answerBand, isAnswer, isQuestion, phraseDecoder, phraseWiki, ANSWER_WINDOW_MS, DIAL_PHRASES } from '../../../src/services/dialChat';

/** The vocabulary, in the order a conversation actually runs: ask, act, answer, thank.
 *  ★★ TAKEN FROM JR, NOT INVENTED HERE — `Canned.fmdx` in Chat.swift, plus the long-decode lines.
 *     One vocabulary across the watch, the phone and the browser, so a conversation reads the same
 *     wherever it is held. Change a wording freely; change or remove an ID and the two ends stop
 *     understanding each other. */
export const PHRASES: Array<{ id: string; text: string; group?: string; key?: string }> = [
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
  // ★★★ ANSWERS TO "Anyone know what this is?" (Stuart, 2026-10-09) — one "It's …" row, drawn only while somebody
  //   else's question is open and only the half for the band the dial is on. Band, wiki page and decoder come from the
  //   app's dialChat.ts (one rule, two readers); the ids and wording are listed here like every other phrase.
  { id: 'not_sure', text: 'Not sure, sorry', group: 'answer', key: 'Not sure' },
  { id: 'is_wefax',  text: "It's WEFAX (weather fax)", group: 'answer', key: 'WEFAX' },
  { id: 'is_rtty',   text: "It's RTTY", group: 'answer', key: 'RTTY' },
  { id: 'is_navtex', text: "It's NAVTEX", group: 'answer', key: 'NAVTEX' },
  { id: 'is_sstv',   text: "It's SSTV (slow-scan TV)", group: 'answer', key: 'SSTV' },
  { id: 'is_ft8',    text: "It's FT8", group: 'answer', key: 'FT8' },
  { id: 'is_cw',     text: "It's Morse (CW)", group: 'answer', key: 'Morse' },
  { id: 'is_ssb',    text: "It's SSB voice", group: 'answer', key: 'SSB voice' },
  { id: 'is_drm',    text: "It's DRM (digital radio)", group: 'answer', key: 'DRM' },
  { id: 'is_stanag', text: "It's STANAG (military data)", group: 'answer', key: 'STANAG' },
  { id: 'is_ale',    text: "It's ALE", group: 'answer', key: 'ALE' },
  { id: 'is_hfdl',   text: "It's HFDL (aircraft data)", group: 'answer', key: 'HFDL' },
  { id: 'is_codar',  text: "It's CODAR (ocean radar)", group: 'answer', key: 'CODAR' },
  { id: 'is_oth',    text: "It's over-the-horizon radar", group: 'answer', key: 'OTH radar' },
  { id: 'is_time',   text: "It's a time signal", group: 'answer', key: 'Time signal' },
  { id: 'is_dmr',    text: "It's DMR", group: 'answer', key: 'DMR' },
  { id: 'is_dstar',  text: "It's D-STAR", group: 'answer', key: 'D-STAR' },
  { id: 'is_p25',    text: "It's P25", group: 'answer', key: 'P25' },
  { id: 'is_nxdn',   text: "It's NXDN", group: 'answer', key: 'NXDN' },
  { id: 'is_dpmr',   text: "It's dPMR", group: 'answer', key: 'dPMR' },
  { id: 'is_pocsag', text: "It's POCSAG (pager)", group: 'answer', key: 'POCSAG' },
  { id: 'is_aprs',   text: "It's APRS", group: 'answer', key: 'APRS' },
  { id: 'is_adsb',   text: "It's ADS-B (aircraft)", group: 'answer', key: 'ADS-B' },
  { id: 'is_acars',  text: "It's ACARS (aircraft data)", group: 'answer', key: 'ACARS' },
  { id: 'is_ais',    text: "It's AIS (ships)", group: 'answer', key: 'AIS' },
  { id: 'is_apt',    text: "It's a weather satellite (APT)", group: 'answer', key: 'Weather sat' },
  { id: 'is_fm_bc',  text: "It's an FM broadcast station", group: 'answer', key: 'FM broadcast' },
];

const TEXT: Record<string, string> = Object.fromEntries(PHRASES.map(p => [p.id, p.text]));
/** The label a phrase group's row starts with (the app: dialChat.ts PHRASE_GROUP_LABEL). */
const PHRASE_GROUP_LABEL: Record<string, string> = { sounds: 'This sounds', answer: "It's" };

/* ★★★ THE ONE PHRASE THAT CARRIES FACTS (Stuart, 2026-09-20): "Hey, check out 96.1 MHz Advanced RDS". A shared
 *  receiver is a room of people finding things, and the canned vocabulary let them agree who tunes but never
 *  say WHAT they found — the one thing worth saying on a radio.
 *  ★★★ AND SINCE 2026-10-01 IT IS "CHECK OUT [BOOKMARK] [MANUAL]": a station picked from the frequency card's
 *     own search and bookmark lists, or a frequency + demodulator typed inline. Built by src/services/chatShare.ts
 *     — the same file the app uses — which never reads a bookmark's label: the room hears the name THE RECEIVER
 *     knows. */

export type DialState = {
  mode: string; tuner: number; mine: boolean; you: number;
  listeners: number; decoding?: boolean;
};

type Deps = {
  /** Send a phrase id to the server. */
  say: (id: string) => void;
  /** Send a station share — the frame chatShare built (numbers and ids, never a label). */
  share?: (out: ShareOut) => void;
  /** ★★ BOOKMARK: hand off to the frequency card's search + bookmark lists in PICK mode. The host closes this
   *  panel, and when a row is chosen reopens it and calls `done` — the title is the listener's own label,
   *  shown in the draft here only; `out` is what is sent. Never called back if the pick is abandoned. */
  pickStation?: (done: (p: { title: string; out: ShareOut }) => void) => void;
  /** The DAB service playing now, when DAB is on — the manual field then starts empty (not FM hiss). */
  dabNow?: () => { channel: string; sid?: number; eid?: number; label?: string } | null;
  /** Can this receiver play DAB? A DAB bookmark is not offered, and a DAB TUNE not attempted, where not. */
  dabCapable?: () => boolean;
  /** Is this listener the unlocked admin (a spectator dial is theirs to tune)? */
  isAdmin?: () => boolean;
  /** TUNE on a shared line — the host's ordinary tune path (dabGoTo / the bookmark tune). */
  tuneShare?: (s: SharedStation) => void;
  /** The current dial — the manual field's demodulator starts on it. */
  tuned?: () => { frequency: number; mode: string; bandwidthLow?: number; bandwidthHigh?: number } | null;
  /** The modes and decoders this receiver actually offers, so the picker cannot suggest a dead one. */
  modes?: () => string[];
  /** Where the dial is now — the manual field starts there, because "check out" usually means "here". */
  freqHz?: () => number;
  /** Raise the unread count on whatever button opens this. */
  onUnread: (n: number) => void;
  /** ★ Open the decoder an answer names — the decoders panel's own button. Only offered when canDecode says so. */
  openDecoder?: (d: string) => void;
  /** ★ Does this receiver run that decoder? DECODE is drawn only when it does (never a dead key). */
  canDecode?: (d: string) => boolean;
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
    const mkKey = (p: { id: string; text: string }, label: string) => {
      const b = document.createElement('button');
      // ★ A SHORT phrase ("Thanks!", "Nice catch!") pairs up on a phone — see #chatPhrases .chatShort (2026-10-08).
      b.className = label.length <= 22 ? 'btn chatShort' : 'btn';
      b.textContent = label;
      b.onclick = () => {
        deps?.say(p.id);
        // ★ NO LOCAL ECHO. The server is what everybody else sees, so waiting for it to come back
        //   is the only way this client's transcript matches theirs — and if flood control drops
        //   the phrase, nothing should be shown that other people never received.
        b.disabled = true;
        setTimeout(() => { b.disabled = false; }, 3000);   // mirrors the server's own 3s gap
      };
      return b;
    };
    /* ★ A phrase GROUP ("This sounds" Awesome · Weird …) is one row — its label, then a short key per member — where
     *  its first member stands (Stuart, 2026-10-08). The app's ChatDrawer does the same. */
    const done = new Set<string>();
    for (const p of PHRASES) {
      if (!p.group) { list.appendChild(mkKey(p, p.text)); continue; }
      if (done.has(p.group)) continue;
      done.add(p.group);
      const row = document.createElement('div');
      row.className = 'chatGroup';
      if (p.group === 'answer') { row.id = 'chatAnswerRow'; row.hidden = true; }
      const lbl = document.createElement('span');
      lbl.className = 'chatGroupLbl';
      lbl.textContent = PHRASE_GROUP_LABEL[p.group] ?? '';
      row.appendChild(lbl);
      for (const q of PHRASES.filter((x) => x.group === p.group)) {
        const k = mkKey(q, q.key ?? q.text);
        k.dataset.id = q.id;
        row.appendChild(k);
      }
      list.appendChild(row);
    }
    /* ★★★ "CHECK OUT [BOOKMARK] [MANUAL]" — ONE ROW, FIRST, because it is the one key that says WHAT you found
     *  (Stuart, 2026-10-01: the station sharing "is a bit of a rubbish UI"). It replaced two things: a
     *  "📻 Share a station…" list of now-playing plus every bookmark in one flat run, and a frequency/unit/mode
     *  composer that sat open under the phrases whether anybody wanted it or not.
     *  ★ BOOKMARK hands off to the frequency card's own SEARCH and BOOKMARKS lists in a pick mode (main.ts
     *    beginSharePick): the chat drops away while you choose, then comes back with the choice POPULATED as a
     *    draft — two steps, and nothing is sent until SEND. Those lists are the ones a listener already knows,
     *    with EiBi and the receiver's learnt stations in them; a second, poorer list here was the problem.
     *  ★ MANUAL is a frequency and a demodulator, inline. ✗ NOT DAB: a typed DAB share would need the
     *    multiplex loaded to name a service — the learnt DAB stations are in the Bookmark lists instead. */
    const box = document.createElement('div');
    box.className = 'chatShare';
    const row = document.createElement('div');
    row.className = 'chatCheckOut';
    const lead = document.createElement('span');
    lead.className = 'chatCheckLead'; lead.textContent = 'Check out';
    const bmBtn = document.createElement('button');
    bmBtn.className = 'btn chatShareBtn'; bmBtn.textContent = '📻 Bookmark';
    bmBtn.title = 'Pick a station from the search and bookmark lists';
    const manBtn = document.createElement('button');
    manBtn.className = 'btn chatShareBtn'; manBtn.textContent = 'Manual';
    manBtn.title = 'Type a frequency and choose a demodulator';
    row.append(lead, bmBtn, manBtn);

    // ── The draft: what Bookmark chose, waiting for SEND ─────────────────────
    const draft = document.createElement('div');
    draft.className = 'chatShareDraft'; draft.hidden = true;
    const draftTxt = document.createElement('span');
    draftTxt.className = 'chatShareDraftTxt';
    const draftSend = document.createElement('button');
    draftSend.className = 'btn'; draftSend.textContent = 'Send';
    const draftX = document.createElement('button');
    draftX.className = 'btn'; draftX.textContent = '×'; draftX.title = 'Discard';
    draft.append(draftTxt, draftSend, draftX);
    let draftOut: ShareOut | null = null;
    const clearDraft = () => { draftOut = null; draft.hidden = true; draftTxt.textContent = ''; };
    draftX.onclick = clearDraft;
    draftSend.onclick = () => { if (draftOut) { sendShare(draftOut, draftSend); clearDraft(); } };
    showDraft = (title, out) => {
      draftOut = out;
      // ★ The NAME is the listener's own label and is shown only here — textContent, and never sent: the
      //   room hears the name THE RECEIVER knows (chatShare.ts). The summary says what will actually go.
      draftTxt.textContent = `Check out ${title ? `${title} · ` : ''}${shareSummary(out)}`;
      draft.hidden = false;
      manual.hidden = true; manBtn.classList.remove('on');
    };

    // ── Manual: a frequency, its unit, a demodulator ─────────────────────────
    const manual = document.createElement('div');
    manual.className = 'chatManual'; manual.hidden = true;
    const freq = document.createElement('input');
    freq.type = 'text'; freq.inputMode = 'decimal'; freq.placeholder = 'frequency';
    freq.className = 'chatFreq'; freq.spellcheck = false; freq.autocomplete = 'off';
    const unit = document.createElement('select');
    for (const u of ['MHz', 'kHz']) { const o = document.createElement('option'); o.value = u; o.textContent = u; unit.appendChild(o); }
    const mode = document.createElement('select');
    /* ★★ Only the demodulators this receiver offers. The web's MODES spell CW as cwu/cwl; the share's closed
     *  list says "cw", so either of those offers it. Unknown (no modes() dep) = the whole manual list. */
    const offered = deps?.modes?.();
    for (const m of MANUAL_SHARE_MODES) {
      if (offered && !(offered.includes(m) || (m === 'cw' && (offered.includes('cwu') || offered.includes('cwl'))))) continue;
      const o = document.createElement('option'); o.value = m; o.textContent = SHARE_MODE_LABEL[m] || m.toUpperCase(); mode.appendChild(o);
    }
    const manSend = document.createElement('button');
    manSend.className = 'btn'; manSend.textContent = 'Send';
    manual.append(freq, unit, mode, manSend);
    /* ★ STARTS WHERE THE DIAL IS — "check out" usually means "here", so the common case is Manual → Send.
     *  ✗ Not while DAB is on: the frequency under a multiplex is not a station; Bookmark has the service. */
    const prefill = () => {
      if (deps?.dabNow?.()) { freq.value = ''; return; }
      const f = manualFieldFrom(deps?.freqHz?.() ?? 0);
      freq.value = f.value; unit.value = f.unit;
      const cur = (deps?.tuned?.()?.mode ?? '').toLowerCase();
      const want = cur === 'cwu' || cur === 'cwl' ? 'cw' : cur;
      if (Array.from(mode.options).some((o) => o.value === want)) mode.value = want;
    };
    manBtn.onclick = () => {
      manual.hidden = !manual.hidden;
      manBtn.classList.toggle('on', !manual.hidden);
      if (!manual.hidden) { prefill(); freq.focus(); freq.select(); }
    };
    const sendManual = () => {
      const out = shareFromManual(freq.value, unit.value as 'MHz' | 'kHz', mode.value);
      if (!out) { freq.focus(); freq.select(); return; }     // ★ nothing is sent that is not a frequency
      sendShare(out, manSend);
      manual.hidden = true; manBtn.classList.remove('on');
    };
    manSend.onclick = sendManual;
    freq.onkeydown = (e) => { if (e.key === 'Enter') { e.preventDefault(); sendManual(); } };

    bmBtn.onclick = () => {
      if (!deps?.pickStation) return;
      manual.hidden = true; manBtn.classList.remove('on');
      deps.pickStation((p) => showDraft?.(p.title, p.out));
    };

    box.append(row, draft, manual);
    list.insertBefore(box, list.firstChild);
  }
}

/** Set by initChat: put a picked station in the draft row. */
let showDraft: ((title: string, out: ShareOut) => void) | null = null;

/** ★★ THE ONE SEND PATH for a share — the frame chatShare built, and nothing else. */
function sendShare(out: ShareOut | null, btn?: HTMLButtonElement) {
  if (!out) return;
  deps?.share?.(out);
  if (btn) { btn.disabled = true; setTimeout(() => { btn.disabled = false; }, 3000); }   // the server's 3 s gap
}

/** Called when the panel opens or closes, so the unread count can be cleared and stop counting. */
export function chatOpened(open: boolean) {
  isOpen = open;
  if (open) { unread = 0; deps?.onUnread(0); syncTitle(); refreshAnswerRow(); }   // ★ the dial may have changed band
}

/** A line arrived. */
/** ★ Each user number's `since` stamp as last heard — see onSaid. */
/* ★★ THE ANSWER ROW (Stuart, 2026-10-09). "Anyone know what this is?" from somebody ELSE opens it for ANSWER_WINDOW_MS;
 *  the first answer closes it; only the answers for the band the dial is on are shown (dialChat answerBand — the app
 *  draws the same row from the same rule). */
let questionOpenUntil = 0;
let questionTimer: ReturnType<typeof setTimeout> | null = null;
const BAND_OF: Record<string, string | undefined> = Object.fromEntries(DIAL_PHRASES.map((p) => [p.id, p.band]));
function refreshAnswerRow() {
  const row = $('chatAnswerRow');
  if (!row) return;
  const open = questionOpenUntil > Date.now();
  row.hidden = !open;
  if (!open) return;
  const band = answerBand(deps?.freqHz?.() ?? 0);
  for (const k of Array.from(row.querySelectorAll('button[data-id]')) as HTMLButtonElement[]) {
    const b = BAND_OF[k.dataset.id ?? ''];
    k.hidden = !!b && b !== band;
  }
}
function setQuestionOpen(until: number) {
  questionOpenUntil = until;
  if (questionTimer) { clearTimeout(questionTimer); questionTimer = null; }
  if (until) questionTimer = setTimeout(() => { questionOpenUntil = 0; refreshAnswerRow(); }, Math.max(0, until - Date.now()));
  refreshAnswerRow();
}

const sinceByUser = new Map<number, number>();
export function onSaid(from: number, id: string, admin = false, msg?: Record<string, unknown>) {
  /* ★★ A share is drawn from the SERVER's line (chatShare.parseShared): the station as THE RECEIVER named it.
   *  The sender's label never travelled and there is no client name field to trust. One with nothing usable is
   *  dropped rather than drawn as a bare "shared". Every other phrase is a fixed string. */
  const share = id === 'check_out' ? parseShared(msg) : null;
  const text = id === 'check_out' ? (share ? sharedLineText(share) : '') : TEXT[id];
  if (!text) return;                       // an id this build cannot draw — see the header note
  const mine = !!dial && from === dial.you;
  if (isQuestion(id) && !mine) setQuestionOpen(Date.now() + ANSWER_WINDOW_MS);
  else if (isAnswer(id)) setQuestionOpen(0);
  const log = $('chatLog');
  /* ★★ "— User 1 joined 17:40 —" (Stuart's option B, 2026-10-08; the app's SDRScreen onSaid does the same). The server
   *  stamps every line with WHEN its number was given (`since`); a different stamp on a number already heard means the
   *  number has passed to somebody new — one quiet divider first. The same person back within the hold: no divider. */
  const since = typeof msg?.since === 'number' ? (msg.since as number) : 0;
  const seen = sinceByUser.get(from);
  if (since) sinceByUser.set(from, since);
  if (log && since && seen !== undefined && seen !== since) {
    const j = document.createElement('div');
    j.className = 'chatJoin';
    const who = (dial && from === dial.you) ? 'You' : `User ${from}`;
    j.textContent = `— ${who} joined ${new Date(since * 1000).toISOString().slice(11, 16)} UTC —`;
    log.appendChild(j);
  }
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
    /* ★ An answer: WIKI opens its Signal Identification Wiki page (a real link, new tab, the page unmodified);
     *  DECODE starts the decoder that reads it, when this receiver runs one. */
    const wikiUrl = phraseWiki(id);
    if (wikiUrl) {
      const a = document.createElement('a');
      a.className = 'btn chatTune';
      a.textContent = 'WIKI';
      a.href = wikiUrl; a.target = '_blank'; a.rel = 'noopener noreferrer';
      a.title = 'Read about this signal on the Signal Identification Wiki';
      row.append(a);
    }
    const dec = phraseDecoder(id);
    if (dec && deps?.openDecoder && deps.canDecode?.(dec)) {
      const d = document.createElement('button');
      d.className = 'btn chatTune';
      d.textContent = 'DECODE';
      d.title = 'Open the decoder for this signal';
      d.onclick = () => deps?.openDecoder?.(dec);
      row.append(d);
    }
    // ★ Same rule as the decoder box: only follow a reader who is at the bottom (B11).
    const atBottom = log.scrollHeight - log.scrollTop - log.clientHeight < 24;
    log.appendChild(row);
    while (log.children.length > 40) log.removeChild(log.firstChild!);
    if (atBottom) log.scrollTop = log.scrollHeight;
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
