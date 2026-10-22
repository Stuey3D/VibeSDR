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
 *
 * ★★★ IN THE LISTENER'S OWN LANGUAGE (2026-10-22). Stuart in the UK and Kiko in Brazil on one dial: the id travels, so
 *     Stuart reads "Can I tune?" and Kiko reads "Posso sintonizar?". The WORDS live in i18n/chat/<lang>.json, written
 *     natively per meaning (chat only — owner-approved), and reach this file as chatWords.generated.ts
 *     (scripts/gen-chat-phrases.mjs). Which language: see chatLangChoice. Nothing outside the chat and the
 *     shared-tuner banner is translated — the rest of the app stays English.
 */

import { CHAT_PHRASE_META, CHAT_WORDS, CHAT_LANG_OF, CHAT_LANG_OF_COUNTRY, CHAT_LANG_FLAG,
         type ChatLang, type ChatChromeKey } from './chatWords.generated.ts';
export type { ChatLang } from './chatWords.generated.ts';

/** ★ `group`/`key`: phrases drawn as ONE row — a label and a short key each ("This sounds" Awesome · Weird …).
 *  ★ An ANSWER (group 'answer') also says which band it belongs to, its Signal Identification Wiki page and — when this
 *    app has one — the decoder that reads it (i18n/chat/phrases.json). */
export type Phrase = { id: string; text: string; group?: string; key?: string;
                       band?: 'hf' | 'vhf'; wiki?: string; decoder?: 'rtty' | 'navtex' | 'wefax' | 'sstv' | 'time' };

/** ★★ THE SIGNAL IDENTIFICATION WIKI (Stuart, 2026-10-09: "Anyone know what this is with no way of someone telling
 *  them"). Linked, never copied or framed: opened in the system's in-app browser (SFSafariViewController / Chrome
 *  Custom Tabs) — the real site, its address bar, its own attribution, exactly as published. Every page below was
 *  checked to exist (HTTP 200) on 2026-10-09; one without a page (time signals) simply has no link. */
export const SIGID_HOME = 'https://www.sigidwiki.com/wiki/Signal_Identification_Guide';
const SIGID_PAGE = 'https://www.sigidwiki.com/wiki/';

/* ── ★★★ WHICH LANGUAGE (Stuart, 2026-10-22: "we detect the listeners country and offer the appropriate, and where no
 *  appropriate we offer English and we offer a little drop down menu … such as an english person on holiday to
 *  portugal listening on a local friends laptop") ─────────────────────────────────────────────────────────────────
 *  In order, the first that answers:
 *    1. PICKED — the chat's own language menu, remembered on this device ('auto' = not picked);
 *    2. DEVICE — the phone's / browser's language list, first tag we have words for. Before the country because it is
 *       the person's OWN setting: a Brazilian with an English phone gets English, the Englishman abroad keeps his;
 *    3. COUNTRY — the server's GeoIP answer for THIS listener ("cc", sent to its own socket only), for a device whose
 *       language we do not speak. Only one-language countries map (BE, CH, CA … pick nothing);
 *    4. ENGLISH.
 *  ★ It changes only how THIS device draws the chat. What is SENT is an id, in every language. */
export const CHAT_LANG_NAMES: Readonly<Record<ChatLang, string>> =
  Object.fromEntries(Object.entries(CHAT_WORDS).map(([k, v]) => [k, v.name])) as Record<ChatLang, string>;
export const isChatLang = (x: unknown): x is ChatLang => typeof x === 'string' && x in CHAT_WORDS;

/** The first of these language tags ("pt-BR", "en_GB", "de") that we have words for, or null. */
export function deviceChatLang(tags: readonly string[] | null | undefined): ChatLang | null {
  for (const t of tags ?? []) {
    const primary = String(t).toLowerCase().split(/[-_]/)[0];
    if (primary && CHAT_LANG_OF[primary]) return CHAT_LANG_OF[primary];
  }
  return null;
}
/** The language of a one-language country ("BR" → pt-BR), or null. */
export function countryChatLang(cc: string | null | undefined): ChatLang | null {
  return (cc && CHAT_LANG_OF_COUNTRY[String(cc).toUpperCase()]) || null;
}
/** ★ The choice itself — a pure function, so the order is pinned by scripts/test_chat_i18n.ts. */
export function chatLangChoice(o: { picked?: string | null; device?: readonly string[] | null; cc?: string | null }): ChatLang {
  if (isChatLang(o.picked)) return o.picked;
  return deviceChatLang(o.device) ?? countryChatLang(o.cc) ?? 'en';
}

let lang: ChatLang = 'en';
let langIn: { picked?: string | null; device?: readonly string[] | null; cc?: string | null } = {};
let words = CHAT_WORDS.en;
const langListeners = new Set<(l: ChatLang) => void>();
/** The language the chat is drawn in now. */
export function chatLang(): ChatLang { return lang; }
/** ★ Feed the choice any of its inputs as they become known (the picked one from storage, the device's list at start,
 *  the server's cc when it arrives); the rest are kept. Returns the language now in force. */
export function setChatLangInputs(next: { picked?: string | null; device?: readonly string[] | null; cc?: string | null }): ChatLang {
  const wasDetected = chatLangDetected(), was = lang;
  langIn = { ...langIn, ...next };
  const l = chatLangChoice(langIn);
  if (l !== was) { lang = l; words = CHAT_WORDS[l]; rebuild(); }
  // ★ Told on a change of language OR of what Default would pick (the country arriving while a language is picked:
  //   the menu's Default entry says what it resolves to). A copy of the set — a listener may re-subscribe while told.
  if (l !== was || chatLangDetected() !== wasDetected) for (const f of [...langListeners]) f(l);
  return lang;
}
/** What the menu shows as chosen: 'auto' unless a language was picked. */
export function chatLangPicked(): ChatLang | 'auto' { return isChatLang(langIn.picked) ? langIn.picked : 'auto'; }
/** What Default resolves to right now (the device, else the country, else English) — whatever was picked. */
export function chatLangDetected(): ChatLang { return chatLangChoice({ ...langIn, picked: null }); }

/* ── ★★ THE LANGUAGE MENU'S LABELS (Stuart, 2026-10-22): "Default", then each language with a small flag and its own
 *  name — 🇧🇷 Português (Brasil). Default SAYS what it resolves to ("Default · 🇧🇷 Português (Brasil)"), so a listener
 *  can see what was detected before overriding it; the closed key shows the same. ─────────────────────────────────── */
/** ★ Can this platform draw a flag emoji? Windows' emoji font draws a regional-indicator pair as two plain letters, so
 *  there the flag is a small COUNTRY-CODE badge instead — never a broken glyph. Everything else we run on (iOS, iPadOS,
 *  macOS, Android, tvOS, Linux and ChromeOS browsers) draws them. Fed what the platform says about itself. */
export function flagsDrawn(platform: string | null | undefined): boolean {
  return !/^win|windows/i.test(platform ?? '');
}
/** A language's flag: the emoji where flags are drawn, else null (draw `CHAT_LANG_FLAG[l]` as a badge). */
export function chatLangFlag(l: ChatLang, emoji: boolean): { emoji: string | null; cc: string } {
  const cc = CHAT_LANG_FLAG[l];
  return { emoji: emoji ? String.fromCodePoint(...[...cc].map((c) => 0x1f1e6 + c.charCodeAt(0) - 65)) : null, cc };
}
/** One entry of the menu as plain text: "🇧🇷 Português (Brasil)", or "BR Português (Brasil)" without flags. */
export function chatLangLabel(l: ChatLang, emoji: boolean): string {
  const f = chatLangFlag(l, emoji);
  return `${f.emoji ?? f.cc} ${CHAT_LANG_NAMES[l]}`;
}
/** The Default entry, and the closed key while nothing is picked: "Default · 🇧🇷 Português (Brasil)" — "Default" in the
 *  language showing now. */
export function chatLangDefaultLabel(emoji: boolean): string {
  return `${chatT('langDefault')} · ${chatLangLabel(chatLangDetected(), emoji)}`;
}
/** The closed key: what is in force, said the way the menu says it. */
export function chatLangKeyLabel(emoji: boolean): string {
  const p = chatLangPicked();
  return p === 'auto' ? chatLangDefaultLabel(emoji) : chatLangLabel(p, emoji);
}

/** Be told when the language changes (a pick, or a cc arriving). Returns the unsubscribe. */
export function onChatLang(f: (l: ChatLang) => void): () => void { langListeners.add(f); return () => { langListeners.delete(f); }; }

/** ★ The chat's own furniture in the chosen language — "{n}", "{who}", "{time}" … filled from `vars`. */
export function chatT(key: ChatChromeKey, vars: Record<string, string | number> = {}): string {
  return (words.chrome[key] ?? CHAT_WORDS.en.chrome[key]).replace(/\{(\w+)\}/g, (m, k) => (k in vars ? String(vars[k]) : m));
}

const BY_ID: Record<string, (typeof CHAT_PHRASE_META)[number]> = Object.fromEntries(CHAT_PHRASE_META.map(p => [p.id, p]));
/** In the order a conversation actually runs: ask, act, answer, thank — in the chosen language. ★ Rebuilt when the
 *  language changes (a live binding: readers see the new list). */
export let DIAL_PHRASES: Phrase[] = [];
/** The label a phrase group's row starts with ("This sounds", "O som tá"). */
export let PHRASE_GROUP_LABEL: Record<string, string> = {};
/** ★ What the shared-tuner banner flashes when a locked control is touched. Caps, like the banner it replaces. */
export let ASK_NUDGE_TEXT = '';
function rebuild() {
  DIAL_PHRASES = CHAT_PHRASE_META.map((p) => {
    const w = words.phrases[p.id];
    return { ...p, text: w.text, ...(w.key ? { key: w.key } : {}) };
  });
  PHRASE_GROUP_LABEL = { ...words.groups };
  ASK_NUDGE_TEXT = chatT('askNudge');
}
rebuild();

/** How long "Anyone know what this is?" keeps the answer row open, unless somebody answers first. */
export const ANSWER_WINDOW_MS = 5 * 60_000;
/** HF below 30 MHz, VHF/UHF above — the half of the answers that can be on the dial. */
export function answerBand(hz: number): 'hf' | 'vhf' { return hz > 0 && hz < 30_000_000 ? 'hf' : 'vhf'; }

/** The pad to draw: everything but the answers, plus — while a question is open — the answers for this band. */
export function padPhrases(questionOpen: boolean, hz: number): Phrase[] {
  const band = answerBand(hz);
  return DIAL_PHRASES.filter(p => p.group !== 'answer' || (questionOpen && (!p.band || p.band === band)));
}
/** ★★ THE TUNING CONVERSATION — the lines that go on the SHARED TUNER banner when they arrive (Stuart, 2026-10-09:
 *  "keep it to asking for tuning messages not the full signal ID messages"). Who may move the dial, and when; not
 *  the social lines, the "This sounds …" row, the answers or a shared station. */
/* ★ Each with its BANNER wording (i18n/chat/<lang>.json `banner`) — short enough for the narrowest banner (Stuart chose
 *  this over scrolling, 2026-10-09: the longest full line, "I'm running a decoder — can you wait please?", is 44
 *  characters and the banner ~34). The chat keeps the full sentence. "U2: " + these stays within 28, in every language
 *  (scripts/test_chat_i18n.ts). Which ids are tuning lines is the same in all of them — the English file says. */
export const isTuningPhrase = (id: string) => !!CHAT_WORDS.en.phrases[id]?.banner;
/** The banner's line for somebody's tuning phrase ("U2: Can I tune?"), or null for any other phrase. */
export function bannerLine(from: number, id: string): string | null {
  const t = words.phrases[id]?.banner;
  return t ? `${chatT('userShort', { n: from })}: ${t}` : null;
}

/** ★★ THE BANNER CANNOT BE SPAMMED (Stuart, 2026-10-09: "the issue becomes if users are spamming it"). The server
 *  already allows one phrase per listener every 3 s; on top, a line takes the banner only if nobody's has in the last
 *  ANY_GAP_MS, this person's has not in PERSON_GAP_MS, and it is not the same phrase they last had there. Anything held
 *  back still arrives in the chat and its unread count — it just cannot strobe the display. */
export class BannerGate {
  static ANY_GAP_MS = 10_000;
  static PERSON_GAP_MS = 30_000;
  private lastAny = -Infinity;
  private byPerson = new Map<number, { at: number; id: string }>();
  allow(from: number, id: string, now: number): boolean {
    if (!isTuningPhrase(id)) return false;
    if (now - this.lastAny < BannerGate.ANY_GAP_MS) return false;
    const p = this.byPerson.get(from);
    if (p && (now - p.at < BannerGate.PERSON_GAP_MS || p.id === id && now - p.at < 120_000)) return false;
    this.lastAny = now;
    this.byPerson.set(from, { at: now, id });
    return true;
  }
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
  return words.phrases[id]?.text ?? null;
}

/** What the server says about the room. `mode` is 'exclusive' on an ordinary receiver. */
export type DialState = {
  mode: string; tuner: number; mine: boolean; you: number;
  listeners: number; decoding: boolean;
} & AskState;

/* ── ★★★ ASK BEFORE TUNING (docs/TUNING-ETIQUETTE.md, 2026-10-21) ─────────────────────────────────────────────
 *  The server's opt-in etiquette lock. ONE READER for the browser and the app (web/client imports this file), so
 *  the two can never disagree about what the dial message means.
 *  ★★★ THE CLIENT IS TOLD, IT NEVER WORKS IT OUT (shared-dial contract). Nothing here decides whether a listener
 *      may tune: `tuneLocked` is the server's answer, and an old server that sends none means "not locked". */
export type AskState = {
  /** The lock is in force on this radio right now (the owner's switch, a shared dial, company). */
  askFirst?: boolean;
  /** THIS listener may not move the dial yet. */
  tuneLocked?: boolean;
  /** '' — not asked yet · 'open' — asked, waiting for a reply · 'no' — told to wait. */
  askState?: '' | 'open' | 'no';
  /** When `askState` ends (local ms clock), from the server's seconds-left on arrival. 0 = none. */
  askUntil?: number;
};

/** Read the ask fields off a `dial` message. Absent = not locked — a new client on an old server dims nothing. */
export function parseAsk(msg: Record<string, unknown>, now = Date.now()): AskState {
  if (msg.askFirst !== true) return { askFirst: false, tuneLocked: false, askState: '', askUntil: 0 };
  const st = msg.askState === 'open' || msg.askState === 'no' ? msg.askState : '';
  const secs = Number(msg.askSecs) || 0;
  return { askFirst: true, tuneLocked: msg.tuneLocked === true, askState: st, askUntil: st && secs > 0 ? now + secs * 1000 : 0 };
}

/** May this listener NOT tune right now? Only ever the server's answer. */
export function askLocked(d: AskState | null | undefined): boolean {
  return !!d && d.askFirst === true && d.tuneLocked === true;
}

/** ★★★ THE SHARED RECEIVER SETTINGS A LOCKED LISTENER MAY NOT SEND (Stuart, 2026-10-21: "lock gain too"). Every
 *  message here changes what EVERYBODY on a shared dial hears — the front end of whichever radio it is (RTL gain and
 *  AGC, the RSP's gain reduction / LNA / AGC, the HF+ attenuator / preamp / AGC, the Airspy and HackRF stages, the
 *  antenna, the tuner's IF filter, the sample rate, direct sampling) and the ONE shared pipeline's DSP (NR, notch,
 *  de-emphasis, stereo, the blankers, the broadcast-FM row, NFM voice). The server refuses the same list
 *  (local_sdr_shim.cpp askRefuses) — this copy only keeps a client from sending what it would refuse.
 *  ★ Dropped SILENTLY by the clients: a page restoring its preferences on connect is not somebody reaching for a
 *    control, so only the UI's own touch shields nudge. Tuning (tune/mode/bandwidth/dab) has its own gate.
 *  ★ NOT here, because they are this listener's own: squelch (per session), the waterfall rate, zoom/pan, RDS
 *    subscriptions, recording, uncompressed audio, raw IQ out, and everything behind the admin password. */
export const ASK_SHARED_SETTINGS = new Set<string>([
  'gain', 'agc', 'rsp_control', 'rsp_agc_restart', 'ahf_control', 'airspy_control', 'hackrf_control', 'antenna',
  'tunerbw', 'sampleRate', 'directSampling',
  'nr', 'notch', 'deemph', 'stereo', 'nfmvoice', 'wsp', 'nb', 'nbx', 'ceq', 'ims', 'autobw',
]);


/** ★★ The shared-tuner banner's standing words while locked — what to do NOW, not a rule to remember.
 *  ★ `tight` drops the "SHARED TUNER · " prefix first (SE in Display Zoom), never the words that matter.
 *  ★ A NO says WHEN it ends as a clock time, so the banner does not have to tick every second. */
export function askBannerText(d: AskState, tight: boolean, clock?: (ms: number) => string): string | null {
  if (!askLocked(d)) return null;
  const pre = tight ? '' : `${chatT('bannerShared')} · `;
  // ★ No longer than today's "SHARED TUNER · ASK TO TUNE · 2/5 👤", which is what the narrowest layout was fitted to —
  //   in every language (scripts/test_chat_i18n.ts measures each).
  if (d.askState === 'open') return `${pre}${chatT('bannerAwaiting')}`;
  if (d.askState === 'no' && d.askUntil) {
    const t = clock ? clock(d.askUntil)
      : (() => { const x = new Date(d.askUntil); return `${String(x.getHours()).padStart(2, '0')}:${String(x.getMinutes()).padStart(2, '0')}`; })();
    return `${pre}${chatT('bannerYoursAt', { time: t })}`;
  }
  return `${pre}${chatT('bannerAskInChat')}`;
}

/** ★★ The SHARED TUNER banner's standing words when the ask lock is not in force — one copy for the app's bar and the
 *  browser's: FREE TO TUNE alone, ASK TO TUNE · 3/5 👤 with company, ASK BEFORE TUNING before the count is known
 *  (listeners < 0). `tight` drops the prefix (SE in Display Zoom), never the words that matter. */
export function sharedTunerText(alone: boolean, listeners: number, max: number, tight = false): string {
  const pre = tight ? '' : `${chatT('bannerShared')} · `;
  if (listeners < 0) return `${pre}${chatT('bannerAskBefore')}`;
  if (alone) return `${pre}${chatT('bannerFree')}`;
  return `${pre}${chatT('bannerAsk')} · ${listeners}${max > 1 ? `/${max}` : ''} 👤`;
}

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
  return dialSummaryParts(d, false).join(' · ');
}
/** The strip's pieces. `hint` (the browser) points at the ask key below: "ask before tuning — Can I tune? below". */
export function dialSummaryParts(d: DialState, hint: boolean): string[] {
  const bits: string[] = [chatT('listening', { n: d.listeners })];
  const user = (n: number) => chatT('user', { n });
  if (d.mode === 'spectator')      bits.push(chatT('ownerTunes'));
  else if (d.decoding)             bits.push(d.mine ? chatT('youDecoding') : chatT('userDecoding', { who: user(d.tuner) }));
  // ★ Ask before tuning: say what to do, never "go ahead" to somebody the server will refuse.
  else if (askLocked(d))           bits.push(d.askState === 'open' ? chatT('askedWaiting')
                                           : d.askState === 'no' ? chatT('askedToWait')
                                           : hint ? chatT('askBeforeBelow', { ask: phraseText('ask_tune') ?? '' }) : chatT('askBefore'));
  else if (d.tuner && d.mine)      bits.push(chatT('youTunedLast'));
  else if (d.tuner)                bits.push(chatT('userTuning', { who: user(d.tuner) }));
  else                             bits.push(chatT('nobodyTuning'));
  return bits;
}

/** How a speaker is named. ★ "You" rather than your own ordinal: everybody else sees a number, and
 *  you know which is yours, but reading your own words back as a stranger's is oddly cold. */
export function speakerName(from: number, you: number): string {
  return from === you ? chatT('you') : chatT('user', { n: from });
}

/** "— User 1 joined 17:40 UTC —": a number passed to somebody new (`since` = when the server gave it, in seconds). */
export function joinedLine(from: number, you: number, since: number): string {
  const time = new Date(since * 1000).toISOString().slice(11, 16);
  return from === you ? chatT('joinedYou', { time }) : chatT('joined', { who: chatT('user', { n: from }), time });
}
