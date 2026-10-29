// cwVocab.ts — THE ham-CW vocabulary, ONE source (V12, 2026-10-29) for:
//   • the PLAIN ENGLISH key in the CW skimmer box (app + web): `plainEnglish(text)`;
//   • the TERMINOLOGY GUIDE under "NOT SURE WHAT A SIGNAL IS?" (app + web): `CW_TERMS`, `CW_LOOKALIKES`,
//     `searchCwTerms(q)`;
//   • the server decoder's small language model (scripts/gen-cw-lm.mjs reads this file and writes
//     android/app/src/main/cpp/decoders/cw_lm_data.h — the codes, the words and the QSO templates it is built from).
//
// ★★ Stuart, 2026-10-29: "callsigns, names and numbers untouched, unknown/garbled text left as sent". So the plain-English
//    reading ONLY replaces codes it recognises; everything else is passed through exactly as decoded. Other-language text
//    (an Italian QSO on 3554 kHz) keeps its words: a code that is ALSO an ordinary word in another language (ES, DE, SI…)
//    is `ambiguous` and is left alone on a line that reads as another language.
// ★ Sources: the Q-code series is ITU-R M.1172 (the "QRA…QUZ" table) and the prosigns ITU-R M.1677; the abbreviations are
//   the common amateur usage that every licence syllabus teaches. Written here in our own words.
// ★ Erasable TypeScript only (no enums, no imports): the generator runs this file directly under Node.

export type CwKind = 'q' | 'abbr' | 'prosign' | 'term' | 'number';

export interface CwTerm {
  /** As sent (upper case). A prosign is written as the decoder prints it: `<SK>`, `+` (AR), `=` (BT), `(` (KN). */
  code: string;
  kind: CwKind;
  /** The plain-English reading the toggle puts in its place ('' = guide only, never replaced). */
  plain: string;
  /** …when the code is sent as a question (`QRZ?`, `HW?`). */
  ask?: string;
  /** The guide's explanation, in plain words. */
  meaning: string;
  /** A short example of it in a real exchange. */
  example?: string;
  /** Other spellings that mean the same (TKS for TNX). */
  aliases?: string[];
  /** Also an ordinary word in another language (Spanish "es", Italian "si", French "de") — left as sent on a line that
   *  reads as another language. */
  ambiguous?: boolean;
}

/** The terms, in guide order. */
export const CW_TERMS: CwTerm[] = [
  // ── the essentials ──
  { code: 'CQ', kind: 'term', plain: 'calling anyone', meaning: 'A general call: "is anyone there who would like a contact?"',
    example: 'CQ CQ DE G4ABC G4ABC K — G4ABC is calling anyone, over.' },
  { code: 'DE', kind: 'term', plain: 'this is', meaning: '"From" / "this is" — it comes before the sender\'s own callsign. (French "de", not a German callsign: see the look-alikes.)',
    example: 'M0PKZ DE G4TPJ — to M0PKZ, this is G4TPJ.', ambiguous: true },
  { code: 'K', kind: 'prosign', plain: 'over', meaning: '"Over to you" — anyone may reply.', example: 'CQ DE G4ABC K' },
  { code: 'KN', kind: 'prosign', plain: 'over (only the station I am working)', meaning: '"Over, to the station I am in contact with only" — please do not break in. Sent as one character, so the decoder may also print it as "(".',
    example: 'HW? M0PKZ DE G4TPJ KN' },
  { code: '(', kind: 'prosign', plain: 'over, only the station I am working', meaning: 'KN sent as one character (the same Morse as an opening bracket).' },
  { code: 'BK', kind: 'prosign', plain: 'back to you', meaning: '"Break" — a quick hand-back without callsigns.', example: 'UR 599 BK' },
  { code: 'AR', kind: 'prosign', plain: 'end of message', meaning: 'End of the message. Sent as one character, so the decoder may print it as "+".', example: 'TNX FER QSO + M0PKZ DE G4TPJ' },
  { code: '+', kind: 'prosign', plain: 'end of message', meaning: 'AR sent as one character (the same Morse as "+").' },
  { code: '<SK>', kind: 'prosign', plain: 'end of contact', meaning: 'The end of the contact ("silent key" is something else: an operator who has died).', example: '73 <SK>', aliases: ['SK'] },
  { code: 'BT', kind: 'prosign', plain: '—', meaning: 'A break between thoughts, like a new paragraph. Printed as "=".', example: 'NAME JOHN = QTH LONDON' },
  { code: '=', kind: 'prosign', plain: '—', meaning: 'BT, the break between thoughts (the same Morse as "=").' },
  { code: '<KA>', kind: 'prosign', plain: '(start of message)', meaning: 'The start of a message (CT).', aliases: ['<CT>'] },
  { code: '<AS>', kind: 'prosign', plain: '(wait)', meaning: '"Wait" — stand by a moment.' },
  { code: '<SN>', kind: 'prosign', plain: '(understood)', meaning: '"Understood" (also written VE).' },
  { code: '<HH>', kind: 'prosign', plain: '(error)', meaning: 'Eight dots: "I made a mistake", the word is sent again.' },
  { code: 'R', kind: 'term', plain: 'received', meaning: '"Received" — everything was copied.', example: 'R R TNX FER RPRT' },
  { code: 'RR', kind: 'term', plain: 'received', meaning: '"Received", sent twice.' },
  { code: 'TU', kind: 'abbr', plain: 'thank you', meaning: 'Thank you (contests end each contact with it).', example: 'TU 5NN 14' },
  { code: '73', kind: 'number', plain: 'best regards', meaning: 'Best regards — the usual sign-off.', example: 'TNX QSO 73 GL' },
  { code: '88', kind: 'number', plain: 'love and kisses', meaning: 'Love and kisses (a warm sign-off, usually to a friend or a lady operator).' },
  { code: '72', kind: 'number', plain: 'best regards (low power)', meaning: 'Best regards, among low-power (QRP) operators.' },
  // ── the signal report ──
  { code: 'RST', kind: 'term', plain: 'signal report', meaning: 'The signal report, three figures: R = readability 1–5 (5 = perfectly readable), S = strength 1–9 (9 = very strong), T = tone 1–9 (9 = a pure, clean note). 599 is the best there is.',
    example: 'UR RST 579 — you are perfectly readable, fairly strong, with a clean note.' },
  { code: '5NN', kind: 'number', plain: '599', meaning: '599 with the 9s cut to N (contest shorthand). In contests T can stand for 0 and A for 1 too.', example: 'TU 5NN 14' },
  { code: 'RPRT', kind: 'abbr', plain: 'report', meaning: 'Report.', aliases: ['RPT'] },
  { code: 'SIG', kind: 'abbr', plain: 'signal', meaning: 'Signal.', aliases: ['SIGS'] },
  // ── Q-codes (ITU-R M.1172); with "?" each is a question ──
  { code: 'QTH', kind: 'q', plain: 'my location is', ask: 'where are you?', meaning: 'My location is… (QTH? = where are you?)', example: 'QTH NR TONBRIDGE' },
  { code: 'QRZ', kind: 'q', plain: 'you are being called by', ask: 'who is calling me?', meaning: 'Who is calling me? Also the name of QRZ.COM, the callsign lookup site.', example: 'QRZ? DE G4ABC' },
  { code: 'QSL', kind: 'q', plain: 'I confirm', ask: 'can you confirm?', meaning: 'I confirm receipt. A "QSL card" confirms a contact on paper or online.', example: 'PSE QSL VIA BURO' },
  { code: 'QSO', kind: 'q', plain: 'contact', meaning: 'A contact (a conversation) between two stations.', example: 'TNX FER QSO' },
  { code: 'QRM', kind: 'q', plain: 'interference', ask: 'is there interference?', meaning: 'Interference from other stations.', example: 'SRI QRM' },
  { code: 'QRN', kind: 'q', plain: 'static', ask: 'is there static?', meaning: 'Natural noise — static, thunderstorms.', example: 'QRN HVY HR' },
  { code: 'QSB', kind: 'q', plain: 'fading', ask: 'are my signals fading?', meaning: 'Your signal is fading in and out.', example: 'UR 559 QSB' },
  { code: 'QRP', kind: 'q', plain: 'low power', ask: 'shall I reduce power?', meaning: 'Low power (5 W or less), or "reduce power".', example: 'RIG QRP 5W' },
  { code: 'QRO', kind: 'q', plain: 'high power', ask: 'shall I increase power?', meaning: 'High power, or "increase power".' },
  { code: 'QSY', kind: 'q', plain: 'change frequency', ask: 'shall I change frequency?', meaning: 'Change frequency (QSY 3560 = move to 3560 kHz).', example: 'QSY 7030' },
  { code: 'QRL', kind: 'q', plain: 'this frequency is in use', ask: 'is this frequency in use?', meaning: 'This frequency is busy. Sent with "?" before calling CQ: "is anyone using this frequency?"', example: 'QRL?' },
  { code: 'QRS', kind: 'q', plain: 'send slower', ask: 'shall I send slower?', meaning: 'Please send more slowly.', example: 'PSE QRS' },
  { code: 'QRQ', kind: 'q', plain: 'send faster', ask: 'shall I send faster?', meaning: 'Please send faster.' },
  { code: 'QRT', kind: 'q', plain: 'closing down', ask: 'shall I stop sending?', meaning: 'I am stopping / closing the station.', example: 'QRT NW 73' },
  { code: 'QRV', kind: 'q', plain: 'ready', ask: 'are you ready?', meaning: 'I am ready.' },
  { code: 'QRX', kind: 'q', plain: 'please wait', ask: 'when will you call again?', meaning: 'Wait (QRX 5 = wait five minutes).' },
  { code: 'QRG', kind: 'q', plain: 'frequency', ask: 'what is my exact frequency?', meaning: 'Your exact frequency (QRG BUSY = this frequency is busy).' },
  { code: 'QRK', kind: 'q', plain: 'readability', ask: 'how readable am I?', meaning: 'The readability of your signal (the R of RST).' },
  { code: 'QSK', kind: 'q', plain: 'full break-in', ask: 'can you hear me between your signals?', meaning: 'I can hear between my own dots and dashes — break in any time.' },
  { code: 'QRU', kind: 'q', plain: 'nothing more for you', ask: 'have you anything for me?', meaning: 'I have nothing more for you.' },
  { code: 'QSP', kind: 'q', plain: 'relay', ask: 'will you relay?', meaning: 'I will relay (pass a message on).' },
  { code: 'QRA', kind: 'q', plain: 'my station is', ask: 'what is your station called?', meaning: 'The name of my station; in Europe also a grid locator ("QRA locator").' },
  { code: 'QTR', kind: 'q', plain: 'the time is', ask: 'what is the time?', meaning: 'The correct time is…' },
  { code: 'QRT?', kind: 'q', plain: 'shall I stop sending?', meaning: '' },
  // ── common abbreviations ──
  { code: 'TNX', kind: 'abbr', plain: 'thanks', meaning: 'Thanks.', aliases: ['TKS', 'THX'], example: 'TNX FER CALL' },
  { code: 'FER', kind: 'abbr', plain: 'for', meaning: 'For.', example: 'TNX FER CALL' },
  { code: 'UR', kind: 'abbr', plain: 'your', meaning: 'Your / you are.', example: 'UR RST 589' },
  { code: 'U', kind: 'abbr', plain: 'you', meaning: 'You.' },
  { code: 'ES', kind: 'abbr', plain: 'and', meaning: '"And" (from the old landline "&"). Also a Spanish word, so it is left alone in a Spanish exchange.', ambiguous: true, example: 'NAME JOHN ES QTH LONDON' },
  { code: 'OP', kind: 'abbr', plain: 'my name is', meaning: 'Operator — the operator\'s name follows.', example: 'OP IS RICHARD' },
  { code: 'NAME', kind: 'abbr', plain: 'my name is', meaning: 'My name is…', example: 'NAME JOSE' },
  { code: 'NR', kind: 'abbr', plain: 'near', meaning: '"Near" (QTH NR LONDON), or "number" before a figure (NR 5).', example: 'QTH NR TONBRIDGE' },
  { code: 'HW', kind: 'abbr', plain: 'how', ask: 'how do you copy?', meaning: 'How. HW? = how do you copy me?', example: 'HW? M0PKZ DE G4TPJ KN' },
  { code: 'CPY', kind: 'abbr', plain: 'copy', meaning: 'Copy (receive).', aliases: ['CPI'] },
  { code: 'PSE', kind: 'abbr', plain: 'please', meaning: 'Please.', aliases: ['PLS'] },
  { code: 'AGN', kind: 'abbr', plain: 'again', meaning: 'Again.', example: 'PSE AGN' },
  { code: 'SRI', kind: 'abbr', plain: 'sorry', meaning: 'Sorry.' },
  { code: 'FB', kind: 'abbr', plain: 'fine (excellent)', meaning: '"Fine business" — excellent, very good.', example: 'FB OM' },
  { code: 'OM', kind: 'abbr', plain: 'friend', meaning: '"Old man" — any male operator, a friendly "mate".', example: 'TNX OM' },
  { code: 'YL', kind: 'abbr', plain: 'lady operator', meaning: '"Young lady" — any female operator.' },
  { code: 'XYL', kind: 'abbr', plain: 'wife', meaning: 'Wife.' },
  { code: 'DR', kind: 'abbr', plain: 'dear', meaning: 'Dear.', example: 'TNX DR MICHAEL' },
  { code: 'GM', kind: 'abbr', plain: 'good morning', meaning: 'Good morning.' },
  { code: 'GA', kind: 'abbr', plain: 'good afternoon', meaning: 'Good afternoon (or "go ahead").' },
  { code: 'GE', kind: 'abbr', plain: 'good evening', meaning: 'Good evening.' },
  { code: 'GN', kind: 'abbr', plain: 'good night', meaning: 'Good night.' },
  { code: 'GL', kind: 'abbr', plain: 'good luck', meaning: 'Good luck.' },
  { code: 'GD', kind: 'abbr', plain: 'good', meaning: 'Good (GD DX = good long-distance contacts).' },
  { code: 'VY', kind: 'abbr', plain: 'very', meaning: 'Very.' },
  { code: 'WX', kind: 'abbr', plain: 'weather', meaning: 'Weather.', example: 'WX FINE TEMP 12C' },
  { code: 'TEMP', kind: 'abbr', plain: 'temperature', meaning: 'Temperature.' },
  { code: 'RIG', kind: 'abbr', plain: 'radio', meaning: 'The radio (transceiver) in use.', example: 'RIG IC7300' },
  { code: 'ANT', kind: 'abbr', plain: 'antenna', meaning: 'Antenna (aerial).', example: 'ANT DIPOLE' },
  { code: 'PWR', kind: 'abbr', plain: 'power', meaning: 'Transmitter power.', example: 'PWR 100W' },
  { code: 'HR', kind: 'abbr', plain: 'here', meaning: 'Here.' },
  { code: 'HPE', kind: 'abbr', plain: 'hope', meaning: 'Hope.' },
  { code: 'CUL', kind: 'abbr', plain: 'see you later', meaning: 'See you later.' },
  { code: 'CUAGN', kind: 'abbr', plain: 'see you again', meaning: 'See you again.' },
  { code: 'BCNU', kind: 'abbr', plain: 'be seeing you', meaning: 'Be seeing you.' },
  { code: 'ABT', kind: 'abbr', plain: 'about', meaning: 'About.' },
  { code: 'WID', kind: 'abbr', plain: 'with', meaning: 'With.' },
  { code: 'NW', kind: 'abbr', plain: 'now', meaning: 'Now.' },
  { code: 'WKD', kind: 'abbr', plain: 'worked', meaning: 'Worked (made a contact with).' },
  { code: 'WL', kind: 'abbr', plain: 'will', meaning: 'Will.' },
  { code: 'WUD', kind: 'abbr', plain: 'would', meaning: 'Would.' },
  { code: 'MNI', kind: 'abbr', plain: 'many', meaning: 'Many.' },
  { code: 'B4', kind: 'abbr', plain: 'before', meaning: 'Before.' },
  { code: 'BTU', kind: 'abbr', plain: 'back to you', meaning: 'Back to you.' },
  { code: 'SKED', kind: 'abbr', plain: 'schedule', meaning: 'A scheduled contact.' },
  { code: 'STN', kind: 'abbr', plain: 'station', meaning: 'Station.' },
  { code: 'XCVR', kind: 'abbr', plain: 'transceiver', meaning: 'Transceiver.' },
  { code: 'TRX', kind: 'abbr', plain: 'transceiver', meaning: 'Transceiver.' },
  { code: 'CONDX', kind: 'abbr', plain: 'conditions', meaning: 'Radio conditions (propagation).', example: 'IN VIEW OF CONDX HERE' },
  { code: 'DX', kind: 'term', plain: 'distant station', meaning: 'A distant station, or long-distance contacts. CQ DX = "calling distant stations only".' },
  { code: 'TEST', kind: 'term', plain: 'contest', meaning: 'A contest call (CQ TEST).' },
  { code: 'POTA', kind: 'term', plain: 'Parks on the Air', meaning: 'Parks on the Air — an operator in a park, portable.' },
  { code: 'SOTA', kind: 'term', plain: 'Summits on the Air', meaning: 'Summits on the Air — an operator on a hilltop, portable.' },
  { code: 'QRPP', kind: 'q', plain: 'very low power', meaning: 'Very low power (under a watt).' },
  { code: 'OB', kind: 'abbr', plain: 'old boy', meaning: '"Old boy" — a friendly term for another operator.' },
  { code: 'OT', kind: 'abbr', plain: 'old timer', meaning: 'An operator licensed for many years.' },
  { code: 'NIL', kind: 'abbr', plain: 'nothing', meaning: 'Nothing (NIL = I have nothing for you / did not copy).' },
  { code: 'INFO', kind: 'abbr', plain: 'information', meaning: 'Information.' },
  { code: 'SN', kind: 'abbr', plain: 'soon', meaning: 'Soon.' },
  { code: 'TMW', kind: 'abbr', plain: 'tomorrow', meaning: 'Tomorrow.' },
  { code: 'ENUF', kind: 'abbr', plain: 'enough', meaning: 'Enough.' },
  { code: 'LID', kind: 'abbr', plain: 'poor operator', meaning: 'A poor or careless operator (not polite!).' },
  { code: 'HVY', kind: 'abbr', plain: 'heavy', meaning: 'Heavy.' },
  { code: 'WRK', kind: 'abbr', plain: 'work', meaning: 'Work.' },
  { code: 'YRS', kind: 'abbr', plain: 'years', meaning: 'Years.' },
  { code: 'AGE', kind: 'abbr', plain: 'age', meaning: 'Age (the operator\'s).' },
  { code: 'BURO', kind: 'abbr', plain: 'the QSL bureau', meaning: 'The QSL bureau, which posts QSL cards for its members.' },
  { code: 'LOTW', kind: 'abbr', plain: 'Logbook of the World', meaning: 'Logbook of the World — online QSL confirmation.' },
];

/** Look-alikes the guide warns about. */
export const CW_LOOKALIKES: { title: string; text: string }[] = [
  { title: 'DE is not a German callsign',
    text: 'DE means "from / this is" (as in French "de"): "M0PKZ DE G4TPJ" = "to M0PKZ, this is G4TPJ". German callsigns START with D — DL, DK, DJ, DB, DO and so on — and always have a figure in them (DL1XYZ).' },
  { title: '0 (zero) and O (the letter)',
    text: 'Callsigns put a FIGURE after the prefix: M0PKD has a zero, not the letter O. In Morse a zero is five dashes (-----) and O is three (---), so they never sound alike — the confusion is only on the page.' },
  { title: 'Prosigns that look like punctuation',
    text: 'Some prosigns are sent as one character with the same Morse as a punctuation mark, so a decoder prints them as that mark: AR as "+", BT as "=", KN as "(".' },
  { title: 'SK — two meanings',
    text: 'On the air <SK> ends a contact. "A silent key" (SK in writing) is an operator who has died.' },
  { title: 'Cut numbers',
    text: 'Contests shorten figures: N = 9, T = 0, A = 1, E = 5 — so "5NN" is 599 and "TT1" can be 001.' },
  { title: 'Codes that are words in other languages',
    text: 'ES is "and" in ham CW but a Spanish word; SI, DE, NR and others are ordinary words in Italian, French or German. The plain-English key leaves them alone in a conversation in another language.' },
];

/** ★ The terms guide's opening lines (app CwTermsGuide + web cwGuide.ts — one wording). */
export const CW_GUIDE_INTRO = 'The codes and short words used in Morse (CW) contacts. The PLAIN key in the CW skimmer box reads them for you.';
export const CW_GUIDE_RST_LINE = 'RST is the signal report: R = readability (1–5), S = strength (1–9), T = tone (1–9). 599 is perfect.';
export const CW_KIND_LABEL: Record<CwKind, string> = { q: 'Q-CODE', abbr: 'ABBREVIATION', prosign: 'PROSIGN', term: 'TERM', number: 'NUMBER' };

/** The look-alikes the guide's search matches (every one with no search). */
export function matchCwLookalikes(q: string): { title: string; text: string }[] {
  const s = q.trim().toUpperCase();
  if (!s) return CW_LOOKALIKES;
  return CW_LOOKALIKES.filter((l) => l.title.toUpperCase().includes(s) || l.text.toUpperCase().includes(s));
}

const BY_CODE: Map<string, CwTerm> = (() => {
  const m = new Map<string, CwTerm>();
  for (const t of CW_TERMS) {
    if (!m.has(t.code)) m.set(t.code, t);
    for (const a of t.aliases ?? []) if (!m.has(a)) m.set(a, t);
  }
  return m;
})();
export function cwTerm(code: string): CwTerm | undefined { return BY_CODE.get(code.toUpperCase()); }

/** ★ The guide's search: code, alias, plain reading or meaning, case-insensitive; codes that START with the query first. */
export function searchCwTerms(q: string): CwTerm[] {
  const terms = CW_TERMS.filter((t) => t.meaning);
  const s = q.trim().toUpperCase();
  if (!s) return terms;
  const starts: CwTerm[] = [], rest: CwTerm[] = [];
  for (const t of terms) {
    const codes = [t.code, ...(t.aliases ?? [])];
    if (codes.some((c) => c.startsWith(s))) starts.push(t);
    else if (codes.some((c) => c.includes(s)) || t.plain.toUpperCase().includes(s) || t.meaning.toUpperCase().includes(s)
      || (t.example ?? '').toUpperCase().includes(s)) rest.push(t);
  }
  return [...starts, ...rest];
}

// ── The plain-English reading ─────────────────────────────────────────────────────────────────────────────────────

/** A callsign as the decoder's spot rule sees it: a prefix, a figure, a suffix (G4ABC, M0PKZ, 2E0ABC, EA8/G4ABC/P). */
export function looksLikeCallsign(w: string): boolean {
  const parts = w.toUpperCase().split('/');
  if (parts.length > 3) return false;
  let best = parts[0];
  for (const p of parts) if (p.length > best.length) best = p;
  return /^(?:[A-Z]{1,2}|[A-Z][0-9]|[0-9][A-Z]{1,2})[0-9]{1,2}[A-Z]{1,4}$/.test(best) && /[A-Z]/.test(best) && !BY_CODE.has(w);
}

/** ★ Function words that mark a line as ANOTHER language (Italian, Spanish, French, German, Portuguese, Dutch). Only
 *  ones that are not also English or ham words. Two on a line = not English. */
const FOREIGN_WORDS = new Set([
  // Italian
  'IL', 'LA', 'LE', 'DI', 'DEL', 'DELLA', 'CHE', 'NON', 'PER', 'UNA', 'UNO', 'SONO', 'MOLTO', 'GRAZIE', 'CIAO', 'QUI', 'TUTTO', 'TUTTA',
  'ANCHE', 'COME', 'STATO', 'BUONA', 'SERA', 'BUONGIORNO', 'CON', 'NELLA', 'FILARE', 'SI', 'GLI', 'HO',
  // Spanish
  'EL', 'LOS', 'LAS', 'MUY', 'BUENAS', 'NOCHES', 'GRACIAS', 'AQUI', 'PARA', 'ESTA', 'ESTOY', 'COMO', 'SALUDOS', 'QUE',
  // French
  'LE', 'LES', 'DU', 'DES', 'ET', 'EST', 'MERCI', 'BONSOIR', 'BONJOUR', 'TRES', 'AVEC', 'ICI', 'JE', 'SUIS', 'VOUS', 'NOUS',
  // German
  'DER', 'DIE', 'DAS', 'UND', 'ICH', 'BIN', 'NICHT', 'MIT', 'HIER', 'DANKE', 'GUTEN', 'ABEND', 'SEHR', 'GUT', 'IST', 'EIN', 'EINE',
  // Portuguese / Dutch
  'OBRIGADO', 'MUITO', 'BOA', 'NOITE', 'EEN', 'HET', 'EN', 'IK', 'BEDANKT', 'GOEDE', 'AVOND',
]);
/** Words of English that also appear above — never count as foreign. */
const ENGLISH_TOO = new Set(['I', 'A', 'IN', 'IS', 'ON', 'TO', 'AS', 'HE', 'ME', 'NO']);

export function readsAsOtherLanguage(words: string[]): boolean {
  let n = 0;
  for (const w of words) if (FOREIGN_WORDS.has(w) && !ENGLISH_TOO.has(w)) n++;
  return n >= 2;
}

/** ★ Multi-word phrases first (they read better than word-by-word). Matched on whole words; `?` may follow the last. */
const PHRASES: { words: string[]; plain: string }[] = [
  { words: ['TNX', 'FER', 'CALL'], plain: 'thanks for the call' },
  { words: ['TNX', 'FER', 'QSO'], plain: 'thanks for the contact' },
  { words: ['TNX', 'FER', 'RPRT'], plain: 'thanks for the report' },
  { words: ['TNX', 'FER', 'RPT'], plain: 'thanks for the report' },
  { words: ['TU', 'FER', 'CALL'], plain: 'thank you for the call' },
  { words: ['UR', 'RST', 'IS'], plain: 'your signal report is' },
  { words: ['UR', 'RST'], plain: 'your signal report is' },
  { words: ['RST', 'IS'], plain: 'signal report is' },
  { words: ['UR', 'SIGS'], plain: 'your signals' },
  { words: ['UR', 'QTH'], plain: 'your location' },
  { words: ['MY', 'QTH', 'IS'], plain: 'my location is' },
  { words: ['QTH', 'IS'], plain: 'my location is' },
  { words: ['OP', 'IS'], plain: 'my name is' },
  { words: ['NAME', 'IS'], plain: 'my name is' },
  { words: ['MY', 'NAME', 'IS'], plain: 'my name is' },
  { words: ['HW', 'CPY'], plain: 'how do you copy' },
  { words: ['HW', 'CPI'], plain: 'how do you copy' },
  { words: ['PSE', 'QSL'], plain: 'please confirm' },
  { words: ['PSE', 'AGN'], plain: 'please say again' },
  { words: ['CQ', 'DX'], plain: 'calling distant stations' },
  { words: ['CQ', 'TEST'], plain: 'calling, contest' },
  { words: ['CQ', 'POTA'], plain: 'calling from a park' },
  { words: ['CQ', 'SOTA'], plain: 'calling from a summit' },
  { words: ['GD', 'DX'], plain: 'good long-distance contacts' },
];
const PROSIGN_CHARS = new Set(['=', '+', '(']);

/** Splits glued prosign characters off a word: "=OPIS" → "=", "OPIS"; "HW?+M0PKZ" → "HW?", "+", "M0PKZ"; "G4TPJ(" →
 *  "G4TPJ", "(" (KN). */
function tokens(line: string): string[] {
  const out: string[] = [];
  for (const raw of line.split(/\s+/)) {
    if (!raw) continue;
    let cur = '';
    for (const ch of raw) {
      if (ch === '=' || ch === '+' || ch === '(') { if (cur) out.push(cur); out.push(ch); cur = ''; }
      else cur += ch;
    }
    if (cur) out.push(cur);
  }
  return out;
}

/** ★★ THE PLAIN-ENGLISH READING of decoded CW (one line at a time). Recognised codes become their plain words; callsigns,
 *  names, numbers and anything unrecognised are passed through exactly as decoded (upper case, so what is raw stays
 *  visibly raw). On a line that reads as another language, codes that are also ordinary words there stay as sent. A code
 *  repeated (CQ CQ CQ) is said once. */
export function plainEnglish(text: string): string {
  return text.split('\n').map(plainLine).join('\n');
}

function plainLine(line: string): string {
  const w = tokens(line);
  if (!w.length) return line;
  const foreign = readsAsOtherLanguage(w);
  const out: string[] = [];
  let lastPlain = '';
  const push = (s: string, isPlain: boolean) => {
    if (isPlain && s === lastPlain) return;      // ★ CQ CQ CQ → "calling anyone" once
    out.push(s); lastPlain = isPlain ? s : '';
  };
  for (let i = 0; i < w.length; ) {
    // ★ Phrases (whole words; the last may carry a "?")
    let matched = false;
    if (!foreign) {
      for (const p of PHRASES) {
        if (i + p.words.length > w.length) continue;
        let ok = true; let q = false;
        for (let k = 0; k < p.words.length && ok; k++) {
          const t = w[i + k];
          const last = k === p.words.length - 1;
          if (t === p.words[k]) continue;
          if (last && t === p.words[k] + '?') { q = true; continue; }
          ok = false;
        }
        if (ok) { push(p.plain + (q ? '?' : ''), true); i += p.words.length; matched = true; break; }
      }
    }
    if (matched) continue;
    const tok = w[i];
    const p = plainWord(tok, w[i + 1], foreign);
    push(p, p !== tok);   // ★ only a code said once — a callsign sent twice (G4ABC G4ABC) stays as sent
    i++;
  }
  return out.join(' ');
}

function plainWord(tok: string, next: string | undefined, foreign: boolean): string {
  if (PROSIGN_CHARS.has(tok)) { const t = BY_CODE.get(tok); return t ? (t.plain === '—' ? '—' : `(${t.plain})`) : tok; }
  if (/^[0-9]+$/.test(tok) && tok !== '73' && tok !== '88' && tok !== '72') return tok;   // ★ numbers untouched (599, 3560)
  const ask = tok.endsWith('?') && tok.length > 1;
  const base = ask ? tok.slice(0, -1) : tok;
  const t = BY_CODE.get(base);
  if (!t || !t.plain) return tok;
  if (foreign && (t.ambiguous || t.kind === 'abbr' || t.kind === 'term')) return tok;   // ★ only Q-codes, prosigns, 73 in another language
  if (base === 'NR' && next && /^[0-9]/.test(next)) return 'number';
  if (t.kind === 'prosign' && base.startsWith('<')) return `(${t.plain})`;
  if (ask) return t.ask ?? `${t.plain}?`;
  return t.plain;
}

// ── For the decoder's language model (scripts/gen-cw-lm.mjs) ──────────────────────────────────────────────────────

/** ★ Words a ham CW exchange is made of that are not codes: they make the model expect "NAME JOHN" and "RIG IC7300". */
export const CW_QSO_WORDS = [
  'IS', 'MY', 'THE', 'AND', 'HERE', 'NAME', 'RIG', 'ANT', 'DIPOLE', 'VERTICAL', 'YAGI', 'BEAM', 'LOOP', 'WIRE', 'END', 'FED',
  'INV', 'VEE', 'WINDOM', 'GP', 'WATTS', 'W', 'KW', 'FT', 'M', 'UP', 'IN', 'AT', 'ON', 'TO', 'OF', 'FOR', 'BUT', 'NOT', 'VERY',
  'FINE', 'NICE', 'COLD', 'WARM', 'RAIN', 'SUNNY', 'CLOUDY', 'WINDY', 'SNOW', 'FOG', 'C', 'F', 'TEMP', 'DEG', 'TODAY', 'TONIGHT',
  'WEEKEND', 'BAND', 'BANDS', 'NOISE', 'NOISY', 'GOOD', 'SIGNAL', 'COPY', 'SOLID', 'WEAK', 'STRONG', 'LOUD', 'FIRST', 'NEW',
  'OLD', 'KEY', 'BUG', 'PADDLE', 'STRAIGHT', 'HOME', 'MADE', 'VALVE', 'TUBE', 'QRP', 'CLUB', 'LICENSED', 'SINCE', 'RETIRED',
  'WORK', 'WORKED', 'HOPE', 'MEET', 'AGAIN', 'SOON', 'SEE', 'YOU', 'THANKS', 'BEST', 'REGARDS', 'WISHES', 'DAY', 'NIGHT',
  'MORNING', 'EVENING', 'HELLO', 'WONT', 'CANT', 'DONT', 'HOLD', 'IT', 'VIEW', 'HAD', 'HAVE', 'HAS', 'A', 'FEW', 'QSOS', 'THIS',
  'THAT', 'WITH', 'FROM', 'BEEN', 'WAS', 'WILL', 'CAN', 'JUST', 'ALL', 'GQRP', 'FISTS', 'CWOPS', 'RSGB', 'ARRL', 'DARC', 'ARI',
  'URE', 'REF', 'UBA', 'VERON', 'ON', 'OFF', 'ONLY', 'PORTABLE', 'MOBILE', 'BATTERY', 'SOLAR', 'GARDEN', 'ATTIC', 'LOFT',
  'COMING', 'SO', 'GOT', 'SOME', 'BACK', 'TIME', 'NOW', 'MUST', 'GO', 'QRT', 'DINNER', 'TEA', 'XYL', 'CALLS',
];
/** ★ Operator names, the commonest across Europe and North America: "NAME JOSE", "OP IS RICHARD". */
export const CW_NAMES = [
  'JOHN', 'DAVE', 'DAVID', 'MIKE', 'MICHAEL', 'PAUL', 'PETER', 'PETE', 'BOB', 'ROBERT', 'JIM', 'JAMES', 'BILL', 'TOM', 'STEVE',
  'CHRIS', 'MARK', 'RICHARD', 'RICH', 'DICK', 'TONY', 'ALAN', 'IAN', 'DAN', 'DON', 'KEN', 'RON', 'GARY', 'ROGER', 'NICK', 'GEOFF',
  'GRAHAM', 'KEITH', 'MARTIN', 'NEIL', 'COLIN', 'BRIAN', 'ANDY', 'PHIL', 'FRANK', 'JOE', 'JACK', 'ED', 'FRED', 'GEORGE', 'HARRY',
  'STUART', 'SIMON', 'TIM', 'ROB', 'MAX', 'SAM', 'BEN', 'ERIC', 'HANS', 'KLAUS', 'JUERGEN', 'WOLFGANG', 'GUENTER', 'UWE', 'WERNER',
  'PETRA', 'JOSE', 'JUAN', 'PEDRO', 'LUIS', 'CARLOS', 'ANTONIO', 'MANUEL', 'JAVIER', 'PACO', 'MARIO', 'LUIGI', 'GIUSEPPE', 'FRANCO',
  'MARCO', 'PAOLO', 'ROBERTO', 'GIANNI', 'ENZO', 'PIERRE', 'JEAN', 'JACQUES', 'MICHEL', 'ALAIN', 'PHILIPPE', 'FRANCOIS', 'BERNARD',
  'JAN', 'PIET', 'KEES', 'WIM', 'LARS', 'OLE', 'BJORN', 'SVEN', 'JUKKA', 'PEKKA', 'JANUSZ', 'PIOTR', 'MAREK', 'TOMASZ', 'JIRI',
  'PAVEL', 'IGOR', 'SERGEY', 'VLAD', 'ALEX', 'YURI', 'OLEG', 'TAKA', 'HIRO', 'MARY', 'SUE', 'ANN', 'JANE', 'LIZ', 'KATE', 'JO',
  'FABIAN', 'FABIO', 'ANDREA', 'STEFANO', 'MASSIMO', 'DANIEL', 'THOMAS', 'MATT', 'KARL', 'OTTO', 'FRITZ', 'DIETER', 'HORST',
];
/** ★ Common words of other languages heard on the bands — so the model does not treat a Spanish or Italian word as an error
 *  to be "corrected" into English. (The model only breaks ties between timing readings; it never changes a letter.) */
export const CW_OTHER_LANGUAGE_WORDS = [
  'TUTTA', 'TUTTO', 'ESTATE', 'STATO', 'SONO', 'ANTENNA', 'FILARE', 'CHE', 'NON', 'PER', 'CON', 'MOLTO', 'BENE', 'GRAZIE', 'CIAO',
  'BUONA', 'SERA', 'QUI', 'ANCHE', 'COME', 'NOME', 'LA', 'IL', 'DI', 'DEL', 'SI', 'HO', 'HA', 'UN', 'UNA', 'ACQUI', 'ACQUA',
  'MUY', 'BUENAS', 'NOCHES', 'GRACIAS', 'AQUI', 'PARA', 'ESTA', 'COMO', 'SALUDOS', 'NOMBRE', 'EL', 'LOS', 'Y', 'MI', 'QUE',
  'MERCI', 'BONSOIR', 'BONJOUR', 'TRES', 'AVEC', 'ICI', 'JE', 'SUIS', 'ET', 'EST', 'LE', 'DES', 'DU', 'PRENOM',
  'DANKE', 'GUTEN', 'ABEND', 'SEHR', 'GUT', 'UND', 'ICH', 'BIN', 'HIER', 'DER', 'DIE', 'DAS', 'MIT', 'NICHT', 'VIELEN', 'DANK',
  'OBRIGADO', 'MUITO', 'BOA', 'NOITE', 'BEDANKT', 'GOEDE', 'AVOND', 'TACK', 'KIITOS', 'DZIEKUJE',
];
/** ★ Towns — "QTH NR TONBRIDGE", "NR BRUSSELS": a handful only (the model learns the SHAPE "QTH word", not a gazetteer). */
export const CW_PLACES = [
  'LONDON', 'BERLIN', 'PARIS', 'MADRID', 'ROME', 'ROMA', 'MILANO', 'BRUSSELS', 'AMSTERDAM', 'DUBLIN', 'EDINBURGH', 'CARDIFF',
  'MANCHESTER', 'BRISTOL', 'LEEDS', 'YORK', 'KENT', 'ESSEX', 'TONBRIDGE', 'OXFORD', 'CAMBRIDGE', 'WALES', 'SCOTLAND', 'ENGLAND',
  'IRELAND', 'HAMBURG', 'MUNICH', 'MUENCHEN', 'KOELN', 'WIEN', 'PRAHA', 'WARSZAWA', 'OSLO', 'STOCKHOLM', 'HELSINKI', 'LISBOA',
  'BARCELONA', 'VALENCIA', 'LYON', 'TORINO', 'NAPOLI', 'BOLOGNA', 'GENOVA', 'FIRENZE', 'ZURICH', 'GENEVA', 'BERN',
];
