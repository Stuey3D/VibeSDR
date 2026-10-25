// test_gallery_translate.ts — the gallery's TRANSLATE (src/utils/galleryTranslate.ts, V12 2026-10-23): which lines go to
// the translator, the source language and its hint, when the key is drawn, and the layout put back together.
import { galleryLineTranslatable, galleryTranslateLines, galleryTranslatableText, galleryTitleIso, galleryHintLanguage,
         galleryGuessLanguage, galleryCanTranslate, galleryAllCaps, galleryTranslate } from '../src/utils/galleryTranslate.ts';
let pass = 0, fail = 0;
const ok = (c: boolean, m: string) => { if (c) pass++; else { fail++; console.log('  FAIL ' + m); } };

// The Mondolfo message as the gallery keeps it (vibe_gallery.h NavtexAsm::fileText).
const MONDOLFO = 'NAVTEX · E · Mondolfo, Italy \u{1F1EE}\u{1F1F9} · Nav warning · #17\nReceived 2026-10-23 04:40 UTC\n'
  + '489.5 kHz USB · background decoding (Profile 1)\n\nZCZC EA17\n230440 UTC OCT 26\nMONDOLFO RADIO\n\n'
  + 'COSTAVURNAV NR. 576/26 OTT 17 (1 RILANCIO)\nMAR ADRIATICO  RAVENNA\nSULLE SEGUENTI PIATTAFORME IN PSN:\n'
  + ' 44 31.4N A   012 30.6E  GARIBALDI A NR. 4039.2 EFF\n[… signal lost 12 s …]\n_.,;: __ ..\n'
  + 'I SISTEMI DI AUSILIO ALLA NAVIGAZIONE ISTALLATI NON SONO FUNZIONANTI\nNNNN\n';

{
  const L = galleryTranslateLines(MONDOLFO, 'navtex');
  const sent = L.filter((l) => l.translate).map((l) => l.text.trim());
  ok(L.length === MONDOLFO.split('\n').length, 'one entry per line — the layout is kept');
  ok(!sent.some((s) => s.startsWith('NAVTEX') || s.startsWith('Received') || s.includes('background decoding')), 'our own header is never sent');
  ok(!sent.includes('ZCZC EA17') && !sent.includes('NNNN'), 'ZCZC and NNNN pass through');
  ok(!sent.includes('230440 UTC OCT 26'), 'the date-time group passes through');
  ok(!sent.some((s) => s.startsWith('44 31.4N')), 'a position line (mostly figures) passes through');
  ok(!sent.some((s) => s.includes('signal lost')), 'the "[… signal lost 12 s …]" marker is skipped');
  ok(!sent.some((s) => s.startsWith('_.,;')), 'a garbage line (punctuation, lost characters) is skipped');
  ok(sent.includes('I SISTEMI DI AUSILIO ALLA NAVIGAZIONE ISTALLATI NON SONO FUNZIONANTI') && sent.includes('SULLE SEGUENTI PIATTAFORME IN PSN:')
     && sent.includes('COSTAVURNAV NR. 576/26 OTT 17 (1 RILANCIO)'), 'the words are sent: ' + JSON.stringify(sent));
  ok(galleryTitleIso('E · Mondolfo, Italy \u{1F1EE}\u{1F1F9} · Nav warning') === 'IT', 'the flag in the title → IT');
  ok(galleryHintLanguage('E · Mondolfo, Italy \u{1F1EE}\u{1F1F9}') === 'it' && galleryHintLanguage('Station E · Nav warning') === '',
     'the station\'s language is the hint; no flag, no hint');
  const src = galleryGuessLanguage(galleryTranslatableText(L), 'it');
  ok(src === 'it', 'the Mondolfo text reads as Italian: ' + src);
  ok(galleryCanTranslate(L, src, 'en-GB'), 'TRANSLATE is drawn for Italian → English');
  ok(!galleryCanTranslate(L, src, 'it'), '...and not when the target is Italian');
  ok(!galleryCanTranslate(L, '', 'en'), '...nor when the language is unknown');
}
{
  const EN = 'NAVTEX · B · Oostende\nReceived x\n\nZCZC BA10\nWRECK IN POSITION 51-14.13N 002-54.22E\nTHE BUOY IS MISSING AND WILL BE REPLACED\nNNNN';
  const L = galleryTranslateLines(EN, 'navtex');
  ok(galleryGuessLanguage(galleryTranslatableText(L), 'nl') === 'en', 'an English Oostende message reads as English, whatever the hint');
  ok(!galleryCanTranslate(L, 'en', 'en-GB'), '★ already in the target language → no TRANSLATE');
  const FR = 'ZCZC TA45\nORIGINE METEO-FRANCE\nAVIS DE GRAND FRAIS A COUP DE VENT SUR LES ZONES ET DANS LE PAS DE CALAIS\nNNNN';
  ok(galleryGuessLanguage(galleryTranslatableText(galleryTranslateLines(FR, 'navtex')), 'en') === 'fr', "Niton's French T reads as French despite the UK hint");
  ok(galleryGuessLanguage('OG DET ER IKKE MULIG TIL', 'no') === 'no' && galleryGuessLanguage('OG DET ER IKKE MULIG TIL', 'da') === 'da',
     'Norwegian or Danish: the station decides');
  ok(galleryGuessLanguage('BUOY', 'de') === 'de', 'too short to read: the station\'s language stands in');
}
{
  const FT8 = 'FT8 / FT4 summary · 1840.000 kHz USB · background decoding\n\nSpots heard:       4 (FT8 4, FT4 0) from 2 stations';
  ok(!galleryTranslateLines(FT8, 'ft8').some((l) => l.translate), 'an FT8 summary (our English) — nothing is sent');
  // ★ 2026-10-25: an ISM day's log and card are sensor readings in our English — nothing is sent either
  const ISM = 'ISM 433.92 MHz · every reading heard on 2026-10-25 (UTC)\n\n17:01:12  433.920  Oregon Scientific THN132N · 10.5 °C · battery low · ch 1\n';
  ok(!galleryTranslateLines(ISM, 'ism').some((l) => l.translate), 'an ISM log (our English, sensor readings) — nothing is sent');
  const RTTY = 'RTTY · 10100.8 kHz USB · background decoding\nStarted 2026-10-23 04:00 UTC\n\nWETTERBERICHT FUER DIE DEUTSCHE BUCHT\n';
  const R = galleryTranslateLines(RTTY, 'rtty');
  ok(R.filter((l) => l.translate).map((l) => l.text).join('|') === 'WETTERBERICHT FUER DIE DEUTSCHE BUCHT', 'RTTY: the header is ours, the text is sent');
  ok(!galleryLineTranslatable('RYRYRYRY') && !galleryLineTranslatable('1234 5678') && galleryLineTranslatable('GALE WARNING'), 'a single word / figures are not sent');
  ok(galleryAllCaps('GALE WARNING 7') && !galleryAllCaps('Gale warning') && !galleryAllCaps('1234'), 'all capitals, or not');
}
// the put-back: line by line, untouched lines in place, capitals kept, a failed line left as received
{
  const L = galleryTranslateLines('ZCZC EA17\n  SULLE PIATTAFORME IN PSN\n44 31.4N 012 30.6E\nNON SONO FUNZIONANTI\nNNNN', 'navtex');
  const seen: string[] = [];
  const r = await galleryTranslate(L, 'en', async (t) => {
    seen.push(t);
    if (t.startsWith('non')) return { ok: false, reason: 'pack missing' };
    return { ok: true, text: 'on the platforms in position' };
  });
  ok(seen.join('|') === 'sulle piattaforme in psn|non sono funzionanti', 'capitals go in lower case: ' + seen.join('|'));
  ok(r.text === 'ZCZC EA17\n  ON THE PLATFORMS IN POSITION\n44 31.4N 012 30.6E\nNON SONO FUNZIONANTI\nNNNN', 'the layout survives: ' + JSON.stringify(r.text));
  ok(r.done === 1 && r.failed === 1 && r.reason === 'pack missing', 'one done, one failed with the reason');
}

console.log(`gallery translate: ${pass} passed, ${fail} failed`);
if (fail) process.exit(1);
