// "In use by another app" through the directory (Stuart, 2026-10-18: "make it so the directory shows the
// server but shows the radio in use in another app").
//
//   node directory/scripts/test-in-use.mjs
//
// ★★★ THE FAULT IT GUARDS. On the Lenovo, OpenWebRX held the RSP1A for 17 hours. The radio's process never
//     started, the front door answered its /r/<id>/vibeserver.json with 503, and the directory page — given a
//     503 for the one radio of a single-radio server — fell back to the MACHINE's answer (the door's: no radio,
//     0 listeners) and drew the radio FREE. Three things now have to hold:
//       1. the Worker passes `inUseElsewhere` through to the listing (and an older server, which never sends
//          it, is unchanged);
//       2. the page treats a radio the server says is held as IN USE, never free;
//       3. a radio whose process did not answer (503) is NOT free either — nobody said it was.
import { readFileSync } from 'node:fs';
import { capRadio } from '../src/index.js';

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) { pass++; console.log('   ok  ', m); } else { fail++; console.log('   FAIL', m); } };

console.log('\nThe listing carries it');
{
  const held = capRadio({ id: '60f00c82', name: 'RSP1A', driver: 'sdrplay', maxListeners: 10, listeners: 0,
                          inUseElsewhere: true });
  ok(held && held.inUseElsewhere === true, '★ a radio the server says is held keeps inUseElsewhere:true');
  const old = capRadio({ id: 'a1', name: 'RTL', driver: 'rtlsdr', maxListeners: 1, listeners: 0 });
  ok(old && !('inUseElsewhere' in old), '★ an older server never sends it — nothing is invented');
}

// ── The page's own functions, lifted out of index.html and run as written ────────────────────────────────
const html = readFileSync(new URL('../public/index.html', import.meta.url), 'utf8');
function lift(name) {
  const at = html.indexOf(`function ${name}(`);
  if (at < 0) throw new Error(`${name} not found in index.html`);
  let i = html.indexOf('{', at), depth = 0;
  for (; i < html.length; i++) {
    if (html[i] === '{') depth++;
    else if (html[i] === '}' && --depth === 0) break;
  }
  return html.slice(at, i + 1);
}
// eslint-disable-next-line no-new-func
const { radioInUse, radioFree } = new Function(`${lift('radioInUse')}\n${lift('radioFree')}\nreturn { radioInUse, radioFree };`)();

console.log('\nThe page: held is never free');
{
  const rad = { id: 'x', inUseElsewhere: true, listeners: 0, maxListeners: 10 };
  const srv = { radios: [rad], listeners: 0, maxListeners: 10 };
  ok(radioInUse(rad, srv) === true, 'the radio says it is held → in use');
  ok(radioFree(rad, srv) === false, '★★★ held with nobody listening is NOT free (the 17-hour FREE)');
  const solo = { id: 'y', listeners: 0, maxListeners: 10 };
  ok(radioInUse(solo, { radios: [solo], inUseElsewhere: true }) === true,
     'a single-radio server whose machine says so → the radio is in use');
  const multi = { id: 'z', listeners: 0, maxListeners: 10 };
  ok(radioInUse(multi, { radios: [multi, { id: 'w' }], inUseElsewhere: true }) === false,
     '★ on a multi-radio server the machine\'s word does not land on every radio');
}

console.log('\nThe page: a radio that did not answer is not free');
{
  const rad = { id: 'x', radioDown: true, listeners: 0, maxListeners: 10 };
  const srv = { radios: [rad], listeners: 0, maxListeners: 10 };
  ok(radioFree(rad, srv) === null, '★★ 503 from the door → nobody said → not free (null), even with the door\'s 0 listeners');
  const up = { id: 'x', listeners: 0, maxListeners: 10 };
  ok(radioFree(up, { radios: [up] }) === true, 'an answering radio with room is free, as before');
  const both = { id: 'x', radioDown: true, inUseElsewhere: true, listeners: 0, maxListeners: 10 };
  ok(radioFree(both, { radios: [both] }) === false, 'down AND known held → in use wins (it is the better answer)');
}

console.log('\nThe page: the single-radio refresh copies the radio\'s own answer');
{
  // ★ A text check, because the refresh is a fetch inside an async render. Both halves must be there:
  //   the radio's own inUseElsewhere is copied, and a 503 marks the radio down rather than borrowing the door's figures.
  ok(/if \(soloFromRadio\) radios\[0\]\.inUseElsewhere = rj\.inUseElsewhere === true;/.test(html),
     'the solo refresh copies rj.inUseElsewhere');
  ok(/else if \(r3\.status === 503\) \{[\s\S]{0,700}radios\[0\]\.radioDown = true;\s*soloFromRadio = true;/.test(html),
     '★★★ a 503 marks the radio down and skips the machine\'s figures');
  ok(/if \(rr\.status === 503\) rad\.radioDown = true;/.test(html), 'the multi-radio refresh does the same');
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
