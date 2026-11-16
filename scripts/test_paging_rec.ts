/**
 * test_paging_rec.ts — V12 PAGE: the law's notice and RECORDINGS SILENT WHILE PAGE RUNS, on the app's side (2026-11-16).
 *
 * Stuart: "Decoding pagers is illegal in certain countries — please check the laws in your country before using it." and
 * "Recording should be silent for pager that way nobody can record and decode again later."
 *
 * ★★★ THE ANDROID RECORDER RULE. The app's recorder (VibeStreamService recTapShorts — REC, the record buffer) and a VFO's
 *     own track (VfoTrack, format 5) record what the phone's own engine sends on /ws/audio; they record THROUGH a mute on
 *     purpose. So the rule lives in the engine: while its paging receiver runs, every audio stream it sends is silence
 *     (vibe_paging_gate.h pagingAudioOut — run end to end by vibeserver/test-paging-gate.cpp). This pins, in the sources:
 *       · every function in local_sdr_shim.cpp that sends PCM-derived audio (Out::Audio) passes it through pagingAudioOut;
 *         the only other Out::Audio senders are named here with why (DAB+'s own AAC, raw IQ's stream — refused / closed);
 *       · raw IQ out is refused while PAGE runs, closed when PAGE starts, and queues nothing in between;
 *       · the Android recorder takes the engine's stream (no other source to leak from);
 *     and the app's words: the law in the hint, the tour and About; REC's note; the banner.
 * Run: npx tsx scripts/test_paging_rec.ts   (run-tests.sh does)
 */
import { readFileSync } from 'node:fs';
import {
  PAGING_HINT, PAGING_LAW_NOTICE, PAGING_REC_SILENT, PAGING_REC_NOTE, pagingHintFor, pagingRecSilent, pagingMutedBanner,
} from '../src/services/paging';

let fails = 0, passes = 0;
const ok = (c: boolean, what: string) => { if (c) passes++; else { fails++; console.log('  FAIL ' + what); } };
const read = (p: string) => readFileSync(new URL('../' + p, import.meta.url), 'utf8');

// ── the words (paging.ts — one copy) ──
ok(PAGING_LAW_NOTICE === 'Decoding pagers is illegal in certain countries — please check the laws in your country before using it.',
  "the law's notice is Stuart's sentence");
ok(PAGING_HINT.startsWith(PAGING_LAW_NOTICE + ' '), 'the hint under PAGE starts with it');
ok(pagingHintFor('PROTOCOL', 'RATE').startsWith(PAGING_LAW_NOTICE), "…and the R36S's hint (its row names) too");
ok(/recordings are silent/.test(PAGING_HINT), 'the hint says recordings are silent while PAGE runs');
ok(PAGING_REC_SILENT === 'REC · silent while PAGE runs', 'REC\'s short note');
ok(/record buffer/.test(PAGING_REC_NOTE) && /raw IQ out is off/.test(PAGING_REC_NOTE) && /Normal audio comes back/.test(PAGING_REC_NOTE),
  'REC\'s note names the buffer, raw IQ out and that audio comes back');
ok(!/\b(color|center|analyze)\b/i.test(PAGING_HINT + PAGING_REC_NOTE + PAGING_LAW_NOTICE), 'UK English');

// ── when REC is silent: PAGE only (APRS mutes the speaker; its audio records as ever) ──
ok(pagingRecSilent('paging') === true, 'PAGE open: REC silent');
ok(pagingRecSilent('aprs') === false && pagingRecSilent('rtty') === false && pagingRecSilent(null) === false && pagingRecSilent(undefined) === false,
  'anything else: REC records the audio');
ok(pagingMutedBanner(false) === '\u{1F507} AUDIO MUTED WHILE PAGE RUNS', 'the banner, not recording');
ok(pagingMutedBanner(true) === '\u{1F507} AUDIO MUTED · REC SILENT WHILE PAGE RUNS', 'the banner says so while recording');

// ── the app shows them where PAGE is offered or described ──
const sdr = read('src/screens/SDRScreen.tsx'), about = read('src/components/AboutOverlay.tsx');
const modeSel = read('src/components/ModeSelector.tsx'), audio = read('src/components/AudioSheet.tsx');
ok(/<Text style=\{capText\}>\{PAGING_HINT\}<\/Text>/.test(modeSel), 'the PAGE key\'s hint is PAGING_HINT (the law first)');
const tour = sdr.match(/Platform\.OS === 'android' \? ', Paging \(PAGE[^]*?\)' : ''\)/)?.[0] ?? '';
ok(tour.includes('PAGING_LAW_NOTICE') && tour.includes('recordings are silent'), "the tour's mode card: the law and REC's silence");
const aboutPaging = about.slice(about.indexOf('PAGING ON ANDROID'), about.indexOf('MSK144 FOR METEOR SCATTER'));
ok(aboutPaging.includes('PAGING_LAW_NOTICE') && /recording made while PAGE runs is silent/.test(aboutPaging) && /REC records silence/.test(aboutPaging),
  'About: the law, and REC silent on Android and on the R36S');
ok(/pagingRecSilent=\{pagingRecSilent\(activeDecoder\)\}/.test(sdr) && /\{PAGING_REC_SILENT\}\. \{PAGING_REC_NOTE\}/.test(audio),
  'the Audio sheet says REC is silent while PAGE runs');
ok(/pagingMutedBanner\(isRecording\)/.test(sdr), 'the banner follows the recording');

// ── ★★★ THE ENGINE: what the Android recorder (and the R36S's tap) records is silence while PAGE runs ──
const shim = read('android/app/src/main/cpp/local_sdr_shim.cpp');
const lines = shim.split('\n');
/** A method of the shim's Impl: 4 spaces, a return type, the name — the signature may run on to the next lines. */
const sig = /^ {4}(?:static |inline )?(?:void|bool|int|double|size_t|std::[^\s(]+)\s+([A-Za-z_]\w*)\(/;
/** The method a line sits in. */
const enclosing = (i: number) => { for (let j = i; j >= 0; j--) { const m = sig.exec(lines[j]); if (m) return m[1]; } return '?'; };
const body = (name: string) => {
  const i = lines.findIndex(l => sig.exec(l)?.[1] === name);
  if (i < 0) return '';
  const e = lines.findIndex((l, k) => k > i && l === '    }');
  return lines.slice(i, e + 1).join('\n');
};
const SILENCED = ['sendClientAudio', 'sendAudioPcmSquelchAware', 'sendAudioPcm', 'watchSendRec', 'watchSendPrivate'];
/** Out::Audio senders that carry no PCM of the channel: DAB+'s own AAC (the decoded broadcast service — not the paging
 *  channel; its PCM path goes through sendClientAudio and IS silenced) and raw IQ out (refused and closed while PAGE runs). */
const NOT_PCM: Record<string, string> = { pumpDabPlusAudio: "DAB+'s own AAC", iqWriteLoop: 'raw IQ out', acceptIqWs: 'raw IQ out (header)' };
const senders = new Set<string>();
lines.forEach((l, i) => { if (/sendWs\(.*Out::Audio\)/.test(l)) senders.add(enclosing(i)); });
ok(senders.size >= 6, `the audio senders were found (${[...senders].join(', ')})`);
for (const s of senders) ok(SILENCED.includes(s) || s in NOT_PCM, `${s}: an audio sender this test knows (silenced, or ${Object.values(NOT_PCM).join(' / ')})`);
for (const f of SILENCED) ok(/vibe::pagingAudioOut\(/.test(body(f)), `${f} sends silence while a paging receiver runs (pagingAudioOut)`);
ok(/pagingAllowsIqOut\(.*\)\) return refuse\(vibe::pagingIqOutRefusal\(\)\)/.test(body('iqStartFor')), 'raw IQ out is refused while PAGE runs');
ok(/if \(!vibe::pagingAllowsIqOut\(.*\)\) return;/.test(body('iqTapInto')), '…and queues not a sample while it runs');
ok(/iqOffForPaging\(\);/.test(shim) && /void iqOffForPaging\(\)/.test(shim), '…and a stream already on is closed when PAGE starts');
const gate = read('android/app/src/main/cpp/vibe_paging_gate.h');
ok(/inline bool pagingSilencesOutput\(int pagingRunning\) \{ return pagingRunning > 0; \}/.test(gate), 'the rule is the running-receiver count');

// ── the Android recorder takes the engine's stream ──
const svc = read('android/app/src/main/java/com/vibesdr/app/VibeStreamService.kt');
ok(/\/ws\/audio/.test(svc) && /startLocalAudio/.test(svc), "the phone's radio is heard through its engine's /ws/audio");
ok(/if \(tapOn\) recTapShorts\(pcm, pcmN, pcmCh, item\.first\)/.test(svc), 'REC and the record buffer take that stream (the ext writer)');
const rec = read('handheld-ui/src/audio_rec.h');
ok(/bool pcm\(const std::string& d, bool silent = false\)/.test(rec), "the R36S's recorder writes silence while PAGE runs, too");

console.log(`${fails ? 'FAIL' : 'ok'}  paging rec + law: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
