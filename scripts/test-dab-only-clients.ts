/**
 * test-dab-only-clients.ts — V12 DAB ONLY in the clients (Stuart, 2026-11-17: "if DAB is the only allowed mode then the exit
 * button goes, also the exit button needs to be removed on existing servers that have been locked to DAB").
 *   · ONE RULE, TWO READERS: the web page's dabOnlyFrom and the app's give the same answer for every case — the server's word
 *     when it gave one (a new server; false for the signed-in admin), an OLDER server's blocked modes otherwise, never for
 *     the admin;
 *   · the app: EXIT DAB drawn only when not DAB only, the box's note, the client passing the server's word only when sent;
 *   · the web page: EXIT DAB and its note follow the rule; the tune message does not point at a key that is not there;
 *   · the R36S: the same rule in dab_box.h, no EXIT key, the header says why.
 * Run: npx tsx scripts/test-dab-only-clients.ts
 */
import { readFileSync } from 'node:fs';
import { dabOnlyFrom as web } from '../web/client/src/dabOnly';
import { dabOnlyFrom as app } from '../src/services/dabOnly';

let fails = 0, passes = 0;
const ok = (c: boolean, what: string) => { if (c) passes++; else { fails++; console.log('  FAIL ' + what); } };
const read = (p: string) => readFileSync(new URL('../' + p, import.meta.url), 'utf8');
const S = (...a: string[]) => new Set(a);
const OLD_LOCKED = S('wfm', 'nfm', 'am', 'usb', 'lsb', 'cwu', 'cwl', 'drm');   // an older server's list, DAB left on
const cases: Array<[boolean, boolean, Set<string>, boolean, boolean, string]> = [
  [true, true, S(), false, true, 'a new server says DAB only'],
  [true, false, OLD_LOCKED, false, false, 'a new server says not (its word wins over the list)'],
  [true, false, OLD_LOCKED, true, false, 'the admin on a new server (the server says false)'],
  [false, false, OLD_LOCKED, false, true, '★ an OLDER server locked to DAB: worked out from its blocked modes'],
  [false, false, OLD_LOCKED, true, false, '★ …but never for the signed-in admin'],
  [false, false, S('wfm', 'nfm', 'am', 'usb'), false, false, 'an older server with LSB still on: not DAB only'],
  [false, false, S(...OLD_LOCKED, 'dab'), false, false, 'an older server with DAB blocked too: not DAB only'],
  [false, false, S(), false, false, 'nothing blocked'],
];
for (const [known, said, blocked, admin, want, what] of cases) {
  ok(web(known, said, blocked, admin) === want, `web: ${what}`);
  ok(app(known, said, blocked, admin) === want, `app: ${what}`);
}

// ── the app ──
const panel = read('src/components/DabPanel.tsx'), scr = read('src/screens/SDRScreen.tsx'), wsc = read('src/services/VibeServerWsClient.ts');
ok(/\{!p\.dabOnly && <DecoderKey onPress=\{p\.onExit\}[^>]*label="EXIT DAB" \/>\}/.test(panel), '★ app: EXIT DAB drawn only when not DAB only');
ok(/\{!!p\.dabOnly && <Text[^>]*>DAB only on this receiver<\/Text>\}/.test(panel), '…and the box says "DAB only on this receiver"');
ok(/const dabOnlyHere = dabOnlyFrom\(hwDabOnly !== null, hwDabOnly === true, blockedModes, adminOk\)/.test(scr), 'app: the rule from the server word, the blocked list and the admin');
ok(/dabOnly=\{dabOnlyHere\}/.test(scr) && /onExit=\{dabOnlyHere \? \(\) => appAlert\('DAB only', DAB_ONLY_WHY\) : exitDab\}/.test(scr), 'app: Back says why instead of leaving');
ok(/setHwDabOnly\(null\);/.test(scr), 'app: a new server starts as "has not said"');
ok(/if \(typeof msg\.dabOnly === 'boolean'\) this\.callbacks\.onHwDabOnly\?\.\(msg\.dabOnly\);/.test(wsc), '★ app: only a server that SENDS dabOnly is believed (an older one is read from its list)');

// ── the web page ──
const main = read('web/client/src/main.ts'), spec = read('web/client/src/spectrum.ts'), html = read('web/client/index.html');
ok(/if \(xb\) xb\.style\.display = dabOn && !only \? '' : 'none';/.test(main), '★ web: EXIT DAB removed when DAB only');
ok(/<div id="dabOnlyNote" hidden>DAB only on this receiver<\/div>/.test(html) && /note\.hidden = !\(dabOn && only\)/.test(main), 'web: the box says why');
ok(/if \(msg\.dabOnly !== undefined\) this\.cb\.onDabOnly\?\.\(msg\.dabOnly === true\);/.test(spec) && /hwDabOnlyKnown = true; hwDabOnly = on;/.test(main), 'web: the server word only when sent');
ok(/dabOnlyNow\(\) \? 'This receiver is DAB only/.test(main), 'web: the locked-dial message does not say "press EXIT DAB" on a DAB-only receiver');

// ── the R36S ──
const hh = read('handheld-ui/src/dab_box.h');
ok(/if \(h\.has\("dabOnly"\)\) return h\["dabOnly"\]\.boolean\(\);/.test(hh) && /blocked\("wfm"\) && blocked\("nfm"\) && blocked\("am"\) && blocked\("usb"\) && blocked\("lsb"\) && !blocked\("dab"\)/.test(hh),
   '★ R36S: the same rule (the server word, else an older server\'s blocked modes)');
ok(/if \(!dabOnlyHere\(s\)\) ks\.push_back\(\{"dab\.k\.exit", "EXIT DAB"/.test(hh), 'R36S: no EXIT key when DAB only');
ok(/DAB only on this receiver/.test(hh), 'R36S: the header says why');

// ── the words ──
ok(/DAB ONLY \\u2014 a VibeServer whose owner switches off every mode except DAB/.test(read('src/components/AboutOverlay.tsx')), 'About says it');
ok(/DAB only: listeners land in DAB and cannot\s+leave it\./.test(read('android/app/src/main/cpp/vibe_setup_page.h')), 'the setup page says it under the modes list');

console.log(`${fails ? 'FAIL' : 'ok'}  DAB only (clients): ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
