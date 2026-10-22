// test_navtex.ts — NAVTEX message blocks from the decoder's character stream (src/utils/navtex.ts, 2026-10-05).
// Real-shaped traffic: CR CR LF line ends, '_' for a lost character (fsk_decoder.cpp), chunks split anywhere.
import { NavtexAssembler, NAVTEX_END_LOST_MS, navtexTitle, navtexLostPct, navtexBody, navtexFileName, navtexFileText }
  from '../src/utils/navtex.ts';
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
  ok(b.tick(u + 1600) && b.live?.endLost && b.live.text === 'HALF A MESSAGE', 'lost 1.6 s: closed [end lost] at the cut');
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
  ok(!a.lockState(0, t), 'fade: the lock lost — C held open at the cut');
  ok(a.lockState(3, t + 2000), '...relocked 2 s later (longer than a blip): C closes at the cut');
  ok(a.live?.id === 'CB01' && a.live.endLost && a.live.text.endsWith('FITZRO'), 'C [end lost], only its own text');
  feed(a, E2, 4, t + 2000);
  const m = a.live!;
  ok(m.startLost && m.id === null && m.text.startsWith('ON 48-20N'), "E's remainder is [start lost], not C: " + JSON.stringify(m.text.slice(0, 20)));
  ok(a.prev?.id === 'CB01' && !a.prev.text.includes('48-20N'), 'nothing of E joined C');
  // E's own NNNN closes the fragment — it is not attached to C (or to E) by guesswork, and takes no FEC count.
  feed(a, 'NNNN\r\r\n', 4, t + 60_000);
  ok(a.live?.startLost === true && a.live.done && !a.setFec({ clean: 1, repaired: 0, lost: 0 }), 'its NNNN ends the fragment; no count');
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

console.log(`navtex: ${pass} passed, ${fail} failed`);
if (fail) process.exit(1);
