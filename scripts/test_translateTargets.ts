/**
 * TRANSLATE TO, ONE ROW PER LANGUAGE (src/services/translateTargets.ts).
 *
 * Proves: Arabic (Saudi Arabia) + Arabic (UAE) fold to one Arabic row; Chinese keeps Simplified and Traditional (Taiwan
 * and Hong Kong fold into Traditional); Portuguese keeps Brazil and Portugal; English keeps UK and US; the phone's own
 * variant is the row's id and its row comes first; a stored older variant still finds its row; Android's bare codes
 * pass through unchanged.
 *
 * Run: node --no-warnings scripts/test_translateTargets.ts
 */
import { translateRows, rowFor } from '../src/services/translateTargets.ts';

let fails = 0;
const eq = (name: string, got: unknown, want: unknown) => {
  const ok = JSON.stringify(got) === JSON.stringify(want);
  if (!ok) fails++;
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${ok ? '' : `\n     got  ${JSON.stringify(got)}\n     want ${JSON.stringify(want)}`}`);
};

// Apple's list as translateTargets() sends it (minimalIdentifier ids, names in English).
const apple = [
  { id: 'ar', name: 'Arabic' }, { id: 'ar-AE', name: 'Arabic (United Arab Emirates)' }, { id: 'ar-SA', name: 'Arabic (Saudi Arabia)' },
  { id: 'zh', name: 'Chinese' }, { id: 'zh-HK', name: 'Chinese (Hong Kong)' }, { id: 'zh-TW', name: 'Chinese (Taiwan)' },
  { id: 'nl', name: 'Dutch' }, { id: 'en', name: 'English' }, { id: 'en-GB', name: 'English (United Kingdom)', installed: true },
  { id: 'fr', name: 'French' }, { id: 'de', name: 'German' },
  { id: 'pt', name: 'Portuguese' }, { id: 'pt-PT', name: 'Portuguese (Portugal)' }, { id: 'es', name: 'Spanish' },
];
const uk = translateRows(apple, 'en-GB');
eq('a British phone: English (UK) first', uk[0], { id: 'en-GB', name: 'English (UK)', installed: true, ids: ['en-GB'] });
eq('one row per language, distinct written forms kept', uk.map(r => r.name), [
  'English (UK)', 'Arabic', 'Chinese (Simplified)', 'Chinese (Traditional)', 'Dutch', 'English (US)', 'French', 'German',
  'Portuguese (Brazil)', 'Portuguese (Portugal)', 'Spanish']);
eq('Arabic: one row, the bare code, every variant folded in', uk.find(r => r.name === 'Arabic'), { id: 'ar', name: 'Arabic', ids: ['ar', 'ar-AE', 'ar-SA'] });
eq('Traditional Chinese covers Taiwan and Hong Kong', uk.find(r => r.name === 'Chinese (Traditional)')?.ids, ['zh-HK', 'zh-TW']);
eq('Traditional Chinese on a British phone is zh-TW (first of the form)', uk.find(r => r.name === 'Chinese (Traditional)')?.id, 'zh-TW');

const hk = translateRows(apple, 'zh-HK');
eq('a Hong Kong phone: Traditional first, sent as zh-HK', [hk[0].name, hk[0].id], ['Chinese (Traditional)', 'zh-HK']);
const br = translateRows(apple, 'pt');
eq('Kiko in Brazil: Portuguese (Brazil) first', [br[0].name, br[0].id], ['Portuguese (Brazil)', 'pt']);
const fr = translateRows(apple, 'fr');
eq('a French phone: French first', fr[0].id, 'fr');
const noAr = translateRows(apple.filter(t => t.id !== 'ar'), 'en');
eq('no bare Arabic: one row anyway (a variant stands for it)', noAr.filter(r => r.name === 'Arabic').length, 1);
eq('a US phone: English (US) first', [noAr[0].name, noAr[0].id], ['English (US)', 'en']);

eq('a stored ar-AE finds the Arabic row', rowFor(uk, 'ar-AE')?.id, 'ar');
eq('a stored zh-hk (any case) finds Traditional', rowFor(uk, 'zh-hk')?.name, 'Chinese (Traditional)');
eq('nothing stored: no row', rowFor(uk, ''), undefined);

const aus = translateRows([...apple, { id: 'en-AU', name: 'English (Australia)' }], 'en-AU');
eq('an Australian phone keeps its own variant, first', [aus[0].id, aus.filter(r => r.ids.includes('en-AU')).length], ['en-AU', 1]);
eq('another country’s extra variant is not offered', translateRows([...apple, { id: 'en-AU', name: 'English (Australia)' }], 'en-GB').some(r => r.ids.includes('en-AU')), false);

const android = [{ id: 'de', name: 'German' }, { id: 'en', name: 'English' }, { id: 'zh', name: 'Chinese' }];
eq('Android bare codes pass through, plain names', translateRows(android, 'en').map(r => [r.id, r.name]), [['en', 'English'], ['zh', 'Chinese'], ['de', 'German']]);

console.log(fails ? `\n${fails} FAILED` : '\nall passed');
process.exit(fails ? 1 : 0);
