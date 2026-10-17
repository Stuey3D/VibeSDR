/**
 * TRANSCRIBE AUTO ORDER (src/services/transcribeOrder.ts) — which languages AUTO tries first.
 *
 * Proves:
 *   • EiBi's languages lead (at most 3), then the receiver's MAIN language, then the ★ preferred list, then the area;
 *   • the main language keeps a round-1 lane even when EiBi names 3 and the user has 5 preferred;
 *   • the main language is sent in the receiver-country variant when that variant is installed (pt-BR, en-GB);
 *   • an uninstalled main language gets NO reserved lane — the preferred list moves up into it;
 *   • one entry per language (en / en-GB, zh / cmn), whatever the source;
 *   • the ★ toggle appends and removes, and refuses a 5th (Apple keeps 5: the phone's language + 4 ★); the stored
 *     form survives junk and keeps the first 4 of an older, longer list;
 *   • the ⚠ for a receiver whose main language is not on the device — and none for a UK receiver with English, nor
 *     without a known receiver position;
 *   • Android 12/13's single LIKELY language: EiBi's, else the SDR's local language, else the first ★, else the phone's.
 *
 * Run: node --no-warnings scripts/test_transcribeOrder.ts
 */
import { autoOrder, togglePreferred, parsePreferred, receiverLanguageMissing, autoLikely, PREFERRED_MAX, preferredFull } from '../src/services/transcribeOrder.ts';
import { countryNear } from '../src/services/regionLanguages.ts';

let fails = 0;
const eq = (name: string, got: unknown, want: unknown) => {
  const ok = JSON.stringify(got) === JSON.stringify(want);
  if (!ok) fails++;
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${ok ? '' : `\n     got  ${JSON.stringify(got)}\n     want ${JSON.stringify(want)}`}`);
};
const has = (...codes: string[]) => (c: string) => codes.includes(c);

// A UK receiver, no EiBi entry (amateur band), Stuart's five preferred.
eq('UK, no EiBi: main, then preferred, then the area',
  autoOrder({ eibi: [], area: ['en', 'fr', 'nl', 'de'], preferred: ['es', 'fr', 'nl', 'de', 'it'],
              installed: has('en', 'fr', 'nl', 'de', 'es', 'it'), mainVariant: c => c === 'en' ? 'en-GB' : null }),
  ['en-GB', 'es', 'fr', 'nl', 'de', 'it']);

// Kiko's receiver in Brazil, China Radio on EiBi (Mandarin + Cantonese), preferred list of five.
eq('Brazil + EiBi: EiBi first, Brazilian Portuguese still in round 1',
  autoOrder({ eibi: ['zh', 'yue'], area: ['pt', 'es'], preferred: ['en', 'fr', 'nl', 'de', 'es'],
              installed: () => true, mainVariant: c => c === 'pt' ? 'pt-BR' : null }).slice(0, 4),
  ['zh', 'yue', 'pt-BR', 'en']);

eq('EiBi capped at 3, main language is the 4th lane',
  autoOrder({ eibi: ['ru', 'uk', 'pl', 'cs'], area: ['de', 'fr'], preferred: ['en'],
              installed: () => true }).slice(0, 4),
  ['ru', 'uk', 'pl', 'de']);

eq('main language not on the device: no reserved lane, preferred moves up',
  autoOrder({ eibi: [], area: ['pt', 'es'], preferred: ['en', 'fr'], installed: has('en', 'fr', 'es'),
              mainVariant: () => 'pt-BR' }),
  ['en', 'fr', 'pt', 'es']);

eq('variant not installed: the bare code (native picks the variant)',
  autoOrder({ eibi: [], area: ['en'], preferred: [], installed: has('en'), mainVariant: () => null }),
  ['en']);

eq('one entry per language across sources (en-GB vs en, cmn vs zh)',
  autoOrder({ eibi: ['zh'], area: ['en', 'zh'], preferred: ['cmn', 'en', 'fr'], installed: () => true,
              mainVariant: c => c === 'en' ? 'en-GB' : null }),
  ['zh', 'en-GB', 'fr']);

eq('main language already named by EiBi is not repeated',
  autoOrder({ eibi: ['pt'], area: ['pt', 'es'], preferred: ['en'], installed: () => true, mainVariant: () => 'pt-BR' }),
  ['pt', 'en', 'es']);

eq('nothing known: empty', autoOrder({ eibi: [], area: [], preferred: [], installed: () => true }), []);

eq('★ toggle appends', togglePreferred(['en', 'fr'], 'nl'), ['en', 'fr', 'nl']);
eq('★ toggle removes and closes the gap', togglePreferred(['en', 'fr', 'nl'], 'fr'), ['en', 'nl']);
eq('★ toggle: Mandarin as cmn removes zh', togglePreferred(['zh', 'en'], 'cmn'), ['en']);
eq('stored list parsed, junk + repeats dropped', parsePreferred('en, FR,,nl,en,xx-YY,de'), ['en', 'fr', 'nl', 'de']);
eq('stored list absent', parsePreferred(null), []);
eq('★ cap is 4', PREFERRED_MAX, 4);
eq('★ full at 4', [preferredFull(['en', 'fr', 'nl']), preferredFull(['en', 'fr', 'nl', 'de'])], [false, true]);
eq('★ toggle refuses a 5th', togglePreferred(['en', 'fr', 'nl', 'de'], 'es'), ['en', 'fr', 'nl', 'de']);
eq('★ toggle still removes when full', togglePreferred(['en', 'fr', 'nl', 'de'], 'nl'), ['en', 'fr', 'de']);
eq('stored list of 5 (before the cap) keeps the first 4', parsePreferred('es,fr,nl,de,it'), ['es', 'fr', 'nl', 'de']);

eq('⚠ Brazilian receiver, no Portuguese on the device', receiverLanguageMissing('pt', true, has('en', 'de')), 'pt');
eq('⚠ none: UK receiver with English', receiverLanguageMissing('en', true, has('en')), null);
eq('⚠ none: receiver position unknown', receiverLanguageMissing('pt', false, has('en')), null);
eq('⚠ none: no language known for the area', receiverLanguageMissing(undefined, true, has('en')), null);

eq('likely: EiBi when installed', autoLikely({ eibi: ['de'], area: ['pt'], preferred: ['en'], installed: () => true, phone: 'en' }),
   { code: 'de', why: 'eibi' });
eq('likely: Kiko\'s receiver → Portuguese', autoLikely({ eibi: [], area: ['pt', 'es'], preferred: ['en'], installed: () => true, phone: 'en' }),
   { code: 'pt', why: 'receiver' });
eq('likely: receiver language missing → first ★ installed', autoLikely({ eibi: ['zh'], area: ['pt'], preferred: ['fr', 'en'], installed: has('fr', 'en'), phone: 'en' }),
   { code: 'fr', why: 'preferred' });
eq('likely: nothing installed → the phone\'s', autoLikely({ eibi: [], area: ['pt'], preferred: [], installed: () => false, phone: 'en' }),
   { code: 'en', why: 'phone' });

eq('countryNear: Kiko (São Paulo) → BR', countryNear({ lat: -23.5, lon: -46.6 }), 'BR');
eq('countryNear: Moulton → GB', countryNear({ lat: 52.3, lon: -0.85 }), 'GB');

console.log(fails ? `\n${fails} FAILED` : '\nall passed');
process.exit(fails ? 1 : 0);
