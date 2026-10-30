// test_spoken_callsign.ts — callsigns heard in the TRANSCRIBE captions (src/utils/spokenCallsign.ts, V12 2026-10-30).
// Run: node scripts/test_spoken_callsign.ts
import { findCallsigns, captionPieces, isValidCallsign } from '../src/utils/spokenCallsign.ts';
let pass = 0, fail = 0;
const ok = (c: boolean, m: string) => { if (c) pass++; else { fail++; console.log('  FAIL ' + m); } };
const calls = (s: string) => findCallsigns(s).map((m) => m.call);
const is = (s: string, want: string[]) => {
  const got = calls(s);
  ok(JSON.stringify(got) === JSON.stringify(want), `${JSON.stringify(s)} → ${JSON.stringify(got)}, want ${JSON.stringify(want)}`);
};

// ── G4TPJ, said several ways ──
is('Golf Four Tango Papa Juliet', ['G4TPJ']);
is('golf four tango papa juliet', ['G4TPJ']);
is('Golf 4 Tango Papa Juliet', ['G4TPJ']);
is('Golf, four, Tango, Papa, Juliett.', ['G4TPJ']);
is('golf for tango papa juliette', ['G4TPJ']);
is('CQ CQ this is Golf Four Tango Papa Juliet calling', ['G4TPJ']);
is('Germany Four Texas Portugal Japan', ['G4TPJ']);
is('George four tango Peter Japan', ['G4TPJ']);
is('this is G4TPJ', ['G4TPJ']);
is('G4 Tango Papa Juliet', ['G4TPJ']);
// ── M0PKZ, 2E0, portable ──
is('Mike Zero Papa Kilo Zulu', ['M0PKZ']);
is('mike oh papa kilo zulu', ['M0PKZ']);
is('two echo zero alpha bravo charlie', ['2E0ABC']);
is('2E0 Alpha Bravo Charlie', ['2E0ABC']);
is('Two England Zero Alpha Boston Charlie', ['2E0ABC']);
is('echo alpha eight stroke golf four tango papa juliet', ['EA8/G4TPJ']);
is('EA8/G4TPJ', ['EA8/G4TPJ']);
is('G4TPJ/P and M0PKZ/M and G4TPJ/MM', ['G4TPJ/P', 'M0PKZ/M', 'G4TPJ/MM']);
is('golf four tango papa juliet portable', ['G4TPJ/P']);
is('golf four tango papa juliet stroke papa', ['G4TPJ/P']);
is('mike zero papa kilo zulu mobile', ['M0PKZ/M']);
is('golf four tango papa juliet maritime mobile', ['G4TPJ/MM']);
// ── two calls ──
is('golf four tango papa juliet this is mike zero papa kilo zulu', ['G4TPJ', 'M0PKZ']);
is('golf four tango papa juliet de mike zero papa kilo zulu', ['G4TPJ', 'M0PKZ']);
is('golf four tango papa juliet mike zero papa kilo zulu', ['G4TPJ', 'M0PKZ']);
is('Golf Four Tango Papa Juliet. Mike Zero Papa Kilo Zulu.', ['G4TPJ', 'M0PKZ']);
// ── a repeated CQ links every occurrence ──
is('CQ CQ CQ de Hotel Bravo Nine Charlie Zulu Kilo Hotel Bravo Nine Charlie Zulu Kilo', ['HB9CZK', 'HB9CZK']);
is('HB9CZK HB9CZK', ['HB9CZK', 'HB9CZK']);
// ── mixed NATO + real-word phonetics in ONE run — general, any prefix ──
is('Hotel Bravo Nine Canada Zulu Kilo', ['HB9CZK']);
is('Honolulu Boston Nine Charlie Zulu Kilowatt', ['HB9CZK']);
is('Hotel Boston 9 Charlie Zebra Kilo', ['HB9CZK']);
is('Kilo Kilowatt Two Queen Romeo', ['KK2QR']);
is('King Kilo two Queen Quebec', ['KK2QQ']);
is('Echo Amsterdam Eight Mexico Lima', ['EA8ML']);
is('Japan Alpha One Yokohama Zulu', ['JA1YZ']);
is('Victor Kilowatt Two Alpha Boston Charlie', ['VK2ABC']);
is('Whiskey Alpha Three Boston Zebra', ['WA3BZ']);
is('Kilo Mexico Four Delta Echo', ['KM4DE']);
is('Two Echo Zero X-ray Yankee Zanzibar', ['2E0XYZ']);
is('two echo zero x ray yankee zulu', ['2E0XYZ']);
is('Whiskey One Alpha Whiskey', ['W1AW']);
is('Golf niner tango papa juliet', ['G9TPJ']);

// ── negatives: ordinary speech ──
is("I'm going to Canada in the summer", []);
is("the queen's speech", []);
is('Texas, England and Japan are all lovely', []);
is('Alpha and omega, the delta of the river, an echo in the hall', []);
is('we played golf near the hotel in India with Mike and Victor', []);
is('Mike said four people came to the hotel at nine', []);
is('the radio four news at six', []);
is('I have 4 kids and 2 dogs', []);
is('it was 1984, room 101, call 999', []);
is('he went to Italy for two weeks', []);
is('England 2 Italy 1', []);
is('the B52s and MP3 and 4G and C3PO', []);
// ── negatives: an unallocated prefix, half runs ──
is('Quebec Four Alpha Bravo Charlie', []);          // Q series
is('Juliet Nine Alpha Bravo', []);                  // J is J2–J8 only
is('Echo Eight Alpha Bravo', []);                   // E is E2–E7 only
is('Hotel Bravo One Charlie Zulu Kilo', ['HB1CZK']); // HB + 1 is fine (two-letter series)
is('Zero Alpha One Bravo Charlie', []);               // 0-prefix and A1 are not series
is('Golf Four', []);
is('Golf Four Tango', []);                          // one suffix letter: a half call
is('Hotel Bravo Nine', []);
is('Hotel Boston Nine Canada', []);                 // near miss: one suffix letter
is('Alpha Bravo Charlie Delta', []);                // no figure
is('one two three four five', []);

// ── isValidCallsign (the skimmer's rule) ──
ok(isValidCallsign('G4TPJ') && isValidCallsign('2E0ABC') && isValidCallsign('EA8/G4TPJ') && isValidCallsign('A65BR'), 'valid calls');
ok(!isValidCallsign('QQ0AB') && !isValidCallsign('Q4ABC') && !isValidCallsign('EEE5E') && !isValidCallsign('1A2BC'), 'invalid calls');

// ── pieces: the spoken run replaced by the call, the words kept for the link's label ──
const p = captionPieces('CQ this is golf four tango papa juliet over');
ok(p.length === 3 && p[0].text === 'CQ this is ' && p[1].call === 'G4TPJ' && (p[1] as any).spoken === 'golf four tango papa juliet'
  && p[2].text === ' over', 'pieces: ' + JSON.stringify(p));
ok(captionPieces('nothing here').length === 1, 'no calls → one piece');

console.log(`${pass} passed, ${fail} failed`);
if (fail) process.exit(1);
