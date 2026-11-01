// test_navtex.ts — NAVTEX message blocks from the decoder's character stream (src/utils/navtex.ts, 2026-10-05).
// Real-shaped traffic: CR CR LF line ends, '_' for a lost character (fsk_decoder.cpp), chunks split anywhere.
import fs from 'node:fs';
import { NavtexAssembler, NAVTEX_END_LOST_MS, navtexTitle, navtexLostPct, navtexBody, navtexFileName, navtexFileText }
  from '../src/utils/navtex.ts';
import { navtexStationLabel, navtexPositions, navtexResolve, navtexLabelOf, navtexSelfId, navtexStartLostLabel } from '../src/utils/navtexStations.generated.ts';
let pass = 0, fail = 0;
const ok = (c: boolean, m: string) => { if (c) pass++; else { fail++; console.log('  FAIL ' + m); } };

const T0 = Date.UTC(2026, 9, 5, 16, 1, 30);
const GALE = 'ZCZC GB98\r\r\nWZ 1234\r\r\nGALE WARNING\r\r\nVIKING NORTH UTSIRE SOUTHWEST 7 TO SEVERE GALE 9\r\r\nNNNN\r\r\n';

/** Feed `s` in chunks of `n` characters, one second apart. */
function feed(a: NavtexAssembler, s: string, n: number, t0 = T0): number {
  let t = t0;
  for (let i = 0; i < s.length; i += n) { a.push(s.slice(i, i + n), t); t += 1000; }
  return t;
}

// 1. Standing by: nothing yet.
{
  const a = new NavtexAssembler();
  ok(a.live === null && a.prev === null && !a.receiving, 'empty assembler is standing by');
}

// 2. A clean message, every chunk size (a header or trailer split across chunks must still be found).
for (const n of [1, 2, 3, 5, 7, 13, 1000]) {
  const a = new NavtexAssembler();
  feed(a, GALE, n);
  const m = a.live;
  ok(!!m && m.done && !m.endLost && !m.startLost, `clean message complete (chunk ${n})`);
  ok(m?.id === 'GB98' && m.station === 'G' && m.subject === 'B' && m.serial === '98', `header parsed (chunk ${n})`);
  ok(m?.text === 'WZ 1234\nGALE WARNING\nVIKING NORTH UTSIRE SOUTHWEST 7 TO SEVERE GALE 9', `body, CR CR LF folded (chunk ${n}): ${JSON.stringify(m?.text)}`);
  ok(!a.receiving, `standing by after NNNN (chunk ${n})`);
  ok(a.prev === null, `no PREV after the first message (chunk ${n})`);
}

// 3. Title, quality, body, file.
{
  const a = new NavtexAssembler();
  feed(a, GALE, 4);
  const m = a.live!;
  ok(navtexTitle(m) === 'Station G · Met warning · #98', 'title: ' + navtexTitle(m));
  ok(navtexLostPct(m) === 0, 'no characters lost');
  ok(navtexBody(m).startsWith('ZCZC GB98\n') && navtexBody(m).endsWith('\nNNNN'), 'body framed by ZCZC / NNNN');
  ok(navtexFileName(m) === 'navtex_GB98_2026-10-05T1601Z.txt', 'file name: ' + navtexFileName(m));
  ok(navtexFileText(m).startsWith('NAVTEX · Station G · Met warning · #98\nReceived 2026-10-05 16:01 UTC'), 'file header');
}

// 4. While a message is arriving it is LIVE and grows; the next message moves the last one to PREV.
{
  const a = new NavtexAssembler();
  let t = feed(a, GALE, 6);
  a.push('ZCZC EA12\r\r\nNAV WARNING 345', t);
  ok(a.receiving, 'second message is arriving');
  ok(a.live?.id === 'EA12' && a.live.text === 'NAV WARNING 345' && !a.live.done, 'LIVE shows the growing message');
  ok(a.prev?.id === 'GB98', 'the finished one moved to PREV');
  t = feed(a, ' WRECK IN POSITION\r\r\nNNNN\r\r\n', 3, t + 1000);
  ok(a.live?.id === 'EA12' && a.live.done && a.prev?.id === 'GB98', 'after NNNN: LIVE = the new one, PREV = the old');
  feed(a, 'ZCZC KE01\r\r\nTEST\r\r\nNNNN\r\r\n', 5, t + 1000);
  ok(a.live?.id === 'KE01' && a.prev?.id === 'EA12', 'only ONE previous is kept, like WEFAX');
  ok(navtexTitle(a.live!) === 'Station K · Weather forecast · #01', 'E = forecast: ' + navtexTitle(a.live!));
}

// 5. Damaged header: one ZCZC letter lost, and lost id characters.
for (const hdr of ['ZC_C GB98', '_CZC GB98', 'ZCZ_ GB98', 'ZCZC G_98', 'ZCZC _B98', 'ZCZC GB9_', 'ZCZCGB98', 'ZCZC  GB98']) {
  const a = new NavtexAssembler();
  feed(a, hdr + '\r\r\nGALE WARNING FORTIES\r\r\nNNNN\r\r\n', 4);
  ok(!!a.live && !a.live.startLost && a.live.done, `damaged header "${hdr}" still starts a message`);
}
{
  const a = new NavtexAssembler();
  feed(a, 'ZCZC G_98\r\r\nGALE\r\r\nNNNN\r\r\n', 3);
  ok(a.live?.subject === null && navtexTitle(a.live!) === 'Station G · Subject ? · #98', 'lost subject shown as ?: ' + navtexTitle(a.live!));
  ok(navtexFileName(a.live!).startsWith('navtex_Gx98_'), 'lost id char → x in the file name: ' + navtexFileName(a.live!));
}
// …but two lost ZCZC letters, a space in it, or a mostly-lost id is NOT a header.
for (const bad of ['Z__C GB98', 'ZC C GB98', 'ZCZC ____', 'ZCZC _ _9', 'ZCZE GB98X']) {
  const a = new NavtexAssembler();
  feed(a, bad + '\r\r\n', 3);
  ok(a.live === null, `"${bad}" is not a header`);
}

// 6. Damaged trailer: one N lost still ends it; another letter does not.
{
  const a = new NavtexAssembler();
  feed(a, 'ZCZC GB98\r\r\nGALE\r\r\nNN_N\r\r\n', 2);
  ok(!!a.live?.done && !a.live.endLost && !a.receiving, 'NN_N ends the message');
  const b = new NavtexAssembler();
  feed(b, 'ZCZC GB98\r\r\nPENNNE ISLAND\r\r\n', 2);
  ok(b.receiving && !b.live?.done, 'NNN inside a word does not end it');
}

// 7. Trailer lost: the next header ends it ("[end lost]"), or 75 s of silence does.
{
  const a = new NavtexAssembler();
  let t = feed(a, 'ZCZC GB98\r\r\nGALE WARNING\r\r\n', 4);
  t = feed(a, 'ZCZC GA99\r\r\nFOG\r\r\nNNNN\r\r\n', 4, t + 5000);
  ok(a.prev?.id === 'GB98' && a.prev.endLost && navtexBody(a.prev).endsWith('[end lost]'), 'next header ends a message whose NNNN was lost');
  ok(a.live?.id === 'GA99' && a.live.done && !a.live.endLost, 'and the new one is clean');
}
{
  const a = new NavtexAssembler();
  const t = feed(a, 'ZCZC GB98\r\r\nGALE WARNING\r\r\n', 4);
  ok(!a.tick(t + NAVTEX_END_LOST_MS - 2000) && a.receiving, 'still receiving before the timeout');
  ok(a.tick(t + NAVTEX_END_LOST_MS + 1000), 'timeout fires');
  ok(!a.receiving && a.live?.id === 'GB98' && a.live.endLost && a.live.done, 'silence ends it, end lost');
  ok(!a.tick(t + 3 * NAVTEX_END_LOST_MS), 'a second tick changes nothing');
}

// 8. Joined mid-message: "[start lost]", then the next header moves it to PREV.
{
  const a = new NavtexAssembler();
  let t = feed(a, 'SOUTHWEST 7 TO SEVERE GALE 9 LATER\r\r\nNNNN\r\r\n', 5);
  ok(!!a.live?.startLost && a.live.done && navtexBody(a.live).startsWith('[start lost]'), 'text before any ZCZC is a [start lost] message');
  ok(navtexTitle(a.live!) === 'Start lost' && navtexFileName(a.live!).startsWith('navtex_partial_'), 'start-lost title / file name');
  feed(a, GALE, 7, t + 1000);
  ok(a.live?.id === 'GB98' && !!a.prev?.startLost, 'the fragment becomes PREV');
}
{
  const a = new NavtexAssembler();
  feed(a, 'SOUTHWEST 7 TO SEVERE GALE 9 LATER', 4);
  ok(a.receiving && !!a.live?.startLost && !a.live.done, 'a growing fragment shows live');
}
// A few characters of noise between broadcasts are not a message.
{
  const a = new NavtexAssembler();
  feed(a, GALE + 'E_T 3\r\r\n', 3);
  ok(a.live?.id === 'GB98' && !a.receiving, 'noise after NNNN is not a message');
  feed(a, 'ZCZC GB99\r\r\nX\r\r\nNNNN\r\r\n', 3, T0 + 100_000);
  ok(a.prev?.id === 'GB98', 'and is not kept as PREV');
}

// 9. Quality: the share of lost characters.
{
  const a = new NavtexAssembler();
  feed(a, 'ZCZC GB98\r\r\nGA_E WA_NING\r\r\nNNNN\r\r\n', 4);
  ok(navtexLostPct(a.live!) === 18, 'lost % = 2 of 11: ' + navtexLostPct(a.live!));
}

// 9b. The server's own FEC count (VibeServer op 0x07, 2026-10-05), sent behind the text with its NNNN.
{
  const a = new NavtexAssembler();
  feed(a, 'ZCZC GB98\r\r\nGA_E WARNING\r\r\nNNNN\r\r\n', 4);
  ok(navtexTitle(a.live!) === 'Station G · Met warning · #98', 'no count yet: the plain title');
  const v = a.version;
  ok(a.setFec({ clean: 14, repaired: 2, lost: 1 }) && a.version > v, 'the count attaches to the message NNNN closed');
  ok(navtexTitle(a.live!) === 'Station G · Met warning · #98 · 2 repaired · 1 lost', 'title: ' + navtexTitle(a.live!));
  ok(navtexLostPct(a.live!) === null, 'and the _-derived % steps aside (one answer, not two)');
  ok(navtexFileText(a.live!).startsWith('NAVTEX · Station G · Met warning · #98 · 2 repaired · 1 lost\nReceived '),
     'the saved file carries it: ' + JSON.stringify(navtexFileText(a.live!).split('\n').slice(0, 2)));
  ok(!a.setFec({ clean: 1, repaired: 0, lost: 0 }), 'never twice for one message');
  // The next message arrives: the count is its own, and PREV keeps the old one.
  feed(a, 'ZCZC GA01\r\r\nNO MESSAGES\r\r\nNNNN\r\r\n', 5, T0 + 600_000);
  ok(a.setFec({ clean: 20, repaired: 0, lost: 0 }), 'a second message takes a second count');
  ok(navtexTitle(a.live!) === 'Station G · Nav warning · #01 · no errors', 'all clean says so: ' + navtexTitle(a.live!));
  ok(navtexTitle(a.prev!).endsWith('2 repaired · 1 lost'), 'PREV keeps its own count');
}
{
  // A message closed "[end lost]" (no NNNN) is not the server's message: no count lands on it.
  const a = new NavtexAssembler();
  feed(a, 'ZCZC GB98\r\r\nGALE\r\r\nZCZC GB99\r\r\nX', 4);
  ok(a.prev?.endLost === true && !a.setFec({ clean: 5, repaired: 0, lost: 0 }), 'no count on an [end lost] message');
  // Nor on a "[start lost]" fragment: the server counted from a boundary this box never saw.
  const b = new NavtexAssembler();
  feed(b, 'VIKING NORTH UTSIRE SOUTHWEST 7 TO SEVERE GALE 9\r\r\nNNNN\r\r\n', 4);
  ok(b.live?.startLost === true && !b.setFec({ clean: 5, repaired: 0, lost: 0 }), 'no count on a [start lost] fragment');
  ok(!new NavtexAssembler().setFec({ clean: 1, repaired: 0, lost: 0 }), 'nothing to attach to: ignored');
}

// 11. ★ A RETUNE MID-MESSAGE (NickB, 2026-10-12: 518 → 490 part way through, and 490's text joined 518's message
//     under its id). clearLive() — what a retune and CLR do — drops the message arriving; what follows is a new
//     "[start lost]" fragment, never a continuation; finished messages stay, the last under PREV.
{
  const a = new NavtexAssembler();
  let t = feed(a, GALE, 4);                                         // finished: GB98
  t = feed(a, 'ZCZC GA01\r\r\nNAV WARNING 537 SEATRIALS WITH UNMA', 4, t + 60_000);
  ok(a.receiving && a.live?.id === 'GA01', 'GA01 arriving on 518');
  const v = a.version;
  a.clearLive();                                                    // the dial moves to 490
  ok(a.version > v && !a.receiving && a.live === null, 'retune: LIVE is empty, standing by');
  ok(a.prev?.id === 'GB98' && a.prev.done && !a.prev.endLost, 'retune: the finished GB98 is untouched, under PREV');
  feed(a, 'SECURITE ROUTE IROISE ZONE DE TIR ACTIVE\r\r\n', 4, t + 1000);
  const m = a.live!;
  ok(m.startLost && m.id === null, '490 text is a [start lost] fragment, not GA01: ' + JSON.stringify(m.id));
  ok(!m.text.includes('SEATRIALS') && m.text.startsWith('SECURITE'), "518's partial text is gone: " + JSON.stringify(m.text));
  ok(a.prev?.id === 'GB98', 'PREV still GB98 while the fragment arrives');
  feed(a, 'ZCZC NA12\r\r\nAVURNAV\r\r\nNNNN\r\r\n', 4, t + 120_000);
  ok(a.live?.id === 'NA12' && a.live.done && a.live.text === 'AVURNAV', "490's own header starts its own message");
  ok(a.prev?.startLost === true && a.prev.endLost && !a.prev.text.includes('SEATRIALS'), 'the fragment closed [end lost], under PREV');
  ok(a.setFec({ clean: 9, repaired: 0, lost: 0 }) && !!a.live.fec && !a.prev.fec, "a count lands on 490's message only");
}
{
  // Cleared with nothing finished yet: still nothing to show; a header split across the retune is not completed.
  const a = new NavtexAssembler();
  feed(a, 'ZCZC GA0', 4);
  a.clearLive();
  ok(a.live === null && a.prev === null, 'clear with no finished message: empty');
  feed(a, '1 HELLO\r\r\nNNNN\r\r\n', 4, T0 + 5000);
  ok(a.live === null && a.prev === null, 'the header\'s tail after a retune does not make a message');
  // And a CLR right after a message finished: LIVE empty, the message is PREV.
  const b = new NavtexAssembler();
  feed(b, GALE, 4);
  b.clearLive();
  ok(b.live === null && b.prev?.id === 'GB98', 'CLR after NNNN: the message moves to PREV');
  ok(b.tick(T0 + 10 * NAVTEX_END_LOST_MS) === false, 'no [end lost] tick after a clear');
}

// 11b. ★★ A BLIP IS NOT AN END (NickB's 490 kHz overnight, 2026-10-22: one clean message came out as "[end lost]" and five
//      "[start lost]" pieces — the decoder resynced on weak patches and relocked within a second each time).
{
  const a = new NavtexAssembler();
  a.lockState(3, T0);
  let t = feed(a, 'ZCZC BA09\r\r\n070825 UTC OCT\r\r\nOOSTENDERADIO LOKAAL BERICHT LB013/26\r\r\n', 4);
  for (let k = 0; k < 4; k++) {                                // four resyncs, each relocked in under a second
    a.lockState(1, t); a.lockState(3, t + 800); t += 900;
    t = feed(a, `KUSTROUTE DEEL ${k} BIJZONDERE MARKERINGSBOEI\r\r\n`, 4, t);
  }
  t = feed(a, 'NNNN\r\r\n', 4, t);
  const m = a.live!;
  ok(m.id === 'BA09' && m.done && !m.endLost && !m.startLost && a.prev === null, '★ four blips mid-message: still ONE message, ZCZC to NNNN');
  ok(m.text.includes('DEEL 0') && m.text.includes('DEEL 3'), '...with all of its text');
  // And a lock lost for good mid-message ends it at the cut once LOCK_BLIP_MS has passed (tick), not 75 s later.
  const b = new NavtexAssembler();
  b.lockState(3, T0);
  let u = feed(b, 'ZCZC GA01\r\r\nHALF A MESSAGE', 4);
  b.lockState(1, u);
  ok(!b.tick(u + 1000) && b.receiving, 'lost 1 s: still open (it may be a blip)');
  // ★ beta4 (Lenovo, real fades of 5–15 s): a lock loss never closes a message — only NNNN, a new ZCZC, or 75 s of silence.
  ok(!b.tick(u + 1600) && b.receiving, 'lost 1.6 s: still open (it may be a fade)');
  ok(b.tick(u + NAVTEX_END_LOST_MS + 1) && b.live?.endLost && b.live.text === 'HALF A MESSAGE', 'silent for 75 s: closed [end lost], at the cut');
}
// 11c. ★★★ FADES (Lenovo, 12.0.0~beta3, 517.5 kHz: lock lost 5–15 s at a time): ONE message, the gaps shown and counted.
{
  const a = new NavtexAssembler();
  a.lockState(3, T0);
  let t = feed(a, 'ZCZC VA28\r\r\nDOVER STRAIT WIND SOUTHWEST 6\r\r\n', 4);
  a.lockState(1, t); feed(a, 'XQ_', 3, t + 500); a.lockState(3, t + 12_000); t += 12_100;   // a 12 s fade, garbage while hunting
  t = feed(a, 'THAMES SOUTHWEST 5 TO 7\r\r\n', 4, t);
  a.lockState(1, t); a.lockState(3, t + 15_000); t += 15_100;                               // and a 15 s one
  t = feed(a, 'DOVER RAIN LATER\r\r\nNNNN\r\r\n', 4, t);
  const m = a.live!;
  ok(m.id === 'VA28' && m.done && !m.endLost && !m.startLost && a.prev === null, '★ two long fades mid-message: ONE message, ZCZC to NNNN, not partial');
  ok((m.text.match(/signal lost/g) || []).length === 2 && m.text.includes('[\u2026 signal lost 12 s \u2026]') && m.text.includes('[\u2026 signal lost 15 s \u2026]'),
     '...with two gap markers where it faded: ' + JSON.stringify(m.text));
  ok(!m.text.includes('XQ_'), '...and the hunting garbage dropped');
  ok(m.gaps === 2 && navtexTitle(m).includes('gaps: 2 (27 s)'), `...its title says so: "${navtexTitle(m)}"`);
  // A fade, then ANOTHER station's ZCZC: two messages (the takeover protection).
  const b = new NavtexAssembler();
  b.lockState(3, T0);
  let u = feed(b, 'ZCZC VA29\r\r\nFIRST STATION TEXT\r\r\n', 4);
  b.lockState(1, u); b.lockState(3, u + 8000); u += 8100;
  u = feed(b, 'ZCZC LB04\r\r\nSECOND STATION\r\r\nNNNN\r\r\n', 4, u);
  ok(b.prev?.id === 'VA29' && b.prev.endLost && b.live?.id === 'LB04' && !b.live.text.includes('FIRST'), '★ a fade then a different ZCZC: two messages');
}

// 12. ★★ CO-CHANNEL TAKEOVER on one frequency (NickB, 2026-10-12, 490 kHz: E transmitting, C comes up on top with a
//     short message and drops, E is decoded again — "the remainder of E got assigned to C until the next E message").
const E1 = 'ZCZC EA12\r\r\nFRENCH NAV WARNING 412 ROUTE IROISE\r\r\nBUOY UNLIT IN POSITI';
const E2 = 'ON 48-20N 005-10W\r\r\nCANCEL THIS MSG 041200 UTC OCT\r\r\n';
{
  // (a) C complete (its NNNN received), the lock kept throughout — what the assembler sees with no server state at all.
  const a = new NavtexAssembler();
  let t = feed(a, E1, 4);
  t = feed(a, 'ZCZC CB01\r\r\nGALE WARNING SOLE\r\r\nNNNN\r\r\n', 4, t);
  ok(a.prev?.id === 'EA12' && a.prev.endLost && a.prev.done, 'C\'s ZCZC ends E [end lost] under E\'s id');
  ok(!!a.prev && !a.prev.text.includes('GALE'), 'none of C is in E');
  ok(a.live?.id === 'CB01' && a.live.done && !a.live.endLost && a.live.text === 'GALE WARNING SOLE', 'C complete on its own');
  feed(a, E2, 4, t);
  const m = a.live!;
  ok(m.startLost && m.id === null && m.text.startsWith('ON 48-20N'), 'E\'s remainder is a [start lost] fragment: ' + JSON.stringify(m.id));
  ok(a.prev?.id === 'CB01' && a.prev.text === 'GALE WARNING SOLE', 'C keeps only its own text');
}
{
  // (b) C fades with no NNNN: the server's decoder loses its lock at the takeover and again at the fade (op 0x03).
  const a = new NavtexAssembler();
  a.lockState(3, T0);
  let t = feed(a, E1, 4);
  // ★★ 2026-10-22: a lock lost no longer closes AT ONCE (a one-second resync split NickB's clean messages) — the CUT is
  //    remembered, and C's own ZCZC (or a loss longer than LOCK_BLIP_MS) ends E there.
  ok(!a.lockState(1, t) && a.receiving, 'takeover: the lock lost — E is held open at the cut, not closed yet');
  t = feed(a, 'X_Q', 1, t);                                   // a few characters of garbage while it hunts
  a.lockState(3, t);
  t = feed(a, 'ZCZC CB01\r\r\nGALE WARNING SOLE SEVERE GALE 9 IMMINENT\r\r\nFITZRO', 4, t);
  ok(a.prev?.id === 'EA12' && a.prev.endLost && a.prev.done && a.prev.text.endsWith('POSITI'), "C's ZCZC ends E at the cut: [end lost], its text as far as it came");
  ok(a.live?.id === 'CB01' && !a.live.done && a.prev?.id === 'EA12' && !a.prev.text.includes('X_Q'), 'C arriving, E under PREV, the garbage dropped');
  // ★★ beta4 (Stuart, the Lenovo's real fades): a lock loss no longer closes a message at all — only a ZCZC does. So C
  //    fading with no NNNN and E's remainder arriving after it now stays in C's message, with the gap SHOWN where it was;
  //    a NEW ZCZC still always splits (the takeover protection above, and the next test).
  ok(!a.lockState(0, t), 'fade: the lock lost — C held open');
  ok(!a.lockState(3, t + 2000) && a.receiving && a.live?.id === 'CB01', '...relocked 2 s later: C carries on, with a gap marker');
  feed(a, E2, 4, t + 2000);
  const m = a.live!;
  ok(m.id === 'CB01' && m.text.includes('[\u2026 signal lost 2 s \u2026]') && m.gaps === 1, 'what follows a fade joins C after the marker (the trade made for real fades)');
  feed(a, 'NNNN\r\r\n', 4, t + 60_000);
  ok(a.live?.id === 'CB01' && a.live.done && !a.live.endLost, 'and the NNNN closes it as one message');
}
{
  // Only a 3 → below-3 change is an event: NoSignal ↔ hunting while unlocked, a first report, a lock regained.
  const a = new NavtexAssembler();
  feed(a, 'ZCZC GA01\r\r\nTEXT', 4);
  ok(!a.lockState(1, T0) && a.receiving, 'a first report (unlocked) closes nothing');
  ok(!a.lockState(0, T0) && !a.lockState(1, T0) && !a.lockState(3, T0) && a.receiving, 'level-gate flips and a relock close nothing');
  // A lock lost with only noise open: dropped, not glued to the next transmission's text.
  const b = new NavtexAssembler();
  b.lockState(3, T0); feed(b, 'E_T', 1); b.lockState(1, T0 + 5000);
  feed(b, 'ZCZC GA01\r\r\nNO MESSAGES\r\r\nNNNN\r\r\n', 4, T0 + 6000);
  ok(b.live?.id === 'GA01' && b.prev === null, 'noise before a lost lock makes no message');
  // Station letters are per message, never per station: the same B1 on two frequencies (or another letter for the same
  // transmitter on 490 than on 518) is just two messages — nothing is keyed or merged by id.
  const c = new NavtexAssembler();
  let t = feed(c, 'ZCZC EA12\r\r\nONE\r\r\nNNNN\r\r\n', 4);
  feed(c, 'ZCZC EA12\r\r\nTWO\r\r\nNNNN\r\r\n', 4, t + 1000);
  ok(c.prev?.text === 'ONE' && c.live?.text === 'TWO', 'two messages with one id stay two messages');
}

// 10. reset() clears everything (open / close / switch).
{
  const a = new NavtexAssembler();
  feed(a, GALE + 'ZCZC GA01\r\r\nX', 4);
  const v = a.version;
  a.reset();
  ok(a.live === null && a.prev === null && !a.receiving && a.version > v, 'reset clears and bumps the version');
}

// 11. ★ V12 beta 4: the STATION by name (navtexStations.generated.ts, NGA Pub. 117) — the Lenovo's evening of
//     2026-10-22 at IO92nh (Moulton, Northamptonshire). The server's copy of the rule is tested with the same cases in
//     vibeserver/test-bgdecode.cpp.
{
  const LA = 52.3125, LO = -0.875;
  const L = (b1: string, hz: number, text = '', la = LA, lo = LO) => navtexStationLabel(b1, hz, la, lo, text);
  ok(L('B', 489500) === 'B · Oostende, Belgium \u{1F1E7}\u{1F1EA}', 'B on 490 from Moulton = Oostende, Belgium + flag: ' + L('B', 489500));
  ok(L('U', 489500).startsWith('U · Cullercoats, United Kingdom'), 'U on 490 = Cullercoats');
  ok(L('A', 489500).startsWith('A · Malin Head, Ireland'), 'A on 490 = Malin Head');
  ok(L('L', 489500).startsWith('L · Hamburg, Germany'), 'L on 490 = Hamburg (731 km)');
  ok(L('G', 517500).startsWith('G · Cullercoats, United Kingdom'), 'G on 518 = Cullercoats');
  ok(L('V', 517500).startsWith('V · Oostende, Belgium'), 'V on 518 = Oostende');
  ok(L('L', 517500).startsWith('L · Rogaland, Norway'), 'L on 518 = Rogaland (813 km): ' + L('L', 517500));
  ok(L('J', 517500).startsWith('J · Stockholm, Sweden'), 'J on 518 = Stockholm (1053 km)');
  ok(L('I', 517500).startsWith('I · Varberg, Sweden'), 'I on 518 = Varberg (1004 km)');
  ok(L('N', 517500).startsWith('N · Orlandet, Norway'), 'N on 518 = Orlandet (1400 km)');
  // ★ 2026-10-23: Jeløy (Norway, M 518) is in the supplementary list (Wikipedia) — Pub. 117 2021 had only Casablanca
  ok(L('M', 517500) === 'M \u00b7 Jeløy \u{1F1F3}\u{1F1F4} or Casablanca \u{1F1F2}\u{1F1E6}?', 'M on 518, no evidence: Jeløy (1040 km) and Casablanca (2150 km) too close to call: ' + L('M', 517500));
  ok(L('W', 517500) === 'W \u00b7 Corsen \u{1F1EB}\u{1F1F7} or Valentia \u{1F1EE}\u{1F1EA}?', 'W on 518 with no evidence: Corsen 520 km / Valentia 648 km — both, asked: ' + L('W', 517500));
  ok(L('W', 517500, 'VALENTIA RADIO GALE WARNING').startsWith('W · Valentia, Ireland'), 'the text names Valentia → Valentia');
  ok(L('W', 517500, 'CROSS CORSEN').startsWith('W · Corsen, France'), 'the text names Corsen → Corsen');
  ok(L('W', 517500, 'ZCZC WB51\nMRSC MALIN, EJM.\nGALE WARNING THURSDAY 22 OCTOBER 2149 UTC 12\nSHANNON\nVIOLENT STORM FORCE 11\nNNNN').endsWith(' or Valentia \u{1F1EE}\u{1F1EA}?'),
     "tonight's W (MRSC MALIN — Malin Head is not on W/518, so it does not count; no positions): still the two");
  // ★★ the likelihood score (owner-approved): positions, language, DX
  ok(L('W', 517500, 'IRISH COASTAL WARNING\nSUBMARINE CABLE OPERATIONS IN POSITION 51-30.2N 010-45.0W AND 51-41N 010-30W').startsWith('W \u00b7 Valentia, Ireland'),
     'a W whose positions are off SW Ireland → Valentia: ' + L('W', 517500, '51-30.2N 010-45.0W 51-41N 010-30W'));
  ok(L('W', 517500, 'AVURNAV BREST 1234\nZONE 48-10N 005-20W\n48-15.5N 005-05.0W').startsWith('W \u00b7 Corsen, France'), 'a W off Brittany → Corsen');
  ok(L('B', 489500, 'ZCZC BA10\nKUSTWACHT OOSTENDE BERICHT\nWRAK OP POSITIE 51-14.13N 002-54.22E\nNNNN').startsWith('B \u00b7 Oostende, Belgium'),
     'BA10 with its coordinates off the Belgian coast → Oostende');
  const carib = 'ZCZC RA21\nNAVAREA IV 0912/26\nCARIBBEAN SEA. PUERTO RICO.\nUNLIGHTED BUOY ADRIFT IN 18-20.5N 066-30.2W AND 18-11N 065-58W.\nNNNN';
  const rc = L('R', 517500, carib);
  ok(/^R · Isabella, United States 🇺🇸 · DX [0-9],[0-9]00 km$/.test(rc), '★ a Caribbean warning with Caribbean positions on R/518 from the UK → Isabella (Puerto Rico), DX: ' + rc);
  ok(L('R', 517500) === 'Station R' || !L('R', 517500).includes('Isabella'), 'the same letter with no evidence never names Isabella from the UK');
  ok(L('M', 517500, 'SWEDISH NAV WARN 1/26\nSKAGERRAK. KOSTER.\nSOUTH CARDINAL MARK MISSING\nPSN 58-41.9N 010-51.2E').startsWith('M \u00b7 Jeløy, Norway'),
     "tonight's M: a Swedish warning, positions in the Skagerrak — Jeløy (supplement), 85 km from them; Pub. 117 2021 had no M near");
  ok(L('L', 489500, 'NCC-HAMBURG\nNAUT. WARN. NR. 520\nMUNITIONSFUNDE AUF POSITION 54-19,2N 005-44,2E').startsWith('L \u00b7 Hamburg, Germany'), 'German comma decimals parse');
  ok(L('J', 517500, 'SWEDISH NAV WARN 225/26\nBOTHNIAN SEA. 60-40.1N 017-55.0E') .startsWith('J \u00b7 Stockholm, Sweden'), 'J with a Baltic position → Stockholm');
  ok(L('B', 489500, '', NaN, NaN) === 'Station B', 'no server position → the letter only');
  ok(L('B', 489500, 'OOSTENDERADIO NAV WARNING', NaN, NaN).startsWith('B · Oostende'), 'no position, but the text names it');
  ok(L('L', 489500, 'NCC-HAMBURG', -33.9, 18.4).startsWith('L · Hamburg'), 'the text wins wherever the server is');
  ok(L('B', 4209500) === 'Station B', 'HF 4209.5: Pub. 117 lists no HF letters — the letter only');
  ok(L('B', 10_000_000) === 'Station B', 'not on a NAVTEX channel → the letter only');
  ok(L('B', 489500, '', -33.9, 18.4) === 'Station B', 'B on 490 from Cape Town: no evidence, nothing near');
  ok(L('L', 489500, 'NCC-HAMBURG', -33.9, 18.4).includes(' \u00b7 DX 9,'), 'named from far away → "DX 9,…00 km": ' + L('L', 489500, 'NCC-HAMBURG', -33.9, 18.4));
  // ★★ 2026-10-23 — MONDOLFO. Received on 489.5 kHz USB at Stuart's server; it was labelled "E · Corsen, France".
  const MONDOLFO = 'ZCZC EA17\n230440 UTC OCT 26\nMONDOLFO RADIO\n\nCOSTAVURNAV NR. 576/26 OTT 17 (1 RILANCIO)\nMAR ADRIATICO  RAVENNA\n'
    + 'SULLE SEGUENTI PIATTAFORME IN PSN:\n 44 31.4N A   012 30.6E  GARIBALDI A NR. 4039.2 EFF\n'
    + 'I SISTEMI DI AUSILIO ALLA NAVIGAZIONE ISTALLATI NON SONO FUNZIONANTI\nNNNN';
  ok(L('E', 489500, MONDOLFO) === 'E \u00b7 Mondolfo, Italy \u{1F1EE}\u{1F1F9}', '★ the Mondolfo message → Mondolfo, Italy (supplement): ' + L('E', 489500, MONDOLFO));
  ok(!L('E', 489500, MONDOLFO).includes('Corsen'), '★ never Corsen again');
  // (b) a station the table does NOT have, naming itself — on T 490, where the candidates are Niton and Tarifa
  const ANCONA = MONDOLFO.replace('ZCZC EA17', 'ZCZC TA17').replace('MONDOLFO RADIO', 'ANCONA RADIO');
  // ★ 2026-11-01: a name the table does not have is NEVER shown ("Condogno Radio, Italy?" did not exist) — the letter only
  ok(L('T', 489500, ANCONA) === 'Station T', '★ (b) not in the table, Adriatic positions + Italian rule out Niton and Tarifa → the letter only: ' + L('T', 489500, ANCONA));
  const ANCONA_NOPOS = 'ZCZC TA17\nANCONA RADIO\nI SISTEMI DI AUSILIO ALLA NAVIGAZIONE NON SONO FUNZIONANTI SULLE PIATTAFORME';
  ok(L('T', 489500, ANCONA_NOPOS) === 'Station T', '(b) no positions, Italian, an unknown name → the letter only: ' + L('T', 489500, ANCONA_NOPOS));
  ok(!L('T', 489500, 'ZCZC TA17\nWIDGET RADIO\nGALE WARNING\nSTORM IN 30-00N 040-00W').includes('Widget'), '(b) never "Widget Radio"');
  ok(L('M', 517500, 'ZCZC MA01\nOOSTENDERADIO\nWRAK OP POSITIE 51-14.13N 002-54.22E') === 'M \u00b7 Suspected Oostende, Belgium \u{1F1E7}\u{1F1EA}',
     '(b) a table station on another letter names itself → Suspected, its own name and country');
  ok(L('T', 489500, 'ZCZC TA17\nWIDGET RADIO\nTHE BUOY IS MISSING AND WILL BE REPLACED FROM MONDAY').startsWith('T \u00b7 Niton, United Kingdom'),
     '(b) an unknown name whose English Niton speaks → no contradiction, Niton as before');
  ok(L('T', 489500, 'ZCZC TA17\nWIDGET RADIO\nWRECK 50-30N 001-10W').startsWith('T \u00b7 Niton, United Kingdom'),
     '(b) an unknown name, positions that fit Niton → Niton (positions outrank the name)');
  // ★★ Niton's T on 490 sends FRENCH (Météo-France for the Channel): language alone never overrides the table
  const NITON_FR = 'ZCZC TA45\n230800 UTC OCT 26\nORIGINE METEO-FRANCE\nBULLETIN METEOROLOGIQUE SPECIAL POUR LA MANCHE\n'
    + 'AVIS DE GRAND FRAIS A COUP DE VENT SUR LES ZONES ET DANS LE PAS DE CALAIS. MER AGITEE A FORTE ENTRE LE CAP ET LA HAGUE\nNNNN';
  ok(L('T', 489500, NITON_FR) === 'T \u00b7 Niton, United Kingdom \u{1F1EC}\u{1F1E7}', '★ Niton T in French stays Niton: ' + L('T', 489500, NITON_FR));
  ok(L('T', 489500, NITON_FR + '\n49-50N 001-20W 50-10.5N 000-30.0E').startsWith('T \u00b7 Niton, United Kingdom'), 'Niton T in French with Channel positions: Niton');
  ok(L('T', 489500, NITON_FR.replace('ORIGINE METEO-FRANCE', 'CROSS RADIO')).startsWith('T \u00b7 Niton'), '"CROSS RADIO" is not a station name');
  // the supplement's other fixes
  ok(L('U', 517500, 'ZCZC UA03\nMONDOLFO RADIO\nNAV WARNING').startsWith('U \u00b7 Mondolfo, Italy'), 'U 518 naming Mondolfo → Mondolfo (supplement; Trieste kept)');
  ok(L('S', 517500, 'NCC-HAMBURG\nNAUT. WARN.').startsWith('S \u00b7 Hamburg, Germany'), 'S 518 = Hamburg (Pub. 117 gave no frequency)');
  ok(L('S', 4209500, 'GREEK NAVTEX 4209.5\n35-20N 025-10E').startsWith('S \u00b7 Irakleio, Greece'), 'HF 4209.5: a Wikipedia letter, with a fitting position');
  // ★★★ 2026-11-01 — the additive clues: the same cases as vibeserver/test-navtex-map.cpp (the server's copy)
  {
    const S = (t: string) => Date.parse(t) / 1000;
    const R = (b1: string, hz: number, t: string, utc: number, hdr: string) => navtexResolve(b1, hz, LA, LO, t, utc, hdr);
    const IT = 'Italy \u{1F1EE}\u{1F1F9}', DE = 'Germany \u{1F1E9}\u{1F1EA}';
    const fx = (f: string) => {
      const raw = fs.readFileSync(new URL('./navtex-fixtures/files/' + f, import.meta.url), 'utf8').split('\n');
      const hz = /(\d+)\.(\d+) kHz/.exec(raw[2])!; const body = raw.slice(4);
      const end = body.findIndex((l, i) => i > 0 && (l === 'NNNN' || l === '[end lost]'));
      const m = /^(\d{4})(\d\d)(\d\d)-(\d\d)(\d\d)(\d\d)/.exec(f)!;
      return { id: body[0].slice(5), text: body.slice(1, end < 0 ? undefined : end).join('\n'), hz: +hz[1] * 1000 + +hz[2],
               utc: Date.UTC(+m[1], +m[2] - 1, +m[3], +m[4], +m[5], +m[6]) / 1000 };
    };
    const lab = (f: string) => { const m = fx(f); return navtexLabelOf(m.id[0], R(m.id[0], m.hz, m.text, m.utc, m.id)); };
    { const m = fx('20261025-164259-navtex-489k5-7503.txt'); const r = R(m.id[0], m.hz, m.text, m.utc, m.id); const l = navtexLabelOf('B', r);
      ok(l === 'E · Suspected Mondolfo, ' + IT && r?.conf === 'likely', '7503 (BD12, CONDOGNO RADIO, outside B’s slots) → Suspected Mondolfo: ' + l);
      ok(r?.why?.[0] === 'Header letter looks corrupted: started outside B’s slots; Mondolfo identifies itself', '…the why: ' + r?.why?.[0]); }
    { const m = fx('20261025-004258-navtex-489k5-f890.txt'); const r = R('E', m.hz, m.text, m.utc, m.id);
      ok(navtexLabelOf('E', r) === 'E · Mondolfo, ' + IT && r?.conf === 'confident', 'f890 (MONDOLFO RADIO) stays confident Mondolfo'); }
    ok(/^N · Orlandet, Norway/.test(lab('20261024-221012-navtex-517k5-7e1e.txt')), '7e1e (NA79, garbled position) → Ørlandet: ' + lab('20261024-221012-navtex-517k5-7e1e.txt'));
    for (const f of ['20261027-220142-navtex-517k5-278a.txt', '20261028-220041-navtex-517k5-f67c.txt'])
      ok(lab(f) === 'M · Suspected Jeløy, Norway \u{1F1F3}\u{1F1F4}', f.slice(28, 32) + ' (MA05, garbled position) → Suspected Jeløy: ' + lab(f));
    ok(lab('20261101-004012-navtex-489k5-cc32.txt') === 'E · Suspected Corsen, France \u{1F1EB}\u{1F1F7}', 'cc32 (EA14, AVURNAV BREST) → Suspected Corsen');
    const JA = '240040 UTC OCT 26\nMTNDOOFO AADIO\n\nCOSTAVURNAV 396/26 AGO 30\nMAR ADRIATICO CENTRALE - TERMOLI.\nIL FARO RISULTA NON FUNZIONANTE.';
    ok(navtexStationLabel('J', 489500, LA, LO, JA, S('2026-10-24T00:43:00Z'), 'JA12') === 'E · Suspected Mondolfo, ' + IT, 'synthetic JA12 MTNDOOFO AADIO 00:43 → Suspected Mondolfo (E)');
    ok(!L('J', 489500, JA).includes('Talcahuano'), '…with no time, never Talcahuano: ' + L('J', 489500, JA));
    const CS = 'NCC-AMBURG\nNAV. WARN. NO. 563\nGERMAN BIGHT\nWINDPARK BUTENDIEK LIGHTS UNRELIABLE';
    const rcs = R('C', 517500, CS, S('2026-10-05T23:01:15Z'), 'CS');
    ok(navtexLabelOf('C', rcs) === 'S · Suspected Hamburg, ' + DE && !!rcs?.why?.[1]?.includes('shifted'), "synthetic 'ZCZC CS' NCC-AMBURG 23:01 → Suspected Hamburg (S), never Odesa");
    const VL = '180550 NAVTEXJAHAMBURG (NCC)\nSEEWETTERVORHERSAGE\nVORHERSAGE FUER SONNTAG:\nDEUTSCHE BUCHT\nNE 4 BIS 5, SEE ZTW. 3M.\nWESTLICHE OSTSEE\nOST 4, SPAETER 5 UND DIE SEE';
    const lvl = navtexStationLabel('V', 489500, LA, LO, VL, S('2026-10-18T05:50:51Z'), 'VL');
    ok(lvl === 'L · Suspected Hamburg, ' + DE, "synthetic 'ZCZC VL' NAVTEXJAHAMBURG 05:50 → Suspected Hamburg (L), never Yarmouth: " + lvl);
    ok(navtexStartLostLabel(489500, LA, LO, 'ZCZC VL\n' + VL, S('2026-10-18T05:50:51Z')) === lvl, '…the same for a start-lost message beginning ZCZC VL');
    const BA43 = 'NORWEGIAN NAV. WARNING 356/2026\nCHART 67\nAREA ANDHOLMFJORDEN\nMAALOEY-SKARHOLMEN LIGHTHOUSE 67-46.08N 014-24.08E IS UNLIT.';
    ok(/^B · Bodo, Norway/.test(navtexStationLabel('B', 517500, LA, LO, BA43, S('2026-10-08T04:10:41Z'), 'BA43')), 'synthetic BA43 → Bodø, never Algiers');
    const BA41 = '281446 UTC SEP 26\nNORWEGIAN NAV. WARNING 347/2026\nCHART 84\nAREA GIBOSTAD\nNORDGRUNNEN LIGHT 69-21.40N 018-05.84E IS UNLIT.';
    const r41 = R('B', 517500, BA41, S('2026-10-07T00:15:47Z'), 'BA41');
    ok(/^B · Bodo, Norway/.test(navtexLabelOf('B', r41)) && r41?.conf === 'confident', "UberSDR's BA41 (Gibostad) → Bodø, confident");
    ok(/^Q · Irakleio, Greece/.test(L('Q', 489500, 'STATHMOS IRAKLEIOY AR.MIN 370/26\nKRITIKO PELAGOS')), 'STATHMOS IRAKLEIOY → Irakleio');
    ok(/^P · Kerkyra, Greece/.test(L('P', 489500, 'STATHMOS KERKYRA AR.MIN 227/25\nIONIO PELAGOS')), 'STATHMOS KERKYRA → Kerkyra');
    ok(navtexSelfId('STATHMOS I_AKLEIO AR.MUN. 223/26', [], [])?.L === 'IRAKLEIO', 'STATHMOS I_AKLEIO → Irakleio');
    ok(L('T', 489500, 'ZCZC TA17\nWIDGET RADIO\nMAR ADRIATICO 44-31N 012-30E\nI SISTEMI DI AUSILIO NON SONO FUNZIONANTI SULLE PIATTAFORME') === 'Station T', 'an unknown name is never shown');
    ok(navtexSelfId('MONDOLFO RADIO', [], [])?.L === 'MONDOLFO' && navtexSelfId('PHIT RADIO', [], []) === null, 'PHIT RADIO (nothing backing it) is ignored');
  }
  // the position parser
  {
    const P = navtexPositions;
    const one = (t: string) => { const p = P(t); return p.length === 1 ? p[0] : null; };
    const near = (a: [number, number] | null, la: number, lo: number) => !!a && Math.abs(a[0] - la) < 0.01 && Math.abs(a[1] - lo) < 0.01;
    ok(near(one('IN 51-14.13N 002-54.22E.'), 51.2355, 2.9037), 'DD-MM.mmN DDD-MM.mmE');
    ok(near(one('5114N 00254E'), 51.2333, 2.9), 'DDMMN DDDMME');
    ok(near(one('51 14.1N 002 54.2E'), 51.235, 2.9033), 'DD MM.mN DDD MM.mE');
    ok(near(one('54-19,2N  005-44,2E'), 54.32, 5.7367), 'comma decimals, two spaces');
    ok(near(one('33-55S 018-25E'), -33.9167, 18.4167), 'south and east');
    ok(P('PSN 58-41.9N 010-51._E').length === 0, "a lost '_' → ignored, never half-read");
    ok(P('99-10N 010-10E 51-75N 002-00E 51-10N 190-00E').length === 0, 'out-of-range degrees and minutes → ignored');
    ok(P('TEL 1234567 2149 UTC 12 CHART 28').length === 0, 'numbers that are not positions → none');
    ok(near(one(' 44 31.4N A   012 30.6E  GARIBALDI A'), 44.5233, 12.51), "ITA2: a lost figures-shift '-' printed as A between the halves");
    ok(near(one('44-31N - 012-30E'), 44.5167, 12.5), "a '-' between the halves");
    ok(P('51-10N AND 002-00E').length === 0, "a word between the halves is not a lost '-'");
  }
  const m = new NavtexAssembler();
  feed(m, 'ZCZC BA10\r\r\nOOSTENDERADIO\r\r\nNNNN\r\r\n', 5);
  const t = navtexTitle(m.live!, { dialHz: 489500, lat: LA, lon: LO });
  ok(t.startsWith('B · Oostende, Belgium \u{1F1E7}\u{1F1EA} · Nav warning · #10'), 'the title carries the station: ' + t);
  ok(navtexTitle(m.live!).startsWith('Station B · Nav warning'), 'no place → "Station B" as before');
  ok(navtexFileText(m.live!, { dialHz: 489500, lat: LA, lon: LO }).startsWith('NAVTEX · B · Oostende'), 'SAVE writes the station too');
}

console.log(`navtex: ${pass} passed, ${fail} failed`);
if (fail) process.exit(1);
